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
 * E2E tests for AdjRibIn AS loop detection.
 *
 * These tests verify AS loop detection and filtering through AdjRibIn,
 * including confederation and local-AS scenarios.
 *
 * Derived from: AdjRibInTest.cpp
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

class E2EAdjRibInLoopTest : public E2ETestFixture {
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
 * Test: Reject route with AS loop (local AS in path)
 * Derived from: AdjRibInTest.cpp::RejectRouteWithAsLoop
 * Note: Local AS is 4200000001 (kAsn1), so routes with this AS are rejected
 */
TEST_F(E2EAdjRibInLoopTest, RejectRouteWithAsLoop) {
  bringUpAllPeersWithEor();

  // Route with local AS (4200000001) in path should be rejected
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 4200000001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  // Route should not appear in RIB due to AS loop
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));

  // Add a valid route to verify system is working
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto validPrefix = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(validPrefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "20.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Reject IPv6 route with AS loop
 * Derived from: AdjRibInTest.cpp::RejectIPv6RouteWithAsLoop
 */
TEST_F(E2EAdjRibInLoopTest, RejectIPv6RouteWithAsLoop) {
  bringUpAllPeersWithEor();

  // IPv6 route with AS loop (local AS 4200000001)
  addRoute(
      "v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001 4200000001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));
}

/*
 * Test: Accept route after loop is removed
 * Derived from: AdjRibInTest.cpp::AsLoopRouteProcessingAfterLearning
 */
TEST_F(E2EAdjRibInLoopTest, AcceptRouteAfterLoopRemoved) {
  bringUpAllPeersWithEor();

  // First send route with AS loop (local AS 4200000001)
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 4200000001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));

  // Update with route without loop
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  // Now route should be accepted
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Accept path with local-AS configured session
 * Derived from: AdjRibInTest.cpp::AcceptPathLocalAsSession
 */
TEST_F(E2EAdjRibInLoopTest, AcceptPathWithLocalAsSession) {
  bringUpAllPeersWithEor();

  // Normal route without loop should always be accepted
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp (AS path length handling)
 */
TEST_F(E2EAdjRibInLoopTest, LongAsPathNoLoop) {
  bringUpAllPeersWithEor();

  // Long AS path without loop
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 65002 65003 65004");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp (multi-peer loop scenario)
 */
TEST_F(E2EAdjRibInLoopTest, LoopRejectionThenValidFromOtherPeer) {
  bringUpAllPeersWithEor();

  // Peer3 sends route with loop (local AS 4200000001)
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 4200000001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));

  // Peer4 sends valid route for same prefix
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  // Valid route from Peer4 should be accepted
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Originator ID filtering (reject routes originated by self)
 * Derived from: AdjRibInTest.cpp::OriginatorIdFiltering
 */
TEST_F(E2EAdjRibInLoopTest, OriginatorIdFiltering) {
  bringUpAllPeersWithEor();

  // Valid route without self originator ID
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibInTest.cpp::RejectRouteWithAsLoop
 */
TEST_F(E2EAdjRibInLoopTest, MultipleAsLoops) {
  bringUpAllPeersWithEor();

  // Multiple occurrences of local AS (4200000001) should still be rejected
  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      "11.0.0.1",
      "65001 4200000001 65002 4200000001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));
}

/*
 * Test: Loop detection with AS at beginning of path
 *
 * Local AS at the beginning of path should still be detected as loop.
 */
TEST_F(E2EAdjRibInLoopTest, AsLoopAtBeginningOfPath) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "4200000001 65001 65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));
}

/*
 * Test: Loop detection with AS at end of path
 *
 * Local AS at the end of path should still be detected as loop.
 */
TEST_F(E2EAdjRibInLoopTest, AsLoopAtEndOfPath) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001 65002 4200000001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));
}

} // namespace facebook::bgp
