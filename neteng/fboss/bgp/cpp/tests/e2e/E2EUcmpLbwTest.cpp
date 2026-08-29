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
 * E2E tests for UCMP (Unequal Cost Multi-Path) and LBW (Link Bandwidth)
 * routing.
 *
 * These tests verify LBW community handling, UCMP weight calculation,
 * and link bandwidth-based route selection.
 *
 * Derived from: RibTest.cpp, AdjRibOutTest.cpp
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2EUcmpLbwTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    /* Enable UCMP weight calculation from LBW extended community */
    enableComputeUcmpFromLbw(true);
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
 * Test: Standard ECMP without LBW
 * Derived from: RibTest.cpp
 *
 * When LBW is not configured, routes should use standard ECMP.
 * E2E verification: Check that multiple paths exist in RIB and
 * multipath nexthops are populated for ECMP.
 */
TEST_F(E2EUcmpLbwTest, StandardEcmpWithoutLbw) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * E2E ECMP verification: Both paths should be installed in RIB.
   * For ECMP to work, routes must have same AS path (65001).
   */
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 2))
      << "ECMP requires 2 paths in RIB";

  /*
   * Verify multipath nexthops are populated.
   * This confirms ECMP is active with both nexthops.
   */
  ASSERT_TRUE(waitForMultipathNexthopCount("10.0.0.0/8", 2))
      << "ECMP should have 2 nexthops in weighted nexthop map";

  /*
   * Verify the specific ECMP legs - should include both nexthops.
   */
  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  EXPECT_EQ(weightedNexthops.size(), 2) << "Should have exactly 2 ECMP legs";
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.1")) > 0)
      << "ECMP should include nexthop 11.0.0.1";
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.2")) > 0)
      << "ECMP should include nexthop 11.0.0.2";

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Route with LBW community is propagated
 * Derived from: AdjRibOutTest.cpp
 *
 * Routes with LBW extended community should be forwarded correctly.
 */
TEST_F(E2EUcmpLbwTest, LbwCommunityForwarding) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Multiple routes with different communities form ECMP
 *
 * Verify that routes with same AS path but different communities
 * are both installed and form ECMP multipath.
 */
TEST_F(E2EUcmpLbwTest, MultipleRoutesWithDifferentCommunities) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "100:2", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * E2E ECMP verification: Both paths should be installed.
   * Different communities don't affect ECMP eligibility.
   */
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 2))
      << "Both paths should be installed in RIB";

  ASSERT_TRUE(waitForMultipathNexthopCount("10.0.0.0/8", 2))
      << "ECMP should have 2 nexthops";

  /*
   * Verify the specific ECMP legs with different communities.
   */
  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  EXPECT_EQ(weightedNexthops.size(), 2) << "Should have exactly 2 ECMP legs";
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.1")) > 0)
      << "ECMP should include nexthop 11.0.0.1 (community 100:1)";
  EXPECT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.2")) > 0)
      << "ECMP should include nexthop 11.0.0.2 (community 100:2)";

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Routes with different AS paths and local_pref
 * Derived from: RibTest.cpp
 *
 * When routes have different AS paths, the higher local_pref wins for
 * bestpath selection. Both paths are kept in RIB for failover.
 */
TEST_F(E2EUcmpLbwTest, DifferentAsPathsWithLocalPref) {
  bringUpAllPeersWithEor();

  /*
   * Routes with different AS paths (65001 vs 65002).
   * The higher local_pref (200) wins for bestpath selection.
   */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0, 200, 0);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002", "100:2", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * E2E verification: Both paths should be installed in RIB.
   * The bestpath (higher local_pref) will be preferred for forwarding.
   */
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 2))
      << "Both paths should be installed in RIB";

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: IPv6 ECMP with same AS path
 *
 * Verify ECMP handling for IPv6 routes with matching AS paths.
 */
TEST_F(E2EUcmpLbwTest, IPv6EcmpRoutes) {
  bringUpAllPeersWithEor();

  addRoute(
      "v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001", "100:1", 0);
  addRoute(
      "v6", "2001:db8::", 32, kPeerAddr4, "2001:db8::2", "65001", "100:2", 0);

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * E2E ECMP verification for IPv6: Both paths with same AS path
   * should form ECMP multipath.
   */
  ASSERT_TRUE(waitForPathCountInRib("2001:db8::/32", 2))
      << "IPv6 ECMP requires 2 paths in RIB";

  ASSERT_TRUE(waitForMultipathNexthopCount("2001:db8::/32", 2))
      << "IPv6 ECMP should have 2 nexthops";

  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));
}

/*
 * Test: Route update changes UCMP weights
 *
 * When route attributes change, UCMP weights should be recalculated.
 */
TEST_F(E2EUcmpLbwTest, RouteUpdateChangesUcmpWeights) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:5", 0);

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Withdrawal removes ECMP path
 *
 * When a route is withdrawn, the path should be removed from RIB.
 * E2E verification: Start with 2 paths, withdraw one, verify 1 remains.
 */
TEST_F(E2EUcmpLbwTest, WithdrawalRemovesEcmpPath) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "100:2", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * Verify both paths are installed before withdrawal.
   */
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 2))
      << "Should start with 2 paths in RIB";

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  /*
   * Withdraw one path - should reduce to single path.
   */
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  /*
   * E2E verification: After withdrawal, only 1 path should remain.
   */
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 1))
      << "Should have 1 path after withdrawal";

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: All paths withdrawn sends withdrawal
 *
 * When all UCMP paths are withdrawn, a withdrawal should be sent.
 */
TEST_F(E2EUcmpLbwTest, AllPathsWithdrawnSendsWithdrawal) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

/*
 * Test: UCMP with different link bandwidths produces unequal weights
 *
 * Two routes with 10G and 20G LBW should produce 1:2 weight ratio.
 * This verifies actual UCMP weight calculation from LBW extended community.
 */
TEST_F(E2EUcmpLbwTest, UcmpWithDifferentLinkBandwidths) {
  bringUpAllPeersWithEor();

  /*
   * Route 1: 10 Gbps link bandwidth
   * Route 2: 20 Gbps link bandwidth
   * Expected weight ratio: 1:2
   */
  addRouteWithLbw("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", 10.0f);
  addRouteWithLbw("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", 20.0f);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 2))
      << "Should have 2 paths in RIB for UCMP";

  /*
   * Verify UCMP weights: 10G and 20G should produce 1:2 ratio.
   * The weights in WeightedNexthopMap are normalized uint32_t values.
   */
  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  ASSERT_EQ(weightedNexthops.size(), 2)
      << "Should have 2 nexthops in weighted map";

  /* Verify exact nexthop IPs are present */
  ASSERT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.1")) > 0)
      << "Nexthop 11.0.0.1 should be in weighted map";
  ASSERT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.2")) > 0)
      << "Nexthop 11.0.0.2 should be in weighted map";

  auto weight1 = weightedNexthops.at(folly::IPAddress("11.0.0.1"));
  auto weight2 = weightedNexthops.at(folly::IPAddress("11.0.0.2"));

  EXPECT_GT(weight1, 0u) << "UCMP weight for 10G path should be non-zero";
  EXPECT_GT(weight2, 0u) << "UCMP weight for 20G path should be non-zero";
  EXPECT_NEAR(static_cast<double>(weight2) / weight1, 2.0, 0.1)
      << "20G path should have ~2x the weight of 10G path";
}

/*
 * Test: Equal link bandwidths produce equal UCMP weights
 *
 * Two routes with same LBW should have identical weights.
 */
TEST_F(E2EUcmpLbwTest, EqualLinkBandwidthsProduceEqualWeights) {
  bringUpAllPeersWithEor();

  addRouteWithLbw("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", 10.0f);
  addRouteWithLbw("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", 10.0f);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 2));

  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  ASSERT_EQ(weightedNexthops.size(), 2);

  /* Verify exact nexthop IPs are present */
  ASSERT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.1")) > 0)
      << "Nexthop 11.0.0.1 should be in weighted map";
  ASSERT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.2")) > 0)
      << "Nexthop 11.0.0.2 should be in weighted map";

  auto weight1 = weightedNexthops.at(folly::IPAddress("11.0.0.1"));
  auto weight2 = weightedNexthops.at(folly::IPAddress("11.0.0.2"));

  EXPECT_EQ(weight1, weight2) << "Equal LBW should produce equal weights";
}

/*
 * Test: Three-way UCMP with different link bandwidths
 *
 * Three routes with 10G, 20G, and 30G LBW should produce 1:2:3 weight ratio.
 */
TEST_F(E2EUcmpLbwTest, ThreeWayUcmpWithDifferentLinkBandwidths) {
  bringUpAllPeersWithEor();

  addRouteWithLbw("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", 10.0f);
  addRouteWithLbw("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", 20.0f);
  addRouteWithLbw("v4", "10.0.0.0", 8, kPeerAddr5, "11.0.0.3", "65001", 30.0f);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 3))
      << "Should have 3 paths in RIB for three-way UCMP";

  auto weightedNexthops = getWeightedNexthops("10.0.0.0/8");
  ASSERT_EQ(weightedNexthops.size(), 3)
      << "Should have 3 nexthops in weighted map";

  /* Verify exact nexthop IPs are present */
  ASSERT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.1")) > 0)
      << "Nexthop 11.0.0.1 should be in weighted map";
  ASSERT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.2")) > 0)
      << "Nexthop 11.0.0.2 should be in weighted map";
  ASSERT_TRUE(weightedNexthops.count(folly::IPAddress("11.0.0.3")) > 0)
      << "Nexthop 11.0.0.3 should be in weighted map";

  auto weight1 = weightedNexthops.at(folly::IPAddress("11.0.0.1"));
  auto weight2 = weightedNexthops.at(folly::IPAddress("11.0.0.2"));
  auto weight3 = weightedNexthops.at(folly::IPAddress("11.0.0.3"));

  /*
   * Verify weight ratios: 10G:20G:30G should be 1:2:3
   */
  EXPECT_GT(weight1, 0u) << "10G weight should be non-zero";
  EXPECT_NEAR(static_cast<double>(weight2) / weight1, 2.0, 0.1)
      << "20G should be ~2x of 10G";
  EXPECT_NEAR(static_cast<double>(weight3) / weight1, 3.0, 0.1)
      << "30G should be ~3x of 10G";
}

} // namespace facebook::bgp
