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
 * E2E tests for RIB route withdrawals.
 *
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

using E2ERibWithdrawalTest = E2ERibTestFixture;

TEST_F(E2ERibWithdrawalTest, BasicRouteWithdrawal) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

TEST_F(E2ERibWithdrawalTest, WithdrawalWithBackupPath) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 200, 0);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERibWithdrawalTest, WithdrawalOfAllPaths) {
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

TEST_F(E2ERibWithdrawalTest, MultiplePrefixWithdrawals) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "30.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  auto prefix3 = folly::IPAddress::createNetwork("30.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix3));

  // Use batch verification - BGP may batch multiple prefixes into one UPDATE
  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
      {.prefix = "20.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
      {.prefix = "30.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, expectedRoutes));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

TEST_F(E2ERibWithdrawalTest, IPv6RouteWithdrawal) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));

  deleteRoute("v6", "2001:db8::", 32, kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v6", "2001:db8::", 32, kPeerAddr5));
}

} // namespace facebook::bgp
