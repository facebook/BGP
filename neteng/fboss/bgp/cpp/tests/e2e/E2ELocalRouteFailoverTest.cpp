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
 * E2E tests for local route + remote route interaction during failures (Gap 8).
 *
 * Tests verify correct behavior when local routes and received routes
 * compete for the same prefix during peer failures and runtime injection.
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2ELocalRouteFailoverTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addLocalRoute("10.0.0.0/8");
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/false);
  }
};

/*
 * Local route present, then remote route arrives with same prefix.
 * Local route should still be advertised (local routes have priority).
 */
TEST_F(E2ELocalRouteFailoverTest, LocalRouteBeatsRemoteRoute) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  /*
   * Local route push to peer4 happens deterministically after EoR.
   * Unbounded blockingWait returns as soon as the push lands — no sleep,
   * no count-based polling.
   */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  /*
   * Drain the post-initial-dump EoRs left in peer4's queue. The peer
   * negotiates both AFIs (v4Nexthop + v6Nexthop in kDefaultPeerSpec4), and
   * AdjRib::sendPendingEoRs pushes one EoR per negotiated AFI. After
   * verifyRouteAdd consumes the v4 announcement, peer4's queue still
   * contains an IPv4 EoR and an IPv6 EoR. Drain them here so the
   * post-addRoute assertion below measures only messages produced by
   * addRoute, not stale initialization messages.
   */
  EXPECT_EQ(2, drainPeerQueueCompletely(peerId4));

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  /*
   * With changeListTracker always enabled (post D98538067), the incoming
   * remote route does NOT trigger a re-advertisement to peer4 because
   * the best-path is unchanged. We need to assert peer4's outbound queue
   * stays empty after any potential push work would have happened.
   * drainPeerQueueCompletely flushes rib_/peerManager_ event bases via
   * runInEventBaseThreadAndWait in a retry loop until the queue is
   * quiescent (deterministic sync — no sleep, no time-based polling).
   * Returning 0 proves the remote route did NOT trigger a push.
   */
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_EQ(0, drainPeerQueueCompletely(peerId4))
      << "peer4 should NOT receive a re-advertisement when local route "
         "still wins best-path after a worse remote route arrives";
  XLOG(INFO, "Local route beats remote route for same prefix");
}

/*
 * Local route present, remote route's peer goes down.
 * Local route should survive without interruption.
 */
TEST_F(E2ELocalRouteFailoverTest, LocalRouteSurvivesPeerDown) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 50, 0);

  bringDownPeer(kPeerAddr3);

  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  XLOG(INFO, "Local route survives peer down");
}

/*
 * Runtime local route injection while peers are active.
 * Inject a new local route at runtime, verify it appears.
 */
TEST_F(E2ELocalRouteFailoverTest, RuntimeLocalRouteInjection) {
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix20 = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix20));

  injectLocalRoutesAtRuntime({"30.0.0.0/8"});
  auto prefix30 = folly::IPAddress::createNetwork("30.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix30));
  XLOG(INFO, "Runtime local route injection successful");
}

} // namespace facebook::bgp
