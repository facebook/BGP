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

#include <folly/coro/BlockingWait.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/synchronization/Baton.h>

#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include "neteng/fboss/bgp/cpp/facebook/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"
#include "neteng/fboss/bgp/if/gen-cpp2/TBgpService.h"

namespace facebook::bgp {
namespace {

constexpr auto kSourcePolicyName = "DYNAMIC_EOR_SOURCE_ACCEPT";
constexpr auto kTargetPolicyName = "DYNAMIC_EOR_TARGET_ACCEPT";
constexpr auto kRoute = "50.0.0.0/8";

class UpdateGroupDynamicMembershipEoRE2ETest : public E2ESessionTestFixture {
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

  static constexpr auto kConditionPollInterval = std::chrono::milliseconds(10);
  static constexpr size_t kConditionPollAttempts = 500;

  void SetUp() override {
    setupPolicies();

    auto peer3 = kDefaultPeerSpec3;
    peer3.egressPolicyName = kSourcePolicyName;
    auto peer4 = kDefaultPeerSpec4;
    peer4.egressPolicyName = kTargetPolicyName;
    addPeer(peer3);
    addPeer(peer4);

    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/true,
        /*enableEgressBackpressure=*/true,
        /*enableSerializeGroupPdu=*/true);

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
  bool waitForPeerManagerCondition(Condition condition) {
    struct WaitState {
      folly::Baton<> done;
      std::unique_ptr<folly::AsyncTimeout> timer;
      std::atomic<bool> matched{false};
      size_t remainingAttempts{kConditionPollAttempts};
    };

    auto state = std::make_shared<WaitState>();
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
                if (--state->remainingAttempts == 0) {
                  state->done.post();
                  return;
                }
                state->timer->scheduleTimeout(kConditionPollInterval);
              });
          state->timer->scheduleTimeout(std::chrono::milliseconds(0));
        });

    try {
      test::boundedBatonWait(
          state->done,
          "dynamic-membership EoR PeerManager condition",
          std::chrono::seconds(10));
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
    UpdateGroupDynamicMembershipEoRE2ETest,
    FullyInitializedPeerJoiningBlockedInitialEorGroupGetsNoDuplicateEor) {
  BgpPeerId peer3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peer4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  sendEoRToPeer(peer3);
  sendEoRToPeer(peer4);
  ASSERT_TRUE(waitForEoR(peer3));
  ASSERT_TRUE(waitForEoR(peer3));
  ASSERT_TRUE(waitForEoR(peer4));
  ASSERT_TRUE(waitForEoR(peer4));

  bringDownPeerAndWait(kPeerAddr4);

  injectLocalRoutesAtRuntime({kRoute});
  ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(kRoute)));
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "50.0.0.0", 8, kPeerAddr3, kNextHopV4_3.str()));

  auto& eventBase = peerManager_->getEventBase();
  bool sourceFullyInitialized{false};
  uint64_t sourceSentEoRs{0};
  eventBase.runInEventBaseThreadAndWait([&]() {
    auto adjRib = getAdjRibByAddr(kPeerAddr3);
    sourceFullyInitialized = adjRib &&
        adjRib->getPeerState() == PeerUpdateState::JOINED_RUNNING &&
        !adjRib->egressEoRPendingV4() && !adjRib->egressEoRPendingV6();
    if (adjRib) {
      sourceSentEoRs = adjRib->getStats().getSentEndOfRibMsgs();
    }
  });
  ASSERT_TRUE(sourceFullyInitialized);

  /*
   * The route is the first initial-dump PDU and reaches peer4's high watermark.
   * The target group's next PDU is its v4 EoR, which therefore takes the real
   * deferred-push/backpressure path and suspends the group builder.
   */
  setDefaultQueueSizes(/*capacity=*/2, /*highWm=*/1, /*lowWm=*/0);
  blockPeer(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr4);
  sendEoRToPeer(peer4);

  ASSERT_TRUE(waitForPeerManagerCondition([&]() {
    auto source = getAdjRibByAddr(kPeerAddr3);
    auto blocked = getAdjRibByAddr(kPeerAddr4);
    auto group = blocked ? blocked->getUpdateGroup() : nullptr;
    return source && blocked && group && source->getUpdateGroup() != group &&
        group->getState() == UpdateGroupState::WAITING &&
        group->hasBlockedPeers() &&
        blocked->getPeerState() == PeerUpdateState::JOINED_BLOCKED &&
        blocked->egressEoRPendingV4() && blocked->egressEoRPendingV6();
  }));
  ASSERT_EQ(1, getPeerQueueSize(peer4));

  ASSERT_EQ(
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED,
      setPeerPolicy(kPeerAddr3, kTargetPolicyName));

  ASSERT_TRUE(waitForPeerManagerCondition([&]() {
    auto moved = getAdjRibByAddr(kPeerAddr3);
    auto blocked = getAdjRibByAddr(kPeerAddr4);
    auto group = blocked ? blocked->getUpdateGroup() : nullptr;
    return moved && blocked && group && moved->getUpdateGroup() == group &&
        moved->getPeerState() == PeerUpdateState::JOINED_RUNNING &&
        blocked->getPeerState() == PeerUpdateState::JOINED_BLOCKED &&
        group->getState() == UpdateGroupState::WAITING &&
        group->hasBlockedPeers() && group->getMemberCount() == 2 &&
        group->getNumInSyncPeers() == 2 && !moved->egressEoRPendingV4() &&
        !moved->egressEoRPendingV6();
  }));
  ASSERT_EQ(0, getPeerQueueSize(peer3));

  ASSERT_TRUE(unblockPeer(kPeerAddr4, /*maxRetries=*/50, /*maxMessages=*/100));

  ASSERT_TRUE(waitForPeerManagerCondition([&]() {
    auto moved = getAdjRibByAddr(kPeerAddr3);
    auto released = getAdjRibByAddr(kPeerAddr4);
    auto group = released ? released->getUpdateGroup() : nullptr;
    return moved && released && group &&
        group->getState() == UpdateGroupState::IDLE &&
        !moved->egressEoRPendingV4() && !moved->egressEoRPendingV6() &&
        !released->egressEoRPendingV4() && !released->egressEoRPendingV6();
  }));

  const auto eorCounts = drainAndCountEoRs(peer3);
  EXPECT_EQ(0, eorCounts.other);
  EXPECT_EQ(0, eorCounts.v4);
  EXPECT_EQ(0, eorCounts.v6)
      << "peer3 had cleared both per-AFI pending flags before joining the "
         "target group, so a v6 EoR here is a duplicate";

  uint64_t movedSentEoRs{0};
  eventBase.runInEventBaseThreadAndWait([&]() {
    auto moved = getAdjRibByAddr(kPeerAddr3);
    if (moved) {
      movedSentEoRs = moved->getStats().getSentEndOfRibMsgs();
    }
  });
  EXPECT_EQ(sourceSentEoRs, movedSentEoRs)
      << "peer3 must not account for an EoR it did not owe";
}

} // namespace
} // namespace facebook::bgp
