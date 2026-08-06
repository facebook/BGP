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
 * Unit tests for the pure filter predicate
 * `shouldSuppressByWellKnownCommunity`. Covers the full RFC 1997 behavior
 * matrix (3 communities x 3 session types), empty/null inputs, the
 * NO_ADVERTISE-wins-over-NO_EXPORT priority rule, and the ASN-filter
 * shortcut for non-well-known reserved-ASN communities. Counter-dispatch
 * coverage is folded in via `incrementSuppressionStat`.
 *
 * E2E coverage exists in
 * fbcode/neteng/fboss/bgp/cpp/tests/e2e/E2EWellKnownCommunitiesTest.cpp,
 * but the E2E path relies on the AdjRib + PeerManagerBase fixtures and only
 * exercises the filter via canAnnounceEntry / canAnnounceForGroup. These
 * unit tests verify the predicate in isolation so production semantics
 * remain verifiable even when the fixture has gaps.
 */

#include <gtest/gtest.h>

#include <fb303/ThreadCachedServiceData.h>

#include "neteng/fboss/bgp/cpp/adjrib/WellKnownCommunityFilter.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Types.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"

namespace facebook::bgp {

namespace {

constexpr uint16_t kReservedAsn = 0xFFFF;
constexpr uint16_t kNoExport = 0xFF01;
constexpr uint16_t kNoAdvertise = 0xFF02;
constexpr uint16_t kNoExportSubconfed = 0xFF03;
constexpr uint16_t kUserAsn = 65001;
constexpr uint16_t kUserValue = 42;

std::shared_ptr<BgpPath> makePath(
    const std::vector<nettools::bgplib::BgpAttrCommunityC>& communities) {
  auto path = std::make_shared<BgpPath>(BgpPathFields());
  nettools::bgplib::BgpAttrCommunitiesC comms;
  for (const auto& c : communities) {
    comms.push_back(c);
  }
  path->setCommunities(std::move(comms));
  return path;
}

int64_t readCounter(folly::StringPiece name) {
  fb303::ThreadCachedServiceData::get()->publishStats();
  return fb303::ThreadCachedServiceData::get()->getCounter(
      std::string(name) + ".count");
}

} // namespace

// ===========================================================================
// 3x3 matrix: 3 communities x 3 session types.
// ===========================================================================

TEST(WellKnownCommunityFilterTest, NoAdvertise_SuppressesEbgp) {
  auto path = makePath({{kReservedAsn, kNoAdvertise}});
  auto result = shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(RFC1997Community::NoAdvertise, *result);
}

TEST(WellKnownCommunityFilterTest, NoAdvertise_SuppressesIbgp) {
  auto path = makePath({{kReservedAsn, kNoAdvertise}});
  auto result = shouldSuppressByWellKnownCommunity(path, BgpSessionType::IBGP);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(RFC1997Community::NoAdvertise, *result);
}

TEST(WellKnownCommunityFilterTest, NoAdvertise_SuppressesConfedEbgp) {
  auto path = makePath({{kReservedAsn, kNoAdvertise}});
  auto result =
      shouldSuppressByWellKnownCommunity(path, BgpSessionType::ConfedEBGP);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(RFC1997Community::NoAdvertise, *result);
}

TEST(WellKnownCommunityFilterTest, NoExport_SuppressesEbgp) {
  auto path = makePath({{kReservedAsn, kNoExport}});
  auto result = shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(RFC1997Community::NoExport, *result);
}

TEST(WellKnownCommunityFilterTest, NoExport_AllowedIbgp) {
  auto path = makePath({{kReservedAsn, kNoExport}});
  auto result = shouldSuppressByWellKnownCommunity(path, BgpSessionType::IBGP);
  EXPECT_FALSE(result.has_value());
}

TEST(WellKnownCommunityFilterTest, NoExport_AllowedConfedEbgp) {
  auto path = makePath({{kReservedAsn, kNoExport}});
  auto result =
      shouldSuppressByWellKnownCommunity(path, BgpSessionType::ConfedEBGP);
  EXPECT_FALSE(result.has_value());
}

TEST(WellKnownCommunityFilterTest, NoExportSubconfed_SuppressesEbgp) {
  auto path = makePath({{kReservedAsn, kNoExportSubconfed}});
  auto result = shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(RFC1997Community::NoExportSubconfed, *result);
}

TEST(WellKnownCommunityFilterTest, NoExportSubconfed_AllowedIbgp) {
  auto path = makePath({{kReservedAsn, kNoExportSubconfed}});
  auto result = shouldSuppressByWellKnownCommunity(path, BgpSessionType::IBGP);
  EXPECT_FALSE(result.has_value());
}

TEST(WellKnownCommunityFilterTest, NoExportSubconfed_SuppressesConfedEbgp) {
  auto path = makePath({{kReservedAsn, kNoExportSubconfed}});
  auto result =
      shouldSuppressByWellKnownCommunity(path, BgpSessionType::ConfedEBGP);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(RFC1997Community::NoExportSubconfed, *result);
}

// ===========================================================================
// Negative / boundary cases.
// ===========================================================================

TEST(WellKnownCommunityFilterTest, EmptyCommunitiesAllowed) {
  auto path = makePath({});
  EXPECT_FALSE(shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP)
                   .has_value());
  EXPECT_FALSE(shouldSuppressByWellKnownCommunity(path, BgpSessionType::IBGP)
                   .has_value());
  EXPECT_FALSE(
      shouldSuppressByWellKnownCommunity(path, BgpSessionType::ConfedEBGP)
          .has_value());
}

TEST(WellKnownCommunityFilterTest, NullPathAllowed) {
  /*
   * Predicate is noexcept and must tolerate a null shared_ptr (logged as
   * DCHECK in dev builds, returned nullopt in opt). Skips dev/asan to
   * avoid the DCHECK firing.
   */
#if !defined(NDEBUG)
  GTEST_SKIP() << "DCHECK fires on null path in debug builds";
#else
  std::shared_ptr<const BgpPath> path;
  EXPECT_FALSE(shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP)
                   .has_value());
#endif
}

TEST(WellKnownCommunityFilterTest, UserCommunityOnlyAllowed) {
  /*
   * A community whose asn is not 0xFFFF (reserved) must never trigger
   * RFC 1997 suppression even if the value happens to overlap with
   * 0xFF01/02/03.
   */
  auto path = makePath({{kUserAsn, kNoExport}});
  EXPECT_FALSE(shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP)
                   .has_value());
}

TEST(WellKnownCommunityFilterTest, ReservedAsnNonWellKnownValueAllowed) {
  /*
   * Reserved ASN (0xFFFF) but value outside the well-known range. RFC
   * 1997 reserves the asn=0xFFFF / value=0x0000-0xFFFF range; values
   * outside 0xFF01-0xFF03 are unassigned and must NOT suppress.
   */
  auto path = makePath({{kReservedAsn, /*value=*/0xFF99}});
  EXPECT_FALSE(shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP)
                   .has_value());
}

// ===========================================================================
// Priority and combination cases.
// ===========================================================================

TEST(WellKnownCommunityFilterTest, NoAdvertisePriorityOverNoExport) {
  /*
   * A route carrying both NO_ADVERTISE and NO_EXPORT must return
   * NoAdvertise regardless of community vector ordering — the filter must
   * not return NoExport just because it appears first in the iteration.
   */
  auto pathNoExportFirst =
      makePath({{kReservedAsn, kNoExport}, {kReservedAsn, kNoAdvertise}});
  auto result1 = shouldSuppressByWellKnownCommunity(
      pathNoExportFirst, BgpSessionType::EBGP);
  ASSERT_TRUE(result1.has_value());
  EXPECT_EQ(RFC1997Community::NoAdvertise, *result1);

  auto pathNoAdvertiseFirst =
      makePath({{kReservedAsn, kNoAdvertise}, {kReservedAsn, kNoExport}});
  auto result2 = shouldSuppressByWellKnownCommunity(
      pathNoAdvertiseFirst, BgpSessionType::EBGP);
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQ(RFC1997Community::NoAdvertise, *result2);
}

TEST(
    WellKnownCommunityFilterTest,
    NoAdvertisePriorityOnIbgpEvenIfNoExportAllowed) {
  /*
   * On IBGP, NO_EXPORT alone is allowed but NO_ADVERTISE suppresses.
   * A path carrying both must still suppress.
   */
  auto path =
      makePath({{kReservedAsn, kNoExport}, {kReservedAsn, kNoAdvertise}});
  auto result = shouldSuppressByWellKnownCommunity(path, BgpSessionType::IBGP);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(RFC1997Community::NoAdvertise, *result);
}

TEST(WellKnownCommunityFilterTest, NoExportSubconfedWithNoExport) {
  /*
   * Both NO_EXPORT and NO_EXPORT_SUBCONFED set, no NO_ADVERTISE.
   *   EBGP -> either community would suppress; predicate returns
   *           NO_EXPORT_SUBCONFED because the post-loop check for
   *           NO_EXPORT_SUBCONFED is evaluated first.
   *   IBGP -> neither applies, allowed.
   *   ConfedEBGP -> NO_EXPORT_SUBCONFED applies, suppresses.
   */
  auto path =
      makePath({{kReservedAsn, kNoExport}, {kReservedAsn, kNoExportSubconfed}});

  auto resultEbgp =
      shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP);
  ASSERT_TRUE(resultEbgp.has_value());
  EXPECT_EQ(RFC1997Community::NoExportSubconfed, *resultEbgp);

  EXPECT_FALSE(shouldSuppressByWellKnownCommunity(path, BgpSessionType::IBGP)
                   .has_value());

  auto resultConfed =
      shouldSuppressByWellKnownCommunity(path, BgpSessionType::ConfedEBGP);
  ASSERT_TRUE(resultConfed.has_value());
  EXPECT_EQ(RFC1997Community::NoExportSubconfed, *resultConfed);
}

TEST(WellKnownCommunityFilterTest, WellKnownPlusUserCommunity) {
  /*
   * Mixing a well-known community with a user-defined one must not change
   * the suppression decision. Verifies the filter only inspects the
   * reserved-ASN entries and ignores everything else.
   */
  auto path = makePath({{kUserAsn, kUserValue}, {kReservedAsn, kNoExport}});
  auto result = shouldSuppressByWellKnownCommunity(path, BgpSessionType::EBGP);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(RFC1997Community::NoExport, *result);
}

// ===========================================================================
// Counter dispatch (incrementSuppressionStat). Verifies the helper picks
// the matching bgpd.well_known_community.* counter for each community.
// ===========================================================================

TEST(WellKnownCommunityFilterTest, IncrementSuppressionStat_NoAdvertise) {
  BgpStats::initWellKnownCommunityStats();
  const auto before =
      readCounter(BgpStats::kWellKnownCommunityNoAdvertiseSuppressed);
  incrementSuppressionStat(RFC1997Community::NoAdvertise);
  EXPECT_EQ(
      before + 1,
      readCounter(BgpStats::kWellKnownCommunityNoAdvertiseSuppressed));
}

TEST(WellKnownCommunityFilterTest, IncrementSuppressionStat_NoExport) {
  BgpStats::initWellKnownCommunityStats();
  const auto before =
      readCounter(BgpStats::kWellKnownCommunityNoExportSuppressed);
  incrementSuppressionStat(RFC1997Community::NoExport);
  EXPECT_EQ(
      before + 1, readCounter(BgpStats::kWellKnownCommunityNoExportSuppressed));
}

TEST(WellKnownCommunityFilterTest, IncrementSuppressionStat_NoExportSubconfed) {
  BgpStats::initWellKnownCommunityStats();
  const auto before =
      readCounter(BgpStats::kWellKnownCommunityNoExportSubconfedSuppressed);
  incrementSuppressionStat(RFC1997Community::NoExportSubconfed);
  EXPECT_EQ(
      before + 1,
      readCounter(BgpStats::kWellKnownCommunityNoExportSubconfedSuppressed));
}

/*
 * Guard against the prior bug where DEFINE_timeseries omitted the key
 * argument and counters were published under the bare symbol name
 * (without the "bgpd.well_known_community." prefix). After incrementing
 * via the public API, the bare-name counters must remain absent.
 */
TEST(WellKnownCommunityFilterTest, BareSymbolCountersAreNotPublished) {
  BgpStats::initWellKnownCommunityStats();
  const auto beforeNoAdv =
      readCounter(BgpStats::kWellKnownCommunityNoAdvertiseSuppressed);
  const auto beforeNoExp =
      readCounter(BgpStats::kWellKnownCommunityNoExportSuppressed);
  const auto beforeNoExpSub =
      readCounter(BgpStats::kWellKnownCommunityNoExportSubconfedSuppressed);
  incrementSuppressionStat(RFC1997Community::NoAdvertise);
  incrementSuppressionStat(RFC1997Community::NoExport);
  incrementSuppressionStat(RFC1997Community::NoExportSubconfed);
  fb303::ThreadCachedServiceData::get()->publishStats();
  auto counters = fb303::ThreadCachedServiceData::getShared();

  // The correct prefixed counters must be incremented.
  EXPECT_EQ(
      beforeNoAdv + 1,
      readCounter(BgpStats::kWellKnownCommunityNoAdvertiseSuppressed));
  EXPECT_EQ(
      beforeNoExp + 1,
      readCounter(BgpStats::kWellKnownCommunityNoExportSuppressed));
  EXPECT_EQ(
      beforeNoExpSub + 1,
      readCounter(BgpStats::kWellKnownCommunityNoExportSubconfedSuppressed));

  // Bare-name counters (without prefix) must remain absent.
  EXPECT_FALSE(counters->hasCounter(
      "well_known_community_no_advertise_suppressed.count"));
  EXPECT_FALSE(counters->hasCounter(
      "well_known_community_no_advertise_suppressed.count.60"));
  EXPECT_FALSE(
      counters->hasCounter("well_known_community_no_export_suppressed.count"));
  EXPECT_FALSE(counters->hasCounter(
      "well_known_community_no_export_suppressed.count.60"));
  EXPECT_FALSE(counters->hasCounter(
      "well_known_community_no_export_subconfed_suppressed.count"));
  EXPECT_FALSE(counters->hasCounter(
      "well_known_community_no_export_subconfed_suppressed.count.60"));
}

// ===========================================================================
// toString round-trips for the enum names used in operator-facing logs.
// ===========================================================================

TEST(WellKnownCommunityFilterTest, ToStringMatchesEnumValues) {
  EXPECT_STREQ("NO_ADVERTISE", toString(RFC1997Community::NoAdvertise));
  EXPECT_STREQ("NO_EXPORT", toString(RFC1997Community::NoExport));
  EXPECT_STREQ(
      "NO_EXPORT_SUBCONFED", toString(RFC1997Community::NoExportSubconfed));
}

} // namespace facebook::bgp
