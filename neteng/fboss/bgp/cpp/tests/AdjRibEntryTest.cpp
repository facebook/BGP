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
    return std::make_shared<const BgpPath>(*fields);
  }

  // Helper function to verify the deduplicator size
  void verifyDeduplicatorSize(size_t expectedSize) {
    EXPECT_EQ(DeDuplicatedBgpPath::deduplicatorSize(), expectedSize);
  }
};

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
