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
 * E2E tests for RFC 1997 well-known community egress filtering.
 *
 * Behavior matrix exercised:
 *
 *   NO_ADVERTISE        -> suppressed to ANY peer (IBGP, EBGP, ConfedEBGP)
 *   NO_EXPORT           -> suppressed to EBGP only
 *                          (allowed to IBGP and ConfedEBGP)
 *   NO_EXPORT_SUBCONFED -> suppressed to EBGP and ConfedEBGP
 *                          (allowed only to IBGP within local sub-AS)
 *
 * Each test:
 *   1. Sets up an EBGP source peer and a single destination peer of the
 *      target session type.
 *   2. Opts the fixture into the bgp_rfc1997_well_known_communities
 *      feature flag (default OFF in production).
 *   3. Injects a route from the source, optionally carrying a well-known
 *      community.
 *   4. Verifies the route either reaches the destination peer's outbound
 *      queue or is suppressed there, depending on RFC 1997 rules.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real:   RIB, PeerManagerBase, AdjRib / AdjRibOutGroup
 */

#include <gtest/gtest.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/logging/xlog.h>

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/adjrib/WellKnownCommunityFilter.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

namespace facebook::bgp {

namespace {

/*
 * Parent confederation AS used whenever a test enrolls a ConfedEBGP peer.
 * Must differ from kAsn1 (the fixture's local sub-AS) and from any peer
 * ASN we plug in as a confed peer.
 */
constexpr uint32_t kConfedParentAsn = 65000;

constexpr const char* kTestPrefix = "10.0.0.0";
constexpr uint8_t kTestPrefixLen = 8;
constexpr const char* kTestNexthop = "11.0.0.1";
constexpr const char* kTestAsPath = "65001";

/*
 * Helper to read the current value of an ODS counter.
 *
 * publishStats() flushes thread-local increments into the global
 * ServiceData store. ThreadCachedServiceData buffers incrementCounter
 * calls per thread and relies on a background publisher to flush; the
 * publisher does not run reliably in test processes, so explicit flush
 * is required before each read or recently-recorded increments are
 * invisible to getCounter.
 */
int64_t readCounter(folly::StringPiece name) {
  facebook::fb303::ThreadCachedServiceData::get()->publishStats();
  return facebook::fb303::ThreadCachedServiceData::get()->getCounter(
      std::string(name) + ".count");
}

} // namespace

class E2EWellKnownCommunitiesTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    /*
     * Enable the gflag for ALL tests in this suite. Saved + restored in
     * TearDown so it does not leak into other suites in the same binary.
     */
    prevFlagValue_ = FLAGS_enable_well_known_community_filter;
    FLAGS_enable_well_known_community_filter = true;
    /*
     * Pre-initialize the bgpd.well_known_community.* counters to 0 so the
     * Counters_* tests can read a baseline before any suppression event.
     * Production initializes these via BgpStats::initCounters() at peer
     * manager startup, which the E2E fixture does not invoke.
     */
    BgpStats::initWellKnownCommunityStats();
  }

  void TearDown() override {
    FLAGS_enable_well_known_community_filter = prevFlagValue_;
    E2ETestFixture::TearDown();
  }

  bool prevFlagValue_{false};

  /* EBGP source peer: peer.asn != kAsn1, isConfedPeer=false. */
  static BgpPeerSpec makeEbgpSourceSpec() {
    return BgpPeerSpec{
        .asn = kPeerAsn3,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr3,
        .v4Nexthop = kNextHopV4_3,
        .v6Nexthop = kNextHopV6_3,
        .description = kDescription1,
    };
  }

  /* EBGP destination peer: peer.asn != kAsn1, isConfedPeer=false. */
  static BgpPeerSpec makeEbgpDestSpec() {
    return BgpPeerSpec{
        .asn = kPeerAsn5,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
    };
  }

  /* IBGP destination peer: peer.asn == kAsn1 (same as local). */
  static BgpPeerSpec makeIbgpDestSpec() {
    return BgpPeerSpec{
        .asn = kAsn1,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
    };
  }

  /*
   * IBGP RR-client destination peer: peer.asn == kAsn1 (IBGP) and
   * isRrClient=true. The RR path skips the IBGP split-horizon early
   * return in canAnnounce / canAnnounceForGroup so RFC 1997 suppression
   * is observable end-to-end even when the route was originally learned
   * from another IBGP peer in plain IBGP (it isn't here — source is
   * EBGP — but the RR-aware path is the one we want to exercise to
   * confirm the filter triggers there too).
   */
  static BgpPeerSpec makeIbgpRrClientDestSpec() {
    return BgpPeerSpec{
        .asn = kAsn1,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
        .isRrClient = true,
    };
  }

  /*
   * ConfedEBGP destination peer: isConfedPeer=true, peer.asn differs from
   * the parent confederation AS so the session classifies as ConfedEBGP.
   */
  static BgpPeerSpec makeConfedEbgpDestSpec() {
    return BgpPeerSpec{
        .asn = kPeerAsn5,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
        .isConfedPeer = true,
        .localConfedAsn = kConfedParentAsn,
    };
  }

  /*
   * Bring up a source EBGP peer and a destination peer of the given
   * session type. enableUpdateGroup controls which egress path is
   * exercised (per-peer AdjRib vs AdjRibOutGroup).
   */
  void setupTopology(
      const BgpPeerSpec& sourceSpec,
      const BgpPeerSpec& destSpec,
      bool enableUpdateGroup = false) {
    addPeer(sourceSpec);
    addPeer(destSpec);
    createRib();
    createPeerManager(
        enableUpdateGroup,
        /*enableEgressBackpressure=*/true);
    bringUpPeer(sourceSpec.peerAddr);
    bringUpPeer(destSpec.peerAddr);

    BgpPeerId srcId{
        sourceSpec.peerAddr, sourceSpec.peerAddr.asV4().toLongHBO()};
    BgpPeerId dstId{destSpec.peerAddr, destSpec.peerAddr.asV4().toLongHBO()};
    sendEoRToPeer(srcId);
    sendEoRToPeer(dstId);
    /*
     * Drain both v4 and v6 EoRs per peer (dual-stack peers emit two EoRs).
     * Leaving the second EoR in the egress queue would cause downstream
     * isPeerEgressQueueEmpty / waitForOutboundUpdate calls to observe a
     * stale message and either misclassify or block.
     */
    ASSERT_TRUE(waitForEoR(srcId));
    ASSERT_TRUE(waitForEoR(srcId));
    ASSERT_TRUE(waitForEoR(dstId));
    ASSERT_TRUE(waitForEoR(dstId));
  }

  /*
   * Inject the test prefix carrying the given communities (space-separated
   * tokens parsed by E2ETestFixture::addRoute -> parseCommunities ->
   * BgpAttrCommunityC::createBgpAttrCommunity). Confirms it reaches the
   * shadow RIB before returning.
   */
  void injectRouteWithCommunity(
      const folly::IPAddress& sourceAddr,
      const std::string& community) {
    addRoute(
        "v4",
        kTestPrefix,
        kTestPrefixLen,
        sourceAddr,
        kTestNexthop,
        kTestAsPath,
        community);

    const auto prefix = folly::IPAddress::createNetwork(
        fmt::format("{}/{}", kTestPrefix, kTestPrefixLen));
    ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  }

  /* Expect the test prefix to arrive at destPeer (route allowed). */
  void expectDelivered(const folly::IPAddress& destAddr) {
    BgpPeerId destId{destAddr, destAddr.asV4().toLongHBO()};
    auto updateOpt = waitForOutboundUpdate(destId, 50);
    ASSERT_TRUE(updateOpt.has_value())
        << "Expected route to be delivered to " << destAddr.str()
        << " but no outbound update arrived";
    const folly::CIDRNetwork expectedCidr{
        folly::IPAddress(kTestPrefix), kTestPrefixLen};
    EXPECT_TRUE(
        findPrefixInAnnouncements(**updateOpt, /*isV4=*/true, expectedCidr, 0))
        << "Prefix not found in delivered announcement";
  }

  /*
   * Expect the test prefix NOT to arrive at destPeer (route suppressed).
   *
   * setupTopology has already drained the destination peer's egress EoR
   * via waitForEoR, so the queue is empty before route injection. We
   * reuse waitForOutboundUpdate as the bounded wait primitive — it pumps
   * the event loop, retries up to N times, and returns nullopt only when
   * nothing was enqueued for the whole window. That is exactly the
   * negative assertion we need, and it keeps the polling cadence inside
   * the framework helper so this test contains no direct sleep_for.
   */
  void expectSuppressed(const folly::IPAddress& destAddr) {
    BgpPeerId destId{destAddr, destAddr.asV4().toLongHBO()};
    auto updateOpt = waitForOutboundUpdate(destId, /*maxRetries=*/20);
    if (updateOpt.has_value()) {
      ADD_FAILURE() << "Prefix " << kTestPrefix << "/" << int(kTestPrefixLen)
                    << " enqueued for delivery to " << destAddr.str()
                    << " but RFC 1997 well-known community suppression "
                    << "should have prevented any announcement";
    }
  }
};

/* ==========================================================================
 * NO_ADVERTISE: suppressed to ANY peer type.
 * ========================================================================== */

TEST_F(E2EWellKnownCommunitiesTest, NoAdvertise_ToEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-advertise");
  expectSuppressed(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, NoAdvertise_ToIbgp) {
  setupTopology(makeEbgpSourceSpec(), makeIbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-advertise");
  expectSuppressed(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, NoAdvertise_ToConfedEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeConfedEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-advertise");
  expectSuppressed(kPeerAddr5);
}

/*
 * RR-client IBGP destination: plain IBGP can be incidentally blocked by
 * split-horizon (the egress path may early-return before consulting the
 * well-known community filter), which would mask filter regressions in
 * that code path. RR clients are explicitly allowed to receive routes
 * learned from other IBGP peers, so the RR path runs the filter without
 * the split-horizon short-circuit — making this the cleanest assertion
 * that NO_ADVERTISE is honored on the IBGP egress branch.
 */
TEST_F(E2EWellKnownCommunitiesTest, NoAdvertise_ToIbgpRrClient) {
  setupTopology(makeEbgpSourceSpec(), makeIbgpRrClientDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-advertise");
  expectSuppressed(kPeerAddr5);
}

/* ==========================================================================
 * NO_EXPORT: suppressed to EBGP only; allowed to IBGP and ConfedEBGP.
 * ========================================================================== */

TEST_F(E2EWellKnownCommunitiesTest, NoExport_ToEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-export");
  expectSuppressed(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, NoExport_ToIbgp) {
  setupTopology(makeEbgpSourceSpec(), makeIbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-export");
  expectDelivered(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, NoExport_ToConfedEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeConfedEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-export");
  expectDelivered(kPeerAddr5);
}

/* ==========================================================================
 * NO_EXPORT_SUBCONFED: suppressed to EBGP and ConfedEBGP; allowed to IBGP.
 * ========================================================================== */

TEST_F(E2EWellKnownCommunitiesTest, NoExportSubconfed_ToEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-export-subconfed");
  expectSuppressed(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, NoExportSubconfed_ToIbgp) {
  setupTopology(makeEbgpSourceSpec(), makeIbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-export-subconfed");
  expectDelivered(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, NoExportSubconfed_ToConfedEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeConfedEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-export-subconfed");
  expectSuppressed(kPeerAddr5);
}

/* ==========================================================================
 * Baseline: a route carrying no well-known community is delivered to all
 * destination peer types (sanity check that the filter only suppresses on
 * the well-known community values, not on every route).
 * ========================================================================== */

TEST_F(E2EWellKnownCommunitiesTest, NoCommunity_DeliveredToEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, /*community=*/"");
  expectDelivered(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, NoCommunity_DeliveredToIbgp) {
  setupTopology(makeEbgpSourceSpec(), makeIbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, /*community=*/"");
  expectDelivered(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, NoCommunity_DeliveredToConfedEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeConfedEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, /*community=*/"");
  expectDelivered(kPeerAddr5);
}

/* ==========================================================================
 * Mixed communities: NO_EXPORT together with a user-defined community.
 * Verifies the well-known community still triggers suppression, but for
 * destinations where the route IS allowed (IBGP) the user-defined
 * community survives intact.
 * ========================================================================== */

TEST_F(E2EWellKnownCommunitiesTest, MixedCommunities_SuppressedAtEbgp) {
  setupTopology(makeEbgpSourceSpec(), makeEbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-export 65001:42");
  expectSuppressed(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, MixedCommunities_DeliveredToIbgpKeepsUser) {
  setupTopology(makeEbgpSourceSpec(), makeIbgpDestSpec());
  injectRouteWithCommunity(kPeerAddr3, "no-export 65001:42");

  BgpPeerId destId{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  auto updateOpt = waitForOutboundUpdate(destId, 50);
  ASSERT_TRUE(updateOpt.has_value())
      << "Expected route to be delivered to IBGP peer";

  const folly::CIDRNetwork expectedCidr{
      folly::IPAddress(kTestPrefix), kTestPrefixLen};
  ASSERT_TRUE(
      findPrefixInAnnouncements(**updateOpt, /*isV4=*/true, expectedCidr, 0));

  /*
   * Verify the user-defined community (65001:42 = asn=65001, value=42) is
   * still present on the announcement.
   */
  bool foundUserCommunity = false;
  for (const auto& comm : (*updateOpt)->attrs()->communities().value()) {
    if (comm.asn().value() == 65001 && comm.value().value() == 42) {
      foundUserCommunity = true;
      break;
    }
  }
  EXPECT_TRUE(foundUserCommunity)
      << "User-defined community 65001:42 lost when delivering to IBGP";
}

/* ==========================================================================
 * Update-group path: enableUpdateGroup=true routes the canAnnounce check
 * through AdjRibOutGroup::canAnnounceForGroup rather than the per-peer
 * AdjRib::canAnnounce. The same RFC 1997 suppression rules must apply.
 * ========================================================================== */

TEST_F(E2EWellKnownCommunitiesTest, UpdateGroup_NoExportSuppressedAtEbgp) {
  setupTopology(
      makeEbgpSourceSpec(),
      makeEbgpDestSpec(),
      /*enableUpdateGroup=*/true);
  injectRouteWithCommunity(kPeerAddr3, "no-export");
  expectSuppressed(kPeerAddr5);
}

TEST_F(E2EWellKnownCommunitiesTest, UpdateGroup_NoAdvertiseSuppressedAtIbgp) {
  setupTopology(
      makeEbgpSourceSpec(),
      makeIbgpDestSpec(),
      /*enableUpdateGroup=*/true);
  injectRouteWithCommunity(kPeerAddr3, "no-advertise");
  expectSuppressed(kPeerAddr5);
}

/* ==========================================================================
 * Counter assertion: confirm the matching ODS counter increments for each
 * suppression type. Reads the bgpd.well_known_community.* counters before
 * and after one suppression event apiece.
 * ========================================================================== */

TEST_F(E2EWellKnownCommunitiesTest, Counters_NoAdvertiseIncrements) {
  setupTopology(makeEbgpSourceSpec(), makeEbgpDestSpec());

  const auto before =
      readCounter(BgpStats::kWellKnownCommunityNoAdvertiseSuppressed);
  injectRouteWithCommunity(kPeerAddr3, "no-advertise");
  expectSuppressed(kPeerAddr5);

  WITH_RETRIES({
    auto after =
        readCounter(BgpStats::kWellKnownCommunityNoAdvertiseSuppressed);
    EXPECT_EVENTUALLY_GE(after, before + 1)
        << "bgpd.well_known_community.no_advertise_suppressed did not increment";
  });
}

TEST_F(E2EWellKnownCommunitiesTest, Counters_NoExportIncrements) {
  setupTopology(makeEbgpSourceSpec(), makeEbgpDestSpec());

  const auto before =
      readCounter(BgpStats::kWellKnownCommunityNoExportSuppressed);
  injectRouteWithCommunity(kPeerAddr3, "no-export");
  expectSuppressed(kPeerAddr5);

  WITH_RETRIES({
    auto after = readCounter(BgpStats::kWellKnownCommunityNoExportSuppressed);
    EXPECT_EVENTUALLY_GE(after, before + 1)
        << "bgpd.well_known_community.no_export_suppressed did not increment";
  });
}

TEST_F(E2EWellKnownCommunitiesTest, Counters_NoExportSubconfedIncrements) {
  setupTopology(makeEbgpSourceSpec(), makeConfedEbgpDestSpec());

  const auto before =
      readCounter(BgpStats::kWellKnownCommunityNoExportSubconfedSuppressed);
  injectRouteWithCommunity(kPeerAddr3, "no-export-subconfed");
  expectSuppressed(kPeerAddr5);

  WITH_RETRIES({
    auto after =
        readCounter(BgpStats::kWellKnownCommunityNoExportSubconfedSuppressed);
    EXPECT_EVENTUALLY_GE(after, before + 1)
        << "bgpd.well_known_community.no_export_subconfed_suppressed "
        << "did not increment";
  });
}

/* ==========================================================================
 * Feature flag default OFF: when the flag is not set, suppression does
 * NOT happen even if a well-known community is present. Confirms the
 * gate is wired correctly so production rollout is bisectable.
 * ========================================================================== */

class E2EWellKnownCommunitiesFlagOffTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    /*
     * Force the gflag OFF in case a prior test in the binary left it on.
     * Save + restore so the cleanup also rolls back any modification we
     * made during this suite.
     */
    prevFlagValue_ = FLAGS_enable_well_known_community_filter;
    FLAGS_enable_well_known_community_filter = false;
  }

  void TearDown() override {
    FLAGS_enable_well_known_community_filter = prevFlagValue_;
    E2ETestFixture::TearDown();
  }

  bool prevFlagValue_{false};

  /*
   * Bring up an EBGP src->dest pair, inject a route carrying the given
   * well-known community, and assert it is delivered to the destination.
   * Used by the three Delivered* tests below so each well-known community
   * (NO_ADVERTISE / NO_EXPORT / NO_EXPORT_SUBCONFED) is independently
   * verified against the OFF gate.
   */
  void verifyDeliveredWhenFlagOff(const std::string& community) {
    addPeer(
        BgpPeerSpec{
            .asn = kPeerAsn3,
            .localAddr = kLocalAddr1,
            .peerAddr = kPeerAddr3,
            .v4Nexthop = kNextHopV4_3,
            .v6Nexthop = kNextHopV6_3,
            .description = kDescription1,
        });
    addPeer(
        BgpPeerSpec{
            .asn = kPeerAsn5,
            .localAddr = kLocalAddr5,
            .peerAddr = kPeerAddr5,
            .v4Nexthop = kNextHopV4_5,
            .v6Nexthop = kNextHopV6_5,
        });
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/true);
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr5);
    BgpPeerId srcId{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId dstId{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
    sendEoRToPeer(srcId);
    sendEoRToPeer(dstId);
    /*
     * Drain both v4 and v6 EoRs per peer (dual-stack peers emit two EoRs).
     * Mirrors the pattern in E2EWellKnownCommunitiesTest::setupTopology so a
     * stale second EoR doesn't get popped by the downstream
     * waitForOutboundUpdate call and cause the test to either misclassify or
     * block indefinitely.
     */
    ASSERT_TRUE(waitForEoR(srcId));
    ASSERT_TRUE(waitForEoR(srcId));
    ASSERT_TRUE(waitForEoR(dstId));
    ASSERT_TRUE(waitForEoR(dstId));

    addRoute(
        "v4",
        kTestPrefix,
        kTestPrefixLen,
        kPeerAddr3,
        kTestNexthop,
        kTestAsPath,
        community);
    const auto prefix = folly::IPAddress::createNetwork(
        fmt::format("{}/{}", kTestPrefix, kTestPrefixLen));
    ASSERT_TRUE(waitForRouteInShadowRib(prefix));

    auto updateOpt = waitForOutboundUpdate(dstId, 50);
    ASSERT_TRUE(updateOpt.has_value())
        << "Route with " << community
        << " should still be delivered when feature flag is off "
        << "(legacy behavior)";
    const folly::CIDRNetwork expectedCidr{
        folly::IPAddress(kTestPrefix), kTestPrefixLen};
    EXPECT_TRUE(
        findPrefixInAnnouncements(**updateOpt, /*isV4=*/true, expectedCidr, 0));
  }
};

TEST_F(E2EWellKnownCommunitiesFlagOffTest, NoAdvertise_DeliveredWhenFlagOff) {
  verifyDeliveredWhenFlagOff("no-advertise");
}

TEST_F(E2EWellKnownCommunitiesFlagOffTest, NoExport_DeliveredWhenFlagOff) {
  verifyDeliveredWhenFlagOff("no-export");
}

TEST_F(
    E2EWellKnownCommunitiesFlagOffTest,
    NoExportSubconfed_DeliveredWhenFlagOff) {
  verifyDeliveredWhenFlagOff("no-export-subconfed");
}

} // namespace facebook::bgp
