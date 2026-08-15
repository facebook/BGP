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
 * E2E tests: Update Group GROUP-LEVEL egress policy re-evaluation.
 *
 * Peers share one update group via a common peer group and NO per-peer egress
 * policy override. A peer-group-level policy change (co_setPeerGroupsPolicy)
 * with no override takes the in-place group rekey + single group RIB re-walk
 * path — the whole group is re-evaluated once, no peer is moved.
 *
 * Base state (before any re-eval): peers join one update group with no egress
 * policy applied (permit-all), so every route is advertised to every member.
 * Each test then applies a peer-group-level policy and checks the result.
 *
 * Tests:
 *   GroupPolicyChangeAllJoinedRunningGroupOnlyReEval: all peers JOINED_RUNNING;
 *     scale re-eval over kNumPeers peers / kNumRoutes community-tagged routes
 *     through permit-all -> match-no-advt-deny -> match-modify-append,
 * asserting RIB-OUT state per route.
 *   GroupPolicyChangeJoinedBlockedPeerQueuedReEval: one JOINED_BLOCKED peer;
 * the synchronous group walk queues behind it and completes on drain.
 *   GroupPolicyChangeDetachedSlowPeerIndividualReEval: one detached slow peer
 *     re-eval'd individually while in-sync peers re-eval via the group walk.
 *   GroupPolicyChangeUpdateGroupDisabledPerPeerFallback: enable_update_group=
 *     false -> legacy per-peer fallback path.
 *
 * All cases run in both serialization modes. Every policy change here is
 * peer-group-level (co_setPeerGroupsPolicy); per-peer override split/move is
 * covered by sibling diffs.
 *
 * Prefix ranges: 36.0.x.0/24 (scale test), 36.x.0.0/16 (flag-off and
 * blocked-peer tests), 110-112.0.0.0/8 (detached-slow-peer test).
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupPolicyReEvalE2ECommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

namespace {
/* AS path stamped on locally-originated routes by the RIB. */
constexpr auto kLocalOriginAsPath = "4200000001";
} // namespace

/*
 * Policy change with all peers JOINED_RUNNING.
 *
 * A DENY policy applied at the peer-group level re-evaluates the whole group in
 * a single RIB walk: every member withdraws the route, the group is rekeyed in
 * place (same group object, same membership), and all peers stay JOINED_RUNNING
 * and in sync.
 */
TEST_P(
    ConsumerOnChangelistWithOnlyAnnouncements,
    GroupPolicyChangeAllJoinedRunningGroupOnlyReEval) {
  XLOGF(INFO, "=== TEST: GroupPolicyChangeAllJoinedRunningGroupOnlyReEval ===");

  /*
   * kNumRoutes route indices 0..kNumRoutes-1: half even, half odd
   * (kCommNoAdvt), one third i%3==0 (kCommModify).
   */
  constexpr size_t kEvenRoutes = kNumRoutes / 2;

  setupPolicies();
  /* 10 peers in one group, production-sized outbound queues. */
  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers, /*queueCapacity=*/10, /*queueHighWm=*/8, /*queueLowWm=*/2);

  std::vector<folly::IPAddress> peers;
  for (const auto& peerId : peerIds) {
    peers.push_back(peerId.peerAddr);
  }
  /*
   * Drain each peer's outbound queue after every re-eval phase, recording the
   * prefixes/attributes each peer received into the fixture's received-routes
   * map. Draining also keeps the production-sized queue from filling and
   * backpressuring the synchronous group walk.
   */
  auto drainAll = [&]() {
    for (const auto& peerId : peerIds) {
      recordDrainedRoutes(peerId);
    }
  };

  /* All peers share one update group. */
  auto group = getUpdateGroupForPeer(peers[0]);
  ASSERT_NE(group, nullptr);
  for (const auto& peerAddr : peers) {
    EXPECT_EQ(getUpdateGroupForPeer(peerAddr), group);
  }
  EXPECT_EQ(getGroupMemberCount(peers[0]), kNumPeers);

  /*
   * Inject kNumRoutes routes tagged with ingress communities (odd carry
   * kCommNoAdvt, i % 3 == 0 carry kCommModify). With no egress policy they are
   * all advertised to every member.
   */
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainAll();
  for (const auto& peerAddr : peers) {
    waitForRibOutAdvertisedCount(peerAddr, kNumRoutes);
    expectRibOutForPolicy(peerAddr, kPermitAllPolicyName, kNumRoutes);
  }
  /* Each peer received an announcement for exactly the injected prefixes. */
  for (const auto& peerId : peerIds) {
    expectReceivedRoutesForPolicy(
        peerId, kPermitAllPolicyName, injectedPrefixes);
  }

  /*
   * Group-level policy matching kCommNoAdvt -> DENY: a single group re-eval
   * withdraws the 50 odd routes; the 50 even routes stay advertised.
   */
  auto denyResult =
      setPeerGroupPolicy(kReEvalPeerGroupName, kMatchNoAdvtDenyPolicyName);
  EXPECT_EQ(
      denyResult,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  drainAll();
  for (const auto& peerAddr : peers) {
    waitForRibOutAdvertisedCount(peerAddr, kEvenRoutes);
    expectRibOutForPolicy(peerAddr, kMatchNoAdvtDenyPolicyName, kNumRoutes);
  }
  /* Each peer withdrew the odd prefixes and kept the even ones. */
  for (const auto& peerId : peerIds) {
    expectReceivedRoutesForPolicy(
        peerId, kMatchNoAdvtDenyPolicyName, injectedPrefixes);
  }

  /*
   * Group-level policy matching kCommModify -> APPEND kCommAppend: a single
   * group re-eval re-advertises all routes, and the 34 i % 3 == 0 routes now
   * carry the appended community.
   */
  auto appendResult =
      setPeerGroupPolicy(kReEvalPeerGroupName, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      appendResult,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  drainAll();
  for (const auto& peerAddr : peers) {
    waitForRibOutAdvertisedCount(peerAddr, kNumRoutes);
    expectRibOutForPolicy(peerAddr, kMatchModifyAppendPolicyName, kNumRoutes);
  }
  /* Each peer re-received all prefixes; i % 3 == 0 carry the appended comm. */
  for (const auto& peerId : peerIds) {
    expectReceivedRoutesForPolicy(
        peerId, kMatchModifyAppendPolicyName, injectedPrefixes);
  }

  /*
   * In-place rekey throughout: the group object and membership are unchanged
   * (no peer was moved/split), and every member remains JOINED_RUNNING and in
   * sync.
   */
  for (const auto& peerAddr : peers) {
    EXPECT_EQ(getUpdateGroupForPeer(peerAddr), group);
  }
  EXPECT_EQ(getGroupMemberCount(peers[0]), kNumPeers);

  for (const auto& peerAddr : peers) {
    EXPECT_TRUE(waitForPeerState(peerAddr, PeerUpdateState::JOINED_RUNNING));
    EXPECT_TRUE(isPeerInSync(peerAddr));
  }
  verifySlowPeerInvariants(peers[0]);

  XLOGF(
      INFO,
      "=== TEST PASSED: GroupPolicyChangeAllJoinedRunningGroupOnlyReEval ===");
}

/*
 * Flag-off guard: with enable_update_group=false, a peer-group policy change
 * falls back to the legacy per-peer re-evaluation path
 * (schedulePolicyReEvalForAdjRibs). Routes must still drain from every peer.
 *
 * The update-group peer-state machine does not apply here, so this test does
 * not assert group state / JOINED_RUNNING — only route propagation via the
 * fallback path.
 */
TEST_P(
    ConsumerOnChangelistWithOnlyAnnouncements,
    GroupPolicyChangeUpdateGroupDisabledPerPeerFallback) {
  XLOGF(
      INFO,
      "=== TEST: GroupPolicyChangeUpdateGroupDisabledPerPeerFallback ===");

  setupPolicies();
  /*
   * Update group disabled: peers stay in the default DOWN state (no
   * JOINED_RUNNING state machine), routes flow via the legacy per-peer path. We
   * verify via outbound messages rather than RIB-OUT state (there is no group
   * LiteTree to walk).
   */
  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers,
      /*queueCapacity=*/10,
      /*queueHighWm=*/8,
      /*queueLowWm=*/2,
      /*enableUpdateGroup=*/false,
      /*waitForJoinedRunning=*/false);
  std::vector<folly::IPAddress> peers;
  for (const auto& peerId : peerIds) {
    peers.push_back(peerId.peerAddr);
  }

  /* Inject kNumRoutes routes; the legacy path advertises them to every peer. */
  std::vector<std::string> prefixes;
  prefixes.reserve(kNumRoutes);
  for (int i = 0; i < kNumRoutes; ++i) {
    prefixes.push_back(fmt::format("36.0.{}.0/24", i));
  }
  injectLocalRoutesAtRuntime(prefixes, {}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork(
          fmt::format("36.0.{}.0/24", kNumRoutes - 1))));

  for (const auto& peerAddr : peers) {
    std::vector<VerifySpec> adds;
    adds.reserve(kNumRoutes);
    for (int i = 0; i < kNumRoutes; ++i) {
      adds.push_back(
          {fmt::format("36.0.{}.0", i),
           24,
           getExpectedNexthop(peerAddr),
           kLocalOriginAsPath,
           ""});
    }
    EXPECT_TRUE(verifyRoutes("v4", peerAddr, adds));
  }

  auto result = setPeerGroupPolicy(kReEvalPeerGroupName, kDenyPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  for (const auto& peerAddr : peers) {
    std::vector<WithdrawSpec> withdraws;
    withdraws.reserve(kNumRoutes);
    for (int i = 0; i < kNumRoutes; ++i) {
      withdraws.push_back({fmt::format("36.0.{}.0", i), 24});
    }
    EXPECT_TRUE(verifyRouteWithdraws("v4", peerAddr, withdraws));
  }

  XLOGF(
      INFO,
      "=== TEST PASSED: GroupPolicyChangeUpdateGroupDisabledPerPeerFallback ===");
}

/*
 * Policy change with one peer JOINED_BLOCKED.
 *
 * peer3's egress queue is test-blocked and filled to the high watermark so it
 * is JOINED_BLOCKED but still an in-sync group member (not detached). A
 * group-level DENY re-evaluation is a synchronous walk over the in-sync
 * members, so it is queued behind peer3's blocked queue and cannot complete
 * for the whole group until peer3 drains. Unblocking peer3 lets the queued
 * re-eval finish: all members withdraw the routes and peer3 converges back to
 * JOINED_RUNNING and in sync, with the group unchanged.
 */
TEST_P(
    ConsumerOnChangelistWithOnlyAnnouncements,
    GroupPolicyChangeJoinedBlockedPeerQueuedReEval) {
  XLOGF(INFO, "=== TEST: GroupPolicyChangeJoinedBlockedPeerQueuedReEval ===");

  constexpr size_t kEvenRoutes = kNumRoutes / 2;

  setupPolicies();
  /*
   * peer3 gets a tiny outbound queue so it blocks under load; peers 4-12 keep
   * production-sized queues and are drained each phase so they stay in-sync
   * while the group walk queues behind the blocked peer.
   */
  setQueueSizeForPeer(kPeerAddr3, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers, /*queueCapacity=*/10, /*queueHighWm=*/8, /*queueLowWm=*/2);
  std::vector<folly::IPAddress> peers;
  for (const auto& peerId : peerIds) {
    peers.push_back(peerId.peerAddr);
  }
  /* Drain the in-sync peers (all but the blocked peer3 = peers[0]). */
  auto drainInSync = [&]() {
    for (size_t i = 1; i < peerIds.size(); ++i) {
      recordDrainedRoutes(peerIds[i]);
    }
  };

  auto group = getUpdateGroupForPeer(peers[0]);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(getGroupMemberCount(peers[0]), kNumPeers);

  /* High slow-peer thresholds so peer3 stays JOINED_BLOCKED, never detached. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1000000,
      std::chrono::milliseconds(600000));

  /*
   * Block peer3, then inject kNumRoutes routes. The in-sync peers receive them
   * (drained); peer3's tiny queue fills past the high watermark so it becomes
   * JOINED_BLOCKED, still an in-sync member (not detached).
   */
  blockPeer(kPeerAddr3);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainInSync();
  EXPECT_TRUE(waitForPeerQueueBlocked(peerIds[0]));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /*
   * Group-level DENY (match kCommNoAdvt). The re-eval walk is queued behind
   * peer3's blocked queue and cannot complete for the group until it drains.
   */
  auto result =
      setPeerGroupPolicy(kReEvalPeerGroupName, kMatchNoAdvtDenyPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  /*
   * Unblock peer3 so the queued group re-eval can drain through it and complete
   * for the whole group. peer3 converges back to JOINED_RUNNING.
   */
  unblockAndDrainToJoined(kPeerAddr3, peerIds[0]);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  drainInSync();
  /*
   * unblockAndDrainToJoined stops draining once peer3 reaches JOINED_RUNNING,
   * so fully drain it here to record its complete delivery before verifying.
   */
  recordDrainedRoutes(peerIds[0]);

  /* Every member's RIB-OUT reflects the policy: even advertised, odd denied. */
  for (const auto& peerAddr : peers) {
    waitForRibOutAdvertisedCount(peerAddr, kEvenRoutes);
    expectRibOutForPolicy(peerAddr, kMatchNoAdvtDenyPolicyName, kNumRoutes);
  }
  /*
   * Every member (including the once-blocked peer3) received the even prefixes
   * and withdrew the odd ones on the wire.
   */
  for (const auto& peerId : peerIds) {
    expectReceivedRoutesForPolicy(
        peerId, kMatchNoAdvtDenyPolicyName, injectedPrefixes);
  }

  /* Group unchanged: same object, all members. */
  for (const auto& peerAddr : peers) {
    EXPECT_EQ(getUpdateGroupForPeer(peerAddr), group);
  }
  EXPECT_EQ(getGroupMemberCount(peers[0]), kNumPeers);
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(
      INFO,
      "=== TEST PASSED: GroupPolicyChangeJoinedBlockedPeerQueuedReEval ===");
}

/*
 * Policy change while one peer is a detached slow peer (DSP).
 *
 * peer3 is detached via frequency-based slow-peer detection (DETACHED_BLOCKED)
 * with change-list entries pending — a DSP. A group-level DENY re-evaluates the
 * in-sync peers (4/5) via the group walk, so they withdraw the routes, while
 * the detached peer is individually re-evaluated in place (not via the group
 * walk). The detached peer stays detached and healthy (not DOWN) and the group
 * stays consistent.
 *
 * Note: this test verifies correct handling of a detached peer during a group
 * policy change; full DSP rejoin to JOINED_RUNNING is a separate concern
 * exercised by the dedicated recovery tests.
 */
TEST_P(
    ConsumerOnChangelistWithOnlyAnnouncements,
    GroupPolicyChangeDetachedSlowPeerIndividualReEval) {
  XLOGF(
      INFO, "=== TEST: GroupPolicyChangeDetachedSlowPeerIndividualReEval ===");

  constexpr size_t kEvenRoutes = kNumRoutes / 2;

  setupPolicies();
  /* peer3 tiny queue so it detaches under load; peers 4-12 production-sized. */
  setQueueSizeForPeer(kPeerAddr3, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers, /*queueCapacity=*/10, /*queueHighWm=*/8, /*queueLowWm=*/2);
  std::vector<folly::IPAddress> peers;
  for (const auto& peerId : peerIds) {
    peers.push_back(peerId.peerAddr);
  }
  /* Drain the in-sync peers (all but the detached peer3 = peers[0]). */
  auto drainInSync = [&]() {
    for (size_t i = 1; i < peerIds.size(); ++i) {
      recordDrainedRoutes(peerIds[i]);
    }
  };

  auto group = getUpdateGroupForPeer(peers[0]);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(getGroupMemberCount(peers[0]), kNumPeers);

  /*
   * Detach peer3 via frequency-based slow-peer detection (threshold=1): block
   * it, inject kNumRoutes, and its tiny queue fills so it detaches
   * (DETACHED_BLOCKED). The in-sync peers receive the routes (drained).
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainInSync();
  EXPECT_TRUE(waitForPeerQueueBlocked(peerIds[0]));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(isPeerDetached(kPeerAddr3));

  /*
   * Group-level DENY (match kCommNoAdvt): the in-sync peers re-eval via the
   * group walk (skips the detached peer), while the detached peer is
   * re-evaluated individually in place.
   */
  auto result =
      setPeerGroupPolicy(kReEvalPeerGroupName, kMatchNoAdvtDenyPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  drainInSync();

  /* Every in-sync peer's RIB-OUT reflects the policy: even advertised, odd
   * denied. */
  for (size_t i = 1; i < peers.size(); ++i) {
    waitForRibOutAdvertisedCount(peers[i], kEvenRoutes);
    expectRibOutForPolicy(peers[i], kMatchNoAdvtDenyPolicyName, kNumRoutes);
  }
  /* Each in-sync peer received the even prefixes and withdrew the odd ones on
   * the wire (the detached peer3 is not drained here). */
  for (size_t i = 1; i < peerIds.size(); ++i) {
    expectReceivedRoutesForPolicy(
        peerIds[i], kMatchNoAdvtDenyPolicyName, injectedPrefixes);
  }

  /*
   * The detached peer stays detached and healthy (not DOWN); the in-sync peers
   * remain in sync.
   */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  for (size_t i = 1; i < peers.size(); ++i) {
    EXPECT_TRUE(isPeerInSync(peers[i]));
  }
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(
      INFO,
      "=== TEST PASSED: GroupPolicyChangeDetachedSlowPeerIndividualReEval ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    ConsumerOnChangelistWithOnlyAnnouncements,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

/*
 * Changelist carrying announcements AND withdrawals, with a JOINED_BLOCKED
 * peer.
 *
 * Everything starts propagated (default permit-all). peer3 is first driven to
 * JOINED_BLOCKED by the initial 100-route injection (its tiny queue fills).
 * Only then are 5 routes withdrawn over the changelist, so the group's
 * changelist carries withdrawals as well as announcements while a member is
 * blocked. A group-level MODIFY/APPEND re-eval is then queued behind peer3;
 * once peer3 is unblocked and drained, every member converges to the policy
 * result -- all routes re-advertised (with the appended community on the i % 3
 * == 0 routes), and the 5 changelist-withdrawn routes still gone (the re-eval
 * did not resurrect them).
 */
TEST_P(
    ConsumerOnChangelistWithAnnouncementsWithdrawals,
    GroupPolicyChangeJoinedBlockedPeerAfterChangelistWithdrawal) {
  XLOGF(
      INFO,
      "=== TEST: GroupPolicyChangeJoinedBlockedPeerAfterChangelistWithdrawal ===");

  setupPolicies();
  setQueueSizeForPeer(kPeerAddr3, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers, /*queueCapacity=*/10, /*queueHighWm=*/8, /*queueLowWm=*/2);
  std::vector<folly::IPAddress> peers;
  for (const auto& peerId : peerIds) {
    peers.push_back(peerId.peerAddr);
  }
  auto drainInSync = [&]() {
    for (size_t i = 1; i < peerIds.size(); ++i) {
      recordDrainedRoutes(peerIds[i]);
    }
  };

  auto group = getUpdateGroupForPeer(peers[0]);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(getGroupMemberCount(peers[0]), kNumPeers);

  /* High slow-peer thresholds so peer3 stays JOINED_BLOCKED, never detached. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1000000,
      std::chrono::milliseconds(600000));

  /*
   * 1. Block peer3, then inject the tagged routes under the default
   * propagate-everything policy. peer3's tiny queue fills so it becomes
   * JOINED_BLOCKED (still an in-sync member); the group advertises all routes.
   */
  blockPeer(kPeerAddr3);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainInSync();
  EXPECT_TRUE(waitForPeerQueueBlocked(peerIds[0]));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  for (const auto& peerAddr : peers) {
    waitForRibOutAdvertisedCount(peerAddr, kNumRoutes);
  }

  /*
   * 2. With peer3 blocked, withdraw 5 routes so the group's changelist now
   * carries withdrawals as well as announcements while a member is blocked.
   */
  std::vector<folly::CIDRNetwork> withdrawn;
  std::vector<folly::CIDRNetwork> remaining;
  for (int i = 0; i < kNumRoutes; ++i) {
    if (i < 5) {
      withdrawn.push_back(injectedPrefixes[i]);
    } else {
      remaining.push_back(injectedPrefixes[i]);
    }
  }
  withdrawRoutes(withdrawn);
  drainInSync();

  /*
   * 3. Apply the MODIFY/APPEND policy. It re-advertises every route (appending
   * a community to the i % 3 == 0 routes), but the re-eval walk skips the
   * changelist-pending withdrawals, so the 5 withdrawn routes must stay gone
   * rather than be resurrected. The whole group send is queued behind peer3.
   */
  auto result =
      setPeerGroupPolicy(kReEvalPeerGroupName, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  /* 4. Unblock peer3 and let the queued withdrawals + re-eval drain through it.
   */
  unblockAndDrainToJoined(kPeerAddr3, peerIds[0]);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  drainInSync();
  recordDrainedRoutes(peerIds[0]);

  /*
   * 5. Every member: the remaining routes follow APPEND (all advertised, i % 3
   * == 0 carry the appended community) and the changelist-withdrawn routes stay
   * gone.
   */
  const size_t expectedAdvertised = kNumRoutes - withdrawn.size();
  for (const auto& peerAddr : peers) {
    waitForRibOutAdvertisedCount(peerAddr, expectedAdvertised);
  }
  for (const auto& peerId : peerIds) {
    expectReceivedRoutesForPolicy(
        peerId, kMatchModifyAppendPolicyName, remaining);
    const auto& received = receivedRoutes(peerId);
    for (const auto& prefix : withdrawn) {
      EXPECT_TRUE(received.find(prefix) == received.end())
          << "peer " << peerId.peerAddr.str()
          << " still has changelist-withdrawn "
          << folly::IPAddress::networkToString(prefix);
    }
  }

  for (const auto& peerAddr : peers) {
    EXPECT_EQ(getUpdateGroupForPeer(peerAddr), group);
  }
  EXPECT_EQ(getGroupMemberCount(peers[0]), kNumPeers);
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(
      INFO,
      "=== TEST PASSED: GroupPolicyChangeJoinedBlockedPeerAfterChangelistWithdrawal ===");
}

/*
 * Changelist carrying announcements AND withdrawals, with a DETACHED_BLOCKED
 * peer.
 *
 * Everything starts propagated (default permit-all). peer3 uses a slow-peer
 * block-count threshold of 1, so the initial 100-route injection fills its tiny
 * queue and detaches it (DETACHED_BLOCKED). Only then are 5 routes withdrawn
 * over the changelist, so the group's changelist carries withdrawals as well as
 * announcements while a member is detached. A group-level MODIFY/APPEND re-eval
 * then serves the in-sync peers via the group walk; they converge to the policy
 * result -- all routes re-advertised (with the appended community on the i % 3
 * == 0 routes) and the 5 changelist-withdrawn routes gone -- and the detached
 * peer stays detached and healthy.
 */
TEST_P(
    ConsumerOnChangelistWithAnnouncementsWithdrawals,
    GroupPolicyChangeDetachedSlowPeerAfterChangelistWithdrawal) {
  XLOGF(
      INFO,
      "=== TEST: GroupPolicyChangeDetachedSlowPeerAfterChangelistWithdrawal ===");

  setupPolicies();
  setQueueSizeForPeer(kPeerAddr3, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers, /*queueCapacity=*/10, /*queueHighWm=*/8, /*queueLowWm=*/2);
  std::vector<folly::IPAddress> peers;
  for (const auto& peerId : peerIds) {
    peers.push_back(peerId.peerAddr);
  }
  auto drainInSync = [&]() {
    for (size_t i = 1; i < peerIds.size(); ++i) {
      recordDrainedRoutes(peerIds[i]);
    }
  };

  auto group = getUpdateGroupForPeer(peers[0]);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(getGroupMemberCount(peers[0]), kNumPeers);

  /* Block-count threshold of 1 so the first block detaches peer3. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /*
   * 1. Block peer3, then inject the tagged routes under the default
   * propagate-everything policy. With block-count threshold 1, peer3's tiny
   * queue fills and it detaches (DETACHED_BLOCKED); the group advertises all.
   */
  blockPeer(kPeerAddr3);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainInSync();
  EXPECT_TRUE(waitForPeerQueueBlocked(peerIds[0]));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(isPeerDetached(kPeerAddr3));
  for (size_t i = 1; i < peers.size(); ++i) {
    waitForRibOutAdvertisedCount(peers[i], kNumRoutes);
  }

  /*
   * 2. With peer3 detached, withdraw 5 routes so the group's changelist carries
   * withdrawals as well as announcements while a member is detached.
   */
  std::vector<folly::CIDRNetwork> withdrawn;
  std::vector<folly::CIDRNetwork> remaining;
  for (int i = 0; i < kNumRoutes; ++i) {
    if (i < 5) {
      withdrawn.push_back(injectedPrefixes[i]);
    } else {
      remaining.push_back(injectedPrefixes[i]);
    }
  }
  withdrawRoutes(withdrawn);
  drainInSync();

  /*
   * 3. Apply the MODIFY/APPEND policy. The in-sync peers re-eval via the group
   * walk (which skips the changelist-pending withdrawals, so the 5 withdrawn
   * routes stay gone); the detached peer is re-evaluated individually.
   */
  auto result =
      setPeerGroupPolicy(kReEvalPeerGroupName, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  drainInSync();

  /*
   * 4. In-sync peers: remaining routes follow APPEND (all advertised, i % 3 ==
   * 0 carry the appended community) and the changelist-withdrawn routes stay
   * gone.
   */
  const size_t expectedAdvertised = kNumRoutes - withdrawn.size();
  for (size_t i = 1; i < peers.size(); ++i) {
    waitForRibOutAdvertisedCount(peers[i], expectedAdvertised);
  }
  for (size_t i = 1; i < peerIds.size(); ++i) {
    expectReceivedRoutesForPolicy(
        peerIds[i], kMatchModifyAppendPolicyName, remaining);
    const auto& received = receivedRoutes(peerIds[i]);
    for (const auto& prefix : withdrawn) {
      EXPECT_TRUE(received.find(prefix) == received.end())
          << "peer " << peerIds[i].peerAddr.str()
          << " still has changelist-withdrawn "
          << folly::IPAddress::networkToString(prefix);
    }
  }

  /* Detached peer stays detached and healthy; in-sync peers remain in sync. */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  for (size_t i = 1; i < peers.size(); ++i) {
    EXPECT_TRUE(isPeerInSync(peers[i]));
  }
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(
      INFO,
      "=== TEST PASSED: GroupPolicyChangeDetachedSlowPeerAfterChangelistWithdrawal ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    ConsumerOnChangelistWithAnnouncementsWithdrawals,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
