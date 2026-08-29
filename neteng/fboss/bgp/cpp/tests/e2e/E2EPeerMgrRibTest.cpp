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
 * E2E tests for PeerManagerBase RIB integration.
 *
 * These tests verify the integration between PeerManagerBase and RIB,
 * including initial announcement, shadow RIB, and RIB dump requests.
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

class E2EPeerMgrRibTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
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
 * Test: Route appears in shadow RIB after announcement
 */
TEST_F(E2EPeerMgrRibTest, RouteInShadowRibAfterAnnouncement) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(waitForRouteInShadowRib(prefix));
}

/*
 * Test: Multiple routes in shadow RIB
 */
TEST_F(E2EPeerMgrRibTest, MultipleRoutesInShadowRib) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "30.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  EXPECT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.0.0.0/8")));
  EXPECT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("20.0.0.0/8")));
  EXPECT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.0.0.0/8")));
}

/*
 * Test: Route removed from shadow RIB after withdrawal
 */
TEST_F(E2EPeerMgrRibTest, RouteRemovedFromShadowRibAfterWithdrawal) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

/*
 * Test: Bestpath change reflected in shadow RIB
 */
TEST_F(E2EPeerMgrRibTest, BestpathChangeInShadowRib) {
  bringUpAllPeersWithEor();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");

  // First path from peer4 with LP=200
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002", "", 0, 200);
  EXPECT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65002"));

  // Better path from peer3 with LP=300 (becomes new bestpath)
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 300);
  EXPECT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65001"));
}

/*
 * Test: Peer session established before RIB initial announcement
 */
TEST_F(E2EPeerMgrRibTest, SessionEstablishedBeforeRibInit) {
  // Bring up peers
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId5));

  // Now announce route
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Peer session flap during RIB operation
 */
TEST_F(E2EPeerMgrRibTest, SessionFlapDuringRibOperation) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  // Peer5 flaps
  bringDownPeer(kPeerAddr5);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId5);

  // Route should be re-advertised to Peer5 in initial dump
  // Don't wait for EoR from peer5 as it would consume the UPDATE
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: IPv6 routes in shadow RIB
 */
TEST_F(E2EPeerMgrRibTest, IPv6RoutesInShadowRib) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  EXPECT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));
}

/*
 * Test: Replicate RIB message to multiple peers
 */
TEST_F(E2EPeerMgrRibTest, ReplicateRibMessageToMultiplePeers) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(waitForRouteInShadowRib(prefix));

  // Route should be advertised to both Peer4 and Peer5
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

} // namespace facebook::bgp
