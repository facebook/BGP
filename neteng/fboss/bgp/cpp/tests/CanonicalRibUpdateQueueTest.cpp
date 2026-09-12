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

#include "neteng/fboss/bgp/cpp/fsdb/CanonicalRibUpdateQueue.h"

#include <gtest/gtest.h>

#include <utility>

namespace facebook::bgp {
namespace {

CanonicalRibPrefixUpdate prefixUpdate(
    const folly::CIDRNetwork& prefix,
    bool pathSelectionPending) {
  CanonicalRibEntryInput entryInput;
  entryInput.fields.pathSelectionPending = pathSelectionPending;
  return CanonicalRibPrefixUpdate{
      .prefix = prefix,
      .entry = std::move(entryInput),
  };
}

TEST(CanonicalRibUpdateQueueTest, PrefixMergeKeepsLatestState) {
  CanonicalRibUpdateQueue queue;
  const auto prefix = folly::IPAddress::createNetwork("10.0.0.0/24");

  EXPECT_FALSE(queue.pushPrefixUpdate(prefixUpdate(prefix, false)));
  EXPECT_TRUE(queue.pushPrefixUpdate(prefixUpdate(prefix, true)));
  EXPECT_EQ(queue.size(), 1);

  const auto update = queue.tryPop();
  ASSERT_TRUE(update.has_value());
  const auto& prefixState = std::get<CanonicalRibPrefixUpdate>(*update);
  ASSERT_TRUE(prefixState.entry.has_value());
  EXPECT_EQ(prefixState.prefix, prefix);
  EXPECT_EQ(prefixState.entry->fields.pathSelectionPending, true);
  EXPECT_TRUE(queue.empty());
}

TEST(CanonicalRibUpdateQueueTest, FullSnapshotSupersedesPendingUpdates) {
  CanonicalRibUpdateQueue queue;
  const auto firstPrefix = folly::IPAddress::createNetwork("10.0.0.0/24");
  const auto secondPrefix = folly::IPAddress::createNetwork("10.0.1.0/24");
  CanonicalRibFullSnapshotInput queuedSnapshot;
  queuedSnapshot.entries.push_back(prefixUpdate(secondPrefix, true));

  queue.pushPrefixUpdate(prefixUpdate(firstPrefix, false));
  queue.pushPrefixUpdate(prefixUpdate(secondPrefix, false));
  queue.pushFullSnapshot(std::move(queuedSnapshot));

  ASSERT_EQ(queue.size(), 1);
  const auto update = queue.tryPop();
  ASSERT_TRUE(update.has_value());
  const auto& poppedSnapshot = std::get<CanonicalRibFullSnapshotInput>(*update);
  ASSERT_EQ(poppedSnapshot.entries.size(), 1);
  EXPECT_EQ(poppedSnapshot.entries.front().prefix, secondPrefix);
  EXPECT_TRUE(queue.empty());
}

} // namespace
} // namespace facebook::bgp
