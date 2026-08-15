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

#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
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

constexpr auto kAcceptPolicyName = "ERASED_DUMP_ACCEPT";
constexpr auto kDummyPolicyName = "ERASED_DUMP_DUMMY";
constexpr auto kTagPolicyName = "ERASED_DUMP_TAG";
constexpr auto kTagCommunity = "64512:99";
constexpr auto kRoute = "40.0.0.0/8";

class UpdateGroupErasedInitialDumpConsumerE2ETest
    : public E2ESessionTestFixture {
 protected:
  using BgpClient =
      apache::thrift::Client<neteng::fboss::bgp::thrift::TBgpService>;

  void SetUp() override {
    setupPolicies();

    auto peer3 = kDefaultPeerSpec3_AddPath;
    peer3.egressPolicyName = kAcceptPolicyName;
    auto peer4 = kDefaultPeerSpec4;
    peer4.egressPolicyName = kTagPolicyName;
    auto peer5 = kDefaultPeerSpec5;
    peer5.egressPolicyName = kAcceptPolicyName;
    addPeer(peer3);
    addPeer(peer4);
    addPeer(peer5);

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
    auto tagStatement = createBgpPolicyStatement(
        kTagPolicyName,
        {createBgpPolicyTerm(
            "tag-all",
            "Accept and tag all routes",
            {},
            {createBgpPolicyCommunityAction(
                 bgp_policy::CommunityActionType::ADD, {kTagCommunity}),
             createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(
        makeAcceptStatement(kAcceptPolicyName));
    policies.bgp_policy_statements()->emplace_back(
        makeAcceptStatement(kDummyPolicyName));
    policies.bgp_policy_statements()->emplace_back(std::move(tagStatement));
    setPolicyConfig(policies);
  }

  neteng::fboss::bgp::thrift::BgpPolicyChangeResult setPeerPolicy(
      const folly::IPAddress& peerAddr,
      const std::string& policyName) {
    std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
        peersPolicy;
    peersPolicy[peerAddr.str()][bgp_policy::DIRECTION::OUT] = policyName;
    return folly::coro::blockingWait(
        bgpClient_->co_setPeersPolicy(peersPolicy));
  }

  uint64_t getShadowRibEntryCount() {
    auto stats = folly::coro::blockingWait(bgpClient_->co_getEntryStats());
    return *stats.total_shadow_rib_entries();
  }

  bool waitForPeerUpdateState(
      const folly::IPAddress& peerAddr,
      PeerUpdateState targetState,
      int maxRetries = 50) {
    auto& eventBase = peerManager_->getEventBase();
    PeerUpdateState state = PeerUpdateState::DOWN;
    WITH_RETRIES_N(maxRetries, {
      eventBase.runInEventBaseThreadAndWait([&]() {
        auto adjRib = getAdjRibByAddr(peerAddr);
        state = adjRib ? adjRib->getPeerState() : PeerUpdateState::DOWN;
      });
      EXPECT_EVENTUALLY_EQ(targetState, state);
    });
    return state == targetState;
  }

  void completeInitialExchange(
      const BgpPeerId& peer3,
      const BgpPeerId& peer4,
      const BgpPeerId& peer5) {
    sendEoRToPeer(peer3);
    sendEoRToPeer(peer4);
    sendEoRToPeer(peer5);
    ASSERT_TRUE(waitForEoR(peer3));
    ASSERT_TRUE(waitForEoR(peer3));
    ASSERT_TRUE(waitForEoR(peer4));
    ASSERT_TRUE(waitForEoR(peer4));
    ASSERT_TRUE(waitForEoR(peer5));
    ASSERT_TRUE(waitForEoR(peer5));
  }

  std::unique_ptr<Watchdog> watchdog_;
  std::shared_ptr<BgpServiceBB> bgpService_;
  std::unique_ptr<BgpClient> bgpClient_;
};

TEST_F(
    UpdateGroupErasedInitialDumpConsumerE2ETest,
    ErasedEmptyGroupCannotRegisterConsumerFromQueuedInitialDump) {
  BgpPeerId peer3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peer4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peer5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);
  completeInitialExchange(peer3, peer4, peer5);

  std::weak_ptr<AdjRibOutGroup> originalAcceptGroup;
  auto& eventBase = peerManager_->getEventBase();
  eventBase.runInEventBaseThreadAndWait([&]() {
    auto adjRib = getAdjRibByAddr(kPeerAddr5);
    originalAcceptGroup = adjRib->getUpdateGroup();
  });
  bringDownPeerAndWait(kPeerAddr5);
  ASSERT_TRUE(originalAcceptGroup.expired());

  folly::Baton<> sourceCaptured;
  folly::Baton<> racePreconditionChecked;
  std::weak_ptr<AdjRibOutGroup> erasedSourceGroup;
  uint64_t erasedSourceGroupId{0};
  bool sourceCaptureObserved{false};
  bool racePreconditionObserved{false};
  auto dummyPolicyResult =
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::INTERNAL_ERROR;
  auto tagPolicyResult =
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::INTERNAL_ERROR;

  struct EventBaseBarrier {
    folly::Baton<> entered;
    folly::Baton<> release;
    folly::Baton<> exited;
  };
  constexpr auto kGateTimeout = std::chrono::seconds(30);
  auto firstGate = std::make_shared<EventBaseBarrier>();
  auto secondGate = std::make_shared<EventBaseBarrier>();
  auto firstGateGuard =
      folly::makeGuard([firstGate] { firstGate->release.post(); });
  auto secondGateGuard =
      folly::makeGuard([secondGate] { secondGate->release.post(); });

  /*
   * The first gate lets the dummy-policy RPC queue its fleet reconciliation
   * behind the second gate. While that gate holds, the production session
   * event, the source-group capture and the TAG-policy RPC are queued in that
   * order. Processing the session event creates the source group and queues
   * its initial dump ahead of reconciliation, producing the required ordering
   * without modifying production state from the test.
   */
  eventBase.runInEventBaseThread([firstGate, kGateTimeout] {
    firstGate->entered.post();
    firstGate->release.try_wait_for(kGateTimeout);
    firstGate->exited.post();
  });
  test::boundedBatonWait(firstGate->entered, "erased dump first gate");
  dummyPolicyResult = setPeerPolicy(kPeerAddr3, kDummyPolicyName);
  eventBase.runInEventBaseThread([secondGate, kGateTimeout] {
    secondGate->entered.post();
    secondGate->release.try_wait_for(kGateTimeout);
    secondGate->exited.post();
  });
  firstGate->release.post();
  firstGateGuard.dismiss();
  test::boundedBatonWait(firstGate->exited, "erased dump first gate exit");
  test::boundedBatonWait(secondGate->entered, "erased dump second gate");

  bringUpPeer(kPeerAddr5);

  /*
   * Capture the source group BEFORE the TAG-policy RPC is issued. Applying the
   * policy names and re-keying the groups is a single event-base item, so once
   * reconciliation has run the peer already points at the target group and the
   * source is gone from updateGroups_ -- there is no later point at which the
   * source can still be found. Queuing this ahead of the RPC is what keeps a
   * handle on it: the session event is queued first (creating the source
   * group), this capture second, and the RPC enqueues its work before
   * returning, so reconciliation is strictly third.
   */
  eventBase.runInEventBaseThread([&]() {
    auto adjRib5 = getAdjRibByAddr(kPeerAddr5);
    auto sourceGroup = adjRib5 ? adjRib5->getUpdateGroup() : nullptr;
    sourceCaptureObserved = adjRib5 && sourceGroup &&
        sourceGroup->getState() == UpdateGroupState::UNINITIALIZED &&
        sourceGroup->getMemberCount() == 1 &&
        adjRib5->getPeerState() == PeerUpdateState::INIT &&
        sourceGroup->getGroupKey().egressPolicyName.value_or("") ==
            kAcceptPolicyName &&
        !adjRib5->getChangeListConsumer() &&
        !sourceGroup->getChangeListConsumer();
    erasedSourceGroup = sourceGroup;
    if (sourceGroup) {
      erasedSourceGroupId = sourceGroup->getGroupId();
    }
    sourceCaptured.post();
  });

  tagPolicyResult = setPeerPolicy(kPeerAddr5, kTagPolicyName);

  /*
   * Post-reconciliation checkpoint. The peer has been moved to the TAG group
   * and the source group has been erased from updateGroups_, but the source
   * object is still alive: maybeDestroyUpdateGroups holds it in removedGroups
   * and suspends on drainAsyncScope(), which is what lets this callback run
   * before the group is destroyed. That suspension is the window in which the
   * source group's queued initial dump could still register a consumer into an
   * erased group -- the bug this test guards.
   */
  eventBase.runInEventBaseThread([&]() {
    auto adjRib5 = getAdjRibByAddr(kPeerAddr5);
    auto adjRib4 = getAdjRibByAddr(kPeerAddr4);
    auto peerGroup = adjRib5 ? adjRib5->getUpdateGroup() : nullptr;
    auto targetGroup = adjRib4 ? adjRib4->getUpdateGroup() : nullptr;
    auto sourceGroup = erasedSourceGroup.lock();
    racePreconditionObserved = adjRib5 && peerGroup && targetGroup &&
        sourceGroup && sourceGroup != targetGroup && peerGroup == targetGroup &&
        sourceGroup->getMemberCount() == 0 &&
        adjRib5->getPeerState() == PeerUpdateState::DETACHED_INIT_DUMP &&
        targetGroup->getGroupKey().egressPolicyName.value_or("") ==
            kTagPolicyName &&
        adjRib5->getEgressPolicyName().value_or("") == kTagPolicyName;
    racePreconditionChecked.post();
  });
  secondGate->release.post();
  secondGateGuard.dismiss();
  test::boundedBatonWait(secondGate->exited, "erased dump second gate exit");
  test::boundedBatonWait(sourceCaptured, "erased dump source capture");
  test::boundedBatonWait(racePreconditionChecked, "erased dump precondition");

  ASSERT_TRUE(sourceCaptureObserved)
      << "reconnecting peer must own an uninitialized single-member source "
         "group with the pre-update policy before reconciliation runs";
  ASSERT_TRUE(racePreconditionObserved)
      << "reconciliation must have moved the peer to the target group and "
         "emptied the source while the peer's initial dump is still pending";
  ASSERT_EQ(
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED,
      dummyPolicyResult);
  ASSERT_EQ(
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED,
      tagPolicyResult);
  ASSERT_TRUE(waitForSessionEstablished(kPeerAddr5));

  bool movedToTarget{false};
  eventBase.runInEventBaseThreadAndWait([&]() {
    auto adjRib5 = getAdjRibByAddr(kPeerAddr5);
    auto adjRib4 = getAdjRibByAddr(kPeerAddr4);
    movedToTarget = adjRib5 && adjRib4 &&
        adjRib5->getUpdateGroup() == adjRib4->getUpdateGroup();
  });
  ASSERT_TRUE(movedToTarget);
  ASSERT_TRUE(
      waitForPeerUpdateState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /*
   * The policy-re-evaluation flag clears before empty-group destruction is
   * awaited. Wait until the source is absent from the production group view
   * and its queued dump has either been cancelled (the group expires) or has
   * reproduced the bug by registering a consumer after erasure.
   */
  bool sourceCleanupFinished{false};
  WITH_RETRIES_N(50, {
    const bool erasedFromManager =
        peerManager_
            ->getUpdateGroupInfo(static_cast<int64_t>(erasedSourceGroupId))
            .empty();
    bool consumerRegisteredAfterErase{false};
    eventBase.runInEventBaseThreadAndWait([&]() {
      if (auto sourceGroup = erasedSourceGroup.lock()) {
        consumerRegisteredAfterErase =
            static_cast<bool>(sourceGroup->getChangeListConsumer());
      }
    });
    if (erasedFromManager &&
        (erasedSourceGroup.expired() || consumerRegisteredAfterErase)) {
      sourceCleanupFinished = true;
    }
    EXPECT_EVENTUALLY_TRUE(sourceCleanupFinished);
  });
  ASSERT_TRUE(sourceCleanupFinished);

  const auto baselineShadowRibEntries = getShadowRibEntryCount();
  injectLocalRoutesAtRuntime({kRoute});
  ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(kRoute)));
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "40.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str(), "", kTagCommunity));
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "40.0.0.0", 8, kPeerAddr3, kNextHopV4_3.str()));

  withdrawLocalRoutesAtRuntime({kRoute});
  ASSERT_TRUE(verifyRouteWithdraw("v4", "40.0.0.0", 8, kPeerAddr4));
  ASSERT_TRUE(verifyRouteWithdraw("v4", "40.0.0.0", 8, kPeerAddr3));

  EXPECT_EQ(baselineShadowRibEntries, getShadowRibEntryCount());
  EXPECT_TRUE(erasedSourceGroup.expired());
}

} // namespace
} // namespace facebook::bgp
