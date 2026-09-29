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

#include "neteng/fboss/bgp/cpp/rib/FibOut.h"

#include <algorithm>
#include <cstdint>

#include <gtest/gtest.h>

namespace facebook::bgp {
namespace {

/** Construct one primary IPv4 FIB-out nexthop for registry tests. */
FibOutNexthop makeNexthop(uint32_t address, uint32_t weight = 1) {
  return FibOutNexthop{
      .address = folly::IPAddress::fromLongHBO(address),
      .weight = weight,
      .role = FibOutNexthopRole::PRIMARY,
  };
}

TEST(FibOutTest, ReturnsSamePointerForEqualNormalizedSets) {
  const auto nexthop1 = makeNexthop(0x01000001, 10);
  const auto nexthop2 = makeNexthop(0x01000002, 20);

  FibNexthopSets nexthopSets;
  auto set1 = nexthopSets.getOrCreate({nexthop2, nexthop1, nexthop1});
  auto set2 = nexthopSets.getOrCreate({nexthop1, nexthop2});

  EXPECT_EQ(set1, set2);
  EXPECT_EQ(*set1, (FibNexthopSet{nexthop1, nexthop2}));
  EXPECT_EQ(nexthopSets.size(), 1);

  set1.reset();
  EXPECT_EQ(nexthopSets.size(), 1);
  set2.reset();
  EXPECT_EQ(nexthopSets.size(), 0);
}

TEST(FibOutTest, Shares128WaySet) {
  FibNexthopSet nexthops;
  nexthops.reserve(128);
  for (uint32_t index = 0; index < 128; ++index) {
    nexthops.push_back(makeNexthop(0x01000001 + index, index + 1));
  }

  FibNexthopSets nexthopSets;
  const auto set1 = nexthopSets.getOrCreate(nexthops);
  std::reverse(nexthops.begin(), nexthops.end());
  const auto set2 = nexthopSets.getOrCreate(nexthops);

  EXPECT_EQ(set1, set2);
  EXPECT_EQ(set1->size(), 128);
}

TEST(FibOutTest, KeepsDifferentSetsDistinct) {
  const auto nexthop1 = makeNexthop(0x01000001);
  const auto nexthop2 = makeNexthop(0x01000002);
  const auto nexthop3 = makeNexthop(0x01000003);
  FibNexthopSets nexthopSets;
  const auto set1 = nexthopSets.getOrCreate({nexthop1, nexthop2});
  const auto set2 = nexthopSets.getOrCreate({nexthop2, nexthop3});

  EXPECT_NE(set1, set2);
}

TEST(FibOutTest, ReleasesSetsAfterLastSharedPointer) {
  FibNexthopSets nexthopSets;
  std::weak_ptr<const FibNexthopSet> weakSet;
  EXPECT_EQ(nexthopSets.size(), 0);
  {
    const auto set = nexthopSets.getOrCreate({makeNexthop(0x01000001)});
    weakSet = set;
    EXPECT_EQ(nexthopSets.size(), 1);
  }

  EXPECT_TRUE(weakSet.expired());
  EXPECT_EQ(nexthopSets.size(), 0);
}

TEST(FibOutTest, SetCanOutliveRegistry) {
  std::shared_ptr<const FibNexthopSet> set;
  {
    FibNexthopSets nexthopSets;
    set = nexthopSets.getOrCreate({makeNexthop(0x01000001)});
    EXPECT_EQ(nexthopSets.size(), 1);
  }

  EXPECT_EQ(set->size(), 1);
  set.reset();
}

TEST(FibOutTest, ReleasingClearedSetDoesNotRemoveReplacement) {
  FibNexthopSets nexthopSets;
  auto oldSet = nexthopSets.getOrCreate({makeNexthop(0x01000001)});
  nexthopSets.clear();
  ASSERT_EQ(nexthopSets.size(), 0);

  auto replacement = nexthopSets.getOrCreate({makeNexthop(0x01000001)});
  ASSERT_NE(oldSet, replacement);
  ASSERT_EQ(nexthopSets.size(), 1);

  oldSet.reset();
  EXPECT_EQ(nexthopSets.size(), 1);
  replacement.reset();
  EXPECT_EQ(nexthopSets.size(), 0);
}

TEST(FibOutTest, DistinguishesMissingOptionalFields) {
  auto withMissingFields = makeNexthop(0x01000001);
  auto withValues = withMissingFields;
  withValues.interfaceName = "";
  withValues.connected = false;

  FibNexthopSets nexthopSets;
  const auto missing = nexthopSets.getOrCreate({withMissingFields});
  const auto present = nexthopSets.getOrCreate({withValues});

  EXPECT_NE(missing, present);
}

} // namespace
} // namespace facebook::bgp
