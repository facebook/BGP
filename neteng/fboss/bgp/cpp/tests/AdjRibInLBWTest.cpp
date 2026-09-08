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

#define AdjRib_TEST_FRIENDS          \
  friend class AdjRibInboundFixture; \
  FRIEND_TEST(AdjRibInboundFixture, ReceiveLinkBandwidth);

#include <folly/IPAddress.h>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibInUtils.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;
using namespace facebook::neteng::fboss::bgp::thrift;
using namespace facebook::neteng::fboss::bgp_attr;
using namespace folly::fibers;

using folly::IPAddress;

using bgp_policy::BgpPolicyActionType;

namespace facebook::bgp {

using ::testing::ElementsAre;

/**
 * Ensure that route advertisement or withdraw from `AdjRib -> Rib` includes
 * the peer link-bandwidth-bps if configure.
 */
TEST_F(AdjRibInboundFixture, PeerLinkBandwidthBps) {
  const std::optional<float> linkBandwidthBps{100.0f};

  setupAdjRib(linkBandwidthBps);

  fm_->addTask([&] {
    // Announce the route
    auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
    adjRibInQ_->fiberPush(std::move(update));

    // Verify rib In message
    auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    ASSERT_TRUE(std::holds_alternative<RibInAnnouncement>(msg));
    auto announcement = std::get<RibInAnnouncement>(msg);
    EXPECT_EQ(kPeerAddr1, announcement.peer.addr);
    EXPECT_EQ(linkBandwidthBps, announcement.peer.ucmpWeight);
    PrefixPathIds prefixSet{{kV4Prefix1, kDefaultPathID}};
    EXPECT_EQ(prefixSet, announcement.pfxPathIds);

    // Enqueue messages for stop sequence
    terminateAdjRib();
  });

  evb_.loop();
}

/**
 * Verify all possible ReceiveLinkBandwidth configurations
 * settings:
 * - receivedLBW: 10Gbps
 * - peerLBW (from config): 200bps
 *
 * with different configs, we expect RIB to see attrs with following behaviors
 * DISABLE: -> attrs has no LbwExtCommunity
 * ACCEPT: -> [no change] attrs retain LbwExtCommunity as received (e.g 10Gbps)
 * SET_LINK_BPS: -> attrs has LbwExtCommunity overwritten with lbw defined in
 * Config (e.g 200bps)
 */
TEST_F(AdjRibInboundFixture, ReceiveLinkBandwidth) {
  const std::optional<float> linkBandwidthBps{200.0f};

  {
    // ReceiveLinkBandwidth: DISABLE -> no LbwExtCommunity
    setupAdjRib(linkBandwidthBps, ReceiveLinkBandwidth::DISABLE);

    fm_->addTask([&] {
      auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
      adjRibInQ_->fiberPush(std::move(update));

      // Verify rib In message
      auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      ASSERT_TRUE(std::holds_alternative<RibInAnnouncement>(msg));
      auto announcement = std::get<RibInAnnouncement>(msg);
      EXPECT_EQ(kPeerAddr1, announcement.peer.addr);
      EXPECT_EQ(linkBandwidthBps, announcement.peer.ucmpWeight);
      EXPECT_FALSE(announcement.attrs->hasNonTransitiveLbwExtCommunity());

      // Enqueue messages for stop sequence
      terminateAdjRib();

      auto msgWithdrawal =
          facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      EXPECT_TRUE(std::holds_alternative<RibInWithdrawal>(msgWithdrawal));
    });

    evb_.loop();
  }

  {
    // ReceiveLinkBandwidth: ACCEPT -> LbwExtCommunity:10Gbps
    adjRib_.reset();
    adjRibInQ_->open();
    ribInQ_.open();
    setupAdjRib(linkBandwidthBps, ReceiveLinkBandwidth::ACCEPT);

    fm_->addTask([&] {
      auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
      adjRibInQ_->fiberPush(std::move(update));

      // Verify rib In message
      auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      ASSERT_TRUE(std::holds_alternative<RibInAnnouncement>(msg));
      auto announcement = std::get<RibInAnnouncement>(msg);
      EXPECT_EQ(kPeerAddr1, announcement.peer.addr);
      EXPECT_EQ(linkBandwidthBps, announcement.peer.ucmpWeight);

      EXPECT_TRUE(announcement.attrs->hasNonTransitiveLbwExtCommunity());
      EXPECT_TRUE(announcement.attrs->getNonTransitiveLbwAsn().has_value());
      EXPECT_TRUE(announcement.attrs->getNonTransitiveLbwValue().has_value());
      EXPECT_EQ(announcement.attrs->getNonTransitiveLbwAsn().value(), 4369);
      EXPECT_EQ(announcement.attrs->getNonTransitiveLbwValue().value(), 1e10);

      // Enqueue messages for stop sequence
      terminateAdjRib();

      auto msgWithdrawal =
          facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      ASSERT_TRUE(std::holds_alternative<RibInWithdrawal>(msgWithdrawal));
    });

    evb_.loop();
  }

  {
    // ReceiveLinkBandwidth: SET_LINK_BPS -> LbwExtCommunity:200 bps
    adjRib_.reset();
    adjRibInQ_->open();
    ribInQ_.open();
    setupAdjRib(linkBandwidthBps, ReceiveLinkBandwidth::SET_LINK_BPS);

    fm_->addTask([&] {
      auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
      adjRibInQ_->fiberPush(std::move(update));

      // Verify rib In message
      auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      ASSERT_TRUE(std::holds_alternative<RibInAnnouncement>(msg));
      auto announcement = std::get<RibInAnnouncement>(msg);
      EXPECT_EQ(kPeerAddr1, announcement.peer.addr);
      EXPECT_EQ(linkBandwidthBps, announcement.peer.ucmpWeight);

      EXPECT_TRUE(announcement.attrs->hasNonTransitiveLbwExtCommunity());
      EXPECT_TRUE(announcement.attrs->getNonTransitiveLbwAsn().has_value());
      EXPECT_TRUE(announcement.attrs->getNonTransitiveLbwValue().has_value());
      EXPECT_EQ(
          announcement.attrs->getNonTransitiveLbwAsn().value(),
          static_cast<uint16_t>(kLocalAs1));
      EXPECT_EQ(announcement.attrs->getNonTransitiveLbwValue().value(), 200.0f);

      // Enqueue messages for stop sequence
      terminateAdjRib();

      auto msgWithdrawal =
          facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      ASSERT_TRUE(std::holds_alternative<RibInWithdrawal>(msgWithdrawal));
    });

    evb_.loop();
  }
  {
    adjRib_.reset();
    adjRibInQ_->open();
    ribInQ_.open();
    setupAdjRib(linkBandwidthBps, ReceiveLinkBandwidth::SET_LINK_BPS);

    fm_->addTask([&] {
      auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
      adjRibInQ_->fiberPush(std::move(update));

      auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      terminateAdjRib();

      auto msgWithdrawal =
          facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");

      // Cover the case that BgpPath is nullptr
      adjRib_->updateReceiveLbwExtCommunity(nullptr);

      // Invalid receive link bandwidth parameter
      auto inputUpdate =
          createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
      auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
          BgpPathFields(*BgpUpdate2toBgpPathC(*inputUpdate)));
      inputAttrs->publish();

      adjRib_->peeringParams_.receiveLinkBandwidth =
          static_cast<ReceiveLinkBandwidth>(10);
      EXPECT_DEATH(adjRib_->updateAttributesIn(inputAttrs), "");
    });
    evb_.loop();
  }
}

/**
 * Test Ingress UCMP Policy (Per Route UCMP)
 * both per-peer and per-route config has 3 possible values
 * (DISABLE, ENABLE, SET_LINK_BPS)
 * we test all 3 * 3 = 9 combinations
 */
struct TestParam {
  ReceiveLinkBandwidth recvLinkBandwidth;
  bgp_policy::LbwExtCommunityActionType lbwPolicyActionType;
  TestParam(
      ReceiveLinkBandwidth recvLinkBandwidth,
      bgp_policy::LbwExtCommunityActionType lbwPolicyActionType)
      : recvLinkBandwidth(recvLinkBandwidth),
        lbwPolicyActionType(lbwPolicyActionType) {}
};

class IngressUcmpPolicyFixture
    : public AdjRibInboundFixture,
      public ::testing::WithParamInterface<TestParam> {};

INSTANTIATE_TEST_CASE_P(
    IngressUcmpPolicyInstance,
    IngressUcmpPolicyFixture,
    ::testing::Values(
        TestParam(
            ReceiveLinkBandwidth::DISABLE,
            bgp_policy::LbwExtCommunityActionType::DISABLE),
        TestParam(
            ReceiveLinkBandwidth::DISABLE,
            bgp_policy::LbwExtCommunityActionType::ACCEPT),
        TestParam(
            ReceiveLinkBandwidth::DISABLE,
            bgp_policy::LbwExtCommunityActionType::SET_LINK_BPS),
        TestParam(
            ReceiveLinkBandwidth::ACCEPT,
            bgp_policy::LbwExtCommunityActionType::DISABLE),
        TestParam(
            ReceiveLinkBandwidth::ACCEPT,
            bgp_policy::LbwExtCommunityActionType::ACCEPT),
        TestParam(
            ReceiveLinkBandwidth::ACCEPT,
            bgp_policy::LbwExtCommunityActionType::SET_LINK_BPS),
        TestParam(
            ReceiveLinkBandwidth::SET_LINK_BPS,
            bgp_policy::LbwExtCommunityActionType::DISABLE),
        TestParam(
            ReceiveLinkBandwidth::SET_LINK_BPS,
            bgp_policy::LbwExtCommunityActionType::ACCEPT),
        TestParam(
            ReceiveLinkBandwidth::SET_LINK_BPS,
            bgp_policy::LbwExtCommunityActionType::SET_LINK_BPS)));

/**
 * Test IngressUcmpPolicy with following configs:
 * - prePolicyAttr asn-lbw:
 *     (4369, 1e10)
 * - peer-config asn-lbw:
 *     (kLocalAs1, kLbw5G)
 * two routes passed in: p1, p2
 * p1 (no ucmp policy)
 * p2 (has ucmp policy)
 * verify expected behaviors (policy takes precedence over peer-config)
 */
TEST_P(IngressUcmpPolicyFixture, IngressUcmpPolicy) {
  // create a single prefix match
  auto createPrefixMatch =
      [&](folly::CIDRNetwork prefix) -> bgp_policy::BgpPolicyAtomicMatch {
    routing_policy::CompareNumericValue compareStructEQ;
    compareStructEQ.compare_operator() = routing_policy::ComparisonOperator::EQ;
    compareStructEQ.value() = prefix.second;
    const auto& prefixListEntry = createPrefixListEntry(
        IPAddress::networkToString(prefix), {compareStructEQ});
    return createPrefixListMatch({prefixListEntry});
  };

  // verify link bandwidth per peer config
  auto verifyLbwPerPeerConfig = [&](const std::shared_ptr<const BgpPath> attrs,
                                    ReceiveLinkBandwidth recvLinkBandwidth) {
    switch (recvLinkBandwidth) {
      case ReceiveLinkBandwidth::DISABLE:
        EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
        EXPECT_FALSE(attrs->getNonTransitiveLbwAsn().has_value());
        EXPECT_FALSE(attrs->getNonTransitiveLbwValue().has_value());
        break;
      case ReceiveLinkBandwidth::ACCEPT:
        EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
        EXPECT_TRUE(attrs->getNonTransitiveLbwAsn().has_value());
        EXPECT_TRUE(attrs->getNonTransitiveLbwValue().has_value());
        EXPECT_EQ(attrs->getNonTransitiveLbwAsn().value(), 4369);
        EXPECT_EQ(attrs->getNonTransitiveLbwValue().value(), 1e10);
        break;
      case ReceiveLinkBandwidth::SET_LINK_BPS:
        EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
        EXPECT_TRUE(attrs->getNonTransitiveLbwAsn().has_value());
        EXPECT_TRUE(attrs->getNonTransitiveLbwValue().has_value());
        EXPECT_EQ(AsNum(attrs->getNonTransitiveLbwAsn().value()), kLocalAs1);
        EXPECT_EQ(attrs->getNonTransitiveLbwValue().value(), kLbw5G);
        break;
      default:
        EXPECT_FALSE(true);
    }
  };

  // verify link bandwidth per route config
  auto verifyLbwPerRouteConfig =
      [&](const std::shared_ptr<const BgpPath> attrs,
          bgp_policy::LbwExtCommunityActionType lbwPolicyActionType) {
        switch (lbwPolicyActionType) {
          case bgp_policy::LbwExtCommunityActionType::DISABLE:
            EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
            EXPECT_FALSE(attrs->getNonTransitiveLbwAsn().has_value());
            EXPECT_FALSE(attrs->getNonTransitiveLbwValue().has_value());
            break;
          case bgp_policy::LbwExtCommunityActionType::ACCEPT:
            EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
            EXPECT_TRUE(attrs->getNonTransitiveLbwAsn().has_value());
            EXPECT_TRUE(attrs->getNonTransitiveLbwValue().has_value());
            EXPECT_EQ(attrs->getNonTransitiveLbwAsn().value(), 4369);
            EXPECT_EQ(attrs->getNonTransitiveLbwValue().value(), 1e10);
            break;
          case bgp_policy::LbwExtCommunityActionType::SET_LINK_BPS:
            EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
            EXPECT_TRUE(attrs->getNonTransitiveLbwAsn().has_value());
            EXPECT_TRUE(attrs->getNonTransitiveLbwValue().has_value());
            EXPECT_EQ(
                AsNum(attrs->getNonTransitiveLbwAsn().value()), kLocalAs1);
            EXPECT_EQ(attrs->getNonTransitiveLbwValue().value(), kLbw5G);
            break;
          default:
            EXPECT_FALSE(true);
        }
      };

  /*
   * helper function to verify each test case, each test case takes two
   * prefixes: p1, p2, ONLY p2 will match UCMP policy. Veriy that p1 takes "per
   * peer config" while p2 takes "per route config"
   * - recvLinkBandwidth: per peer config
   * - lbwPolicyActionType: per route config (policy)
   */
  auto verify = [&](ReceiveLinkBandwidth recvLinkBandwidth,
                    bgp_policy::LbwExtCommunityActionType lbwPolicyActionType) {
    // create term1 without UCMP policy
    auto const match1 = createPrefixMatch(kV4Prefix1);
    auto actionEgp = createBgpPolicyAction(
        BgpPolicyActionType::ORIGIN, {}, "", bgp_policy::Origin::EGP);
    auto term1 =
        createBgpPolicyTerm("non-UCMP-term", "", {match1}, {actionEgp});

    // create term2 with UCMP policy
    auto const match2 = createPrefixMatch(kV4Prefix2);
    auto ucmpAction = createBgpPolicyLbwExtCommunityAction(lbwPolicyActionType);
    auto term2 = createBgpPolicyTerm("UCMP-term", "", {match2}, {ucmpAction});

    // create policy manager
    const std::string policyName = kIngressPolicyName;
    const auto& policyConfig = createBgpPolicies(policyName, {term1, term2});
    auto policyManager = std::make_shared<PolicyManager>(
        policyConfig, createTestBgpGlobalConfig());

    // set up AdjRib
    setupAdjRib(
        kLbw5G, // link bps
        recvLinkBandwidth,
        policyManager,
        policyName);

    fm_->addTask([&] {
      {
        auto update =
            createV4BgpUpdateMultipleAnnounce({kV4Prefix1, kV4Prefix2});
        adjRibInQ_->fiberPush(std::move(update));
      }

      // Verify rib In message, expect 2 msgs out, one for each prefix
      for (int i = 0; i < 2; ++i) {
        auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
        ASSERT_TRUE(std::holds_alternative<RibInAnnouncement>(msg));
        auto announcement = std::get<RibInAnnouncement>(msg);
        EXPECT_EQ(kPeerAddr1, announcement.peer.addr);
        EXPECT_EQ(announcement.pfxPathIds.size(), 1);
        if (get<0>(announcement.pfxPathIds.at(0)) == kV4Prefix1) {
          // prefix applied with per-peer-config
          verifyLbwPerPeerConfig(announcement.attrs, recvLinkBandwidth);
          XLOG(DBG1, "verified per-peer-config prefix");
        } else if (get<0>(announcement.pfxPathIds.at(0)) == kV4Prefix2) {
          // prefix applied with per-route-config
          verifyLbwPerRouteConfig(announcement.attrs, lbwPolicyActionType);
          XLOG(DBG1, "verified per-route-config prefix");
        } else {
          // unexpected prefix
          EXPECT_FALSE(true);
        }
      }

      // Enqueue messages for stop sequence
      terminateAdjRib();

      auto msgWithdrawal =
          facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      ASSERT_TRUE(std::holds_alternative<RibInWithdrawal>(msgWithdrawal));
    });

    evb_.loop();
  };

  const auto& param = GetParam();
  const auto& recvLinkBandwidth = param.recvLinkBandwidth;
  const auto& lbwPolicyActionType = param.lbwPolicyActionType;

  verify(recvLinkBandwidth, lbwPolicyActionType);
}

/*
 * test ucmp conifg/policy when route gets updated
 * - PeerConfig: DISABLE
 * - Policy: ACCEPT (takes precedence over PeerConfig)
 * time0: input: route (LBW 10G) -> output route (LBW 10G)
 * time1: input: route (LBW 20G) -> output route (LBW 20G)
 */
TEST_F(AdjRibInboundFixture, IngressUcmpPolicyRouteUpdate) {
  // create a single prefix match
  auto createPrefixMatch =
      [&](folly::CIDRNetwork prefix) -> bgp_policy::BgpPolicyAtomicMatch {
    routing_policy::CompareNumericValue compareStructEQ;
    compareStructEQ.compare_operator() = routing_policy::ComparisonOperator::EQ;
    compareStructEQ.value() = prefix.second;
    const auto& prefixListEntry = createPrefixListEntry(
        IPAddress::networkToString(prefix), {compareStructEQ});
    return createPrefixListMatch({prefixListEntry});
  };

  // create term with UCMP policy: ACCEPT
  auto const match = createPrefixMatch(kV4Prefix1);
  auto ucmpAction = createBgpPolicyLbwExtCommunityAction(
      bgp_policy::LbwExtCommunityActionType::ACCEPT);
  auto term = createBgpPolicyTerm("UCMP-term", "", {match}, {ucmpAction});

  // create policy manager
  const std::string policyName = kIngressPolicyName;
  const auto& policyConfig = createBgpPolicies(policyName, {term});
  auto policyManager = std::make_shared<PolicyManager>(
      policyConfig, createTestBgpGlobalConfig());

  // set up AdjRib Peer Config: DISABLE (will be overwritten by policy: ACCEPT)
  setupAdjRib(
      kLbw5G, // link bps
      ReceiveLinkBandwidth::DISABLE,
      policyManager,
      policyName);

  fm_->addTask([&] {
    // send route with 10G LBW
    {
      auto update = createV4BgpUpdateMultipleAnnounce(
          {kV4Prefix1},
          BgpAttrOrigin::BGP_ORIGIN_IGP,
          kExtCommLbwTypeSecondWord10G);
      adjRibInQ_->fiberPush(std::move(update));
    }

    {
      // Verify rib In message
      auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      ASSERT_TRUE(std::holds_alternative<RibInAnnouncement>(msg));
      auto announcement = std::get<RibInAnnouncement>(msg);
      EXPECT_EQ(kPeerAddr1, announcement.peer.addr);
      EXPECT_EQ(announcement.pfxPathIds.size(), 1);
      EXPECT_EQ(get<0>(announcement.pfxPathIds.at(0)), kV4Prefix1);

      const auto& attrs = announcement.attrs;
      EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
      EXPECT_TRUE(attrs->getNonTransitiveLbwAsn().has_value());
      EXPECT_TRUE(attrs->getNonTransitiveLbwValue().has_value());
      EXPECT_EQ(attrs->getNonTransitiveLbwAsn().value(), 4369);
      EXPECT_EQ(attrs->getNonTransitiveLbwValue().value(), 1e10); // 10G
    }

    // update route with 20G LBW
    {
      auto update = createV4BgpUpdateMultipleAnnounce(
          {kV4Prefix1},
          BgpAttrOrigin::BGP_ORIGIN_IGP,
          kExtCommLbwTypeSecondWord20G);
      adjRibInQ_->fiberPush(std::move(update));
    }

    {
      // Verify rib In message
      auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
      ASSERT_TRUE(std::holds_alternative<RibInAnnouncement>(msg));
      auto announcement = std::get<RibInAnnouncement>(msg);
      EXPECT_EQ(kPeerAddr1, announcement.peer.addr);
      EXPECT_EQ(announcement.pfxPathIds.size(), 1);
      EXPECT_EQ(get<0>(announcement.pfxPathIds.at(0)), kV4Prefix1);

      const auto& attrs = announcement.attrs;
      EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
      EXPECT_TRUE(attrs->getNonTransitiveLbwAsn().has_value());
      EXPECT_TRUE(attrs->getNonTransitiveLbwValue().has_value());
      EXPECT_EQ(attrs->getNonTransitiveLbwAsn().value(), 4369);
      EXPECT_EQ(attrs->getNonTransitiveLbwValue().value(), 2e10); // 20G
    }

    // Enqueue messages for stop sequence
    terminateAdjRib();

    auto msgWithdrawal =
        facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    ASSERT_TRUE(std::holds_alternative<RibInWithdrawal>(msgWithdrawal));
  });

  evb_.loop();
}

} // namespace facebook::bgp
