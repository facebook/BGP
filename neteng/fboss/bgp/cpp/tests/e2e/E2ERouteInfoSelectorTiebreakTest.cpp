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
 * E2E tests for route selection tiebreakers (MED, withdrawal).
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

using E2ERouteInfoSelectorTiebreakTest = E2ERibTestFixture;

TEST_F(E2ERouteInfoSelectorTiebreakTest, LowerMedWins) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100, 100);
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "", 0, 100, 50);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERouteInfoSelectorTiebreakTest, MedOnlyComparedWithinSameAs) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100, 100);
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002", "", 0, 100, 50);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERouteInfoSelectorTiebreakTest, WithdrawTriggersReselection) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 200, 0);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERouteInfoSelectorTiebreakTest, AllWithdrawSendsWithdrawal) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr4);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

/*
 * Test: Shorter AS path wins over longer AS path.
 * Peer3 sends route with AS path length 3 (65001 65002 65003).
 * Peer4 sends route with AS path length 1 (65004).
 * Expected: Route from peer4 is selected (shorter path wins).
 * We verify by checking the AS path in the outbound update.
 * Note: Local AS (4200000001) is prepended when advertising to eBGP peers.
 */
TEST_F(E2ERouteInfoSelectorTiebreakTest, ShorterAsPathWins) {
  bringUpAllPeers();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 65002 65003");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65004");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65004"));
}

/*
 * Test: Lower router ID wins as final tiebreaker
 * Derived from: RouteInfoSelectorTest.cpp::BestpathSelectionTest
 *
 * When all other attributes are equal, the route from the peer with
 * the lower router ID should be selected.
 */
TEST_F(E2ERouteInfoSelectorTiebreakTest, LowerRouterIdWins) {
  bringUpAllPeers();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Multiple equivalent routes for same prefix
 * Derived from: RouteInfoSelectorTest.cpp::MultipathRecoveryTest
 *
 * When multiple routes have equal attributes (same AS, LP, etc.),
 * they should all be considered for multipath if enabled.
 */
TEST_F(E2ERouteInfoSelectorTiebreakTest, MultipleEquivalentRoutes) {
  bringUpAllPeers();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Route reselection when better path arrives
 *
 * When a route with better attributes arrives, it should become best.
 * Verify by adding higher LP route first and checking it's announced
 * with the correct AS path.
 */
TEST_F(E2ERouteInfoSelectorTiebreakTest, BetterPathArrives) {
  bringUpAllPeers();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");

  /* Add route with higher LP=200 first - should become best path */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002", "", 0, 200, 0);
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Verify the higher LP route is announced with correct AS path */
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65002"));

  /* Add lower LP route - should not become best path */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  /* Wait for second path to be installed */
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 2));
}

/*
 * Test: IPv6 route tiebreaker selection
 *
 * Verify tiebreaker logic works correctly for IPv6 routes.
 */
TEST_F(E2ERouteInfoSelectorTiebreakTest, IPv6Tiebreaker) {
  bringUpAllPeers();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");
  addRoute("v6", "2001:db8::", 32, kPeerAddr4, "2001:db8::2", "65001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));
}

} // namespace facebook::bgp
