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

#include <folly/ScopeGuard.h>
#include <folly/coro/BlockingWait.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define RibBase_TEST_FRIENDS friend class RibFsdbFixture;

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibFsdbPolicyTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"

using namespace facebook::bgp;
using namespace facebook::bgp::rib_policy;
using namespace facebook::neteng::fboss::bgp::thrift;
using namespace std::chrono;
using ::testing::_;

namespace facebook {
namespace bgp {

/*
 * End-to-end publish path with publish_partial_drain_state_to_fsdb enabled.
 * Drives the Rib through a partial-drain transition (entry then exit) and
 * verifies both edges publish a populated TPartialDrainState (never nullopt):
 * is_partially_drained=true on entry, =false on exit. Mirrors
 * PartialDrainStatusReflectsRibState (RibTest.cpp) but under RibFsdbFixture so
 * a real FSDB syncer is wired in.
 */
TEST_F(RibFsdbFixture, PartialDrainStatePublishedToFsdbOnTransition) {
  FLAGS_publish_partial_drain_state_to_fsdb = true;
  SCOPE_EXIT {
    FLAGS_publish_partial_drain_state_to_fsdb = false;
  };

  auto subscribedState = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().partialDrainState());

  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();
  rib_->setFibBatchTime(milliseconds(2));

  // Drain the initial-dump messages (RibInitialAnnouncementStart +
  // RibOutAnnouncement with initialDump=true).
  WITH_RETRIES({ ASSERT_EVENTUALLY_EQ(ribOutQ_.size(), 2); });
  REPEAT_N(2, folly::coro::blockingWait(ribOutQ_.pop()));

  // Install one path so any mnh > 1 policy violates → partial drain.
  auto prefixBatch = PrefixPathIds{{kV4Prefix1, kDefaultPathID}};
  fibFuture = fib_->getFibProgramFuture();
  sendAnnouncement(prefixBatch, eBgpPeer1_, attr_);
  fibFuture.wait();

  WITH_RETRIES({ ASSERT_EVENTUALLY_GE(ribOutQ_.size(), 1); });
  folly::coro::blockingWait(ribOutQ_.pop());

  // Inject CPS policy: mnh=3 + drain_on_min_nexthop_violation. The single
  // installed path violates mnh=3, so the prefix enters partial drain and
  // drainedPrefixCount_ flips 0 → 1 → setPartialDrainState(state) fires.
  TPathSelector tPathSelector;
  tPathSelector.bgp_native_path_selection_min_nexthop() = 3;
  tPathSelector.drain_on_min_nexthop_violation() = true;

  auto ribFuture = rib_->getRibPrepareFibProgrammingFuture();
  sendPathSelectionPolicySet(
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector));
  rib_->waitForPathSelectionPolicyUpdate();
  ribFuture.wait();

  WITH_RETRIES_N(5, {
    auto stateLk = subscribedState.rlock();
    ASSERT_EVENTUALLY_TRUE(stateLk->has_value());
    EXPECT_EVENTUALLY_TRUE(
        *(*stateLk)->partial_drain_state()->is_partially_drained());
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->num_affected_prefixes(), 1);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->partial_drain_transition_count(),
        1);
    /*
     * On a drain-status transition, RibDC::publishPartialDrainState builds
     * the drained-prefix snapshot on demand (getPartialDrainState scans
     * ribEntries_ for getIsPartialDrain() entries) and publishes it
     * alongside the device summary. One path was installed for kV4Prefix1
     * before the policy injection.
     *
     * min_capacity (TCapacity) carries the trigger criterion: this drain
     * was triggered by an MNH violation, so the union must hold
     * next_hop_count == 3 (the configured
     * bgp_native_path_selection_min_nexthop) and the agg_lbw_bps arm must NOT
     * be set. Pinned here so a future refactor of buildPartialDrainPrefixEntry
     * that mis-selects the union arm or forgets to plumb mnhThreshold_
     * end-to-end (RibPolicy → RibEntry → FSDB) fails this test.
     */
    ASSERT_EVENTUALLY_EQ((*stateLk)->drained_prefixes()->size(), 1);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->drained_prefixes()->at(0).min_capacity()->next_hop_count(),
        3);
    EXPECT_EVENTUALLY_FALSE((*stateLk)
                                ->drained_prefixes()
                                ->at(0)
                                .min_capacity()
                                ->agg_lbw_bps()
                                .has_value());
    /*
     * current_capacity mirrors the trigger criterion: MNH drain → the
     * next_hop_count arm holds the current nexthop count (1), and the
     * agg_lbw_bps arm is unset. Pins buildPartialDrainPrefixEntry's arm
     * selection for the current-value union end-to-end.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .current_capacity()
             ->next_hop_count(),
        1);
    EXPECT_EVENTUALLY_FALSE((*stateLk)
                                ->drained_prefixes()
                                ->at(0)
                                .current_capacity()
                                ->agg_lbw_bps()
                                .has_value());
  })

  // Withdraw the only path → drainedPrefixCount_ flips 1 → 0. The exit edge
  // publishes a populated is_partially_drained=false snapshot (not nullopt), so
  // the node stays present with an empty drained set and transition_count=2.
  fibFuture = fib_->getFibProgramFuture();
  sendWithdrawal(prefixBatch, eBgpPeer1_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto stateLk = subscribedState.rlock();
    ASSERT_EVENTUALLY_TRUE(stateLk->has_value());
    EXPECT_EVENTUALLY_FALSE(
        *(*stateLk)->partial_drain_state()->is_partially_drained());
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->num_affected_prefixes(), 0);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->partial_drain_transition_count(),
        2);
    EXPECT_EVENTUALLY_TRUE((*stateLk)->drained_prefixes()->empty());
  })
}

/*
 * Initial-publish path with no drain transition. With
 * publish_partial_drain_state_to_fsdb enabled, the first completed FIB pass
 * publishes the device partial-drain state even though no prefix ever drains,
 * so a never-drained device reports a populated is_partially_drained=false (not
 * nullopt) with transition_count=0 and an empty drained set. Guards the
 * single-gate behavior that replaced the separate post-start seed.
 */
TEST_F(RibFsdbFixture, PartialDrainStateInitialFalsePublishedWhenNeverDrained) {
  FLAGS_publish_partial_drain_state_to_fsdb = true;
  SCOPE_EXIT {
    FLAGS_publish_partial_drain_state_to_fsdb = false;
  };

  auto subscribedState = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().partialDrainState());

  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();
  rib_->setFibBatchTime(milliseconds(2));

  WITH_RETRIES({ ASSERT_EVENTUALLY_EQ(ribOutQ_.size(), 2); });
  REPEAT_N(2, folly::coro::blockingWait(ribOutQ_.pop()));

  // Install a path with no drain-triggering policy, so the device never enters
  // partial drain.
  auto prefixBatch = PrefixPathIds{{kV4Prefix1, kDefaultPathID}};
  fibFuture = fib_->getFibProgramFuture();
  sendAnnouncement(prefixBatch, eBgpPeer1_, attr_);
  fibFuture.wait();

  // The first completed FIB pass publishes a positive
  // is_partially_drained=false snapshot without any transition: node present,
  // no affected prefixes, empty drained set, and transition_count still 0 (the
  // initial publish does not bump the enter/exit counter).
  WITH_RETRIES_N(5, {
    auto stateLk = subscribedState.rlock();
    ASSERT_EVENTUALLY_TRUE(stateLk->has_value());
    EXPECT_EVENTUALLY_FALSE(
        *(*stateLk)->partial_drain_state()->is_partially_drained());
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->num_affected_prefixes(), 0);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->partial_drain_transition_count(),
        0);
    EXPECT_EVENTUALLY_TRUE((*stateLk)->drained_prefixes()->empty());
  })
}

/*
 * Disabled (default) path: with publish_partial_drain_state_to_fsdb off,
 * publishPartialDrainState is a no-op on both edges. Drives the same drain
 * entry/exit as PartialDrainStatePublishedToFsdbOnTransition and asserts the
 * FSDB node stays absent (!has_value()) throughout.
 */
TEST_F(RibFsdbFixture, PartialDrainStateNotPublishedWhenFlagDisabled) {
  ASSERT_FALSE(FLAGS_publish_partial_drain_state_to_fsdb);

  auto subscribedState = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().partialDrainState());

  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();
  rib_->setFibBatchTime(milliseconds(2));

  WITH_RETRIES({ ASSERT_EVENTUALLY_EQ(ribOutQ_.size(), 2); });
  REPEAT_N(2, folly::coro::blockingWait(ribOutQ_.pop()));

  auto prefixBatch = PrefixPathIds{{kV4Prefix1, kDefaultPathID}};
  fibFuture = fib_->getFibProgramFuture();
  sendAnnouncement(prefixBatch, eBgpPeer1_, attr_);
  fibFuture.wait();

  WITH_RETRIES({ ASSERT_EVENTUALLY_GE(ribOutQ_.size(), 1); });
  folly::coro::blockingWait(ribOutQ_.pop());

  // mnh=3 with a single installed path → prefix enters partial drain
  // (drainedPrefixCount_ 0 → 1). Flag off → no publish.
  TPathSelector tPathSelector;
  tPathSelector.bgp_native_path_selection_min_nexthop() = 3;
  tPathSelector.drain_on_min_nexthop_violation() = true;

  auto ribFuture = rib_->getRibPrepareFibProgrammingFuture();
  sendPathSelectionPolicySet(
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector));
  rib_->waitForPathSelectionPolicyUpdate();
  ribFuture.wait();

  WITH_RETRIES_N(5, {
    auto stateLk = subscribedState.rlock();
    EXPECT_EVENTUALLY_FALSE(stateLk->has_value());
  })

  // Withdraw the only path → drainedPrefixCount_ flips 1 → 0. Still off, so the
  // exit edge is also a no-op and the node remains absent.
  fibFuture = fib_->getFibProgramFuture();
  sendWithdrawal(prefixBatch, eBgpPeer1_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto stateLk = subscribedState.rlock();
    EXPECT_EVENTUALLY_FALSE(stateLk->has_value());
  })
}

/*
 * End-to-end gating test for the LBW-violation branch of the FSDB
 * partial-drain publish. Mirrors PartialDrainStatePublishedToFsdbOnTransition
 * but triggers the drain via bgp_min_aggregate_lbw_bps instead of
 * bgp_native_path_selection_min_nexthop. Verifies the published
 * TPartiallyDrainedPrefix carries min_capacity.agg_lbw_bps() set
 * to the configured threshold (and the next_hop_count union arm unset).
 *
 * Pinned end-to-end so a future regression in
 * RibBase::buildPartialDrainPrefixEntry that mis-selects the union arm — or
 * a missed aggLbwBpsThreshold_ plumbing step (RibPolicy → RibEntry → FSDB)
 * — fails here.
 */
TEST_F(RibFsdbFixture, PartialDrainStatePublishedWithLbwThresholdOnTransition) {
  FLAGS_publish_partial_drain_state_to_fsdb = true;
  SCOPE_EXIT {
    FLAGS_publish_partial_drain_state_to_fsdb = false;
  };

  auto subscribedState = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().partialDrainState());

  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();
  rib_->setFibBatchTime(milliseconds(2));

  // Drain the initial-dump messages (RibInitialAnnouncementStart +
  // RibOutAnnouncement with initialDump=true).
  WITH_RETRIES({ ASSERT_EVENTUALLY_EQ(ribOutQ_.size(), 2); });
  REPEAT_N(2, folly::coro::blockingWait(ribOutQ_.pop()));

  /*
   * Build an LBW-capable attribute path: 10 Gbps via non-transitive LBW
   * community on AS1. Any policy threshold above 10 Gbps will violate the
   * aggregate-LBW check after the single path is multipath-selected.
   * (RibPolicy.cpp converts community bytes/sec to bits/sec via `*8` and
   * sums across multipaths before comparing against bgpNativeMinAggLbwbps_.)
   */
  auto attrLbw =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(4, 4, 4, 4));
  attrLbw->setNonTransitiveLbwExtCommunity(uint16_t(kLocalAs1), kLbw10G);
  attrLbw->publish();

  auto prefixBatch = PrefixPathIds{{kV4Prefix1, kDefaultPathID}};
  fibFuture = fib_->getFibProgramFuture();
  sendAnnouncement(prefixBatch, eBgpPeer1_, attrLbw);
  fibFuture.wait();

  WITH_RETRIES({ ASSERT_EVENTUALLY_GE(ribOutQ_.size(), 1); });
  folly::coro::blockingWait(ribOutQ_.pop());

  /*
   * Inject CPS policy with bgp_min_aggregate_lbw_bps = 100 Gbps and
   * drain_on_min_nexthop_violation = true. The single installed path
   * provides 10 Gbps, so the aggregate falls below 100 Gbps and the prefix
   * enters partial drain via the LBW branch.
   *
   * Critical: bgp_native_path_selection_min_nexthop must NOT be set. When
   * MNH is set, PathSelector::overrideMultipathSelection returns before
   * reaching the LBW check (see the MNH branch in RibPolicy.cpp), which
   * would route the drain through the MNH arm instead.
   */
  constexpr int64_t kAggLbwBpsThreshold = 100LL * 1'000'000'000LL; // 100 Gbps
  TPathSelector tPathSelector;
  tPathSelector.bgp_min_aggregate_lbw_bps() = kAggLbwBpsThreshold;
  tPathSelector.drain_on_min_nexthop_violation() = true;

  auto ribFuture = rib_->getRibPrepareFibProgrammingFuture();
  sendPathSelectionPolicySet(
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector));
  rib_->waitForPathSelectionPolicyUpdate();
  ribFuture.wait();

  WITH_RETRIES_N(5, {
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
    /*
     * The drain trigger was LBW, so min_capacity (TCapacity) must hold
     * agg_lbw_bps == the configured threshold (100 Gbps) and the
     * next_hop_count arm must NOT be set. Mirrors the inverse assertion in
     * the MNH test above.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->drained_prefixes()->at(0).min_capacity()->agg_lbw_bps(),
        kAggLbwBpsThreshold);
    EXPECT_EVENTUALLY_FALSE((*stateLk)
                                ->drained_prefixes()
                                ->at(0)
                                .min_capacity()
                                ->next_hop_count()
                                .has_value());
    /*
     * current_capacity mirrors the LBW criterion: the agg_lbw_bps arm holds
     * the CURRENT aggregate link bandwidth (not the threshold) — one path of
     * kLbw10G bytes/sec * 8 = 10 Gbps, computed on demand by
     * RibDC::buildPartialDrainPrefixEntry — and the next_hop_count arm is
     * unset. This is the behavioral evidence that the producer recomputes and
     * surfaces the current aggregate LBW, the gap yikailin's comment flagged.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .current_capacity()
             ->agg_lbw_bps(),
        static_cast<int64_t>(kLbw10G * 8));
    EXPECT_EVENTUALLY_FALSE((*stateLk)
                                ->drained_prefixes()
                                ->at(0)
                                .current_capacity()
                                ->next_hop_count()
                                .has_value());
  })
}

} // namespace bgp
} // namespace facebook
