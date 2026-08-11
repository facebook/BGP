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

#include <deque>
#include <set>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibEntry.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

using DeDuplicatedBgpPath = nettools::bgplib::DeDuplicatedBgpPath;

class AdjRibEntryFixture : public ::testing::Test {
 public:
  void SetUp() override {
    // Clear the BgpPath deduplicator before each test
    DeDuplicatedBgpPath::clearDeduplicator();
  }

  void TearDown() override {
    // Clean up after each test
    DeDuplicatedBgpPath::clearDeduplicator();
  }

  // Helper function to create a BgpPath with specific AS path length
  std::shared_ptr<const BgpPath> createBgpPath(uint32_t asCount) {
    auto fields = buildBgpPathFields(asCount, 0, 0, 0);
    return std::make_shared<BgpPath>(*fields);
  }

  // Helper function to verify the deduplicator size
  void verifyDeduplicatorSize(size_t expectedSize) {
    EXPECT_EQ(DeDuplicatedBgpPath::deduplicatorSize(), expectedSize);
  }
};

/*
 * Pre-policy paths are interned globally, like post-policy ones.
 *
 * Both `setPreIn` and `setPostAttr` route their value through
 * DeDuplicatedBgpPath, so identical paths collapse onto one object across
 * every peer and every UPDATE.
 *
 * The distinction these tests pin down is WHICH axis the footprint follows.
 * AdjRib mints ONE pre-policy BgpPath per UPDATE PDU and hands that same
 * pointer to every prefix in the message, so pre-policy sharing within a PDU
 * predates interning. What interning adds is sharing ACROSS PDUs: without it,
 * two UPDATEs carrying byte-identical attributes and nexthop keep two objects
 * forever and the footprint tracks UPDATE COUNT rather than distinct values.
 *
 * They also pin the meaning of the `bgp_path` gauge
 * (= DeDuplicatedBgpPath::deduplicatorSize()): with both slots interned it
 * covers pre- and post-policy alike, so it can be read as "unique paths".
 */
TEST_F(AdjRibEntryFixture, PrePolicyPathsAreSharedWithinOneUpdate) {
  // One UPDATE => one BgpPath handed to every prefix it announces.
  auto fromOneUpdate = createBgpPath(3);

  AdjRibEntry entryA(/*pathId=*/1);
  AdjRibEntry entryB(/*pathId=*/2);
  entryA.setPreIn(fromOneUpdate);
  entryB.setPreIn(fromOneUpdate);

  // Same pointer: prefixes in one PDU share their pre-policy path.
  EXPECT_EQ(entryA.getPreIn().get(), entryB.getPreIn().get());
  // ...and the value is interned, so it is visible to the deduplicator.
  verifyDeduplicatorSize(1);
}

TEST_F(AdjRibEntryFixture, PrePolicyPathsAreDeduplicatedAcrossUpdates) {
  /*
   * Two UPDATEs carrying byte-identical attributes: separate allocations,
   * exactly as AdjRibIn does per PDU.
   */
  auto fromUpdate1 = createBgpPath(3);
  auto fromUpdate2 = createBgpPath(3);
  ASSERT_NE(fromUpdate1.get(), fromUpdate2.get());

  AdjRibEntry entryA(/*pathId=*/1);
  AdjRibEntry entryB(/*pathId=*/2);
  entryA.setPreIn(fromUpdate1);
  entryB.setPreIn(fromUpdate2);

  /*
   * Two byte-identical values minted by separate UPDATEs collapse onto one
   * stored object, so pre-policy footprint tracks DISTINCT VALUES rather than
   * UPDATE COUNT.
   */
  EXPECT_EQ(entryA.getPreIn().get(), entryB.getPreIn().get());
  verifyDeduplicatorSize(1);
}

TEST_F(AdjRibEntryFixture, PostPolicyPathsAreDeduplicatedAcrossUpdates) {
  // The contrast: the same two values through setPostAttr collapse to one.
  auto fromUpdate1 = createBgpPath(3);
  auto fromUpdate2 = createBgpPath(3);
  ASSERT_NE(fromUpdate1.get(), fromUpdate2.get());

  AdjRibEntry entryA(/*pathId=*/1);
  AdjRibEntry entryB(/*pathId=*/2);
  entryA.setPostAttr(fromUpdate1);
  entryB.setPostAttr(fromUpdate2);

  EXPECT_EQ(entryA.getPostAttr().get(), entryB.getPostAttr().get());
  verifyDeduplicatorSize(1);
}

TEST_F(AdjRibEntryFixture, DeduplicatorCountsBothPolicyStages) {
  /*
   * With both slots interned, an entry holding a DISTINCT pre- and post-policy
   * path contributes 2 -- and `bgp_path` finally means "unique paths", closing
   * the 2x gap against the walked total_unique_attributes.
   */
  auto prePolicy = createBgpPath(3);
  auto postPolicy = createBgpPath(4); // different value => distinct object
  ASSERT_NE(prePolicy.get(), postPolicy.get());

  AdjRibEntry entry(/*pathId=*/1);
  entry.setPreIn(prePolicy);
  entry.setPostAttr(postPolicy);

  verifyDeduplicatorSize(2);
}

/*
 * SCALE: what interning the pre-policy slot buys, at N.
 *
 * Each loop iteration models one UPDATE PDU announcing one prefix, all carrying
 * byte-identical attributes and the same nexthop -- a table dump split
 * per-prefix, or a route flapping N times. AdjRibIn mints one BgpPath per PDU
 * (outside the NLRI loop), so the VALUE repeats while the ALLOCATION does not.
 *
 * Both slots collapse all N onto one object. Pre-policy footprint is therefore
 * O(distinct values), not O(UPDATE count) -- note update count rather than
 * prefix count, because prefixes sharing a PDU already share their path.
 */
TEST_F(AdjRibEntryFixture, IdenticalPathsFromNUpdatesCostOnePrePolicyObject) {
  constexpr size_t kUpdates = 100;

  /*
   * deque: AdjRibEntry is held by reference below, so the container must not
   * reallocate its elements.
   */
  std::deque<AdjRibEntry> entries;
  std::set<const BgpPath*> distinctPreIn;
  std::set<const BgpPath*> distinctPostAttr;

  for (size_t i = 0; i < kUpdates; ++i) {
    // A fresh allocation per "UPDATE", identical in value to every other.
    auto mintedForThisUpdate = createBgpPath(/*asCount=*/3);

    entries.emplace_back(static_cast<uint32_t>(i + 1));
    auto& entry = entries.back();
    entry.setPreIn(mintedForThisUpdate);
    entry.setPostAttr(mintedForThisUpdate);

    distinctPreIn.insert(entry.getPreIn().get());
    distinctPostAttr.insert(entry.getPostAttr().get());
  }

  /*
   * THE POINT OF THE FIX: N UPDATEs carrying one value cost ONE object, not N.
   * A 100k-prefix table dump split per prefix no longer costs 100k objects.
   */
  EXPECT_EQ(1u, distinctPreIn.size());

  // Post-policy already behaved this way; both slots now agree.
  EXPECT_EQ(1u, distinctPostAttr.size());
  verifyDeduplicatorSize(1);
}

/*
 * The control for the test above: the same N prefixes arriving in ONE UPDATE
 * already cost one object without interning, because AdjRibIn hands every
 * prefix in a PDU the same pointer. Isolating this from the N-UPDATE case is
 * what shows the waste interning removes is driven by UPDATE count.
 */
TEST_F(AdjRibEntryFixture, PrefixesSharingOneUpdateCostOnePrePolicyObject) {
  constexpr size_t kPrefixesInOnePdu = 100;

  auto mintedOncePerPdu = createBgpPath(/*asCount=*/3);
  std::deque<AdjRibEntry> entries;
  std::set<const BgpPath*> distinctPreIn;

  for (size_t i = 0; i < kPrefixesInOnePdu; ++i) {
    entries.emplace_back(static_cast<uint32_t>(i + 1));
    entries.back().setPreIn(mintedOncePerPdu);
    distinctPreIn.insert(entries.back().getPreIn().get());
  }

  EXPECT_EQ(1u, distinctPreIn.size());
  verifyDeduplicatorSize(1);
}

/*
 * Interning carries NO precondition on publish state, deliberately.
 *
 * Production reaches setPreIn with a PUBLISHED path -- AdjRibIn publishes the
 * object it mints for an UPDATE before handing it over -- which is the shape
 * this test covers. The other tests deliberately do not publish, so between
 * them both shapes are exercised and neither is load-bearing.
 *
 * setPreIn must not depend on publish state: the deduplicator hands back a
 * shared_ptr<const BgpPath>, so nothing can mutate the stored object through
 * the interned handle regardless of how it arrived. setPostAttr interns
 * unconditionally for the same reason; requiring published input on only one
 * slot would make it the outlier and turn a future caller into a crash rather
 * than simply a missed dedup.
 *
 * setPreOut still stores verbatim, and does not need to intern: it is handed
 * the RIB best-entry path, which reached the RIB as an already-interned
 * postAttr, so peers announcing the same best path already share one object.
 */
TEST_F(AdjRibEntryFixture, AcceptsPublishedPaths) {
  auto fields = buildBgpPathFields(3, 0, 0, 0);
  auto published1 = std::make_shared<BgpPath>(*fields);
  auto published2 = std::make_shared<BgpPath>(*fields);
  published1->publish();
  published2->publish();
  ASSERT_TRUE(published1->isPublished());
  ASSERT_NE(published1.get(), published2.get());

  AdjRibEntry entryA(/*pathId=*/1);
  AdjRibEntry entryB(/*pathId=*/2);
  entryA.setPreIn(published1);
  entryB.setPreIn(published2);

  // Interned on value, exactly as an unpublished pair would be.
  EXPECT_EQ(entryA.getPreIn().get(), entryB.getPreIn().get());
  verifyDeduplicatorSize(1);
}

// Test eviction with empty deduplicator
TEST_F(AdjRibEntryFixture, EvictEmptyDeduplicatorTest) {
  EXPECT_EQ(DeDuplicatedBgpPath::deduplicatorSize(), 0);

  // Eviction should not crash on empty deduplicator
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();

  // Deduplicator should still be empty
  verifyDeduplicatorSize(0);
}

// Test eviction with all active entries
TEST_F(AdjRibEntryFixture, EvictNoStaleEntriesTest) {
  // Create some BgpPath objects and dedup them
  auto path1 = createBgpPath(2);
  auto path2 = createBgpPath(3);
  auto path3 = createBgpPath(4);

  // Dedup via DeDuplicatedBgpPath — these hold references
  auto deduped1 = DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path1));
  auto deduped2 = DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path2));
  auto deduped3 = DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path3));

  // Verify deduplicator size
  verifyDeduplicatorSize(3);

  // Eviction should not remove any entries (all have external refs)
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();

  // Deduplicator should still have 3 entries
  verifyDeduplicatorSize(3);
}

// Test eviction with all stale entries
TEST_F(AdjRibEntryFixture, EvictAllStaleEntriesTest) {
  auto tcData = facebook::fb303::ThreadCachedServiceData::get();

  // Create and dedup paths, then let the DeDuplicatedBgpPath wrappers
  // go out of scope so only the deduplicator cache holds references
  {
    auto path1 = createBgpPath(2);
    auto path2 = createBgpPath(3);
    auto path3 = createBgpPath(4);

    DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path1));
    DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path2));
    DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path3));

    verifyDeduplicatorSize(3);
  }
  // All DeDuplicatedBgpPath wrappers and paths go out of scope

  // Deduplicator should still have 3 entries before eviction
  verifyDeduplicatorSize(3);
  // Verify ODS counter reflects deduplicator size
  BgpStats::setDeduplicatedAttributesBgpPath(
      DeDuplicatedBgpPath::deduplicatorSize());
  tcData->publishStats();
  EXPECT_EQ(3, tcData->getCounter(BgpStats::kDeduplicatedAttributesBgpPath));

  // Eviction should remove all stale entries
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();

  // Deduplicator should now be empty
  verifyDeduplicatorSize(0);
  // ODS counter should reflect 0 after eviction
  BgpStats::setDeduplicatedAttributesBgpPath(
      DeDuplicatedBgpPath::deduplicatorSize());
  tcData->publishStats();
  EXPECT_EQ(0, tcData->getCounter(BgpStats::kDeduplicatedAttributesBgpPath));
}

// Test eviction with mixed active and stale entries
TEST_F(AdjRibEntryFixture, EvictMixedEntriesTest) {
  auto tcData = facebook::fb303::ThreadCachedServiceData::get();

  // Create active deduped paths (will be kept)
  auto activePath1 = createBgpPath(2);
  auto activePath2 = createBgpPath(3);

  auto activeDeduped1 =
      DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(activePath1));
  auto activeDeduped2 =
      DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(activePath2));

  {
    // These paths will become stale when they go out of scope
    auto stalePath1 = createBgpPath(4);
    auto stalePath2 = createBgpPath(5);
    auto stalePath3 = createBgpPath(6);

    DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(stalePath1));
    DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(stalePath2));
    DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(stalePath3));

    verifyDeduplicatorSize(5);
  }
  // Stale paths go out of scope

  // Deduplicator should still have 5 entries before eviction
  verifyDeduplicatorSize(5);
  // ODS counter reflects 5 entries
  BgpStats::setDeduplicatedAttributesBgpPath(
      DeDuplicatedBgpPath::deduplicatorSize());
  tcData->publishStats();
  EXPECT_EQ(5, tcData->getCounter(BgpStats::kDeduplicatedAttributesBgpPath));

  // Eviction should remove only the 3 stale entries
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();

  // Deduplicator should now have 2 active entries
  verifyDeduplicatorSize(2);
  // ODS counter reflects 2 after eviction
  BgpStats::setDeduplicatedAttributesBgpPath(
      DeDuplicatedBgpPath::deduplicatorSize());
  tcData->publishStats();
  EXPECT_EQ(2, tcData->getCounter(BgpStats::kDeduplicatedAttributesBgpPath));
}

// Test dedup after AdjRibEntry usage via setPostAttr
TEST_F(AdjRibEntryFixture, DedupAfterAdjRibEntryUsageTest) {
  // Create AdjRibEntry and set post attributes
  AdjRibEntry entry1(1);
  AdjRibEntry entry2(2);

  {
    // Create paths in a scope so they can be released
    auto path1 = createBgpPath(2);
    auto path2 = createBgpPath(3);

    entry1.setPostAttr(path1);
    entry2.setPostAttr(path2);

    // Both paths should be in the deduplicator
    EXPECT_GE(DeDuplicatedBgpPath::deduplicatorSize(), 1);
  }
  // path1 and path2 local variables go out of scope here

  // After paths go out of scope, entries should still hold references
  // via the deduplicator
  EXPECT_GE(DeDuplicatedBgpPath::deduplicatorSize(), 1);

  // Clear one entry's attributes
  entry1.setPostAttr(nullptr);

  // Eviction should remove stale entries
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();

  // At least one entry should remain (the one used by entry2)
  verifyDeduplicatorSize(1);

  // Verify entry2 still has a valid postAttr
  auto postAttr = entry2.getPostAttr();
  EXPECT_NE(postAttr, nullptr);
}

// Test eviction is idempotent
TEST_F(AdjRibEntryFixture, EvictIdempotentTest) {
  // Insert some stale entries
  {
    auto path1 = createBgpPath(2);
    auto path2 = createBgpPath(3);
    DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path1));
    DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path2));
  }

  verifyDeduplicatorSize(2);

  // First eviction
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();
  verifyDeduplicatorSize(0);

  // Second eviction should not crash
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();
  verifyDeduplicatorSize(0);

  // Third eviction should also not crash
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();
  verifyDeduplicatorSize(0);
}

// Test dedup with shared references (same path deduped multiple times)
TEST_F(AdjRibEntryFixture, DedupWithSharedReferencesTest) {
  auto path1 = createBgpPath(2);

  // Dedup path multiple times (should only create one cache entry)
  auto deduped1 = DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path1));
  auto deduped2 = DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path1));
  auto deduped3 = DeDuplicatedBgpPath(std::const_pointer_cast<BgpPath>(path1));

  // Should only have one entry in the deduplicator
  verifyDeduplicatorSize(1);

  // All deduped wrappers should be equal (pointer comparison via operator==)
  EXPECT_EQ(deduped1, deduped2);
  EXPECT_EQ(deduped2, deduped3);

  // Drop one reference
  deduped3 = DeDuplicatedBgpPath();

  // Eviction should not remove the entry (still has external refs)
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();
  verifyDeduplicatorSize(1);

  // Drop another reference
  deduped2 = DeDuplicatedBgpPath();

  // Still should not be removed
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();
  verifyDeduplicatorSize(1);

  // Drop the last deduped ref but path1 is still alive.
  // path1 shares the control block with the cache entry
  // (via const_pointer_cast), so use_count is still > 1.
  deduped1 = DeDuplicatedBgpPath();

  // path1 keeps the cache entry alive (shared control block)
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();
  verifyDeduplicatorSize(1);

  // Now drop path1 — only the cache reference remains
  path1.reset();

  // Now eviction should remove it (use_count == 1)
  DeDuplicatedBgpPath::evictDeletedEntriesFromDeduplicator();
  verifyDeduplicatorSize(0);
}

// Test that setPostAttr deduplicates identical BgpPaths
TEST_F(AdjRibEntryFixture, SetPostAttrDeduplicatesTest) {
  AdjRibEntry entry1(1);
  AdjRibEntry entry2(2);

  // Create two paths with identical content
  auto path1 = createBgpPath(2);
  auto path2 = createBgpPath(2);

  // Set post attrs — both should dedup to the same pointer
  entry1.setPostAttr(path1);
  entry2.setPostAttr(path2);

  // Only one entry in the deduplicator (same content)
  verifyDeduplicatorSize(1);

  // Both entries should point to the same deduped object
  EXPECT_EQ(entry1.getPostAttr().get(), entry2.getPostAttr().get());
}

// Test stale bit bitmap accessor methods
TEST_F(AdjRibEntryFixture, StaleBitAccessorTest) {
  AdjRibEntry entry(1);

  // Entry should not be stale by default
  EXPECT_FALSE(entry.isStale());

  // Set entry as stale
  entry.setStale(true);
  EXPECT_TRUE(entry.isStale());

  // Clear stale bit
  entry.setStale(false);
  EXPECT_FALSE(entry.isStale());
}

// Add-path GR old-path-id ownership accessors (flags_ bit 2 +
// oldPathId_)
TEST_F(AdjRibEntryFixture, OldPathIdOwnershipAccessors) {
  AdjRibEntry entry(1);

  // No old-path-id ownership by default.
  EXPECT_FALSE(entry.hasOldPathId());

  // Presence is tracked by the flag bit, so ids 0 and UINT32_MAX (both valid
  // path ids) round-trip correctly.
  entry.setOldPathId(0);
  EXPECT_TRUE(entry.hasOldPathId());
  EXPECT_EQ(entry.getOldPathId(), 0u);

  entry.setOldPathId(UINT32_MAX);
  EXPECT_TRUE(entry.hasOldPathId());
  EXPECT_EQ(entry.getOldPathId(), UINT32_MAX);

  // Ownership is independent of the stale bit (guards against a whole-byte
  // clobber of flags_).
  entry.setStale(true);
  EXPECT_TRUE(entry.hasOldPathId());
  EXPECT_TRUE(entry.isStale());

  entry.clearOldPathId();
  EXPECT_FALSE(entry.hasOldPathId());
  EXPECT_TRUE(entry.isStale());
}

// Add-path GR pending-op accessors (flags_ bits 3-4)
TEST_F(AdjRibEntryFixture, PendingOpAccessors) {
  AdjRibEntry entry(1);

  // Defaults to None.
  EXPECT_EQ(entry.getPendingOp(), AdjRibEntry::PendingOp::None);

  entry.setPendingOp(AdjRibEntry::PendingOp::Announce);
  EXPECT_EQ(entry.getPendingOp(), AdjRibEntry::PendingOp::Announce);

  // Re-derivation overwrites the 2-bit field cleanly (no bit leakage).
  entry.setPendingOp(AdjRibEntry::PendingOp::Withdraw);
  EXPECT_EQ(entry.getPendingOp(), AdjRibEntry::PendingOp::Withdraw);

  // Pending op is independent of the stale / nexthopSetByPolicy / old-path-id
  // bits packed into the same byte.
  entry.setStale(true);
  entry.setNexthopSetByPolicy(true);
  entry.setOldPathId(7);
  EXPECT_EQ(entry.getPendingOp(), AdjRibEntry::PendingOp::Withdraw);
  EXPECT_TRUE(entry.isStale());
  EXPECT_TRUE(entry.isNexthopSetByPolicy());
  EXPECT_TRUE(entry.hasOldPathId());

  entry.setPendingOp(AdjRibEntry::PendingOp::None);
  EXPECT_EQ(entry.getPendingOp(), AdjRibEntry::PendingOp::None);
  EXPECT_TRUE(entry.isStale());
  EXPECT_TRUE(entry.hasOldPathId());
}

// clearAddPathGrState clears ONLY the RIB-IN-only add-path GR marker bits.
TEST_F(AdjRibEntryFixture, ClearAddPathGrStateClearsOnlyMarkerBits) {
  AdjRibEntry entry(1);
  entry.setStale(true);
  entry.setNexthopSetByPolicy(true);
  entry.setOldPathId(42);
  entry.setPendingOp(AdjRibEntry::PendingOp::Withdraw);

  entry.clearAddPathGrState();

  // Add-path GR marker state is gone...
  EXPECT_FALSE(entry.hasOldPathId());
  EXPECT_EQ(entry.getOldPathId(), 0u);
  EXPECT_EQ(entry.getPendingOp(), AdjRibEntry::PendingOp::None);
  // ...but stale / nexthopSetByPolicy survive.
  EXPECT_TRUE(entry.isStale());
  EXPECT_TRUE(entry.isNexthopSetByPolicy());
}

TEST_F(AdjRibEntryFixture, RibVersionDefaultsToZero) {
  AdjRibEntry entry(/* pathId */ 0);
  EXPECT_EQ(entry.getRibVersion(), 0);
}

TEST_F(AdjRibEntryFixture, RibVersionSetAndGet) {
  AdjRibEntry entry(/* pathId */ 0);

  entry.setRibVersion(42);
  EXPECT_EQ(entry.getRibVersion(), 42);

  entry.setRibVersion(100);
  EXPECT_EQ(entry.getRibVersion(), 100);
}

} // namespace facebook::bgp
