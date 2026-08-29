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
 * E2E tests for Extended Communities attribute propagation.
 *
 * Tests the behavior of AdvertiseLinkBandwidth (sender/egress) and
 * ReceiveLinkBandwidth (receiver/ingress) knobs for both EBGP and IBGP peers.
 *
 * Test route is injected with 4 extended communities:
 *   RT-T:   Route Target, transitive     (type=0x00, subtype=0x02)
 *   RO-NT:  Route Origin, non-transitive  (type=0x40, subtype=0x03)
 *   LBW-T:  Link Bandwidth, transitive    (type=0x00, subtype=0x04)
 *   LBW-NT: Link Bandwidth, non-transitive (type=0x40, subtype=0x04)
 *
 * Key behaviors validated:
 *   EBGP egress: strips non-transitive ext communities per RFC 4360,
 *     except non-transitive LBW when AdvertiseLinkBandwidth is configured.
 *   IBGP egress: preserves all ext communities (no non-transitive stripping).
 *   AdvertiseLinkBandwidth: prunes transitive LBW, then applies DISABLE/
 *     BEST_PATH logic to non-transitive LBW.
 *   ReceiveLinkBandwidth: prunes or accepts non-transitive LBW at ingress.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>
#include <neteng/fboss/bgp/cpp/lib/BgpStructs.h>

#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;
using facebook::nettools::bgplib::BgpAttrExtCommunityC;
using facebook::nettools::bgplib::BgpExtCommunityBaseTypeC;

namespace facebook::bgp {

// Type bytes from BgpStructs.h
static constexpr uint8_t kTransitive =
    BgpExtCommunityBaseTypeC::kBgpExtCommASTransitiveType;
static constexpr uint8_t kNonTransitive =
    BgpExtCommunityBaseTypeC::kBgpExtCommASNonTransitiveType;

// Subtype bytes from BgpStructs.h
static constexpr uint8_t kRouteTargetSubtype =
    static_cast<uint8_t>(BgpAttrExtCommunityC::BGP_EXT_COMMUNITY_SUBTYPES::
                             ROUTE_TARGET_COMMUNITY_SUBTYPE);
static constexpr uint8_t kRouteOriginSubtype =
    static_cast<uint8_t>(BgpAttrExtCommunityC::BGP_EXT_COMMUNITY_SUBTYPES::
                             ROUTE_ORIGIN_COMMUNITY_SUBTYPE);
static constexpr uint8_t kLinkBwSubtype =
    static_cast<uint8_t>(BgpAttrExtCommunityC::BGP_EXT_COMMUNITY_SUBTYPES::
                             LINK_BW_COMMUNITY_SUBTYPE);

// Test ASN and values for ext communities
static constexpr uint16_t kExtCommAsn = 65001;
static constexpr uint32_t kRouteTargetValue = 100;
static constexpr uint32_t kRouteOriginValue = 200;
// 10.0f as IEEE 754 uint32 — represents 10 bytes/sec LBW (value is arbitrary
// for propagation tests; only presence/absence matters)
static constexpr uint32_t kLbwValue = 0x41200000;

// Helper to construct a BgpAttrExtCommunityC from type/subtype/asn/value
static BgpAttrExtCommunityC
makeExtComm(uint8_t type, uint8_t subType, uint16_t asn, uint32_t value) {
  return BgpAttrExtCommunityC(
      (uint32_t(type) << 24) | (uint32_t(subType) << 16) | asn, value);
}

/*
 * Enum identifying which extended communities we expect in an update.
 * Used as flags for concise verification.
 */
enum ExtCommFlag : uint8_t {
  RT_T = 1 << 0, // Route Target, transitive
  RO_NT = 1 << 1, // Route Origin, non-transitive
  LBW_T = 1 << 2, // Link Bandwidth, transitive
  LBW_NT = 1 << 3, // Link Bandwidth, non-transitive
};

/*
 * Base fixture for Extended Communities propagation tests.
 * Derives from E2ETestFixture directly (not E2ERibTestFixture) because each
 * test needs different peer configurations.
 */
class ExtCommunitiesPropagationTest : public E2ETestFixture {
 protected:
  /*
   * Build the standard set of 4 test extended communities.
   */
  static std::vector<BgpAttrExtCommunityC> makeAllExtCommunities() {
    return {
        makeExtComm(
            kTransitive, kRouteTargetSubtype, kExtCommAsn, kRouteTargetValue),
        makeExtComm(
            kNonTransitive,
            kRouteOriginSubtype,
            kExtCommAsn,
            kRouteOriginValue),
        makeExtComm(kTransitive, kLinkBwSubtype, kExtCommAsn, kLbwValue),
        makeExtComm(kNonTransitive, kLinkBwSubtype, kExtCommAsn, kLbwValue),
    };
  }

  /*
   * Classify a BgpAttrExtCommunity into one of our test flags.
   * Returns 0 if the community doesn't match any of the 4 test communities.
   */
  static uint8_t classifyExtCommunity(
      const nettools::bgplib::BgpAttrExtCommunity& ec) {
    BgpAttrExtCommunityC ecC(*ec.firstWord(), *ec.secondWord());
    if (ecC.isRouteTarget() && ecC.isTransitive()) {
      return RT_T;
    }
    if (ecC.isLinkBandwidthCommunity() && ecC.isTransitive()) {
      return LBW_T;
    }
    if (ecC.isNonTransitiveLinkBandwidthCommunity()) {
      return LBW_NT;
    }
    // Any remaining non-transitive community is our RO_NT test community.
    // Note: isRouteOrigin() only recognizes transitive types (type <= 0x02),
    // so we can't use it for our non-transitive Route Origin (type 0x40).
    if (!ecC.isTransitive()) {
      return RO_NT;
    }
    return 0;
  }

  /*
   * Verify that an outbound update to destPeer contains exactly the expected
   * extended communities (specified as OR'd ExtCommFlag values).
   *
   * Also verifies the route announcement is present for the given prefix.
   */
  void verifyOutboundExtCommunities(
      const folly::IPAddress& destPeerAddr,
      const std::string& prefix,
      uint8_t prefixLen,
      uint8_t expectedFlags) {
    BgpPeerId destPeerId{destPeerAddr, destPeerAddr.asV4().toLongHBO()};

    auto updateOpt = waitForOutboundUpdate(destPeerId, 50);
    ASSERT_TRUE(updateOpt.has_value())
        << "No outbound update for peer " << destPeerAddr.str();

    const auto& update = **updateOpt;

    // Verify the prefix is in the announcement
    const folly::CIDRNetwork expectedCidr{folly::IPAddress(prefix), prefixLen};
    ASSERT_TRUE(
        findPrefixInAnnouncements(update, true /* isV4 */, expectedCidr, 0))
        << "Prefix " << prefix << "/" << (int)prefixLen
        << " not found in announcement";

    // Classify all ext communities in the update.
    // Note: do NOT use .has_value() on extCommunities() — it's a non-optional
    // Thrift list field, and has_value() tracks "was explicitly assigned", not
    // "has elements". getBgpUpdate2() populates via emplace_back which doesn't
    // set the is_set flag.
    uint8_t actualFlags = 0;
    for (const auto& ec : *update.attrs()->extCommunities()) {
      actualFlags |= classifyExtCommunity(ec);
    }

    // Build human-readable flag strings for error messages
    auto flagStr = [](uint8_t flags) -> std::string {
      std::string s;
      if (flags & RT_T) {
        s += "RT-T ";
      }
      if (flags & RO_NT) {
        s += "RO-NT ";
      }
      if (flags & LBW_T) {
        s += "LBW-T ";
      }
      if (flags & LBW_NT) {
        s += "LBW-NT ";
      }
      return s.empty() ? "(none)" : s;
    };

    EXPECT_EQ(actualFlags, expectedFlags)
        << "Ext community mismatch: expected [" << flagStr(expectedFlags)
        << "] got [" << flagStr(actualFlags) << "]";
  }

  /*
   * Common setup: add peers, create RIB + PeerManagerBase, bring up peers.
   * sourcePeer injects routes. destPeer receives them.
   */
  void setupPeers(const BgpPeerSpec& sourceSpec, const BgpPeerSpec& destSpec) {
    addPeer(sourceSpec);
    addPeer(destSpec);
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/true);
    bringUpPeer(sourceSpec.peerAddr);
    bringUpPeer(destSpec.peerAddr);

    BgpPeerId srcId{
        sourceSpec.peerAddr, sourceSpec.peerAddr.asV4().toLongHBO()};
    BgpPeerId dstId{destSpec.peerAddr, destSpec.peerAddr.asV4().toLongHBO()};
    sendEoRToPeer(srcId);
    sendEoRToPeer(dstId);
    ASSERT_TRUE(waitForEoR(srcId));
    ASSERT_TRUE(waitForEoR(dstId));
  }

  /*
   * Inject the test route with all 4 ext communities from the source peer,
   * and wait for it to appear in the shadow RIB.
   */
  void injectTestRoute(const folly::IPAddress& sourcePeerAddr) {
    addRouteWithExtCommunities(
        "v4",
        kTestPrefix,
        kTestPrefixLen,
        sourcePeerAddr,
        kTestNexthop,
        kTestAsPath,
        makeAllExtCommunities());

    auto prefix = folly::IPAddress::createNetwork(
        fmt::format("{}/{}", kTestPrefix, kTestPrefixLen));
    ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  }

  // EBGP source peer spec (different ASN from local kAsn1=4200000001)
  BgpPeerSpec makeEbgpSourceSpec(
      std::optional<ReceiveLinkBandwidth> rcvLbw = std::nullopt) {
    return BgpPeerSpec{
        .asn = kPeerAsn3,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr3,
        .v4Nexthop = kNextHopV4_3,
        .v6Nexthop = kNextHopV6_3,
        .receiveLinkBandwidth = rcvLbw,
    };
  }

  // EBGP destination peer spec
  BgpPeerSpec makeEbgpDestSpec(
      std::optional<AdvertiseLinkBandwidth> advLbw = std::nullopt) {
    return BgpPeerSpec{
        .asn = kPeerAsn5,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
        .advertiseLinkBandwidth = advLbw,
    };
  }

  // IBGP destination peer spec (same ASN as local = kAsn1)
  BgpPeerSpec makeIbgpDestSpec(
      std::optional<AdvertiseLinkBandwidth> advLbw = std::nullopt) {
    return BgpPeerSpec{
        .asn = kAsn1, // Same as local ASN → IBGP
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
        .advertiseLinkBandwidth = advLbw,
    };
  }

  // IBGP source peer spec (same ASN as local = kAsn1)
  BgpPeerSpec makeIbgpSourceSpec(
      std::optional<ReceiveLinkBandwidth> rcvLbw = std::nullopt) {
    return BgpPeerSpec{
        .asn = kAsn1,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr3,
        .v4Nexthop = kNextHopV4_3,
        .v6Nexthop = kNextHopV6_3,
        .receiveLinkBandwidth = rcvLbw,
    };
  }

  static constexpr const char* kTestPrefix = "10.0.0.0";
  static constexpr uint8_t kTestPrefixLen = 8;
  static constexpr const char* kTestNexthop = "11.0.0.1";
  static constexpr const char* kTestAsPath = "65001";
};

// ==========================================================================
// SENDER-SIDE TESTS (AdvertiseLinkBandwidth on egress)
// ==========================================================================

/*
 * EBGP Sender UNCONFIGURED (AdvertiseLinkBandwidth = nullopt)
 *
 * EBGP egress strips all non-transitive ext communities.
 * AdvertiseLinkBandwidth not configured → no LBW-specific processing,
 * keepNonTransitiveLbw=false → standard RFC 4360 stripping.
 *
 * Expected post-policy: RT-T, LBW-T
 *   - RT-T: transitive, passes through
 *   - RO-NT: non-transitive, stripped by EBGP
 *   - LBW-T: transitive, passes through
 *   - LBW-NT: non-transitive, stripped by EBGP
 */
TEST_F(ExtCommunitiesPropagationTest, SenderEbgp_Unconfigured) {
  setupPeers(makeEbgpSourceSpec(), makeEbgpDestSpec(std::nullopt));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | LBW_T);
}

/*
 * EBGP Sender DISABLE (AdvertiseLinkBandwidth = DISABLE)
 *
 * AdvertiseLinkBandwidth::DISABLE:
 *   1. pruneTransitiveLbwExtCommunity() removes LBW-T
 *   2. pruneNonTransitiveLbwExtCommunity() removes LBW-NT
 * EBGP egress: keepNonTransitiveLbw=true (AdvLbw configured), but no LBW
 *   communities remain. RO-NT is still stripped (not LBW).
 *
 * Expected post-policy: RT-T
 */
TEST_F(ExtCommunitiesPropagationTest, SenderEbgp_Disable) {
  setupPeers(
      makeEbgpSourceSpec(), makeEbgpDestSpec(AdvertiseLinkBandwidth::DISABLE));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T);
}

/*
 * EBGP Sender ADVERTISE (AdvertiseLinkBandwidth = BEST_PATH)
 *
 * AdvertiseLinkBandwidth::BEST_PATH:
 *   1. pruneTransitiveLbwExtCommunity() removes LBW-T
 *   2. Single path has LBW → aggregateReceivedUcmpWeight is set → keep LBW-NT
 * EBGP egress: keepNonTransitiveLbw=true (AdvLbw configured) → keeps LBW-NT.
 *   RO-NT stripped (non-transitive, not LBW).
 *
 * Expected post-policy: RT-T, LBW-NT
 */
TEST_F(ExtCommunitiesPropagationTest, SenderEbgp_Advertise) {
  setupPeers(
      makeEbgpSourceSpec(),
      makeEbgpDestSpec(AdvertiseLinkBandwidth::BEST_PATH));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | LBW_NT);
}

/*
 * IBGP Sender UNCONFIGURED (AdvertiseLinkBandwidth = nullopt)
 *
 * IBGP egress preserves all ext communities (no non-transitive stripping).
 * AdvertiseLinkBandwidth not configured → no LBW-specific processing.
 *
 * Expected post-policy: RT-T, RO-NT, LBW-T, LBW-NT
 */
TEST_F(ExtCommunitiesPropagationTest, SenderIbgp_Unconfigured) {
  // Source must be EBGP so the route propagates to IBGP destination
  // (IBGP→IBGP is filtered without route reflector)
  setupPeers(makeEbgpSourceSpec(), makeIbgpDestSpec(std::nullopt));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | RO_NT | LBW_T | LBW_NT);
}

/*
 * IBGP Sender DISABLE (AdvertiseLinkBandwidth = DISABLE)
 *
 * AdvertiseLinkBandwidth::DISABLE removes both LBW-T and LBW-NT.
 * IBGP egress preserves remaining non-transitive communities (RO-NT).
 *
 * Expected post-policy: RT-T, RO-NT
 */
TEST_F(ExtCommunitiesPropagationTest, SenderIbgp_Disable) {
  setupPeers(
      makeEbgpSourceSpec(), makeIbgpDestSpec(AdvertiseLinkBandwidth::DISABLE));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | RO_NT);
}

/*
 * IBGP Sender ADVERTISE (AdvertiseLinkBandwidth = BEST_PATH)
 *
 * AdvertiseLinkBandwidth::BEST_PATH: prunes LBW-T, keeps LBW-NT.
 * IBGP egress preserves all non-transitive communities.
 *
 * Expected post-policy: RT-T, RO-NT, LBW-NT
 */
TEST_F(ExtCommunitiesPropagationTest, SenderIbgp_Advertise) {
  setupPeers(
      makeEbgpSourceSpec(),
      makeIbgpDestSpec(AdvertiseLinkBandwidth::BEST_PATH));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | RO_NT | LBW_NT);
}

// ==========================================================================
// RECEIVER-SIDE TESTS (ReceiveLinkBandwidth on ingress)
// ==========================================================================
//
// For receiver tests, the source peer has ReceiveLinkBandwidth configured.
//
// EBGP receiver: EBGP source (with rcvLbw) + EBGP dest (no AdvLbw).
//   EBGP egress strips non-transitive per RFC 4360.
//
// IBGP receiver: IBGP source (with rcvLbw) + EBGP dest (BEST_PATH AdvLbw).
//   IBGP→IBGP is filtered without route reflector, so dest must be EBGP.
//   Dest uses BEST_PATH so that the ingress rcvLbw effect on LBW-NT is
//   observable at egress (without AdvLbw, EBGP egress strips all
//   non-transitive communities, masking the ingress effect).

/*
 * EBGP Receiver UNCONFIGURED (ReceiveLinkBandwidth = nullopt)
 *
 * No ingress LBW processing (no-op). All ext communities pass through.
 * EBGP destination (no AdvLbw) → strips non-transitive per RFC 4360.
 *
 * Expected: RT-T, LBW-T
 */
TEST_F(ExtCommunitiesPropagationTest, ReceiverEbgp_Unconfigured) {
  setupPeers(makeEbgpSourceSpec(std::nullopt), makeEbgpDestSpec(std::nullopt));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | LBW_T);
}

/*
 * EBGP Receiver DISABLE (ReceiveLinkBandwidth = DISABLE)
 *
 * Ingress prunes non-transitive LBW (LBW-NT removed).
 * Remaining: RT-T, RO-NT, LBW-T.
 * EBGP destination (no AdvLbw) → strips non-transitive → RT-T, LBW-T.
 *
 * Expected: RT-T, LBW-T
 */
TEST_F(ExtCommunitiesPropagationTest, ReceiverEbgp_Disable) {
  setupPeers(
      makeEbgpSourceSpec(ReceiveLinkBandwidth::DISABLE),
      makeEbgpDestSpec(std::nullopt));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | LBW_T);
}

/*
 * EBGP Receiver ACCEPT (ReceiveLinkBandwidth = ACCEPT)
 *
 * Ingress accepts all LBW as-is (no-op for LBW).
 * All 4 ext communities pass through ingress.
 * EBGP destination (no AdvLbw) → strips non-transitive → RT-T, LBW-T.
 *
 * Expected: RT-T, LBW-T
 */
TEST_F(ExtCommunitiesPropagationTest, ReceiverEbgp_Accept) {
  setupPeers(
      makeEbgpSourceSpec(ReceiveLinkBandwidth::ACCEPT),
      makeEbgpDestSpec(std::nullopt));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | LBW_T);
}

/*
 * IBGP Receiver UNCONFIGURED (ReceiveLinkBandwidth = nullopt)
 *
 * No ingress LBW processing. All 4 ext communities pass through ingress.
 * EBGP destination (BEST_PATH):
 *   - pruneTransitiveLbwExtCommunity removes LBW-T
 *   - keepNonTransitiveLbw=true → preserves LBW-NT
 *   - Strips non-transitive non-LBW (RO-NT)
 *
 * Expected: RT-T, LBW-NT
 */
TEST_F(ExtCommunitiesPropagationTest, ReceiverIbgp_Unconfigured) {
  setupPeers(
      makeIbgpSourceSpec(std::nullopt),
      makeEbgpDestSpec(AdvertiseLinkBandwidth::BEST_PATH));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | LBW_NT);
}

/*
 * IBGP Receiver DISABLE (ReceiveLinkBandwidth = DISABLE)
 *
 * Ingress prunes non-transitive LBW (LBW-NT removed).
 * Remaining after ingress: RT-T, RO-NT, LBW-T.
 * EBGP destination (BEST_PATH):
 *   - pruneTransitiveLbwExtCommunity removes LBW-T
 *   - keepNonTransitiveLbw=true, but no LBW-NT remains
 *   - Strips RO-NT (non-transitive, not LBW)
 *
 * Expected: RT-T
 */
TEST_F(ExtCommunitiesPropagationTest, ReceiverIbgp_Disable) {
  setupPeers(
      makeIbgpSourceSpec(ReceiveLinkBandwidth::DISABLE),
      makeEbgpDestSpec(AdvertiseLinkBandwidth::BEST_PATH));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T);
}

/*
 * IBGP Receiver ACCEPT (ReceiveLinkBandwidth = ACCEPT)
 *
 * Ingress accepts all LBW as-is. All 4 ext communities pass through ingress.
 * EBGP destination (BEST_PATH):
 *   - pruneTransitiveLbwExtCommunity removes LBW-T
 *   - keepNonTransitiveLbw=true → preserves LBW-NT
 *   - Strips RO-NT (non-transitive, not LBW)
 *
 * Expected: RT-T, LBW-NT
 */
TEST_F(ExtCommunitiesPropagationTest, ReceiverIbgp_Accept) {
  setupPeers(
      makeIbgpSourceSpec(ReceiveLinkBandwidth::ACCEPT),
      makeEbgpDestSpec(AdvertiseLinkBandwidth::BEST_PATH));
  injectTestRoute(kPeerAddr3);
  verifyOutboundExtCommunities(
      kPeerAddr5, kTestPrefix, kTestPrefixLen, RT_T | LBW_NT);
}

} // namespace facebook::bgp
