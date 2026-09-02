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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include <gtest/gtest.h>

#include <folly/ScopeGuard.h>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/synchronization/Baton.h>

#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include "neteng/fboss/bgp/cpp/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"
#include "neteng/fboss/bgp/if/gen-cpp2/TBgpService.h"

namespace facebook::bgp {
namespace {

constexpr auto kSourcePolicyName = "SPLIT_EOR_SOURCE_ACCEPT";
constexpr auto kTargetPolicyName = "SPLIT_EOR_TARGET_ACCEPT";
constexpr auto kPeerGroupName = "split-eor-peer-group";
constexpr auto kInitialRoute = "55.0.0.0/8";

class UpdateGroupSplitDuringEoRWaitE2ETest : public E2ESessionTestFixture {
 protected:
  using BgpClient =
      apache::thrift::Client<neteng::fboss::bgp::thrift::TBgpService>;
  using PeersPolicy =
      std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>;

  struct EorCounts {
    size_t v4{0};
    size_t v6{0};
    size_t other{0};
  };

  void SetUp() override {
    setupPolicies();

    auto slowPeer = kDefaultPeerSpec3;
    slowPeer.peerGroupName = kPeerGroupName;
    slowPeer.egressPolicyName = kSourcePolicyName;
    auto movedPeer = kDefaultPeerSpec4;
    movedPeer.peerGroupName = kPeerGroupName;
    movedPeer.egressPolicyName = kSourcePolicyName;
    addPeer(slowPeer);
    addPeer(movedPeer);

    addLocalRoute(kInitialRoute, {"5500:1"}, 100);
    setEorTimeSeconds(1);

    thrift::UpdateGroupConfig updateGroupConfig;
    updateGroupConfig.allowSlowPeerDetach() = false;
    setUpdateGroupConfig(updateGroupConfig);

    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/true,
        /*enableEgressBackpressure=*/true,
        /*enableSerializeGroupPdu=*/true);
  }

  void createPolicyClient() {
    watchdog_ = std::make_unique<Watchdog>(config_);
    bgpService_ = std::make_shared<BgpServiceBB>(
        *peerManager_,
        configManager_,
        *rib_,
        *watchdog_,
        /*nlWrapper=*/nullptr,
        /*enable_thrift_protection=*/false);
    bgpClient_ = apache::thrift::makeTestClient<BgpClient>(bgpService_);
  }

  void TearDown() override {
    bgpClient_.reset();
    bgpService_.reset();
    watchdog_.reset();
    E2ESessionTestFixture::TearDown();
  }

  void setupPolicies() {
    auto makeAcceptStatement = [](const std::string& policyName) {
      return createBgpPolicyStatement(
          policyName,
          {createBgpPolicyTerm(
              "accept-all",
              "Accept all routes",
              {},
              {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
              bgp_policy::FlowControlAction::NEXT_TERM)});
    };

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(
        makeAcceptStatement(kSourcePolicyName));
    policies.bgp_policy_statements()->emplace_back(
        makeAcceptStatement(kTargetPolicyName));
    setPolicyConfig(policies);
  }

  neteng::fboss::bgp::thrift::BgpPolicyChangeResult setPeerPolicy(
      const folly::IPAddress& peerAddr,
      const std::string& policyName) {
    PeersPolicy peersPolicy;
    peersPolicy[peerAddr.str()][bgp_policy::DIRECTION::OUT] = policyName;
    return folly::coro::blockingWait(
        bgpClient_->co_setPeersPolicy(peersPolicy));
  }

  template <typename Condition>
  bool waitForPeerManagerCondition(
      Condition condition,
      std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
    struct WaitState {
      folly::Baton<> done;
      std::unique_ptr<folly::AsyncTimeout> timer;
      std::atomic<bool> matched{false};
      std::chrono::steady_clock::time_point deadline;
    };

    auto state = std::make_shared<WaitState>();
    state->deadline = std::chrono::steady_clock::now() + timeout;
    auto& eventBase = peerManager_->getEventBase();
    eventBase.runInEventBaseThreadAndWait(
        [state, &eventBase, condition = std::move(condition)]() mutable {
          state->timer = folly::AsyncTimeout::make(
              eventBase,
              [state, condition = std::move(condition)]() mutable noexcept {
                if (condition()) {
                  state->matched.store(true, std::memory_order_release);
                  state->done.post();
                  return;
                }
                if (std::chrono::steady_clock::now() >= state->deadline) {
                  state->done.post();
                  return;
                }
                state->timer->scheduleTimeout(std::chrono::milliseconds(10));
              });
          state->timer->scheduleTimeout(std::chrono::milliseconds(0));
        });

    try {
      test::boundedBatonWait(
          state->done,
          "split-during-EoR-wait PeerManager condition",
          std::chrono::ceil<std::chrono::seconds>(timeout) +
              std::chrono::seconds(5));
    } catch (...) {
      eventBase.runInEventBaseThread([state]() {
        state->timer->cancelTimeout();
        state->timer.reset();
      });
      throw;
    }
    eventBase.runInEventBaseThreadAndWait([state]() {
      state->timer->cancelTimeout();
      state->timer.reset();
    });
    return state->matched.load(std::memory_order_acquire);
  }

  EorCounts drainAndCountEoRs(const BgpPeerId& peerId) {
    EorCounts counts;
    auto queues = getPeerQueues(peerId);
    EXPECT_TRUE(queues.has_value());
    if (!queues) {
      return counts;
    }

    while (!queues->boundedAdjRibOutQ->empty()) {
      auto message = test::boundedBlockingPop(
          *queues->boundedAdjRibOutQ, "boundedAdjRibOutQ");
      if (!message) {
        continue;
      }
      if (auto* eor = std::get_if<BgpEndOfRib>(&*message)) {
        if (*eor->afi() == nettools::bgplib::BgpUpdateAfi::AFI_IPv4) {
          ++counts.v4;
        } else if (*eor->afi() == nettools::bgplib::BgpUpdateAfi::AFI_IPv6) {
          ++counts.v6;
        } else {
          ++counts.other;
        }
      } else {
        ++counts.other;
      }
    }
    return counts;
  }

  std::unique_ptr<Watchdog> watchdog_;
  std::shared_ptr<BgpServiceBB> bgpService_;
  std::unique_ptr<BgpClient> bgpClient_;
};

TEST_F(
    UpdateGroupSplitDuringEoRWaitE2ETest,
    PartialPolicySplitWhileV4EorBlockedStillSendsMovedPeerV6Eor) {
  BgpPeerId slowPeerId{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId movedPeerId{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /*
   * The initial route reaches peer3's high watermark, so its next push is
   * deferred. The fast peer has room for the route and both dual-stack EoRs.
   * Hold the PeerManager event loop while both production session events are
   * queued. Otherwise peer3 can start the group initial dump before peer4 is
   * registered, sending peer4 through an independent detached dump instead of
   * the shared EoR epoch this test requires.
   */
  struct EventBaseBarrier {
    folly::Baton<> entered;
    folly::Baton<> release;
    folly::Baton<> exited;
    std::atomic<bool> released{false};
  };
  auto gate = std::make_shared<EventBaseBarrier>();
  auto gateGuard = folly::makeGuard([gate] { gate->release.post(); });
  peerManager_->getEventBase().runInEventBaseThread([gate] {
    gate->entered.post();
    gate->released.store(
        gate->release.try_wait_for(std::chrono::seconds(10)),
        std::memory_order_release);
    gate->exited.post();
  });
  test::boundedBatonWait(gate->entered, "split EoR session setup gate");

  setDefaultQueueSizes(/*capacity=*/2, /*highWm=*/1, /*lowWm=*/0);
  bringUpPeer(kPeerAddr3);
  setDefaultQueueSizes(/*capacity=*/8, /*highWm=*/6, /*lowWm=*/2);
  bringUpPeer(kPeerAddr4);

  gate->release.post();
  gateGuard.dismiss();
  test::boundedBatonWait(gate->exited, "split EoR session setup gate exit");
  ASSERT_TRUE(gate->released.load(std::memory_order_acquire));
  ASSERT_TRUE(waitForSessionEstablished(kPeerAddr3));
  ASSERT_TRUE(waitForSessionEstablished(kPeerAddr4));

  sendEoRToPeer(slowPeerId);
  sendEoRToPeer(movedPeerId);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork(kInitialRoute)));

  std::shared_ptr<AdjRibOutGroup> sourceGroup;
  ASSERT_TRUE(waitForPeerManagerCondition([&]() {
    auto slow = getAdjRibByAddr(kPeerAddr3);
    auto moved = getAdjRibByAddr(kPeerAddr4);
    if (!slow || !moved || !slow->getUpdateGroup() ||
        slow->getUpdateGroup() != moved->getUpdateGroup()) {
      return false;
    }
    auto group = slow->getUpdateGroup();
    if (group->getState() != UpdateGroupState::WAITING ||
        !group->hasBlockedPeers() ||
        slow->getPeerState() != PeerUpdateState::JOINED_BLOCKED ||
        moved->getPeerState() != PeerUpdateState::JOINED_RUNNING ||
        !slow->egressEoRPendingV4() || !slow->egressEoRPendingV6() ||
        moved->egressEoRPendingV4() || !moved->egressEoRPendingV6()) {
      return false;
    }
    sourceGroup = std::move(group);
    return true;
  })) << "the source group did not suspend on peer3's real deferred v4 EoR "
         "after committing v4 to peer4";
  ASSERT_EQ(1, getPeerQueueSize(slowPeerId));
  ASSERT_EQ(2, getPeerQueueSize(movedPeerId));

  /*
   * Egress EoR is intentionally incomplete, so initialization must come from
   * the production initialized max-wait timer (5 * eor_time_s), not from the
   * successful-EoR path. Create the policy RPC endpoint only after forming this
   * state because starting its test server can outlast eor_time_s.
   */
  ASSERT_FALSE(peerManager_->getIsInitialized());
  createPolicyClient();
  ASSERT_TRUE(waitForPeerManagerCondition(
      [&]() { return peerManager_->getIsInitialized(); },
      std::chrono::seconds(10)));

  ASSERT_EQ(
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED,
      setPeerPolicy(kPeerAddr4, kTargetPolicyName));

  std::shared_ptr<AdjRibOutGroup> targetGroup;
  ASSERT_TRUE(waitForPeerManagerCondition([&]() {
    auto slow = getAdjRibByAddr(kPeerAddr3);
    auto moved = getAdjRibByAddr(kPeerAddr4);
    if (!slow || !moved || slow->getUpdateGroup() != sourceGroup ||
        !moved->getUpdateGroup() || moved->getUpdateGroup() == sourceGroup) {
      return false;
    }
    targetGroup = moved->getUpdateGroup();
    return sourceGroup->getMemberCount() == 1 &&
        targetGroup->getMemberCount() == 1 &&
        sourceGroup->getGroupKey().egressPolicyName == kSourcePolicyName &&
        targetGroup->getGroupKey().egressPolicyName == kTargetPolicyName &&
        targetGroup->getState() == UpdateGroupState::IDLE;
  })) << "the real per-peer policy override did not complete its partial "
         "update-group split";

  drainPeerQueueCompletely(slowPeerId, /*maxRetries=*/100, /*maxMessages=*/100);
  ASSERT_TRUE(waitForPeerManagerCondition(
      [&]() {
        auto slow = getAdjRibByAddr(kPeerAddr3);
        return slow && slow->getUpdateGroup() == sourceGroup &&
            sourceGroup->getState() == UpdateGroupState::IDLE &&
            !slow->egressEoRPendingV4() && !slow->egressEoRPendingV6();
      },
      std::chrono::seconds(15)))
      << "releasing peer3 did not let the source group finish v4 then v6";

  struct TargetState {
    UpdateGroupState groupState{UpdateGroupState::UNINITIALIZED};
    bool movedPeerV4Pending{false};
    bool movedPeerV6Pending{false};
  } targetState;
  bool movedPeerStillInTarget = false;
  peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
    auto moved = getAdjRibByAddr(kPeerAddr4);
    movedPeerStillInTarget = moved && targetGroup == moved->getUpdateGroup();
    targetState.groupState = targetGroup->getState();
    targetState.movedPeerV4Pending = moved && moved->egressEoRPendingV4();
    targetState.movedPeerV6Pending = moved && moved->egressEoRPendingV6();
  });
  ASSERT_TRUE(movedPeerStillInTarget);

  const auto movedPeerEors = drainAndCountEoRs(movedPeerId);
  EXPECT_EQ(1, movedPeerEors.other);
  EXPECT_EQ(1, movedPeerEors.v4);
  EXPECT_EQ(1, movedPeerEors.v6)
      << "peer4 committed v4 before the split but never received v6; target "
         "group state="
      << static_cast<int>(targetState.groupState)
      << ", peer4 v4-pending=" << targetState.movedPeerV4Pending
      << ", peer4 v6-pending=" << targetState.movedPeerV6Pending;
  EXPECT_FALSE(targetState.movedPeerV4Pending)
      << "peer4 must not re-owe v4 after the split";
  EXPECT_FALSE(targetState.movedPeerV6Pending)
      << "peer4 remained permanently v6-pending after leaving the source "
         "group's suspended EoR builder";
}

} // namespace
} // namespace facebook::bgp
