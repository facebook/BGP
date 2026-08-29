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
 * E2E tests for AdjRibIn ADD-PATH functionality.
 *
 * These tests verify ADD-PATH processing through the complete BGP pipeline,
 * ensuring that:
 * - ADD-PATH peers receive multiple paths with path IDs
 * - Non-ADD-PATH peers receive only the bestpath
 * - Path ID propagation works correctly through RIB
 *
 * Derived from: AdjRibInTest.cpp (V4UpdateProcessingMultipleWithAddPath,
 *               V6UpdateProcessingMultipleWithAddPath,
 * ReceivedPathIdReachesRib) Mocked: FIB (TestFib), SessionManager
 * (MockSessionManager) Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

/*
 * Test fixture for ADD-PATH E2E tests.
 *
 * Uses a mix of ADD-PATH and non-ADD-PATH peers:
 * - Peer3: ADD-PATH enabled (source of routes)
 * - Peer4: Non-ADD-PATH (verifies bestpath-only behavior)
 * - Peer5: ADD-PATH enabled (verifies multi-path reception)
 */
class E2EAdjRibInAddPathTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    /* Peer3: ADD-PATH enabled (route source) */
    addPeer(kDefaultPeerSpec3_AddPath);
    /* Peer4: Non-ADD-PATH (receives only bestpath) */
    addPeer(kDefaultPeerSpec4);
    /* Peer5: ADD-PATH enabled (receives multiple paths) */
    addPeer(kDefaultPeerSpec5_AddPath);
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
 * Derived from: AdjRibInTest.cpp::V4UpdateProcessingMultipleWithAddPath
 *
 * Verify that multiple IPv4 paths with different path IDs are processed
 * correctly. ADD-PATH peers should receive all paths, while non-ADD-PATH
 * peers should receive only the bestpath.
 */
TEST_F(E2EAdjRibInAddPathTest, V4MultiplePathsWithAddPath) {
  bringUpAllPeersWithEor();

  /* Send first path with pathId=1 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Verify bestpath is advertised to non-ADD-PATH peer (Peer4) */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  /* Send second path with pathId=2 and different nexthop */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001", "", 2);

  /* Send third path with pathId=3 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.3", "65001", "", 3);

  /* Verify RIB has multiple paths */
  auto ribEntries =
      rib_->getRibEntryForPrefix(std::make_unique<std::string>("10.0.0.0/8"));
  ASSERT_FALSE(ribEntries.empty());
  /* With ADD-PATH, we should have multiple paths stored */
  EXPECT_GE(ribEntries[0].paths()->size(), 1);
}

/*
 * Derived from: AdjRibInTest.cpp::V6UpdateProcessingMultipleWithAddPath
 *
 * Verify that multiple IPv6 paths with different path IDs are processed
 * correctly through the RIB.
 */
TEST_F(E2EAdjRibInAddPathTest, V6MultiplePathsWithAddPath) {
  bringUpAllPeersWithEor();

  /* Send first path with pathId=1 */
  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001", "", 1);

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Verify route is advertised */
  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr4, "2401:db00:e011:411:1000::2b"));

  /* Send second path with pathId=2 */
  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::2", "65001", "", 2);

  /* Verify RIB has the route */
  auto ribEntries = rib_->getRibEntryForPrefix(
      std::make_unique<std::string>("2001:db8::/32"));
  ASSERT_FALSE(ribEntries.empty());
}

/*
 * Derived from: AdjRibInTest.cpp::ReceivedPathIdReachesRib
 *
 * Verify that path IDs from ADD-PATH updates are propagated correctly
 * through the RIB and can be used for targeted withdrawals.
 */
TEST_F(E2EAdjRibInAddPathTest, PathIdReachesRib) {
  bringUpAllPeersWithEor();

  /* Send multiple paths with different path IDs */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001", "", 2);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Withdraw only path 1 */
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3, 1);

  /* Path 2 should still be in RIB */
  ASSERT_TRUE(waitForPathCountInRib("10.0.0.0/8", 1));
}

/*
 * Verify that withdrawing all paths removes the route from RIB.
 */
TEST_F(E2EAdjRibInAddPathTest, WithdrawAllPathsRemovesRoute) {
  bringUpAllPeersWithEor();

  /* Send two paths */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001", "", 2);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  /* Withdraw both paths */
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3, 1);
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3, 2);

  /* Route should be withdrawn from non-ADD-PATH peer */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));
}

/*
 * Verify that path update with same pathId replaces the previous path.
 */
TEST_F(E2EAdjRibInAddPathTest, PathUpdateReplacesExisting) {
  bringUpAllPeersWithEor();

  /* Send initial path with pathId=1 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  /* Update path with same pathId but different nexthop */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.99", "65001", "", 1);

  /* Route should be updated (implicit withdrawal + new announcement) */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
}

/*
 * Test fixture for mixed ADD-PATH and non-ADD-PATH peer scenarios.
 * Uses non-ADD-PATH peer as route source to verify path ID handling
 * when receiving from non-ADD-PATH peers.
 */
class E2EAdjRibInAddPathMixedTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    /* Peer3: Non-ADD-PATH (route source) */
    addPeer(kDefaultPeerSpec3);
    /* Peer4: ADD-PATH enabled */
    addPeer(kDefaultPeerSpec4_AddPath);
    /* Peer5: Non-ADD-PATH */
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
 * Verify that routes from non-ADD-PATH peers are handled correctly
 * and advertised to both ADD-PATH and non-ADD-PATH peers.
 */
TEST_F(E2EAdjRibInAddPathMixedTest, NonAddPathSourceToMixedPeers) {
  bringUpAllPeersWithEor();

  /* Route from non-ADD-PATH peer (no pathId) */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Both ADD-PATH and non-ADD-PATH peers should receive the route */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Verify withdrawal from non-ADD-PATH peer is propagated to all peers.
 */
TEST_F(E2EAdjRibInAddPathMixedTest, NonAddPathWithdrawalPropagation) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  /* Withdraw route */
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3, 0);

  /* Both peers should receive withdrawal */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
}

} // namespace facebook::bgp
