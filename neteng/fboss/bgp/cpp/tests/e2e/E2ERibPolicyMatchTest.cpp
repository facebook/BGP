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
 * E2E tests for RIB policy evaluation - Part 1 (Matching).
 *
 * These tests verify policy matching and application through the
 * complete BGP pipeline.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib, RibPolicy
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2ERibPolicyMatchTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
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
 * Test: Route with community is processed
 *
 * Verifies that a route with a community attribute is correctly processed
 * through the RIB and advertised with the community preserved.
 */
TEST_F(E2ERibPolicyMatchTest, RouteWithCommunityProcessed) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * Verify the route is advertised with the community preserved.
   * The local AS (4200000001) is prepended to the origin AS (65001).
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr5,
      "127.5.0.4",
      "4200000001 65001",
      "100:1"))
      << "Route should be advertised with community 100:1 preserved";
}

/*
 * Test: Route with multiple communities
 *
 * Verifies that a route with multiple community attributes is correctly
 * processed through the RIB and advertised with all communities preserved.
 */
TEST_F(E2ERibPolicyMatchTest, RouteWithMultipleCommunities) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      "11.0.0.1",
      "65001",
      "100:1 100:2 100:3",
      0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * Verify the route is advertised with all communities preserved.
   * The local AS (4200000001) is prepended to the origin AS (65001).
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr5,
      "127.5.0.4",
      "4200000001 65001",
      "100:1 100:2 100:3"))
      << "Route should be advertised with all communities (100:1 100:2 100:3) preserved";
}

/*
 * Test: AS path matching
 *
 * Verifies that a route with a long AS path is correctly processed through
 * the RIB and advertised with the complete AS path preserved.
 */
TEST_F(E2ERibPolicyMatchTest, AsPathMatching) {
  bringUpAllPeersWithEor();

  // Long AS path
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 65002 65003 65004");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * Verify the route is advertised with the complete AS path preserved.
   * The local AS (4200000001) is prepended to the original AS path.
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr5,
      "127.5.0.4",
      "4200000001 65001 65002 65003 65004"))
      << "Route should be advertised with complete AS path preserved";
}

/*
 * Test: MED value processing
 */
TEST_F(E2ERibPolicyMatchTest, MedValueProcessing) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100, 50);
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "", 0, 100, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  /*
   * Verify lower MED (50) route from kPeerAddr3 is selected as bestpath.
   * MED is compared when AS paths are equal - lower MED wins.
   */
  ASSERT_TRUE(waitForRouteInShadowRib(prefix, kPeerAddr3))
      << "Expected route from kPeerAddr3 (MED=50) to be selected over "
         "kPeerAddr4 (MED=100)";

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Local preference takes precedence
 *
 * Verify that when comparing routes, higher local_pref wins over lower MED.
 * The route with LP=200 from kPeerAddr4 should be selected despite having
 * worse MED (100) compared to the route with LP=100 and MED=10 from kPeerAddr3.
 */
TEST_F(E2ERibPolicyMatchTest, LocalPrefPrecedence) {
  bringUpAllPeersWithEor();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");

  /*
   * Step 1: Send route with higher local_pref from kPeerAddr4 (LP=200).
   * Wait for it to become bestpath before sending the competing route.
   */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "", 0, 200, 100);
  ASSERT_TRUE(waitForRouteInShadowRib(prefix, kPeerAddr4))
      << "Route from kPeerAddr4 (local_pref=200) should be in shadowRIB";

  /*
   * Step 2: Send route with lower local_pref from kPeerAddr3 (LP=100).
   * Despite having lower MED (10 vs 100), this should NOT become bestpath
   * because local_pref (200 > 100) is compared before MED.
   */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100, 10);

  /*
   * Wait for the route to be processed and verify that kPeerAddr4's route
   * with higher local_pref remains the bestpath.
   */
  ASSERT_TRUE(waitForRouteInShadowRib(prefix, kPeerAddr4))
      << "Route from kPeerAddr4 (local_pref=200) should remain bestpath over "
         "kPeerAddr3 (local_pref=100) despite higher MED";

  /*
   * Verify the advertised route has the expected AS path from kPeerAddr4.
   * The local AS (4200000001) is prepended to the origin AS (65001).
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65001"))
      << "Route should be advertised with AS path from higher local_pref route";
}

/*
 * Test: Prefix-based route selection
 */
TEST_F(E2ERibPolicyMatchTest, PrefixBasedSelection) {
  bringUpAllPeersWithEor();

  // More specific prefix
  addRoute("v4", "10.0.0.0", 24, kPeerAddr3, "11.0.0.1", "65001");

  // Less specific prefix
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("10.0.0.0/8");

  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  // Both prefixes should be advertised (routes may arrive in any order)
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr5,
      {{"10.0.0.0", 24, "127.5.0.4", "", "", 0},
       {"10.0.0.0", 8, "127.5.0.4", "", "", 0}}));
}

} // namespace facebook::bgp
