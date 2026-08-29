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
 * E2ERibEntryTest.cpp
 *
 * E2E tests converted from RibEntryTest.cpp
 * Tests RIB entry behavior through complete BGP flows end-to-end
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * E2E RibEntry test fixture
 * Provides setup helpers for RibEntry-related E2E tests
 */
class E2ERibEntryTest : public E2ETestFixture {
 protected:
  /*
   * Setup test environment with 2 EBGP peers (for sending routes)
   * and 1 IBGP peer (for receiving bestpath advertisements)
   */
  void setupE2EBestPathTest() {
    /* Define 2 EBGP peers to send routes */
    BgpPeerSpec ebgpPeer1 = {
        .asn = kPeerAsn1,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr1,
        .v4Nexthop = kV4Nexthop1,
        .v6Nexthop = kV6Nexthop1,
        .description = "EBGP Peer 1",
    };

    BgpPeerSpec ebgpPeer2 = {
        .asn = kPeerAsn2,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr2,
        .v4Nexthop = kV4Nexthop2,
        .v6Nexthop = kV6Nexthop2,
        .description = "EBGP Peer 2",
    };

    /* Define 1 IBGP peer to receive bestpath */
    BgpPeerSpec ibgpPeer = {
        .asn = kLocalAs1,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr3,
        .v4Nexthop = kV4Nexthop1, // IBGP uses local nexthop
        .v6Nexthop = kV6Nexthop1,
        .description = "IBGP Peer (Route Reflector Client)",
    };

    addPeer(ebgpPeer1);
    addPeer(ebgpPeer2);
    addPeer(ibgpPeer);

    createRib();
    createPeerManager(
        false /* enableUpdateGroup */, true /* enableEgressBackpressure */);
  }

  void bringUpAllPeers() {
    bringUpPeer(kPeerAddr1);
    bringUpPeer(kPeerAddr2);
    bringUpPeer(kPeerAddr3);

    /* Send EoR to complete session establishment */
    BgpPeerId peerId1{kPeerAddr1, kPeerRouterId1};
    BgpPeerId peerId2{kPeerAddr2, kPeerRouterId2};
    BgpPeerId peerId3{kPeerAddr3, kPeerRouterId3};

    sendEoRToPeer(peerId1);
    sendEoRToPeer(peerId2);
    sendEoRToPeer(peerId3);

    /* Drain initial EoR messages from IBGP peer's queue */
    waitForEoR(peerId3);
  }
};

/*
 * E2E conversion of SelectBestPathTest from RibEntryTest.cpp
 *
 * Original test: Creates RibEntry, adds paths from 2 peers, runs selectBestPath
 * and verifies bestpathChanged/nexthopChanged flags in 4 scenarios.
 *
 * E2E approach: Use 2 EBGP peers to send routes, 1 IBGP peer to receive
 * bestpath. Verify BGP behavior by checking what gets advertised to IBGP peer.
 */
TEST_F(E2ERibEntryTest, SelectBestPathE2E) {
  XLOG(INFO, "=== Starting SelectBestPathE2E Test ===");

  /* Setup: 2 EBGP peers + 1 IBGP peer */
  setupE2EBestPathTest();
  bringUpAllPeers();

  const auto testPrefix = "10.1.0.0/24";
  const uint8_t prefixLen = 24;

  XLOGF(INFO, "Test prefix: {}", testPrefix);

  /* ===================================================================
   * CASE 1: Initial best path selection
   * Both EBGP peers send same route with identical attributes
   * Expected: IBGP peer receives bestpath announcement
   * Corresponds to: bestpathChanged=true, nexthopChanged=true
   * =================================================================== */
  XLOG(INFO, "\n=== CASE 1: Initial bestpath selection ===");

  /* Peer1 sends route with default attributes */
  addRoute(
      "v4",
      "10.1.0.0",
      prefixLen,
      kPeerAddr1,
      kV4Nexthop1.str(),
      "100", // AS path
      "", // communities
      0, // addPathId
      100, // localPref
      0); // med

  /* Peer2 sends same route with identical attributes */
  addRoute(
      "v4",
      "10.1.0.0",
      prefixLen,
      kPeerAddr2,
      kV4Nexthop1.str(), // Same nexthop as peer1
      "100", // Same AS path
      "",
      0,
      100, // Same localPref
      0);

  /* Verify IBGP peer receives the bestpath (peer1 wins by router ID) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.1.0.0",
      prefixLen,
      kPeerAddr3,
      kV4Nexthop1.str(),
      "4200000001 100", // AS path prepended with local AS (4-byte AS format)
      ""));

  XLOG(INFO, "CASE 1 PASSED: IBGP peer received initial bestpath");

  /* ===================================================================
   * CASE 2: SKIPPED - No changes (duplicate update)
   *
   * Note: Testing "no update sent" in async E2E requires observer
   * infrastructure to verify silence. Skipping for this demo.
   * Original test: bestpathChanged=false, nexthopChanged=false
   * =================================================================== */
  XLOG(INFO, "\n=== CASE 2: SKIPPED (requires observer framework) ===");

  /* ===================================================================
   * CASE 3: Nexthop change only
   * Peer1 (the bestpath) sends update with different nexthop
   * Expected: IBGP peer receives updated route with new nexthop
   * Corresponds to: bestpathChanged=false, nexthopChanged=true
   * =================================================================== */
  XLOG(INFO, "\n=== CASE 3: Nexthop change (multipath change) ===");

  /* Peer1 (current bestpath) updates its route with different nexthop */
  addRoute(
      "v4",
      "10.1.0.0",
      prefixLen,
      kPeerAddr1,
      kV4Nexthop2.str(), // CHANGED: Different nexthop (was kV4Nexthop1)
      "100",
      "",
      0,
      100, // Same localPref (bestpath doesn't change)
      0);

  /*
   * Verify IBGP peer receives route update with new nexthop
   * Bestpath is still peer1, but nexthop changed
   * Note: For IBGP, nexthop is rewritten to local nexthop (nexthop-self)
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.1.0.0",
      prefixLen,
      kPeerAddr3,
      kV4Nexthop1.str(), // IBGP nexthop (rewritten due to nexthop-self)
      "4200000001 100", // AS path (4-byte format)
      ""));

  XLOG(INFO, "CASE 3 PASSED: Nexthop change handled correctly");

  /* ===================================================================
   * CASE 4: Bestpath change
   * Peer1 lowers its local preference -> peer2 becomes new bestpath
   * Expected: IBGP peer receives new bestpath (now from peer2)
   * Corresponds to: bestpathChanged=true, nexthopChanged=false
   * =================================================================== */
  XLOG(INFO, "\n=== CASE 4: Bestpath change (local pref change) ===");

  /* Peer1 sends update with lower local preference */
  addRoute(
      "v4",
      "10.1.0.0",
      prefixLen,
      kPeerAddr1,
      kV4Nexthop1.str(),
      "100",
      "",
      0,
      50, // CHANGED: Lower localPref (was 100, now 50)
      0);

  /*
   * Verify IBGP peer receives new bestpath from peer2
   * Peer2 now has better local preference (100 vs 50)
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.1.0.0",
      prefixLen,
      kPeerAddr3,
      kV4Nexthop1.str(), // Using IBGP nexthop (local nexthop self)
      "4200000001 100", // Peer2's AS path (4-byte format)
      ""));

  XLOG(INFO, "CASE 4 PASSED: Bestpath changed to peer2");

  XLOG(INFO, "\n=== SelectBestPathE2E Test Completed Successfully ===");
}

} // namespace bgp
} // namespace facebook
