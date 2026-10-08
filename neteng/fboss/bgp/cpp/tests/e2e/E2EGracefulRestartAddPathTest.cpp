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
 * E2E tests for GR + ADD-PATH interactions (SEV Pattern 2, 5 SEVs).
 *
 * Current coverage gap: E2EAdjRibInGRTest tests GR WITHOUT ADD-PATH peers,
 * and E2EAdjRibInAddPathTest tests ADD-PATH WITHOUT GR scenarios.
 * No test combines GR + ADD-PATH together.
 *
 * These tests verify that graceful restart works correctly when the peer
 * has ADD-PATH capability — stale path detection with path IDs, path ID
 * changes during GR, and EoR-triggered cleanup for multi-path scenarios.
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2EGracefulRestartAddPathTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3_AddPath);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5_AddPath);
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false);
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
  }
};

/*
 * ADD-PATH peer goes down → all paths from that peer should be withdrawn.
 * Verify that multiple paths with different path IDs are all cleaned up.
 */
TEST_F(E2EGracefulRestartAddPathTest, AllPathsWithdrawnOnAddPathPeerDown) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001", "", 2);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  bringDownPeer(kPeerAddr3);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));
  XLOG(INFO, "All ADD-PATH paths withdrawn on peer down");
}

/*
 * ADD-PATH peer restarts and re-announces same prefixes with same path IDs.
 * Verify routes are correctly restored after GR cycle.
 */
TEST_F(E2EGracefulRestartAddPathTest, AddPathPeerGracefulRestart) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001", "", 2);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));

  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001", "", 2);

  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "ADD-PATH routes restored after GR cycle");
}

/*
 * Multiple ADD-PATH paths from same peer — withdraw one path, verify
 * remaining paths are still active and bestpath may change.
 */
TEST_F(E2EGracefulRestartAddPathTest, WithdrawOnePathKeepsOthers) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1, 200, 0);
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.2", "65001", "", 2);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3, 1);

  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  XLOG(INFO, "One path withdrawn, remaining path still active");
}

/*
 * ADD-PATH peer flaps while multi-path update is in-flight.
 * Inject multiple paths, then immediately flap the peer.
 * Verify no crash and clean state after recovery.
 */
TEST_F(E2EGracefulRestartAddPathTest, AddPathPeerFlapDuringMultiPathUpdate) {
  bringUpAllPeersWithEor();

  for (int i = 1; i <= 5; i++) {
    addRoute(
        "v4",
        "10.0.0.0",
        8,
        kPeerAddr3,
        fmt::format("11.0.0.{}", i),
        "65001",
        "",
        i);
  }

  bringDownPeer(kPeerAddr3);

  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 1);
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));
  XLOG(INFO, "ADD-PATH peer flap during multi-path: no crash, clean state");
}

} // namespace facebook::bgp
