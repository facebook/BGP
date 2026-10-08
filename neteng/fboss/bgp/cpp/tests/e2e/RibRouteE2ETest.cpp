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
 * RibRouteE2ETest.cpp
 *
 * E2E tests for RIB route operations including route flapping scenarios.
 * Converted from RibTest.cpp unit tests.
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook {
namespace bgp {

/*
 * E2E RibRoute test fixture
 * Provides setup helpers for RIB route-related E2E tests
 */
class E2ERibRouteTest : public E2ETestFixture {
 protected:
  /*
   * Setup test environment with a single EBGP peer for route operations
   */
  void setupSinglePeerTest() {
    BgpPeerSpec ebgpPeer = {
        .asn = kPeerAsn1,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr1,
        .v4Nexthop = kV4Nexthop1,
        .v6Nexthop = kV6Nexthop1,
        .description = "EBGP Peer for Route Tests",
    };

    addPeer(ebgpPeer);

    createRib();
    createPeerManager(false /* enableUpdateGroup */);
  }

  void bringUpPeerAndSendEoR() {
    bringUpPeer(kPeerAddr1);

    BgpPeerId peerId{kPeerAddr1, kPeerRouterId1};
    sendEoRToPeer(peerId);
  }
};

/*
 * E2E conversion of RouteFlapWithSpuriousWithdrawTest from RibTest.cpp
 *
 * Original test purpose:
 * Ensure BGP does not crash when:
 * 1. route flaps (advertise/withdraw cycles)
 * 2. spurious withdrawal - withdrawing a non-existing prefix
 *
 * E2E approach:
 * - Use single EBGP peer to send routes
 * - Perform rapid route advertisement and withdrawal cycles
 * - Include spurious withdrawals (withdraw immediately after announce,
 *   then withdraw again)
 * - Verify system stability by completing all iterations without crash
 */
TEST_F(E2ERibRouteTest, RouteFlapWithSpuriousWithdrawE2E) {
  XLOG(INFO, "=== Starting RouteFlapWithSpuriousWithdrawE2E Test ===");

  /* Setup: single EBGP peer */
  setupSinglePeerTest();
  bringUpPeerAndSendEoR();

  const std::string prefix1 = "1::";
  const std::string prefix2 = "2::";
  const uint8_t prefixLen = 64;

  XLOGF(
      INFO,
      "Test prefixes: {}/{} and {}/{}",
      prefix1,
      (int)prefixLen,
      prefix2,
      (int)prefixLen);

  /* ===================================================================
   * Initial advertisement: Advertise both routes
   * =================================================================== */
  XLOG(INFO, "\n=== Initial route advertisement ===");

  addRoute(
      "v6",
      prefix1,
      prefixLen,
      kPeerAddr1,
      kV6Nexthop1.str(),
      "100", // AS path
      "", // communities
      0, // addPathId
      100, // localPref
      0); // med

  addRoute("v6", prefix2, prefixLen, kPeerAddr1, kV6Nexthop1.str(), "100");

  XLOG(INFO, "Initial routes advertised successfully");

  /* ===================================================================
   * Route flapping loop with spurious withdrawals
   *
   * Each iteration:
   * 1. Withdraw prefix1
   * 2. Withdraw prefix2
   * 3. Announce prefix2 (re-advertise)
   * 4. Withdraw prefix2 (spurious - just announced it)
   * 5. Announce prefix1 (re-advertise)
   *
   * This pattern tests:
   * - Rapid route churn
   * - Withdrawing a route that was just announced
   * - Re-advertising after withdrawal
   * =================================================================== */
  const int kNumIterations = 10;

  for (int i = 0; i < kNumIterations; i++) {
    XLOGF(INFO, "\n=== Iteration {}/{} ===", i + 1, kNumIterations);

    /* Withdraw both prefixes */
    deleteRoute("v6", prefix1, prefixLen, kPeerAddr1, 0);
    deleteRoute("v6", prefix2, prefixLen, kPeerAddr1, 0);

    /* Re-announce prefix2, then immediately withdraw it (spurious pattern) */
    addRoute("v6", prefix2, prefixLen, kPeerAddr1, kV6Nexthop1.str(), "100");

    deleteRoute("v6", prefix2, prefixLen, kPeerAddr1, 0);

    /* Re-announce prefix1 */
    addRoute("v6", prefix1, prefixLen, kPeerAddr1, kV6Nexthop1.str(), "100");

    XLOGF(INFO, "Iteration {} completed", i + 1);
  }

  /*
   * If we reach here without crash or hang, the test passes.
   * The original unit test also just verified no crash occurred.
   */
  XLOG(
      INFO,
      "\n=== RouteFlapWithSpuriousWithdrawE2E Test Completed Successfully ===");
  XLOGF(
      INFO,
      "Completed {} iterations of route flapping with spurious withdrawals",
      kNumIterations);
}

} // namespace bgp
} // namespace facebook
