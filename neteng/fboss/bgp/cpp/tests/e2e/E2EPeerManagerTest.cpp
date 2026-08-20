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

/*
 * E2E tests for PeerManagerBase session lifecycle.
 *
 * These tests verify peer session establishment, termination, and restart
 * scenarios through the complete BGP pipeline.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "fb303/ThreadCachedServiceData.h"
#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2EPeerManagerTest : public E2ERibTestFixture {};

struct VipAdjRibCleanupCase {
  uint32_t remoteAs;
  bool dynamic;
  bool expectCleanup;
  bool expectVipSession;
  const char* name;
};

class VipAdjRibCleanupTest
    : public E2ETestFixture,
      public testing::WithParamInterface<VipAdjRibCleanupCase> {
 protected:
  void SetUp() override {
    RibStats::setAdjRibCount(0);
    BgpStats::setRunningVipSessions(0);
    if (GetParam().dynamic) {
      peers_.push_back(createBgpPeer(
          GetParam().remoteAs,
          kLocalAddr2,
          kPeerPrefix4,
          kNextHopV4_2,
          kNextHopV6_2,
          true,
          kPeerTypeShiv));
      peerAddr_ = kDynamicPeerAddr4;
    } else {
      auto peer = kDefaultPeerSpec3;
      peer.asn = GetParam().remoteAs;
      addPeer(peer);
      peerAddr_ = kPeerAddr3;
    }
    createRib();
    createPeerManager();
  }

  folly::IPAddress peerAddr_{kPeerAddr3};
};

TEST_P(VipAdjRibCleanupTest, SessionDisconnectUpdatesTotalAdjRibs) {
  auto* stats = facebook::fb303::ThreadCachedServiceData::get();
  stats->publishStats();
  const auto adjRibsBefore = stats->getCounter(RibStats::kTotalAdjRibs);

  bringUpPeer(peerAddr_);
  auto adjRib = getAdjRibByAddr(peerAddr_);
  ASSERT_NE(nullptr, adjRib);
  stats->publishStats();
  const auto adjRibsAfterEstablishment =
      stats->getCounter(RibStats::kTotalAdjRibs);
  ASSERT_EQ(adjRibsBefore + 1, adjRibsAfterEstablishment);
  EXPECT_EQ(
      GetParam().expectVipSession ? 1 : 0,
      stats->getCounter(BgpStats::kRunningVipSessions));

  bringDownPeer(peerAddr_);
  stats->publishStats();
  EXPECT_EQ(0, stats->getCounter(BgpStats::kRunningVipSessions));

  if (GetParam().expectCleanup) {
    EXPECT_EQ(nullptr, getAdjRibByAddr(peerAddr_));
    EXPECT_EQ(adjRibsBefore, stats->getCounter(RibStats::kTotalAdjRibs));
  } else {
    EXPECT_EQ(adjRib, getAdjRibByAddr(peerAddr_));
    EXPECT_EQ(
        adjRibsAfterEstablishment, stats->getCounter(RibStats::kTotalAdjRibs));
  }
}

INSTANTIATE_TEST_SUITE_P(
    RemoteAs,
    VipAdjRibCleanupTest,
    testing::Values(
        VipAdjRibCleanupCase{
            .remoteAs = kVipAsn,
            .dynamic = true,
            .expectCleanup = true,
            .expectVipSession = true,
            .name = "DynamicVip"},
        VipAdjRibCleanupCase{
            .remoteAs = kVipAsn,
            .dynamic = false,
            .expectCleanup = false,
            .expectVipSession = false,
            .name = "StaticPeerWithVipAsn"},
        VipAdjRibCleanupCase{
            .remoteAs = kPeerAsn3,
            .dynamic = false,
            .expectCleanup = false,
            .expectVipSession = false,
            .name = "NonVip"}),
    [](const testing::TestParamInfo<VipAdjRibCleanupCase>& info) {
      return info.param.name;
    });

/*
 * Fixture that intentionally does NOT add peer3 — used to exercise delPeers
 * against a peer ID that was never registered.
 */
class E2EDeleteUnknownPeerTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    createRib();
    createPeerManager();
  }
};

/*
 * Test: Peer session establishment
 *
 * Scenario:
 * - Bring up peer session
 * - Send EoR
 * - Verify session is established and routes can be exchanged
 */
TEST_F(E2EPeerManagerTest, SessionEstablishment) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId5);

  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId5));

  // Announce route and verify it's propagated
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Peer session termination
 *
 * Scenario:
 * - Establish session and exchange routes
 * - Bring down peer
 * - Verify routes are withdrawn from other peers
 */
TEST_F(E2EPeerManagerTest, SessionTermination) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId5));

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  // Bring down peer3 - routes should be withdrawn
  bringDownPeer(kPeerAddr3);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

/*
 * Test: Peer session restart (Graceful Restart scenario)
 *
 * Scenario:
 * - Session established with routes
 * - Peer goes down and comes back up
 * - Routes should be re-established
 */
TEST_F(E2EPeerManagerTest, SessionRestart) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId5));

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  // Restart peer3
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  // Bring peer back up and re-announce route
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  // Use batch verification for re-announcement after peer restart
  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, expectedRoutes));
}

/*
 * Test: Multiple peer sessions
 *
 * Scenario:
 * - Establish multiple peer sessions
 * - Verify routes are correctly propagated to all peers
 */
TEST_F(E2EPeerManagerTest, MultiplePeerSessions) {
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

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  // Route should be advertised to both Peer4 and Peer5
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Rapid session flap with version compression (S619541 regression guard)
 *
 * Reproduces the exact failure scenario from S619541:
 *  1. Session DOWN  -> AdjRib loops exit -> baton posted
 *  2. Session UP (stale version) -> waitForSessionTerminateBaton() passes
 *     -> version check fails -> early return (baton NOT reset)
 *  3. Session UP (valid version) -> waitForSessionTerminateBaton() must
 *     pass again (baton still posted due to latch semantics) -> proceeds
 *
 * With the old BatchSemaphore (D91101949):
 *  Step 2: wait(2) CONSUMES 2 tokens
 *  Step 3: wait(2) -> 0 tokens -> HANGS FOREVER -> S619541
 *
 * With folly::coro::Baton (latch semantics):
 *  Step 2: co_await *baton passes through -> early return leaves baton posted
 *  Step 3: co_await *baton passes through again -> version check passes
 */
TEST_F(E2EPeerManagerTest, RapidSessionFlapWithVersionCompressionE2e) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId5));

  // Announce route and verify propagation
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  // Step 1: Bring down peer3 -- AdjRib loops terminate, baton posted via
  // postTerminateBaton()
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  // Step 2: Dispatch a STALE sessionEstablished event.
  // PeerManagerBase will wait on the baton (passes -- still posted from step
  // 1), detect the version mismatch, and return early WITHOUT resetting the
  // baton.
  dispatchStaleSessionEstablished(kPeerAddr3);

  // Step 3: Bring peer3 back up with a VALID version.
  // With latch semantics (folly::coro::Baton), the baton is still posted
  // from step 1 (step 2's early return did NOT reset it), so
  // waitForSessionTerminateBaton() passes through immediately.
  // With the old BatchSemaphore, wait(2) in step 2 consumed the tokens,
  // so this wait(2) would HANG FOREVER -- the S619541 bug.
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  // Verify routes still work after the stale event + re-establishment
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: peerDelete=true on the IDLE ObservableStateT triggers
 * cleanupPeerState end-to-end through PeerManagerBase::sessionTerminated.
 *
 * Mirrors the production path that delPeers initiates:
 *   delPeers → co_dropPeer(peerDelete=true) → BgpSessionStop{peerDelete}
 *     → FSM IDLE event → sessionTerminated co_awaits cleanupPeerState
 *       → AdjRib erased from adjRibs_
 *
 * E2E uses MockSessionManager (no real FSM), so we synthesize the IDLE
 * event with peerDelete=true via bringDownPeer(addr, /*peerDelete=*\/true)
 * to drive the same PeerManagerBase-side logic. After cleanup the peer can be
 * brought back up on the same address with a fresh AdjRib instance,
 * proving no stale state leaked.
 */
TEST_F(E2EPeerManagerTest, PeerDeleteTriggersCleanupAndAllowsReBringup) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId5));

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  auto oldAdjRib = getAdjRibByAddr(kPeerAddr3);
  ASSERT_NE(nullptr, oldAdjRib);

  // bringDownPeer(addr, peerDelete=true) dispatches an IDLE event with
  // peerDelete=true to PeerManagerBase::sessionTerminated, which co_awaits
  // cleanupPeerState inline. By the time bringDownPeer returns the
  // AdjRib must be erased.
  bringDownPeer(kPeerAddr3, /*peerDelete=*/true);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
  EXPECT_EQ(nullptr, getAdjRibByAddr(kPeerAddr3))
      << "AdjRib for peer3 was not erased after bringDownPeer with peerDelete";

  // Re-bringup on the same address — PeerManagerBase creates a fresh AdjRib
  // whose identity must differ from the pre-cleanup one.
  bringUpPeer(kPeerAddr3, /*versionNumber=*/2);
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  auto newAdjRib = getAdjRibByAddr(kPeerAddr3);
  ASSERT_NE(nullptr, newAdjRib);
  EXPECT_NE(oldAdjRib, newAdjRib)
      << "Re-bringup must create a fresh AdjRib instance";

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: delPeers fallback for previously-established-now-IDLE peers.
 *
 * Reproduces the IDLE-peer scenario the fallback in delPeers handles:
 * peer was established, session went down naturally (sessionTerminated
 * ran with peerDelete=false -> markStateTerminated, baton posted, AdjRib
 * stays in adjRibs_), then operator calls delPeers.
 *
 * Without the fallback, shutdownPeer skips peer->stop() (activeSessionInfo
 * is null) → no BgpSessionStop event -> sessionTerminated never runs ->
 * cleanupPeerState never runs -> leak.
 *
 * With the fallback, delPeers detects !isStateEstablished and drives
 * cleanupPeerState directly. The previously-posted baton (latch
 * semantics) makes cleanupPeerState's wait pass through immediately.
 */
TEST_F(E2EPeerManagerTest, IdlePeerDelPeers_FallbackCleansUp) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId5));

  // Natural session-down — AdjRib stays, isStateEstablished flips false.
  bringDownPeer(kPeerAddr3);
  ASSERT_NE(nullptr, getAdjRibByAddr(kPeerAddr3))
      << "AdjRib must remain after natural session-down (no peerDelete)";

  // Now delPeers — fallback fires because !isStateEstablished, runs
  // cleanupPeerState inline. Baton was posted in bringDownPeer; latch
  // semantics let cleanupPeerState's wait pass through.
  auto delResult = delPeerAtRuntime(kPeerAddr3);
  ASSERT_TRUE(delResult.hasValue());

  facebook::fboss::checkWithRetry(
      [&]() { return getAdjRibByAddr(kPeerAddr3) == nullptr; },
      /*retries=*/30,
      /*msBetweenRetry=*/std::chrono::milliseconds(100),
      "AdjRib for peer3 was not erased after delPeers fallback");
}

/*
 * Counterpart: bringDownPeer with peerDelete=false (the natural
 * session-down path, no delPeers) leaves the AdjRib in adjRibs_ for the
 * next session establishment. Guards against the consumer firing on
 * unrelated terminations.
 */
TEST_F(E2EPeerManagerTest, NoPeerDeleteKeepsAdjRibForReEstablish) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId5));

  auto adjRibBefore = getAdjRibByAddr(kPeerAddr3);
  ASSERT_NE(nullptr, adjRibBefore);

  // Default peerDelete=false — sessionTerminated should NOT erase.
  bringDownPeer(kPeerAddr3);

  EXPECT_NE(nullptr, getAdjRibByAddr(kPeerAddr3))
      << "AdjRib for peer3 must be retained when peerDelete=false";
}

/*
 * Test: delPeers on a peer that was never added is a no-op.
 *
 * Scenario:
 * - Fixture adds peer4 and peer5 only; peer3 is never registered.
 * - Calling delPeerAtRuntime(kPeerAddr3) routes through
 *   PeerManagerBase::delPeers, which catches the PEER_DOES_NOT_EXIST error
 *   from sessionMgr_->co_dropPeer and returns success (folly::unit).
 * - No AdjRib should be created for the unknown peer.
 * - Shadow RIB should remain empty for the control prefix.
 * - A second delPeerAtRuntime call must also succeed (idempotency).
 */
TEST_F(E2EDeleteUnknownPeerTest, DeleteUnknownPeerNoOp) {
  auto delResult = delPeerAtRuntime(kPeerAddr3);
  ASSERT_TRUE(delResult.hasValue())
      << "delPeers on unknown peer should return success "
         "(PEER_DOES_NOT_EXIST is caught and skipped)";

  EXPECT_EQ(nullptr, getAdjRibByAddr(kPeerAddr3))
      << "delPeers on never-added peer must not create any AdjRib";

  EXPECT_TRUE(
      verifyRouteNotInShadowRib(folly::IPAddress::createNetwork("10.0.0.0/8")));

  auto delResult2 = delPeerAtRuntime(kPeerAddr3);
  ASSERT_TRUE(delResult2.hasValue())
      << "Second delPeers on unknown peer must also succeed (idempotent)";
  EXPECT_EQ(nullptr, getAdjRibByAddr(kPeerAddr3));
}

} // namespace facebook::bgp
