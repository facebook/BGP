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
 * E2E tests for route selection based on Local Preference.
 *
 * These tests verify that when multiple routes for the same prefix are received
 * from different peers with different local preferences, the route with the
 * highest local preference is selected and advertised to other peers.
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

using E2ERouteInfoSelectorLocalPrefTest = E2ERibTestFixture;

/*
 * Test: Higher local preference wins
 */
TEST_F(E2ERouteInfoSelectorLocalPrefTest, HigherLocalPrefWins) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 300);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002", "", 0, 200);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Local preference change triggers bestpath recomputation
 *
 * This test verifies that when a route's local preference changes,
 * the bestpath is recomputed and the new best route is advertised.
 *
 * Pattern: peer3 and peer4 send routes, peer5 receives bestpath
 * Step 1: peer4 sends route with LP=200 (becomes best - only route)
 * Step 2: peer3 sends route with LP=100 (peer4 still best)
 * Step 3: peer3 updates to LP=300 (peer3 now best)
 */
TEST_F(E2ERouteInfoSelectorLocalPrefTest, LocalPrefChangeUpdatesBestpath) {
  bringUpAllPeersWithEor();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");

  /*
   * Step 1: peer4 sends route with LP=200
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002", "", 0, 200);
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * Verify peer4's route is advertised to peer5
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65002"));

  /*
   * Step 2: peer3 sends route with LP=100 (peer4 still wins)
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100);

  /*
   * Step 3: peer3 updates to LP=300 (peer3 now wins)
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 300);

  /*
   * Verify peer3's route is now advertised to peer5
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65001"));
}

/*
 * Test: Equal local preference falls through to AS path tiebreaker
 */
TEST_F(E2ERouteInfoSelectorLocalPrefTest, EqualLocalPrefUsesAsPathTiebreak) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 65002", "", 0, 100);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "", 0, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Local preference beats AS path length
 */
TEST_F(E2ERouteInfoSelectorLocalPrefTest, LocalPrefBeatsAsPath) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      "11.0.0.1",
      "65001 65002 65003",
      "",
      0,
      300);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "", 0, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

} // namespace facebook::bgp
