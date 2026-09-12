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

class FsdbSyncerTests;

#define FSDB_SYNCER_TEST_FRIENDS friend class ::FsdbSyncerTests;
#include "neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.h"
#undef FSDB_SYNCER_TEST_FRIENDS

#include <fb303/ThreadCachedServiceData.h>
#include <fboss/fsdb/if/gen-cpp2/fsdb_common_types.h>
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "fboss/fsdb/client/FsdbPubSubManager.h"
#include "fboss/fsdb/tests/utils/FsdbTestServer.h"
#include "fboss/fsdb/tests/utils/FsdbTestSubscriber.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

#include <fmt/format.h>
#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/synchronization/Baton.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <atomic>
#include <map>
#include <thread>

using namespace facebook::bgp;
namespace bgp_thrift = facebook::neteng::fboss::bgp::thrift;
namespace bgp_attr = facebook::neteng::fboss::bgp_attr;

using facebook::fboss::fsdb::PublisherId;
using facebook::fboss::fsdb::PublisherIds;
using facebook::fboss::fsdb::test::FsdbTestServer;
using facebook::fboss::fsdb::test::FsdbTestSubscriber;

namespace {
int64_t sumCounter(std::string_view name) {
  auto* stats = facebook::fb303::ThreadCachedServiceData::get();
  stats->publishStats();
  const auto key = fmt::format("{}.sum", name);
  return stats->hasCounter(key) ? stats->getCounter(key) : 0;
}

std::string subtreeCounter(std::string_view subtree, std::string_view event) {
  return fmt::format("bgpcpp.fsdbSyncer.{}.{}", subtree, event);
}

std::string lifecycleCounter(std::string_view event) {
  return fmt::format("bgpcpp.fsdbSyncer.{}", event);
}

bgp_thrift::TPartialDrainState partialDrainState(bool isPartiallyDrained) {
  bgp_thrift::TPartialDrainState state;
  state.partial_drain_state()->is_partially_drained() = isPartiallyDrained;
  state.partial_drain_state()->num_affected_prefixes() = 0;
  return state;
}
} // namespace

class FsdbSyncerTests : public ::testing::Test {
 public:
  void SetUp() override {
    fsdbTestServer_ = std::make_unique<FsdbTestServer>();
    FLAGS_fsdbPort = fsdbTestServer_->getFsdbPort();
    FLAGS_publish_state_to_fsdb = true;
    FLAGS_publish_stats_to_fsdb = true;
    subscriber_ = std::make_unique<FsdbTestSubscriber>("test-subscriber");
    fsdbSyncer_ = std::make_unique<FsdbSyncer>(*syncerThread_.getEventBase());
  }

  void TearDown() override {
    subscriber_.reset();
    fsdbSyncer_.reset();
  }

  void simulateDisconnected() {
    fsdbSyncer_->onPublisherStateChanged(
        facebook::fboss::fsdb::FsdbStreamClient::State::CONNECTED,
        facebook::fboss::fsdb::FsdbStreamClient::State::DISCONNECTED);
  }

  uint64_t prepareInitialSnapshotSubmission() {
    auto publication = fsdbSyncer_->publicationState_.wlock();
    publication->connected = true;
    publication->initialSnapshotSubmitted = false;
    return ++publication->generation;
  }

  uint64_t publisherStateGeneration() const {
    return fsdbSyncer_->publicationState_.rlock()->generation;
  }

  bool trySubmitPatchForGeneration(
      FsdbSyncer::PublicationKind kind,
      uint64_t expectedGeneration) {
    bool submitted{true};
    fsdbSyncer_->syncerEventBase_.runInEventBaseThreadAndWait(
        [this, kind, expectedGeneration, &submitted]() {
          submitted = fsdbSyncer_->trySubmitPatch(
              facebook::fboss::fsdb::Patch{}, kind, expectedGeneration);
        });
    return submitted;
  }

  bool trySubmitInitialSnapshotForGeneration(uint64_t expectedGeneration) {
    return trySubmitPatchForGeneration(
        FsdbSyncer::PublicationKind::INITIAL_SNAPSHOT, expectedGeneration);
  }

  bool trySubmitIncrementalForGeneration(uint64_t expectedGeneration) {
    return trySubmitPatchForGeneration(
        FsdbSyncer::PublicationKind::INCREMENTAL, expectedGeneration);
  }

  bool publisherCreated(const FsdbSyncer& syncer) const {
    return syncer.publisherCreated_;
  }

  bool hasPubSubManager(const FsdbSyncer& syncer) const {
    return syncer.fsdbPubSubMgr_ != nullptr;
  }

  bool stopping(const FsdbSyncer& syncer) const {
    return syncer.stopping_.load(std::memory_order_acquire);
  }

  int32_t retainedHoldTime(const FsdbSyncer& syncer) const {
    return *syncer.retained_.config.hold_time();
  }

  void blockSyncerThread(
      folly::Baton<>& syncerThreadBlocked,
      folly::Baton<>& releaseSyncerThread) {
    fsdbSyncer_->syncerEventBase_.runInEventBaseThread(
        [&syncerThreadBlocked, &releaseSyncerThread]() {
          syncerThreadBlocked.post();
          releaseSyncerThread.wait();
        });
  }

  folly::ScopedEventBaseThread syncerThread_{"FsdbSyncerTests"};
  std::unique_ptr<FsdbSyncer> fsdbSyncer_;
  std::unique_ptr<FsdbTestServer> fsdbTestServer_;
  std::unique_ptr<FsdbTestSubscriber> subscriber_;
  std::vector<std::vector<std::string>> subscriptions_;
};

TEST_F(FsdbSyncerTests, testConnection) {
  EXPECT_FALSE(this->fsdbSyncer_->isIncrementalPublicationReady());
  this->fsdbSyncer_->start();
  auto bgpPubId = PublisherId("bgpd");
  PublisherIds pubIds = {bgpPubId};
  std::vector<std::string> bgpPubPath = {"bgp"};
  WITH_RETRIES({
    auto publisherToInfo = folly::coro::blockingWait(
        this->fsdbTestServer_->getClient()->co_getOperPublisherInfos(pubIds));
    ASSERT_EVENTUALLY_GT(publisherToInfo.count(bgpPubId), 0);
    auto publisherInfo = publisherToInfo.at(bgpPubId);
    ASSERT_EVENTUALLY_EQ(publisherInfo.size(), 1);
    EXPECT_EVENTUALLY_EQ(*publisherInfo[0].publisherId(), bgpPubId);
    EXPECT_EVENTUALLY_EQ(*publisherInfo[0].path()->raw(), bgpPubPath);
    EXPECT_EVENTUALLY_FALSE(*publisherInfo[0].isStats());
    EXPECT_EVENTUALLY_TRUE(this->fsdbSyncer_->isIncrementalPublicationReady());
  });
  this->fsdbSyncer_->stop();
  EXPECT_FALSE(this->fsdbSyncer_->isIncrementalPublicationReady());
}

TEST_F(FsdbSyncerTests, PublishingDisabledByFlag) {
  FLAGS_publish_state_to_fsdb = false;
  SCOPE_EXIT {
    FLAGS_publish_state_to_fsdb = true;
  };

  this->fsdbSyncer_->start();
  EXPECT_FALSE(this->publisherCreated(*this->fsdbSyncer_));
  EXPECT_FALSE(this->fsdbSyncer_->isIncrementalPublicationReady());
  this->fsdbSyncer_->stop();
}

TEST_F(FsdbSyncerTests, LifecycleUsesCallerOwnedEventBase) {
  folly::ScopedEventBaseThread syncerThread;
  auto syncer = std::make_unique<FsdbSyncer>(*syncerThread.getEventBase());

  syncerThread.getEventBase()->runInEventBaseThreadAndWait(
      [&syncer]() { syncer->start(); });
  EXPECT_TRUE(publisherCreated(*syncer));

  syncer->stop();
  EXPECT_FALSE(publisherCreated(*syncer));
  EXPECT_FALSE(hasPubSubManager(*syncer));
  syncer.reset();

  bool callbackRan{false};
  syncerThread.getEventBase()->runInEventBaseThreadAndWait(
      [&callbackRan]() { callbackRan = true; });
  EXPECT_TRUE(callbackRan);
}

TEST_F(FsdbSyncerTests, StaleInitialSnapshotSubmissionIsRejected) {
  this->fsdbSyncer_->start();
  SCOPE_EXIT {
    this->fsdbSyncer_->stop();
  };
  WITH_RETRIES({
    EXPECT_EVENTUALLY_TRUE(this->fsdbSyncer_->isIncrementalPublicationReady());
  })

  const auto staleGeneration = this->prepareInitialSnapshotSubmission();
  const auto dropsBefore = sumCounter(lifecycleCounter("numStaleSnapshotDrop"));

  this->simulateDisconnected();
  const auto currentGeneration = this->prepareInitialSnapshotSubmission();
  ASSERT_GT(currentGeneration, staleGeneration);

  EXPECT_FALSE(this->trySubmitInitialSnapshotForGeneration(staleGeneration));
  EXPECT_FALSE(this->fsdbSyncer_->isIncrementalPublicationReady());
  EXPECT_EQ(
      dropsBefore + 1, sumCounter(lifecycleCounter("numStaleSnapshotDrop")));
}

TEST_F(FsdbSyncerTests, PublicationKindEnforcesInitialSnapshotOrdering) {
  this->fsdbSyncer_->start();
  SCOPE_EXIT {
    this->fsdbSyncer_->stop();
  };
  WITH_RETRIES({
    EXPECT_EVENTUALLY_TRUE(this->fsdbSyncer_->isIncrementalPublicationReady());
  })

  EXPECT_FALSE(this->trySubmitInitialSnapshotForGeneration(
      this->publisherStateGeneration()));

  const auto waitingForSnapshotGeneration =
      this->prepareInitialSnapshotSubmission();
  const auto rejectedIncrementalsBefore =
      sumCounter(lifecycleCounter("numRejectedIncremental"));
  EXPECT_FALSE(
      this->trySubmitIncrementalForGeneration(waitingForSnapshotGeneration));
  EXPECT_EQ(
      rejectedIncrementalsBefore + 1,
      sumCounter(lifecycleCounter("numRejectedIncremental")));
}

TEST_F(FsdbSyncerTests, StopIsIdempotent) {
  EXPECT_NO_THROW({
    this->fsdbSyncer_->stop();
    this->fsdbSyncer_->stop();
  });
}

TEST_F(FsdbSyncerTests, StopDrainsUpdatesQueuedBeforeProducerQuiescence) {
  this->fsdbSyncer_->start();
  WITH_RETRIES({
    EXPECT_EVENTUALLY_TRUE(this->fsdbSyncer_->isIncrementalPublicationReady());
  })

  folly::Baton<> syncerThreadBlocked;
  folly::Baton<> releaseSyncerThread;
  this->blockSyncerThread(syncerThreadBlocked, releaseSyncerThread);
  syncerThreadBlocked.wait();

  std::thread producer([this]() {
    for (int32_t holdTime = 1; holdTime <= 100; ++holdTime) {
      thrift::BgpConfig config;
      config.hold_time() = holdTime;
      this->fsdbSyncer_->setConfig(config);
    }
  });
  producer.join();

  std::atomic<bool> stopReturned{false};
  std::thread stopThread([this, &stopReturned]() {
    this->fsdbSyncer_->stop();
    stopReturned.store(true, std::memory_order_release);
  });
  WITH_RETRIES_N(
      100, { EXPECT_EVENTUALLY_TRUE(this->stopping(*this->fsdbSyncer_)); })
  EXPECT_FALSE(stopReturned.load(std::memory_order_acquire));
  releaseSyncerThread.post();
  stopThread.join();

  EXPECT_TRUE(stopReturned.load(std::memory_order_acquire));
  EXPECT_EQ(100, this->retainedHoldTime(*this->fsdbSyncer_));
  EXPECT_FALSE(this->hasPubSubManager(*this->fsdbSyncer_));
  this->fsdbSyncer_.reset();

  bool callbackRan{false};
  this->syncerThread_.getEventBase()->runInEventBaseThreadAndWait(
      [&callbackRan]() { callbackRan = true; });
  EXPECT_TRUE(callbackRan);
}

TEST_F(FsdbSyncerTests, testConfigPublish) {
  auto subscribedConfig = this->subscriber_->subscribe(
      this->subscriber_->getRootStatePath().bgp().config());

  auto config = thrift::BgpConfig();
  config.hold_time() = 123;
  config.ucmp_width() = 10000;
  config.defaultCommandLineArgs() = {{"foo", "1"}, {"bar", "2"}};

  this->fsdbSyncer_->setConfig(config);
  this->fsdbSyncer_->start();

  WITH_RETRIES({
    auto configLk = subscribedConfig.rlock();
    ASSERT_EVENTUALLY_TRUE(configLk->has_value());
    EXPECT_EVENTUALLY_EQ((*configLk)->hold_time(), 123);
    EXPECT_EVENTUALLY_EQ((*configLk)->ucmp_width(), 10000);
    EXPECT_EVENTUALLY_EQ((*configLk)->defaultCommandLineArgs()->size(), 2);
  })

  // set empty config
  this->fsdbSyncer_->setConfig(thrift::BgpConfig());
  WITH_RETRIES({
    auto configLk = subscribedConfig.rlock();
    ASSERT_EVENTUALLY_TRUE(configLk->has_value());
    EXPECT_EVENTUALLY_EQ((*configLk)->defaultCommandLineArgs()->size(), 0);
  })

  this->fsdbSyncer_->stop();
}

TEST_F(FsdbSyncerTests, FirstSnapshotContainsAllRetainedSubtrees) {
  using BgpUpdates =
      folly::Synchronized<std::vector<facebook::fboss::fsdb::BgpData>>;

  BgpUpdates updates;
  facebook::fboss::fsdb::FsdbPubSubManager subscriber("bgp-root-subscriber");
  subscriber.addStatePathSubscription(
      std::vector<std::string>{"bgp"},
      [](facebook::fboss::fsdb::SubscriptionState /*oldState*/,
         facebook::fboss::fsdb::SubscriptionState /*newState*/,
         std::optional<bool> /*initialSyncHasData*/) {},
      [&updates](facebook::fboss::fsdb::OperState state) {
        if (!state.contents()) {
          return;
        }
        updates.wlock()->push_back(
            apache::thrift::BinarySerializer::deserialize<
                facebook::fboss::fsdb::BgpData>(*state.contents()));
      });
  WITH_RETRIES({
    EXPECT_EVENTUALLY_FALSE(
        this->fsdbTestServer_->getActiveSubscriptions().empty());
  })

  thrift::BgpConfig config;
  config.hold_time() = 123;
  this->fsdbSyncer_->setConfig(config);
  this->fsdbSyncer_->setRouteAttributePolicy(
      rib_policy::TRouteAttributePolicy{});
  this->fsdbSyncer_->setPathSelectionPolicy(rib_policy::TPathSelectionPolicy{});
  this->fsdbSyncer_->setRouteFilterPolicy(rib_policy::TRouteFilterPolicy{});

  bgp_thrift::TPartialDrainState partialDrainState;
  partialDrainState.partial_drain_state()->is_partially_drained() = false;
  partialDrainState.partial_drain_state()->num_affected_prefixes() = 0;
  this->fsdbSyncer_->setPartialDrainState(std::move(partialDrainState));

  this->fsdbSyncer_->start();
  SCOPE_EXIT {
    this->fsdbSyncer_->stop();
  };

  WITH_RETRIES({
    auto updatesLocked = updates.rlock();
    ASSERT_EVENTUALLY_FALSE(updatesLocked->empty());
    const auto& firstUpdate = updatesLocked->front();
    EXPECT_EVENTUALLY_EQ(123, *firstUpdate.config()->hold_time());
    EXPECT_EVENTUALLY_TRUE(firstUpdate.routeAttributePolicy().has_value());
    EXPECT_EVENTUALLY_TRUE(firstUpdate.pathSelectionPolicy().has_value());
    EXPECT_EVENTUALLY_TRUE(firstUpdate.routeFilterPolicy().has_value());
    ASSERT_EVENTUALLY_TRUE(firstUpdate.partialDrainState().has_value());
    EXPECT_EVENTUALLY_FALSE(*firstUpdate.partialDrainState()
                                 ->partial_drain_state()
                                 ->is_partially_drained());
  })
}

TEST_F(FsdbSyncerTests, ReconnectPublishesLatestRetainedSnapshot) {
  auto subscribedConfig = this->subscriber_->subscribe(
      this->subscriber_->getRootStatePath().bgp().config());
  thrift::BgpConfig config;
  config.hold_time() = 111;
  this->fsdbSyncer_->setConfig(config);
  this->fsdbSyncer_->start();
  SCOPE_EXIT {
    this->fsdbSyncer_->stop();
  };

  WITH_RETRIES({
    auto configLocked = subscribedConfig.rlock();
    ASSERT_EVENTUALLY_TRUE(configLocked->has_value());
    EXPECT_EVENTUALLY_EQ(111, *(*configLocked)->hold_time());
  })

  const auto fsdbPort = this->fsdbTestServer_->getFsdbPort();
  this->fsdbTestServer_.reset();
  WITH_RETRIES({
    EXPECT_EVENTUALLY_FALSE(this->fsdbSyncer_->isIncrementalPublicationReady());
  })

  config.hold_time() = 222;
  this->fsdbSyncer_->setConfig(config);
  this->fsdbTestServer_ = std::make_unique<FsdbTestServer>(fsdbPort);

  WITH_RETRIES({
    auto configLocked = subscribedConfig.rlock();
    ASSERT_EVENTUALLY_TRUE(configLocked->has_value());
    EXPECT_EVENTUALLY_EQ(222, *(*configLocked)->hold_time());
    EXPECT_EVENTUALLY_TRUE(this->fsdbSyncer_->isIncrementalPublicationReady());
  })
}

TEST_F(FsdbSyncerTests, SyncerThreadPublishesRetainedSnapshotAndUpdates) {
  const auto configUpdatesBefore =
      sumCounter(subtreeCounter("config", "numUpdate"));
  const auto configIncrementalsBefore =
      sumCounter(subtreeCounter("config", "numIncrementalPublish"));
  const auto policyUpdatesBefore =
      sumCounter(subtreeCounter("routeAttributePolicy", "numUpdate"));
  const auto policyClearsBefore =
      sumCounter(subtreeCounter("routeAttributePolicy", "numClear"));
  const auto policyIncrementalsBefore = sumCounter(
      subtreeCounter("routeAttributePolicy", "numIncrementalPublish"));
  const std::vector<std::string_view> otherOptionalSubtrees = {
      "pathSelectionPolicy", "routeFilterPolicy", "partialDrainState"};
  std::map<std::string_view, int64_t> optionalUpdatesBefore;
  std::map<std::string_view, int64_t> optionalClearsBefore;
  std::map<std::string_view, int64_t> optionalIncrementalsBefore;
  for (const auto subtree : otherOptionalSubtrees) {
    optionalUpdatesBefore[subtree] =
        sumCounter(subtreeCounter(subtree, "numUpdate"));
    optionalClearsBefore[subtree] =
        sumCounter(subtreeCounter(subtree, "numClear"));
    optionalIncrementalsBefore[subtree] =
        sumCounter(subtreeCounter(subtree, "numIncrementalPublish"));
  }
  const auto snapshotsBefore =
      sumCounter(lifecycleCounter("numSnapshotPublish"));
  const auto incrementalPatchesBefore =
      sumCounter(lifecycleCounter("numIncrementalPublish"));
  folly::Synchronized<std::vector<facebook::fboss::fsdb::BgpData>> updates;
  facebook::fboss::fsdb::FsdbPubSubManager subscriber(
      "retained-state-subscriber");
  subscriber.addStatePathSubscription(
      std::vector<std::string>{"bgp"},
      [](facebook::fboss::fsdb::SubscriptionState /*oldState*/,
         facebook::fboss::fsdb::SubscriptionState /*newState*/,
         std::optional<bool> /*initialSyncHasData*/) {},
      [&updates](facebook::fboss::fsdb::OperState state) {
        if (state.contents()) {
          updates.wlock()->push_back(
              apache::thrift::BinarySerializer::deserialize<
                  facebook::fboss::fsdb::BgpData>(*state.contents()));
        }
      });
  WITH_RETRIES({
    EXPECT_EVENTUALLY_FALSE(
        this->fsdbTestServer_->getActiveSubscriptions().empty());
  })

  thrift::BgpConfig config;
  config.hold_time() = 111;
  this->fsdbSyncer_->setConfig(config);
  this->fsdbSyncer_->setRouteAttributePolicy(
      rib_policy::TRouteAttributePolicy{});
  this->fsdbSyncer_->setPathSelectionPolicy(rib_policy::TPathSelectionPolicy{});
  this->fsdbSyncer_->setRouteFilterPolicy(rib_policy::TRouteFilterPolicy{});
  this->fsdbSyncer_->setPartialDrainState(partialDrainState(false));
  this->fsdbSyncer_->start();
  SCOPE_EXIT {
    this->fsdbSyncer_->stop();
  };

  WITH_RETRIES({
    auto updatesLocked = updates.rlock();
    ASSERT_EVENTUALLY_FALSE(updatesLocked->empty());
    const auto& firstUpdate = updatesLocked->front();
    EXPECT_EVENTUALLY_EQ(111, *firstUpdate.config()->hold_time());
    EXPECT_EVENTUALLY_TRUE(firstUpdate.routeAttributePolicy().has_value());
    EXPECT_EVENTUALLY_TRUE(firstUpdate.pathSelectionPolicy().has_value());
    EXPECT_EVENTUALLY_TRUE(firstUpdate.routeFilterPolicy().has_value());
    EXPECT_EVENTUALLY_TRUE(firstUpdate.partialDrainState().has_value());
  })
  updates.wlock()->clear();

  config.hold_time() = 222;
  this->fsdbSyncer_->setConfig(config);
  this->fsdbSyncer_->setRouteAttributePolicy(std::nullopt);
  this->fsdbSyncer_->setPathSelectionPolicy(std::nullopt);
  this->fsdbSyncer_->setRouteFilterPolicy(std::nullopt);
  this->fsdbSyncer_->setPartialDrainState(std::nullopt);
  WITH_RETRIES({
    auto updatesLocked = updates.rlock();
    ASSERT_EVENTUALLY_FALSE(updatesLocked->empty());
    EXPECT_EVENTUALLY_EQ(222, *updatesLocked->back().config()->hold_time());
    EXPECT_EVENTUALLY_FALSE(
        updatesLocked->back().routeAttributePolicy().has_value());
    EXPECT_EVENTUALLY_FALSE(
        updatesLocked->back().pathSelectionPolicy().has_value());
    EXPECT_EVENTUALLY_FALSE(
        updatesLocked->back().routeFilterPolicy().has_value());
    EXPECT_EVENTUALLY_FALSE(
        updatesLocked->back().partialDrainState().has_value());
  })
  EXPECT_EQ(
      configUpdatesBefore + 2,
      sumCounter(subtreeCounter("config", "numUpdate")));
  EXPECT_EQ(
      configIncrementalsBefore + 1,
      sumCounter(subtreeCounter("config", "numIncrementalPublish")));
  EXPECT_EQ(
      policyUpdatesBefore + 2,
      sumCounter(subtreeCounter("routeAttributePolicy", "numUpdate")));
  EXPECT_EQ(
      policyClearsBefore + 1,
      sumCounter(subtreeCounter("routeAttributePolicy", "numClear")));
  EXPECT_EQ(
      policyIncrementalsBefore + 1,
      sumCounter(
          subtreeCounter("routeAttributePolicy", "numIncrementalPublish")));
  for (const auto subtree : otherOptionalSubtrees) {
    EXPECT_EQ(
        optionalUpdatesBefore.at(subtree) + 2,
        sumCounter(subtreeCounter(subtree, "numUpdate")));
    EXPECT_EQ(
        optionalClearsBefore.at(subtree) + 1,
        sumCounter(subtreeCounter(subtree, "numClear")));
    EXPECT_EQ(
        optionalIncrementalsBefore.at(subtree) + 1,
        sumCounter(subtreeCounter(subtree, "numIncrementalPublish")));
  }
  EXPECT_EQ(
      snapshotsBefore + 1, sumCounter(lifecycleCounter("numSnapshotPublish")));
  EXPECT_EQ(
      incrementalPatchesBefore + 5,
      sumCounter(lifecycleCounter("numIncrementalPublish")));
}

TEST_F(FsdbSyncerTests, testPartialDrainStatePublish) {
  auto subscribedState = this->subscriber_->subscribe(
      this->subscriber_->getRootStatePath().bgp().partialDrainState());

  this->fsdbSyncer_->start();
  /*
   * partialDrainState is an `optional` field on BgpData; after a delete patch,
   * the subscriber observes !has_value(). If an assertion below escapes before
   * stop() runs, the SCOPE_EXIT still closes the publisher before fixture
   * teardown.
   */
  SCOPE_EXIT {
    this->fsdbSyncer_->stop();
  };

  /*
   * Publish a non-empty TPartialDrainState mirroring what
   * Rib::prepareFibProgramming builds when a prefix enters partial drain:
   * device-summary populated, one drained prefix entry attached.
   */
  bgp_thrift::TPartialDrainState state;
  state.partial_drain_state()->is_partially_drained() = true;
  state.partial_drain_state()->num_affected_prefixes() = 1;
  state.partial_drain_state()->partial_drain_transition_count() = 1;

  bgp_thrift::TPartiallyDrainedPrefix drainedPrefix;
  drainedPrefix.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  drainedPrefix.prefix()->num_bits() = 24;
  drainedPrefix.prefix()->prefix_bin() = std::string("\x0a\x00\x00\x00", 4);
  drainedPrefix.min_capacity()->next_hop_count() = 3;
  drainedPrefix.current_capacity()->next_hop_count() = 1;
  state.drained_prefixes()->push_back(drainedPrefix);

  this->fsdbSyncer_->setPartialDrainState(std::make_optional(state));

  WITH_RETRIES({
    auto stateLk = subscribedState.rlock();
    ASSERT_EVENTUALLY_TRUE(stateLk->has_value());
    EXPECT_EVENTUALLY_TRUE(
        *(*stateLk)->partial_drain_state()->is_partially_drained());
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->num_affected_prefixes(), 1);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->partial_drain_transition_count(),
        1);
    ASSERT_EVENTUALLY_EQ((*stateLk)->drained_prefixes()->size(), 1);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .min_capacity()
             ->next_hop_count_ref(),
        3);
    /*
     * current_capacity carries the same criterion as min_capacity — the
     * next_hop_count arm (current nexthop count) for this MNH-triggered drain.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .current_capacity()
             ->next_hop_count_ref(),
        1);
  })

  /*
   * Now publish an updated state — prefix count goes to 2. Verifies
   * subsequent publishes overwrite the prior value. The 2nd prefix uses
   * the `agg_lbw_bps` variant of the TCapacity union (for both min_capacity
   * and current_capacity) instead of the `next_hop_count` arm, so this single
   * test exercises both union branches round-tripping through the FSDB
   * serialization layer — matching the two production branches in
   * RibDC::buildPartialDrainPrefixEntry (next_hop_count arm = threshold +
   * current count on MNH violation, agg_lbw_bps arm = threshold + current agg
   * LBW on LBW violation).
   */
  state.partial_drain_state()->num_affected_prefixes() = 2;
  bgp_thrift::TPartiallyDrainedPrefix drainedPrefix2;
  drainedPrefix2.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  drainedPrefix2.prefix()->num_bits() = 24;
  drainedPrefix2.prefix()->prefix_bin() = std::string("\x14\x00\x00\x00", 4);
  constexpr int64_t kAggLbwBpsThreshold = 100LL * 1'000'000'000LL; // 100 Gbps
  // Current aggregate LBW sits below the threshold (that is why it drained).
  constexpr int64_t kAggLbwBpsCurrent = 50LL * 1'000'000'000LL; // 50 Gbps
  drainedPrefix2.min_capacity()->agg_lbw_bps() = kAggLbwBpsThreshold;
  drainedPrefix2.current_capacity()->agg_lbw_bps() = kAggLbwBpsCurrent;
  state.drained_prefixes()->push_back(drainedPrefix2);

  this->fsdbSyncer_->setPartialDrainState(std::make_optional(state));

  WITH_RETRIES({
    auto stateLk = subscribedState.rlock();
    ASSERT_EVENTUALLY_TRUE(stateLk->has_value());
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->num_affected_prefixes(), 2);
    ASSERT_EVENTUALLY_EQ((*stateLk)->drained_prefixes()->size(), 2);
    // 1st prefix kept the next_hop_count variant from the prior publish.
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .min_capacity()
             ->next_hop_count_ref(),
        3);
    /*
     * 2nd prefix carries the LBW variant — proves the agg_lbw_bps union
     * arm survives FSDB serialization just like the next_hop_count arm above.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(1)
             .min_capacity()
             ->agg_lbw_bps_ref(),
        kAggLbwBpsThreshold);
    /*
     * current_capacity carries the current aggregate LBW on the matching
     * agg_lbw_bps arm and round-trips through FSDB like the threshold.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(1)
             .current_capacity()
             ->agg_lbw_bps_ref(),
        kAggLbwBpsCurrent);
  })

  /*
   * Clear the state with std::nullopt. FsdbSyncer submits a delete patch, so
   * the subscriber observes the optional field becoming unset. Rib uses this
   * path when drainedPrefixCount_ returns to zero.
   */
  this->fsdbSyncer_->setPartialDrainState(std::nullopt);

  WITH_RETRIES({
    auto stateLk = subscribedState.rlock();
    EXPECT_EVENTUALLY_FALSE(stateLk->has_value());
  })
}
