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
#include <memory>

#include <gtest/gtest.h>

#include <folly/ScopeGuard.h>
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

constexpr auto kAcceptPolicyName = "RECONNECT_ACCEPT";
constexpr auto kDummyPolicyName = "RECONNECT_DUMMY";
constexpr auto kTagPolicyName = "RECONNECT_TAG";
constexpr auto kTagCommunity = "64512:99";

class UpdateGroupPolicyReconnectRaceE2ETest : public E2ESessionTestFixture {
 protected:
  using BgpClient =
      apache::thrift::Client<neteng::fboss::bgp::thrift::TBgpService>;

  static constexpr auto kWaitTimeout = std::chrono::seconds(30);

  void SetUp() override {
    setupPolicies();

    auto peer3 = kDefaultPeerSpec3;
    peer3.egressPolicyName = kAcceptPolicyName;
    auto peer4 = kDefaultPeerSpec4;
    peer4.egressPolicyName = kTagPolicyName;
    auto peer5 = kDefaultPeerSpec5;
    peer5.egressPolicyName = kAcceptPolicyName;
    BgpPeerSpec peer6{
        .asn = kPeerAsn6,
        .localAddr = kLocalAddr6,
        .peerAddr = kPeerAddr6,
        .v4Nexthop = kNextHopV4_6,
        .v6Nexthop = kNextHopV6_6,
        .egressPolicyName = kAcceptPolicyName,
    };
    addPeer(peer3);
    addPeer(peer4);
    addPeer(peer5);
    addPeer(peer6);

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
    auto acceptStatement = createBgpPolicyStatement(
        kAcceptPolicyName,
        {createBgpPolicyTerm(
            "accept-all",
            "Accept all routes",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});
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
    auto dummyStatement = createBgpPolicyStatement(
        kDummyPolicyName,
        {createBgpPolicyTerm(
            "accept-all",
            "Accept all routes",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});
    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(std::move(acceptStatement));
    policies.bgp_policy_statements()->emplace_back(std::move(dummyStatement));
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

  bool waitForPeerUpdateState(
      const folly::IPAddress& peerAddr,
      PeerUpdateState targetState,
      int maxRetries = 50) {
    auto& eventBase = peerManager_->getEventBase();
    PeerUpdateState state = PeerUpdateState::DOWN;
    WITH_RETRIES_N(maxRetries, {
      eventBase.runInEventBaseThreadAndWait([&] {
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
      const BgpPeerId& peer5,
      const BgpPeerId& peer6) {
    sendEoRToPeer(peer3);
    sendEoRToPeer(peer4);
    sendEoRToPeer(peer5);
    sendEoRToPeer(peer6);
    ASSERT_TRUE(waitForEoR(peer3));
    ASSERT_TRUE(waitForEoR(peer3));
    ASSERT_TRUE(waitForEoR(peer4));
    ASSERT_TRUE(waitForEoR(peer4));
    ASSERT_TRUE(waitForEoR(peer5));
    ASSERT_TRUE(waitForEoR(peer5));
    ASSERT_TRUE(waitForEoR(peer6));
    ASSERT_TRUE(waitForEoR(peer6));
  }

  std::unique_ptr<Watchdog> watchdog_;
  std::shared_ptr<BgpServiceBB> bgpService_;
  std::unique_ptr<BgpClient> bgpClient_;
};

TEST_F(
    UpdateGroupPolicyReconnectRaceE2ETest,
    PolicyReevaluationCancellingReconnectDumpKeepsIncrementalUpdatesFlowing) {
  BgpPeerId peer3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peer4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peer5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  BgpPeerId peer6{kPeerAddr6, kPeerAddr6.asV4().toLongHBO()};

  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);
  bringUpPeerAndWait(kPeerAddr6);
  completeInitialExchange(peer3, peer4, peer5, peer6);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.0.0.0/8")));
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));

  addRoute("v4", "30.0.0.0", 8, kPeerAddr5, "31.0.0.1", "65005");
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.0.0.0/8")));
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "30.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str()));
  bringDownPeerAndWait(kPeerAddr5);
  ASSERT_TRUE(verifyRouteWithdraw("v4", "30.0.0.0", 8, kPeerAddr4));

  struct EventBaseBarrier {
    folly::Baton<> entered;
    folly::Baton<> release;
    folly::Baton<> exited;
    std::atomic<bool> released{false};
  };
  struct RaceState {
    folly::Baton<> checkpoint;
    bool preconditionObserved{false};
  };
  auto race = std::make_shared<RaceState>();
  auto& eventBase = peerManager_->getEventBase();
  auto firstGate = std::make_shared<EventBaseBarrier>();
  auto secondGate = std::make_shared<EventBaseBarrier>();
  auto firstGateGuard =
      folly::makeGuard([firstGate] { firstGate->release.post(); });
  auto secondGateGuard =
      folly::makeGuard([secondGate] { secondGate->release.post(); });

  /*
   * The first gate lets the dummy-policy callback queue a fleet reconciliation
   * behind the second gate. While the second gate holds, the production session
   * event and target-policy callback are queued ahead of that reconciliation.
   * The resulting FIFO order is: dummy apply, reconnect registration, target
   * apply plus fleet reconciliation, checkpoint. Applying the policy names and
   * re-keying the groups is a single event-base item, and reconciliation
   * cancels the reconnecting peer's scheduled dump to re-run it inline under
   * the new policy, so the checkpoint observes the peer already carrying the
   * target policy with that inline dump applied, before it rejoins its group.
   * This reaches the cancellation race without mutating peer or group state
   * from the test.
   */
  eventBase.runInEventBaseThread([firstGate] {
    firstGate->entered.post();
    firstGate->released.store(
        firstGate->release.try_wait_for(kWaitTimeout),
        std::memory_order_release);
    firstGate->exited.post();
  });
  test::boundedBatonWait(firstGate->entered, "reconnect first gate");
  const auto dummyPolicyResult = setPeerPolicy(kPeerAddr3, kDummyPolicyName);
  eventBase.runInEventBaseThread([secondGate] {
    secondGate->entered.post();
    secondGate->released.store(
        secondGate->release.try_wait_for(kWaitTimeout),
        std::memory_order_release);
    secondGate->exited.post();
  });
  firstGate->release.post();
  firstGateGuard.dismiss();
  test::boundedBatonWait(firstGate->exited, "reconnect first gate exit");
  test::boundedBatonWait(secondGate->entered, "reconnect second gate");

  bringUpPeer(kPeerAddr5);
  const auto tagPolicyResult = setPeerPolicy(kPeerAddr5, kTagPolicyName);
  eventBase.runInEventBaseThread([this, race] {
    auto adjRib = getAdjRibByAddr(kPeerAddr5);
    auto group = adjRib ? adjRib->getUpdateGroup() : nullptr;
    race->preconditionObserved = adjRib && group &&
        adjRib->getPeerState() == PeerUpdateState::DETACHED_INIT_DUMP &&
        adjRib->getUpdateGroupKey().egressPolicyName.value_or("") ==
            kTagPolicyName &&
        group->getGroupKey().egressPolicyName.value_or("") == kTagPolicyName &&
        adjRib->getEgressPolicyName().value_or("") == kTagPolicyName &&
        adjRib->getChangeListConsumer();
    race->checkpoint.post();
  });
  secondGate->release.post();
  secondGateGuard.dismiss();
  test::boundedBatonWait(secondGate->exited, "reconnect second gate exit");
  test::boundedBatonWait(race->checkpoint, "reconnect race checkpoint");

  ASSERT_TRUE(firstGate->released.load(std::memory_order_acquire));
  ASSERT_TRUE(secondGate->released.load(std::memory_order_acquire));
  ASSERT_EQ(
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED,
      dummyPolicyResult);
  ASSERT_EQ(
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED,
      tagPolicyResult);
  ASSERT_TRUE(race->preconditionObserved)
      << "fleet reconciliation must have moved the reconnecting peer onto the "
         "target policy and re-run its cancelled dump inline, leaving it "
         "detached with a consumer before it rejoins";
  ASSERT_TRUE(waitForEgressReEvalComplete());

  bool movedToTagGroup{false};
  eventBase.runInEventBaseThreadAndWait([&]() {
    auto adjRib = getAdjRibByAddr(kPeerAddr5);
    auto group = adjRib ? adjRib->getUpdateGroup() : nullptr;
    movedToTagGroup = group &&
        group->getGroupKey().egressPolicyName.value_or("") == kTagPolicyName;
  });
  ASSERT_TRUE(movedToTagGroup);

  ASSERT_TRUE(waitForSessionEstablished(kPeerAddr5));
  sendEoRToPeer(peer5);

  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str(), "", kTagCommunity));
  ASSERT_TRUE(
      waitForPeerUpdateState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  addRoute("v4", "20.0.0.0", 8, kPeerAddr6, "21.0.0.1", "65006");
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.0.0.0/8")));
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "20.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str()));
  EXPECT_TRUE(drainAndFindRouteAdvertised(
      "v4", "20.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str(), "", kTagCommunity));
}

} // namespace
} // namespace facebook::bgp
