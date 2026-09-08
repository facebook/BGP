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

#include <folly/Singleton.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"

namespace facebook::bgp {

class AdjRibPrefixSetFixture : public ::testing::Test {
 public:
  void SetUp() override {
    // Register Singleton
    folly::SingletonVault::singleton()->registrationComplete();
  }

  folly::CIDRNetwork v4Network1{"10.0.0.1", 32};
  folly::CIDRNetwork v4Network2{"10.0.0.1", 24};
  folly::CIDRNetwork v6Network1{"2001::1", 128};
  folly::CIDRNetwork v6Network2{"2001::1", 64};
};

/*
 * Unit test to check empty AdjRibPrefixSet.
 */
TEST_F(AdjRibPrefixSetFixture, EmptyAdjRibPrefixSetTest) {
  auto uniquePrefixSet = AdjRibPrefixSet::get();

  EXPECT_EQ(0, uniquePrefixSet->size());
  EXPECT_EQ(0, uniquePrefixSet->goldenVipSize());
}

/*
 * Unit test to check refCount check for a non-existing prefix.
 */
TEST_F(AdjRibPrefixSetFixture, NonExistingPrefixRefCountTest) {
  auto uniquePrefixSet = AdjRibPrefixSet::get();

  const auto res = uniquePrefixSet->getRefCount(v6Network1);
  EXPECT_FALSE(res.first);
  EXPECT_EQ(0, res.second.refCount_);
  EXPECT_FALSE(res.second.isGoldenVip_);
  EXPECT_NO_THROW(uniquePrefixSet->delPrefix(v6Network1));
  EXPECT_EQ(0, uniquePrefixSet->size());
  EXPECT_EQ(0, uniquePrefixSet->goldenVipSize());
}

/*
 * Unit test to validate basic functionality of AdjRibPrefixSet including:
 *  1) add a prefix(first time, repeatedly, etc.)
 *  2) remove a prefix(first time, repeatedly, non-existing one, etc.)
 */
TEST_F(AdjRibPrefixSetFixture, AdjRibPrefixSetBasicTest) {
  auto uniquePrefixSet = AdjRibPrefixSet::get();

  {
    // Test 1: add a v4Network.
    uniquePrefixSet->addPrefix(v4Network1, false);
    EXPECT_EQ(1, uniquePrefixSet->size());

    const auto res = uniquePrefixSet->getRefCount(v4Network1);
    EXPECT_TRUE(res.first);
    EXPECT_EQ(1, res.second.refCount_);
    EXPECT_FALSE(res.second.isGoldenVip_);
  }
  {
    /*
     * Test 2: add the same v4Network again along with the same prefix but a
     * different mask length.
     */
    uniquePrefixSet->addPrefix(v4Network1, false);
    uniquePrefixSet->addPrefix(v4Network2, true);
    EXPECT_EQ(2, uniquePrefixSet->size());
    EXPECT_EQ(1, uniquePrefixSet->goldenVipSize());

    const auto res1 = uniquePrefixSet->getRefCount(v4Network1);
    EXPECT_TRUE(res1.first);
    EXPECT_EQ(2, res1.second.refCount_);
    EXPECT_FALSE(res1.second.isGoldenVip_);

    const auto res2 = uniquePrefixSet->getRefCount(v4Network2);
    EXPECT_TRUE(res2.first);
    EXPECT_EQ(1, res2.second.refCount_);
    EXPECT_TRUE(res2.second.isGoldenVip_);
  }
  {
    // Test 3: remove existing v4Networks
    uniquePrefixSet->delPrefix(v4Network1);
    uniquePrefixSet->delPrefix(v4Network2);
    EXPECT_EQ(1, uniquePrefixSet->size()); // v4Network2 is removed
    EXPECT_EQ(0, uniquePrefixSet->goldenVipSize());

    const auto res1 = uniquePrefixSet->getRefCount(v4Network1);
    EXPECT_TRUE(res1.first);
    EXPECT_EQ(1, res1.second.refCount_); // refCount--

    const auto res2 = uniquePrefixSet->getRefCount(v4Network2);
    EXPECT_FALSE(res2.first); // non-existing
    EXPECT_EQ(0, res2.second.refCount_);
  }
  {
    // Test 4: add v6Network
    uniquePrefixSet->addPrefix(v6Network1, false);
    uniquePrefixSet->addPrefix(v6Network2, true);
    EXPECT_EQ(3, uniquePrefixSet->size()); // 1 v4Network + 2 v6Networks
    EXPECT_EQ(1, uniquePrefixSet->goldenVipSize());

    const auto res1 = uniquePrefixSet->getRefCount(v6Network1);
    EXPECT_TRUE(res1.first);
    EXPECT_EQ(1, res1.second.refCount_);
    EXPECT_FALSE(res1.second.isGoldenVip_);

    const auto res2 = uniquePrefixSet->getRefCount(v6Network2);
    EXPECT_TRUE(res2.first); // non-existing
    EXPECT_EQ(1, res2.second.refCount_);
    EXPECT_TRUE(res2.second.isGoldenVip_);
  }
  {
    // Test 5: clear the tree
    uniquePrefixSet->clear();
    EXPECT_EQ(0, uniquePrefixSet->size());
    EXPECT_EQ(0, uniquePrefixSet->goldenVipSize());

    const auto res1 = uniquePrefixSet->getRefCount(v6Network1);
    EXPECT_FALSE(res1.first); // non-existing
    EXPECT_EQ(0, res1.second.refCount_);

    const auto res2 = uniquePrefixSet->getRefCount(v6Network2);
    EXPECT_FALSE(res2.first); // non-existing
    EXPECT_EQ(0, res2.second.refCount_);
  }
  {
    // Test 6: re-add the networks
    uniquePrefixSet->addPrefix(v6Network1, false);
    EXPECT_EQ(1, uniquePrefixSet->size());
    EXPECT_EQ(0, uniquePrefixSet->goldenVipSize());

    const auto res1 = uniquePrefixSet->getRefCount(v6Network1);
    EXPECT_TRUE(res1.first);
    EXPECT_EQ(1, res1.second.refCount_);
    EXPECT_FALSE(res1.second.isGoldenVip_);

    const auto res2 = uniquePrefixSet->getRefCount(v6Network2);
    EXPECT_FALSE(res2.first);
    EXPECT_EQ(0, res2.second.refCount_);

    uniquePrefixSet->delPrefix(v6Network2);
    EXPECT_EQ(1, uniquePrefixSet->size());
    const auto res3 = uniquePrefixSet->getRefCount(v6Network2);
    EXPECT_FALSE(res3.first);
    EXPECT_EQ(0, res3.second.refCount_);
  }
}
} // namespace facebook::bgp
