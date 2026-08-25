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

#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalRibEncoder.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

using nettools::bgplib::DeDuplicatedAsPath;
using nettools::bgplib::DeDuplicatedBgpAttributesC;
using nettools::bgplib::DeDuplicatedBgpPath;
using nettools::bgplib::DeDuplicatedClusterList;
using nettools::bgplib::DeDuplicatedCommunities;
using nettools::bgplib::DeDuplicatedExtCommunities;

namespace {
constexpr int64_t kRibVersion = 7;

folly::CIDRNetwork prefix(const std::string& cidr) {
  return folly::IPAddress::createNetwork(cidr);
}
} // namespace

class CanonicalRibEncoderTest : public ::testing::Test {
 public:
  void SetUp() override {
    clearAllDeduplicators();
  }
  void TearDown() override {
    clearAllDeduplicators();
  }

  static void clearAllDeduplicators() {
    DeDuplicatedBgpPath::clearDeduplicator();
    DeDuplicatedBgpAttributesC::clearDeduplicator();
    DeDuplicatedAsPath::clearDeduplicator();
    DeDuplicatedCommunities::clearDeduplicator();
    DeDuplicatedExtCommunities::clearDeduplicator();
    DeDuplicatedClusterList::clearDeduplicator();
  }

  /*
   * Evict deduplicator entries that no longer have an external owner, then
   * sweep the encoder. This mirrors production, where PeerManager's periodic
   * deduplicator eviction destroys withdrawn paths/sub-attrs and the encoder
   * reclaims their slots on the next publish. Without an external strong ref,
   * a path/sub-attr is destroyed here and its encoder slot becomes reclaimable.
   */
  void evictAndSweep() {
    /*
     * Evict outer-to-inner: destroying the BgpPath releases its BgpAttributesC
     * bundle, whose eviction in turn releases the list-valued sub-attributes.
     */
    DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();
    DeDuplicatedBgpAttributesC::evictDeletedEntriesFromDeduplicator();
    DeDuplicatedAsPath::evictDeletedEntriesFromDeduplicator();
    DeDuplicatedCommunities::evictDeletedEntriesFromDeduplicator();
    DeDuplicatedExtCommunities::evictDeletedEntriesFromDeduplicator();
    DeDuplicatedClusterList::evictDeletedEntriesFromDeduplicator();
    encoder_.markReclamationPending();
    now_ += std::chrono::minutes(3);
    encoder_.consumeDirtyAndSweep(now_);
  }

  /*
   * Build a deduplicated BgpPath. Two calls with identical (asCount, commCount,
   * nexthop) return the same shared_ptr (whole-path dedup); a differing asCount
   * yields a distinct path that still shares the community list when commCount
   * matches.
   */
  std::shared_ptr<const BgpPath> makePath(
      uint32_t asCount,
      uint32_t commCount,
      const folly::IPAddress& nexthop) {
    auto path = std::make_shared<BgpPath>(
        *buildBgpPathFields(asCount, commCount, 0, 0, 0, nexthop));
    return DeDuplicatedBgpPath(path).getSharedPtr();
  }

  /* A deduplicated path with an empty AS_PATH (e.g. locally-originated). */
  std::shared_ptr<const BgpPath> makePathNoAsPath(
      const folly::IPAddress& nexthop) {
    auto path =
        std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0, 0, nexthop));
    path->setAsPath(nettools::bgplib::BgpAttrAsPathC{});
    return DeDuplicatedBgpPath(path).getSharedPtr();
  }

  CanonicalPathInput bestPath(std::shared_ptr<const BgpPath> path) {
    /*
     * Field-by-field (not brace-init) so the trailing peer/operational fields
     * keep their defaults without tripping -Wmissing-field-initializers.
     */
    CanonicalPathInput in;
    in.path = std::move(path);
    in.group = kBestPathGroup;
    in.isBestPath = true;
    return in;
  }

  CanonicalRibEncoder::TimePoint now_{};
  CanonicalRibEncoder encoder_{now_};
};

/*
 * buildEntry of a single best path returns a canonical entry whose inline
 * next_hop and dict indices resolve back to the input attributes.
 */
TEST_F(CanonicalRibEncoderTest, SingleBestPath_ResolvesToInputAttributes) {
  auto nexthop = folly::IPAddress("10.0.0.1");
  auto entry = encoder_.buildEntry(
      prefix("2401:db00::/32"),
      kRibVersion,
      {bestPath(makePath(/*asCount=*/3, /*commCount=*/2, nexthop))},
      /*exportMultipaths=*/true);

  EXPECT_EQ(createTIpPrefix(prefix("2401:db00::/32")), entry.prefix().value());
  EXPECT_EQ(kRibVersion, entry.rib_version().value());

  const auto& groups = entry.paths().value();
  ASSERT_EQ(1, groups.count(std::string(kBestPathGroup)));
  const auto& best = groups.at(std::string(kBestPathGroup));
  ASSERT_EQ(1, best.size());
  EXPECT_TRUE(best[0].is_best_path().value());

  /*
   * Resolve the published value: next_hop is inline, the community list comes
   * through the dict by index.
   */
  const auto dict = encoder_.dictSnapshot();
  const auto pool = encoder_.pathAttrsSnapshot();
  const auto& attrs = pool.at(best[0].path_idx().value());
  EXPECT_EQ(createTIpPrefix(nexthop), attrs.next_hop().value());
  ASSERT_TRUE(attrs.communities_idx().has_value());
  EXPECT_EQ(
      2,
      dict.community_lists()
          .value()
          .at(attrs.communities_idx().value())
          .size());

  /*
   * The top-level best_path is the same self-contained TBgpDedupedPath,
   * decodable through the dict without the deduped_paths pool.
   */
  ASSERT_TRUE(entry.best_path().has_value());
  EXPECT_EQ(attrs, entry.best_path().value());
}

// A path with an empty AS_PATH carries no as_path dict index.
TEST_F(CanonicalRibEncoderTest, EmptyAsPath_NoDictIndex) {
  auto entry = encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(makePathNoAsPath(folly::IPAddress("10.0.0.1")))},
      /*exportMultipaths=*/true);

  const auto pool = encoder_.pathAttrsSnapshot();
  const auto& attrs = pool.at(entry.paths()
                                  .value()
                                  .at(std::string(kBestPathGroup))[0]
                                  .path_idx()
                                  .value());
  EXPECT_FALSE(attrs.as_path_idx().has_value());
  EXPECT_EQ(0, encoder_.dictSnapshot().as_path_lists().value().size());
}

// Two prefixes advertising the identical (deduplicated) path share one slot.
TEST_F(CanonicalRibEncoderTest, IdenticalPath_SharesOnePoolSlot) {
  auto nexthop = folly::IPAddress("10.0.0.1");
  auto a = encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(makePath(3, 2, nexthop))},
      /*exportMultipaths=*/true);
  auto b = encoder_.buildEntry(
      prefix("10.2.0.0/24"),
      kRibVersion,
      {bestPath(makePath(3, 2, nexthop))},
      /*exportMultipaths=*/true);

  EXPECT_EQ(1, encoder_.livePathAttrsCount());
  EXPECT_EQ(
      a.paths().value().at(std::string(kBestPathGroup))[0].path_idx().value(),
      b.paths().value().at(std::string(kBestPathGroup))[0].path_idx().value());
}

/*
 * Distinct whole paths sharing a sub-attribute (community list) intern it once
 * in the dict (two-level deduplication).
 */
TEST_F(CanonicalRibEncoderTest, SharedSubAttr_InternedOnceInDict) {
  // Same commCount (=> identical community list), different asCount + nexthop.
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(makePath(2, 3, folly::IPAddress("10.0.0.1")))},
      /*exportMultipaths=*/true);
  encoder_.buildEntry(
      prefix("10.2.0.0/24"),
      kRibVersion,
      {bestPath(makePath(4, 3, folly::IPAddress("10.0.0.2")))},
      /*exportMultipaths=*/true);

  const auto dict = encoder_.dictSnapshot();
  EXPECT_EQ(2, encoder_.livePathAttrsCount());
  EXPECT_EQ(1, dict.community_lists().value().size()); // shared
  EXPECT_EQ(2, dict.as_path_lists().value().size()); // distinct
}

/*
 * A path whose object is destroyed (no remaining owner) is reclaimed by the
 * lazy sweep; while an external owner remains, the slot is retained.
 */
TEST_F(CanonicalRibEncoderTest, LazySweep_ReclaimsDestroyedPath) {
  auto p = makePath(2, 3, folly::IPAddress("10.0.0.1")); // external owner
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(p)},
      /*exportMultipaths=*/true);
  ASSERT_EQ(1, encoder_.livePathAttrsCount());

  // Still owned (p) -> sweep retains the slot.
  evictAndSweep();
  EXPECT_EQ(1, encoder_.livePathAttrsCount());
  EXPECT_FALSE(encoder_.reclamationPending());

  // Drop the last owner -> sweep reclaims the slot and its dict references.
  p.reset();
  evictAndSweep();
  EXPECT_EQ(0, encoder_.livePathAttrsCount());
  EXPECT_EQ(0, encoder_.liveDictEntryCount());
}

/*
 * A reclaimed slot must not be reused by a distinct path. FSDB may still hold
 * an unchanged entry that references the old index, so changing its meaning
 * would silently corrupt that entry until BGP happened to re-emit it.
 */
TEST_F(CanonicalRibEncoderTest, ReclaimedSlot_IsNeverReused) {
  int64_t oldIdx;
  {
    auto p = makePath(2, 1, folly::IPAddress("10.0.0.1"));
    auto oldEntry = encoder_.buildEntry(
        prefix("10.1.0.0/24"),
        kRibVersion,
        {bestPath(p)},
        /*exportMultipaths=*/true);
    oldIdx = oldEntry.paths()
                 .value()
                 .at(std::string(kBestPathGroup))[0]
                 .path_idx()
                 .value();
  }
  ASSERT_EQ(1, encoder_.livePathAttrsCount());
  evictAndSweep(); // path has no owner -> reclaimed
  EXPECT_EQ(0, encoder_.livePathAttrsCount());

  auto newEntry = encoder_.buildEntry(
      prefix("10.2.0.0/24"),
      kRibVersion,
      {bestPath(makePath(5, 4, folly::IPAddress("10.0.0.9")))},
      /*exportMultipaths=*/true);
  auto newIdx = newEntry.paths()
                    .value()
                    .at(std::string(kBestPathGroup))[0]
                    .path_idx()
                    .value();

  EXPECT_NE(oldIdx, newIdx);
  EXPECT_EQ(1, encoder_.livePathAttrsCount());
  const auto snapshot = encoder_.pathAttrsSnapshot();
  EXPECT_FALSE(snapshot.contains(oldIdx));
  EXPECT_NE(bgp_thrift::TBgpDedupedPath{}, snapshot.at(newIdx));
}

TEST_F(CanonicalRibEncoderTest, PeerDescriptionChangeRefreshesExistingSlot) {
  auto input = bestPath(makePath(2, 1, folly::IPAddress("10.0.0.1")));
  input.peerAddr = folly::IPAddress("10.0.0.2");
  input.peerRouterId = 42;
  input.peerDescription = "old-description";
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {input},
      /*exportMultipaths=*/true);
  EXPECT_TRUE(encoder_.consumeDirtyAndSweep(now_));

  input.peerDescription = "new-description";
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion + 1,
      {input},
      /*exportMultipaths=*/true);
  EXPECT_TRUE(encoder_.consumeDirtyAndSweep(now_));
  const auto peers = encoder_.peersSnapshot();
  ASSERT_EQ(1, peers.size());
  EXPECT_EQ(
      "new-description", peers.begin()->second.peer_description().value());

  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion + 2,
      {input},
      /*exportMultipaths=*/true);
  EXPECT_FALSE(encoder_.consumeDirtyAndSweep(now_));
}

/*
 * Replacing a prefix's path interns the new one immediately; the old path's
 * slot is reclaimed by a later sweep once it is no longer owned.
 */
TEST_F(CanonicalRibEncoderTest, ReplacePath_OldReclaimedWhenDead) {
  auto oldPath = makePath(2, 1, folly::IPAddress("10.0.0.1"));
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(oldPath)},
      /*exportMultipaths=*/true);
  // The new best path stays owned (the RIB holds the current best path).
  auto newPath = makePath(3, 2, folly::IPAddress("10.0.0.2"));
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion + 1,
      {bestPath(newPath)},
      /*exportMultipaths=*/true);

  // Both interned until the old path is reaped.
  EXPECT_EQ(2, encoder_.livePathAttrsCount());

  // The RIB drops the old path; the new path remains.
  oldPath.reset();
  evictAndSweep();
  EXPECT_EQ(1, encoder_.livePathAttrsCount());
}

/*
 * buildEntry marks the pool dirty (forcing a full dict/pool publish); a
 * subsequent publish with no new interning needs only an entries-only delta.
 */
TEST_F(CanonicalRibEncoderTest, PoolDirty_OnlyWhenPoolChanges) {
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(makePath(2, 1, folly::IPAddress("10.0.0.1")))},
      /*exportMultipaths=*/true);
  EXPECT_TRUE(encoder_.consumeDirtyAndSweep(now_)); // new slot -> full publish

  // No new interning since the last consume -> entries-only is sufficient.
  EXPECT_FALSE(encoder_.consumeDirtyAndSweep(now_));
}

TEST_F(CanonicalRibEncoderTest, ElapsedIntervalWithoutRetirementDoesNotSweep) {
  now_ += std::chrono::minutes(20);
  EXPECT_FALSE(encoder_.consumeDirtyAndSweep(now_));
}

TEST_F(CanonicalRibEncoderTest, ReclamationWaitsForThreeMinuteInterval) {
  auto path = makePath(2, 1, folly::IPAddress("10.0.0.1"));
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(path)},
      /*exportMultipaths=*/true);
  EXPECT_TRUE(encoder_.consumeDirtyAndSweep(now_));

  path.reset();
  clearAllDeduplicators();
  encoder_.markReclamationPending();
  now_ += std::chrono::minutes(2);
  EXPECT_FALSE(encoder_.consumeDirtyAndSweep(now_));
  EXPECT_EQ(1, encoder_.livePathAttrsCount());
  EXPECT_TRUE(encoder_.reclamationPending());

  now_ += std::chrono::minutes(1);
  EXPECT_TRUE(encoder_.consumeDirtyAndSweep(now_));
  EXPECT_EQ(0, encoder_.livePathAttrsCount());
  EXPECT_FALSE(encoder_.reclamationPending());
}

/*
 * The optional ECMP multipath group is preserved alongside the best path, and
 * each path interns independently.
 */
TEST_F(CanonicalRibEncoderTest, MultipathGroup_PreservedAlongsideBest) {
  CanonicalPathInput best =
      bestPath(makePath(2, 1, folly::IPAddress("10.0.0.1")));
  CanonicalPathInput ecmp;
  ecmp.path = makePath(2, 1, folly::IPAddress("10.0.0.2"));
  ecmp.group = kMultiPathGroup;
  ecmp.nextHopWeight = 42;

  auto entry = encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {best, ecmp},
      /*exportMultipaths=*/true);

  const auto& groups = entry.paths().value();
  ASSERT_EQ(1, groups.count(std::string(kBestPathGroup)));
  ASSERT_EQ(1, groups.count(std::string(kMultiPathGroup)));
  EXPECT_TRUE(groups.at(std::string(kBestPathGroup))[0].is_best_path().value());
  EXPECT_EQ(
      42, groups.at(std::string(kMultiPathGroup))[0].next_hop_weight().value());
  EXPECT_EQ(2, encoder_.livePathAttrsCount());
}

/*
 * In best-path-only mode (exportMultipaths=false) only the top-level best_path
 * is built: the pooled paths map and whole-path pool stay empty, but the best
 * path's sub-attributes are still interned so best_path resolves via the dict.
 */
TEST_F(CanonicalRibEncoderTest, BestPathOnly_SkipsPathsAndPool) {
  auto nexthop = folly::IPAddress("10.0.0.1");
  auto entry = encoder_.buildEntry(
      prefix("2401:db00::/32"),
      kRibVersion,
      {bestPath(makePath(/*asCount=*/3, /*commCount=*/2, nexthop))},
      /*exportMultipaths=*/false);

  // No pooled per-path representation.
  EXPECT_TRUE(entry.paths().value().empty());
  EXPECT_EQ(0, encoder_.livePathAttrsCount());

  // best_path is set and resolves via the dict (its sub-attrs were interned).
  ASSERT_TRUE(entry.best_path().has_value());
  const auto& best = entry.best_path().value();
  EXPECT_EQ(createTIpPrefix(nexthop), best.next_hop().value());
  ASSERT_TRUE(best.communities_idx().has_value());
  EXPECT_EQ(
      2,
      encoder_.dictSnapshot()
          .community_lists()
          .value()
          .at(best.communities_idx().value())
          .size());
}

TEST_F(CanonicalRibEncoderTest, PoolStatsTrackRetiredIdsWithoutHoles) {
  auto retained = makePath(2, 1, folly::IPAddress("10.0.0.1"));
  auto retired = makePath(3, 2, folly::IPAddress("10.0.0.2"));
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(retained)},
      /*exportMultipaths=*/true);
  encoder_.buildEntry(
      prefix("10.2.0.0/24"),
      kRibVersion,
      {bestPath(retired)},
      /*exportMultipaths=*/true);

  retired.reset();
  evictAndSweep();

  const auto stats = encoder_.poolStats();
  EXPECT_EQ(1, stats.wholePath.live);
  EXPECT_EQ(2, stats.wholePath.highWater);
  EXPECT_EQ(1, stats.wholePath.retired());
  EXPECT_EQ(1, encoder_.pathAttrsSnapshot().size());
}

TEST_F(CanonicalRibEncoderTest, IndividualSubAttrPoolsEraseRetiredIds) {
  auto retained = makePath(2, 1, folly::IPAddress("10.0.0.1"));
  auto retired = makePath(2, 2, folly::IPAddress("10.0.0.2"));
  encoder_.buildEntry(
      prefix("10.1.0.0/24"),
      kRibVersion,
      {bestPath(retained)},
      /*exportMultipaths=*/false);
  encoder_.buildEntry(
      prefix("10.2.0.0/24"),
      kRibVersion,
      {bestPath(retired)},
      /*exportMultipaths=*/false);

  retired.reset();
  evictAndSweep();

  const auto stats = encoder_.poolStats();
  EXPECT_EQ(0, stats.wholePath.highWater);
  EXPECT_EQ(1, stats.asPath.live);
  EXPECT_EQ(1, stats.asPath.highWater);
  EXPECT_EQ(1, stats.communities.live);
  EXPECT_EQ(2, stats.communities.highWater);
  EXPECT_EQ(1, encoder_.dictSnapshot().community_lists()->size());
}

TEST(CanonicalPoolStatsTest, RejectsLiveCountAboveHighWatermark) {
  const canonical::PoolStats invalid{
      .live = 2,
      .highWater = 1,
  };
  EXPECT_DEATH((void)invalid.retired(), "live count exceeds");
}

} // namespace facebook::bgp
