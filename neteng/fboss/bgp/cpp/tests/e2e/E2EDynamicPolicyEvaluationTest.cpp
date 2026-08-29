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

/**
 * E2EDynamicPolicyEvaluationTest.cpp
 *
 * E2E tests for runtime dynamic policy evaluation APIs:
 * - setPeersPolicy
 * - setPeerGroupsPolicy
 * - unsetPeersPolicy
 *
 * These tests verify the complete drain workflow through the real BGP pipeline.
 * Routes flow through AdjRibIn → RIB → AdjRibOut and we verify the effect of
 * runtime policy changes on route propagation.
 *
 * Key difference from existing E2E tests: Existing tests set policies at setup
 * time (before createRib). This test changes policies at runtime via
 * BgpServiceBB thrift APIs after peers are established and routes are
 * flowing.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib, PolicyManager, BgpServiceBB
 */

#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/facebook/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"
#include "neteng/fboss/bgp/if/gen-cpp2/TBgpService.h"

using namespace facebook::bgp;

namespace facebook::bgp {

namespace {

constexpr auto kDrainPolicyName = "DRAIN_POLICY";
constexpr auto kUndrainPolicyName = "UNDRAIN_POLICY";
constexpr auto kTagPolicyName = "TAG_POLICY";
constexpr auto kTagCommunity = "12345:1";

} // namespace

/**
 * E2EDynamicPolicyEvaluationTest
 *
 * Test fixture for runtime dynamic policy evaluation APIs.
 * Configures peers with drain/undrain policies, brings up sessions, injects
 * routes, then exercises the drain APIs and verifies route behavior.
 */
class E2EDynamicPolicyEvaluationTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    setupDrainPolicies();

    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);

    createRib();
    createPeerManagerWithBgpService();
  }

  /**
   * Set up drain (DENY) and undrain (ACCEPT) policies in PolicyManager.
   * These policies will be used for runtime drain/undrain operations.
   */
  void setupDrainPolicies() {
    auto drainStatement = createBgpPolicyStatement(
        kDrainPolicyName,
        {createBgpPolicyTerm(
            "deny-all",
            "Deny all routes (drain)",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::DENY)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    auto undrainStatement = createBgpPolicyStatement(
        kUndrainPolicyName,
        {createBgpPolicyTerm(
            "accept-all",
            "Accept all routes (undrain)",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(drainStatement);
    policies.bgp_policy_statements()->emplace_back(undrainStatement);

    setPolicyConfig(policies);
  }

  /**
   * Create PeerManagerBase and BgpServiceBB instance for runtime API calls.
   * This extends the base createPeerManager to also create the service layer.
   * Both PeerManagerBase and BgpServiceBB share the same ConfigManager so that
   * config version tracking is consistent across policy updates.
   */
  void createPeerManagerWithBgpService(bool enableUpdateGroup = false) {
    createPeerManager(
        enableUpdateGroup,
        /*enableEgressBackpressure=*/true);

    watchdog_ = std::make_unique<Watchdog>(config_);
    bgpService_ = std::make_shared<BgpServiceBB>(
        *peerManager_,
        configManager_,
        *rib_,
        *watchdog_,
        /*nlWrapper=*/nullptr,
        false);
    bgpClient_ = apache::thrift::makeTestClient<
        apache::thrift::Client<neteng::fboss::bgp::thrift::TBgpService>>(
        bgpService_);
  }

  /**
   * Bring up all 3 peers and complete EoR exchange.
   */
  void bringUpAllPeersWithEor() {
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);

    BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
    BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

    sendEoRToPeer(peerId3);
    sendEoRToPeer(peerId4);
    sendEoRToPeer(peerId5);

    EXPECT_TRUE(waitForEoR(peerId3));
    EXPECT_TRUE(waitForEoR(peerId4));
    EXPECT_TRUE(waitForEoR(peerId5));
  }

  /**
   * Inject a test route from peer3 and verify it's propagated.
   */
  void injectAndVerifyTestRoute() {
    addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

    auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
    EXPECT_TRUE(waitForRouteInShadowRib(prefix));
    EXPECT_TRUE(
        verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));
  }

  /**
   * Helper to call setPeersPolicy via BgpServiceBB
   */
  neteng::fboss::bgp::thrift::BgpPolicyChangeResult setPeerPolicy(
      const std::string& peerAddr,
      const std::string& policyName,
      bgp_policy::DIRECTION direction = bgp_policy::DIRECTION::OUT) {
    std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
        peersPolicy;
    peersPolicy[peerAddr][direction] = policyName;

    return folly::coro::blockingWait(
        bgpClient_->co_setPeersPolicy(peersPolicy));
  }

  /**
   * Helper to call unsetPeersPolicy via BgpServiceBB
   */
  neteng::fboss::bgp::thrift::BgpPolicyChangeResult unsetPeerPolicy(
      const std::string& peerAddr,
      bgp_policy::DIRECTION direction = bgp_policy::DIRECTION::OUT) {
    std::map<std::string, std::set<bgp_policy::DIRECTION>> peersToUnset;
    peersToUnset[peerAddr].insert(direction);

    return folly::coro::blockingWait(
        bgpClient_->co_unsetPeersPolicy(peersToUnset));
  }

  std::unique_ptr<Watchdog> watchdog_;
  std::shared_ptr<BgpServiceBB> bgpService_;
  std::unique_ptr<
      apache::thrift::Client<neteng::fboss::bgp::thrift::TBgpService>>
      bgpClient_;
};

/**
 * Test: SetPeersPolicyRejectsBeforeInitialization
 *
 * Verify that setPeersPolicy rejects the API call when BGP is not
 * initialized (before peers are brought up and EoR is exchanged).
 */
TEST_F(
    E2EDynamicPolicyEvaluationTest,
    SetPeersPolicyRejectsBeforeInitialization) {
  auto result = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result, neteng::fboss::bgp::thrift::BgpPolicyChangeResult::INPUT_ERROR);
}

/**
 * Test: SetPeerGroupsPolicyRejectsBeforeInitialization
 *
 * Verify that setPeerGroupsPolicy rejects the API call when BGP is not
 * initialized (before peers are brought up and EoR is exchanged).
 */
TEST_F(
    E2EDynamicPolicyEvaluationTest,
    SetPeerGroupsPolicyRejectsBeforeInitialization) {
  std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
      peerGroupsPolicy;
  peerGroupsPolicy["test-peer-group-1"][bgp_policy::DIRECTION::OUT] =
      kDrainPolicyName;

  auto result = folly::coro::blockingWait(
      bgpClient_->co_setPeerGroupsPolicy(peerGroupsPolicy));
  EXPECT_EQ(
      result, neteng::fboss::bgp::thrift::BgpPolicyChangeResult::INPUT_ERROR);
}

/**
 * Test: SetPeersPolicyDrainsRoutes
 *
 * Verify that applying a DENY policy to a peer via setPeersPolicy causes
 * routes to be withdrawn from that peer via policy re-evaluation.
 *
 * Sequence:
 * T0: Inject routes from peer3, verify propagation to peer5
 * T1: setPeersPolicy(peer5, DRAIN_POLICY) - apply DENY egress policy
 * T2: Verify routes are withdrawn from peer5
 */
TEST_F(E2EDynamicPolicyEvaluationTest, SetPeersPolicyDrainsRoutes) {
  bringUpAllPeersWithEor();

  injectAndVerifyTestRoute();

  auto result = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

/**
 * Test: UnsetPeersPolicyRestoresRoutes
 *
 * Verify that unsetting a peer-level policy causes routes to be restored
 * when the peer was previously drained.
 *
 * Sequence:
 * T0: Inject routes, verify propagation
 * T1: setPeersPolicy(peer5, DRAIN_POLICY) - drain peer5 at peer level
 * T2: Verify peer5 routes withdrawn
 * T3: unsetPeersPolicy(peer5) - remove peer-level policy
 * T4: Verify peer5 routes restored
 */
TEST_F(E2EDynamicPolicyEvaluationTest, UnsetPeersPolicyRestoresRoutes) {
  bringUpAllPeersWithEor();

  injectAndVerifyTestRoute();

  auto result1 = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result1,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  auto result2 = unsetPeerPolicy(kPeerAddr5.str());
  EXPECT_EQ(
      result2,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));
}

/**
 * Test: DrainUndrainCyclePreservesRoutes
 *
 * Verify the complete drain/undrain cycle works correctly.
 * Routes should be withdrawn on drain and restored on undrain.
 *
 * Sequence:
 * T0: Inject routes, verify propagation
 * T1: Drain peer5 via setPeersPolicy
 * T2: Verify routes withdrawn
 * T3: Undrain peer5 via setPeersPolicy (change to ACCEPT policy)
 * T4: Verify routes restored
 */
TEST_F(E2EDynamicPolicyEvaluationTest, DrainUndrainCyclePreservesRoutes) {
  bringUpAllPeersWithEor();

  injectAndVerifyTestRoute();

  auto result1 = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result1,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  auto result2 = setPeerPolicy(kPeerAddr5.str(), kUndrainPolicyName);
  EXPECT_EQ(
      result2,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));
}

/**
 * Test: MultiplePeersDrainIndependently
 *
 * Verify that draining one peer does not affect other peers.
 * Each peer should be drained/undrained independently.
 *
 * Sequence:
 * T0: Inject routes, verify propagation to peer4 and peer5
 * T1: Drain peer5 only
 * T2: Verify peer5 routes withdrawn, peer4 still receiving routes
 */
TEST_F(E2EDynamicPolicyEvaluationTest, MultiplePeersDrainIndependently) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str()));
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));

  auto result = setPeerPolicy(kPeerAddr5.str(), kDrainPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  EXPECT_TRUE(
      verifyRouteAdd("v4", "20.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str()));
}
/**
 * E2EUpdateGroupEoRIterationTest
 *
 * Tests that EoR is sent exactly once per peer, even when one peer is blocked
 * during EoR distribution and another peer flaps (goes down and comes back up).
 *
 * Uses update groups with egress backpressure and the ACCEPT egress policy.
 */
class E2EUpdateGroupEoRIterationTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    setupPolicies();

    /* v4-only peers so EoR is a single message per peer */
    auto spec3 = kDefaultPeerSpec3;
    spec3.egressPolicyName = kUndrainPolicyName;
    spec3.disableIpv6Afi = true;
    auto spec4 = kDefaultPeerSpec4;
    spec4.egressPolicyName = kUndrainPolicyName;
    spec4.disableIpv6Afi = true;
    auto spec5 = kDefaultPeerSpec5;
    spec5.egressPolicyName = kUndrainPolicyName;
    spec5.disableIpv6Afi = true;

    addPeer(spec3);
    addPeer(spec4);
    addPeer(spec5);

    /* Add local routes before createRib so they're part of the initial RIB
     * computation and present in shadow RIB for group initial dumps. */
    addLocalRoute("10.0.0.0/8", {"100:1"}, 100);
    addLocalRoute("20.0.0.0/8", {"200:1"}, 150);

    createRib();
    /* Peer4 gets a small queue (highWm=2) so both routes fit but EoR
     * blocks on backpressure. Other peers use default queue sizes. */
    setQueueSizeForPeer(kPeerAddr4, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
    createPeerManager(
        /*enableUpdateGroup=*/true, /*enableEgressBackpressure=*/true);
  }

  void setupPolicies() {
    auto undrainStatement = createBgpPolicyStatement(
        kUndrainPolicyName,
        {createBgpPolicyTerm(
            "accept-all",
            "Accept all routes",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(undrainStatement);
    setPolicyConfig(policies);
  }
};

/**
 * Test: BlockedPeerReceivesEoROnceAfterPeerFlap
 *
 * Verifies that when peer4 is blocked during EoR distribution and peer5
 * flaps (down + up), EoR is sent exactly once to each peer.
 *
 * Sequence:
 * T0: Bring up peer3 (source), peer4, peer5. Inject pfx1.
 * T1: peer4 receives pfx1, then blocks (queue full at highWm=1).
 *     EoR cannot be pushed to peer4 yet.
 * T2: Bring down peer5, then bring it back up.
 * T3: peer5 receives pfx1 + EoR from its rib walk.
 * T4: Unblock peer4. Inject pfx2.
 * T5: Verify peer4 messages in order: [UPDATE(pfx1), EoR, UPDATE(pfx2)].
 *     Verify peer5 messages: [UPDATE(pfx2)] only — no second EoR.
 */
TEST_F(
    E2EUpdateGroupEoRIterationTest,
    BlockedPeerReceivesEoROnceAfterPeerFlap) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Send EoR after all peers are up so they all join the group before
   * the initial dump runs. The local route (added in SetUp before
   * createRib) is already in RIB when the initial path computation
   * executes. */
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Drain peer3 and peer5 to see which ones have received EoR.
   * Peer4's queue is full (highWm=2) so the group's EoR distribution
   * coroutine is suspended on peer4. Depending on iteration order,
   * peer3 and/or peer5 may or may not have received EoR yet. */
  size_t p3Updates = 0, p3EoRs = 0;
  drainAndClassifyMessages(peerId3, p3Updates, p3EoRs);
  XLOGF(
      INFO,
      "After initial dump: peer3 got {} updates, {} EoRs",
      p3Updates,
      p3EoRs);

  // Don't drain peer4 — its small queue (highWm=2) should block on EoR

  size_t p5Updates = 0, p5EoRs = 0;
  drainAndClassifyMessages(peerId5, p5Updates, p5EoRs);
  XLOGF(
      INFO,
      "After initial dump: peer5 got {} updates, {} EoRs",
      p5Updates,
      p5EoRs);

  /* Flap peers that haven't received EoR yet, then verify they
   * received both routes + EoR from their rib walk after reconnect. */
  if (p3EoRs == 0) {
    bringDownPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr3, /*versionNumber=*/2);
    sendEoRToPeer(peerId3);

    size_t p3ReconnUpdates = 0, p3ReconnEoRs = 0;
    drainAndClassifyMessages(peerId3, p3ReconnUpdates, p3ReconnEoRs);
    EXPECT_EQ(p3ReconnUpdates, 2)
        << "Peer3 should have received 2 routes after reconnect";
    EXPECT_EQ(p3ReconnEoRs, 1)
        << "Peer3 should have received 1 EoR after reconnect";
  }
  if (p5EoRs == 0) {
    bringDownPeer(kPeerAddr5);
    bringUpPeer(kPeerAddr5, /*versionNumber=*/2);
    sendEoRToPeer(peerId5);

    size_t p5ReconnUpdates = 0, p5ReconnEoRs = 0;
    drainAndClassifyMessages(peerId5, p5ReconnUpdates, p5ReconnEoRs);
    EXPECT_EQ(p5ReconnUpdates, 2)
        << "Peer5 should have received 2 routes after reconnect";
    EXPECT_EQ(p5ReconnEoRs, 1)
        << "Peer5 should have received 1 EoR after reconnect";
  }

  /* Unblock peer4 — drain its queue and verify 2 routes + 1 EoR */
  size_t p4Updates = 0, p4EoRs = 0;
  drainAndClassifyMessages(peerId4, p4Updates, p4EoRs);
  EXPECT_EQ(p4Updates, 2) << "Peer4 should have received 2 routes";
  EXPECT_EQ(p4EoRs, 1) << "Peer4 should have received exactly 1 EoR";

  /* Inject a third route at runtime */
  injectLocalRoutesAtRuntime({"30.0.0.0/8"}, {"300:1"}, 200);
  auto prefix3 = folly::IPAddress::createNetwork("30.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix3));

  /* Verify flapped peers receive the third route but NO extra EoR */
  if (p3EoRs == 0) {
    size_t p3FinalUpdates = 0, p3FinalEoRs = 0;
    drainAndClassifyMessages(peerId3, p3FinalUpdates, p3FinalEoRs);
    EXPECT_EQ(p3FinalUpdates, 1)
        << "Peer3 should have received the third route";
    EXPECT_EQ(p3FinalEoRs, 0) << "Peer3 should NOT have received extra EoR";
  }
  if (p5EoRs == 0) {
    size_t p5FinalUpdates = 0, p5FinalEoRs = 0;
    drainAndClassifyMessages(peerId5, p5FinalUpdates, p5FinalEoRs);
    EXPECT_EQ(p5FinalUpdates, 1)
        << "Peer5 should have received the third route";
    EXPECT_EQ(p5FinalEoRs, 0) << "Peer5 should NOT have received extra EoR";
  }

  /* Peer4 should also receive the third route with no extra EoR */
  size_t p4FinalUpdates = 0, p4FinalEoRs = 0;
  drainAndClassifyMessages(peerId4, p4FinalUpdates, p4FinalEoRs);
  EXPECT_EQ(p4FinalUpdates, 1) << "Peer4 should have received the third route";
  EXPECT_EQ(p4FinalEoRs, 0) << "Peer4 should NOT have received extra EoR";
}

/**
 * E2EDynamicPolicyEvaluationUpdateGroupTest
 *
 * Same as E2EDynamicPolicyEvaluationTest but with update groups enabled.
 * Tests behavior specific to update group peer movement during policy changes.
 */
class E2EDynamicPolicyEvaluationUpdateGroupTest
    : public E2EDynamicPolicyEvaluationTest {
 protected:
  void SetUp() override {
    setupPolicies();

    /* All peers start with the ACCEPT egress policy so they share an
     * update group. Applying TAG to one peer later triggers a group move. */
    auto spec3 = kDefaultPeerSpec3;
    spec3.egressPolicyName = kUndrainPolicyName;
    auto spec4 = kDefaultPeerSpec4;
    spec4.egressPolicyName = kUndrainPolicyName;
    auto spec5 = kDefaultPeerSpec5;
    spec5.egressPolicyName = kUndrainPolicyName;

    addPeer(spec3);
    addPeer(spec4);
    addPeer(spec5);

    createRib();
    createPeerManagerWithBgpService(/*enableUpdateGroup=*/true);
  }

  void setupPolicies() {
    auto undrainStatement = createBgpPolicyStatement(
        kUndrainPolicyName,
        {createBgpPolicyTerm(
            "accept-all",
            "Accept all routes",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    /* TAG policy: permits routes but attaches a community, causing
     * a different UpdateGroupKey and triggering a group move. */
    auto tagStatement = createBgpPolicyStatement(
        kTagPolicyName,
        {createBgpPolicyTerm(
            "tag-all",
            "Accept and tag with community",
            {},
            {createBgpPolicyCommunityAction(
                 bgp_policy::CommunityActionType::ADD, {kTagCommunity}),
             createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(undrainStatement);
    policies.bgp_policy_statements()->emplace_back(tagStatement);

    setPolicyConfig(policies);
  }
};

/**
 * Test: GroupMoveRibWalkDoesNotResendEoR
 *
 * When a per-peer policy change moves a peer to a new update group,
 * processRibDumpForGroup walks the ShadowRib for the new group. This should
 * NOT re-send EoR markers since the peer already sent them in the old group.
 *
 * Sequence:
 * T0: Bring up peer3 (source), peer4 and peer5 (egress). Inject route, EoR.
 * T1: Apply TAG policy to peer5 — permits routes with added community,
 *     triggers group move to a new update group.
 * T2: Verify peer5 receives the first route (re-evaluated with TAG policy).
 * T3: Inject a second route.
 * T4: Verify peer5 receives the second route but does NOT receive EoR.
 */
TEST_F(
    E2EDynamicPolicyEvaluationUpdateGroupTest,
    GroupMoveRibWalkDoesNotResendEoR) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));

  /* Inject first route from peer3 and verify propagation to peer5 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));

  /* Apply TAG policy to peer5 — triggers group move */
  auto result = setPeerPolicy(kPeerAddr5.str(), kTagPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  /* Inject a second route so peer5 has two routes to receive */
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  /*
   * Drain peer5's queue and classify all messages. After the group move,
   * peer5 should receive route updates (re-evaluated first route + second
   * route) but NO EoR — the peer already sent EoR in the old group.
   */
  size_t updateCount = 0;
  size_t eorCount = 0;
  drainAndClassifyMessages(peerId5, updateCount, eorCount);

  EXPECT_EQ(updateCount, 2)
      << "Peer5 should have received exactly 2 route updates";
  EXPECT_EQ(eorCount, 0)
      << "Peer5 should NOT have received EoR after group move";
}

/**
 * Test: SecondPeerMovesToExistingGroupWithoutEoR
 *
 * After peer5 moves to a new group via TAG policy, apply the same TAG policy
 * to peer4 so it joins peer5's group. Verify peer4 also does not receive a
 * spurious EoR when it moves.
 *
 * Sequence:
 * T0: Bring up peers, inject first route, complete EoR exchange.
 * T1: Apply TAG policy to peer5 — moves to new group.
 * T2: Apply TAG policy to peer4 — moves to peer5's group.
 * T3: Inject a third route.
 * T4: Drain peer4's queue — verify 3 route updates (re-evaluated first +
 *     second from rib walk, plus third via CL) and 0 EoRs.
 */
TEST_F(
    E2EDynamicPolicyEvaluationUpdateGroupTest,
    SecondPeerMovesToExistingGroupWithoutEoR) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));

  /* Inject first route */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str()));
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));

  /* Move peer5 to a new group via TAG policy */
  auto result5 = setPeerPolicy(kPeerAddr5.str(), kTagPolicyName);
  EXPECT_EQ(
      result5,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  /* Inject second route before moving peer4 */
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  /* Drain peer5's queue to confirm it moved successfully */
  size_t p5Updates = 0;
  size_t p5EoRs = 0;
  drainAndClassifyMessages(peerId5, p5Updates, p5EoRs);
  EXPECT_EQ(p5Updates, 2);
  EXPECT_EQ(p5EoRs, 0);

  /* Move peer4 to peer5's group via the same TAG policy */
  auto result4 = setPeerPolicy(kPeerAddr4.str(), kTagPolicyName);
  EXPECT_EQ(
      result4,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  /* Inject a third route so peer4 has something to receive via CL */
  addRoute("v4", "30.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix3 = folly::IPAddress::createNetwork("30.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix3));

  /*
   * Drain peer4's queue. It should receive:
   * - 2 route updates from rib walk (re-evaluated first + second routes)
   * - 1 route update from CL (third route)
   * - 0 EoRs
   */
  size_t p4Updates = 0;
  size_t p4EoRs = 0;
  drainAndClassifyMessages(peerId4, p4Updates, p4EoRs);

  EXPECT_EQ(p4Updates, 3)
      << "Peer4 should have received exactly 3 route updates";
  EXPECT_EQ(p4EoRs, 0) << "Peer4 should NOT have received EoR after group move";
}

} // namespace facebook::bgp
