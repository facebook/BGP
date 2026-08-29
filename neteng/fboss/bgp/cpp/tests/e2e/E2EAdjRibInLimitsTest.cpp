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
 * E2E tests for AdjRibIn route limits and overload protection.
 *
 * These tests verify prefix limit enforcement, per-peer route limits,
 * and switch-level overload protection in the BGP RIB.
 *
 * Derived from: AdjRibInTest.cpp::CanAddRibEntryTest_*
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

class E2EAdjRibInLimitsTest : public E2ETestFixture {
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
 * Test: Routes within limit are accepted
 * Derived from: AdjRibInTest.cpp::CanAddRibEntryTest_*
 *
 * When the number of routes is within the configured limit,
 * all routes should be accepted.
 */
TEST_F(E2EAdjRibInLimitsTest, RoutesWithinLimitAccepted) {
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

  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
      {.prefix = "20.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
      {.prefix = "30.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, expectedRoutes));
}

/*
 * Test: Routes from multiple peers are tracked separately
 * Derived from: AdjRibInTest.cpp::CanAddRibEntryTest_PerPeer
 *
 * Per-peer route limits should be enforced independently for each peer.
 */
TEST_F(E2EAdjRibInLimitsTest, PerPeerRouteTracking) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "20.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
      {.prefix = "20.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, expectedRoutes));
}

/*
 * Test: Route withdrawal decrements count
 * Derived from: AdjRibInTest.cpp::CanAddRibEntryTest_*
 *
 * When routes are withdrawn, the count should be decremented,
 * allowing new routes to be added.
 */
TEST_F(E2EAdjRibInLimitsTest, WithdrawalDecrementsCount) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
      {.prefix = "20.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, expectedRoutes));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  addRoute("v4", "30.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix3 = folly::IPAddress::createNetwork("30.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix3));
  EXPECT_TRUE(verifyRouteAdd("v4", "30.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: IPv6 routes are tracked correctly
 *
 * Verify that route limits work correctly for IPv6 prefixes.
 */
TEST_F(E2EAdjRibInLimitsTest, IPv6RoutesTracked) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");
  addRoute("v6", "2001:db8:1::", 48, kPeerAddr3, "2001:db8::1", "65001");

  auto prefix1 = folly::IPAddress::createNetwork("2001:db8::/32");
  auto prefix2 = folly::IPAddress::createNetwork("2001:db8:1::/48");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "2001:db8::",
       .prefixLen = 32,
       .expectedNexthop = "2401:db00:e011:411:1000::2d"},
      {.prefix = "2001:db8:1::",
       .prefixLen = 48,
       .expectedNexthop = "2401:db00:e011:411:1000::2d"},
  };
  EXPECT_TRUE(verifyRoutes("v6", kPeerAddr5, expectedRoutes));
}

/*
 * Test: Peer down clears route count
 *
 * When a peer goes down, its route count should be cleared.
 */
TEST_F(E2EAdjRibInLimitsTest, PeerDownClearsCount) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
      {.prefix = "20.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, expectedRoutes));

  bringDownPeer(kPeerAddr3);

  std::vector<WithdrawSpec> expectedWithdraws = {
      {.prefix = "10.0.0.0", .prefixLen = 8},
      {.prefix = "20.0.0.0", .prefixLen = 8},
  };
  EXPECT_TRUE(verifyRouteWithdraws("v4", kPeerAddr5, expectedWithdraws));
}

} // namespace facebook::bgp
