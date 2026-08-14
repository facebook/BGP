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
 * E2E: group egress policy re-evaluation landing while the group's only
 * builder is parked in the EoR wait.
 *
 * A single-member update group re-dumps the whole table after its peer
 * reconnects post-initialization. The peer's queue is sized off the measured
 * dump size so the dump's UPDATEs all push inline and the EoR that follows is
 * the first push to be deferred: the builder reaches distributePendingEoRs
 * with an empty packing list and parks in waitForAllPendingPushes, yielding
 * the event base with the group in WAITING.
 *
 * The peer group's egress policy is then changed to append. Every member moves
 * to the same new key, so the manager rekeys the group in place rather than
 * splitting it, and reEvaluateSyncPeersEgressPolicy refills the packing list
 * behind the parked builder without touching the group's state.
 *
 * Releasing the queue lets the EoR land and the builder return. The consume
 * timer is what carries the appended work out; it starts a build whenever
 * messages are pending and no packing is already in progress. The test pins
 * that the re-evaluated routes reach the wire at all -- the append community,
 * and a route injected afterwards -- rather than the session staying
 * established with the re-evaluation never advertised.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupPolicyReEvalE2ECommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

namespace {
/* A prefix outside the 36.0.x.0/24 community-tagged set used for the dump. */
constexpr auto kLaterPrefix = "37.0.1.0/24";
} // namespace

class UpdateGroupPolicyDuringEoRWaitE2ETest
    : public UpdateGroupPolicyReEvalE2EBase {
 protected:
  /*
   * Packing list size, read on the PeerManagerBase event base: the builder and
   * the re-evaluation both mutate attrToPrefixMap_ there, so an off-evb read
   * races them.
   */
  size_t getGroupPackingListSize(const folly::IPAddress& peerAddr) {
    size_t size = 0;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto group = getUpdateGroupForPeer(peerAddr);
      size = group ? group->getAttrToPrefixMap().size() : 0;
    });
    return size;
  }

  /*
   * Whether a build currently owns the group, read on the PeerManagerBase
   * event base for the same reason as the packing list.
   */
  bool isPackingInProgress(const folly::IPAddress& peerAddr) {
    bool packing = false;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto group = getUpdateGroupForPeer(peerAddr);
      packing = group && group->isPackingInProgress();
    });
    return packing;
  }

  /*
   * Number of UPDATEs (EoRs excluded) the group packs the current table into.
   * The peer's queue watermark is derived from this rather than hardcoded, so
   * a packing change makes the checkpoints below fail loudly instead of
   * quietly not exercising the EoR wait.
   */
  size_t drainAndCountUpdates(const BgpPeerId& peerId) {
    size_t updates = 0;
    for (const auto& msg : drainAllOutboundMessagesToOrderedVec(peerId)) {
      if (!msg.isEoR) {
        ++updates;
      }
    }
    return updates;
  }
};

TEST_P(
    UpdateGroupPolicyDuringEoRWaitE2ETest,
    PolicyReEvalDuringGroupEoRWaitStillSends) {
  XLOGF(INFO, "=== TEST: PolicyReEvalDuringGroupEoRWaitStillSends ===");

  setupPolicies();
  auto peerIds = setupNPeersInGroupJoined(/*numPeers=*/1);
  const auto subjectId = peerIds.front();

  /* Propagate-everything baseline: the peer holds the full tagged route set. */
  std::vector<folly::CIDRNetwork> injectedPrefixes;
  injectCommunityTaggedRoutes(kNumRoutes, &injectedPrefixes);
  const auto dumpUpdates = drainAndCountUpdates(subjectId);
  ASSERT_GT(dumpUpdates, 0u);

  /*
   * The queue this peer comes back with must not detach it: the wedge needs a
   * SYNC member, since a detached peer is served by the per-peer walk instead
   * of the group walk.
   */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1000000,
      std::chrono::milliseconds(600000));

  /*
   * Reconnect post-initialization so the peer's group runs a fresh initial
   * dump, which is what leaves egressEoRPending set and sends the builder into
   * distributePendingEoRs. highWm == dumpUpdates blocks the queue on exactly
   * the push that follows the last UPDATE, i.e. the EoR.
   */
  bringDownPeer(kPeerAddr3);
  setQueueSizeForPeer(
      kPeerAddr3,
      /*capacity=*/static_cast<int>(dumpUpdates) + 4,
      /*highWm=*/static_cast<int>(dumpUpdates),
      /*lowWm=*/0);
  bringUpPeerBlocked(kPeerAddr3);
  sendEoRToPeer(subjectId);

  /*
   * Checkpoint A -- builder parked in the EoR wait: group WAITING, packing
   * list drained by the message loop, peer queue blocked on the EoR push, peer
   * still a SYNC member.
   */
  ASSERT_TRUE(waitForPeerQueueBlocked(subjectId));
  WITH_RETRIES_N(30, {
    EXPECT_EVENTUALLY_TRUE(isPackingInProgress(kPeerAddr3));
    EXPECT_EVENTUALLY_EQ(getGroupState(kPeerAddr3), UpdateGroupState::WAITING);
    EXPECT_EVENTUALLY_EQ(getGroupPackingListSize(kPeerAddr3), 0u);
  });
  EXPECT_FALSE(isPeerDetached(kPeerAddr3));
  auto group = getUpdateGroupForPeer(kPeerAddr3);
  ASSERT_NE(group, nullptr);

  /* Re-evaluate the group's egress policy while that builder is parked. */
  auto result =
      setPeerGroupPolicy(kReEvalPeerGroupName, kMatchModifyAppendPolicyName);
  EXPECT_EQ(
      result,
      neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  ASSERT_TRUE(waitForEgressReEvalComplete());

  /*
   * Checkpoint B -- the group was rekeyed in place (same object, no split) and
   * the re-evaluation refilled its packing list behind the build that is still
   * parked. packingInProgress_ is what says so, and it is what makes this the
   * interesting case: the work landed on a group already past the point where
   * that build would pack it.
   */
  EXPECT_EQ(getUpdateGroupForPeer(kPeerAddr3), group);
  EXPECT_TRUE(isPackingInProgress(kPeerAddr3));
  EXPECT_GT(getGroupPackingListSize(kPeerAddr3), 0u);
  EXPECT_EQ(getGroupState(kPeerAddr3), UpdateGroupState::WAITING);

  /*
   * Release the queue. Reading is what makes room for the deferred EoR, so
   * drain while polling: that is what lets the parked build resolve its push
   * and return. packingInProgress_ clearing is the observable proof that it
   * did; the group state cannot serve here, since it stays WAITING on the work
   * the re-evaluation appended.
   */
  unblockPeer(kPeerAddr3, /*maxRetries=*/0);
  WITH_RETRIES_N_TIMED(600, std::chrono::milliseconds(1), {
    recordDrainedRoutes(subjectId, /*idleRetries=*/1, /*maxMessages=*/50);
    EXPECT_EVENTUALLY_FALSE(isPackingInProgress(kPeerAddr3));
  });

  /*
   * The re-evaluated routes reach RIB-OUT and the wire, and the group settles
   * with a drained packing list. Assert this before injecting anything else: a
   * later route would carry a change item that drives a build on its own and
   * would mask a re-evaluation that never got sent.
   */
  WITH_RETRIES_N(30, {
    recordDrainedRoutes(subjectId, /*idleRetries=*/1, /*maxMessages=*/500);
    EXPECT_EVENTUALLY_EQ(getGroupPackingListSize(kPeerAddr3), 0u);
    EXPECT_EVENTUALLY_EQ(getGroupState(kPeerAddr3), UpdateGroupState::IDLE);
  });
  waitForRibOutAdvertisedCount(kPeerAddr3, kNumRoutes);
  expectRibOutForPolicy(kPeerAddr3, kMatchModifyAppendPolicyName, kNumRoutes);
  recordDrainedRoutes(subjectId);
  expectReceivedRoutesForPolicy(
      subjectId, kMatchModifyAppendPolicyName, injectedPrefixes);

  /* A route injected after the release is advertised too. */
  const auto laterPrefix = folly::IPAddress::createNetwork(kLaterPrefix);
  injectLocalRoutesAtRuntime({kLaterPrefix});
  WITH_RETRIES_N(30, {
    recordDrainedRoutes(subjectId, /*idleRetries=*/1, /*maxMessages=*/500);
    EXPECT_EVENTUALLY_TRUE(receivedRoutes(subjectId).count(laterPrefix) > 0);
  });

  verifySlowPeerInvariants(kPeerAddr3);

  XLOGF(INFO, "=== TEST PASSED: PolicyReEvalDuringGroupEoRWaitStillSends ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupPolicyDuringEoRWaitE2ETest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
