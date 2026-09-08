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

#include <folly/logging/xlog.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"

namespace facebook::bgp {

class AdjRibPostPolicyResultCacheFixture : public ::testing::Test {
 public:
  void SetUp() override {
    postPolicyResultCache_.clear();
  }
  // Test prefix.
  folly::CIDRNetwork v4Network{"10.0.0.1", 32};
  folly::CIDRNetwork v6Network{"2001::1", 128};
  const uint32_t kPathId = 0x1;

  // Test policy term strings
  const std::string kPostPolicyResultDeniedByCrf = "Denied by CRF";
  const std::string kPostPolicyResultAdjRibIn = "AdjRibInPolicyName";
  const std::string kPostPolicyResultAdjRibOut = "AdjRibOutPolicyName";
  const std::string kPolicyEmptyTerm = {};

  void verifyCacheState(const std::vector<std::string>& expectedState) {
    EXPECT_EQ(expectedState.size(), postPolicyResultCache_.size());
    for (auto& k : expectedState) {
      PostPolicyResultT term = std::make_shared<const std::string>(k);
      EXPECT_TRUE(postPolicyResultCache_.contains(term));
      auto it = postPolicyResultCache_.find(term);
      EXPECT_GT(it->use_count(), 1);
    }
  }

  void setAndVerify(
      AdjRibEntry& entry,
      const std::string& term,
      const std::vector<std::string>& expectedState,
      const bool ribIn = true) {
    if (ribIn) {
      entry.setPostInPolicy(term);
    } else {
      entry.setPostOutPolicy(term);
    }
    verifyCacheState(expectedState);
  }
};

TEST_F(AdjRibPostPolicyResultCacheFixture, SetPostOutPolicyTest) {
  EXPECT_TRUE(postPolicyResultCache_.empty());
  // Use one AdjRibEntry to populate postPolicyResultCache_.
  AdjRibEntry entry = AdjRibEntry(kPathId);

  entry.setPostOutPolicy(kPostPolicyResultDeniedByCrf);
  entry.setPostOutPolicy(kPolicyEmptyTerm);
  entry.setPostOutPolicy(kPostPolicyResultAdjRibOut);

  // Verify cache state and pruning.
  verifyCacheState({kPostPolicyResultAdjRibOut});
  // Verify the shared_ptr is in cache.
  EXPECT_TRUE(postPolicyResultCache_.contains(entry.getPostOutPolicy()));
}

TEST_F(AdjRibPostPolicyResultCacheFixture, SetPostInPolicyTest) {
  EXPECT_TRUE(postPolicyResultCache_.empty());
  // Use one AdjRibEntry to populate postPolicyResultCache_.
  AdjRibEntry entry = AdjRibEntry(kPathId);

  entry.setPostInPolicy(kPostPolicyResultDeniedByCrf);
  entry.setPostInPolicy(kPolicyEmptyTerm);
  entry.setPostInPolicy(kPostPolicyResultAdjRibIn);

  // Verify cache state and pruning.
  verifyCacheState({kPostPolicyResultAdjRibIn});
  // Verify the shared_ptr is in cache.
  EXPECT_TRUE(postPolicyResultCache_.contains(entry.getPostInPolicy()));
}

/*
 * We will initialize three AdjRibs entries.
 * This test checks cache state when multiple adj ribs
 * are being modified.
 */
TEST_F(AdjRibPostPolicyResultCacheFixture, MultipleAdjRibEntryTest) {
  AdjRibEntry entry1 = AdjRibEntry(0x1 /* pathId */);
  AdjRibEntry entry2 = AdjRibEntry(0x2 /* pathId */);
  AdjRibEntry entry3 = AdjRibEntry(0x3 /* pathId */);

  // T0: Empty cache
  EXPECT_TRUE(postPolicyResultCache_.empty());

  // T1: Set result on entry1 via setPostInPolicy.
  setAndVerify(
      entry1,
      kPostPolicyResultDeniedByCrf,
      // expected cache state
      {kPostPolicyResultDeniedByCrf});

  // T2: Set result on entry2 via setPostInPolicy.
  setAndVerify(
      entry2,
      kPostPolicyResultAdjRibIn,
      // expected cache state
      {kPostPolicyResultDeniedByCrf, kPostPolicyResultAdjRibIn});

  // T3: Set result on entry3 via setPostInPolicy.
  setAndVerify(
      entry3,
      kPolicyEmptyTerm,
      // expected cache state
      {kPostPolicyResultDeniedByCrf,
       kPostPolicyResultAdjRibIn,
       kPolicyEmptyTerm});

  /*
   * T4: Set new result on entry1 via setPostOutPolicy.
   * We should prune kPostPolicyResultDeniedByCrf from cache.
   */
  setAndVerify(
      entry1,
      kPolicyEmptyTerm,
      {kPostPolicyResultAdjRibIn, kPolicyEmptyTerm}, // expected cache state
      false /* ribIn */);
}

} // namespace facebook::bgp
