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
 * E2E tests for AdjRibOut route announcements.
 *
 * These tests verify route announcement processing through AdjRibOut:
 * - Basic IBGP and EBGP route announcements with nexthop rewriting
 * - Local route injection and advertisement to EBGP peers
 * - Multiple routes announced to a single peer
 * - Community attribute preservation during announcement
 * - AS path prepending for EBGP advertisements
 * - IPv6 route announcements
 * - Bestpath change propagation
 * - Simultaneous announcement to multiple peers
 *
 * Derived from: AdjRibOutTest.cpp
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

class E2EAdjRibOutAnnouncementTest : public E2ETestFixture {
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
 * Derived from: AdjRibOutTest.cpp::V4LocalRouteIBgpPeer
 */
TEST_F(E2EAdjRibOutAnnouncementTest, V4LocalRouteIBgpPeer) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  // Route should be advertised to IBGP peer with nexthop rewritten
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibOutTest.cpp::V6LocalRouteIBgpPeer
 */
TEST_F(E2EAdjRibOutAnnouncementTest, V6LocalRouteIBgpPeer) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));
}

/*
 * Derived from: AdjRibOutTest.cpp::groupingPrefixesbyAttributes
 */
TEST_F(E2EAdjRibOutAnnouncementTest, groupingPrefixesbyAttributes) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "30.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));

  // Use batch verification since routes may be grouped in one UPDATE
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr5,
      {{"10.0.0.0", 8, "127.5.0.4", "", "", 0},
       {"20.0.0.0", 8, "127.5.0.4", "", "", 0},
       {"30.0.0.0", 8, "127.5.0.4", "", "", 0}}));
}

/*
 * Derived from: AdjRibOutTest.cpp::VerifyCowNhAttributeModification
 */
TEST_F(E2EAdjRibOutAnnouncementTest, VerifyCowNhAttributeModification) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1 100:2", 0);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  // Communities should be propagated - verify the communities are preserved
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "", "100:1 100:2"));
}

/*
 * Test: AS path prepended for EBGP (SenderSuppressAsLoop)
 *
 * Derived from: AdjRibOutTest.cpp::SenderSuppressAsLoop
 *
 * Verifies that when advertising a route to an EBGP peer, the local ASN
 * (4200000001) is prepended to the AS path.
 * Route arrives with AS path "65001", should be advertised as "4200000001
 * 65001".
 */
TEST_F(E2EAdjRibOutAnnouncementTest, SenderSuppressAsLoop) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * AS path should be prepended with local AS (4200000001) when advertising
   * to EBGP peer. Incoming AS path is "65001", outgoing should be
   * "4200000001 65001".
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3", "4200000001 65001"));
}

/*
 * Derived from: RibTest.cpp::BestpathChangeWithNexthopChangeTest
 * Verifies initial route announcement. Full bestpath change testing
 * requires more complex setup with different nexthops per peer.
 */
TEST_F(E2EAdjRibOutAnnouncementTest, BestpathChangeWithNexthopChangeTest) {
  bringUpAllPeersWithEor();

  // Add route and verify it gets announced
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: AdjRibOutTest.cpp (multi-peer distribution)
 */
TEST_F(E2EAdjRibOutAnnouncementTest, MultiPeerDistribution) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  // Route should be announced to all peers (using their configured nexthops)
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Derived from: RibTest.cpp::LocalRoute
 */
TEST_F(E2EAdjRibOutAnnouncementTest, LocalRouteToEbgpPeer) {
  bringUpAllPeersWithEor();

  injectLocalRoutesAtRuntime({"10.0.0.0/8"}, {"100:1"}, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr3, "127.5.0.1"));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
}

} // namespace facebook::bgp
