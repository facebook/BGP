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
 * E2E tests: Update Group MULTI-GROUP egress policy re-evaluation.
 *
 * Peers are spread across kNumGroups (4) peer groups, kPeersPerGroup (10) each.
 * Because peerGroupName is part of the update-group key, each peer group forms
 * its own update group. A single co_setPeerGroupsPolicy call then operates on 2
 * of the 4 groups; each test asserts those 2 update groups are re-evaluated to
 * the new policy while the other 2 stay on their original permit-all policy.
 *
 * Mirrors SimpleGroupPolicyEval's cases across multiple groups:
 *   MultiGroupPolicyChangeAllJoinedRunning: all peers JOINED_RUNNING.
 *   MultiGroupPolicyChangeJoinedBlocked: one JOINED_BLOCKED peer in an operated
 *     group; that group's re-eval queues behind it and completes on drain.
 *   MultiGroupPolicyChangeDetachedSlowPeer: one DETACHED_BLOCKED peer in an
 *     operated group; its in-sync group-mates re-eval via the group walk.
 *
 * Plus changelist-withdrawal variants (5 routes withdrawn over the changelist
 * on top of the announcements before the re-eval; a changelist withdrawal is
 * RIB-level, so those prefixes are gone from all groups and must not be
 * resurrected by the operated groups' re-eval):
 *   MultiGroupPolicyChangeJoinedBlockedAfterChangelistWithdrawal
 *   MultiGroupPolicyChangeDetachedSlowPeerAfterChangelistWithdrawal
 *
 * Starting policy is propagate-everything; the applied policy is MODIFY/APPEND.
 * All cases run in both serialization modes.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupPolicyReEvalE2ECommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

namespace {
/* Operate on the first 2 of the 4 groups; leave groups 2 and 3 untouched. */
constexpr int kNumOperatedGroups = 2;
} // namespace

/*
 * All peers JOINED_RUNNING. Apply APPEND to 2 of the 4 update groups in one
 * RPC; those 2 groups carry the appended community on the i % 3 == 0 routes
 * while the other 2 groups stay permit-all. Group identity and membership are
 * unchanged.
 */
TEST_P(MultiGroupPolicyReEval, MultiGroupPolicyChangeAllJoinedRunning) {
  XLOGF(INFO, "=== TEST: MultiGroupPolicyChangeAllJoinedRunning ===");

  setupPolicies();
  auto groups = setupPeersInGroups(kNumGroups, kPeersPerGroup);
  ASSERT_EQ(groups.size(), static_cast<size_t>(kNumGroups));

  /* The 4 peer groups form 4 distinct update groups. */
  using GroupPtr = decltype(getUpdateGroupForPeer(groups[0][0].peerAddr));
  std::vector<GroupPtr> groupObjs;
  for (int g = 0; g < kNumGroups; ++g) {
    auto obj = getUpdateGroupForPeer(groups[g][0].peerAddr);
    ASSERT_NE(obj, nullptr);
    for (const auto& pid : groups[g]) {
      EXPECT_EQ(getUpdateGroupForPeer(pid.peerAddr), obj);
    }
    for (const auto& prev : groupObjs) {
      EXPECT_NE(obj, prev) << "peer groups must form distinct update groups";
    }
    groupObjs.push_back(obj);
    EXPECT_EQ(getGroupMemberCount(groups[g][0].peerAddr), kPeersPerGroup);
  }

  auto drainAll = [&]() {
    for (const auto& group : groups) {
      for (const auto& pid : group) {
        recordDrainedRoutes(pid);
      }
    }
  };

  /* Inject under the default propagate-everything policy; all groups advertise
   * every route. */
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainAll();
  for (const auto& group : groups) {
    for (const auto& pid : group) {
      waitForRibOutAdvertisedCount(pid.peerAddr, kNumRoutes);
      expectReceivedRoutesForPolicy(
          pid, kPermitAllPolicyName, injectedPrefixes);
    }
  }

  /* Apply APPEND to groups 0 and 1 in a single multi-group RPC. */
  const std::vector<std::string> operatedGroups = {
      reEvalPeerGroupName(0), reEvalPeerGroupName(1)};
  auto result =
      setPeerGroupsPolicy(operatedGroups, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  drainAll();

  for (int g = 0; g < kNumGroups; ++g) {
    const bool operated = g < kNumOperatedGroups;
    const std::string policy =
        operated ? kMatchModifyAppendPolicyName : kPermitAllPolicyName;
    for (const auto& pid : groups[g]) {
      waitForRibOutAdvertisedCount(pid.peerAddr, kNumRoutes);
      expectRibOutForPolicy(pid.peerAddr, policy, kNumRoutes);
      expectReceivedRoutesForPolicy(pid, policy, injectedPrefixes);
    }
  }

  /* Every update group unchanged in identity and membership. */
  for (int g = 0; g < kNumGroups; ++g) {
    for (const auto& pid : groups[g]) {
      EXPECT_EQ(getUpdateGroupForPeer(pid.peerAddr), groupObjs[g]);
    }
    EXPECT_EQ(getGroupMemberCount(groups[g][0].peerAddr), kPeersPerGroup);
  }

  XLOGF(INFO, "=== TEST PASSED: MultiGroupPolicyChangeAllJoinedRunning ===");
}

/*
 * One peer (peer3, in operated group 0) is JOINED_BLOCKED via the initial dump.
 * Applying APPEND to groups 0 and 1 queues group 0's re-eval behind peer3 while
 * group 1 re-evals immediately; after unblock+drain both operated groups carry
 * the appended community and the untouched groups stay permit-all.
 */
TEST_P(MultiGroupPolicyReEval, MultiGroupPolicyChangeJoinedBlocked) {
  XLOGF(INFO, "=== TEST: MultiGroupPolicyChangeJoinedBlocked ===");

  setupPolicies();
  setQueueSizeForPeer(kPeerAddr3, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  auto groups = setupPeersInGroups(kNumGroups, kPeersPerGroup);
  ASSERT_EQ(groups.size(), static_cast<size_t>(kNumGroups));
  /* peer3 (the blocked peer) is the first peer of operated group 0. */
  ASSERT_EQ(groups[0][0].peerAddr, kPeerAddr3);

  using GroupPtr = decltype(getUpdateGroupForPeer(groups[0][0].peerAddr));
  std::vector<GroupPtr> groupObjs;
  for (int g = 0; g < kNumGroups; ++g) {
    groupObjs.push_back(getUpdateGroupForPeer(groups[g][0].peerAddr));
  }

  /* High slow-peer thresholds so peer3 stays JOINED_BLOCKED, never detached. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1000000,
      std::chrono::milliseconds(600000));

  auto drainExceptBlocked = [&]() {
    for (const auto& group : groups) {
      for (const auto& pid : group) {
        if (pid.peerAddr != kPeerAddr3) {
          recordDrainedRoutes(pid);
        }
      }
    }
  };

  /* Block peer3, inject -> peer3's tiny queue fills and it becomes
   * JOINED_BLOCKED (still an in-sync member of group 0). */
  blockPeer(kPeerAddr3);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainExceptBlocked();
  EXPECT_TRUE(waitForPeerQueueBlocked(groups[0][0]));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));

  /* Apply APPEND to groups 0 and 1 in one RPC. */
  const std::vector<std::string> operatedGroups = {
      reEvalPeerGroupName(0), reEvalPeerGroupName(1)};
  auto result =
      setPeerGroupsPolicy(operatedGroups, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  /* Unblock peer3 and let group 0's queued re-eval drain through it. */
  unblockAndDrainToJoined(kPeerAddr3, groups[0][0]);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  drainExceptBlocked();
  recordDrainedRoutes(groups[0][0]);

  for (int g = 0; g < kNumGroups; ++g) {
    const bool operated = g < kNumOperatedGroups;
    const std::string policy =
        operated ? kMatchModifyAppendPolicyName : kPermitAllPolicyName;
    for (const auto& pid : groups[g]) {
      waitForRibOutAdvertisedCount(pid.peerAddr, kNumRoutes);
      expectRibOutForPolicy(pid.peerAddr, policy, kNumRoutes);
      expectReceivedRoutesForPolicy(pid, policy, injectedPrefixes);
    }
  }

  for (int g = 0; g < kNumGroups; ++g) {
    for (const auto& pid : groups[g]) {
      EXPECT_EQ(getUpdateGroupForPeer(pid.peerAddr), groupObjs[g]);
    }
    EXPECT_EQ(getGroupMemberCount(groups[g][0].peerAddr), kPeersPerGroup);
  }
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: MultiGroupPolicyChangeJoinedBlocked ===");
}

/*
 * One peer (peer3, in operated group 0) is DETACHED_BLOCKED via the initial
 * dump (slow-peer block-count threshold 1). Applying APPEND to groups 0 and 1
 * re-evals the in-sync group-mates via the group walk while the detached peer
 * is re-evaluated individually; the untouched groups stay permit-all. The
 * detached peer is then unblocked and drained so it rejoins JOINED_RUNNING, and
 * its received routes are verified to carry the APPEND result once caught up.
 */
TEST_P(MultiGroupPolicyReEval, MultiGroupPolicyChangeDetachedSlowPeer) {
  XLOGF(INFO, "=== TEST: MultiGroupPolicyChangeDetachedSlowPeer ===");

  setupPolicies();
  setQueueSizeForPeer(kPeerAddr3, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  auto groups = setupPeersInGroups(kNumGroups, kPeersPerGroup);
  ASSERT_EQ(groups.size(), static_cast<size_t>(kNumGroups));
  ASSERT_EQ(groups[0][0].peerAddr, kPeerAddr3);

  /* Block-count threshold of 1 so the first block detaches peer3. */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  auto drainExceptBlocked = [&]() {
    for (const auto& group : groups) {
      for (const auto& pid : group) {
        if (pid.peerAddr != kPeerAddr3) {
          recordDrainedRoutes(pid);
        }
      }
    }
  };

  /* Block peer3, inject -> peer3 detaches (DETACHED_BLOCKED). */
  blockPeer(kPeerAddr3);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainExceptBlocked();
  EXPECT_TRUE(waitForPeerQueueBlocked(groups[0][0]));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(isPeerDetached(kPeerAddr3));

  /* Apply APPEND to groups 0 and 1 in one RPC. */
  const std::vector<std::string> operatedGroups = {
      reEvalPeerGroupName(0), reEvalPeerGroupName(1)};
  auto result =
      setPeerGroupsPolicy(operatedGroups, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  drainExceptBlocked();

  /* In-sync peers: operated groups carry APPEND, untouched groups stay
   * permit-all. The detached peer3 is skipped (it is not drained). */
  for (int g = 0; g < kNumGroups; ++g) {
    const bool operated = g < kNumOperatedGroups;
    const std::string policy =
        operated ? kMatchModifyAppendPolicyName : kPermitAllPolicyName;
    for (const auto& pid : groups[g]) {
      if (pid.peerAddr == kPeerAddr3) {
        continue;
      }
      waitForRibOutAdvertisedCount(pid.peerAddr, kNumRoutes);
      expectRibOutForPolicy(pid.peerAddr, policy, kNumRoutes);
      expectReceivedRoutesForPolicy(pid, policy, injectedPrefixes);
    }
  }

  /* Before rejoin the peer is still detached and healthy (not DOWN). */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  verifySlowPeerInvariants(kPeerAddr3);

  /*
   * Rejoin the detached peer (unblock + drain) so its received routes can be
   * checked. peer3 is in operated group 0, so after catching up it must carry
   * the APPEND result like its group-mates -- all routes advertised with the
   * appended community on the i % 3 == 0 routes.
   */
  unblockAndDrainToJoined(kPeerAddr3, groups[0][0]);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  waitForRibOutAdvertisedCount(kPeerAddr3, kNumRoutes);
  expectRibOutForPolicy(kPeerAddr3, kMatchModifyAppendPolicyName, kNumRoutes);
  expectReceivedRoutesForPolicy(
      groups[0][0], kMatchModifyAppendPolicyName, injectedPrefixes);

  XLOGF(INFO, "=== TEST PASSED: MultiGroupPolicyChangeDetachedSlowPeer ===");
}

/*
 * Like MultiGroupPolicyChangeJoinedBlocked, but with changelist withdrawals on
 * top: after peer3 (operated group 0) is JOINED_BLOCKED, 5 routes are withdrawn
 * over the changelist. A changelist withdrawal is RIB-level, so those prefixes
 * disappear from ALL groups; the subsequent APPEND on groups 0 and 1 still only
 * re-evals those two, and must not resurrect the changelist-withdrawn routes.
 */
TEST_P(
    MultiGroupPolicyReEval,
    MultiGroupPolicyChangeJoinedBlockedAfterChangelistWithdrawal) {
  XLOGF(
      INFO,
      "=== TEST: MultiGroupPolicyChangeJoinedBlockedAfterChangelistWithdrawal ===");

  setupPolicies();
  setQueueSizeForPeer(kPeerAddr3, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  auto groups = setupPeersInGroups(kNumGroups, kPeersPerGroup);
  ASSERT_EQ(groups.size(), static_cast<size_t>(kNumGroups));
  ASSERT_EQ(groups[0][0].peerAddr, kPeerAddr3);

  using GroupPtr = decltype(getUpdateGroupForPeer(groups[0][0].peerAddr));
  std::vector<GroupPtr> groupObjs;
  for (int g = 0; g < kNumGroups; ++g) {
    groupObjs.push_back(getUpdateGroupForPeer(groups[g][0].peerAddr));
  }

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1000000,
      std::chrono::milliseconds(600000));

  auto drainExceptBlocked = [&]() {
    for (const auto& group : groups) {
      for (const auto& pid : group) {
        if (pid.peerAddr != kPeerAddr3) {
          recordDrainedRoutes(pid);
        }
      }
    }
  };

  /* Block peer3, inject -> peer3 becomes JOINED_BLOCKED. */
  blockPeer(kPeerAddr3);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainExceptBlocked();
  EXPECT_TRUE(waitForPeerQueueBlocked(groups[0][0]));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_BLOCKED));
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));

  /*
   * Withdraw 5 routes over the changelist while peer3 is blocked. No
   * intermediate drain: the withdrawal stays pending so it is sent out together
   * with the announcements when peer3 is unblocked and catches up.
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

  /* Apply APPEND to groups 0 and 1. */
  const std::vector<std::string> operatedGroups = {
      reEvalPeerGroupName(0), reEvalPeerGroupName(1)};
  auto result =
      setPeerGroupsPolicy(operatedGroups, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  /* Unblock peer3 and let the queued withdrawals + re-eval drain through it. */
  unblockAndDrainToJoined(kPeerAddr3, groups[0][0]);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  drainExceptBlocked();
  recordDrainedRoutes(groups[0][0]);

  /*
   * Every peer: the remaining routes follow the group's policy (operated groups
   * APPEND, untouched groups permit-all) and the 5 changelist-withdrawn routes
   * stay gone everywhere.
   */
  const size_t expectedAdvertised = kNumRoutes - withdrawn.size();
  for (int g = 0; g < kNumGroups; ++g) {
    const bool operated = g < kNumOperatedGroups;
    const std::string policy =
        operated ? kMatchModifyAppendPolicyName : kPermitAllPolicyName;
    for (const auto& pid : groups[g]) {
      waitForRibOutAdvertisedCount(pid.peerAddr, expectedAdvertised);
      expectReceivedRoutesForPolicy(pid, policy, remaining);
      const auto& received = receivedRoutes(pid);
      for (const auto& prefix : withdrawn) {
        EXPECT_TRUE(received.find(prefix) == received.end())
            << "peer " << pid.peerAddr.str()
            << " still has changelist-withdrawn "
            << folly::IPAddress::networkToString(prefix);
      }
    }
  }

  for (int g = 0; g < kNumGroups; ++g) {
    for (const auto& pid : groups[g]) {
      EXPECT_EQ(getUpdateGroupForPeer(pid.peerAddr), groupObjs[g]);
    }
    EXPECT_EQ(getGroupMemberCount(groups[g][0].peerAddr), kPeersPerGroup);
  }
  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(
      INFO,
      "=== TEST PASSED: MultiGroupPolicyChangeJoinedBlockedAfterChangelistWithdrawal ===");
}

/*
 * Like MultiGroupPolicyChangeDetachedSlowPeer, but with changelist withdrawals
 * on top: after peer3 (operated group 0) is DETACHED_BLOCKED, 5 routes are
 * withdrawn over the changelist (RIB-level, so gone from all groups). APPEND on
 * groups 0 and 1 re-evals the in-sync group-mates without resurrecting the
 * withdrawn routes; the detached peer stays detached and healthy.
 */
TEST_P(
    MultiGroupPolicyReEval,
    MultiGroupPolicyChangeDetachedSlowPeerAfterChangelistWithdrawal) {
  XLOGF(
      INFO,
      "=== TEST: MultiGroupPolicyChangeDetachedSlowPeerAfterChangelistWithdrawal ===");

  setupPolicies();
  setQueueSizeForPeer(kPeerAddr3, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  auto groups = setupPeersInGroups(kNumGroups, kPeersPerGroup);
  ASSERT_EQ(groups.size(), static_cast<size_t>(kNumGroups));
  ASSERT_EQ(groups[0][0].peerAddr, kPeerAddr3);

  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  auto drainExceptBlocked = [&]() {
    for (const auto& group : groups) {
      for (const auto& pid : group) {
        if (pid.peerAddr != kPeerAddr3) {
          recordDrainedRoutes(pid);
        }
      }
    }
  };

  /* Block peer3, inject -> peer3 detaches (DETACHED_BLOCKED). */
  blockPeer(kPeerAddr3);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  drainExceptBlocked();
  EXPECT_TRUE(waitForPeerQueueBlocked(groups[0][0]));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));
  ASSERT_TRUE(isPeerDetached(kPeerAddr3));

  /*
   * Withdraw 5 routes over the changelist while peer3 is detached. No
   * intermediate drain: the withdrawal stays pending so it is sent out together
   * with the announcements when peer3 catches up.
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

  /* Apply APPEND to groups 0 and 1. */
  const std::vector<std::string> operatedGroups = {
      reEvalPeerGroupName(0), reEvalPeerGroupName(1)};
  auto result =
      setPeerGroupsPolicy(operatedGroups, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  drainExceptBlocked();

  /*
   * In-sync peers: the remaining routes follow the group's policy (operated
   * groups APPEND, untouched groups permit-all) and the 5 changelist-withdrawn
   * routes stay gone. The detached peer3 is skipped.
   */
  const size_t expectedAdvertised = kNumRoutes - withdrawn.size();
  for (int g = 0; g < kNumGroups; ++g) {
    const bool operated = g < kNumOperatedGroups;
    const std::string policy =
        operated ? kMatchModifyAppendPolicyName : kPermitAllPolicyName;
    for (const auto& pid : groups[g]) {
      if (pid.peerAddr == kPeerAddr3) {
        continue;
      }
      waitForRibOutAdvertisedCount(pid.peerAddr, expectedAdvertised);
      expectReceivedRoutesForPolicy(pid, policy, remaining);
      const auto& received = receivedRoutes(pid);
      for (const auto& prefix : withdrawn) {
        EXPECT_TRUE(received.find(prefix) == received.end())
            << "peer " << pid.peerAddr.str()
            << " still has changelist-withdrawn "
            << folly::IPAddress::networkToString(prefix);
      }
    }
  }

  /* Before rejoin the peer is still detached and healthy (not DOWN). */
  EXPECT_TRUE(isPeerDetached(kPeerAddr3));
  EXPECT_NE(getPeerState(kPeerAddr3), PeerUpdateState::DOWN);
  verifySlowPeerInvariants(kPeerAddr3);

  /*
   * Rejoin the detached peer: it catches up and receives the pending
   * announcements together with the changelist withdrawals. peer3 is in
   * operated group 0, so its remaining routes follow APPEND and the 5
   * changelist-withdrawn routes stay gone.
   */
  unblockAndDrainToJoined(kPeerAddr3, groups[0][0]);
  EXPECT_TRUE(isPeerInSync(kPeerAddr3));
  waitForRibOutAdvertisedCount(kPeerAddr3, expectedAdvertised);
  expectReceivedRoutesForPolicy(
      groups[0][0], kMatchModifyAppendPolicyName, remaining);
  {
    const auto& received = receivedRoutes(groups[0][0]);
    for (const auto& prefix : withdrawn) {
      EXPECT_TRUE(received.find(prefix) == received.end())
          << "peer " << kPeerAddr3.str() << " still has changelist-withdrawn "
          << folly::IPAddress::networkToString(prefix);
    }
  }

  XLOGF(
      INFO,
      "=== TEST PASSED: MultiGroupPolicyChangeDetachedSlowPeerAfterChangelistWithdrawal ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    MultiGroupPolicyReEval,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
