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
 * E2E tests for AdjRibIn update processing.
 *
 * These tests verify route announcement and withdrawal processing through
 * AdjRibIn, including IPv4/IPv6 and ADD-PATH scenarios.
 *
 * Derived from: AdjRibInTest.cpp
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2EAdjRibInUpdateTest : public E2ETestFixture {
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
 * Derived from: AdjRibInTest.cpp::V4UpdateProcessingSingle
 */
TEST_F(E2EAdjRibInUpdateTest, V4UpdateProcessingSingle) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  // Route should be propagated to other peers
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp::V4UpdateProcessingMultiple
 */
TEST_F(E2EAdjRibInUpdateTest, V4UpdateProcessingMultiple) {
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
}

/*
 * Derived from: AdjRibInTest.cpp::V6UpdateProcessingMultiple
 */
TEST_F(E2EAdjRibInUpdateTest, V6UpdateProcessingMultiple) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");
  addRoute("v6", "2001:db8:1::", 48, kPeerAddr3, "2001:db8::1", "65001");

  auto prefix1 = folly::IPAddress::createNetwork("2001:db8::/32");
  auto prefix2 = folly::IPAddress::createNetwork("2001:db8:1::/48");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  // Use batch verification - BGP may batch multiple prefixes into one UPDATE
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
 * Derived from: AdjRibInTest.cpp::V4AnnounceAndWithdraw
 */
TEST_F(E2EAdjRibInUpdateTest, V4AnnounceAndWithdraw) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

/*
 * Derived from: AdjRibInTest.cpp::ConsecutiveV4Withdraw
 */
TEST_F(E2EAdjRibInUpdateTest, ConsecutiveV4Withdraw) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  // Use batch verification - BGP may batch multiple prefixes into one UPDATE
  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
      {.prefix = "20.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, expectedRoutes));

  // Verify ODS counter after adjRibIn insertions (2 routes from peer3)
  auto tcData = fb303::ThreadCachedServiceData::get();
  tcData->publishStats();
  EXPECT_EQ(2, tcData->getCounter(RibStats::kAdjRibInCount));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  deleteRoute("v4", "20.0.0.0", 8, kPeerAddr3);

  // Use batch verification for withdrawals
  std::vector<WithdrawSpec> expectedWithdraws = {
      {.prefix = "10.0.0.0", .prefixLen = 8},
      {.prefix = "20.0.0.0", .prefixLen = 8},
  };
  EXPECT_TRUE(verifyRouteWithdraws("v4", kPeerAddr5, expectedWithdraws));

  // Verify ODS counter decremented to 0 after withdrawals
  tcData->publishStats();
  EXPECT_EQ(0, tcData->getCounter(RibStats::kAdjRibInCount));
}

/*
 * Derived from: AdjRibInTest.cpp::SpuriousWithdraw
 */
TEST_F(E2EAdjRibInUpdateTest, SpuriousWithdraw) {
  bringUpAllPeersWithEor();

  // Withdraw a route that was never announced - should not cause issues
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  // System should remain stable, add a real route
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "20.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp::NexthopChangeHandling
 *
 * Verify that when a route's nexthop changes, the update is processed.
 */
TEST_F(E2EAdjRibInUpdateTest, NexthopChangeHandling) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  // Update with different nexthop
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001");

  // Wait for the update to be processed
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 1));

  // Verify peer5 receives the route
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp::AttributeChangeHandling
 *
 * Verify that when a route's attributes change, the update is processed.
 */
TEST_F(E2EAdjRibInUpdateTest, AttributeChangeHandling) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  // Update with different local pref
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 200, 0);

  // Wait for the update to be processed
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 1));

  // Verify peer5 receives the route
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp::RedistributePeerUpdate
 */
TEST_F(E2EAdjRibInUpdateTest, RedistributePeerUpdate) {
  bringUpAllPeersWithEor();

  // Same prefix from two different peers
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  // Best route should be advertised to Peer5
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Rapid successive updates for same prefix
 *
 * Multiple updates for the same prefix in quick succession should
 * result in the final state being propagated.
 */
TEST_F(E2EAdjRibInUpdateTest, RapidSuccessiveUpdates) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 150, 0);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 200, 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Update followed by immediate withdrawal
 *
 * An update followed by immediate withdrawal should result in no route.
 */
TEST_F(E2EAdjRibInUpdateTest, UpdateFollowedByWithdrawal) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

} // namespace facebook::bgp
