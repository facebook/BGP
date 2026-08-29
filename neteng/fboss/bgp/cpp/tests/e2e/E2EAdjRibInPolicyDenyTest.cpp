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
 * E2E tests for AdjRibIn ingress policy deny behavior.
 *
 * These tests verify that routes denied by ingress policy are NOT propagated
 * to peers. This is E2E observable behavior (route in -> no route out).
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib, PolicyManager
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2EAdjRibInPolicyDenyTest : public E2ETestFixture {
 protected:
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
 * Test: Route denied by prefix policy is NOT propagated
 *
 * Setup:
 *   - Configure ingress policy to deny 10.0.0.0/8
 *   - Peer 3 announces 10.0.0.0/8
 *
 * Verify:
 *   - Route does NOT appear in shadow RIB
 */
TEST_F(E2EAdjRibInPolicyDenyTest, DenyByPrefixNotPropagated) {
  /*
   * Configure policy BEFORE creating RIB and PeerManagerBase
   * Policy denies 10.0.0.0/8
   */
  addPrefixDenyPolicy({"10.0.0.0/8"});

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpAllPeersWithEor();

  /* Announce route that should be denied */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  /* Route should NOT appear in shadow RIB (policy denied it) */
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));
}

/*
 * Test: Non-matching route IS propagated (deny policy only affects matches)
 *
 * Setup:
 *   - Configure ingress policy to deny 10.0.0.0/8
 *   - Peer 3 announces 20.0.0.0/8 (not matching deny)
 *
 * Verify:
 *   - Route DOES appear in shadow RIB
 *   - Route IS announced to peers 4 and 5
 */
TEST_F(E2EAdjRibInPolicyDenyTest, NonMatchingRouteIsPropagated) {
  addPrefixDenyPolicy({"10.0.0.0/8"});

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpAllPeersWithEor();

  /* Announce route that should NOT be denied */
  addRoute("v4", "20.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  /* Route SHOULD appear in shadow RIB */
  auto prefix = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Route should be announced to peers */
  EXPECT_TRUE(verifyRouteAdd("v4", "20.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Route denied by community policy is NOT propagated
 *
 * Setup:
 *   - Configure ingress policy to deny routes with community 65000:666
 *   - Peer 3 announces route with that community
 *
 * Verify:
 *   - Route does NOT appear in shadow RIB
 */
TEST_F(E2EAdjRibInPolicyDenyTest, DenyByCommunityNotPropagated) {
  addCommunityDenyPolicy("65000:666");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpAllPeersWithEor();

  /* Announce route with community that should be denied */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "65000:666", 0);

  /* Route should NOT appear in shadow RIB */
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  EXPECT_TRUE(verifyRouteNotInShadowRib(prefix));
}

/*
 * Test: Route without matching community IS propagated
 *
 * Setup:
 *   - Configure ingress policy to deny routes with community 65000:666
 *   - Peer 3 announces route with different community 65000:777
 *
 * Verify:
 *   - Route DOES appear in shadow RIB
 *   - Route IS announced to peers
 */
TEST_F(E2EAdjRibInPolicyDenyTest, NonMatchingCommunityIsPropagated) {
  addCommunityDenyPolicy("65000:666");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  createRib();
  createPeerManager(/*enableUpdateGroup=*/false,
                    /*enableEgressBackpressure=*/true);

  bringUpAllPeersWithEor();

  /* Announce route with different community */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "65000:777", 0);

  /* Route SHOULD appear in shadow RIB */
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Route should be announced to peers */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

} /* namespace facebook::bgp */
