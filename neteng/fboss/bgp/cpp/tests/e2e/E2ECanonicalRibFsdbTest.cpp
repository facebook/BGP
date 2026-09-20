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

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <atomic>
#include <optional>

#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/synchronization/Baton.h>

namespace facebook::bgp {
class E2ECanonicalRibFsdbTest;
}

#define FSDB_SYNCER_TEST_FRIENDS \
  friend class ::facebook::bgp::E2ECanonicalRibFsdbTest;
#define FsdbPubSubManager_TEST_FRIENDS \
  friend class ::facebook::bgp::E2ECanonicalRibFsdbTest

#include "fboss/fsdb/client/FsdbPatchPublisher.h"
#include "neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.h"

#undef FsdbPubSubManager_TEST_FRIENDS
#undef FSDB_SYNCER_TEST_FRIENDS

#include "fboss/fsdb/tests/utils/FsdbTestServer.h"
#include "fboss/fsdb/tests/utils/FsdbTestSubscriber.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/rib/RibDC.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

namespace facebook::bgp {

using fboss::fsdb::test::FsdbTestServer;
using fboss::fsdb::test::FsdbTestSubscriber;

class ScopedEventBaseBlocker {
 public:
  explicit ScopedEventBaseBlocker(folly::EventBase& eventBase)
      : eventBase_(eventBase) {
    eventBase_.runInEventBaseThread([this]() {
      blocked_.post();
      if (!release_.try_wait_for(test::kDefaultPopTimeout)) {
        timedOut_.store(true);
      }
    });
  }

  ~ScopedEventBaseBlocker() {
    release();
  }

  void waitUntilBlocked(folly::StringPiece context) {
    test::boundedBatonWait(blocked_, context);
  }

  void release() {
    if (released_.exchange(true)) {
      return;
    }
    release_.post();
    eventBase_.runInEventBaseThreadAndWait([]() {});
  }

  bool timedOut() const {
    return timedOut_.load();
  }

 private:
  folly::EventBase& eventBase_;
  folly::Baton<> blocked_;
  folly::Baton<> release_;
  std::atomic<bool> released_{false};
  std::atomic<bool> timedOut_{false};
};

class E2ECanonicalRibFsdbTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    FLAGS_publish_state_to_fsdb = true;
    FLAGS_publish_rib_to_fsdb = true;
    FLAGS_fsdb_reconnect_ms = 100;
    FLAGS_fsdb_initial_backoff_reconnect_ms = 100;
    FLAGS_fsdb_max_backoff_reconnect_ms = 100;
    FLAGS_publish_multipaths_to_fsdb = publishMultipathsToFsdb();

    fsdbServer_ = std::make_unique<FsdbTestServer>();
    fsdbPort_ = fsdbServer_->getFsdbPort();
    FLAGS_fsdbPort = fsdbPort_;
    if (!fsdbAvailableAtStartup()) {
      fsdbServer_.reset();
    }
    subscriber_ = std::make_unique<FsdbTestSubscriber>("rib-e2e-subscriber");
    syncer_ = std::make_unique<FsdbSyncer>(
        *fsdbSyncerEventBaseThread_.getEventBase());

    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec5);
    setPeerGrRestartTimeSeconds(grRestartTimeSeconds());
    createRib(
        /*enableNexthopTracking=*/false,
        /*localRoutes=*/{},
        syncer_.get());
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
  }

  void bringUpPeers() {
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr5);
    BgpPeerId peer3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId peer5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
    sendEoRToPeer(peer3);
    sendEoRToPeer(peer5);
    ASSERT_TRUE(waitForEoR(peer3));
    ASSERT_TRUE(waitForEoR(peer5));
  }

  void assertBgpPipelineProgress(
      const std::string& prefix,
      uint8_t prefixLength,
      const folly::IPAddress& expectedFibNexthop) {
    const folly::CIDRNetwork network{folly::IPAddress(prefix), prefixLength};
    const auto networkString = folly::IPAddress::networkToString(network);

    ASSERT_TRUE(waitForPathCountInRib(networkString, 1));
    ASSERT_NE(nullptr, getBestPath(network));
    WITH_RETRIES({
      const auto fibNexthops = getFibWeightedNexthops(networkString);
      EXPECT_EVENTUALLY_TRUE(
          fibNexthops && fibNexthops->contains(expectedFibNexthop));
    });
    EXPECT_TRUE(drainAndFindRouteAdvertised(
        "v4", prefix, prefixLength, kPeerAddr5, kNextHopV4_5.str()));
  }

  folly::EventBase* fsdbPublisherStreamEventBase() const {
    if (!syncer_ || !syncer_->fsdbPubSubMgr_) {
      return nullptr;
    }
    return syncer_->fsdbPubSubMgr_->statePublisherEvb_;
  }

  std::optional<ssize_t> fsdbPublisherQueueSize() const {
    if (!syncer_ || !syncer_->fsdbPubSubMgr_) {
      return std::nullopt;
    }
    const auto* publisher = syncer_->fsdbPubSubMgr_->getPatchPublisher();
    return publisher ? std::optional<ssize_t>{publisher->queueSize()}
                     : std::nullopt;
  }

  /*
   * Returns whether multipath publication must be enabled before RIB startup.
   */
  virtual bool publishMultipathsToFsdb() const {
    return false;
  }

  /* Returns the GR restart time configured before the peers start. */
  virtual int grRestartTimeSeconds() const {
    return 1;
  }

  /* Returns whether the FSDB endpoint is available when BGP starts. */
  virtual bool fsdbAvailableAtStartup() const {
    return true;
  }

  static std::vector<bgp_thrift::TBgpDedupedPath> pathsForPrefix(
      const bgp_thrift::TCanonicalRibState& state,
      const std::string& prefix) {
    std::vector<bgp_thrift::TBgpDedupedPath> paths;
    if (!state.rib_entries()->contains(prefix)) {
      return paths;
    }
    const auto& entry = state.rib_entries()->at(prefix);
    /*
     * In multipath mode, best_path is a summary copy of the one selected
     * member already represented in paths. Only use it for best-path-only
     * state; otherwise appending it would double-count that member.
     */
    if (entry.paths()->empty() && entry.best_path().has_value()) {
      paths.push_back(*entry.best_path());
    }
    for (const auto& [group, instances] : *entry.paths()) {
      for (const auto& instance : instances) {
        const auto pathIdx = *instance.path_idx();
        if (state.deduped_paths()->contains(pathIdx)) {
          paths.push_back(state.deduped_paths()->at(pathIdx));
        }
      }
    }
    return paths;
  }

  static bool hasNexthop(
      const bgp_thrift::TCanonicalRibState& state,
      const std::string& prefix,
      const folly::IPAddress& nexthop) {
    const auto expected = createTIpPrefix(nexthop);
    for (const auto& path : pathsForPrefix(state, prefix)) {
      if (*path.next_hop() == expected) {
        return true;
      }
    }
    return false;
  }

  static std::optional<int64_t> communityIndexForPrefix(
      const bgp_thrift::TCanonicalRibState& state,
      const std::string& prefix) {
    for (const auto& path : pathsForPrefix(state, prefix)) {
      if (path.communities_idx().has_value()) {
        return *path.communities_idx();
      }
    }
    return std::nullopt;
  }

  static bool hasCommunity(
      const bgp_thrift::TCanonicalRibState& state,
      const std::string& prefix,
      int32_t asn,
      int32_t value) {
    const auto idx = communityIndexForPrefix(state, prefix);
    if (!idx.has_value() ||
        !state.attr_dict()->community_lists()->contains(*idx)) {
      return false;
    }
    for (const auto& community :
         state.attr_dict()->community_lists()->at(*idx)) {
      if (*community.asn() == asn && *community.value() == value) {
        return true;
      }
    }
    return false;
  }

  static bool canonicalReferencesResolve(
      const bgp_thrift::TCanonicalRibState& state) {
    const auto& dict = *state.attr_dict();
    auto validPath = [&](const bgp_thrift::TBgpDedupedPath& path) {
      return (!path.as_path_idx().has_value() ||
              dict.as_path_lists()->contains(*path.as_path_idx())) &&
          (!path.communities_idx().has_value() ||
           dict.community_lists()->contains(*path.communities_idx())) &&
          (!path.ext_communities_idx().has_value() ||
           dict.ext_community_lists()->contains(*path.ext_communities_idx())) &&
          (!path.cluster_list_idx().has_value() ||
           dict.cluster_lists()->contains(*path.cluster_list_idx()));
    };

    for (const auto& [prefix, entry] : *state.rib_entries()) {
      if (entry.best_path().has_value() && !validPath(*entry.best_path())) {
        return false;
      }
      for (const auto& [group, paths] : *entry.paths()) {
        for (const auto& path : paths) {
          if (!state.deduped_paths()->contains(*path.path_idx()) ||
              !validPath(state.deduped_paths()->at(*path.path_idx()))) {
            return false;
          }
          if (path.peer_idx().has_value() &&
              !state.peers()->contains(*path.peer_idx())) {
            return false;
          }
        }
      }
    }
    return true;
  }

  void TearDown() override {
    E2ETestFixture::TearDown();
    if (syncer_) {
      syncer_->stop();
    }
    subscriber_.reset();
    syncer_.reset();
    fsdbServer_.reset();
  }

  gflags::FlagSaver flagSaver_;
  uint16_t fsdbPort_{0};
  std::unique_ptr<FsdbTestServer> fsdbServer_;
  std::unique_ptr<FsdbTestSubscriber> subscriber_;
  folly::ScopedEventBaseThread fsdbSyncerEventBaseThread_{"FsdbSyncerE2E"};
  std::unique_ptr<FsdbSyncer> syncer_;
};

class E2ECanonicalMultipathRibFsdbTest : public E2ECanonicalRibFsdbTest {
 protected:
  bool publishMultipathsToFsdb() const override {
    return true;
  }
};

class E2ECanonicalLongGrRibFsdbTest : public E2ECanonicalRibFsdbTest {
 protected:
  int grRestartTimeSeconds() const override {
    return 30;
  }
};

class E2ECanonicalNoFsdbAtStartupTest : public E2ECanonicalRibFsdbTest {
 protected:
  bool fsdbAvailableAtStartup() const override {
    return false;
  }
};

TEST_F(E2ECanonicalRibFsdbTest, SubscriberResyncsAfterPublisherRestart) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());

  bringUpPeers();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE((*state)->rib_entries()->contains("10.0.0.0/8"));
  });

  const auto fsdbPort = fsdbServer_->getFsdbPort();
  fsdbServer_.reset();
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));

  fsdbServer_ = std::make_unique<FsdbTestServer>(fsdbPort);
  WITH_RETRIES_N_TIMED(100, std::chrono::milliseconds(100), {
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_FALSE((*state)->rib_entries()->contains("10.0.0.0/8"));
  });
}

TEST_F(
    E2ECanonicalNoFsdbAtStartupTest,
    UnavailableFsdbDoesNotBlockBgpPipeline) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  EXPECT_FALSE(syncer_->isIncrementalPublicationReady());
  bringUpPeers();

  addRoute("v4", "10.13.0.0", 16, kPeerAddr3, "11.0.0.13", "65013");
  assertBgpPipelineProgress("10.13.0.0", 16, folly::IPAddress("11.0.0.13"));

  fsdbServer_ = std::make_unique<FsdbTestServer>(fsdbPort_);
  WITH_RETRIES_N_TIMED(100, std::chrono::milliseconds(100), {
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE((*state)->rib_entries()->contains("10.13.0.0/16"));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.13.0.0/16", folly::IPAddress("11.0.0.13")));
  });
}

TEST_F(E2ECanonicalRibFsdbTest, StalledFsdbSyncerDoesNotBlockBgpPipeline) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(syncer_->isIncrementalPublicationReady());
  });

  constexpr auto kWithdrawnPrefix = "10.14.254.0";
  constexpr auto kWithdrawnPrefixWithLength = "10.14.254.0/24";
  addRoute("v4", kWithdrawnPrefix, 24, kPeerAddr3, "11.0.0.14", "65014");
  assertBgpPipelineProgress(
      kWithdrawnPrefix, 24, folly::IPAddress("11.0.0.14"));
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(
        (*state)->rib_entries()->contains(kWithdrawnPrefixWithLength));
  });

  ScopedEventBaseBlocker syncerBlocker(
      *fsdbSyncerEventBaseThread_.getEventBase());
  ASSERT_NO_THROW(
      syncerBlocker.waitUntilBlocked("FsdbSyncer EventBase to become blocked"));

  deleteRoute("v4", kWithdrawnPrefix, 24, kPeerAddr3);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib(kWithdrawnPrefixWithLength));
  WITH_RETRIES({
    EXPECT_EVENTUALLY_EQ(
        nullptr, getFibWeightedNexthops(kWithdrawnPrefixWithLength));
  });
  ASSERT_TRUE(waitForPeerEgressQueueNonEmpty(kPeerAddr5));
  EXPECT_TRUE(verifyRouteWithdraw("v4", kWithdrawnPrefix, 24, kPeerAddr5));

  constexpr auto kReplacedPrefix = "10.14.253.0";
  constexpr auto kReplacedPrefixWithLength = "10.14.253.0/24";
  addRoute("v4", kReplacedPrefix, 24, kPeerAddr3, "11.0.0.14", "65014");
  assertBgpPipelineProgress(kReplacedPrefix, 24, folly::IPAddress("11.0.0.14"));
  addRoute("v4", kReplacedPrefix, 24, kPeerAddr3, "11.0.0.15", "65015");
  assertBgpPipelineProgress(kReplacedPrefix, 24, folly::IPAddress("11.0.0.15"));

  constexpr uint8_t kLastRouteOctet = 63;
  for (uint8_t octet = 0; octet <= kLastRouteOctet; ++octet) {
    addRoute(
        "v4",
        "10.14." + std::to_string(octet) + ".0",
        24,
        kPeerAddr3,
        "11.0.0.14",
        "65014");
  }
  const auto lastPrefix = "10.14." + std::to_string(kLastRouteOctet) + ".0";
  const auto lastPrefixWithLength = lastPrefix + "/24";
  assertBgpPipelineProgress(lastPrefix, 24, folly::IPAddress("11.0.0.14"));
  ASSERT_FALSE(syncerBlocker.timedOut())
      << "FsdbSyncer EventBase blocker timed out before stalled-state checks";
  {
    auto state = subscribed.rlock();
    ASSERT_TRUE(state->has_value());
    EXPECT_TRUE((*state)->rib_entries()->contains(kWithdrawnPrefixWithLength));
    EXPECT_FALSE((*state)->rib_entries()->contains(kReplacedPrefixWithLength));
    EXPECT_FALSE((*state)->rib_entries()->contains(lastPrefixWithLength));
  }

  syncerBlocker.release();
  EXPECT_FALSE(syncerBlocker.timedOut());

  WITH_RETRIES_N_TIMED(100, std::chrono::milliseconds(100), {
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_FALSE(
        (*state)->rib_entries()->contains(kWithdrawnPrefixWithLength));
    EXPECT_EVENTUALLY_TRUE(hasNexthop(
        **state, kReplacedPrefixWithLength, folly::IPAddress("11.0.0.15")));
    EXPECT_EVENTUALLY_FALSE(hasNexthop(
        **state, kReplacedPrefixWithLength, folly::IPAddress("11.0.0.14")));
    EXPECT_EVENTUALLY_TRUE(
        (*state)->rib_entries()->contains(lastPrefixWithLength));
    EXPECT_EVENTUALLY_TRUE(hasNexthop(
        **state, lastPrefixWithLength, folly::IPAddress("11.0.0.14")));
  });
}

TEST_F(
    E2ECanonicalRibFsdbTest,
    StalledFsdbPublisherStreamDoesNotBlockBgpPipeline) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(syncer_->isIncrementalPublicationReady());
  });

  auto* publisherStreamEventBase = fsdbPublisherStreamEventBase();
  ASSERT_NE(nullptr, publisherStreamEventBase);
  ScopedEventBaseBlocker publisherStreamBlocker(*publisherStreamEventBase);
  ASSERT_NO_THROW(publisherStreamBlocker.waitUntilBlocked(
      "FSDB publisher stream EventBase to become blocked"));

  addRoute("v4", "10.15.0.0", 16, kPeerAddr3, "11.0.0.15", "65015");
  assertBgpPipelineProgress("10.15.0.0", 16, folly::IPAddress("11.0.0.15"));
  ASSERT_FALSE(publisherStreamBlocker.timedOut())
      << "publisher EventBase blocker timed out before queue checks";
  WITH_RETRIES({
    const auto queueSize = fsdbPublisherQueueSize();
    EXPECT_EVENTUALLY_TRUE(queueSize.has_value());
    EXPECT_EVENTUALLY_GT(queueSize.value_or(0), 0);
  });
  ASSERT_FALSE(publisherStreamBlocker.timedOut())
      << "publisher EventBase blocker timed out before stalled-state checks";
  {
    auto state = subscribed.rlock();
    ASSERT_TRUE(state->has_value());
    EXPECT_FALSE((*state)->rib_entries()->contains("10.15.0.0/16"));
  }

  publisherStreamBlocker.release();
  EXPECT_FALSE(publisherStreamBlocker.timedOut());
  WITH_RETRIES_N_TIMED(100, std::chrono::milliseconds(100), {
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE((*state)->rib_entries()->contains("10.15.0.0/16"));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.15.0.0/16", folly::IPAddress("11.0.0.15")));
  });
}

TEST_F(E2ECanonicalRibFsdbTest, SubscriberTracksUpdatesAndGracefulRestart) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());

  bringUpPeers();
  BgpPeerId peer3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE((*state)->rib_entries()->contains("10.0.0.0/8"));
  });

  bringDownPeerWithGr(kPeerAddr3);

  WITH_RETRIES_N_TIMED(50, std::chrono::milliseconds(100), {
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_FALSE((*state)->rib_entries()->contains("10.0.0.0/8"));
  });

  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peer3);
  ASSERT_TRUE(waitForEoR(peer3));
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65002");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE((*state)->rib_entries()->contains("10.0.0.0/8"));
  });
}

TEST_F(E2ECanonicalRibFsdbTest, LateSubscriberReceivesCurrentSnapshot) {
  bringUpPeers();
  addRoute("v4", "10.1.0.0", 16, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v6", "2401:db00:1::", 48, kPeerAddr5, "2401:db00::1", "65002");
  ASSERT_TRUE(waitForPathCountInRib("10.1.0.0/16", 1));
  ASSERT_TRUE(waitForPathCountInRib("2401:db00:1::/48", 1));

  auto lateSubscriber =
      std::make_unique<FsdbTestSubscriber>("late-rib-e2e-subscriber");
  auto subscribed = lateSubscriber->subscribe(
      lateSubscriber->getRootStatePath().bgp().canonicalRib());
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE((*state)->rib_entries()->contains("10.1.0.0/16"));
    EXPECT_EVENTUALLY_TRUE(
        (*state)->rib_entries()->contains("2401:db00:1::/48"));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.1.0.0/16", folly::IPAddress("11.0.0.1")));
    EXPECT_EVENTUALLY_TRUE(hasNexthop(
        **state, "2401:db00:1::/48", folly::IPAddress("2401:db00::1")));
    EXPECT_EVENTUALLY_TRUE(canonicalReferencesResolve(**state));
  });
}

TEST_F(E2ECanonicalRibFsdbTest, ReplacesPathAndWithdrawsPrefix) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();

  addRoute("v4", "10.2.0.0", 16, kPeerAddr3, "11.0.0.1", "65001");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.2.0.0/16", folly::IPAddress("11.0.0.1")));
  });

  addRoute("v4", "10.2.0.0", 16, kPeerAddr3, "11.0.0.9", "65009");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.2.0.0/16", folly::IPAddress("11.0.0.9")));
    EXPECT_EVENTUALLY_FALSE(
        hasNexthop(**state, "10.2.0.0/16", folly::IPAddress("11.0.0.1")));
  });

  deleteRoute("v4", "10.2.0.0", 16, kPeerAddr3);
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_FALSE((*state)->rib_entries()->contains("10.2.0.0/16"));
  });
}

TEST_F(E2ECanonicalRibFsdbTest, CommunityOnlyChangeGetsNewIndex) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();

  addRoute("v4", "10.12.0.0", 16, kPeerAddr3, "11.0.0.1", "65001", "65000:1");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(hasCommunity(**state, "10.12.0.0/16", 65000, 1));
  });

  std::optional<int64_t> liveCommunityIdx;
  {
    auto state = subscribed.rlock();
    ASSERT_TRUE(state->has_value());
    liveCommunityIdx = communityIndexForPrefix(**state, "10.12.0.0/16");
    ASSERT_TRUE(liveCommunityIdx.has_value());
  }

  addRoute("v4", "10.12.0.0", 16, kPeerAddr3, "11.0.0.1", "65001", "65446:10");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(hasCommunity(**state, "10.12.0.0/16", 65446, 10));
    EXPECT_EVENTUALLY_FALSE(hasCommunity(**state, "10.12.0.0/16", 65000, 1));
    const auto drainCommunityIdx =
        communityIndexForPrefix(**state, "10.12.0.0/16");
    ASSERT_EVENTUALLY_TRUE(drainCommunityIdx.has_value());
    EXPECT_EVENTUALLY_NE(*liveCommunityIdx, *drainCommunityIdx);
    EXPECT_EVENTUALLY_TRUE(canonicalReferencesResolve(**state));
  });
}

TEST_F(E2ECanonicalRibFsdbTest, ReconnectReconcilesAllOfflineMutations) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();
  addRoute("v4", "10.3.0.0", 16, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.4.0.0", 16, kPeerAddr3, "11.0.0.2", "65002");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_EQ(2, (*state)->rib_entries()->size());
  });

  const auto fsdbPort = fsdbServer_->getFsdbPort();
  fsdbServer_.reset();
  deleteRoute("v4", "10.3.0.0", 16, kPeerAddr3);
  addRoute("v4", "10.4.0.0", 16, kPeerAddr3, "11.0.0.8", "65008");
  addRoute("v4", "10.5.0.0", 16, kPeerAddr5, "11.0.0.5", "65005");
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.3.0.0/16"));
  ASSERT_TRUE(waitForPathCountInRib("10.5.0.0/16", 1));

  fsdbServer_ = std::make_unique<FsdbTestServer>(fsdbPort);
  addRoute("v4", "10.6.0.0", 16, kPeerAddr5, "11.0.0.6", "65006");
  WITH_RETRIES_N_TIMED(100, std::chrono::milliseconds(100), {
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_EQ(3, (*state)->rib_entries()->size());
    EXPECT_EVENTUALLY_FALSE((*state)->rib_entries()->contains("10.3.0.0/16"));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.4.0.0/16", folly::IPAddress("11.0.0.8")));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.5.0.0/16", folly::IPAddress("11.0.0.5")));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.6.0.0/16", folly::IPAddress("11.0.0.6")));
    EXPECT_EVENTUALLY_TRUE(canonicalReferencesResolve(**state));
  });
}

TEST_F(
    E2ECanonicalMultipathRibFsdbTest,
    MultipathMembershipSurvivesLegWithdrawal) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();
  addRoute("v4", "10.7.0.0", 16, kPeerAddr3, "11.0.0.3", "65001");
  addRoute("v4", "10.7.0.0", 16, kPeerAddr5, "11.0.0.5", "65001");
  ASSERT_TRUE(waitForMultipathNexthopCount("10.7.0.0/16", 2));

  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    ASSERT_EVENTUALLY_TRUE((*state)->rib_entries()->contains("10.7.0.0/16"));
    EXPECT_EVENTUALLY_EQ(2, pathsForPrefix(**state, "10.7.0.0/16").size());
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.7.0.0/16", folly::IPAddress("11.0.0.3")));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.7.0.0/16", folly::IPAddress("11.0.0.5")));
  });

  deleteRoute("v4", "10.7.0.0", 16, kPeerAddr3);
  WITH_RETRIES(
      { EXPECT_EVENTUALLY_EQ(1, getMultipathNexthopCount("10.7.0.0/16")); });
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_EQ(1, pathsForPrefix(**state, "10.7.0.0/16").size());
    EXPECT_EVENTUALLY_FALSE(
        hasNexthop(**state, "10.7.0.0/16", folly::IPAddress("11.0.0.3")));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.7.0.0/16", folly::IPAddress("11.0.0.5")));
    EXPECT_EVENTUALLY_TRUE(canonicalReferencesResolve(**state));
  });
}

TEST_F(
    E2ECanonicalLongGrRibFsdbTest,
    GracefulRestartRecoversBeforeStaleExpiry) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();
  BgpPeerId peer3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};

  addRoute("v4", "10.8.0.0", 16, kPeerAddr3, "11.0.0.1", "65001");
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.8.0.0/16", folly::IPAddress("11.0.0.1")));
  });

  bringDownPeerWithGr(kPeerAddr3);
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.8.0.0/16", folly::IPAddress("11.0.0.1")));
  });

  bringUpPeer(kPeerAddr3);
  addRoute("v4", "10.8.0.0", 16, kPeerAddr3, "11.0.0.8", "65008");
  sendEoRToPeer(peer3);
  ASSERT_TRUE(waitForEoR(peer3));

  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.8.0.0/16", folly::IPAddress("11.0.0.8")));
    EXPECT_EVENTUALLY_FALSE(
        hasNexthop(**state, "10.8.0.0/16", folly::IPAddress("11.0.0.1")));
    EXPECT_EVENTUALLY_TRUE(canonicalReferencesResolve(**state));
  });
}

TEST_F(
    E2ECanonicalMultipathRibFsdbTest,
    MultipathReconnectPublishesCurrentMembership) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();
  addRoute("v4", "10.9.0.0", 16, kPeerAddr3, "11.0.0.3", "65001");
  addRoute("v4", "10.9.0.0", 16, kPeerAddr5, "11.0.0.5", "65001");
  ASSERT_TRUE(waitForMultipathNexthopCount("10.9.0.0/16", 2));

  const auto fsdbPort = fsdbServer_->getFsdbPort();
  fsdbServer_.reset();
  deleteRoute("v4", "10.9.0.0", 16, kPeerAddr3);
  WITH_RETRIES(
      { EXPECT_EVENTUALLY_EQ(1, getMultipathNexthopCount("10.9.0.0/16")); });
  fsdbServer_ = std::make_unique<FsdbTestServer>(fsdbPort);

  WITH_RETRIES_N_TIMED(100, std::chrono::milliseconds(100), {
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_EQ(1, pathsForPrefix(**state, "10.9.0.0/16").size());
    EXPECT_EVENTUALLY_FALSE(
        hasNexthop(**state, "10.9.0.0/16", folly::IPAddress("11.0.0.3")));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.9.0.0/16", folly::IPAddress("11.0.0.5")));
    EXPECT_EVENTUALLY_TRUE(canonicalReferencesResolve(**state));
  });
}

TEST_F(E2ECanonicalRibFsdbTest, BestPathFailoverUpdatesSubscriber) {
  auto subscribed = subscriber_->subscribe(
      subscriber_->getRootStatePath().bgp().canonicalRib());
  bringUpPeers();
  addRoute("v4", "10.10.0.0", 16, kPeerAddr3, "11.0.0.3", "65001");
  addRoute("v4", "10.10.0.0", 16, kPeerAddr5, "11.0.0.5", "65002 65003");
  ASSERT_TRUE(waitForPathCountInRib("10.10.0.0/16", 2));

  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.10.0.0/16", folly::IPAddress("11.0.0.3")));
  });

  deleteRoute("v4", "10.10.0.0", 16, kPeerAddr3);
  WITH_RETRIES({
    auto state = subscribed.rlock();
    ASSERT_EVENTUALLY_TRUE(state->has_value());
    EXPECT_EVENTUALLY_EQ(1, pathsForPrefix(**state, "10.10.0.0/16").size());
    EXPECT_EVENTUALLY_FALSE(
        hasNexthop(**state, "10.10.0.0/16", folly::IPAddress("11.0.0.3")));
    EXPECT_EVENTUALLY_TRUE(
        hasNexthop(**state, "10.10.0.0/16", folly::IPAddress("11.0.0.5")));
    EXPECT_EVENTUALLY_TRUE(canonicalReferencesResolve(**state));
  });
}

} // namespace facebook::bgp
