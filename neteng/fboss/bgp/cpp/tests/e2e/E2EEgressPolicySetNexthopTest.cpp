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
 * E2E test: a community-gated egress SetNexthop policy combined with egress
 * queue backpressure.
 *
 * The egress policy EGRESS_POLICY_WITH_SET_NEXTHOP has two terms:
 *   term1: if the route carries kSetNexthopCommunity -> SetNexthop(10.0.0.1)
 *          and clear the community list, then accept.
 *   term2: catch-all -> clear the community list, then accept.
 *
 * Scenario: the receiving peer's egress queue is padded so it is blocked
 * (highWm=1) BEFORE any route is advertised. A prefix is then announced twice
 * in succession while the queue stays blocked, so both updates collapse in the
 * AdjRib packing list and only the final state is ever advertised:
 *   announce #1: 5.0.0.0/8, nexthop 10.0.0.1, no community.
 *   announce #2: 5.0.0.0/8, with kSetNexthopCommunity attached (the flap).
 *
 * The receiving peer is eBGP, so nexthop-self would normally rewrite the
 * nexthop to the local peering address. term1's SetNexthop overrides
 * nexthop-self, so the single collapsed advertisement is expected to be
 * 5.0.0.0/8 with nexthop 10.0.0.1 and no communities.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib, PolicyManager
 */

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

static const std::string kSetNexthopPolicyName =
    "EGRESS_POLICY_WITH_SET_NEXTHOP";
static const std::string kSetNexthopCommunity = "65500:100";
static const std::string kPrefix = "5.0.0.0";
static constexpr uint8_t kPrefixLen = 8;
static const std::string kPolicyNexthop = "10.0.0.1";
static const std::string kAsPath = "65001";

class E2EEgressPolicySetNexthopTest : public E2ETestFixture {
 protected:
  /*
   * Build EGRESS_POLICY_WITH_SET_NEXTHOP and register it.
   *
   * term1 matches the kSetNexthopCommunity community, sets the nexthop to
   * kPolicyNexthop and clears communities (SET with an empty list clears).
   * term2 is a catch-all that clears communities. A term that matches applies
   * its actions and accepts by default; term_miss_action=NEXT_TERM falls
   * through to the next term when the match fails.
   */
  void setupEgressPolicy() {
    auto communityMatch = createBgpPolicyAtomicMatch(
        bgp_policy::BgpPolicyAtomicMatchType::COMMUNITY_LIST,
        {kSetNexthopCommunity});
    auto setNexthop =
        createBgpPolicyNexthopAction(folly::IPAddress(kPolicyNexthop));
    auto clearCommunities = createBgpPolicyCommunityAction(
        bgp_policy::CommunityActionType::SET, /*communities=*/{});

    auto term1 = createBgpPolicyTerm(
        "match-community-set-nexthop-clear-community",
        "Match kSetNexthopCommunity: set nexthop and clear communities",
        {communityMatch},
        {setNexthop, clearCommunities},
        bgp_policy::FlowControlAction::NEXT_TERM);

    auto term2 = createBgpPolicyTerm(
        "catch-all-clear-community",
        "Catch-all: clear communities and accept",
        {} /* matches all */,
        {createBgpPolicyCommunityAction(
            bgp_policy::CommunityActionType::SET, /*communities=*/{})},
        bgp_policy::FlowControlAction::NEXT_TERM);

    setPolicyConfig(createBgpPolicies(kSetNexthopPolicyName, {term1, term2}));
  }

  /*
   * eBGP source peer that injects routes. v4-only: this test is entirely v4,
   * so a single AFI keeps initialization to one EoR (important under the tiny
   * highWm=1 queue, where a second EoR would itself trigger backpressure).
   */
  BgpPeerSpec makeSource() {
    return BgpPeerSpec{
        .asn = kPeerAsn3,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr3,
        .v4Nexthop = kNextHopV4_3,
        .v6Nexthop = kEmptyV6Nexthop,
        .disableIpv6Afi = true,
    };
  }

  /* v4-only eBGP receiving peer with the egress SetNexthop policy. */
  BgpPeerSpec makeDestWithPolicy() {
    return BgpPeerSpec{
        .asn = kPeerAsn5,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kEmptyV6Nexthop,
        .disableIpv6Afi = true,
        .egressPolicyName = kSetNexthopPolicyName,
    };
  }
};

TEST_F(
    E2EEgressPolicySetNexthopTest,
    BackpressureCollapsesFlap_FinalNexthopApplied) {
  /* Arrange: peers + egress policy, with a tiny queue (block after 1 item). */
  addPeer(makeSource());
  addPeer(makeDestWithPolicy());
  setupEgressPolicy();
  createRib();
  /* Converge immediately so the receiving peer's initial dump completes before
   * we pad/announce; otherwise the post-convergence route advertisement races
   * with the test reads. */
  setEorTimeSeconds(0);
  createPeerManager(
      /*enableUpdateGroup=*/false);

  setDefaultQueueSizes(/*capacity=*/2, /*highWm=*/1, /*lowWm=*/0);
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId srcId{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId dstId{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(srcId);
  sendEoRToPeer(dstId);
  ASSERT_TRUE(waitForEoR(srcId));

  /*
   * Leave the receiving peer's single EoR in its egress queue. With highWm=1
   * that one message keeps the queue blocked (but not backpressured) — no
   * padding needed.
   */
  ASSERT_TRUE(waitForPeerQueueBlocked(dstId));

  const folly::CIDRNetwork expectedCidr{folly::IPAddress(kPrefix), kPrefixLen};

  /*
   * Announce kPrefix with no community. waitForRouteInShadowRib confirms it was
   * published; waitForChangeListConsumerReady confirms it was consumed into the
   * packing list as { selfNh 5.0.0.0/8 }. The queue stays blocked on the EoR.
   */
  addRoute("v4", kPrefix, kPrefixLen, kPeerAddr3, kPolicyNexthop, kAsPath);
  ASSERT_TRUE(waitForRouteInShadowRib(expectedCidr));
  ASSERT_TRUE(waitForChangeListConsumerReady(kPeerAddr5));
  ASSERT_TRUE(isPeerQueueBlocked(dstId));

  /*
   * The RIB-OUT entry now holds the self-nexthop result (term2, no SetNexthop),
   * so isNexthopSetByPolicy is false.
   */
  {
    auto nhSetByPolicy =
        checkRibOutNexthopSetByPolicy(kPeerAddr5, expectedCidr);
    ASSERT_TRUE(nhSetByPolicy.has_value());
    EXPECT_FALSE(*nhSetByPolicy);
  }

  /*
   * Flap kPrefix by re-advertising it with the community. The queue is still
   * blocked, so the change-list consumer cannot drain this update into the
   * packing list. Confirm it is pended on kPrefix's change item (published but
   * unconsumed).
   */
  addRoute(
      "v4",
      kPrefix,
      kPrefixLen,
      kPeerAddr3,
      kPolicyNexthop,
      kAsPath,
      kSetNexthopCommunity);
  ASSERT_TRUE(waitForRouteInShadowRib(expectedCidr));
  ASSERT_TRUE(waitForChangeListConsumerPended(kPeerAddr5, expectedCidr));

  /*
   * Drain every emitted message in order (popping the EoR unblocks the queue,
   * which lets the consumer drain the flap and emit the collapsed result). We
   * do not drop anything, so we can assert the exact order and cardinality.
   *
   * Expected, in order:
   *   [0] EoR (the message that was blocking the queue)
   *   [1] 5.0.0.0/8 announcement with the policy-set nexthop 10.0.0.1
   *       (term1 SetNexthop overriding nexthop-self), communities cleared
   * The flap collapses against the self-nexthop entry already in the packing
   * list, so no stale self-nexthop announcement appears.
   */
  auto drained = drainAllOutboundMessagesToOrderedVec(dstId);

  ASSERT_EQ(2u, drained.size());
  EXPECT_TRUE(drained[0].isEoR);

  ASSERT_NE(nullptr, drained[1].update);
  EXPECT_TRUE(findPrefixInAnnouncements(
      *drained[1].update, /*isV4=*/true, expectedCidr, /*addPathId=*/0));
  EXPECT_TRUE(verifyRouteAttributes(
      *drained[1].update,
      kPolicyNexthop,
      /*expectedAsPath=*/"",
      /*expectedCommunity=*/""));
  EXPECT_TRUE(drained[1].update->attrs()->communities()->empty());

  /*
   * After the flap was consumed and advertised, the RIB-OUT entry reflects the
   * policy SetNexthop result, so isNexthopSetByPolicy is now true.
   */
  auto nhSetByPolicy = checkRibOutNexthopSetByPolicy(kPeerAddr5, expectedCidr);
  ASSERT_TRUE(nhSetByPolicy.has_value());
  EXPECT_TRUE(*nhSetByPolicy);
}

/*
 * Backpressure + announce-then-withdraw (the stale-route SEV scenario).
 *
 * An announcement whose egress SetNexthop fired (isNexthopSetByPolicy=true) is
 * parked in the packing list while the egress queue is blocked. The prefix is
 * then withdrawn. The withdrawal must coalesce with the still-pending flag=true
 * announce, so the peer receives the prefix exactly once -- as a withdrawal.
 *
 * Without the both-flags cleanup in tryUpdateAttrToPrefixMapImpl, the flag=true
 * announce is orphaned (the withdraw only probes the flag=false bucket) and is
 * emitted alongside the withdrawal, re-installing the route on the peer after
 * the withdrawal -- exactly the production stale route.
 */
TEST_F(
    E2EEgressPolicySetNexthopTest,
    BackpressureCollapsesAnnounceThenWithdraw_OnlyWithdrawalEmitted) {
  /* Arrange: peers + egress policy, with a tiny queue (block after 1 item). */
  addPeer(makeSource());
  addPeer(makeDestWithPolicy());
  setupEgressPolicy();
  createRib();
  setEorTimeSeconds(0);
  createPeerManager(
      /*enableUpdateGroup=*/false);

  setDefaultQueueSizes(/*capacity=*/2, /*highWm=*/1, /*lowWm=*/0);
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  BgpPeerId srcId{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId dstId{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(srcId);
  sendEoRToPeer(dstId);
  ASSERT_TRUE(waitForEoR(srcId));

  /* The dest's single EoR keeps its egress queue blocked (highWm=1). */
  ASSERT_TRUE(waitForPeerQueueBlocked(dstId));

  const folly::CIDRNetwork expectedCidr{folly::IPAddress(kPrefix), kPrefixLen};

  /*
   * Announce kPrefix WITH the community so term1's SetNexthop fires
   * (isNexthopSetByPolicy=true). It is consumed into the packing list under the
   * flag=true bucket while the queue stays blocked.
   */
  addRoute(
      "v4",
      kPrefix,
      kPrefixLen,
      kPeerAddr3,
      kPolicyNexthop,
      kAsPath,
      kSetNexthopCommunity);
  ASSERT_TRUE(waitForRouteInShadowRib(expectedCidr));
  ASSERT_TRUE(waitForChangeListConsumerReady(kPeerAddr5));
  ASSERT_TRUE(isPeerQueueBlocked(dstId));
  {
    auto nhSetByPolicy =
        checkRibOutNexthopSetByPolicy(kPeerAddr5, expectedCidr);
    ASSERT_TRUE(nhSetByPolicy.has_value());
    EXPECT_TRUE(*nhSetByPolicy); /* announce staged with flag=true */
  }

  /*
   * Withdraw kPrefix. The queue is still blocked, so the change-list consumer
   * cannot drain; the withdrawal is pended on kPrefix's change item.
   */
  deleteRoute("v4", kPrefix, kPrefixLen, kPeerAddr3);
  ASSERT_TRUE(waitForChangeListConsumerPended(kPeerAddr5, expectedCidr));

  /*
   * Drain every emitted message in order. Popping the EoR unblocks the queue;
   * the consumer drains the withdrawal, which coalesces against the still-
   * pending flag=true announce (both-flags cleanup), so kPrefix is emitted
   * exactly once -- a withdrawal, with no orphaned announcement.
   */
  auto drained = drainAllOutboundMessagesToOrderedVec(dstId);

  int announceCnt = 0;
  int withdrawCnt = 0;
  for (const auto& msg : drained) {
    if (msg.update == nullptr) {
      continue;
    }
    if (findPrefixInAnnouncements(
            *msg.update, /*isV4=*/true, expectedCidr, /*addPathId=*/0)) {
      ++announceCnt;
    }
    if (findPrefixInWithdrawals(
            *msg.update, /*isV4=*/true, expectedCidr, /*addPathId=*/0)) {
      ++withdrawCnt;
    }
  }
  EXPECT_EQ(
      0, announceCnt); /* orphan announce coalesced by both-flags cleanup */
  EXPECT_EQ(1, withdrawCnt); /* prefix emitted exactly once, as a withdrawal */
}

} // namespace facebook::bgp
