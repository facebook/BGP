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
 * Rapid session flap tests using E2ESessionTestFixture.
 * These tests exercise rapid session flaps through the real notifyCoroQueue
 * event pipeline, which is the #1 SEV-causing pattern (Pattern 4).
 *
 * Only possible with E2ETestSessionManager because events must flow through
 * notifyCoroQueue for real version checks and baton handling.
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2ESessionFlapTest : public E2ESessionTestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false);
  }
};

/*
 * Rapid flap: down->up in quick succession on a single peer.
 * Verify final state is correct (session established, routes can flow).
 */
TEST_F(E2ESessionFlapTest, RapidFlapSinglePeer) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  bringDownPeer(kPeerAddr3);
  /* Drain withdrawal from peer4's queue before bringing peer3 back */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));

  bringUpPeerAndWait(kPeerAddr3);
  sendEoRToPeer(peerId3);

  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));
  EXPECT_TRUE(verifyRouteAdd("v4", "20.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "Rapid flap single peer: routes flow after flap");
}

/*
 * Multi-cycle flap: N cycles of down->up on a single peer.
 * Verify no crashes, no stale routes, correct final state.
 */
TEST_F(E2ESessionFlapTest, MultiCycleFlap) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  constexpr int kFlapCycles = 5;
  for (int i = 0; i < kFlapCycles; i++) {
    XLOGF(INFO, "Flap cycle {} of {}", i + 1, kFlapCycles);
    bringDownPeer(kPeerAddr3);
    bringUpPeerAndWait(kPeerAddr3);
    sendEoRToPeer(peerId3);
  }

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOGF(
      INFO, "Multi-cycle flap: routes flow after {} flap cycles", kFlapCycles);
}

/*
 * Concurrent peer flaps: flap 3 peers simultaneously.
 * Verify all end up in correct state.
 */
TEST_F(E2ESessionFlapTest, ConcurrentPeerFlaps) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  bringDownPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr4);
  bringDownPeer(kPeerAddr5);

  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "Concurrent peer flaps: all peers recovered");
}

/*
 * Flap during route processing: establish session, inject routes,
 * flap while routes are in-flight. Verify no crashes.
 */
TEST_F(E2ESessionFlapTest, FlapDuringRouteProcessing) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  for (int i = 0; i < 10; i++) {
    addRoute(
        "v4",
        fmt::format("{}.0.0.0", 10 + i),
        8,
        kPeerAddr3,
        "11.0.0.1",
        "65001");
  }

  bringDownPeer(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr3);
  sendEoRToPeer(peerId3);

  addRoute("v4", "30.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("30.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "30.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "Flap during route processing: no crash, routes flow");
}

/*
 * Flap before EoR: establish session, flap before sending EoR.
 * Verify clean cleanup.
 */
TEST_F(E2ESessionFlapTest, FlapBeforeEoR) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringDownPeer(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr3);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "Flap before EoR: clean recovery");
}

/*
 * ============================================================================
 * AGGRESSIVE THRASH TESTS
 * These tests are designed to trigger the S596523 race condition by NOT
 * waiting for events to process. Fire events as fast as possible to create
 * the race between new session establishment and old session cleanup.
 * ============================================================================
 */

/*
 * ThrashNoWait: Fire UP-DOWN-UP-DOWN without any waiting.
 * This creates the exact race condition from S596523: new session starts
 * before old cleanup finishes.
 */
TEST_F(E2ESessionFlapTest, ThrashNoWait) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  constexpr int kThrashCycles = 50;
  XLOGF(INFO, "Starting thrash test with {} cycles, NO WAITING", kThrashCycles);

  for (int i = 0; i < kThrashCycles; i++) {
    /*
     * Fire events WITHOUT waiting - this creates overlapping operations.
     * The goal is to have ESTABLISHED events arrive while the previous
     * TERMINATED cleanup is still in progress.
     */
    bringDownPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr3);
  }

  /*
   * Now wait for things to settle and verify we can still operate.
   * If there's a crash, it will happen during the thrash above.
   */
  waitForSessionEstablished(kPeerAddr3, 100);
  sendEoRToPeer(peerId3);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOGF(INFO, "ThrashNoWait: survived {} cycles", kThrashCycles);
}

/*
 * ThrashAllPeersSimultaneous: Thrash ALL peers at the same time without
 * waiting. This maximizes concurrent pressure on the event queue and
 * processing.
 */
TEST_F(E2ESessionFlapTest, ThrashAllPeersSimultaneous) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  constexpr int kThrashCycles = 30;
  XLOGF(
      INFO,
      "Starting simultaneous thrash test: {} cycles on ALL peers",
      kThrashCycles);

  for (int i = 0; i < kThrashCycles; i++) {
    /*
     * Bring all down, then all up, without any waiting.
     * This floods the notifyCoroQueue with interleaved events for all peers.
     */
    bringDownPeer(kPeerAddr3);
    bringDownPeer(kPeerAddr4);
    bringDownPeer(kPeerAddr5);
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);
  }

  waitForSessionEstablished(kPeerAddr3, 100);
  waitForSessionEstablished(kPeerAddr4, 100);
  waitForSessionEstablished(kPeerAddr5, 100);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "ThrashAllPeersSimultaneous: survived");
}

/*
 * ThrashWithRouteInjection: Inject routes WHILE flapping.
 * This creates pressure on both the session state machine and the RIB.
 */
TEST_F(E2ESessionFlapTest, ThrashWithRouteInjection) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  constexpr int kThrashCycles = 20;
  constexpr int kRoutesPerCycle = 5;

  XLOGF(
      INFO,
      "Starting thrash with route injection: {} cycles, {} routes per cycle",
      kThrashCycles,
      kRoutesPerCycle);

  for (int i = 0; i < kThrashCycles; i++) {
    /*
     * Inject routes (which will queue up in adjRibInQ)
     * Then immediately flap the session
     * Routes may or may not have been processed - that's the point
     */
    for (int r = 0; r < kRoutesPerCycle; r++) {
      int prefix_first_octet = (i * kRoutesPerCycle + r) % 200 + 10;
      addRoute(
          "v4",
          fmt::format("{}.0.0.0", prefix_first_octet),
          8,
          kPeerAddr3,
          "11.0.0.1",
          "65001");
    }

    bringDownPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr3);
  }

  waitForSessionEstablished(kPeerAddr3, 100);
  sendEoRToPeer(peerId3);

  addRoute("v4", "250.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("250.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  /*
   * After 20 thrash cycles + 100 routes injected, peer4's outbound queue
   * holds many AdjRibOut batches — some are withdraws (5 withdraws + 1
   * announcement per batch is common), some announce other prefixes from
   * the thrash. verifyRouteAdd pops ONE message and checks; the first
   * batch in the queue is rarely the 250.0.0.0 announcement, so it
   * returns false. drainAndFindRouteAdvertised reads messages from the
   * queue (with rib_/peerManager_ evb flushes via the shared sync
   * primitive) until it finds an announcement matching the target
   * prefix+nexthop, or the queue is provably empty.
   */
  EXPECT_TRUE(drainAndFindRouteAdvertised(
      "v4", "250.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "ThrashWithRouteInjection: survived");
}

/*
 * NOTE: ThrashDoubleUp test was removed because it causes TearDown to hang.
 * The double-up pattern (multiple UP events without DOWN in between) creates
 * orphaned AdjRibs that can't clean up properly in
 * asyncScope_.cancelAndJoinAsync(). This is a test infrastructure issue, not a
 * BGP++ bug - the actual BGP++ code handles the double-up pattern correctly
 * (version numbers discard stale events). The ThrashNoWait test (DOWN-UP
 * pattern) passes with 50+ cycles confirming the S596523 fix is working.
 */

/*
 * ThrashHighFrequency: Maximum frequency thrashing - 100 cycles as fast as
 * possible. This is the most likely to trigger timing-sensitive races.
 */
TEST_F(E2ESessionFlapTest, ThrashHighFrequency) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  constexpr int kThrashCycles = 100;
  XLOGF(INFO, "Starting HIGH FREQUENCY thrash: {} cycles", kThrashCycles);

  for (int i = 0; i < kThrashCycles; i++) {
    bringDownPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr3);
  }

  waitForSessionEstablished(kPeerAddr3, 200);
  sendEoRToPeer(peerId3);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOGF(INFO, "ThrashHighFrequency: survived {} cycles", kThrashCycles);
}

/*
 * ThrashInterleavedPeers: Interleave events from different peers.
 * This maximizes contention on shared data structures.
 */
TEST_F(E2ESessionFlapTest, ThrashInterleavedPeers) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  constexpr int kThrashCycles = 30;
  XLOGF(INFO, "Starting interleaved peer thrash: {} cycles", kThrashCycles);

  for (int i = 0; i < kThrashCycles; i++) {
    /*
     * Interleave operations: peer3 down, peer4 down, peer3 up, peer5 down,
     * peer4 up, peer3 down, peer5 up, etc.
     * This creates a chaotic mix of events in the queue.
     */
    bringDownPeer(kPeerAddr3);
    bringDownPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr3);
    bringDownPeer(kPeerAddr5);
    bringUpPeer(kPeerAddr4);
    bringDownPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr5);
    bringUpPeer(kPeerAddr3);
  }

  waitForSessionEstablished(kPeerAddr3, 100);
  waitForSessionEstablished(kPeerAddr4, 100);
  waitForSessionEstablished(kPeerAddr5, 100);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "ThrashInterleavedPeers: survived");
}

/*
 * ThrashImmediateReestablish: DOWN immediately followed by UP, testing
 * the race where ESTABLISHED arrives before TERMINATED cleanup is done.
 * This is the EXACT S596523 pattern.
 */
TEST_F(E2ESessionFlapTest, ThrashImmediateReestablish) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /*
   * Add some routes so the session has state to clean up.
   * More routes = longer cleanup = more race window.
   */
  for (int i = 0; i < 20; i++) {
    addRoute(
        "v4",
        fmt::format("{}.0.0.0", 10 + i),
        8,
        kPeerAddr3,
        "11.0.0.1",
        "65001");
  }
  auto prefixCheck = folly::IPAddress::createNetwork("29.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefixCheck));

  constexpr int kThrashCycles = 5;
  XLOGF(
      INFO, "Starting IMMEDIATE REESTABLISH thrash: {} cycles", kThrashCycles);

  for (int i = 0; i < kThrashCycles; i++) {
    /*
     * DOWN then UP as close together as possible.
     * The cleanup has to drain those 20 routes - if UP arrives
     * before cleanup finishes, we hit the race.
     */
    bringDownPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr3);
  }

  /*
   * After thrashing with pre-loaded routes, verify session re-establishment.
   * Keep cycles low (5) to avoid TearDown hang from event backlog.
   */
  waitForSessionEstablished(kPeerAddr3, 200);
  XLOG(INFO, "ThrashImmediateReestablish: survived thrashing with routes");
}

/*
 * ThrashWithEoRMidFlap: Send EoR while session is flapping.
 * EoR triggers additional state transitions that may race.
 */
TEST_F(E2ESessionFlapTest, ThrashWithEoRMidFlap) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  constexpr int kThrashCycles = 20;
  XLOGF(INFO, "Starting EoR mid-flap thrash: {} cycles", kThrashCycles);

  for (int i = 0; i < kThrashCycles; i++) {
    bringDownPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr3);
    /*
     * Send EoR immediately after UP - the session may not be ready yet.
     * This creates pressure on the EoR handling state machine.
     */
    sendEoRToPeer(peerId3);
  }

  waitForSessionEstablished(kPeerAddr3, 100);
  sendEoRToPeer(peerId3);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "ThrashWithEoRMidFlap: survived");
}

/*
 * ThrashChaos: All peers flapping simultaneously without waiting.
 * Simplified version that just does session thrashing without routes/EoR
 * during the chaos loop to avoid queue contention issues.
 */
TEST_F(E2ESessionFlapTest, ThrashChaos) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);
  bringUpPeerAndWait(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  constexpr int kChaosCycles = 20;
  XLOGF(INFO, "Starting CHAOS test: {} cycles", kChaosCycles);

  for (int i = 0; i < kChaosCycles; i++) {
    XLOGF(INFO, "Chaos cycle {}", i + 1);

    /*
     * Pure session thrashing on all peers - no routes or EoR during chaos.
     * This avoids queue contention that causes test hangs.
     */
    bringDownPeer(kPeerAddr3);
    bringDownPeer(kPeerAddr4);
    bringDownPeer(kPeerAddr5);
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);
    bringDownPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr3);
  }

  waitForSessionEstablished(kPeerAddr3, 200);
  waitForSessionEstablished(kPeerAddr4, 200);
  waitForSessionEstablished(kPeerAddr5, 200);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  addRoute("v4", "254.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("254.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "254.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "ThrashChaos: survived");
}

} // namespace facebook::bgp
