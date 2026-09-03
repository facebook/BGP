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

#include "neteng/fboss/bgp/cpp/lib/coro/MergeQueue.h"

#include <folly/CancellationToken.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/WithCancellation.h>
#include <folly/executors/ManualExecutor.h>
#include <folly/portability/GTest.h>

#include <array>
#include <atomic>
#include <list>
#include <optional>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace facebook::bgp::coro {

namespace {

// Pop one item synchronously (the queue already holds a wakeup for it).
template <typename T, typename Key>
T popNow(MergeQueue<T, Key>& q) {
  return folly::coro::blockingWait(q.pop());
}

} // namespace

TEST(MergeQueueTest, FifoDistinctSlots) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  q.pushMerge(20, /*key=*/1);
  q.pushMerge(30, /*key=*/2);
  EXPECT_EQ(q.size(), 3);
  EXPECT_EQ(popNow(q), 10);
  EXPECT_EQ(popNow(q), 20);
  EXPECT_EQ(popNow(q), 30);
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, MergeSameSlotKeepsLatest) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  q.pushMerge(20, /*key=*/0);
  q.pushMerge(30, /*key=*/0);
  EXPECT_EQ(q.size(), 1);
  EXPECT_EQ(popNow(q), 30);
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, MergePreservesOrder) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0); // slot 0 at position 0
  q.pushMerge(20, /*key=*/1); // slot 1 at position 1
  q.pushMerge(30, /*key=*/0); // merges into slot 0, keeps position 0
  EXPECT_EQ(q.size(), 2);
  EXPECT_EQ(popNow(q), 30); // slot 0 retained its earlier position
  EXPECT_EQ(popNow(q), 20);
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, MergeAfterPopAppendsFresh) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  EXPECT_EQ(popNow(q), 10);
  // slot 0's node was consumed, so this is a fresh append, not a merge.
  q.pushMerge(20, /*key=*/0);
  EXPECT_EQ(q.size(), 1);
  EXPECT_EQ(popNow(q), 20);
}

TEST(MergeQueueTest, PushMergeReturnsWhetherCoalesced) {
  MergeQueue<int> q;
  EXPECT_FALSE(q.pushMerge(1, /*key=*/0)); // new slot -> appended
  EXPECT_TRUE(q.pushMerge(2, /*key=*/0)); // same slot -> coalesced
  EXPECT_FALSE(q.pushMerge(3, /*key=*/1)); // different slot -> appended
  EXPECT_EQ(q.size(), 2);
  // Once slot 0's node is consumed, the next push to slot 0 appends afresh.
  EXPECT_EQ(popNow(q), 2);
  EXPECT_FALSE(q.pushMerge(4, /*key=*/0));
}

TEST(MergeQueueTest, SecondPushSupersedesFirst) {
  // A value immediately followed by another in the same slot collapses to the
  // latest, so the consumer never observes the superseded intermediate.
  MergeQueue<int> q;
  constexpr int kSlot = 0;
  q.pushMerge(1, kSlot);
  q.pushMerge(2, kSlot);
  EXPECT_EQ(q.size(), 1);
  EXPECT_EQ(popNow(q), 2);
}

TEST(MergeQueueTest, PurgeAllDropsPending) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  q.pushMerge(20, /*key=*/1);
  q.pushMerge(30, /*key=*/2);
  q.pushPurgeAll(99);
  EXPECT_EQ(q.size(), 1);
  EXPECT_EQ(popNow(q), 99);
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, PurgeAllThenMergeIsSeparate) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  q.pushPurgeAll(99);
  // A slot push after the purge starts a fresh node after the purge entry.
  q.pushMerge(20, /*key=*/0);
  EXPECT_EQ(q.size(), 2);
  EXPECT_EQ(popNow(q), 99);
  EXPECT_EQ(popNow(q), 20);
}

TEST(MergeQueueTest, PurgeAllClearsEverySlotThenReMerges) {
  MergeQueue<int> q;
  q.pushMerge(1, /*key=*/0);
  q.pushMerge(2, /*key=*/1);
  q.pushMerge(3, /*key=*/2);
  q.pushMerge(4, /*key=*/3); // a fourth distinct slot
  q.pushPurgeAll(0);
  q.pushMerge(5, /*key=*/0);
  q.pushMerge(6, /*key=*/0); // merges into the post-purge slot-0 node
  EXPECT_EQ(q.size(), 2); // [purge(0), slot0(6)]
  EXPECT_EQ(popNow(q), 0);
  EXPECT_EQ(popNow(q), 6);
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, PurgeAllThenFreshPushIsDelivered) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  q.pushMerge(20, /*key=*/1);
  q.pushPurgeAll(99);
  EXPECT_EQ(q.size(), 1);
  EXPECT_EQ(popNow(q), 99);
  EXPECT_TRUE(q.empty());
  q.pushMerge(42, /*key=*/0);
  EXPECT_EQ(popNow(q), 42);
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, PopBlocksUntilPush) {
  MergeQueue<int> q;
  folly::ManualExecutor executor;
  auto result = folly::coro::co_withExecutor(&executor, q.pop()).start();

  executor.drain();
  ASSERT_FALSE(result.isReady());

  q.pushMerge(42, /*key=*/0);
  executor.drain();

  ASSERT_TRUE(result.isReady());
  EXPECT_EQ(std::move(result).get(), 42);
}

TEST(MergeQueueTest, BlockedPopWakesOnPurgeAll) {
  MergeQueue<int> q;
  folly::ManualExecutor executor;
  auto result = folly::coro::co_withExecutor(&executor, q.pop()).start();

  executor.drain();
  ASSERT_FALSE(result.isReady());

  q.pushPurgeAll(99);
  executor.drain();

  ASSERT_TRUE(result.isReady());
  EXPECT_EQ(std::move(result).get(), 99);
}

TEST(MergeQueueTest, PopReblocksAfterNonblockingDrain) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  q.pushMerge(20, /*key=*/1);
  EXPECT_EQ(q.tryPop(), std::optional<int>{10});
  EXPECT_EQ(q.tryPop(), std::optional<int>{20});

  folly::ManualExecutor executor;
  auto result = folly::coro::co_withExecutor(&executor, q.pop()).start();
  executor.drain();
  ASSERT_FALSE(result.isReady());

  q.pushMerge(30, /*key=*/2);
  executor.drain();

  ASSERT_TRUE(result.isReady());
  EXPECT_EQ(std::move(result).get(), 30);
}

TEST(MergeQueueTest, PopReblocksAfterCoalescedUpdatesAreDrained) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  q.pushMerge(20, /*key=*/0);
  q.pushMerge(30, /*key=*/0);
  q.pushPurgeAll(40);
  q.pushPurgeAll(50);
  EXPECT_EQ(q.tryPop(), std::optional<int>{50});

  folly::ManualExecutor executor;
  auto result = folly::coro::co_withExecutor(&executor, q.pop()).start();
  executor.drain();
  ASSERT_FALSE(result.isReady());

  q.pushMerge(60, /*key=*/1);
  executor.drain();

  ASSERT_TRUE(result.isReady());
  EXPECT_EQ(std::move(result).get(), 60);
}

TEST(MergeQueueTest, CancellationThrows) {
  MergeQueue<int> q;
  folly::CancellationSource cs;
  cs.requestCancellation();
  EXPECT_THROW(
      folly::coro::blockingWait(
          folly::coro::co_withCancellation(cs.getToken(), q.pop())),
      folly::OperationCancelled);
}

TEST(MergeQueueTest, BlockedPopCancellationThrows) {
  MergeQueue<int> q;
  folly::ManualExecutor executor;
  folly::CancellationSource cs;
  auto result =
      folly::coro::co_withExecutor(
          &executor, folly::coro::co_withCancellation(cs.getToken(), q.pop()))
          .start();

  executor.drain();
  ASSERT_FALSE(result.isReady());

  cs.requestCancellation();
  executor.drain();

  ASSERT_TRUE(result.isReady());
  EXPECT_THROW(std::move(result).get(), folly::OperationCancelled);

  q.pushMerge(42, /*key=*/0);
  EXPECT_EQ(popNow(q), 42);
}

/* Random push/purge sequence checked against an independent reference model. */
TEST(MergeQueueTest, RandomSequenceMatchesReference) {
  constexpr int kNumSlots = 4;
  MergeQueue<int> q;

  // Reference model: an ordered list of values with per-slot in-place merge and
  // purge-clears-all, matching the queue's documented contract.
  std::list<int> refOrder;
  std::array<std::optional<std::list<int>::iterator>, kNumSlots> refIters{};

  std::mt19937 rng(0xC0FFEE);
  int value = 0;
  for (int i = 0; i < 5000; ++i) {
    const auto operation = rng() % 20;
    if (operation == 0) {
      q.pushPurgeAll(value);
      refOrder.clear();
      refIters = {};
      refOrder.push_back(value);
    } else {
      const int slot = static_cast<int>(rng() % kNumSlots);
      q.pushMerge(value, slot);
      if (refIters[slot].has_value()) {
        **refIters[slot] = value;
      } else {
        refOrder.push_back(value);
        refIters[slot] = std::prev(refOrder.end());
      }
    }
    ++value;
  }

  std::vector<int> got;
  while (!q.empty()) {
    got.push_back(popNow(q));
  }
  const std::vector<int> want(refOrder.begin(), refOrder.end());
  EXPECT_EQ(got, want);
}

// Many producers + a single consumer; run under TSAN to catch data races.
TEST(MergeQueueTest, MultiProducerSingleConsumerNoRace) {
  MergeQueue<int> q;
  constexpr int kProducers = 8;
  constexpr int kPerProducer = 2000;
  constexpr int kTotal = kProducers * kPerProducer;

  // Each (producer, i) uses a distinct slot and value, so nothing merges and
  // exactly kTotal items flow through -- lets us assert an exact permutation.
  std::vector<char> seen(kTotal, 0);
  std::thread consumer([&]() {
    for (int i = 0; i < kTotal; ++i) {
      int v = folly::coro::blockingWait(q.pop());
      ASSERT_GE(v, 0);
      ASSERT_LT(v, kTotal);
      EXPECT_EQ(seen[v], 0);
      seen[v] = 1;
    }
  });

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p]() {
      for (int i = 0; i < kPerProducer; ++i) {
        const int v = p * kPerProducer + i;
        q.pushMerge(v, /*key=*/v);
      }
    });
  }

  for (auto& t : producers) {
    t.join();
  }
  consumer.join();

  EXPECT_TRUE(q.empty());
  for (int v = 0; v < kTotal; ++v) {
    EXPECT_EQ(seen[v], 1) << "missing value " << v;
  }
}

// Many producers merging concurrently into a small shared slot set (TSAN target
// for concurrent merge). After they finish, at most one node per slot can
// remain, so the drained count is bounded by the slot count.
TEST(MergeQueueTest, ConcurrentMergesBoundedBySlots) {
  MergeQueue<int> q;
  constexpr int kProducers = 8;
  constexpr int kPerProducer = 5000;
  constexpr int kSlots = 4;

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&, p]() {
      std::mt19937 rng(static_cast<unsigned>(p) + 1);
      for (int i = 0; i < kPerProducer; ++i) {
        q.pushMerge(static_cast<int>(rng()), static_cast<int>(rng() % kSlots));
      }
    });
  }
  for (auto& t : producers) {
    t.join();
  }

  size_t drained = 0;
  while (!q.empty()) {
    (void)folly::coro::blockingWait(q.pop());
    ++drained;
  }
  EXPECT_TRUE(q.empty());
  EXPECT_GE(drained, 1u);
  EXPECT_LE(drained, static_cast<size_t>(kSlots));
}

} // namespace facebook::bgp::coro
