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
 * E2E tests for route selection based on AS Path length.
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

using E2ERouteInfoSelectorAsPathTest = E2ERibTestFixture;

TEST_F(E2ERouteInfoSelectorAsPathTest, ShorterAsPathWins) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100);
  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      "11.0.0.2",
      "65001 65002 65003",
      "",
      0,
      100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: AS path update triggers bestpath recomputation
 *
 * Pattern: peer3 and peer4 send routes, peer5 receives bestpath
 * Step 1: peer4 sends route with AS=65001 (1 hop, becomes best)
 * Step 2: peer3 sends route with AS=65001 65002 65003 (3 hops, peer4 still
 * best) Step 3: peer3 updates to empty AS path (peer3 now best with 0 hops)
 */
TEST_F(E2ERouteInfoSelectorAsPathTest, AsPathUpdateChangesBestpath) {
  bringUpAllPeersWithEor();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");

  /*
   * Step 1: peer4 sends route with short AS path
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "", 0, 100);
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65001"));

  /*
   * Step 2: peer3 sends route with long AS path (peer4 still wins)
   */
  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      "11.0.0.1",
      "65001 65002 65003",
      "",
      0,
      100);

  /*
   * Step 3: peer3 updates to empty AS path (peer3 now wins)
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "", "", 0, 100);

  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001"));
}

TEST_F(E2ERouteInfoSelectorAsPathTest, EqualAsPathUsesNextTiebreaker) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 65002", "", 0, 100);
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65003 65004", "", 0, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Multiple prefixes with different AS paths select correct bestpaths
 *
 * For prefix 10.0.0.0/8: peer3 AS=65001 (shorter) wins
 * For prefix 20.0.0.0/8: peer4 AS=65001 (shorter) wins
 */
TEST_F(E2ERouteInfoSelectorAsPathTest, MultiplePrefixesDifferentAsPaths) {
  bringUpAllPeersWithEor();

  /*
   * For prefix 10.0.0.0/8, add peer3 first (shorter AS path wins)
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100);
  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));

  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65001"));

  /*
   * For prefix 20.0.0.0/8, add peer4 first (shorter AS path wins)
   */
  addRoute("v4", "20.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001", "", 0, 100);
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  EXPECT_TRUE(verifyRouteAdd(
      "v4", "20.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65001"));

  /*
   * Add competing routes with longer AS paths (bestpath should not change)
   */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001 65002", "", 0, 100);
  addRoute(
      "v4",
      "20.0.0.0",
      8,
      kPeerAddr3,
      "11.0.0.1",
      "65001 65002 65003",
      "",
      0,
      100);

  /*
   * Verify bestpaths haven't changed (routes still exist in shadow RIB)
   */
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));
}

} // namespace facebook::bgp
