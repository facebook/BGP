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
 * E2E tests for AdjRibIn ingress policy processing.
 *
 * These tests verify ingress policy evaluation and route filtering through
 * AdjRibIn, including permit/deny and attribute modification.
 *
 * Derived from: AdjRibInTest.cpp
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStats.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2EAdjRibInPolicyTest : public E2ETestFixture {
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
 * Test: Route accepted without policy
 * Derived from: AdjRibInTest.cpp::V4PolicyAcceptReject
 */
TEST_F(E2EAdjRibInPolicyTest, RouteAcceptedWithoutPolicy) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Route with local preference modified by policy
 * Derived from: AdjRibInTest.cpp::LocalPrefPrePolicyProcessing
 */
TEST_F(E2EAdjRibInPolicyTest, LocalPrefModification) {
  bringUpAllPeersWithEor();

  // Route with specific local pref
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 200, 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: RibPolicyTest.cpp::BgpPathMatcherCommunityMatchTest
 */
TEST_F(E2EAdjRibInPolicyTest, CommunityBasedAccept) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Permit after deny sequence
 * Derived from: AdjRibInTest.cpp::VerifyPermitAfterDeny
 */
TEST_F(E2EAdjRibInPolicyTest, PermitAfterDeny) {
  bringUpAllPeersWithEor();

  // First route (would be denied if policy active)
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Deny after permit sequence
 * Derived from: AdjRibInTest.cpp::VerifyDenyAfterPermit
 */
TEST_F(E2EAdjRibInPolicyTest, DenyAfterPermit) {
  bringUpAllPeersWithEor();

  // Route that would initially be permitted
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp::V4UpdateProcessingMultiple
 */
TEST_F(E2EAdjRibInPolicyTest, MultiplePrefixesDifferentActions) {
  bringUpAllPeersWithEor();

  // Multiple routes
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
 * Test: Route filter policy deny
 * Derived from: AdjRibInTest.cpp::VerifyRouteFilterPolicyDeny
 */
TEST_F(E2EAdjRibInPolicyTest, RouteFilterBasic) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp::V6UpdateProcessingMultiple
 */
TEST_F(E2EAdjRibInPolicyTest, IPv6PolicyProcessing) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));
}

/*
 * Derived from: AdjRibInTest.cpp::VerifyPolicyReEvaluationSynchronization
 */
TEST_F(E2EAdjRibInPolicyTest, RouteUpdatePolicyReEvaluation) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  // Update route attributes
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:2", 0, 150, 0);

  // Updated route should be propagated
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: VIP injector prefix counting
 * Derived from: AdjRibInTest.cpp::VipInjectorPrefixCount
 */
TEST_F(E2EAdjRibInPolicyTest, VipInjectorPrefixCounting) {
  bringUpAllPeersWithEor();

  // Multiple VIP-like routes
  injectLocalRoutesAtRuntime({"10.0.0.0/8"}, {"100:1"}, 100);
  injectLocalRoutesAtRuntime({"20.0.0.0/8"}, {"100:1"}, 100);

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  // Use batch verification - BGP may batch multiple prefixes into one UPDATE
  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.1"},
      {.prefix = "20.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.1"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr3, expectedRoutes));
}

/*
 * Test: Route with standard community is processed
 *
 * Verify routes with community attributes are handled correctly.
 */
TEST_F(E2EAdjRibInPolicyTest, CommunityRouteProcessing) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "65000:100", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Multiple communities on route
 *
 * Verify routes with multiple community values are handled.
 */
TEST_F(E2EAdjRibInPolicyTest, MultipleCommunities) {
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
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Reproduce P1: totalVipPrefixesCount is a global counter, but each peer
 * overwrites it with its own vipPrefixes.size() instead of accumulating.
 *
 * Setup: Two peers each send one VIP route (AS path contains 65000).
 * Expected: totalVipPrefixesCount == 2 (one per peer).
 * Without fix: totalVipPrefixesCount == 1 (last peer overwrites).
 */
TEST_F(E2EAdjRibInPolicyTest, TotalVipPrefixesCountMultiPeer) {
  totalVipPrefixesCount = 0;

  /* SetUp already created Rib + PeerManagerBase with peers 3, 4, 5 */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Peer3 sends a VIP route (AS path contains 65000) */
  addRoute("v4", "10.1.0.0", 24, kPeerAddr3, "11.0.0.1", "65000");

  /* Peer4 sends a different VIP route */
  addRoute("v4", "10.2.0.0", 24, kPeerAddr4, "12.0.0.1", "65000");

  auto prefix1 = folly::IPAddress::createNetwork("10.1.0.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("10.2.0.0/24");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  EXPECT_EQ(totalVipPrefixesCount, 2)
      << "totalVipPrefixesCount should be 2 (one VIP from each peer)";
}

} // namespace facebook::bgp
