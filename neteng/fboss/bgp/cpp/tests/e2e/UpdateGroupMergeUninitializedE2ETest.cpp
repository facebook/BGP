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
 * E2E: a detached peer merged into an update group that has never dumped.
 *
 * Peers moved into an ALREADY EXISTING group take case (1) of the egress
 * policy reconciliation, which re-evaluates each moved peer but never the
 * target group -- processGroupEgressPolicyReEvaluation runs only for case (2).
 * An UNINITIALIZED target is therefore never walked, and never registers the
 * change list consumer that the dump would have created.
 *
 * The moved peer matters most when it arrives DETACHED_BLOCKED: movePeers'
 * onPeerMoved hook deliberately skips activateDetachedModeProcessing for it,
 * so it is revived later by markPeerUnblocked, which sets DETACHED_RUNNING and
 * activates it. That is the state that consumes the change list bounded by the
 * GROUP's marker:
 *
 *     auto groupConsumer = adjRibOutGroup_->getChangeListConsumer();
 *     auto* groupMarker = groupConsumer->getMarker();
 *
 * which has no null check, unlike the peer's own consumer two lines above.
 *
 * The held group is kept at UNINITIALIZED by bringing its peer up before the
 * initial RIB announcement completes -- sessionEstablished then defers the
 * dump rather than scheduling one -- and by holding the dump that the
 * announcement's completion would otherwise start.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupPolicyReEvalE2ECommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

namespace {
constexpr int kHeldPeerIdx = 0;
constexpr int kMovingPeerIdx = 1;
constexpr int kCompanionPeerIdx = 2;
} // namespace

class UpdateGroupMergeUninitializedE2ETest
    : public UpdateGroupPolicyReEvalE2EBase {
 protected:
  /* Group state read on the PeerManagerBase event base. */
  UpdateGroupState groupState(const folly::IPAddress& peerAddr) {
    auto state = UpdateGroupState::UNINITIALIZED;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      if (auto group = getUpdateGroupForPeer(peerAddr)) {
        state = group->getState();
      }
    });
    return state;
  }

  bool groupHasConsumer(const folly::IPAddress& peerAddr) {
    bool hasConsumer = false;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      if (auto group = getUpdateGroupForPeer(peerAddr)) {
        hasConsumer = group->getChangeListConsumer() != nullptr;
      }
    });
    return hasConsumer;
  }

  void holdGroupDump(const folly::IPAddress& peerAddr) {
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto group = getUpdateGroupForPeer(peerAddr);
      ASSERT_NE(group, nullptr);
      group->testOnlyDeferInitialDump = true;
    });
  }

  void releaseGroupDump(const folly::IPAddress& peerAddr) {
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      if (auto group = getUpdateGroupForPeer(peerAddr)) {
        group->testOnlyDeferInitialDump = false;
      }
    });
  }
};

TEST_P(
    UpdateGroupMergeUninitializedE2ETest,
    UnblockedDetachedPeerMergedIntoUninitializedGroup) {
  XLOGF(
      INFO, "=== TEST: UnblockedDetachedPeerMergedIntoUninitializedGroup ===");

  setupPolicies();

  /*
   * One peer group -- peerGroupName is part of the update group key, so two
   * peer groups could never merge. The split comes from a per-peer egress
   * policy on the moving pair, which gives them their own update group.
   * That pair needs two members: detachSlowPeer refuses to detach the only
   * synced member.
   */
  auto heldSpec = makePeerSpec(kHeldPeerIdx);
  heldSpec.peerGroupName = reEvalPeerGroupName(0);
  auto movingSpec = makePeerSpec(kMovingPeerIdx);
  movingSpec.peerGroupName = reEvalPeerGroupName(0);
  movingSpec.egressPolicyName = kMatchNoAdvtDenyPolicyName;
  auto companionSpec = makePeerSpec(kCompanionPeerIdx);
  companionSpec.peerGroupName = reEvalPeerGroupName(0);
  companionSpec.egressPolicyName = kMatchNoAdvtDenyPolicyName;
  addPeer(heldSpec);
  addPeer(movingSpec);
  addPeer(companionSpec);

  setupComponentsWithBgpService();

  /* A tiny queue so one round of routes backs the moving peer up. */
  setQueueSizeForPeer(
      movingSpec.peerAddr, /*capacity=*/2, /*highWm=*/1, /*lowWm=*/0);

  const BgpPeerId heldPeer{
      heldSpec.peerAddr, heldSpec.peerAddr.asV4().toLongHBO()};
  const BgpPeerId movingPeer{
      movingSpec.peerAddr, movingSpec.peerAddr.asV4().toLongHBO()};
  const BgpPeerId companionPeer{
      companionSpec.peerAddr, companionSpec.peerAddr.asV4().toLongHBO()};

  /*
   * Bring every peer up before any EoR. The initial announcement is not
   * complete yet, so sessionEstablished defers each group's dump instead of
   * scheduling one -- the groups exist, UNINITIALIZED, with no dump in flight
   * and no consumer.
   */
  bringUpPeer(heldSpec.peerAddr);
  bringUpPeer(movingSpec.peerAddr);
  bringUpPeer(companionSpec.peerAddr);

  ASSERT_EQ(groupState(heldSpec.peerAddr), UpdateGroupState::UNINITIALIZED);
  ASSERT_FALSE(groupHasConsumer(heldSpec.peerAddr));

  /*
   * Hold the held group's dump before initialization completes, so the dump
   * that triggerInitialDumpsForUninitializedGroups starts re-queues itself
   * instead of walking. No event-base race: the group already exists.
   */
  ASSERT_NO_FATAL_FAILURE(holdGroupDump(heldSpec.peerAddr));

  /* Completing the EoRs marks the initial announcement done. */
  sendEoRToPeer(heldPeer);
  sendEoRToPeer(movingPeer);
  sendEoRToPeer(companionPeer);
  ASSERT_TRUE(waitForEoR(movingPeer));
  ASSERT_TRUE(waitForEoR(companionPeer));

  /* The moving group dumps; the held group stays put with its peer in INIT. */
  ASSERT_TRUE(
      waitForPeerState(movingSpec.peerAddr, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(
      companionSpec.peerAddr, PeerUpdateState::JOINED_RUNNING));
  EXPECT_EQ(groupState(heldSpec.peerAddr), UpdateGroupState::UNINITIALIZED);
  EXPECT_FALSE(groupHasConsumer(heldSpec.peerAddr));

  /*
   * Drive the moving peer to DETACHED_BLOCKED: one block inside the window
   * detaches it, and its queue is small enough that a single round fills it.
   */
  setSlowPeerThresholds(
      movingSpec.peerAddr,
      std::chrono::milliseconds(600000),
      /*countThreshold=*/1,
      std::chrono::milliseconds(60000));
  blockPeer(movingSpec.peerAddr);
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  ASSERT_TRUE(waitForPeerQueueBlocked(movingPeer));
  ASSERT_TRUE(
      waitForPeerState(movingSpec.peerAddr, PeerUpdateState::DETACHED_BLOCKED))
      << "moving peer never reached DETACHED_BLOCKED";

  /*
   * Record from here on rather than discarding: everything this peer was
   * given under its old group's policy has to count toward the final tally.
   */
  recordDrainedRoutes(movingPeer, /*idleRetries=*/1, /*maxMessages=*/500);

  /*
   * Advance the OLD group past the blocked peer. The companion keeps that
   * group's consumer moving, so the peer ends up BEHIND its group on the
   * change list -- the one condition under which sendBgpUpdates' tail takes
   * the final else and calls reschedulePackingTimers(), arming the peer's
   * changeListConsumeTimer_. Every other branch parks the peer and cancels it.
   */
  std::vector<folly::CIDRNetwork> moreRoutes;
  injectCommunityTaggedRoutes(kNumRoutes, &moreRoutes);
  drainAllOutboundMessagesToOrderedVec(companionPeer);

  /*
   * Unblocking arms that latent timer with one MRAI of delay. The timer lives
   * on the AdjRib, but its callback resolves adjRibOutGroup_ when it FIRES --
   * so if the peer changes groups first, the callback lands on the new group.
   */
  unblockPeer(movingSpec.peerAddr, /*maxRetries=*/0);
  recordDrainedRoutes(movingPeer, /*idleRetries=*/1, /*maxMessages=*/500);

  /*
   * Merge into the consumer-less group inside that window. movePeers does not
   * cancel packing timers, so the armed timer survives the move.
   */
  EXPECT_EQ(
      unsetPeerPolicy(movingSpec.peerAddr.str()),
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);

  bool merged = false;
  WITH_RETRIES_N(50, {
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto heldGroup = getUpdateGroupForPeer(heldSpec.peerAddr);
      auto movingGroup = getUpdateGroupForPeer(movingSpec.peerAddr);
      merged = heldGroup && movingGroup && heldGroup == movingGroup;
    });
    EXPECT_EVENTUALLY_TRUE(merged);
  });
  ASSERT_TRUE(merged) << "moving peer never merged into the held group";

  /*
   * The reconciliation walks an uninitialized group that peers merged into,
   * which is what registers its change list consumer. Without that walk the
   * group keeps an empty RIB-OUT and no consumer, leaving its own peers in
   * INIT and its detached members with no marker to bound themselves against.
   */
  EXPECT_TRUE(groupHasConsumer(heldSpec.peerAddr))
      << "merged-into group was never walked, so it has no change list "
         "consumer for its detached members to bound against";
  EXPECT_NE(groupState(heldSpec.peerAddr), UpdateGroupState::UNINITIALIZED)
      << "merged-into group was left uninitialized";

  ASSERT_TRUE(waitForPeerQueueBlocked(movingPeer))
      << "moving peer's detached consume timer never produced an update";
  ASSERT_TRUE(
      waitForPeerState(movingSpec.peerAddr, PeerUpdateState::DETACHED_BLOCKED));

  /* Release the hold; both peers must end up served. */
  releaseGroupDump(heldSpec.peerAddr);

  /*
   * Drain continuously from here. The merged peer's queue is two deep, so the
   * full table only reaches it a couple of messages at a time: it blocks,
   * drains, unblocks, and works through the table before rejoining. Unblock
   * without draining first, so every message is recorded rather than
   * discarded.
   */
  ASSERT_NO_FATAL_FAILURE(
      unblockAndDrainToJoined(movingSpec.peerAddr, movingPeer));
  EXPECT_TRUE(
      waitForPeerState(heldSpec.peerAddr, PeerUpdateState::JOINED_RUNNING))
      << "held peer never converged after the merge";
  WITH_RETRIES_N(40, {
    recordDrainedRoutes(heldPeer, /*idleRetries=*/1, /*maxMessages=*/500);
    EXPECT_EVENTUALLY_GE(
        receivedRoutes(heldPeer).size(), static_cast<size_t>(kNumRoutes));
  });

  /*
   * The walk has to produce the whole table, not merely a consumer: the
   * group's RIB-OUT carries every injected prefix for both its original peer
   * and the one that merged in, and both actually receive them on the wire.
   */
  waitForRibOutAdvertisedCount(heldSpec.peerAddr, kNumRoutes);
  waitForRibOutAdvertisedCount(movingSpec.peerAddr, kNumRoutes);

  recordDrainedRoutes(heldPeer);
  recordDrainedRoutes(movingPeer);
  EXPECT_EQ(receivedRoutes(heldPeer).size(), static_cast<size_t>(kNumRoutes))
      << "held peer did not receive every route from the RIB";
  for (const auto& prefix : injectedPrefixes) {
    EXPECT_TRUE(receivedRoutes(heldPeer).contains(prefix))
        << "held peer missing " << prefix.first.str();
  }

  EXPECT_EQ(receivedRoutes(movingPeer).size(), static_cast<size_t>(kNumRoutes))
      << "merged peer did not receive every route from the RIB";
  for (const auto& prefix : injectedPrefixes) {
    EXPECT_TRUE(receivedRoutes(movingPeer).contains(prefix))
        << "merged peer missing " << prefix.first.str();
  }

  XLOGF(INFO, "=== TEST PASSED ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMergeUninitializedE2ETest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
