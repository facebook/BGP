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
 * E2E tests for eiBGP multipath feature.
 *
 * Tests verify that when eiBGP multipath is enabled, the EXTERNAL_ROUTE
 * preference filter is skipped in best path selection, equalizing eBGP
 * and iBGP paths for multipath.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib, RouteInfoSelector
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

/*
 * iBGP peer specs: use kAsn1 (local AS) so they form iBGP sessions.
 * Peer4 is reused as an iBGP peer by setting its ASN to kAsn1.
 */
inline const BgpPeerSpec kIbgpPeerSpec4 = {
    .asn = kAsn1,
    .localAddr = kLocalAddr1,
    .peerAddr = kPeerAddr4,
    .v4Nexthop = kNextHopV4_4,
    .v6Nexthop = kNextHopV6_4,
};

/*
 * E2E test fixture with eiBGP multipath enabled.
 * Sets up mixed eBGP + iBGP peers:
 * - Peer3: eBGP (kPeerAsn3, different from local AS kAsn1)
 * - Peer4: iBGP (kAsn1, same as local AS)
 * - Peer5: eBGP (kPeerAsn5, used as route receiver/verifier)
 */
class E2ERouteInfoSelectorEiBgpTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3); /* eBGP peer */
    addPeer(kIbgpPeerSpec4); /* iBGP peer */
    addPeer(kDefaultPeerSpec5); /* eBGP peer (route receiver) */
    enableEiBgpMultipath(true);
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/true);
  }

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
};

/*
 * Test: eBGP and iBGP routes are equalized with eiBGP enabled.
 *
 * With eiBGP multipath, both eBGP (peer3) and iBGP (peer4) routes
 * should form ECMP multipath since EXTERNAL_ROUTE filter is skipped.
 */
TEST_F(E2ERouteInfoSelectorEiBgpTest, EbgpAndIbgpEqualizedForMultipath) {
  bringUpAllPeersWithEor();

  /* eBGP route from peer3, iBGP route from peer4 - same AS path */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Both paths should be installed in RIB for ECMP */
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 2))
      << "eiBGP should allow both eBGP and iBGP paths in RIB";

  /* Verify multipath: both nexthops should be in the weighted nexthop map */
  ASSERT_TRUE(waitForMultipathNexthopCount("10.0.0.0/8", 2))
      << "eiBGP should have 2 nexthops (eBGP + iBGP equalized)";

  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  EXPECT_EQ(weightedNexthops.size(), 2)
      << "Should have exactly 2 ECMP legs (eBGP + iBGP)";
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.1")) > 0)
      << "eBGP nexthop should be in ECMP";
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.2")) > 0)
      << "iBGP nexthop should be in ECMP";
}

/*
 * Test: Local preference still breaks ties with eiBGP enabled.
 *
 * Even with eiBGP multipath, LP should still be the primary tiebreaker.
 * Higher LP route should win regardless of session type.
 * Note: iBGP preserves LP from UPDATE, eBGP may reset LP to default.
 * We set higher LP on the iBGP route to ensure LP comparison works.
 */
TEST_F(E2ERouteInfoSelectorEiBgpTest, LocalPrefStillBreaksTies) {
  bringUpAllPeersWithEor();

  /* eBGP route LP=100, iBGP route LP=200 (iBGP preserves LP from UPDATE) */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "", 0, 200, 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Only 1 nexthop - higher LP wins, eiBGP doesn't override LP */
  ASSERT_TRUE(waitForMultipathNexthopCount("10.0.0.0/8", 1))
      << "Higher LP should win over eiBGP equalization";

  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  EXPECT_EQ(weightedNexthops.size(), 1) << "Only LP winner should be selected";
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.2")) > 0)
      << "Higher LP route from iBGP peer should win";
}

/*
 * Test: AS path length still breaks ties with eiBGP enabled.
 *
 * Shorter AS path should win even with eiBGP multipath enabled.
 */
TEST_F(E2ERouteInfoSelectorEiBgpTest, AsPathLengthStillBreaksTies) {
  bringUpAllPeersWithEor();

  /* eBGP route with longer AS path, iBGP route with shorter */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 65002 65003");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65004");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Shorter AS path should win - eiBGP doesn't override AS path comparison */
  ASSERT_TRUE(waitForMultipathNexthopCount("10.0.0.0/8", 1))
      << "Shorter AS path should still win with eiBGP";

  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  EXPECT_EQ(weightedNexthops.size(), 1);
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.2")) > 0)
      << "Shorter AS path route should win";
}

/*
 * Test: Withdrawal triggers reselection with eiBGP.
 *
 * When one of the equalized paths is withdrawn, the remaining path
 * should still be active. Verify by consuming the outbound route update
 * to peer5 after the withdrawal.
 */
TEST_F(E2ERouteInfoSelectorEiBgpTest, WithdrawalTriggersReselection) {
  bringUpAllPeersWithEor();

  /* Two equalized paths */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Consume the initial route announcement to peer5 */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  /* Withdraw eBGP route */
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  /* Verify a route update is sent to peer5 with the remaining iBGP nexthop.
   * Use maxWaitRetries to wait for the withdrawal to be processed. */
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "", "", 0, 10))
      << "iBGP route should remain after eBGP withdrawal";
}

/*
 * Test: All paths withdrawn sends withdrawal with eiBGP.
 *
 * When all equalized paths are withdrawn, a route withdrawal is sent.
 */
TEST_F(E2ERouteInfoSelectorEiBgpTest, AllPathsWithdrawnSendsWithdrawal) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  /* Withdraw both paths */
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr4);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

/*
 * Test: IPv6 eBGP+iBGP equalization with eiBGP.
 *
 * Verify eiBGP multipath works correctly for IPv6 routes.
 */
TEST_F(E2ERouteInfoSelectorEiBgpTest, IPv6EbgpIbgpEqualized) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");
  addRoute("v6", "2001:db8::", 32, kPeerAddr4, "2001:db8::2", "65001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Both IPv6 paths should be in ECMP */
  ASSERT_TRUE(waitForPathCountInRib("2001:db8::/32", 2))
      << "IPv6 eiBGP should equalize eBGP and iBGP paths";

  ASSERT_TRUE(waitForMultipathNexthopCount("2001:db8::/32", 2))
      << "IPv6 eiBGP should have 2 nexthops";

  auto weightedNexthops = getWeightedNexthops("2001:db8::/32");
  EXPECT_EQ(weightedNexthops.size(), 2);
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("2001:db8::1")) > 0);
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("2001:db8::2")) > 0);
}

/*
 * Fixture without eiBGP enabled for control/comparison tests.
 * Uses same eBGP + iBGP peer setup but eiBGP is disabled.
 */
class E2ERouteInfoSelectorEiBgpDisabledTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3); /* eBGP peer */
    addPeer(kIbgpPeerSpec4); /* iBGP peer */
    addPeer(kDefaultPeerSpec5); /* eBGP peer (route receiver) */
    /* eiBGP NOT enabled - default behavior */
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/true);
  }

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
};

/*
 * Control test: Without eiBGP, eBGP is preferred over iBGP.
 *
 * Default behavior: EXTERNAL_ROUTE filter prefers eBGP over iBGP.
 * The eBGP route should be the sole bestpath.
 */
TEST_F(E2ERouteInfoSelectorEiBgpDisabledTest, EbgpPreferredOverIbgp) {
  bringUpAllPeersWithEor();

  /* eBGP route from peer3, iBGP route from peer4 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Without eiBGP, only eBGP route should be selected (EXTERNAL_ROUTE filter
   * prefers it) */
  ASSERT_TRUE(waitForMultipathNexthopCount("10.0.0.0/8", 1))
      << "Without eiBGP, only eBGP route should be selected";

  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  EXPECT_EQ(weightedNexthops.size(), 1)
      << "Only eBGP route should be in nexthop map";
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.1")) > 0)
      << "eBGP nexthop should be preferred over iBGP";
}

} // namespace facebook::bgp
