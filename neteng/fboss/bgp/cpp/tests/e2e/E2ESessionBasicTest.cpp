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
 * Basic tests for E2ESessionTestFixture validation.
 * Verifies that session events flow through the real notifyCoroQueue pipeline
 * and produce the same results as the original E2ETestFixture.
 */

#include <gtest/gtest.h>

#include <string>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2ESessionBasicTest : public E2ESessionTestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/false);
  }
};

class E2ESessionAdditionalRemoteAsTest : public E2ESessionTestFixture {
 protected:
  void SetUp() override {
    auto peerSpec = kDefaultPeerSpec3;
    peerSpec.additionalRemoteAs = kPeerAsn4;
    peerSpec.enforceFirstAs = true;
    addPeer(peerSpec);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false,
        /*enableEgressBackpressure=*/false);
  }
};

TEST_F(
    E2ESessionAdditionalRemoteAsTest,
    AdditionalRemoteAsDrivesEnforceFirstAs) {
  const auto peerConfig = config_->getConfigOfAPeer(kPeerAddr3);
  ASSERT_TRUE(peerConfig.has_value());
  EXPECT_EQ(kPeerAsn3, peerConfig->peerAsn);
  EXPECT_EQ(kPeerAsn4, peerConfig->additionalRemoteAs);

  bringUpPeerAndWait(kPeerAddr3);

  const auto peeringParams = getAdjRibPeeringParams(kPeerAddr3);
  ASSERT_TRUE(peeringParams.has_value());
  EXPECT_EQ(kPeerAsn3, peeringParams->remoteAs);
  EXPECT_EQ(kPeerAsn4, peeringParams->additionalRemoteAs);
  EXPECT_TRUE(peeringParams->enforceFirstAs);

  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", std::to_string(kPeerAsn4));
  EXPECT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.0.0.0/8")));
}

/*
 * Verify basic session establishment through the coro queue pipeline.
 * Bring up peers, send a route, verify it is advertised to the other peer.
 */
TEST_F(E2ESessionBasicTest, BasicSessionEstablishAndRoute) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "Route propagated from peer3 to peer4 via session pipeline");
}

/*
 * Verify session termination withdraws routes.
 */
TEST_F(E2ESessionBasicTest, SessionTerminateWithdrawsRoutes) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  bringDownPeerAndWait(kPeerAddr3);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));
  XLOG(INFO, "Route withdrawn after peer3 session terminated");
}

/*
 * Verify session restart and route recovery.
 */
TEST_F(E2ESessionBasicTest, SessionRestartRecoversRoutes) {
  bringUpPeerAndWait(kPeerAddr3);
  bringUpPeerAndWait(kPeerAddr4);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));

  bringDownPeerAndWait(kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));

  bringUpPeerAndWait(kPeerAddr3);
  sendEoRToPeer(peerId3);

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr4, "127.5.0.3"));
  XLOG(INFO, "Route recovered after peer3 session restart");
}

} // namespace facebook::bgp
