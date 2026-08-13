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

#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook::bgp {
namespace {

constexpr int kRetries = 50;
constexpr int kHolderHighWatermark = 2;
constexpr auto kPeerGroupName = "collapse-retry-repro";

const std::vector<std::string> kPlugPrefixes{
    "65.0.1.0/24",
    "65.0.2.0/24",
    "65.0.3.0/24"};

const folly::CIDRNetwork kSeedPrefix =
    folly::IPAddress::createNetwork("66.10.0.0/16");
const folly::CIDRNetwork kMissingPrefix =
    folly::IPAddress::createNetwork("66.11.0.0/16");
const folly::CIDRNetwork kHighVersionPrefix =
    folly::IPAddress::createNetwork("66.12.0.0/16");
const folly::CIDRNetwork kBoundaryPrefix =
    folly::IPAddress::createNetwork("66.13.0.0/16");

struct OwnerPresence {
  bool peer{false};
  bool group{false};

  bool operator==(const OwnerPresence& other) const {
    return peer == other.peer && group == other.group;
  }
};

struct CollapseGateState {
  bool peerChangeListReady{false};
  bool groupChangeListReady{false};
  bool markersEqual{false};
  bool versionsEqual{false};
  bool groupPackingListEmpty{false};
};

} // namespace

class UpdateGroupCollapseRetryConvergenceE2ETest : public SlowPeerTestBase {
 protected:
  BgpPeerSpec makeGroupPeer(
      uint32_t asn,
      const folly::IPAddress& peerAddr,
      const folly::IPAddress& v4Nexthop) {
    return BgpPeerSpec{
        .asn = asn,
        .localAddr = kLocalAddr1,
        .peerAddr = peerAddr,
        .v4Nexthop = v4Nexthop,
        .v6Nexthop = kEmptyV6Nexthop,
        .disableIpv6Afi = true,
        .peerGroupName = kPeerGroupName,
    };
  }

  int64_t getTotalDiscrepancies(const folly::IPAddress& peerAddr) {
    int64_t discrepancies = 0;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto group = getUpdateGroupForPeer(peerAddr);
      if (group) {
        discrepancies = group->getTotalDiscrepancies();
      }
    });
    return discrepancies;
  }

  OwnerPresence getOwnerPresence(
      const folly::IPAddress& peerAddr,
      const folly::CIDRNetwork& prefix) {
    OwnerPresence presence;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto adjRib = getAdjRibByAddr(peerAddr);
      auto group = adjRib ? adjRib->getUpdateGroup() : nullptr;
      if (!adjRib || !group) {
        return;
      }
      presence.peer =
          group->getFromLiteTree(
              group->LiteTree_, prefix, adjRib->getPeerOwnerKey()) != nullptr;
      presence.group =
          group->getFromLiteTree(
              group->LiteTree_, prefix, group->getGroupOwnerKey()) != nullptr;
    });
    return presence;
  }

  bool waitForOwnerPresence(
      const folly::IPAddress& peerAddr,
      const folly::CIDRNetwork& prefix,
      const OwnerPresence& expected) {
    OwnerPresence actual;
    WITH_RETRIES_N(kRetries, {
      actual = getOwnerPresence(peerAddr, prefix);
      EXPECT_EVENTUALLY_EQ(actual, expected);
    });
    return actual == expected;
  }

  std::optional<folly::CIDRNetwork> getGroupMarker(
      const folly::IPAddress& peerAddr) {
    std::optional<folly::CIDRNetwork> prefix;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto group = getUpdateGroupForPeer(peerAddr);
      auto consumer = group ? group->getChangeListConsumer() : nullptr;
      auto* marker = consumer ? consumer->getMarker() : nullptr;
      if (marker) {
        prefix = marker->getTypedObject().prefix;
      }
    });
    return prefix;
  }

  bool isGroupPackingListEmpty(const folly::IPAddress& peerAddr) {
    bool empty = false;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto group = getUpdateGroupForPeer(peerAddr);
      empty = group && group->getAttrToPrefixMap().empty();
    });
    return empty;
  }

  CollapseGateState getCollapseGateState(const folly::IPAddress& peerAddr) {
    CollapseGateState state;
    peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
      auto adjRib = getAdjRibByAddr(peerAddr);
      auto group = adjRib ? adjRib->getUpdateGroup() : nullptr;
      auto peerConsumer = adjRib ? adjRib->getChangeListConsumer() : nullptr;
      auto groupConsumer = group ? group->getChangeListConsumer() : nullptr;
      if (!adjRib || !group || !peerConsumer || !groupConsumer) {
        return;
      }
      state.peerChangeListReady = peerConsumer->isReady();
      state.groupChangeListReady = groupConsumer->isReady();
      state.markersEqual =
          peerConsumer->getMarker() == groupConsumer->getMarker();
      state.versionsEqual =
          adjRib->getLastSeenRibVersion() == group->getLastSeenRibVersion();
      state.groupPackingListEmpty = group->getAttrToPrefixMap().empty();
    });
    return state;
  }
};

TEST_P(UpdateGroupCollapseRetryConvergenceE2ETest, RetryConverges) {
  addPeer(makeGroupPeer(kPeerAsn3, kPeerAddr3, kNextHopV4_3));
  addPeer(makeGroupPeer(kPeerAsn4, kPeerAddr4, kNextHopV4_4));
  addPeer(makeGroupPeer(kPeerAsn5, kPeerAddr5, kNextHopV4_5));
  setDefaultQueueSizes(/*capacity=*/100, /*highWm=*/80, /*lowWm=*/2);
  setQueueSizeForPeer(
      kPeerAddr4,
      /*capacity=*/3,
      /*highWm=*/kHolderHighWatermark,
      /*lowWm=*/0);
  setQueueSizeForPeer(kPeerAddr5, /*capacity=*/2, /*highWm=*/1, /*lowWm=*/0);
  thrift::UpdateGroupConfig updateGroupConfig;
  updateGroupConfig.allowSlowPeerDetach() = false;
  setUpdateGroupConfig(updateGroupConfig);
  setEorTimeSeconds(1);
  setupComponents();

  const BgpPeerId anchorPeer{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  const BgpPeerId holderPeer{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  const BgpPeerId detachedPeer{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(anchorPeer);
  sendEoRToPeer(holderPeer);
  ASSERT_TRUE(waitForEoR(anchorPeer));
  ASSERT_TRUE(waitForEoR(holderPeer));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_EQ(0, getPeerQueueSize(holderPeer));

  /*
   * Three distinct plug UPDATEs against a (3, 2, 0) queue leave two queued
   * and the third deferred. The real group send coroutine is now suspended,
   * and its change-list timer stays canceled until that send completes.
   */
  blockPeer(kPeerAddr4);
  injectDistinctRoutes(kPlugPrefixes, /*communityBase=*/65000);
  ASSERT_TRUE(waitForPeerQueueBlocked(holderPeer));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_BLOCKED));
  ASSERT_TRUE(isPeerInSync(kPeerAddr4));

  /*
   * Withdraw the plug routes while the group is frozen, then publish S.
   * The late peer's normal initial dump sees only S; the group has not yet
   * consumed either the withdrawals or S.
   */
  withdrawLocalRoutesAtRuntime(kPlugPrefixes);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib(kPlugPrefixes.back()));
  WITH_RETRIES_N(kRetries, {
    EXPECT_EVENTUALLY_TRUE(getGroupMarker(kPeerAddr3).has_value());
  });
  injectLocalRoutesAtRuntime({"66.10.0.0/16"}, {"6610:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(kSeedPrefix));

  /*
   * A one-message high watermark holds the late peer on its outbound EoR:
   * S lands in the queue, while the EoR push waits for queue space. This is a
   * normal DETACHED_INIT_DUMP using the production dump and send paths.
   */
  bringUpPeerBlocked(kPeerAddr5);
  sendEoRToPeer(detachedPeer);
  ASSERT_TRUE(waitForPeerQueueBlocked(detachedPeer));
  ASSERT_TRUE(waitForChangeListConsumerReady(kPeerAddr5));
  ASSERT_TRUE(waitForOwnerPresence(
      kPeerAddr5, kSeedPrefix, OwnerPresence{true, false}));
  ASSERT_TRUE(waitForOwnerPresence(
      kPeerAddr5,
      folly::IPAddress::createNetwork(kPlugPrefixes.front()),
      OwnerPresence{false, true}));

  /*
   * B and H arrive after the dump registered its detached consumer. The dump
   * remains blocked on EoR, so that consumer stays parked on B.
   */
  injectLocalRoutesAtRuntime({"66.11.0.0/16"}, {"6611:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(kMissingPrefix));
  injectLocalRoutesAtRuntime({"66.12.0.0/16"}, {"6612:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(kHighVersionPrefix));
  ASSERT_TRUE(
      waitForChangeListConsumerPended(kPeerAddr5, kMissingPrefix, kRetries));

  /*
   * Drain exactly the two queued plug messages. The deferred third plug lands,
   * the first group send completes, and the group consumes withdrawals/S/B/H.
   * Its next send naturally re-blocks on the holder queue.
   */
  ASSERT_TRUE(unblockPeer(kPeerAddr4, /*maxRetries=*/0));
  auto firstHolderDrain = drainAllOutboundMessagesToOrderedVec(
      holderPeer,
      /*idleRetries=*/1,
      /*maxMessages=*/kHolderHighWatermark,
      /*sleepMsBetweenRetries=*/0);
  ASSERT_EQ(kHolderHighWatermark, firstHolderDrain.size());
  blockPeer(kPeerAddr4);

  ASSERT_TRUE(
      waitForOwnerPresence(kPeerAddr5, kSeedPrefix, OwnerPresence{true, true}));
  ASSERT_TRUE(waitForOwnerPresence(
      kPeerAddr5, kMissingPrefix, OwnerPresence{false, true}));
  ASSERT_TRUE(waitForOwnerPresence(
      kPeerAddr5, kHighVersionPrefix, OwnerPresence{false, true}));
  ASSERT_TRUE(waitForOwnerPresence(
      kPeerAddr5,
      folly::IPAddress::createNetwork(kPlugPrefixes.front()),
      OwnerPresence{false, false}));
  ASSERT_TRUE(waitForPeerQueueBlocked(holderPeer));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_BLOCKED));

  /*
   * The holder currently contains two messages and the group is deferred with
   * two messages still unbuilt. Draining exactly two advances the coroutine to
   * its final PDU: that PDU is removed from the packing list, then blocks on
   * the holder. Thus the group PL is empty while its CL timer remains canceled.
   */
  auto secondHolderDrain = drainAllOutboundMessagesToOrderedVec(
      holderPeer,
      /*idleRetries=*/1,
      /*maxMessages=*/kHolderHighWatermark,
      /*sleepMsBetweenRetries=*/0);
  ASSERT_EQ(kHolderHighWatermark, secondHolderDrain.size());
  WITH_RETRIES_N(kRetries, {
    EXPECT_EVENTUALLY_TRUE(
        isGroupPackingListEmpty(kPeerAddr3) && isPeerQueueBlocked(holderPeer));
  });
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_BLOCKED));

  /*
   * Append boundary M, then republish B. B moves behind M in the change list:
   *
   *   P marker -> H -> M
   *   G marker ------> M
   *   remaining list: H, M, B
   *
   * G already materialized B; P did not. Consuming H makes their scalar RIB
   * versions and markers equal even though the two RIB-OUTs differ on B.
   */
  injectLocalRoutesAtRuntime({"66.13.0.0/16"}, {"6613:1"}, 150);
  ASSERT_TRUE(waitForRouteInShadowRib(kBoundaryPrefix));
  injectLocalRoutesAtRuntime({"66.11.0.0/16"}, {"6611:2"}, 200);
  ASSERT_TRUE(waitForChangeListConsumerPended(
      kPeerAddr5, kHighVersionPrefix, kRetries));
  WITH_RETRIES_N(kRetries, {
    EXPECT_EVENTUALLY_EQ(getGroupMarker(kPeerAddr3), kBoundaryPrefix);
  });

  /*
   * Pop only S. The waiting EoR lands and completes the dump, but remains in
   * the one-message queue. P's detached timer can now consume H; its H send
   * blocks behind that EoR while G remains frozen on the independent holder.
   */
  auto seedMessage = drainAllOutboundMessagesToOrderedVec(
      detachedPeer,
      /*idleRetries=*/1,
      /*maxMessages=*/1,
      /*sleepMsBetweenRetries=*/0);
  ASSERT_EQ(1, seedMessage.size());
  ASSERT_NE(nullptr, seedMessage.front().update);
  EXPECT_TRUE(findPrefixInAnnouncements(
      *seedMessage.front().update,
      /*isV4=*/true,
      kSeedPrefix,
      /*addPathId=*/0));
  EXPECT_TRUE(verifyRouteAttributes(
      *seedMessage.front().update,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6610:1"));

  ASSERT_TRUE(waitForPeerQueueBlocked(detachedPeer));
  ASSERT_TRUE(
      waitForChangeListConsumerPended(kPeerAddr5, kBoundaryPrefix, kRetries));
  ASSERT_TRUE(waitForOwnerPresence(
      kPeerAddr5, kHighVersionPrefix, OwnerPresence{true, true}));
  EXPECT_EQ(
      (OwnerPresence{false, true}),
      getOwnerPresence(kPeerAddr5, kMissingPrefix));

  CollapseGateState gateState;
  WITH_RETRIES_N(kRetries, {
    gateState = getCollapseGateState(kPeerAddr5);
    EXPECT_EVENTUALLY_TRUE(
        !gateState.peerChangeListReady && !gateState.groupChangeListReady &&
        gateState.markersEqual && gateState.versionsEqual &&
        gateState.groupPackingListEmpty);
  });

  /*
   * Pop the EoR. H lands and P's send completes at the old premature-
   * acceptance point. The event-base snapshot below is a barrier after that
   * completion; both consumers must still be parked together at M.
   */
  auto eorMessage = drainAllOutboundMessagesToOrderedVec(
      detachedPeer,
      /*idleRetries=*/1,
      /*maxMessages=*/1,
      /*sleepMsBetweenRetries=*/0);
  ASSERT_EQ(1, eorMessage.size());
  EXPECT_TRUE(eorMessage.front().isEoR);
  ASSERT_TRUE(waitForPeerQueueBlocked(detachedPeer));
  const auto afterRejectedGate = getCollapseGateState(kPeerAddr5);
  EXPECT_FALSE(afterRejectedGate.peerChangeListReady);
  EXPECT_FALSE(afterRejectedGate.groupChangeListReady);
  EXPECT_TRUE(afterRejectedGate.markersEqual);
  EXPECT_TRUE(afterRejectedGate.versionsEqual);
  EXPECT_TRUE(afterRejectedGate.groupPackingListEmpty);

  EXPECT_EQ(0, getTotalDiscrepancies(kPeerAddr3));
  EXPECT_EQ(
      (OwnerPresence{true, true}), getOwnerPresence(kPeerAddr5, kSeedPrefix));
  EXPECT_EQ(
      (OwnerPresence{true, true}),
      getOwnerPresence(kPeerAddr5, kHighVersionPrefix));

  /*
   * P cannot move past G's marker. Release G first: the old deferred PDU
   * lands, G consumes M/B to the end, and its next send blocks with an empty
   * group PL. P can now legally consume M/B to the end behind it.
   */
  ASSERT_TRUE(unblockPeer(kPeerAddr4, /*maxRetries=*/0));
  auto firstGroupCatchupDrain = drainAllOutboundMessagesToOrderedVec(
      holderPeer,
      /*idleRetries=*/1,
      /*maxMessages=*/kHolderHighWatermark,
      /*sleepMsBetweenRetries=*/0);
  ASSERT_EQ(kHolderHighWatermark, firstGroupCatchupDrain.size());
  WITH_RETRIES_N(kRetries, {
    const auto state = getCollapseGateState(kPeerAddr5);
    EXPECT_EVENTUALLY_TRUE(
        state.groupChangeListReady && state.groupPackingListEmpty &&
        isPeerQueueBlocked(holderPeer));
  });
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_BLOCKED));
  ASSERT_TRUE(waitForChangeListConsumerReady(kPeerAddr5));

  /*
   * Drain H, then one M/B PDU. The final PDU lands and P's PL becomes empty.
   * Both consumers are now at the end, so the production acceptance path may
   * collapse P into G without a discrepancy.
   */
  ASSERT_TRUE(unblockPeer(kPeerAddr5, /*maxRetries=*/0));
  auto highVersionMessage = drainAllOutboundMessagesToOrderedVec(
      detachedPeer,
      /*idleRetries=*/1,
      /*maxMessages=*/1,
      /*sleepMsBetweenRetries=*/0);
  ASSERT_EQ(1, highVersionMessage.size());
  ASSERT_NE(nullptr, highVersionMessage.front().update);
  EXPECT_TRUE(findPrefixInAnnouncements(
      *highVersionMessage.front().update,
      /*isV4=*/true,
      kHighVersionPrefix,
      /*addPathId=*/0));
  EXPECT_TRUE(verifyRouteAttributes(
      *highVersionMessage.front().update,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "6612:1"));
  ASSERT_TRUE(waitForPeerQueueBlocked(detachedPeer));

  auto peerCatchupMessage = drainAllOutboundMessagesToOrderedVec(
      detachedPeer,
      /*idleRetries=*/1,
      /*maxMessages=*/1,
      /*sleepMsBetweenRetries=*/0);
  ASSERT_EQ(1, peerCatchupMessage.size());

  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr5));
  EXPECT_EQ(0, getTotalDiscrepancies(kPeerAddr3));

  EXPECT_EQ(
      (OwnerPresence{false, true}), getOwnerPresence(kPeerAddr5, kSeedPrefix));
  EXPECT_EQ(
      (OwnerPresence{false, true}),
      getOwnerPresence(kPeerAddr5, kHighVersionPrefix));
  EXPECT_EQ(
      (OwnerPresence{false, true}),
      getOwnerPresence(kPeerAddr5, kMissingPrefix));
  EXPECT_EQ(
      (OwnerPresence{false, true}),
      getOwnerPresence(kPeerAddr5, kBoundaryPrefix));

  auto secondGroupCatchupDrain = drainAllOutboundMessagesToOrderedVec(
      holderPeer,
      /*idleRetries=*/1,
      /*maxMessages=*/kHolderHighWatermark,
      /*sleepMsBetweenRetries=*/0);
  EXPECT_EQ(kHolderHighWatermark, secondGroupCatchupDrain.size());
}

INSTANTIATE_TEST_SUITE_P(
    SerializationEnabled,
    UpdateGroupCollapseRetryConvergenceE2ETest,
    ::testing::Values(kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace facebook::bgp
