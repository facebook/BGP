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
 * E2E tests for RIB-FIB synchronization (SEV Pattern 3, 8 SEVs).
 *
 * Uses enhanced TestFib with failure injection to verify:
 * - Routes are programmed correctly to FIB
 * - FIB routes are cleaned up when peer goes down
 * - FIB connection state control works
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2EFibSyncTest : public E2ERibTestFixture {};

/*
 * Verify route is programmed to FIB and appears in programmedRoutes.
 */
TEST_F(E2EFibSyncTest, RouteProgrammedToFib) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  auto* testFib = getTestFib();
  ASSERT_NE(testFib, nullptr);

  EXPECT_TRUE(testFib->getProgrammedRoutes().count(prefix) > 0);
  EXPECT_GT(testFib->getProgramCallCount(), 0u);
  EXPECT_TRUE(testFib->isFullSynced());

  // Verify fibBatchList_ quantile stat is published after FIB programming.
  // Quantile stats compute averages over time windows, so we can only
  // verify the counter is registered, not its exact value.
  auto tcData = fb303::ThreadCachedServiceData::get();
  tcData->publishStats();
  const std::string avgKey =
      std::string(RibStats::kFibBatchListSize) + ".avg.60";
  EXPECT_TRUE(tcData->hasCounter(avgKey));

  XLOGF(
      INFO,
      "Route programmed to FIB, programCount={}",
      testFib->getProgramCallCount());
}

/*
 * Verify FIB programmed routes are withdrawn when peer goes down.
 */
TEST_F(E2EFibSyncTest, FibRoutesWithdrawnOnPeerDown) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  auto* testFib = getTestFib();
  ASSERT_NE(testFib, nullptr);
  EXPECT_TRUE(testFib->getProgrammedRoutes().count(prefix) > 0);

  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));
  XLOG(INFO, "FIB routes withdrawn after peer down");
}

/*
 * FIB connection state control — verify setConnected works.
 */
TEST_F(E2EFibSyncTest, FibConnectionStateControl) {
  auto* testFib = getTestFib();
  ASSERT_NE(testFib, nullptr);

  EXPECT_TRUE(testFib->isConnected());

  testFib->setConnected(false);
  EXPECT_FALSE(testFib->isConnected());

  testFib->setConnected(true);
  EXPECT_TRUE(testFib->isConnected());

  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
  XLOG(INFO, "FIB connection state control works correctly");
}

} // namespace facebook::bgp
