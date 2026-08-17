/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/ScopeGuard.h>
#include <folly/coro/BlockingWait.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/test/TestLogHandler.h>

#include <fb303/ThreadCachedServiceData.h>

/*
 * Grant the exit-sentinel test access to the protected exitInitiated_ flag, so
 * it can exercise the -1 "unavailable" return of co_getRibVersion /
 * co_getNumPrefixes without a running RIB evb -- the exit guard short-circuits
 * before the evb hop.
 */
#define BgpServiceBase_TEST_FRIENDS \
  FRIEND_TEST(                      \
      BgpServiceBaseTestFixture,    \
      GetRibVersionAndNumPrefixesReturnNegativeOnExit);

#include "neteng/fboss/bgp/cpp/BgpServiceBase.h"
#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/peer/PeerManagerBase.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

using namespace ::testing;
using namespace facebook::neteng::fboss::bgp_attr;
using namespace facebook::neteng::fboss::bgp::thrift;

namespace facebook::bgp {

static const std::string kExitNullPtrLogPrefix = "ExitOrNullPtr";

namespace {
constexpr int64_t kExpectedBgpPathEntries = 1;
constexpr int64_t kExpectedBgpAttributesEntries = 2;
constexpr int64_t kExpectedAsPathEntries = 3;
constexpr int64_t kExpectedCommunitiesEntries = 4;
constexpr int64_t kExpectedClusterListEntries = 5;
constexpr int64_t kExpectedExtCommunitiesEntries = 6;
constexpr int64_t kEmptyAttributeCount = 0;
constexpr double kEmptyAttributeAverage = 0.0;

void clearAttributeDeduplicators() {
  nettools::bgplib::DeDuplicatedBgpPath::clearDeduplicator();
  nettools::bgplib::DeDuplicatedBgpAttributesC::clearDeduplicator();
  nettools::bgplib::DeDuplicatedAsPath::clearDeduplicator();
  nettools::bgplib::DeDuplicatedCommunities::clearDeduplicator();
  nettools::bgplib::DeDuplicatedClusterList::clearDeduplicator();
  nettools::bgplib::DeDuplicatedExtCommunities::clearDeduplicator();
}

void expectEmptyAttributeStats(const TAttributeStats& stats) {
  EXPECT_EQ(kEmptyAttributeCount, stats.total_num_of_attributes().value());
  EXPECT_EQ(kEmptyAttributeCount, stats.total_unique_attributes().value());
  EXPECT_EQ(kEmptyAttributeAverage, stats.avg_attribute_refcount().value());
  EXPECT_EQ(kEmptyAttributeAverage, stats.avg_community_list_len().value());
  EXPECT_EQ(kEmptyAttributeAverage, stats.avg_extcommunity_list_len().value());
  EXPECT_EQ(kEmptyAttributeAverage, stats.avg_as_path_len().value());
  EXPECT_EQ(kEmptyAttributeAverage, stats.avg_cluster_list_len().value());
  EXPECT_EQ(kEmptyAttributeAverage, stats.avg_topology_info_len().value());
  EXPECT_FALSE(stats.dedup_bgp_path().has_value());
  EXPECT_FALSE(stats.dedup_bgp_attributes().has_value());
  EXPECT_FALSE(stats.dedup_as_path().has_value());
  EXPECT_FALSE(stats.dedup_communities().has_value());
  EXPECT_FALSE(stats.dedup_cluster_list().has_value());
  EXPECT_FALSE(stats.dedup_ext_communities().has_value());
  EXPECT_EQ(TAttributeStatsPayloadKind::UNKNOWN, stats.payload_kind().value());
}
} // namespace

class BgpServiceBaseTestFixture : public ::testing::Test {
 public:
  void SetUp() override {
    // Create config
    config_ = createConfig();
    configManager_ = std::make_shared<ConfigManager>(config_);

    // Create RIB
    rib_ = std::make_unique<MockRib>(
        std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>{},
        globalConfig_,
        std::nullopt /* policyConfig */,
        ribInQ_,
        ribOutQ_,
        "dev" /* platform */,
        nullptr /* fsdbSyncer*/);

    // Create PeerManagerBase with PolicyManager
    policyManager_ = createPolicyManager();
    peerManager_ = std::make_shared<PeerManagerBase>(
        configManager_, policyManager_, ribInQ_, ribOutQ_, neighborEventQ_);

    // Create watchdog
    watchdog_ = std::make_unique<Watchdog>(config_);

    // Create BgpServiceBase
    service_ = std::make_unique<BgpServiceBase>(
        *peerManager_,
        configManager_,
        *rib_,
        *watchdog_,
        false /* thrift protection */);

    // Initialize stats counters
    BgpStats::initCounters();
    counters_ = fb303::ThreadCachedServiceData::getShared();

    // Setup log handler
    logHandler_ = std::make_shared<folly::TestLogHandler>();
    auto logCategory = folly::LoggerDB::get().getCategory("");
    logCategory->addHandler(std::shared_ptr<folly::LogHandler>(logHandler_));
    logCategory->setLevel(folly::LogLevel::INFO);
  }

  void TearDown() override {
    service_.reset();
    peerManager_.reset();
    rib_.reset();
  }

 protected:
  virtual std::shared_ptr<Config> createConfig() {
    thrift::BgpConfig thriftConfig;
    thriftConfig.router_id() = kLocalAddr1.str();
    thriftConfig.local_as() = kAsn1;
    thriftConfig.hold_time() = kHoldTime.count();
    thriftConfig.graceful_restart_convergence_seconds() =
        kGrRestartTime.count();
    thriftConfig.listen_addr() = kLocalAddr1.str();
    thriftConfig.eor_time_s() = 45;

    // Add test peers using thrift BgpPeer
    std::vector<thrift::BgpPeer> testPeers;

    thrift::BgpPeer peer1;
    peer1.peer_addr() = kPeerAddr1.str();
    peer1.local_addr() = kLocalAddr1.str();
    peer1.remote_as() = kAsn2;
    peer1.next_hop4() = kV4Nexthop1.str();
    peer1.next_hop6() = kV6Nexthop1.str();
    testPeers.push_back(peer1);

    thrift::BgpPeer peer2;
    peer2.peer_addr() = kPeerAddr2.str();
    peer2.local_addr() = kLocalAddr1.str();
    peer2.remote_as() = kAsn2;
    peer2.next_hop4() = kV4Nexthop1.str();
    peer2.next_hop6() = kV6Nexthop1.str();
    testPeers.push_back(peer2);

    thriftConfig.peers() = testPeers;

    // Add test peer groups
    std::vector<thrift::PeerGroup> testPeerGroups;
    thrift::PeerGroup peerGroup1;
    peerGroup1.name() = "test-peer-group-1";
    testPeerGroups.push_back(peerGroup1);

    thrift::PeerGroup peerGroup2;
    peerGroup2.name() = "test-peer-group-2";
    testPeerGroups.push_back(peerGroup2);

    thriftConfig.peer_groups() = testPeerGroups;

    return std::make_shared<Config>(std::move(thriftConfig));
  }

  virtual std::shared_ptr<PolicyManager> createPolicyManager() {
    return setupPolicyManagerWithMultiplePolicies(
        {"test-ingress-policy",
         "test-egress-policy",
         "ingress-policy-1",
         "ingress-policy-2",
         "egress-policy-1",
         "egress-policy-2"});
  }

  std::unique_ptr<MockRib> rib_;
  std::shared_ptr<PeerManagerBase> peerManager_;
  std::shared_ptr<PolicyManager> policyManager_;
  std::unique_ptr<BgpServiceBase> service_;
  std::shared_ptr<Config> config_;
  std::shared_ptr<ConfigManager> configManager_;
  std::unique_ptr<Watchdog> watchdog_;
  std::shared_ptr<folly::TestLogHandler> logHandler_;
  std::shared_ptr<fb303::ThreadCachedServiceData> counters_;

 private:
  BgpGlobalConfig globalConfig_{
      kAsn1, // localAsn
      kLocalAddr1, // routerId
      kPeerAddr3, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      {}, // networksV4
      {} // networksV6
  };

  // Rib
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_{
      nettools::bgplib::kMaxIngressQueueSize};
  MonitoredMPMCQueue<RibOutMessage> ribOutQ_;

  // Peer Manager
  std::optional<MonitoredMPMCQueue<NeighborWatcherMessage>> neighborEventQ_;
};

// Test ConfigManager::addPeersToConfig adds peer to config
TEST_F(BgpServiceBaseTestFixture, AddPeersToConfigSuccessTest) {
  auto configManager = std::make_shared<ConfigManager>(config_);

  std::vector<thrift::BgpPeer> newPeers;
  thrift::BgpPeer peer;
  peer.peer_addr() = "3.3.3.3";
  peer.local_addr() = kLocalAddr1.str();
  peer.remote_as() = kAsn2;
  peer.next_hop4() = kV4Nexthop1.str();
  peer.next_hop6() = kV6Nexthop1.str();
  newPeers.push_back(peer);

  auto newConfig = configManager->addPeersToConfig(newPeers);

  auto newPeerAddr = folly::IPAddress("3.3.3.3");
  const auto& peerToConfig = newConfig->getPeerToConfig();
  EXPECT_EQ(3, peerToConfig.size());
  EXPECT_EQ(1, peerToConfig.count(newPeerAddr));
  EXPECT_EQ(newPeerAddr, peerToConfig.at(newPeerAddr)->peerAddr);
}

// Test ConfigManager::removePeersFromConfig removes peer from config
TEST_F(BgpServiceBaseTestFixture, RemovePeersFromConfigSuccessTest) {
  auto configManager = std::make_shared<ConfigManager>(config_);

  // Verify initial state has 2 peers
  auto initialConfig = configManager->getConfig();
  EXPECT_EQ(2, initialConfig->getPeerToConfig().size());

  // Remove one peer
  std::vector<folly::IPAddress> addrsToRemove = {kPeerAddr1};
  auto newConfig = configManager->removePeersFromConfig(addrsToRemove);

  const auto& peerToConfig = newConfig->getPeerToConfig();
  EXPECT_EQ(1, peerToConfig.size());
  EXPECT_EQ(0, peerToConfig.count(kPeerAddr1));
  EXPECT_EQ(1, peerToConfig.count(kPeerAddr2));
}

// Test ConfigManager::removePeersFromConfig with non-existent peer is no-op
TEST_F(BgpServiceBaseTestFixture, RemovePeersFromConfigNonExistentTest) {
  auto configManager = std::make_shared<ConfigManager>(config_);

  auto nonExistentAddr = folly::IPAddress("9.9.9.9");
  std::vector<folly::IPAddress> addrsToRemove = {nonExistentAddr};
  auto newConfig = configManager->removePeersFromConfig(addrsToRemove);

  // Config should be unchanged
  const auto& peerToConfig = newConfig->getPeerToConfig();
  EXPECT_EQ(2, peerToConfig.size());
}
// --- Session state handler tests ---

TEST_F(BgpServiceBaseTestFixture, ShutdownSessionNullPtrTest) {
  folly::coro::blockingWait(service_->co_shutdownSession(nullptr));
  EXPECT_THAT(
      logHandler_->getMessageValues(),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

TEST_F(BgpServiceBaseTestFixture, RestartSessionNullPtrTest) {
  folly::coro::blockingWait(service_->co_restartSession(nullptr));
  EXPECT_THAT(
      logHandler_->getMessageValues(),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

TEST_F(BgpServiceBaseTestFixture, StartSessionNullPtrTest) {
  folly::coro::blockingWait(service_->co_startSession(nullptr));
  EXPECT_THAT(
      logHandler_->getMessageValues(),
      Contains(ContainsRegex(kExitNullPtrLogPrefix)));
}

// --- Global summary getter handler tests ---

/*
 * co_getRibVersion / co_getNumPrefixes are coroutine handlers that read the RIB
 * through a timeout-protected evb hop (co_runOnEvbWithTimeout). That hop
 * requires a running RIB event base, which this MockRib fixture never starts,
 * so they are exercised in RibTest.GetRibVersionAndNumPrefixesHandlers
 * (RibFixture: a real RIB evb driven by real route updates) -- mirroring how
 * co_getRouteFilterPolicy is tested in RibRouteFilterPolicyTest rather than
 * here.
 */

/*
 * When the session is exiting, the scalar RIB-read handlers short-circuit
 * before the evb hop and report -1 ("unavailable"), so a client can tell it
 * apart from a genuinely-empty RIB (which reports 0). This runs on the MockRib
 * fixture with no RIB evb precisely because the exit guard returns before any
 * hop -- exercising the sentinel without needing a real evb or a 30s timeout.
 */
TEST_F(
    BgpServiceBaseTestFixture,
    GetRibVersionAndNumPrefixesReturnNegativeOnExit) {
  service_->exitInitiated_ = true;
  EXPECT_EQ(-1, folly::coro::blockingWait(service_->co_getRibVersion()));
  EXPECT_EQ(-1, folly::coro::blockingWait(service_->co_getNumPrefixes()));
}

TEST_F(BgpServiceBaseTestFixture, GetDeduplicatorStatsReturnsTypedSnapshot) {
  clearAttributeDeduplicators();
  SCOPE_EXIT {
    clearAttributeDeduplicators();
  };

  std::vector<nettools::bgplib::DeDuplicatedBgpPath> bgpPaths;
  bgpPaths.emplace_back(
      std::make_shared<BgpPath>(*buildBgpPathFields(0, 0, 0, 0)));

  nettools::bgplib::BgpAttributesC attributes;
  attributes.med = kMed + 1;
  attributes.isMedSet = true;
  std::vector<nettools::bgplib::DeDuplicatedBgpAttributesC> bgpAttributes;
  bgpAttributes.emplace_back(std::move(attributes));

  std::vector<nettools::bgplib::DeDuplicatedAsPath> asPaths;
  for (int64_t value = 1; value <= kExpectedAsPathEntries; ++value) {
    nettools::bgplib::BgpAttrAsPathC asPath;
    asPath.push_back(
        nettools::bgplib::BgpAttrAsPathSegmentC::fromAsSeq(
            {static_cast<uint32_t>(value)}));
    asPaths.emplace_back(std::move(asPath));
  }

  std::vector<nettools::bgplib::DeDuplicatedCommunities> communities;
  for (int64_t value = 1; value <= kExpectedCommunitiesEntries; ++value) {
    nettools::bgplib::BgpAttrCommunitiesC communityList;
    communityList.emplace_back(
        /*asn=*/1, static_cast<uint16_t>(value));
    communities.emplace_back(std::move(communityList));
  }

  std::vector<nettools::bgplib::DeDuplicatedClusterList> clusterLists;
  for (int64_t value = 1; value <= kExpectedClusterListEntries; ++value) {
    nettools::bgplib::BgpAttrClusterListC clusterList;
    clusterList.push_back(static_cast<uint32_t>(value));
    clusterLists.emplace_back(std::move(clusterList));
  }

  std::vector<nettools::bgplib::DeDuplicatedExtCommunities> extCommunities;
  for (int64_t value = 1; value <= kExpectedExtCommunitiesEntries; ++value) {
    nettools::bgplib::BgpAttrExtCommunitiesC extCommunityList;
    extCommunityList.emplace_back(
        /*rawValHigh=*/0, static_cast<uint32_t>(value));
    extCommunities.emplace_back(std::move(extCommunityList));
  }

  auto response = folly::coro::blockingWait(service_->co_getDeduplicatorStats(
      std::make_unique<TGetDeduplicatorStatsRequest>()));
  ASSERT_NE(nullptr, response);
  EXPECT_EQ(
      kExpectedBgpPathEntries, response->bgp_path()->entry_count().value());
  EXPECT_EQ(
      kExpectedBgpAttributesEntries,
      response->bgp_attributes()->entry_count().value());
  EXPECT_EQ(kExpectedAsPathEntries, response->as_path()->entry_count().value());
  EXPECT_EQ(
      kExpectedCommunitiesEntries,
      response->communities()->entry_count().value());
  EXPECT_EQ(
      kExpectedClusterListEntries,
      response->cluster_list()->entry_count().value());
  EXPECT_EQ(
      kExpectedExtCommunitiesEntries,
      response->ext_communities()->entry_count().value());
}

TEST_F(
    BgpServiceBaseTestFixture,
    GetAttributeStatsReturnsEmptyCompatibilityPlaceholder) {
  auto response = folly::coro::blockingWait(service_->co_getAttributeStats());

  ASSERT_NE(nullptr, response);
  expectEmptyAttributeStats(*response);
}

TEST_F(
    BgpServiceBaseTestFixture,
    GetAttributeStatsFilteredReturnsEmptyCompatibilityPlaceholder) {
  auto filter = std::make_unique<TAttributeStatsFilter>();
  filter->direction() = TDirectionFilter::EGRESS;
  filter->policyStage() = TPolicyStageFilter::POST_POLICY;
  auto response = folly::coro::blockingWait(
      service_->co_getAttributeStatsFiltered(std::move(filter)));

  ASSERT_NE(nullptr, response);
  expectEmptyAttributeStats(*response);
}
// The getProcessUptimeSeconds handler returns a non-negative value that does
// not go backwards across samples. (A deterministic positive value with a
// controlled start time is verified in WatchdogTest.GetUptimeSecondsTest.)
TEST_F(BgpServiceBaseTestFixture, GetProcessUptimeSecondsTest) {
  const int64_t uptime1 = service_->getProcessUptimeSeconds();
  EXPECT_GE(uptime1, 0);

  const int64_t uptime2 = service_->getProcessUptimeSeconds();
  EXPECT_GE(uptime2, uptime1);
}

} // namespace facebook::bgp
