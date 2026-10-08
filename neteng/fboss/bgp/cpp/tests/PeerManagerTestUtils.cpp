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

#define PeerManager_TEST_FRIENDS       \
  friend class PeerManagerTestFixture; \
  friend class StreamSubscriberFixture;

#define SessionManager_TEST_FRIENDS friend class PeerManagerTestFixture;

#define AdjRib_TEST_FRIENDS friend class PeerManagerTestFixture;

#include "neteng/fboss/bgp/cpp/tests/PeerManagerTestUtils.h"

#include "neteng/fboss/bgp/cpp/config/facebook/ConfigDC.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"

/*
 * The fiber default is 16KB. We are increasing the default due to nested
 * recursion with large stack due to installed exception handlers.
 */
DEFINE_int32(fiber_stack_size, 64, "Default fiber stack size in KB");

using namespace facebook::nettools::bgplib;

using facebook::network::toBinaryAddress;
using facebook::network::toIPPrefix;
using folly::IPAddress;

namespace facebook::bgp {

folly::coro::Task<void> waitForConsumerTimerExpiry() noexcept {
  co_await folly::coro::sleep(std::chrono::milliseconds(kDefaultMraiInterval));
  co_return;
}

std::unique_ptr<BgpUpdate2> createBgpUpdate2(
    uint32_t num,
    folly::IPAddress nexthop) {
  CHECK_LE(num, 255);
  auto update = std::make_unique<BgpUpdate2>();
  for (int i = 0; i < num; i++) {
    auto prefixStr = fmt::format("1.1.{}.0/24", i);
    RiggedIPPrefix rigPrefix;
    rigPrefix.prefix() = toIPPrefix(folly::IPAddress::createNetwork(prefixStr));
    update->mpAnnounced()->afi() = BgpUpdateAfi::AFI_IPv4;
    update->mpAnnounced()->safi() = BgpUpdateSafi::SAFI_UNICAST;
    update->mpAnnounced()->prefixes()->push_back(rigPrefix);
    update->mpAnnounced()->nexthop() =
        toBinaryAddress(folly::IPAddress("1.1.1.1"));
  }
  update->attrs()->origin() = BgpAttrOrigin::BGP_ORIGIN_IGP;
  BgpAttrAsPathSegment segment;
  segment.asSequence()->push_back(100);
  update->attrs()->asPath()->push_back(segment);
  update->attrs()->nexthop() = "1.1.1.1";
  update->attrs()->med() = 32;
  update->attrs()->isMedSet() = true;
  update->attrs()->localPref() = kLocalPref;
  BgpAttrCommunity community;
  community.asn() = 65530;
  community.value() = 15800;
  update->attrs()->communities()->push_back(community);
  update->v4Nexthop() = toBinaryAddress(nexthop);
  return update;
}

// RIB update with prefilled attributes and single v4 or v6 announced prefix
RibOutMessage createRibSingleAnnounce(
    const folly::CIDRNetwork& prefix,
    const folly::IPAddress& nexthop,
    AsNum ribEntryAs,
    bool sendWithEoR,
    bool addPath,
    uint32_t pathIdToSend) {
  RibOutAnnouncement ribMsg;
  BgpUpdate2 update = buildBgpUpdateAttributes(nexthop);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(update)));

  auto peer = prefix.first.isV4()
      ? TinyPeerInfo(
            kLocalV4RoutePeerAddr, ribEntryAs, 0, BgpSessionType::IBGP, false)
      : TinyPeerInfo(
            kLocalV6RoutePeerAddr, ribEntryAs, 0, BgpSessionType::IBGP, false);

  if (addPath) {
    ribMsg.addPathEntries.emplace_back(prefix, pathIdToSend, peer, attrs);
  } else {
    ribMsg.entries.emplace_back(prefix, kDefaultPathID, peer, attrs);
  }
  ribMsg.initialDump = sendWithEoR;
  ribMsg.sendWithEoR = sendWithEoR;

  return ribMsg;
}

// RIB update with prefilled attributes and single v4 or v6 announced prefix
RibOutMessage createRibInitialSingleAnnounce(
    const folly::CIDRNetwork& prefix,
    const folly::IPAddress& nexthop,
    AsNum ribEntryAs,
    bool sendWithEoR,
    bool addPath,
    uint32_t pathIdToSend) {
  RibOutAnnouncement ribMsg;
  BgpUpdate2 update = buildBgpUpdateAttributes(nexthop);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(update)));

  auto peer = prefix.first.isV4()
      ? TinyPeerInfo(
            kLocalV4RoutePeerAddr, ribEntryAs, 0, BgpSessionType::IBGP, false)
      : TinyPeerInfo(
            kLocalV6RoutePeerAddr, ribEntryAs, 0, BgpSessionType::IBGP, false);

  if (addPath) {
    ribMsg.addPathEntries.emplace_back(prefix, pathIdToSend, peer, attrs);
  } else {
    ribMsg.entries.emplace_back(prefix, kDefaultPathID, peer, attrs);
  }
  ribMsg.initialDump = true;
  ribMsg.sendWithEoR = sendWithEoR;

  return ribMsg;
}

/*
 * Create GR state from previous incarnation. Save all the peerAddresses as
 * if they support GR and were established in previous incarnation
 */
void createGrState(std::vector<BgpPeerId> peerIds, bool staleTime) {
  std::ofstream grFile;

  grFile.open(FLAGS_gr_state_file, std::ios::out | std::ios::trunc);
  if (!grFile.is_open()) {
    XLOG(ERR, "Could not open GR state saving file for writing");
    return;
  }

  auto nowInSec = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  if (staleTime) {
    // Stale info by 10 minutes
    nowInSec -= 10 * 60;
  }

  // Start with epoch time
  grFile << nowInSec << "\n";

  // All the established GR peers
  for (const auto& peerId : peerIds) {
    grFile << peerId.peerAddr.str() << " " << peerId.remoteBgpId << "\n";
  }

  // Sign termination (signature) to indicate properly terminated.
  grFile << kGrStateFileTermination;
  grFile.close();
  XLOGF(INFO, "Saved GR state to {}", FLAGS_gr_state_file);
}

/*
 * Verify if the GR state exists or not.
 * Returns TRUE if previous state exists else FALSE
 */
bool isGrStateExists() {
  std::ifstream grFile;

  grFile.open(FLAGS_gr_state_file);

  return grFile.is_open();
}

folly::Future<folly::Unit> TestSessionManager::getSessionsComeUpFuture(
    const std::unordered_set<BgpPeerId>& peerSet,
    const std::chrono::seconds& timeout) {
  /*
   * can only be called before stop, or the evb will not run and
   * execute the scheduled fiber
   */
  folly::Future<folly::Unit> future;
  evb_.runInEventBaseThreadAndWait(
      [this, &future, peerSet = peerSet, timeout = timeout]() {
        future = fm_.addTaskFuture(
            [this, peerSet = peerSet, timeout = timeout]() mutable {
              auto start = std::chrono::steady_clock::now();
              // busy waits till sessions come up
              while (true) {
                if (std::chrono::steady_clock::now() - start > timeout) {
                  XLOG(ERR, "Sessions Come Up timeout");
                  break;
                }
                // TODO: remove busy waiting by forking notify queue
                facebook::nettools::bgplib::fiberSleepFor(
                    std::chrono::milliseconds(100));
                // confirm that sessions come up
                bool allUp = true;
                for (const auto& peerId : peerSet) {
                  if (!isPeerUp(peerId)) {
                    allUp = false;
                    break;
                  }
                }
                if (allUp) {
                  // all sessions are up
                  break;
                }
              }
            });
      });
  return future;
}

folly::Future<folly::Unit> TestSessionManager::getSessionsGoDownFuture(
    const std::unordered_set<BgpPeerId>& peerSet,
    const std::chrono::seconds& timeout) {
  /*
   * can only be called before stop, or the evb will not run and
   * execute the schedule
   */
  folly::Future<folly::Unit> future;
  evb_.runInEventBaseThreadAndWait(
      [this, &future, peerSet = peerSet, timeout = timeout]() {
        future = fm_.addTaskFuture(
            [this, peerSet = peerSet, timeout = timeout]() mutable {
              auto start = std::chrono::steady_clock::now();
              // busy waits till sessions go down
              while (true) {
                if (std::chrono::steady_clock::now() - start > timeout) {
                  XLOG(ERR, "Sessions Go Down timeout");
                  break;
                }
                // TODO: remove busy waiting by forking notify queue
                facebook::nettools::bgplib::fiberSleepFor(
                    std::chrono::milliseconds(100));
                // confirm that sessions go down
                bool allDown = true;
                for (const auto& peerId : peerSet) {
                  if (isPeerUp(peerId)) {
                    allDown = false;
                    break;
                  }
                }
                if (allDown) {
                  break;
                }
              }
            });
      });
  return future;
}

void PeerManagerTestFixture::SetUp() {
  dynamicShivPeer1_ = createBgpPeer(
      kAsn2,
      kLocalAddr1,
      kPeerPrefix1,
      kNextHopV4_1,
      kNextHopV6_1,
      true,
      kPeerTypeShiv);
  dynamicShivPeer2_ = createBgpPeer(
      kAsn1,
      kLocalAddr2,
      kPeerPrefix2,
      kNextHopV4_2,
      kNextHopV6_2,
      true,
      kPeerTypeShiv);
  dynamicMonitorPeer1_ = createBgpPeer(
      kAsn1,
      kLocalAddr2,
      kPeerPrefix3,
      kNextHopV4_2,
      kNextHopV6_2,
      true,
      kPeerTypeBgpMonitor);
  // NOTE: explicitly set rfc5549(v4-over-v6) support
  dynamicMonitorPeer1_.v4_over_v6_nexthop() = true;
  dynamicVipInjectorPeer1_ = createBgpPeer(
      kAsn4,
      kLocalAddr2,
      kPeerPrefix4,
      kNextHopV4_2,
      kNextHopV6_2,
      true,
      kPeerTypeShiv);

  thrift::PeerGroup peerGroup;
  peerGroup.name() = kPeerGroupName1;
  staticPeerGroups.push_back(peerGroup);
  peerGroup.name() = kPeerGroupName2;
  staticPeerGroups.push_back(peerGroup);

  staticPeer1_ = createBgpPeer(
      kPeerAsn3,
      kLocalAddr1,
      kPeerAddr3,
      kNextHopV4_3,
      kNextHopV6_3,
      true,
      kPeerTypeCsw);
  staticPeer1_.description() = kDescription1;
  staticPeer1_.peer_group_name() = kPeerGroupName1;

  staticPeer2_ = createBgpPeer(
      kPeerAsn4,
      kLocalAddr1,
      kPeerAddr4,
      kNextHopV4_4,
      kNextHopV6_4,
      true,
      kPeerTypeCsw);
  /*
   * assign same peer group name as associated with staticPeer1_
   */
  staticPeer2_.peer_group_name() = kPeerGroupName1;

  options_ = nettools::bgplib::getFiberManagerOptions(FLAGS_fiber_stack_size);

  /*
   * Override default GR state file with file based on thread id.
   * This ensures stress run will use different file for each run.
   */
  FLAGS_gr_state_file = fmt::format(
      "/dev/shm/bgp_gr_state.txt.{}",
      std::hash<std::thread::id>{}(std::this_thread::get_id()));

  // spin up a config store
  bgpGlobalConfig1_ = std::make_shared<facebook::bgp::BgpGlobalConfig>(
      kPeerAsn3, // localAsn
      kPeerAddr3, // routerId
      kPeerAddr3, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      std::unordered_map<
          folly::CIDRNetwork,
          thrift::BgpNetwork>(), // networksV4
      std::unordered_map<
          folly::CIDRNetwork,
          thrift::BgpNetwork>() // networksV6
  );

  bgpGlobalConfig2_ = std::make_shared<facebook::bgp::BgpGlobalConfig>(
      kPeerAsn4, // localAsn
      kPeerAddr4, // routerId
      kPeerAddr4, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      std::unordered_map<
          folly::CIDRNetwork,
          thrift::BgpNetwork>(), // networksV4
      std::unordered_map<
          folly::CIDRNetwork,
          thrift::BgpNetwork>() // networksV6
  );

  bgpGlobalConfig3_ = std::make_shared<facebook::bgp::BgpGlobalConfig>(
      kPeerAsn2, // localAsn
      kPeerAddr2, // routerId
      kPeerAddr2, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      std::unordered_map<
          folly::CIDRNetwork,
          thrift::BgpNetwork>(), // networksV4
      std::unordered_map<
          folly::CIDRNetwork,
          thrift::BgpNetwork>() // networksV6
  );

  sessionTerminateBaton_ = std::make_shared<folly::coro::Baton>();
}

void PeerManagerTestFixture::TearDown() {
  remove(FLAGS_gr_state_file.c_str());
}

std::shared_ptr<Config> PeerManagerTestFixture::getConfig(
    bool includeStaticPeer,
    bool includeDynamicShivPeer,
    bool includeDynamicMonitorPeer,
    bool includeDynamicVipInjectorPeer,
    bool enableStatefulHa,
    bool enableVipService,
    int32_t eorTimeS,
    bool enableSubscriberLimit,
    bool enableSwitchLimit,
    bool applyGoldenPrefixPolicy,
    const std::set<std::string>& bgpFeatures,
    bool enableDynamicPolicyEvaluation,
    bool enableUpdateGroup) {
  thrift::BgpConfig thriftConfig;
  thriftConfig.router_id() = kLocalAddr1.str();
  thriftConfig.local_as() = kAsn1;
  thriftConfig.hold_time() = kHoldTime.count();
  thriftConfig.graceful_restart_convergence_seconds() = kGrRestartTime.count();
  thriftConfig.listen_addr() = kLocalAddr1.str();
  thriftConfig.eor_time_s() = eorTimeS;
  /*
   * Stress-test will run multiple instance of tests simultaneously.
   * Binding to a static port is bound to fail in that situation.
   * Picking a pseudo random port > 1024 based on getpid().
   * There is a remote possibility this might also result in a collision but
   * that probability is very very low.
   */
  std::srand((uint16_t)getpid());
  thriftConfig.listen_port() = 1179 + (folly::Random::rand32() % 60000);

  if (includeDynamicShivPeer) {
    thriftConfig.peers()->push_back(dynamicShivPeer1_);
    thriftConfig.peers()->push_back(dynamicShivPeer2_);
  }
  if (includeDynamicMonitorPeer) {
    thriftConfig.peers()->push_back(dynamicMonitorPeer1_);
  }
  if (includeDynamicVipInjectorPeer) {
    thriftConfig.peers()->push_back(dynamicVipInjectorPeer1_);
  }
  if (includeStaticPeer) {
    if (enableStatefulHa) {
      staticPeer1_.enable_stateful_ha() = true;
      staticPeer2_.enable_stateful_ha() = true;
    }
    thriftConfig.peers()->push_back(staticPeer1_);
    thriftConfig.peers()->push_back(staticPeer2_);
    thriftConfig.peer_groups() = staticPeerGroups;
  }

  // VipService related fields
  if (enableVipService) {
    vipconfig::config::VipServiceConfig vipSvcConfig;
    vipSvcConfig.port() = 0;
    vipSvcConfig.min_ttl_s() = 60; // for tests, make the vips' ttl larger
    thriftConfig.vip_service_config() = std::move(vipSvcConfig);
    thriftConfig.enable_vip_service() = true;
  }

  if (enableSubscriberLimit) {
    thriftConfig.bgp_setting_config() = thrift::BgpSettingConfig();
    std::set<std::string> features = {"stream_subscriber_limit"};
    thriftConfig.bgp_setting_config()->features() = std::move(features);
  }

  if (enableSwitchLimit) {
    thrift::BgpSwitchLimitConfig switchLimitConfig;
    switchLimitConfig.overload_protection_mode() = applyGoldenPrefixPolicy
        ? thrift::OverloadProtectionMode::APPLY_GOLDEN_PREFIX_POLICY
        : thrift::OverloadProtectionMode::DROP_EXCESS_PREFIXES;
    switchLimitConfig.prefix_limit() = 1;
    switchLimitConfig.total_path_limit() = 1;
    thriftConfig.switch_limit_config() = switchLimitConfig;
  }

  // setup bgpSettingConfig with features and dynamic policy evaluation flag
  thrift::BgpSettingConfig tBgpSettingConfig;
  tBgpSettingConfig.features() = bgpFeatures;
  tBgpSettingConfig.enable_dynamic_policy_evaluation() =
      enableDynamicPolicyEvaluation;
  tBgpSettingConfig.enable_update_group() = enableUpdateGroup;

  // Move the settings into the thrift config
  thriftConfig.bgp_setting_config() = std::move(tBgpSettingConfig);

  FeatureFlags::LoadFromThriftConfig(thriftConfig);

  if (enableVipService) {
    return std::make_shared<ConfigDC>(thriftConfig);
  }
  return std::make_shared<Config>(thriftConfig);
}

std::shared_ptr<Config> PeerManagerTestFixture::addPeerToConfig(
    const std::shared_ptr<Config>& config,
    thrift::BgpPeer& peer,
    const std::optional<const std::string>& peerGroupName) {
  thrift::BgpConfig thriftConfig = config->getConfig();
  if (peerGroupName) {
    peer.peer_group_name() = *peerGroupName;

    thrift::PeerGroup peerGroup;
    peerGroup.name() = *peerGroupName;
    thriftConfig.peer_groups()->push_back(peerGroup);
  }
  thriftConfig.peers()->push_back(peer);
  if (*thriftConfig.enable_vip_service()) {
    return std::make_shared<ConfigDC>(thriftConfig);
  }
  return std::make_shared<Config>(thriftConfig);
}

void PeerManagerTestFixture::initTwoSessionMgrs(
    folly::fibers::FiberManager* fm) {
  sessionMgr1_ = std::make_shared<TestSessionManager>(
      *bgpGlobalConfig1_, &callback1_, *fm, false);

  sessionMgr2_ = std::make_shared<TestSessionManager>(
      *bgpGlobalConfig2_, &callback2_, *fm, false);
}

void PeerManagerTestFixture::initThreeSessionMgrs(
    folly::fibers::FiberManager* fm) {
  sessionMgr1_ = std::make_shared<TestSessionManager>(
      *bgpGlobalConfig1_, &callback1_, *fm, false);

  sessionMgr2_ = std::make_shared<TestSessionManager>(
      *bgpGlobalConfig2_, &callback2_, *fm, false);

  sessionMgr3_ = std::make_shared<TestSessionManager>(
      *bgpGlobalConfig3_, &callback3_, *fm, false);
}

std::shared_ptr<AdjRib> PeerManagerTestFixture::setupAdjRib(
    folly::EventBase& evb,
    std::shared_ptr<ChangeTracker<ShadowRibEntry>> changeListTracker,
    const BgpPeerId& peerId,
    const AsNum& remoteAs,
    std::shared_ptr<folly::coro::Baton>& sessionTerminateBaton,
    std::shared_ptr<const Config> config,
    bool isRrClient,
    const folly::IPAddress& v4Nexthop,
    const folly::IPAddress& v6Nexthop,
    bool enableStatefulHa,
    bool v4OverV6Nexthop,
    std::shared_ptr<PolicyManager> policyManager) {
  auto adjRibOutGroup =
      std::make_shared<AdjRibOutGroup>(evb, "PeerManagerTest");
  auto adjRib = std::make_shared<AdjRib>(
      peerId,
      PeeringParams(
          peerId.peerAddr,
          std::nullopt, // peerPrefix
          kAsn1,
          kAsn1,
          remoteAs,
          kLocalAddr1.asV4(),
          kLocalAddr1.asV4(),
          std::chrono::seconds(kDefaultHoldTime),
          std::chrono::seconds(kGrRestartTime),
          nettools::bgplib::constants::kBgpPort,
          folly::AsyncSocket::anyAddress(),
          TBgpSessionConnectMode::PASSIVE_ACTIVE,
          v4Nexthop.asV4(),
          v6Nexthop.asV6(),
          RrClientConfigured(isRrClient),
          NextHopSelfConfigured{false},
          AfiIpv4Configured{true},
          AfiIpv6Configured{true},
          ConfedPeerConfigured{false},
          RemovePrivateAsConfigured{false},
          std::nullopt, // localConfedAs
          std::nullopt, // asConfedId
          AdvertiseLinkBandwidth::DISABLE,
          ReceiveLinkBandwidth::ACCEPT,
          std::nullopt, // linkBandwidthBps
          ValidateRemoteAs{true},
          std::nullopt, // preRouteLimit
          std::nullopt, // postRouteLimit
          false, // allowLoopbackReflection
          enableStatefulHa ? EnableStatefulHa{true} : EnableStatefulHa{false},
          std::nullopt, // addPath
          V4OverV6Nexthop{v4OverV6Nexthop}),
      evb,
      ribInQ_, /* write to the queue */
      observerQ_,
      sessionTerminateBaton,
      policyManager,
      isSafeModeOn_,
      std::nullopt,
      std::nullopt,
      adjRibOutGroup,
      std::nullopt /* outDelay */,
      std::make_shared<ConfigManager>(config));
  static ConsumerBitmap dummyAddPathBitmap;
  static ConsumerBitmap dummyNonAddPathBitmap;
  auto changeListConsumer = std::make_shared<AdjRibOutConsumer>(
      changeListTracker,
      adjRib,
      "ChangeList Consumer",
      evb,
      dummyAddPathBitmap,
      dummyNonAddPathBitmap);
  adjRib->setChangeListConsumer(changeListConsumer);
  return adjRib;
}

// Overload with bitmap parameters for selective multipath notification
std::shared_ptr<AdjRib> PeerManagerTestFixture::setupAdjRib(
    folly::EventBase& evb,
    std::shared_ptr<ChangeTracker<ShadowRibEntry>> changeListTracker,
    const BgpPeerId& peerId,
    const AsNum& remoteAs,
    std::shared_ptr<folly::coro::Baton>& sessionTerminateBaton,
    std::shared_ptr<const Config> config,
    ConsumerBitmap& addPathBitmap,
    ConsumerBitmap& nonAddPathBitmap,
    bool isRrClient,
    const folly::IPAddress& v4Nexthop,
    const folly::IPAddress& v6Nexthop,
    bool enableStatefulHa,
    bool v4OverV6Nexthop) {
  auto adjRibOutGroup =
      std::make_shared<AdjRibOutGroup>(evb, "PeerManagerTest");
  auto adjRib = std::make_shared<AdjRib>(
      peerId,
      PeeringParams(
          peerId.peerAddr,
          std::nullopt, // peerPrefix
          kAsn1,
          kAsn1,
          remoteAs,
          kLocalAddr1.asV4(),
          kLocalAddr1.asV4(),
          std::chrono::seconds(kDefaultHoldTime),
          std::chrono::seconds(kGrRestartTime),
          nettools::bgplib::constants::kBgpPort,
          folly::AsyncSocket::anyAddress(),
          TBgpSessionConnectMode::PASSIVE_ACTIVE,
          v4Nexthop.asV4(),
          v6Nexthop.asV6(),
          RrClientConfigured(isRrClient),
          NextHopSelfConfigured{false},
          AfiIpv4Configured{true},
          AfiIpv6Configured{true},
          ConfedPeerConfigured{false},
          RemovePrivateAsConfigured{false},
          std::nullopt, // localConfedAs
          std::nullopt, // asConfedId
          AdvertiseLinkBandwidth::DISABLE,
          ReceiveLinkBandwidth::ACCEPT,
          std::nullopt, // linkBandwidthBps
          ValidateRemoteAs{true},
          std::nullopt, // preRouteLimit
          std::nullopt, // postRouteLimit
          false, // allowLoopbackReflection
          enableStatefulHa ? EnableStatefulHa{true} : EnableStatefulHa{false},
          std::nullopt, // addPath
          V4OverV6Nexthop{v4OverV6Nexthop}),
      evb,
      ribInQ_, /* write to the queue */
      observerQ_,
      sessionTerminateBaton,
      nullptr /* PolicyManager */,
      isSafeModeOn_,
      std::nullopt,
      std::nullopt,
      adjRibOutGroup,
      std::nullopt /* outDelay */,
      std::make_shared<ConfigManager>(config));
  auto changeListConsumer = std::make_shared<AdjRibOutConsumer>(
      changeListTracker,
      adjRib,
      "ChangeList Consumer",
      evb,
      addPathBitmap,
      nonAddPathBitmap);
  adjRib->setChangeListConsumer(changeListConsumer);
  return adjRib;
}

std::shared_ptr<MockAdjRib> PeerManagerTestFixture::setupMockAdjRib(
    folly::EventBase& evb,
    const BgpPeerId& peerId,
    const AsNum& remoteAs,
    std::shared_ptr<folly::coro::Baton>& sessionTerminateBaton,
    bool isRrClient,
    const folly::IPAddress& v4Nexthop,
    const folly::IPAddress& v6Nexthop,
    bool enableStatefulHa,
    bool v4OverV6Nexthop) {
  auto adjRib = std::make_shared<MockAdjRib>(
      peerId,
      PeeringParams(
          peerId.peerAddr,
          std::nullopt, // peerPrefix
          kAsn1,
          kAsn1,
          remoteAs,
          kLocalAddr1.asV4(),
          kLocalAddr1.asV4(),
          std::chrono::seconds(kDefaultHoldTime),
          std::chrono::seconds(kGrRestartTime),
          nettools::bgplib::constants::kBgpPort,
          folly::AsyncSocket::anyAddress(),
          TBgpSessionConnectMode::PASSIVE_ACTIVE,
          v4Nexthop.asV4(),
          v6Nexthop.asV6(),
          RrClientConfigured(isRrClient),
          NextHopSelfConfigured{false},
          AfiIpv4Configured{true},
          AfiIpv6Configured{true},
          ConfedPeerConfigured{false},
          RemovePrivateAsConfigured{false},
          std::nullopt, // localConfedAs
          std::nullopt, // asConfedId
          AdvertiseLinkBandwidth::DISABLE,
          ReceiveLinkBandwidth::ACCEPT,
          std::nullopt, // linkBandwidthBps
          ValidateRemoteAs{true},
          std::nullopt, // preRouteLimit
          std::nullopt, // postRouteLimit
          false, // allowLoopbackReflection
          enableStatefulHa ? EnableStatefulHa{true} : EnableStatefulHa{false},
          std::nullopt, // addPath
          V4OverV6Nexthop{v4OverV6Nexthop}),
      evb,
      ribInQ_, /* write to the queue */
      observerQ_,
      sessionTerminateBaton,
      nullptr /* PolicyManager */,
      isSafeModeOn_);
  return adjRib;
}

/*
 * Creates a mock peer manager with appropriate config and
 * mock session manager
 */
std::shared_ptr<MockPeerManager> PeerManagerTestFixture::setupMockPeerManager(
    bool includeStaticPeer,
    bool includeDynamicShivPeer,
    bool includeDynamicMonitorPeer,
    bool includeDynamicVipInjectorPeer) {
  // create session manager
  config_ = getConfig(
      includeStaticPeer,
      includeDynamicShivPeer,
      includeDynamicMonitorPeer,
      includeDynamicVipInjectorPeer);

  /*
   * Setup values returned to sessionEstablishement
   * getEstablishedPeerDisplayInfo
   */
  auto peeringParams = bgp::PeeringParams();
  peeringParams.peerAddr = kPeerAddr1;
  peeringParams.peerPrefix = kV4Prefix3;
  peeringParams.localBgpId = kLocalAddr1.asV4();
  peeringParams.description = kDescription1;
  peeringParams.peerId = fmt::format("{}:v4:1", peeringParams.description);
  peeringParams.isEnhancedRouteRefreshConfigured = true;
  mockInfo1_.peeringParams = peeringParams;

  // create MockPeerManager with the config_
  auto configManager = std::make_shared<ConfigManager>(config_);
  auto mockPeerMgr = std::make_shared<MockPeerManager>(
      configManager,
      ribInQ_, /* write to the queue */
      ribOutQ_, /* read from the queue */
      nbrRouteChangeQ_);
  setupMockSessionManager(mockPeerMgr);

  mockPeerMgr->isSafeModeOn_ = isSafeModeOn_;
  return mockPeerMgr;
}

/*
 * create a mock peer manager in a separate thread such that funcitons running
 * in eventbase loop can also be scheduled
 */
std::shared_ptr<MockPeerManager>
PeerManagerTestFixture::setupMockPeerManagerWithSeparateThread(
    bool includeStaticPeer,
    bool includeDynamicShivPeer) {
  // create session manager
  config_ = getConfig(includeStaticPeer, includeDynamicShivPeer);
  auto globalConfig = config_->getBgpGlobalConfig();
  // initialize peer manager thread fiber manager

  // create MockPeerManager with the config_
  auto configManager = std::make_shared<ConfigManager>(config_);
  auto mockPeerMgr = std::make_shared<MockPeerManager>(
      configManager,
      ribInQ_, /* write to the queue */
      ribOutQ_, /* read from the queue */
      nbrRouteChangeQ_);

  return mockPeerMgr;
}

/*
 * create a mock session manager which shares the same fiber manager with
 * the mock peer manager. The mockPeerMgr should be a non-empty pointer, e.g.,
 * generated by setupMockPeerManager or setupMockPeerManagerWithSeparateThread
 */
std::shared_ptr<MockSessionManager>
PeerManagerTestFixture::setupMockSessionManager(
    std::shared_ptr<MockPeerManager>& mockPeerMgr) {
  // mockPeerMgr should not be empty
  EXPECT_TRUE(mockPeerMgr != nullptr);
  EXPECT_TRUE(mockPeerMgr->changeListTracker_ != nullptr);

  auto globalConfig = config_->getBgpGlobalConfig();

  auto iQueue = std::make_shared<nettools::bgplib::FiberBgpPeer::InputQueueT>();
  auto oQueue = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto biQueue = std::make_shared<AdjRib::AdjRibOutQueueT>(
      kMaxEgressQueueSize, kEgressQueueHighWatermark, kEgressQueueLowWatermark);

  auto mockSessionMgr = std::make_shared<MockSessionManager>(
      *globalConfig,
      false, /* enableMessagesOverNotifyQueue */
      iQueue,
      biQueue,
      oQueue,
      true /* enableCoroNotifyQueue - required for PeerManagerBase's
              processPeerEventLoop */
  );
  mockPeerMgr->setSessionManager(mockSessionMgr);

  // Set default return values for mocked methods
  ON_CALL(*mockSessionMgr, getEstablishedPeerDisplayInfo(testing::_))
      .WillByDefault(testing::Return(mockInfo1_));

  return mockSessionMgr;
}

// Helper function to create a mock peer info for a static peer
std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>
PeerManagerTestFixture::getMockPeerInfo(
    const folly::IPAddress& peerAddr /* static peer */,
    const uint64_t& routerId,
    const uint64_t& numReset /* number of session went down */,
    const uint64_t& lastWentDown /* hours ago */,
    const nettools::bgplib::ResetReason&
        lastResetReason /* iff numReset > 0 */) {
  auto mockConfig = getConfig(
      true /* includeStaticPeer */, false /* includeDynamicShivPeer */);
  auto staticPeerToConfig = mockConfig->getPeerToConfig();
  auto peeringParams =
      mockConfig->getPeeringParamsForPeer(*staticPeerToConfig.at(peerAddr));
  auto peerInfo = std::make_shared<nettools::bgplib::BgpPeerDisplayInfo>();
  peerInfo->peeringParams = peeringParams;
  peerInfo->remoteBgpId = routerId;
  peerInfo->remoteGrRestartTime = std::nullopt;
  peerInfo->state = nettools::bgplib::BgpSessionState::ESTABLISHED;
  peerInfo->localAddr = folly::SocketAddress("127.0.0.1", 1234);
  peerInfo->startTime = std::chrono::steady_clock::now();
  peerInfo->establishedTime = std::chrono::steady_clock::now();
  peerInfo->negotiatedCapabilities = nettools::bgplib::BgpCapabilities();
  peerInfo->remoteAs = peeringParams.remoteAs;
  peerInfo->negotiatedHoldTime = std::nullopt;
  peerInfo->numOfConnectionAttempts = 0;
  peerInfo->lastResetHoldTimer = 0;
  peerInfo->lastResetKeepAliveTimer = 0;
  peerInfo->lastReceivedKeepAlive = 0;
  peerInfo->lastSentKeepAlive = 0;
  peerInfo->numResets = numReset;
  if (numReset > 0) {
    peerInfo->lastResetTime =
        peerInfo->startTime - std::chrono::hours(lastWentDown);
    peerInfo->lastResetReason = lastResetReason;
  } else {
    peerInfo->lastResetReason = std::nullopt;
    peerInfo->lastResetTime = std::chrono::steady_clock::now();
  }

  return peerInfo;
};

// Helper function to create a mock peer info for a dynamic peer
std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>
PeerManagerTestFixture::getMockPeerInfo(
    const folly::CIDRNetwork& prefix /* dynamic peer */,
    const uint64_t& routerId,
    bool vipInjectorPeer,
    bool otherDynamicPeer) {
  auto mockConfig = getConfig(false, otherDynamicPeer, false, vipInjectorPeer);
  auto dynamicPeerToConfig = mockConfig->getDynamicPeerToConfig();
  auto param1 = mockConfig->getPeeringParamsForDynamicPeer(
      *dynamicPeerToConfig.at(prefix));
  auto peerInfo = std::make_shared<nettools::bgplib::BgpPeerDisplayInfo>();
  peerInfo->peeringParams = param1;
  peerInfo->remoteBgpId = routerId;
  peerInfo->remoteGrRestartTime = std::nullopt;
  peerInfo->state = nettools::bgplib::BgpSessionState::ESTABLISHED;
  peerInfo->localAddr = folly::SocketAddress("127.0.0.1", 1234);
  peerInfo->startTime = std::chrono::steady_clock::now();
  peerInfo->establishedTime = std::chrono::steady_clock::now();
  peerInfo->negotiatedCapabilities = nettools::bgplib::BgpCapabilities();
  peerInfo->remoteAs = param1.remoteAs;
  peerInfo->negotiatedHoldTime = std::nullopt;
  peerInfo->numOfConnectionAttempts = 0;
  peerInfo->lastResetHoldTimer = 0;
  peerInfo->lastResetKeepAliveTimer = 0;
  peerInfo->lastReceivedKeepAlive = 0;
  peerInfo->lastSentKeepAlive = 0;
  peerInfo->lastResetReason = std::nullopt;
  return peerInfo;
}

folly::coro::Task<void> PeerManagerTestFixture::waitForAdjRibsToProcessUpdates(
    folly::EventBase& evb,
    std::vector<std::shared_ptr<AdjRib::AdjRibOutQueueT>> queues) {
  evb.loopOnce();
  for (auto& q : queues) {
    while (!q->empty()) {
      co_await facebook::bgp::test::boundedPop(*q, "q");
    }
  }
}

/**
 * @brief Parameterized SetUp() function for StreamSubscriberFixture tests
 * Note that this SetUp() function should be explicitly called within the test
 *
 * @param configureMonitorPeer: If true, configure a BgpMonitor peer
 * @param initialAnnouncementDone: ribInitialAnnouncementDone_ flag in the
 * peerManager
 * @param enableSubscriberLimit: Enable stream subscriber limit.
 */
void StreamSubscriberFixture::SetUp(
    bool configureMonitorPeer,
    bool initialAnnouncementDone,
    bool enableSubscriberLimit) {
  PeerManagerTestFixture::SetUp();

  // Get the BgpMonitor peer config
  config_ = getConfig(
      false /* includeStaticPeer */,
      false /* includeDynamicShivPeer */,
      configureMonitorPeer,
      false /* includeDynamicVipInjectorPeer */,
      false /* enableStatefulHa */,
      true /* enableVipServer */,
      kDefaultEorTimeS /* eorTimeS = */,
      enableSubscriberLimit);

  if (enableSubscriberLimit) {
    // Set the stream subscriber limit to 1
    config_->getBgpGlobalConfig()->streamSubscriberLimit = 1;
  }

  // Instantiate peerManager object
  auto configManager = std::make_shared<ConfigManager>(config_);
  peerMgr = std::make_shared<PeerManagerBase>(
      configManager, nullptr, ribInQ_, ribOutQ_, nbrRouteChangeQ_);

  sessionMgr =
      std::make_shared<SessionManager>(*config_->getBgpGlobalConfig(), false);
  peerMgr->setSessionManager(sessionMgr);

  // Simulate the peerManager flags.
  peerMgr->eorTimerExpired_ = false;
  peerMgr->initialized_ = true;
  /*
   * If both flags below are true, this indicates that the EOR is sent out
   * after initial Fib sync.
   */
  peerMgr->ribInitialAnnouncementStarted_ = true;
  peerMgr->ribInitialAnnouncementDone_ = initialAnnouncementDone;

  // create peer manager thread
  peerMgrThread = std::make_shared<std::thread>(peerMgr->runInThread());

  sessionMgrThread = std::make_shared<std::thread>(sessionMgr->runInThread());
}

/**
 * @brief Override default tear down function
 */
void StreamSubscriberFixture::TearDown() {
  PeerManagerTestFixture::TearDown();
  peerMgr->markDaemonShutdown();
  sessionMgr->stop();
  peerMgr->stop();
  peerMgrThread->join();
  sessionMgrThread->join();
  SUCCEED();
}

/**
 * @brief Implementation of
 * PeerManagerDynamicPolicyEvaluationFixture::SetUp()
 *
 * This initializes the configuration with dynamic policy evaluation
 * enabled/disabled based on the parameter.
 */
void PeerManagerDynamicPolicyEvaluationFixture::SetUp(
    bool enableDynamicPolicyEvaluation) {
  // Call parent's SetUp first
  PeerManagerTestFixture::SetUp();

  /*
   * Create the config with appropriate parameters for dynamic policy evaluation
   * testing
   */
  config_ = getConfig(
      true, // includeStaticPeer
      true, // includeDynamicShivPeer
      false, // includeDynamicMonitorPeer
      false, // includeDynamicVipInjectorPeer
      false, // enableStatefulHa
      true, // enableVipService
      kDefaultEorTimeS, // eorTimeS
      false, // enableSubscriberLimit
      false, // enableSwitchLimit
      false, // applyGoldenPrefixPolicy
      {}, // bgpFeatures
      enableDynamicPolicyEvaluation);
}

void PeerManagerDynamicPolicyEvaluationFixture::verifyStateWithRetries(
    folly::EventBase& evb,
    const std::vector<AdjRibPolicyUpdateState>& adjRibStates) {
  // Wait for async operations to complete and verify the final steady state
  WITH_RETRIES_N(5, {
    evb.runInEventBaseThreadAndWait([&]() {
      for (const auto& state : adjRibStates) {
        EXPECT_EVENTUALLY_EQ(
            state.expectedIngressPendingPolicyUpdate,
            state.adjRib->isPendingIngressPolicyUpdate());
        EXPECT_EVENTUALLY_EQ(
            state.expectedEgressPendingPolicyUpdate,
            state.adjRib->isEgressPolicyUpdateRequired());
      }
    });
  });
}

void PeerManagerDynamicPolicyEvaluationFixture::verifyState(
    folly::EventBase& evb,
    const std::vector<AdjRibPolicyUpdateState>& adjRibStates) {
  evb.runInEventBaseThreadAndWait([&]() {
    for (const auto& state : adjRibStates) {
      EXPECT_EQ(
          state.expectedIngressPendingPolicyUpdate,
          state.adjRib->isPendingIngressPolicyUpdate());
      EXPECT_EQ(
          state.expectedEgressPendingPolicyUpdate,
          state.adjRib->isEgressPolicyUpdateRequired());
    }
  });
}

void PeerManagerDynamicPolicyEvaluationFixture::verifyRouteFilterStatement(
    folly::EventBase& evb,
    const std::vector<
        std::pair<std::shared_ptr<AdjRib>, rib_policy::TRouteFilterStatement>>&
        adjRibStatements) {
  WITH_RETRIES_N(5, {
    evb.runInEventBaseThreadAndWait([&]() {
      for (const auto& [adjRib, expectedStatement] : adjRibStatements) {
        auto currentStatement = adjRib->getRouteFilterStatement();
        EXPECT_EVENTUALLY_NE(nullptr, currentStatement);
        if (currentStatement) {
          EXPECT_EVENTUALLY_EQ(expectedStatement, currentStatement->toThrift());
        }
      }
    });
  });
}

std::unique_ptr<PeerToPolicyMap> createPolicyMap(
    const std::vector<std::tuple<std::string, std::string, std::string>>&
        policyMapEntries) {
  auto policyMap = std::make_unique<PeerToPolicyMap>();

  for (const auto& [key, ingressPolicy, egressPolicy] : policyMapEntries) {
    /*
     * Empty string means "clear/unset policy" (std::nullopt)
     * Non-empty string means "set to this policy"
     */
    (*policyMap)[key][facebook::bgp::bgp_policy::DIRECTION::IN] =
        ingressPolicy.empty() ? std::nullopt
                              : std::make_optional(ingressPolicy);
    (*policyMap)[key][facebook::bgp::bgp_policy::DIRECTION::OUT] =
        egressPolicy.empty() ? std::nullopt : std::make_optional(egressPolicy);
  }

  return policyMap;
}

} // namespace facebook::bgp
