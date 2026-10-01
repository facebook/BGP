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

/* E2E tests: Route Refresh Interactions (PART 11)
 * Prefix range: 30.x.0.0/16
 *
 * Route refresh for JOINED_RUNNING peer — real RFC 2918 request
 * Route refresh for JOINED_BLOCKED peer — wait for the group push to resolve
 * Route refresh for DETACHED_BLOCKED peer — already in detached mode
 * Route refresh for DETACHED_INIT_DUMP peer — defer until init complete
 * Route refresh during acceptance procedure — peer in DRJ
 * Route refresh for all peers simultaneously — burst to all
 *
 * The running and blocked cases use real BGP Route Refresh messages. The
 * remaining state cases are migrated to real requests in the state-completion
 * diffs.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

class UpdateGroupRouteRefreshTest : public UpdateGroupMultiPeerTest {
 protected:
  void waitForRibDumpScheduled(
      const folly::IPAddress& peerAddr,
      bool expected) {
    WITH_RETRIES_N(60, {
      EXPECT_EVENTUALLY_EQ(
          folly::via(
              &peerManager_->getEventBase(),
              [this, peerAddr]() {
                return getAdjRib(peerAddr)->isRibDumpScheduled();
              })
              .get(),
          expected);
    });
  }

  bool drainUntilState(
      const folly::IPAddress& peerAddr,
      const BgpPeerId& peerId,
      PeerUpdateState expectedState) {
    for (int i = 0; i < 20; ++i) {
      drainPeerQueueCompletely(peerId, 1, 100);
      if (getPeerState(peerAddr) == expectedState) {
        break;
      }
    }
    return waitForPeerState(peerAddr, expectedState);
  }
};

/*
 * A real Route Refresh from a joined peer detaches only that requester, sends
 * the requested-AFI replay over its private lane, and rejoins it. The sibling
 * stays in sync and must not receive the replay.
 */
TEST_P(UpdateGroupRouteRefreshTest, JoinedRunning_RouteRefreshRequesterOnly) {
  XLOGF(INFO, "=== TEST: JoinedRunning_RouteRefreshRequesterOnly ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  const auto prefix = folly::IPAddress::createNetwork("30.1.0.0/16");
  injectLocalRoutesAtRuntime({"30.1.0.0/16"}, {"3001:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3001:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3001:1"));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  sendRouteRefreshToPeer(
      peerId3, BgpUpdateAfi::AFI_IPv4, BgpUpdateSafi::SAFI_UNICAST);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3001:1"));
  EXPECT_EQ(drainPeerQueueCompletely(peerId4, 3, 10), 0)
      << "sibling update-group peer received the requester-only replay";

  drainPeerQueueCompletely(peerId3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: JoinedRunning_RouteRefreshRequesterOnly ===");
}

TEST_P(UpdateGroupRouteRefreshTest, JoinedBlocked_RouteRefreshWaitsForUnblock) {
  XLOGF(INFO, "=== TEST: JoinedBlocked_RouteRefreshWaitsForUnblock ===");

  /*
   * Establish a clean baseline route on both peers, then drain their queues so
   * any later copy of replayPrefix is attributable to Route Refresh.
   */
  const auto peerIds = setupTwoPeersJoined();
  const auto& peerId3 = peerIds.peerId3;
  const auto& peerId4 = peerIds.peerId4;
  const auto replayPrefix = folly::IPAddress::createNetwork("30.10.0.0/16");
  injectLocalRoutesAtRuntime({"30.10.0.0/16"}, {"3010:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(replayPrefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3010:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3010:1"));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /*
   * Keep peer3 joined despite blocking: the high thresholds prevent slow-peer
   * detachment while the route burst fills its queue. Peer4 remains the
   * unblocked sibling and consumes the same group updates.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1000000,
      std::chrono::milliseconds(600000));
  blockPeer(kPeerAddr3);
  for (int i = 11; i <= 13; ++i) {
    const auto prefix = fmt::format("30.{}.0.0/16", i);
    const auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  ASSERT_TRUE(isPeerInSync(kPeerAddr3));
  drainPeerQueueCompletely(peerId4);

  /*
   * Hold the private dump so the Route Refresh transition is observable. The
   * request must detach peer3 while preserving its blocked state.
   */
  testOnlyDeferInitDump(kPeerAddr3, true);
  sendRouteRefreshToPeer(
      peerId3, BgpUpdateAfi::AFI_IPv4, BgpUpdateSafi::SAFI_UNICAST);

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  waitForRibDumpScheduled(kPeerAddr3, true);

  /*
   * Let the replay materialize while peer3 is still blocked. It must remain on
   * peer3's private lane and produce no messages for peer4.
   */
  testOnlyDeferInitDump(kPeerAddr3, false);
  waitForRibDumpScheduled(kPeerAddr3, false);
  EXPECT_EQ(drainPeerQueueCompletely(peerId4, 3, 10), 0);

  /*
   * Unblocking activates detached sending. Verify exactly one requested replay
   * and then drain private catch-up until peer3 rejoins the group.
   */
  unblockPeer(kPeerAddr3, /*maxRetries=*/0);
  const auto replayCounts = countPrefixOccurrencesAndDrain(
      peerId3,
      replayPrefix,
      /*isV4=*/true,
      /*maxRetries=*/10,
      /*maxMessages=*/100);
  EXPECT_EQ(replayCounts.announceCount, 1);
  EXPECT_EQ(replayCounts.withdrawCount, 0);
  ASSERT_TRUE(
      drainUntilState(kPeerAddr3, peerId3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: JoinedBlocked_RouteRefreshWaitsForUnblock ===");
}

TEST_P(
    UpdateGroupRouteRefreshTest,
    DetachedBlocked_RouteRefreshUsesPrivateLane) {
  XLOGF(INFO, "=== TEST: DetachedBlocked_RouteRefreshUsesPrivateLane ===");

  /*
   * Establish and drain a baseline route so its next appearance can only be
   * the requested replay.
   */
  const auto peerIds = setupTwoPeersJoined();
  const auto& peerId3 = peerIds.peerId3;
  const auto& peerId4 = peerIds.peerId4;
  const auto replayPrefix = folly::IPAddress::createNetwork("30.20.0.0/16");
  injectLocalRoutesAtRuntime({"30.20.0.0/16"}, {"3020:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(replayPrefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3020:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3020:1"));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  /*
   * Force peer3 onto the detached blocked lane before Route Refresh. A block
   * count threshold of one makes the first blocked update detach it, while
   * peer4 continues receiving group updates.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  for (int i = 21; i <= 23; ++i) {
    const auto prefix = fmt::format("30.{}.0.0/16", i);
    const auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  drainPeerQueueCompletely(peerId4);

  /*
   * Issue a real Route Refresh while peer3 is already detached. Hold its dump
   * long enough to verify that no second detachment or sibling replay occurs.
   */
  testOnlyDeferInitDump(kPeerAddr3, true);
  sendRouteRefreshToPeer(
      peerId3, BgpUpdateAfi::AFI_IPv4, BgpUpdateSafi::SAFI_UNICAST);

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  waitForRibDumpScheduled(kPeerAddr3, true);

  /*
   * Complete the dump while peer3 remains blocked; the replay is materialized
   * on its private packing list but cannot be transmitted yet.
   */
  testOnlyDeferInitDump(kPeerAddr3, false);
  waitForRibDumpScheduled(kPeerAddr3, false);
  EXPECT_EQ(drainPeerQueueCompletely(peerId4, 3, 10), 0);

  /*
   * Unblock the private lane, verify exactly one replay, and drain catch-up
   * until peer3 rejoins the group.
   */
  unblockPeer(kPeerAddr3, /*maxRetries=*/0);
  const auto replayCounts = countPrefixOccurrencesAndDrain(
      peerId3,
      replayPrefix,
      /*isV4=*/true,
      /*maxRetries=*/10,
      /*maxMessages=*/100);
  EXPECT_EQ(replayCounts.announceCount, 1);
  EXPECT_EQ(replayCounts.withdrawCount, 0);
  ASSERT_TRUE(
      drainUntilState(kPeerAddr3, peerId3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(
      INFO, "=== TEST PASSED: DetachedBlocked_RouteRefreshUsesPrivateLane ===");
}

/*
 * A real Route Refresh received during a reconnecting peer's private initial
 * dump is retained behind that dump. The route is therefore announced once by
 * initial synchronization and once by the requested replay.
 */
TEST_P(UpdateGroupRouteRefreshTest, DetachedInitDump_RouteRefreshDefer) {
  XLOG(INFO, "=== TEST: DetachedInitDump_RouteRefreshDefer ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.20.0.0/16"}, {"3020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3020:1"));
  injectLocalRoutesAtRuntime({"30.21.0.0/16"}, {"3021:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3021:1"));
  /* 3rd fill route to ensure queue > hwm=2 */
  injectLocalRoutesAtRuntime({"30.25.0.0/16"}, {"3025:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.25.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.25.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3025:1"));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Bring peer3 down then back up to get DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  /*
   * Restore a roomy queue after the small queue forced detachment, so the
   * reconnect initial dump and deferred replay can drain without re-blocking.
   */
  setDefaultQueueSizes(20, 18, 2);
  testOnlyDeferInitDump(kPeerAddr3, true);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);
  /* Peer3 re-entering existing group -> DETACHED_INIT_DUMP */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_INIT_DUMP));
  drainPeerQueueCompletely(peerId4);

  sendRouteRefreshToPeer(
      peerId3, BgpUpdateAfi::AFI_IPv4, BgpUpdateSafi::SAFI_UNICAST);
  waitForRibDumpScheduled(kPeerAddr3, true);
  testOnlyDeferInitDump(kPeerAddr3, false);
  const auto replayCounts = countPrefixOccurrencesAndDrain(
      peerId3,
      folly::IPAddress::createNetwork("30.20.0.0/16"),
      /*isV4=*/true,
      /*maxRetries=*/20,
      /*maxMessages=*/100);
  EXPECT_EQ(replayCounts.announceCount, 2)
      << "initial dump and deferred Route Refresh must each announce the route";
  EXPECT_EQ(replayCounts.withdrawCount, 0);
  ASSERT_TRUE(
      drainUntilState(kPeerAddr3, peerId3, PeerUpdateState::JOINED_RUNNING));

  EXPECT_EQ(drainPeerQueueCompletely(peerId4, 3, 10), 0);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: DetachedInitDump_RouteRefreshDefer ===");
}

/*
 * A real Route Refresh interrupts ready-to-join acceptance, returns the peer to
 * detached processing, and permits rejoin only after the replay drains.
 */
TEST_P(UpdateGroupRouteRefreshTest, ReadyToJoin_RouteRefreshDefersAcceptance) {
  XLOGF(INFO, "=== TEST: ReadyToJoin_RouteRefreshDefersAcceptance ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.30.0.0/16"}, {"3030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3030:1"));
  injectLocalRoutesAtRuntime({"30.31.0.0/16"}, {"3031:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3031:1"));
  /* 3rd fill route to ensure queue > hwm=2 */
  injectLocalRoutesAtRuntime({"30.35.0.0/16"}, {"3035:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.35.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.35.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3035:1"));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  testOnlyDeferDrjAcceptance(kPeerAddr3, true);
  unblockPeer(kPeerAddr3, /*maxRetries=*/0);
  ASSERT_TRUE(drainUntilState(
      kPeerAddr3, peerId3, PeerUpdateState::DETACHED_READY_TO_JOIN));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  testOnlyDeferInitDump(kPeerAddr3, true);
  sendRouteRefreshToPeer(
      peerId3, BgpUpdateAfi::AFI_IPv4, BgpUpdateSafi::SAFI_UNICAST);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_RUNNING));
  waitForRibDumpScheduled(kPeerAddr3, true);
  testOnlyDeferInitDump(kPeerAddr3, false);
  waitForRibDumpScheduled(kPeerAddr3, false);

  const auto replayCounts = countPrefixOccurrencesAndDrain(
      peerId3,
      folly::IPAddress::createNetwork("30.30.0.0/16"),
      /*isV4=*/true,
      /*maxRetries=*/10,
      /*maxMessages=*/100);
  EXPECT_EQ(replayCounts.announceCount, 1);
  EXPECT_EQ(replayCounts.withdrawCount, 0);
  EXPECT_EQ(drainPeerQueueCompletely(peerId4, 3, 10), 0);
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_READY_TO_JOIN));

  testOnlyDeferDrjAcceptance(kPeerAddr3, false);
  ASSERT_TRUE(
      drainUntilState(kPeerAddr3, peerId3, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: ReadyToJoin_RouteRefreshDefersAcceptance ===");
}

/*
 * Route refresh for all peers simultaneously
 * 3-peer setup. Simulate route refresh burst to all peers. Verify all 3
 * receive routes and remain JOINED_RUNNING. No accidental blocking with
 * larger queue (8,6,2).
 */
TEST_P(UpdateGroupRouteRefreshTest, AllPeers_RouteRefreshBurst) {
  XLOG(INFO, "=== TEST: AllPeers_RouteRefreshBurst ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);
  setupSlowPeerComponents(8, 6, 2);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  EXPECT_TRUE(waitForEoR(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Simulate route refresh burst: 3 routes, inject-drain one at a time */
  for (int i = 40; i <= 42; ++i) {
    auto prefix = fmt::format("30.{}.0.0/16", i);
    auto community = fmt::format("30{:02d}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr3,
        getExpectedNexthop(kPeerAddr3),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr5,
        getExpectedNexthop(kPeerAddr5),
        "4200000001",
        community));
  }

  /* All 3 peers still JOINED_RUNNING */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  verifySlowPeerInvariants(kPeerAddr3);
  verifySlowPeerInvariants(kPeerAddr4);
  verifySlowPeerInvariants(kPeerAddr5);

  XLOG(INFO, "=== TEST PASSED: AllPeers_RouteRefreshBurst ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupRouteRefreshTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
