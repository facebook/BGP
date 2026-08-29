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

#include <fboss/fsdb/if/gen-cpp2/fsdb_common_types.h>
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "fboss/fsdb/client/FsdbPubSubManager.h"
#include "fboss/fsdb/tests/utils/FsdbTestServer.h"
#include "fboss/fsdb/tests/utils/FsdbTestSubscriber.h"
#include "neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

#include <folly/ScopeGuard.h>
#include <folly/Synchronized.h>
#include <folly/coro/BlockingWait.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

using namespace facebook::bgp;
namespace bgp_thrift = facebook::neteng::fboss::bgp::thrift;
namespace bgp_attr = facebook::neteng::fboss::bgp_attr;

using facebook::fboss::fsdb::PublisherId;
using facebook::fboss::fsdb::PublisherIds;
using facebook::fboss::fsdb::test::FsdbTestServer;
using facebook::fboss::fsdb::test::FsdbTestSubscriber;

template <bool EnableFsdbPatchAPI>
struct TestParams {
  static constexpr auto enableFsdbPatchApi = EnableFsdbPatchAPI;
};

using FsdbSyncerTestTypes =
    ::testing::Types<TestParams<false>, TestParams<true>>;

template <typename TestParams>
class FsdbSyncerTests : public ::testing::Test {
 public:
  void SetUp() override {
    fsdbTestServer_ = std::make_unique<FsdbTestServer>();
    FLAGS_fsdbPort = fsdbTestServer_->getFsdbPort();
    FLAGS_publish_state_to_fsdb = true;
    FLAGS_publish_stats_to_fsdb = true;
    subscriber_ = std::make_unique<FsdbTestSubscriber>("test-subscriber");
    FLAGS_fsdb_publish_state_with_patches = TestParams::enableFsdbPatchApi;
    fsdbSyncer_ = std::make_unique<FsdbSyncer>();
  }

  void TearDown() override {
    subscriber_.reset();
    fsdbSyncer_.reset();
  }

  std::unique_ptr<FsdbSyncer> fsdbSyncer_;
  std::unique_ptr<FsdbTestServer> fsdbTestServer_;
  std::unique_ptr<FsdbTestSubscriber> subscriber_;
  std::vector<std::vector<std::string>> subscriptions_;
};

TYPED_TEST_SUITE(FsdbSyncerTests, FsdbSyncerTestTypes);

TYPED_TEST(FsdbSyncerTests, testConnection) {
  this->fsdbSyncer_->start();
  auto bgpPubId = PublisherId("bgpd");
  PublisherIds pubIds = {bgpPubId};
  std::vector<std::string> bgpPubPath = {"bgp"};
  WITH_RETRIES({
    auto publisherToInfo = folly::coro::blockingWait(
        this->fsdbTestServer_->serviceHandler().co_getOperPublisherInfos(
            std::make_unique<PublisherIds>(pubIds)));
    ASSERT_EVENTUALLY_GT(publisherToInfo->count(bgpPubId), 0);
    auto publisherInfo = publisherToInfo->at(bgpPubId);
    ASSERT_EVENTUALLY_EQ(publisherInfo.size(), 1);
    EXPECT_EVENTUALLY_EQ(*publisherInfo[0].publisherId(), bgpPubId);
    EXPECT_EVENTUALLY_EQ(*publisherInfo[0].path()->raw(), bgpPubPath);
    EXPECT_EVENTUALLY_FALSE(*publisherInfo[0].isStats());
  });
  this->fsdbSyncer_->stop();
}

TYPED_TEST(FsdbSyncerTests, testConfigPublish) {
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

TYPED_TEST(FsdbSyncerTests, FirstSnapshotContainsAllRetainedSubtrees) {
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

TYPED_TEST(FsdbSyncerTests, testPartialDrainStatePublish) {
  auto subscribedState = this->subscriber_->subscribe(
      this->subscriber_->getRootStatePath().bgp().partialDrainState());

  this->fsdbSyncer_->start();
  /*
   * partialDrainState is an `optional` field on BgpData; when the publisher
   * sets it to nullptr, the subscriber observes !has_value(). If an
   * assertion below escapes before stop() runs, FsdbSyncManager's destructor
   * CHECKs that the syncer was stopped — the SCOPE_EXIT keeps TearDown safe
   * regardless of which assertion path the test takes.
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
    // current_capacity carries the same criterion as min_capacity — the
    // next_hop_count arm (current nexthop count) for this MNH-triggered drain.
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
    // 2nd prefix carries the LBW variant — proves the agg_lbw_bps union
    // arm survives FSDB serialization just like the next_hop_count arm above.
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(1)
             .min_capacity()
             ->agg_lbw_bps_ref(),
        kAggLbwBpsThreshold);
    // current_capacity carries the current aggregate LBW on the matching
    // agg_lbw_bps arm — round-trips through FSDB just like the threshold.
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(1)
             .current_capacity()
             ->agg_lbw_bps_ref(),
        kAggLbwBpsCurrent);
  })

  /*
   * Clear the state with std::nullopt — exercises the FsdbSyncer branch that
   * sets the FSDB ref to nullptr (used by Rib when drainedPrefixCount_ goes
   * back to 0). The subscriber observes the optional field becoming unset.
   */
  this->fsdbSyncer_->setPartialDrainState(std::nullopt);

  WITH_RETRIES({
    auto stateLk = subscribedState.rlock();
    EXPECT_EVENTUALLY_FALSE(stateLk->has_value());
  })
}
