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
 * E2E tests: P-DOWN state x Event Matrix
 * Tests for P-DOWN peer state against all applicable events.
 *
 * Prefix range: 11.x.0.0/16
 *
 * Tests: PeerDown state × route/block/threshold events
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
/*
 * P-DOWN x E-ROUTE-WD
 * Withdrawal while peer is DOWN — no effect on peer,
 * remaining peer receives the withdrawal normally.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_RouteWithdraw) {
  XLOG(INFO, "=== TEST: PeerDown_RouteWithdraw ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a route while both peers are up */
  injectLocalRoutesAtRuntime({"11.81.0.0/16"}, {"1181:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.81.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.81.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1181:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.81.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1181:1"));

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Withdraw the route while peer3 is DOWN */
  withdrawLocalRoutesAtRuntime({"11.81.0.0/16"});

  /* Peer4 should receive the withdrawal normally */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "11.81.0.0", 16, kPeerAddr4));

  /* Peer3 stays DOWN */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 still functional */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  XLOG(INFO, "=== TEST PASSED: PeerDown_RouteWithdraw ===");
}

/*
 * P-DOWN x E-UNBLOCK
 * N/A — verify no crash when unblocking a DOWN peer.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_Unblock) {
  XLOG(INFO, "=== TEST: PeerDown_Unblock ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Unblock a DOWN peer — should be a no-op, no crash */
  unblockPeer(kPeerAddr3);

  /* Peer3 stays DOWN */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 unaffected — inject a route to confirm */
  injectLocalRoutesAtRuntime({"11.83.0.0/16"}, {"1183:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.83.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.83.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1183:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: PeerDown_Unblock ===");
}

/*
 * P-DOWN x E-SLOW-DUR
 * N/A — timer should not exist for DOWN peer. A DOWN peer is unregistered
 * from its update group and drops its reference to it, so there is nothing
 * for slow peer detection to run against. Group continues functioning for
 * the remaining peer.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_SlowDur) {
  XLOG(INFO, "=== TEST: PeerDown_SlowDur ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /*
   * AdjRibOutGroup::unregisterPeer drops the peer's reference to its group,
   * so a DOWN peer has no group at all -- there is nothing to configure slow
   * peer thresholds on, and hence no duration timer. Read on the PeerManager
   * evb so this is serialized against the unregister that cleared it.
   */
  auto& pmEvb = peerManager_->getEventBase();
  auto groupAfterDown =
      folly::via(&pmEvb, [this]() -> std::shared_ptr<AdjRibOutGroup> {
        return getUpdateGroupForPeer(kPeerAddr3);
      }).get();
  EXPECT_EQ(groupAfterDown, nullptr);

  /* Peer3 stays DOWN */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 unaffected — inject a route to confirm group still works */
  injectLocalRoutesAtRuntime({"11.84.0.0/16"}, {"1184:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.84.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.84.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1184:1"));

  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: PeerDown_SlowDur ===");
}

/*
 * P-DOWN x E-SLOW-FREQ
 * N/A — no block tracking for a DOWN peer. Verify that injecting
 * routes (which would trigger frequency checks for a live peer)
 * has no effect on the DOWN peer and the group continues normally.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerDown_SlowFreq) {
  XLOG(INFO, "=== TEST: PeerDown_SlowFreq ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Set aggressive frequency thresholds before bringing peer3 down */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(60000),
      1,
      std::chrono::milliseconds(5000));

  /* Bring peer3 down */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /*
   * Inject multiple routes with different communities — this would trigger
   * frequency-based detection for a live peer, but should be a no-op for
   * DOWN.
   */
  injectLocalRoutesAtRuntime({"11.85.0.0/16"}, {"1185:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.85.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.85.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1185:1"));

  injectLocalRoutesAtRuntime({"11.86.0.0/16"}, {"1186:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("11.86.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.86.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1186:1"));

  /* Peer3 stays DOWN — frequency detection is irrelevant for DOWN peer */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 still functional */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOG(INFO, "=== TEST PASSED: PeerDown_SlowFreq ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
