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

#define AdjRib_TEST_FRIENDS                                        \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_InitialAdvertisement);          \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_PrefixNotFound);                \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_DuplicateWithdrawal);           \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_DuplicateAnnouncement);         \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_WithdrawalToAnnouncement);      \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_AnnouncementToWithdrawal);      \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_ReannounceWithNewPath);         \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_NexthopPolicyFlagFlipWithdraw); \
  FRIEND_TEST(                                                     \
      AdjRibOutboundPackingFixture,                                \
      TryUpdateAttrToPrefixMapTest_NexthopPolicyFlagFlipReannounce);

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"

namespace facebook::bgp {
using namespace facebook::nettools::bgplib;

namespace {
/*
 * Count how many packing-list buckets currently hold prefixPathId. The core
 * invariant is that a prefix is associated to at most one path (bucket) at a
 * time, so this must never exceed 1.
 */
size_t countBucketsContaining(
    const AttrToPrefixMap& attrToPrefixMap,
    const std::pair<folly::CIDRNetwork, uint32_t>& prefixPathId) {
  size_t count = 0;
  for (const auto& [key, pfxSet] : attrToPrefixMap) {
    if (pfxSet.contains(prefixPathId)) {
      ++count;
    }
  }
  return count;
}
} // namespace

/*
 * This section covers unit tests for tryUpdateAttrToPrefixMap.
 *
 * The next 7 UTs cover the following scenarios:
 *
 *  0. Initial advertisement
 *  1. attrToPrefixMap prefix not found in oldPath pfxSet
 *  2. Duplicate withdrawal
 *  3. Duplicate announcement
 *  4. Withdrawal to announcement
 *  5. Announcement to withdrawal
 *  6. Announcement with new route after previous announcement.
 *
 * Note that empty pfxSet cleanup is automatically covered by checking
 * the size of the attrToPrefixMap after update.
 */
class AdjRibOutboundPackingFixture : public AdjRibOutboundFixture {
 public:
  void SetUp() override {
    // Register Singleton
    folly::SingletonVault::singleton()->registrationComplete();
    DeDuplicatedBgpPath::clearDeduplicator();
    postPolicyResultCache_.clear();
    fm_ = std::make_unique<folly::fibers::FiberManager>(
        std::make_unique<folly::fibers::EventBaseLoopController>(),
        facebook::nettools::bgplib::getFiberManagerOptions());

    static_cast<folly::fibers::EventBaseLoopController&>(fm_->loopController())
        .attachEventBase(evb_);

    setupAdjRibForOutUnitTest();

    auto update = buildBgpUpdateAttributes(kV4Nexthop1);
    announcementAttrs_ = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(update)));
  }

  std::shared_ptr<const BgpPath> withdrawalAttrs_{nullptr};
  const std::pair<folly::CIDRNetwork, uint32_t> prefixPathId_ =
      std::make_pair(kV4Prefix1, 0);

  std::shared_ptr<const BgpPath> announcementAttrs_;
};

TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_InitialAdvertisement) {
  EXPECT_TRUE(adjRib_->attrToPrefixMap_.empty());

  // This should go through with no failure.
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, withdrawalAttrs_, announcementAttrs_);

  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;

  EXPECT_EQ(1, attrToPrefixMap.size());
  EXPECT_EQ(announcementAttrs_, attrToPrefixMap.begin()->first.attrs);

  auto& pfxSet = attrToPrefixMap.begin()->second;
  EXPECT_EQ(1, pfxSet.size());
  EXPECT_EQ(prefixPathId_, *pfxSet.begin());
}

TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_PrefixNotFound) {
  auto attrs2 = announcementAttrs_->clone();
  attrs2->setLocalPref(10000);

  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;
  // attrToPrefixMap should have one entry.
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, withdrawalAttrs_, announcementAttrs_);
  EXPECT_EQ(1, attrToPrefixMap.size());

  std::pair<folly::CIDRNetwork, uint32_t> prefixPathId2 =
      std::make_pair(kV4Prefix2, 0);

  /*
   * announcementAttrs only has prefixPathId_.
   * Announce prefixPathId2, oldPath = announcementAttrs_ and newPath = attrs2.
   * This should succeed.
   */
  adjRib_->tryUpdateAttrToPrefixMap(prefixPathId2, announcementAttrs_, attrs2);

  /*
   * Verify the map is now {announcementAttrs_, prefixPathId_} and
   * {attrs2, prefixPathId2}.
   */
  EXPECT_EQ(2, attrToPrefixMap.size());
  auto attrsWithAfi1 =
      BgpPathWithAfi{announcementAttrs_, BgpUpdateAfi::AFI_IPv4};
  auto attrsWithAfi2 = BgpPathWithAfi{attrs2, BgpUpdateAfi::AFI_IPv4};
  EXPECT_TRUE(attrToPrefixMap.contains(attrsWithAfi1));
  EXPECT_TRUE(attrToPrefixMap.contains(attrsWithAfi2));

  EXPECT_EQ(1, attrToPrefixMap.at(attrsWithAfi1).size());
  EXPECT_TRUE(attrToPrefixMap.at(attrsWithAfi1).contains(prefixPathId_));

  EXPECT_EQ(1, attrToPrefixMap.at(attrsWithAfi2).size());
  EXPECT_TRUE(attrToPrefixMap.at(attrsWithAfi2).contains(prefixPathId2));
}

TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_DuplicateWithdrawal) {
  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;
  EXPECT_TRUE(attrToPrefixMap.empty());

  {
    /*
     * Case 1: Withdrawing a never-announced prefix.
     * tryUpdateAttrToPrefixMap applies the requested mutation unconditionally
     * and does NOT dedup: suppressing withdrawals for prefixes that were never
     * advertised is the caller's responsibility (tryInsertWithdrawal only fires
     * when postAttr is non-null). So the helper stages the withdrawal entry
     * here rather than treating it as a no-op.
     */
    adjRib_->tryUpdateAttrToPrefixMap(
        prefixPathId_, withdrawalAttrs_, withdrawalAttrs_);
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(withdrawalAttrs_, attrToPrefixMap.begin()->first.attrs);
  }

  {
    // Case 2: Withdrawing a seen prefix twice.
    adjRib_->tryUpdateAttrToPrefixMap(
        prefixPathId_, withdrawalAttrs_, announcementAttrs_);
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(announcementAttrs_, attrToPrefixMap.begin()->first.attrs);

    adjRib_->tryUpdateAttrToPrefixMap(
        prefixPathId_, announcementAttrs_, withdrawalAttrs_);
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(withdrawalAttrs_, attrToPrefixMap.begin()->first.attrs);

    adjRib_->tryUpdateAttrToPrefixMap(
        prefixPathId_, withdrawalAttrs_, withdrawalAttrs_);
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(withdrawalAttrs_, attrToPrefixMap.begin()->first.attrs);
  }
}

TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_DuplicateAnnouncement) {
  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;
  EXPECT_TRUE(attrToPrefixMap.empty());

  /*
   * Set the initial state as kV4Prefix1,0 being announced with
   * announcementAttrs_.
   */
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, withdrawalAttrs_, announcementAttrs_);
  {
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(announcementAttrs_, attrToPrefixMap.begin()->first.attrs);

    auto& pfxSet = attrToPrefixMap.begin()->second;
    EXPECT_EQ(1, pfxSet.size());
    EXPECT_EQ(prefixPathId_, *pfxSet.begin());
  }

  // Now, announce kV4Prefix1,0 again with the same announcementAttrs_.
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, announcementAttrs_, announcementAttrs_);
  {
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(announcementAttrs_, attrToPrefixMap.begin()->first.attrs);

    auto& pfxSet = attrToPrefixMap.begin()->second;
    EXPECT_EQ(1, pfxSet.size());
    EXPECT_EQ(prefixPathId_, *pfxSet.begin());
  }
}

TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_WithdrawalToAnnouncement) {
  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;
  EXPECT_TRUE(attrToPrefixMap.empty());

  // Set the initial state as kV4Prefix1,0 being seen and withdrawn.
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, withdrawalAttrs_, announcementAttrs_);
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, announcementAttrs_, withdrawalAttrs_);
  {
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(withdrawalAttrs_, attrToPrefixMap.begin()->first.attrs);

    auto& pfxSet = attrToPrefixMap.begin()->second;
    EXPECT_EQ(1, pfxSet.size());
    EXPECT_EQ(prefixPathId_, *pfxSet.begin());
  }

  // Now, announce kV4Prefix1,0 with announcementAttrs_.
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, withdrawalAttrs_, announcementAttrs_);
  {
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(announcementAttrs_, attrToPrefixMap.begin()->first.attrs);

    auto& pfxSet = attrToPrefixMap.begin()->second;
    EXPECT_EQ(1, pfxSet.size());
    EXPECT_EQ(prefixPathId_, *pfxSet.begin());
  }
}

TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_AnnouncementToWithdrawal) {
  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;
  EXPECT_TRUE(attrToPrefixMap.empty());

  /*
   * Set the initial state as kV4Prefix1,0 being announced with
   * announcementAttrs_.
   */
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, withdrawalAttrs_, announcementAttrs_);
  {
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(announcementAttrs_, attrToPrefixMap.begin()->first.attrs);

    auto& pfxSet = attrToPrefixMap.begin()->second;
    EXPECT_EQ(1, pfxSet.size());
    EXPECT_EQ(prefixPathId_, *pfxSet.begin());
  }

  // Now, withdraw kV4Prefix1,0.
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, announcementAttrs_, withdrawalAttrs_);
  {
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(withdrawalAttrs_, attrToPrefixMap.begin()->first.attrs);

    auto& pfxSet = attrToPrefixMap.begin()->second;
    EXPECT_EQ(1, pfxSet.size());
    EXPECT_EQ(prefixPathId_, *pfxSet.begin());
  }
}

TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_ReannounceWithNewPath) {
  BgpUpdate2 update = buildBgpUpdateAttributes(kV4Nexthop1);
  auto attrs1 = std::make_shared<facebook::bgp::BgpPath>(
      BgpPathFields(*BgpUpdate2toBgpPathC(update)));
  attrs1->setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP);

  auto attrs2 = attrs1->clone();
  attrs2->setOrigin(BgpAttrOrigin::BGP_ORIGIN_EGP);

  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;
  EXPECT_TRUE(attrToPrefixMap.empty());

  // Set the initial state as kV4Prefix1,0 being announced with attrs1.
  adjRib_->tryUpdateAttrToPrefixMap(prefixPathId_, withdrawalAttrs_, attrs1);
  {
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(attrs1, attrToPrefixMap.begin()->first.attrs);

    auto& pfxSet = attrToPrefixMap.begin()->second;
    EXPECT_EQ(1, pfxSet.size());
    EXPECT_EQ(prefixPathId_, *pfxSet.begin());
  }

  std::shared_ptr<const BgpPath> constAttrs1 = attrs1;

  // Now, announce kV4Prefix1,0 again with attrs2.
  adjRib_->tryUpdateAttrToPrefixMap(prefixPathId_, constAttrs1, attrs2);
  {
    EXPECT_EQ(1, attrToPrefixMap.size());
    EXPECT_EQ(attrs2, attrToPrefixMap.begin()->first.attrs);

    auto& pfxSet = attrToPrefixMap.begin()->second;
    EXPECT_EQ(1, pfxSet.size());
    EXPECT_EQ(prefixPathId_, *pfxSet.begin());
  }
}

/*
 * Regression for the isNexthopSetByPolicy packing-list key bug.
 *
 * isNexthopSetByPolicy is part of the packing-list key, so the same
 * (path, AFI) can be staged under either flag value. Announce call sites pass
 * the real flag (which can be true when a SetNexthop policy action ran), but
 * every withdraw/collapse call site defaults the flag to false.
 *
 * If cleanup only probes the caller-supplied flag, withdrawing a prefix that
 * was staged under flag = true misses the stale true-keyed announcement bucket,
 * leaving the prefix in two buckets at once: a withdrawal AND a positive
 * advertisement. Cleanup must probe both flag states so the prefix stays
 * associated to exactly one path.
 */
TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_NexthopPolicyFlagFlipWithdraw) {
  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;
  EXPECT_TRUE(attrToPrefixMap.empty());

  /*
   * Stage an announcement whose nexthop was set by policy (flag = true). This
   * keys the prefix under {announcementAttrs_, AFI_IPv4, true}.
   */
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_,
      withdrawalAttrs_,
      announcementAttrs_,
      /*isNexthopSetByPolicy=*/true);
  EXPECT_EQ(1, attrToPrefixMap.size());
  EXPECT_TRUE(attrToPrefixMap.begin()->first.isNexthopSetByPolicy);

  /*
   * Withdraw the prefix with the default flag (false), mirroring the prod
   * withdraw call sites.
   */
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_,
      announcementAttrs_,
      withdrawalAttrs_,
      /*isNexthopSetByPolicy=*/false);

  /*
   * The prefix must be associated to exactly one path, and that path must be
   * the withdrawal (nullptr attrs), not the leaked announcement.
   */
  EXPECT_EQ(1, countBucketsContaining(attrToPrefixMap, prefixPathId_));
  EXPECT_EQ(1, attrToPrefixMap.size());
  EXPECT_EQ(withdrawalAttrs_, attrToPrefixMap.begin()->first.attrs);
}

/*
 * Regression: re-announcing a prefix with a new path flagged as policy-set
 * nexthop (flag = true) while the previous path was staged under flag = false.
 * Cleanup of the old association must probe both flag states; otherwise the
 * prefix ends up in two positive-advertisement buckets at once.
 */
TEST_F(
    AdjRibOutboundPackingFixture,
    TryUpdateAttrToPrefixMapTest_NexthopPolicyFlagFlipReannounce) {
  auto attrs1 = announcementAttrs_;
  auto attrs2Mutable = announcementAttrs_->clone();
  attrs2Mutable->setLocalPref(10000);
  std::shared_ptr<const BgpPath> attrs2 = attrs2Mutable;

  auto& attrToPrefixMap = adjRib_->attrToPrefixMap_;
  EXPECT_TRUE(attrToPrefixMap.empty());

  // Stage announcement of the prefix under {attrs1, AFI_IPv4, false}.
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, withdrawalAttrs_, attrs1, /*isNexthopSetByPolicy=*/false);
  EXPECT_EQ(1, attrToPrefixMap.size());
  EXPECT_FALSE(attrToPrefixMap.begin()->first.isNexthopSetByPolicy);

  /*
   * Re-announce with a different path, now flagged as policy-set nexthop
   * (flag = true). oldPath (attrs1) is keyed under flag = false.
   */
  adjRib_->tryUpdateAttrToPrefixMap(
      prefixPathId_, attrs1, attrs2, /*isNexthopSetByPolicy=*/true);

  // Exactly one bucket, holding the new path with flag = true.
  EXPECT_EQ(1, countBucketsContaining(attrToPrefixMap, prefixPathId_));
  EXPECT_EQ(1, attrToPrefixMap.size());
  EXPECT_EQ(attrs2, attrToPrefixMap.begin()->first.attrs);
  EXPECT_TRUE(attrToPrefixMap.begin()->first.isNexthopSetByPolicy);
}

} // namespace facebook::bgp
