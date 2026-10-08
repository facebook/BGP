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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define AdjRib_TEST_FRIENDS                                                  \
  friend class AdjRibOutboundFixtureV4V6Nexthop;                             \
  FRIEND_TEST(AdjRibOutboundFixtureV4V6Nexthop, VerifyV4V6PrefixesNexthops); \
  FRIEND_TEST(                                                               \
      AdjRibOutboundFixtureV4V6Nexthop,                                      \
      VerifyNormalizedNexthop_BuildAndQueueAnnouncements);                   \
  FRIEND_TEST(                                                               \
      AdjRibOutboundFixtureV4V6Nexthop,                                      \
      VerifyNormalizedNexthop_BuildUpdateWithSizeEstimation);                \
  FRIEND_TEST(                                                               \
      AdjRibOutboundFixture,                                                 \
      AnnounceThenImplicitWithdraw_PolicyNexthop_OrphansAnnounce);           \
  FRIEND_TEST(                                                               \
      AdjRibOutboundFixtureV4V6Nexthop,                                      \
      SameAttrsDifferentNexthopFlag_StaySeparateBuckets);

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"

namespace facebook::bgp {
using namespace facebook::nettools::bgplib;

using facebook::network::toIPPrefix;
using folly::IPAddress;

// Test parameters for different nexthop scenarios
struct V4V6NexthopTestParam {
  std::string testName;
  bool isEBgpPeer;
  bool isRrClient;
  bool nextHopSelf;
  bool isV4OverV6Negotiated;
  folly::IPAddress configuredV4Nexthop;
  folly::IPAddress configuredV6Nexthop;
  folly::IPAddress originalV4Nexthop;
  folly::IPAddress originalV6Nexthop;
  folly::IPAddress expectedV4Nexthop;
  folly::IPAddress expectedV6Nexthop;
};

class AdjRibOutboundFixtureV4V6Nexthop
    : public AdjRibOutboundFixture,
      public ::testing::WithParamInterface<V4V6NexthopTestParam> {
 public:
  std::shared_ptr<AdjRib> setupEbgpAdjRib(
      const folly::IPAddress& v4Nexthop,
      const folly::IPAddress& v6Nexthop) {
    setupAdjRib(
        kLocalAs1,
        kLocalAs1,
        kRemoteAs2, /* different AS for eBGP */
        false, /* isRrClient */
        false, /* isConfedPeer */
        NextHopSelfConfigured(true),
        v4Nexthop, /* configured v4 local */
        v6Nexthop, /* configured v6 local */
        false, // sessionEstablish
        AfiIpv4Negotiated(true));
    auto adjRib = adjRib_;
    fm_->addTask([&, adjRib, adjRibInQ = adjRibInQ_] {
      adjRib->sessionEstablished(
          std::nullopt, /* remoteGrRestartTime */
          adjRibInQ,
          boundedAdjRibOutQ_,
          true /* isAfiIpv4Negotiated */,
          true /* isAfiIpv6Negotiated */,
          false /* isV4OverV6Nexthop */,
          false /* isEnhancedRouteRefreshNegotiated */,
          false /* isRouteRefreshNegotiated */,
          std::nullopt /* addPathCapa */);
      adjRib->startMessageProcessingLoop();
    });
    /* Restock the member queues on the test fixture for the next caller. */
    adjRibInQ_ = std::make_shared<AdjRib::AdjRibInQueueT>();
    return adjRib;
  }

  void announceTo(
      AdjRib* adjRib,
      const folly::F14NodeMap<
          std::shared_ptr<BgpPath>,
          std::vector<folly::CIDRNetwork>>& entries,
      bool ebgpPeer = true) {
    auto ribOutMsg = buildAnnouncementFromMap(entries, ebgpPeer);
    adjRib->processRibMessage(ribOutMsg);
    adjRib->scheduleSendBgpUpdates(true /* tryPullNewChangeItems */);
  }

  void verifyPrefixesAndNexthopInUpdate(
      const std::optional<FiberBgpPeer::InputMessageT>& updateMsg,
      const folly::F14NodeSet<folly::CIDRNetwork>& expected,
      const folly::IPAddress& nh) {
    auto update = std::get<std::shared_ptr<const BgpUpdate2>>(*updateMsg);
    EXPECT_EQ(expected.size(), update->mpAnnounced()->prefixes()->size());

    folly::F14NodeSet<folly::CIDRNetwork> observed;
    for (auto& pfx : *update->mpAnnounced()->prefixes()) {
      observed.insert(network::toCIDRNetwork(*pfx.prefix()));
    }
    EXPECT_EQ(expected, observed);

    /* Verify nexthops match expected values. */
    EXPECT_EQ(nh.str(), *update->attrs()->nexthop());

    /* Verify nexthops inside of mpAnnounced also match expected values. */
    EXPECT_EQ(network::toBinaryAddress(nh), *update->mpAnnounced()->nexthop());
  }

  void terminateSharedAdjRib(std::shared_ptr<AdjRib>& adjRib) {
    adjRib->adjRibInQueue_->fiberPush(
        nettools::bgplib::FiberBgpPeer::BgpSessionStop{false});
  }
};

TEST_F(
    AdjRibOutboundFixtureV4V6Nexthop,
    VerifyNormalizedNexthop_BuildUpdateWithSizeEstimation) {
  /*
   * Make two AdjRibs with different configured nexthops.
   * They will set the nexthop on egress attributes to self.
   */
  auto adjRib1 = setupEbgpAdjRib(kV4Nexthop1, kV6Nexthop1);
  auto adjRib2 = setupEbgpAdjRib(kV4Nexthop2, kV6Nexthop2);

  /*
   * Create one shared attr from RIB and announce kV4Prefix1 to adjRib1
   * and kV4Prefix2 to adjRib2. To verify the total deduplication at the
   * end of the test, clone the attr for each AdjRib so their memory
   * is even different.
   */
  BgpUpdate2 update = buildBgpUpdateAttributes(kV4Nexthop3);
  auto attrs1 = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(update)));
  auto attrs2 = attrs1->clone();
  EXPECT_NE(attrs1, attrs2);

  auto ribMsg1 = buildAnnouncementFromMap({{attrs1, {kV4Prefix1}}});
  auto ribMsg2 = buildAnnouncementFromMap({{attrs2, {kV4Prefix2}}});

  fm_->addTask([&] {
    adjRib1->handleRibAnnouncedEntry(
        std::get<RibOutAnnouncement>(ribMsg1).entries[0],
        true /* initialDump*/);
    adjRib2->handleRibAnnouncedEntry(
        std::get<RibOutAnnouncement>(ribMsg2).entries[0],
        true /* initialDump*/);

    EXPECT_EQ(1, adjRib1->attrToPrefixMap_.size());
    EXPECT_EQ(1, adjRib2->attrToPrefixMap_.size());

    auto update1 = adjRib1->buildUpdateWithSizeEstimation(
        adjRib1->attrToPrefixMap_.begin()->first,
        adjRib1->attrToPrefixMap_.begin()->second);
    auto update2 = adjRib2->buildUpdateWithSizeEstimation(
        adjRib2->attrToPrefixMap_.begin()->first,
        adjRib2->attrToPrefixMap_.begin()->second);

    verifyPrefixesAndNexthopInUpdate(
        FiberBgpPeer::InputMessageT(update1), {kV4Prefix1}, kV4Nexthop1);
    verifyPrefixesAndNexthopInUpdate(
        FiberBgpPeer::InputMessageT(update2), {kV4Prefix2}, kV4Nexthop2);

    /*
     * Verify each AdjRib's entry has the normalized nexthop.
     * Note that, in this scenario, this assumes that the policy
     * does not modify the nexthop; so the normalized nexthop
     * is the nexthop that was announced from RIB, which was
     * kV4Nexthop3.
     */
    auto entry1 = adjRib1->getRibEntry(/*ingress=*/false, kV4Prefix1);
    auto entry2 = adjRib2->getRibEntry(/*ingress=*/false, kV4Prefix2);

    /* Let's verify that the post attrs are the same shared_ptr. */
    EXPECT_EQ(entry1->getPostAttr(), entry2->getPostAttr());
    EXPECT_EQ(kV4Nexthop3, entry1->getPostAttr()->getNexthop());
  });
  fm_->addTask([&] {
    terminateSharedAdjRib(adjRib1);
    terminateSharedAdjRib(adjRib2);
  });

  evb_.loop();
}

/*
 * Test verifying BgpUpdate2 nexthops for v4 and v6 prefixes are set
 * correctly when they share the same initial BgpPath.
 */
TEST_P(AdjRibOutboundFixtureV4V6Nexthop, VerifyV4V6PrefixesNexthopsSet) {
  const auto& param = GetParam();

  // Setup AdjRib with the test parameters
  setupAdjRib(
      kLocalAs1,
      kLocalAs1,
      param.isEBgpPeer ? kRemoteAs2 : kLocalAs1, // different AS for eBGP
      param.isRrClient, // isRrClient
      false, // isConfedPeer
      NextHopSelfConfigured(param.nextHopSelf),
      param.configuredV4Nexthop,
      param.configuredV6Nexthop,
      true, // sessionEstablish
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      V4OverV6Nexthop(param.isV4OverV6Negotiated),
      EnhancedRouteRefreshNegotiated(false),
      RouteRefreshNegotiated(false));

  // Create BgpPaths that are the same except for nexthop.
  BgpUpdate2 updateV4 = buildBgpUpdateAttributes(param.originalV4Nexthop);
  BgpUpdate2 updateV6 = buildBgpUpdateAttributes(param.originalV6Nexthop);
  auto attrsV4 = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(updateV4)));
  auto attrsV6 = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(updateV6)));

  fm_->addTask([&] {
    announceTo(
        adjRib_.get(),
        {{attrsV4, {kV4Prefix1}}, {attrsV6, {kV6Prefix1}}},
        param.isEBgpPeer);
  });

  fm_->addTask([&] {
    /* Let sendBgpUpdates coro run if scheduled. */
    fiberSleepFor(10ms);

    // We should receive exactly 2 BGP updates (one for v4, one for v6)
    std::vector<std::shared_ptr<const BgpUpdate2>> updates;

    // Collect both updates
    for (int i = 0; i < 2; i++) {
      auto msg = folly::coro::blockingWait(popFromEgressQueue());
      ASSERT_TRUE(
          std::holds_alternative<std::shared_ptr<const BgpUpdate2>>(*msg));
      auto bgpUpdate = std::get<std::shared_ptr<const BgpUpdate2>>(*msg);
      updates.push_back(bgpUpdate);
    }

    // Sort updates by AFI to ensure consistent ordering
    std::sort(updates.begin(), updates.end(), [](const auto& a, const auto& b) {
      return a->mpAnnounced()->afi() < b->mpAnnounced()->afi();
    });

    // First update should be v4, second should be v6
    auto v4Update = updates[0];
    auto v6Update = updates[1];

    // Verify AFI/SAFI
    EXPECT_EQ(BgpUpdateAfi::AFI_IPv4, v4Update->mpAnnounced()->afi());
    EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, v6Update->mpAnnounced()->afi());
    EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, v4Update->mpAnnounced()->safi());
    EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, v6Update->mpAnnounced()->safi());

    // Verify prefixes
    ASSERT_EQ(1, v4Update->mpAnnounced()->prefixes()->size());
    ASSERT_EQ(1, v6Update->mpAnnounced()->prefixes()->size());
    EXPECT_EQ(
        toIPPrefix(kV4Prefix1),
        *v4Update->mpAnnounced()->prefixes()[0].prefix());
    EXPECT_EQ(
        toIPPrefix(kV6Prefix1),
        *v6Update->mpAnnounced()->prefixes()[0].prefix());

    // Verify nexthops match expected values
    EXPECT_EQ(param.expectedV4Nexthop.str(), *v4Update->attrs()->nexthop());
    EXPECT_EQ(param.expectedV6Nexthop.str(), *v6Update->attrs()->nexthop());

    // Verify nexthops inside of mpAnnounced also match expected values.
    EXPECT_EQ(
        network::toBinaryAddress(param.expectedV4Nexthop),
        *v4Update->mpAnnounced()->nexthop());
    EXPECT_EQ(
        network::toBinaryAddress(param.expectedV6Nexthop),
        *v6Update->mpAnnounced()->nexthop());

    /*
     * Verify the adjRibEntry has the normalized nexthop.
     * Note that, in this scenario, this assumes that the policy
     * does not modify the nexthop; so the normalized nexthop
     * is the nexthop that was announced from RIB.
     */
    auto entryV4 = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
    auto entryV6 = adjRib_->getRibEntry(/*ingress=*/false, kV6Prefix1);
    EXPECT_EQ(param.originalV4Nexthop, entryV4->getPostAttr()->getNexthop());
    EXPECT_EQ(param.originalV6Nexthop, entryV6->getPostAttr()->getNexthop());

    // Verify no more messages in queue
    EXPECT_EQ(0, boundedAdjRibOutQ_->size());

    terminateAdjRib();
  });

  evb_.loop();
}

/**
 * Test parameters covering different v4 and v6 nexthop assignment
 * via getNewNexthopFromAttributesOut given the following scenarios
 *
 *   1. peer is configured as EBGP or IBGP
 *   2. peer is configured as RR client or not RR client
 *   3. peer allows v4OverV6 or does not allow v4OverV6
 *   4. Nexthop of postAttrs after policy evaluation is zero
 */
INSTANTIATE_TEST_SUITE_P(
    GetNewNexthopFromAttributesOutTest,
    AdjRibOutboundFixtureV4V6Nexthop,
    ::testing::Values(
        V4V6NexthopTestParam{
            .testName = "eBgpPeer_NexthopSelf",
            .isEBgpPeer = true,
            .isRrClient = false,
            .nextHopSelf = true,
            .isV4OverV6Negotiated = false,
            .configuredV4Nexthop = kV4Nexthop1,
            .configuredV6Nexthop = kV6Nexthop1,
            .originalV4Nexthop = kV4Nexthop2,
            .originalV6Nexthop = kV6Nexthop2,
            .expectedV4Nexthop = kV4Nexthop1,
            .expectedV6Nexthop = kV6Nexthop1},
        V4V6NexthopTestParam{
            .testName = "eBgpPeer_V4OverV6Enabled",
            .isEBgpPeer = true,
            .isRrClient = false,
            .nextHopSelf = true,
            .isV4OverV6Negotiated = true,
            .configuredV4Nexthop = kV4Nexthop1,
            .configuredV6Nexthop = kV6Nexthop1,
            .originalV4Nexthop = kV4Nexthop2,
            .originalV6Nexthop = kV6Nexthop2,
            .expectedV4Nexthop =
                kV6Nexthop1, // V4 uses V6 nexthop when V4OverV6
            .expectedV6Nexthop = kV6Nexthop1},
        V4V6NexthopTestParam{
            .testName = "iBgpPeer_RrClient_NexthopSelf",
            .isEBgpPeer = false,
            .isRrClient = true,
            .nextHopSelf = true,
            .isV4OverV6Negotiated = false,
            .configuredV4Nexthop = kV4Nexthop1,
            .configuredV6Nexthop = kV6Nexthop1,
            .originalV4Nexthop = kV4Nexthop2,
            .originalV6Nexthop = kV6Nexthop2,
            .expectedV4Nexthop = kV4Nexthop1,
            .expectedV6Nexthop = kV6Nexthop1},
        V4V6NexthopTestParam{
            .testName = "iBgpPeer_RrClient_NexthopSelfFalse",
            .isEBgpPeer = false,
            .isRrClient = true,
            .nextHopSelf = false,
            .isV4OverV6Negotiated = false,
            .configuredV4Nexthop = kV4Nexthop1,
            .configuredV6Nexthop = kV6Nexthop1,
            .originalV4Nexthop = kV4Nexthop2,
            .originalV6Nexthop = kV6Nexthop2,
            .expectedV4Nexthop = kV4Nexthop2,
            .expectedV6Nexthop = kV6Nexthop2},
        V4V6NexthopTestParam{
            .testName = "eBgpPeer_V4V6NexthopZero",
            .isEBgpPeer = true,
            .isRrClient = false,
            .nextHopSelf = false,
            .isV4OverV6Negotiated = false,
            .configuredV4Nexthop = kV4Nexthop1,
            .configuredV6Nexthop = kV6Nexthop1,
            .originalV4Nexthop = folly::IPAddress("0.0.0.0"),
            .originalV6Nexthop = folly::IPAddress("::"),
            .expectedV4Nexthop = kV4Nexthop1, // Should trigger configured V4
            .expectedV6Nexthop = kV6Nexthop1 // Should trigger configured V6
        },
        V4V6NexthopTestParam{
            .testName = "eBgpPeer_V4LocalRouteOnly_NexthopUseV4BgpId",
            .isEBgpPeer = true,
            .isRrClient = false,
            .nextHopSelf = false,
            .isV4OverV6Negotiated = false,
            .configuredV4Nexthop = folly::IPAddress("0.0.0.0"),
            .configuredV6Nexthop = kV6Nexthop1,
            .originalV4Nexthop = kV4Nexthop2,
            .originalV6Nexthop = kV6Nexthop2,
            .expectedV4Nexthop = kLocalAddr1.asV4(), // localBgpId
            .expectedV6Nexthop = kV6Nexthop1},
        V4V6NexthopTestParam{
            .testName = "eBgpPeer_V6LocalRouteOnly_NexthopUseV6BgpId",
            .isEBgpPeer = true,
            .isRrClient = false,
            .nextHopSelf = false,
            .isV4OverV6Negotiated = false,
            .configuredV4Nexthop = kV4Nexthop1,
            .configuredV6Nexthop = folly::IPAddress("::"),
            .originalV4Nexthop = kV4Nexthop2,
            .originalV6Nexthop = kV6Nexthop2,
            .expectedV4Nexthop = kV4Nexthop1,
            .expectedV6Nexthop =
                folly::IPAddress::createIPv6(kLocalAddr1.asV4()) // localBgpId
        }),
    [](const ::testing::TestParamInfo<V4V6NexthopTestParam>& info) {
      return info.param.testName;
    });

/**
 * Tests for shouldApplyNexthopSelf() / getNewNexthopFromAttributesOut()
 * with the isNexthopSetByPolicy parameter.
 */
class NexthopSetByPolicyTest : public AdjRibOutboundFixture {};

TEST_F(NexthopSetByPolicyTest, eBgpPeer_PolicySetNexthop_SkipsNexthopSelf) {
  setupAdjRib(
      kLocalAs1,
      kLocalAs1,
      kRemoteAs2, /* eBGP */
      false, /* isRrClient */
      false, /* isConfedPeer */
      NextHopSelfConfigured(true),
      kV4Nexthop1, /* configured v4 local */
      kV6Nexthop1 /* configured v6 local */);

  fm_->addTask([&] {
    BgpUpdate2 update = buildBgpUpdateAttributes(kV4Nexthop2);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(update)));

    /* Without isNexthopSetByPolicy: eBGP applies nexthop-self */
    EXPECT_EQ(
        kV4Nexthop1,
        adjRib_->getNewNexthopFromAttributesOut(
            true /* isV4 */, attrs, false /* isNexthopSetByPolicy */));

    /* With isNexthopSetByPolicy: policy nexthop is preserved */
    EXPECT_EQ(
        kV4Nexthop2,
        adjRib_->getNewNexthopFromAttributesOut(
            true /* isV4 */, attrs, true /* isNexthopSetByPolicy */));

    terminateAdjRib();
  });

  evb_.loop();
}

TEST_F(NexthopSetByPolicyTest, iBgpPeer_NexthopSelf_PolicySetNexthop) {
  setupAdjRib(
      kLocalAs1,
      kLocalAs1,
      kLocalAs1, /* iBGP (same AS) */
      false, /* isRrClient */
      false, /* isConfedPeer */
      NextHopSelfConfigured(true),
      kV4Nexthop1, /* configured v4 local */
      kV6Nexthop1 /* configured v6 local */);

  fm_->addTask([&] {
    BgpUpdate2 update = buildBgpUpdateAttributes(kV4Nexthop2);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(update)));

    /* Without flag: nextHopSelf applies */
    EXPECT_EQ(
        kV4Nexthop1,
        adjRib_->getNewNexthopFromAttributesOut(
            true /* isV4 */, attrs, false /* isNexthopSetByPolicy */));

    /* With flag: policy nexthop preserved */
    EXPECT_EQ(
        kV4Nexthop2,
        adjRib_->getNewNexthopFromAttributesOut(
            true /* isV4 */, attrs, true /* isNexthopSetByPolicy */));

    terminateAdjRib();
  });

  evb_.loop();
}

TEST_F(
    NexthopSetByPolicyTest,
    ZeroNexthop_AlwaysOverridden_EvenWithPolicyFlag) {
  setupAdjRib(
      kLocalAs1,
      kLocalAs1,
      kRemoteAs2, /* eBGP */
      false, /* isRrClient */
      false, /* isConfedPeer */
      NextHopSelfConfigured(true),
      kV4Nexthop1, /* configured v4 local */
      kV6Nexthop1 /* configured v6 local */);

  fm_->addTask([&] {
    BgpUpdate2 update = buildBgpUpdateAttributes(folly::IPAddress("0.0.0.0"));
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(update)));

    /* Zero nexthop is always invalid — nexthop-self overrides even with flag */
    EXPECT_EQ(
        kV4Nexthop1,
        adjRib_->getNewNexthopFromAttributesOut(
            true /* isV4 */, attrs, true /* isNexthopSetByPolicy */));

    /* Same result without flag */
    EXPECT_EQ(
        kV4Nexthop1,
        adjRib_->getNewNexthopFromAttributesOut(
            true /* isV4 */, attrs, false /* isNexthopSetByPolicy */));

    terminateAdjRib();
  });

  evb_.loop();
}

TEST_F(NexthopSetByPolicyTest, iBgpPeer_NoNexthopSelf_NoFlag_KeepsOriginal) {
  setupAdjRib(
      kLocalAs1,
      kLocalAs1,
      kLocalAs1, /* iBGP */
      false, /* isRrClient */
      false, /* isConfedPeer */
      NextHopSelfConfigured(false),
      kV4Nexthop1,
      kV6Nexthop1);

  fm_->addTask([&] {
    BgpUpdate2 update = buildBgpUpdateAttributes(kV4Nexthop2);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(update)));

    /* iBGP without nextHopSelf: original nexthop preserved regardless */
    EXPECT_EQ(
        kV4Nexthop2,
        adjRib_->getNewNexthopFromAttributesOut(
            true /* isV4 */, attrs, false /* isNexthopSetByPolicy */));

    terminateAdjRib();
  });

  evb_.loop();
}

/*
 * AdjRib-level reproduction of the prod stale-route bug.
 *
 * Under egress backpressure (attrToPrefixMap_ is NOT drained between the
 * announce and the withdraw), advertise a prefix to an iBGP peer whose egress
 * policy sets the nexthop (SetNexthop -> isNexthopSetByPolicy=true), staging it
 * under the packing-list key {attrs, afi, true}. Then flip the best path to an
 * iBGP-learned route: split-horizon turns the re-announce into an implicit
 * withdraw, which is processed with the default isNexthopSetByPolicy=false.
 *
 * isNexthopSetByPolicy is part of the BgpPathWithAfi key, so the flag=false
 * withdraw must still clean up the prefix that was staged under flag=true. The
 * fix (tryUpdateAttrToPrefixMapImpl probes BOTH flag states on cleanup)
 * coalesces the pending announce into the withdrawal, leaving exactly one
 * bucket. Without it, the announce is orphaned in {attrs, afi, true} alongside
 * the withdrawal in {nullptr, afi, false}, and can be re-sent on the wire after
 * the withdraw -- re-installing the route on the peer while the sender's entry
 * shows it withdrawn.
 *
 * Driven through the real per-peer paths:
 *   announce: processRibMessage -> processRibOutAnnouncement ->
 *             handleRibAnnouncedEntry -> processRibAnnouncedEntry.
 *   withdraw: processRibMessage -> processRibOutAnnouncement ->
 *             handleRibAnnouncedEntry -> handleImplicitWithdrawal ->
 *             processRibWithdraw -> tryInsertWithdrawal.
 *
 * Asserts exactly one (withdrawal) bucket: the orphaned flag=true announce
 * bucket must be gone.
 */
TEST_F(
    AdjRibOutboundFixture,
    AnnounceThenImplicitWithdraw_PolicyNexthop_OrphansAnnounce) {
  gflags::FlagSaver flags;
  /*
   * Backpressure ON: processRibMessage stages into attrToPrefixMap_ without
   * draining, so the announce is still pending when the withdraw arrives.
   */
  FLAGS_enable_egress_backpressure_in_adjribout_tests = true;

  /*
   * IBGP peer with an EGRESS policy whose only action sets the nexthop, so
   * processRibAnnouncedEntry produces isNexthopSetByPolicy=true.
   */
  const std::string policyName = kEgressPolicyName;
  auto nexthopAction = createBgpPolicyNexthopAction(kV4Nexthop2);
  auto policyManager = std::make_shared<PolicyManager>(
      createBgpPolicies(policyName, {}, {std::move(nexthopAction)}),
      createTestBgpGlobalConfig());
  setupAdjRib(policyManager, policyName); // IBGP, session established

  fm_->addTask([&] {
    auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;

    /*
     * 1) Announce kV4Prefix1 sourced from an eBGP peer. canAnnounce() is true
     *    for an eBGP-sourced route to an iBGP peer, so processRibAnnouncedEntry
     *    runs, the SetNexthop egress policy fires (isNexthopSetByPolicy=true),
     *    and the prefix is staged into bucket {attrs, AFI_IPv4, true}.
     *    sendWithEoR=true sets initialDump=true so processRibOutAnnouncement
     *    is not gated on egressEoRsSent_ (which is false: we never drain).
     */
    auto announce = createRibMultipleAnnounce(
        {kV4Prefix1}, kV4Nexthop3, eBgpPeer_, /*sendWithEoR=*/true);
    adjRib_->processRibMessage(announce);

    auto* entry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
    ASSERT_NE(nullptr, entry);
    ASSERT_TRUE(entry->isNexthopSetByPolicy())
        << "egress SetNexthop policy did not fire on the announce";
    auto postAttr = entry->getPostAttr();
    ASSERT_NE(nullptr, postAttr);
    ASSERT_EQ(1u, attrToPrefixMap.size());
    ASSERT_TRUE(attrToPrefixMap.contains(
        BgpPathWithAfi{
            postAttr, BgpUpdateAfi::AFI_IPv4, /*isNexthopSetByPolicy=*/true}));

    /*
     * 2) Best path changes to an iBGP-learned route. Announcing an iBGP-sourced
     *    route to an iBGP peer is blocked by split-horizon, so
     *    handleRibAnnouncedEntry -> handleImplicitWithdrawal ->
     *    processRibWithdraw -> tryInsertWithdrawal. No drain occurred between
     *    the two processRibMessage calls (we never scheduled sendBgpUpdates),
     *    so the announce from step 1 is still pending in attrToPrefixMap_.
     */
    auto ibgpAnnounce = createRibMultipleAnnounce(
        {kV4Prefix1}, kV4Nexthop1, iBgpPeer_, /*sendWithEoR=*/true);
    adjRib_->processRibMessage(ibgpAnnounce);

    /*
     * EXPECTED: the implicit withdraw coalesces with the staged announce ->
     * exactly ONE withdrawal bucket. With the bug, the announce is orphaned and
     * there are TWO buckets.
     */
    EXPECT_EQ(1u, attrToPrefixMap.size());
    EXPECT_FALSE(attrToPrefixMap.contains(
        BgpPathWithAfi{
            postAttr, BgpUpdateAfi::AFI_IPv4, /*isNexthopSetByPolicy=*/true}))
        << "Orphaned announce left in packing list: the implicit withdraw "
           "dropped the isNexthopSetByPolicy flag and failed to coalesce with "
           "the pending announce (it can be re-sent on the wire after the "
           "withdraw, re-installing the route on the peer)";

    terminateAdjRib();
  });

  evb_.loop();
}

/*
 * Guard for the design doc's "Packing List Corner Case"
 * (https://docs.google.com/document/d/17a71e1cX0uI-G7cONa14lLDjNwesu_uh8ce37eme3Aw,
 * Approach 2; behavioral-matrix row "same NH, different flags -> Different
 * buckets").
 *
 * Two DIFFERENT prefixes can resolve to identical post-policy attrs but differ
 * in isNexthopSetByPolicy:
 *   - Prefix A: an egress SetNexthop set the nexthop -> flag = true.
 *   - Prefix B: no policy, but its bestpath nexthop is already that value ->
 *     flag = false.
 * isNexthopSetByPolicy is part of the BgpPathWithAfi key (hash + equality,
 * AdjRibStructs.h), so the two prefixes MUST land in separate packing-list
 * buckets and each is sent with the correct nexthop: policy NH honored for A
 * (shouldApplyNexthopSelf == false), nexthop-self for B on an eBGP peer.
 *
 * If the flag were dropped from the key, both prefixes would collapse into one
 * bucket and a single flag would decide the nexthop for both -- wrong for one.
 * This test fails in that case.
 */
TEST_F(
    AdjRibOutboundFixtureV4V6Nexthop,
    SameAttrsDifferentNexthopFlag_StaySeparateBuckets) {
  /*
   * eBGP peer with configured local nexthop kV4Nexthop1, so nexthop-self
   * (flag = false) resolves to kV4Nexthop1. The shared attrs carry the
   * policy-set nexthop kV4Nexthop2.
   */
  auto adjRib = setupEbgpAdjRib(kV4Nexthop1, kV6Nexthop1);

  BgpUpdate2 update = buildBgpUpdateAttributes(kV4Nexthop2);
  std::shared_ptr<const BgpPath> attrs =
      std::make_shared<facebook::bgp::BgpPath>(
          BgpPathFields(*BgpUpdate2toBgpPathC(update)));

  fm_->addTask([&] {
    auto& packingList = adjRib->attrToPrefixMap_;

    /* Prefix A: nexthop set by policy -> key flag = true. */
    adjRib->tryUpdateAttrToPrefixMap(
        std::make_pair(kV4Prefix1, 0u /* pathId */),
        nullptr /* oldPath */,
        attrs,
        true /* isNexthopSetByPolicy */);
    /* Prefix B: no policy -> key flag = false; SAME attrs shared_ptr. */
    adjRib->tryUpdateAttrToPrefixMap(
        std::make_pair(kV4Prefix2, 0u /* pathId */),
        nullptr /* oldPath */,
        attrs,
        false /* isNexthopSetByPolicy */);

    /* Flag is part of the key, so the two prefixes occupy two buckets. */
    EXPECT_EQ(2, packingList.size());

    for (auto& [key, prefixSet] : packingList) {
      auto out = adjRib->buildUpdateWithSizeEstimation(key, prefixSet);
      if (key.isNexthopSetByPolicy) {
        /* Policy nexthop is honored (shouldApplyNexthopSelf == false). */
        verifyPrefixesAndNexthopInUpdate(
            FiberBgpPeer::InputMessageT(out), {kV4Prefix1}, kV4Nexthop2);
      } else {
        /* eBGP + flag = false -> nexthop-self (configured local nexthop). */
        verifyPrefixesAndNexthopInUpdate(
            FiberBgpPeer::InputMessageT(out), {kV4Prefix2}, kV4Nexthop1);
      }
    }

    terminateSharedAdjRib(adjRib);
  });

  evb_.loop();
}

} // namespace facebook::bgp
