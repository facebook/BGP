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
 * E2E coverage for the add-path (PathTree) branch of
 * AdjRibOutGroup::cleanUpPeerRibOut.
 *
 * A detached peer's egress prefix counts are seeded from the group at detach
 * (copyEgressPrefixCountsFrom), so at peer-down the cleanup must settle every
 * path the peer advertised -- both the per-peer entries it diverged on and the
 * group-owned entries it still shares. Under update groups the
 * AdjRib::sessionTerminated teardown loop is skipped (it is gated on
 * !enableUpdateGroup_), so this is the single accounting point: anything it
 * misses leaks out of the peer's own counts and permanently out of the global
 * totalSentPrefixCount, which trips the "Non-zero egress prefix counts on
 * session establishment" ERR when the peer reconnects.
 *
 * The peer under test is deliberately left holding BOTH kinds of entry when it
 * goes down: paths re-announced after the detach (lazily cloned under its own
 * owner key) and prefixes untouched since the detach (still group-owned and
 * shared). Divergence is per (prefix, pathId), so with add-path the two can
 * even meet at a single prefix.
 *
 * These are also the first E2E tests to combine update groups with add-path --
 * the pre-existing add-path E2E tests all run with enableUpdateGroup=false, so
 * the group's PathTree had no end-to-end coverage at all.
 *
 * peer3 sources the add-path routes, peer4 detaches and goes down, peer5 stays
 * in sync so the group is never left frozen with no sync peers. All three
 * negotiate add-path, so they share one update group backed by the PathTree.
 */

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStats.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

namespace {
/* The multipath prefix: two equal-cost paths, advertised with add-path IDs. */
constexpr auto kMultiPathPrefix = "30.1.0.0";
constexpr auto kMultiPathPrefixStr = "30.1.0.0/16";
constexpr uint8_t kMultiPathPrefixLen = 16;
constexpr auto kPathNexthop1 = "11.0.0.1";
constexpr auto kPathNexthop2 = "11.0.0.2";
constexpr uint32_t kRecvPathId1 = 1;
constexpr uint32_t kRecvPathId2 = 2;

/* Equal local-pref on both paths so neither wins and both join the ECMP set. */
constexpr uint32_t kEqualLocalPref = 100;

/* Local routes announced before the detach and left untouched afterwards, so
 * the detached peer keeps sharing the group's entries for them. */
constexpr int kNumSharedRoutes = 5;

/* Enough local routes to overrun peer4's queue and drive it into detachment. */
constexpr int kNumPlugRoutes = 4;

constexpr int kRetries = 30;

std::string sharedPrefix(int index) {
  return fmt::format("31.{}.0.0/16", index);
}
std::string plugPrefix(int index) {
  return fmt::format("32.{}.0.0/16", index);
}
} // namespace

class UpdateGroupAddPathDetachedPeerDownTest : public SlowPeerTestBase {
 protected:
  /*
   * How a detached peer's RIB-OUT paths are split between entries it owns and
   * group entries it still shares. mixedNodes counts prefixes holding both --
   * the case where cleanup has to walk past the peer's own paths to reach the
   * shared ones.
   */
  struct PathOwnership {
    uint32_t peerOwned{0};
    uint32_t shared{0};
    uint32_t mixedNodes{0};
  };

  /*
   * Classify the group's PathTree from the peer's point of view, mirroring
   * resolvePathEntriesForPeer. Runs on the PeerManager event base: the tree is
   * mutated there by the group's announce/withdraw and detach paths, so an
   * off-evb walk races those writes under TSan.
   */
  PathOwnership getPathOwnership(const folly::IPAddress& peerAddr) {
    auto& evb = peerManager_->getEventBase();
    return folly::via(
               &evb,
               [this, peerAddr]() -> PathOwnership {
                 PathOwnership out;
                 auto adjRib = getAdjRib(peerAddr);
                 auto group = getUpdateGroupForPeer(peerAddr);
                 if (!adjRib || !group) {
                   return out;
                 }
                 const auto peerOwnerKey = adjRib->getPeerOwnerKey();
                 const auto groupOwnerKey = group->getGroupOwnerKey();
                 const auto sharingVersion = adjRib->getRibOutSharingVersion();
                 for (auto itr = group->PathTree_.begin();
                      itr != group->PathTree_.end();
                      ++itr) {
                   const auto& ownerMap = itr->value();
                   auto peerIt = ownerMap.find(peerOwnerKey);
                   const uint32_t ownedHere =
                       peerIt == ownerMap.end() ? 0 : peerIt->second.size();
                   out.peerOwned += ownedHere;

                   auto groupIt = ownerMap.find(groupOwnerKey);
                   if (groupIt == ownerMap.end()) {
                     continue;
                   }
                   uint32_t sharedHere = 0;
                   for (const auto& [pathId, entry] : groupIt->second) {
                     const bool peerHasOwnEntry = peerIt != ownerMap.end() &&
                         peerIt->second.find(pathId) != peerIt->second.end();
                     if (AdjRibOutGroup::isEntryShared(
                             peerHasOwnEntry,
                             sharingVersion,
                             entry->getRibVersion())) {
                       ++sharedHere;
                     }
                   }
                   out.shared += sharedHere;
                   if (ownedHere > 0 && sharedHere > 0) {
                     ++out.mixedNodes;
                   }
                 }
                 return out;
               })
        .get();
  }

  /*
   * Read a peer's egress prefix counts on the PeerManager event base, for the
   * same reason as above.
   */
  std::pair<uint32_t, uint32_t> getEgressPrefixCounts(
      const folly::IPAddress& peerAddr) {
    auto& evb = peerManager_->getEventBase();
    return folly::via(
               &evb,
               [this, peerAddr]() -> std::pair<uint32_t, uint32_t> {
                 auto adjRib = getAdjRib(peerAddr);
                 if (!adjRib) {
                   return {0, 0};
                 }
                 return {
                     adjRib->getStats().getPostOutPrefixCount(),
                     adjRib->getStats().getPreOutPrefixCount()};
               })
        .get();
  }
};

/*
 * peer4 detaches while holding a mix of shared and diverged RIB-OUT paths,
 * then its session goes down. Its own egress counts must settle to zero and
 * the global total must drop by exactly its share -- no more, no less.
 */
TEST_P(
    UpdateGroupAddPathDetachedPeerDownTest,
    DetachedAddPathPeerDownSettlesEgressPrefixCounts) {
  addPeer(kDefaultPeerSpec3_AddPath);
  addPeer(kDefaultPeerSpec4_AddPath);
  addPeer(kDefaultPeerSpec5_AddPath);

  setDefaultQueueSizes(/*capacity=*/100, /*highWm=*/80, /*lowWm=*/2);
  /* Only peer4 gets a tiny queue -- it is the one that must block. */
  setQueueSizeForPeer(kPeerAddr4, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  setupComponents();

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
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Two equal-cost paths at one prefix -> two PathTree entries in the group. */
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop1,
      "65001" /* asPath */,
      "3010:1" /* community */,
      kRecvPathId1,
      kEqualLocalPref);
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop2,
      "65001" /* asPath */,
      "3010:1" /* community */,
      kRecvPathId2,
      kEqualLocalPref);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork(kMultiPathPrefixStr)));
  ASSERT_TRUE(waitForMultipathNexthopCount(kMultiPathPrefixStr, 2))
      << "the two add-path routes did not both join the ECMP set";

  /* Prefixes peer4 will still be sharing with the group when it goes down:
   * announced before the detach and never touched again. */
  for (int i = 1; i <= kNumSharedRoutes; ++i) {
    injectLocalRoutesAtRuntime(
        {sharedPrefix(i)}, {fmt::format("311{}:1", i)}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(
        folly::IPAddress::createNetwork(sharedPrefix(i))));
  }
  drainPeerQueueCompletely(peerId4, 5, 200);
  drainPeerQueueCompletely(peerId5, 5, 200);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer4: a block-count threshold of 1 detaches it on the first
   * backpressure event, so plugging its queue is enough. */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      /*countThreshold=*/1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 1; i <= kNumPlugRoutes; ++i) {
    injectLocalRoutesAtRuntime(
        {plugPrefix(i)}, {fmt::format("320{}:1", i)}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(
        folly::IPAddress::createNetwork(plugPrefix(i))));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(isPeerDetached(kPeerAddr4));
  drainPeerQueueCompletely(peerId5, 5, 200);

  /* Re-announce the multipath prefix while peer4 is detached. The group
   * mutates its existing PathTree entries, which lazily clones them under
   * peer4's own owner key. */
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop1,
      "65001" /* asPath */,
      "3010:2" /* community */,
      kRecvPathId1,
      kEqualLocalPref);
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop2,
      "65001" /* asPath */,
      "3010:2" /* community */,
      kRecvPathId2,
      kEqualLocalPref);

  /*
   * Now retire and re-originate path 1 on the same nexthop. The withdraw
   * erases the RouteInfo, so the re-advertisement allocates a fresh
   * pathIdToSend instead of reusing the old one -- and because the RIB emits
   * add-path withdrawals keyed on nexthop, the unchanged nexthop set means the
   * retired pathId is never withdrawn from the group. That is the ordinary
   * churn that leaves peer4 owning a clone of one pathId at this prefix while
   * still sharing another at that same prefix.
   */
  deleteRoute(
      "v4", kMultiPathPrefix, kMultiPathPrefixLen, kPeerAddr3, kRecvPathId1);
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop1,
      "65001" /* asPath */,
      "3010:3" /* community */,
      kRecvPathId1,
      kEqualLocalPref);

  PathOwnership ownership;
  WITH_RETRIES_N(kRetries, {
    drainPeerQueueCompletely(peerId5, 1, 200);
    ownership = getPathOwnership(kPeerAddr4);
    EXPECT_EVENTUALLY_GT(ownership.peerOwned, 0u);
  });

  XLOGF(
      INFO,
      "peer4 RIB-OUT before peer-down: {} peer-owned paths, {} shared with "
      "the group, {} prefixes holding both",
      ownership.peerOwned,
      ownership.shared,
      ownership.mixedNodes);
  ASSERT_GT(ownership.peerOwned, 0u)
      << "peer4 never diverged, so the per-peer cleanup branch is untested";
  ASSERT_GT(ownership.shared, 0u)
      << "peer4 shares nothing with the group, so the shared cleanup branch "
         "is untested";
  ASSERT_GT(ownership.mixedNodes, 0u)
      << "no prefix holds both a peer-owned and a shared path, so cleanup is "
         "never forced to walk past the peer's own paths to the shared ones";

  const auto [postOutBefore, preOutBefore] = getEgressPrefixCounts(kPeerAddr4);
  ASSERT_GT(postOutBefore, 0u)
      << "peer4 must be carrying advertised prefixes for this test to mean "
         "anything";
  ASSERT_GT(preOutBefore, 0u);
  const uint32_t totalSentBefore = totalSentPrefixCount;
  ASSERT_GE(totalSentBefore, postOutBefore);

  bringDownPeer(kPeerAddr4);

  /*
   * cleanUpPeerRibOut settles both kinds of entry: the per-peer paths it
   * erases and the shared group paths it leaves in place. Missing either
   * leaves a residue here.
   */
  const auto [postOutAfter, preOutAfter] = getEgressPrefixCounts(kPeerAddr4);
  EXPECT_EQ(0u, postOutAfter)
      << "detached peer kept " << postOutAfter
      << " postOut prefixes after going down: some advertised path was never "
         "decremented";
  EXPECT_EQ(0u, preOutAfter)
      << "detached peer kept " << preOutAfter << " preOut prefixes";
  EXPECT_EQ(totalSentBefore - postOutBefore, totalSentPrefixCount)
      << "the global total must drop by exactly the departing peer's share";

  /* peer5 is untouched by peer4's departure and keeps serving the group. */
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
}

/*
 * The leak is only observable in production on reconnect, via the "Non-zero
 * egress prefix counts on session establishment" ERR. Assert the peer really
 * does come back with zeroed counts.
 */
TEST_P(
    UpdateGroupAddPathDetachedPeerDownTest,
    DetachedAddPathPeerReconnectsWithZeroedEgressCounts) {
  addPeer(kDefaultPeerSpec3_AddPath);
  addPeer(kDefaultPeerSpec4_AddPath);
  addPeer(kDefaultPeerSpec5_AddPath);

  setDefaultQueueSizes(/*capacity=*/100, /*highWm=*/80, /*lowWm=*/2);
  setQueueSizeForPeer(kPeerAddr4, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  setupComponents();

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
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop1,
      "65001" /* asPath */,
      "3010:1" /* community */,
      kRecvPathId1,
      kEqualLocalPref);
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop2,
      "65001" /* asPath */,
      "3010:1" /* community */,
      kRecvPathId2,
      kEqualLocalPref);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork(kMultiPathPrefixStr)));
  ASSERT_TRUE(waitForMultipathNexthopCount(kMultiPathPrefixStr, 2));

  for (int i = 1; i <= kNumSharedRoutes; ++i) {
    injectLocalRoutesAtRuntime(
        {sharedPrefix(i)}, {fmt::format("311{}:1", i)}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(
        folly::IPAddress::createNetwork(sharedPrefix(i))));
  }
  drainPeerQueueCompletely(peerId4, 5, 200);
  drainPeerQueueCompletely(peerId5, 5, 200);

  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      /*countThreshold=*/1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr4);
  for (int i = 1; i <= kNumPlugRoutes; ++i) {
    injectLocalRoutesAtRuntime(
        {plugPrefix(i)}, {fmt::format("320{}:1", i)}, 150);
    ASSERT_TRUE(waitForRouteInShadowRib(
        folly::IPAddress::createNetwork(plugPrefix(i))));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DETACHED_BLOCKED));
  drainPeerQueueCompletely(peerId5, 5, 200);

  /* Diverge peer4 on the multipath prefix, leaving the rest shared. */
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop1,
      "65001" /* asPath */,
      "3010:2" /* community */,
      kRecvPathId1,
      kEqualLocalPref);
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop2,
      "65001" /* asPath */,
      "3010:2" /* community */,
      kRecvPathId2,
      kEqualLocalPref);
  deleteRoute(
      "v4", kMultiPathPrefix, kMultiPathPrefixLen, kPeerAddr3, kRecvPathId1);
  addRoute(
      "v4",
      kMultiPathPrefix,
      kMultiPathPrefixLen,
      kPeerAddr3,
      kPathNexthop1,
      "65001" /* asPath */,
      "3010:3" /* community */,
      kRecvPathId1,
      kEqualLocalPref);

  PathOwnership ownership;
  WITH_RETRIES_N(kRetries, {
    drainPeerQueueCompletely(peerId5, 1, 200);
    ownership = getPathOwnership(kPeerAddr4);
    EXPECT_EVENTUALLY_GT(ownership.peerOwned, 0u);
  });

  /* Same scenario guards as the peer-down test: without them a churn change
   * that stopped producing the mixed prefix would leave this passing while
   * testing nothing. */
  ASSERT_GT(ownership.peerOwned, 0u)
      << "peer4 never diverged, so the per-peer cleanup branch is untested";
  ASSERT_GT(ownership.shared, 0u)
      << "peer4 shares nothing with the group, so the shared cleanup branch "
         "is untested";
  ASSERT_GT(ownership.mixedNodes, 0u)
      << "no prefix holds both a peer-owned and a shared path, so cleanup is "
         "never forced to walk past the peer's own paths to the shared ones";

  const auto [postOutBefore, preOutBefore] = getEgressPrefixCounts(kPeerAddr4);
  ASSERT_GT(postOutBefore, 0u);
  ASSERT_GT(preOutBefore, 0u);
  const uint32_t totalSentBefore = totalSentPrefixCount;
  ASSERT_GE(totalSentBefore, postOutBefore);

  bringDownPeer(kPeerAddr4);

  EXPECT_EQ(totalSentBefore - postOutBefore, totalSentPrefixCount)
      << "the global total must drop by exactly the departing peer's share";

  unblockPeer(kPeerAddr4);

  /*
   * sessionEstablished logs the ERR and then clears the counts, so the leak is
   * invisible afterwards -- assert on the state the reconnect starts from,
   * which is what the ERR reports.
   */
  const auto [postOut, preOut] = getEgressPrefixCounts(kPeerAddr4);
  EXPECT_EQ(0u, postOut);
  EXPECT_EQ(0u, preOut);

  bringUpPeer(kPeerAddr4, /*versionNumber=*/2);
  ASSERT_TRUE(waitForPeerStateAny(
      kPeerAddr4,
      {PeerUpdateState::JOINED_RUNNING,
       PeerUpdateState::INIT,
       PeerUpdateState::DETACHED_INIT_DUMP}));
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupAddPathDetachedPeerDownTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
