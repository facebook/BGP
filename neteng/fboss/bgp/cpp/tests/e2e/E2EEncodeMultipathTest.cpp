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

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

namespace facebook::bgp {

class E2EEncodeMultipathTest : public E2ETestFixture,
                               public ::testing::WithParamInterface<bool> {
 protected:
  static constexpr auto kGarIngressPolicyName = "accept-encoded-lbw";
  static constexpr auto kEncodePolicyName = "encode-multipath";
  static constexpr auto kBestPathPolicyName = "best-path-lbw";
  static constexpr auto kPrefix = "10.42.0.0";
  static constexpr auto kPrefixCidr = "10.42.0.0/24";
  static constexpr uint8_t kPrefixLength = 24;

  void SetUp() override {
    nsf_policy::NsfTeWeightEncoding encoding;
    encoding.fpf_l2_encoding() = nsf_policy::NsfFpfL2TeWeightEncoding();
    encoding.fpf_l2_encoding()->rack_id() = 8;
    encoding.fpf_l2_encoding()->spine_id() = 16;
    encoding.fpf_l2_encoding()->remote_rack_capacity() = 8;

    auto ingressTerm = createBgpPolicyTerm(
        "accept-encoded-lbw",
        "",
        {},
        {createBgpPolicyLbwExtCommunityAction(
            bgp_policy::LbwExtCommunityActionType::ACCEPT, encoding)});
    auto encodeTerm = createBgpPolicyTerm(
        "encode-multipath",
        "",
        {},
        {createBgpPolicyLbwExtCommunityAction(
            bgp_policy::LbwExtCommunityActionType::ENCODE_MULTIPATH,
            encoding,
            2 /* encodingId */)});
    auto bestPathTerm = createBgpPolicyTerm(
        "best-path-lbw",
        "",
        {},
        {createBgpPolicyLbwExtCommunityAction(
            bgp_policy::LbwExtCommunityActionType::BEST_PATH)});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(createBgpPolicyStatement(
        kGarIngressPolicyName, {std::move(ingressTerm)}));
    policies.bgp_policy_statements()->emplace_back(
        createBgpPolicyStatement(kEncodePolicyName, {std::move(encodeTerm)}));
    policies.bgp_policy_statements()->emplace_back(createBgpPolicyStatement(
        kBestPathPolicyName, {std::move(bestPathTerm)}));
    setPolicyConfig(policies);
    ingressPolicyName_ = kGarIngressPolicyName;

    auto source3 = kDefaultPeerSpec3;
    source3.disableIpv6Afi = true;
    auto source4 = kDefaultPeerSpec4;
    source4.disableIpv6Afi = true;
    auto encodedDestination = kDefaultPeerSpec5;
    encodedDestination.disableIpv6Afi = true;
    encodedDestination.egressPolicyName = kEncodePolicyName;
    BgpPeerSpec bestPathDestination{
        .asn = kPeerAsn6,
        .localAddr = kLocalAddr6,
        .peerAddr = kPeerAddr6,
        .v4Nexthop = kNextHopV4_6,
        .v6Nexthop = kEmptyV6Nexthop,
        .disableIpv6Afi = true,
        .egressPolicyName = kBestPathPolicyName,
    };

    addPeer(source3);
    addPeer(source4);
    addPeer(encodedDestination);
    addPeer(bestPathDestination);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/GetParam(),
        /*enableEgressBackpressure=*/true);
  }

  void bringUpPeersWithEor() {
    for (const auto& peerAddr :
         {kPeerAddr3, kPeerAddr4, kPeerAddr5, kPeerAddr6}) {
      bringUpPeer(peerAddr);
      BgpPeerId peerId{peerAddr, peerAddr.asV4().toLongHBO()};
      sendEoRToPeer(peerId);
      ASSERT_TRUE(waitForEoR(peerId));
    }
  }

  void expectEncodedCapacity(
      const BgpPeerId& peerId,
      const folly::CIDRNetwork& prefix,
      uint32_t expectedRawValue) {
    const auto updates = drainPeerQueueAndCollectUpdates(
        peerId, /*maxRetries=*/10, /*maxMessages=*/20);
    size_t matchingAnnouncements = 0;
    std::optional<uint32_t> rawLbw;
    for (const auto& update : updates) {
      if (!findPrefixInAnnouncements(
              *update, /*isV4=*/true, prefix, /*addPathId=*/0)) {
        continue;
      }
      matchingAnnouncements++;
      for (const auto& extCommunity : *update->attrs()->extCommunities()) {
        const auto firstWord = static_cast<uint32_t>(*extCommunity.firstWord());
        constexpr uint32_t kTypeAndSubtypeMask = 0xffff0000;
        constexpr uint32_t kNonTransitiveLbw = 0x40040000;
        if ((firstWord & kTypeAndSubtypeMask) == kNonTransitiveLbw) {
          rawLbw = static_cast<uint32_t>(*extCommunity.secondWord());
        }
      }
    }

    ASSERT_EQ(1, matchingAnnouncements);
    ASSERT_TRUE(rawLbw.has_value());
    EXPECT_EQ(expectedRawValue, *rawLbw);
  }

  static nettools::bgplib::BgpAttrExtCommunityC makeEncodedFpfLbw(
      uint32_t rawValue) {
    constexpr uint32_t kNonTransitiveLbwTypeAndSubtype = 0x40040000;
    constexpr uint16_t kEncodedLbwAsn = 65001;
    return nettools::bgplib::BgpAttrExtCommunityC(
        kNonTransitiveLbwTypeAndSubtype | kEncodedLbwAsn, rawValue);
  }

  void expectNoPrefixUpdate(
      const BgpPeerId& peerId,
      const folly::CIDRNetwork& prefix) {
    const auto occurrences = countPrefixOccurrencesAndDrain(
        peerId, prefix, /*isV4=*/true, /*maxRetries=*/10);
    EXPECT_EQ(0, occurrences.announceCount);
    EXPECT_EQ(0, occurrences.withdrawCount);
  }
};

INSTANTIATE_TEST_SUITE_P(
    UpdateGroupModes,
    E2EEncodeMultipathTest,
    ::testing::Values(false, true),
    [](const testing::TestParamInfo<bool>& info) {
      return std::string(info.param ? "Enabled" : "Disabled");
    });

TEST_P(
    E2EEncodeMultipathTest,
    MultipathSizeChangeUpdatesEncodePolicyButSuppressesBestPathPolicy) {
  bringUpPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork(kPrefixCidr);
  const BgpPeerId encodedPeerId{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  const BgpPeerId bestPathPeerId{kPeerAddr6, kPeerAddr6.asV4().toLongHBO()};

  /*
   * Both paths carry valid FPF topology. Their raw values decode as floats
   * with sufficiently different magnitudes that adding the second path does
   * not change the float aggregate used by normal best-path tracking.
   */
  addRouteWithExtCommunities(
      "v4",
      kPrefix,
      kPrefixLength,
      kPeerAddr3,
      "11.0.0.1",
      "65001",
      {makeEncodedFpfLbw(0x20000001)});
  ASSERT_TRUE(waitForPathCountInRib(kPrefixCidr, 1));
  auto bestpath = getBestPath(prefix);
  ASSERT_NE(nullptr, bestpath);
  EXPECT_EQ(kPeerAddr3, bestpath->peer.addr);
  ASSERT_TRUE(bestpath->attrs->getNonTransitiveRawLbwValue().has_value());
  EXPECT_EQ(0x20000001, *bestpath->attrs->getNonTransitiveRawLbwValue());

  expectEncodedCapacity(encodedPeerId, prefix, 0x01000001);
  const auto initialBestPath = countPrefixOccurrencesAndDrain(
      bestPathPeerId, prefix, /*isV4=*/true, /*maxRetries=*/10);
  EXPECT_EQ(1, initialBestPath.announceCount);

  addRouteWithExtCommunities(
      "v4",
      kPrefix,
      kPrefixLength,
      kPeerAddr4,
      "11.0.0.2",
      "65001",
      {makeEncodedFpfLbw(0x01000002)});
  ASSERT_TRUE(waitForPathCountInRib(kPrefixCidr, 2));
  ASSERT_EQ(bestpath, getBestPath(prefix));

  expectEncodedCapacity(encodedPeerId, prefix, 0x02000001);
  expectNoPrefixUpdate(bestPathPeerId, prefix);

  deleteRoute("v4", kPrefix, kPrefixLength, kPeerAddr4);
  WITH_RETRIES_N(
      50, { EXPECT_EVENTUALLY_EQ(getMultipathNexthopCount(kPrefixCidr), 1); });
  ASSERT_EQ(bestpath, getBestPath(prefix));

  expectEncodedCapacity(encodedPeerId, prefix, 0x01000001);
  expectNoPrefixUpdate(bestPathPeerId, prefix);
}

} // namespace facebook::bgp
