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
 * E2E tests for RIB local routes.
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

class E2ERibLocalRouteTest : public E2ETestFixture {
 protected:
  /*
   * Bring up peers but only wait for EoR from senders (peer3, peer4).
   * Don't wait for EoR from receiver (peer5) because it would consume
   * the UPDATE message for the local route.
   */
  void bringUpPeersForLocalRoutes() {
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);
    BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
    sendEoRToPeer(peerId3);
    sendEoRToPeer(peerId4);
    EXPECT_TRUE(waitForEoR(peerId3));
    EXPECT_TRUE(waitForEoR(peerId4));
  }

  /*
   * Standard setup with all 3 peers and full EoR exchange.
   */
  void setupStandardPeers() {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/true);
  }

  /*
   * Bring up all 3 peers and send EoR to all of them.
   */
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

TEST_F(E2ERibLocalRouteTest, LocalRouteAdvertisedToPeers) {
  addLocalRoute("10.0.0.0/8", {"100:1"}, 100);

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpPeersForLocalRoutes();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERibLocalRouteTest, MultipleLocalRoutes) {
  /*
   * Add one local route before setup, then inject more at runtime
   * to ensure each gets a separate UPDATE.
   */
  addLocalRoute("10.0.0.0/8", {"100:1"}, 100);

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpPeersForLocalRoutes();

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  injectLocalRoutesAtRuntime({"20.0.0.0/8"}, {"100:2"}, 100);
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));
  EXPECT_TRUE(verifyRouteAdd("v4", "20.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  injectLocalRoutesAtRuntime({"30.0.0.0/8"}, {"100:3"}, 100);
  auto prefix3 = folly::IPAddress::createNetwork("30.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix3));
  EXPECT_TRUE(verifyRouteAdd("v4", "30.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERibLocalRouteTest, LocalRouteInjectedAtRuntime) {
  setupStandardPeers();
  bringUpAllPeersWithEor();

  injectLocalRoutesAtRuntime({"10.0.0.0/8"}, {"100:1"}, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERibLocalRouteTest, LocalRouteWithdrawnAtRuntime) {
  setupStandardPeers();
  bringUpAllPeersWithEor();

  injectLocalRoutesAtRuntime({"10.0.0.0/8"}, {"100:1"}, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  withdrawLocalRoutesAtRuntime({"10.0.0.0/8"});
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

TEST_F(E2ERibLocalRouteTest, LocalRouteBeatsReceivedRoute) {
  setupStandardPeers();
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  injectLocalRoutesAtRuntime({"10.0.0.0/8"}, {"100:1"}, 100);
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test that a local route with minSupportingRoutes is NOT announced
 * until enough supporting routes exist. This verifies the aggregation/
 * min-support feature works correctly.
 */
TEST_F(E2ERibLocalRouteTest, LocalRouteNotAnnouncedWithoutMinSupport) {
  /*
   * Add a local route with minSupportingRoutes=2, meaning it should NOT
   * be announced until at least 2 supporting (more specific) routes exist.
   */
  addLocalRoute("10.0.0.0/8", {"100:1"}, 100, "", 2);

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpPeersForLocalRoutes();

  /*
   * Verify the route is NOT announced initially since no supporting
   * routes exist yet and minSupportingRoutes=2 is required.
   */
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));
}

/*
 * Test that a local route with minSupportingRoutes IS announced once
 * the required number of supporting (more specific) routes are added,
 * and is withdrawn when support drops below the threshold.
 * This tests the full lifecycle of min-support toggling via shadowRib state.
 */
TEST_F(E2ERibLocalRouteTest, LocalRouteAnnouncedWithMinSupport) {
  /*
   * Add a local route with minSupportingRoutes=2, meaning it should NOT
   * be announced until at least 2 supporting (more specific) routes exist.
   */
  addLocalRoute("10.0.0.0/8", {"100:1"}, 100, "", 2);

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpPeersForLocalRoutes();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto supportPrefix1 = folly::IPAddress::createNetwork("10.1.0.0/16");
  auto supportPrefix2 = folly::IPAddress::createNetwork("10.2.0.0/16");

  /*
   * Verify the local route is NOT in shadowRib initially since no
   * supporting routes exist yet and minSupportingRoutes=2 is required.
   */
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));

  /*
   * Add first supporting route - still below threshold (need 2).
   * Local route should still NOT be in shadowRib.
   */
  addRoute("v4", "10.1.0.0", 16, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(supportPrefix1));
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));

  /*
   * Add second supporting route - now at threshold (2 routes).
   * Local route should now appear in shadowRib.
   */
  addRoute("v4", "10.2.0.0", 16, kPeerAddr4, "11.0.0.2", "65002");
  ASSERT_TRUE(waitForRouteInShadowRib(supportPrefix2));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * Withdraw one supporting route - drops below threshold.
   * Local route should be removed from shadowRib.
   */
  deleteRoute("v4", "10.1.0.0", 16, kPeerAddr3);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.1.0.0/16"));
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));

  /*
   * Re-add the supporting route - back at threshold.
   * Local route should reappear in shadowRib.
   */
  addRoute("v4", "10.1.0.0", 16, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(supportPrefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
}

/*
 * Reproduce P1: Origin validation in createLocalRoute uses && instead of ||.
 * The condition "value < min && value > max" is always false, so invalid
 * origin values (e.g., 999) are never rejected and get cast to the enum.
 *
 * With the fix (&& -> ||), createLocalRoute returns nullopt for invalid
 * origins, so the local route is not installed.
 */
TEST_F(E2ERibLocalRouteTest, InvalidOriginRejected) {
  auto prefix = folly::IPAddress::createNetwork("192.168.99.0/24");

  thrift::BgpNetwork network;
  network.prefix() = "192.168.99.0/24";
  network.origin() = 999;

  localRoutes_[prefix] = std::move(network);

  addPeer(kDefaultPeerSpec3);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  /*
   * Check the RIB directly - the local route with invalid origin should
   * not have been loaded.
   */
  bool routeFound = false;
  rib_->getEventBase().runInEventBaseThreadAndWait([&]() {
    auto localRoutes = rib_->getLocalRoutes();
    routeFound = localRoutes.contains(prefix);
  });
  EXPECT_FALSE(routeFound)
      << "Local route with invalid origin=999 should be rejected";
}

} // namespace facebook::bgp
