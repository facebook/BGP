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

#include <folly/coro/BlockingWait.h>
#include <folly/portability/GTest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace facebook::bgp::coro {

namespace {

struct ThrowOnMove {
  explicit ThrowOnMove(int value) : value(value) {}

  // Intentionally throwing to verify payload move failures propagate.
  // NOLINTNEXTLINE(performance-noexcept-move-constructor)
  ThrowOnMove(ThrowOnMove&& other) {
    if (throwOnMove) {
      throw std::runtime_error("move failed");
    }
    value = other.value;
  }

  ThrowOnMove& operator=(ThrowOnMove&&) = delete;

  static inline bool throwOnMove{false};
  int value;
};

struct MoveConstructibleOnly {
  int latest;
  bool sticky;

  MoveConstructibleOnly(int latest, bool sticky)
      : latest(latest), sticky(sticky) {}
  MoveConstructibleOnly(MoveConstructibleOnly&&) = default;
  MoveConstructibleOnly& operator=(MoveConstructibleOnly&&) = delete;
};

struct KeyAliasingPayload {
  int key;
  int value;

  KeyAliasingPayload(int key, int value) : key(key), value(value) {}
  KeyAliasingPayload(KeyAliasingPayload&& other) noexcept
      : key(std::exchange(other.key, -1)), value(other.value) {}
  KeyAliasingPayload& operator=(KeyAliasingPayload&& other) noexcept {
    key = std::exchange(other.key, -1);
    value = other.value;
    return *this;
  }
};

struct NonTrivialKeyAliasingPayload {
  std::string key;
  int value;

  NonTrivialKeyAliasingPayload(std::string key, int value)
      : key(std::move(key)), value(value) {}
  NonTrivialKeyAliasingPayload(NonTrivialKeyAliasingPayload&&) noexcept =
      default;
  NonTrivialKeyAliasingPayload& operator=(NonTrivialKeyAliasingPayload&&) =
      delete;
};

struct ReentrantDestructionState {
  std::atomic<bool> armed{false};
  std::atomic<size_t> calls{0};
  std::function<void()> callback;
};

struct ReentrantDestructionPayload {
  int value;
  std::shared_ptr<ReentrantDestructionState> state;
  ReentrantDestructionState* probeState;

  explicit ReentrantDestructionPayload(
      int value,
      std::shared_ptr<ReentrantDestructionState> state = nullptr)
      : value(value), state(std::move(state)), probeState(this->state.get()) {}
  ReentrantDestructionPayload(ReentrantDestructionPayload&& other) noexcept
      : value(other.value),
        state(std::move(other.state)),
        probeState(other.probeState) {}
  ReentrantDestructionPayload& operator=(ReentrantDestructionPayload&&) =
      delete;
  ~ReentrantDestructionPayload() {
    if (probeState && probeState->armed.load(std::memory_order_acquire)) {
      probeState->callback();
      probeState->calls.fetch_add(1, std::memory_order_relaxed);
    }
  }
};

struct ConcurrentValue {
  uint64_t latest;
  bool sticky;
  std::string payload;
};

// Pop one item synchronously (the queue already holds a wakeup for it).
template <typename T, typename Key>
T popNow(MergeQueue<T, Key>& q) {
  return folly::coro::blockingWait(q.pop());
}

} // namespace

TEST(MergeQueueTest, GenericKeyKeepsLatest) {
  MergeQueue<int, std::string> q;
  q.pushMerge(10, "prefix-a");
  q.pushMerge(20, "prefix-b");
  q.pushMerge(30, "prefix-a");

  EXPECT_EQ(q.size(), 2);
  EXPECT_EQ(popNow(q), 30);
  EXPECT_EQ(popNow(q), 20);
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, CustomMergePreservesStickyMetadata) {
  struct Value {
    int latest;
    bool sticky;
  };
  MergeQueue<Value> q;
  q.pushMerge(Value{.latest = 1, .sticky = true}, /*key=*/0);
  q.pushMergeWith(
      Value{.latest = 2, .sticky = false},
      /*key=*/0,
      [](Value& incoming, const Value& pending) noexcept {
        incoming.sticky |= pending.sticky;
      });

  const auto result = popNow(q);
  EXPECT_EQ(result.latest, 2);
  EXPECT_TRUE(result.sticky);
}

TEST(MergeQueueTest, CustomMergeSupportsNonMoveAssignablePayload) {
  MergeQueue<MoveConstructibleOnly> q;
  const auto preserveSticky =
      [](MoveConstructibleOnly& incoming,
         const MoveConstructibleOnly& pending) noexcept {
        incoming.sticky |= pending.sticky;
      };

  EXPECT_FALSE(q.pushMerge(MoveConstructibleOnly{1, true}, /*key=*/0));
  EXPECT_FALSE(q.pushMerge(MoveConstructibleOnly{10, false}, /*key=*/1));
  EXPECT_TRUE(q.pushMergeWith(
      MoveConstructibleOnly{2, false}, /*key=*/0, preserveSticky));
  EXPECT_TRUE(q.pushMergeWith(
      MoveConstructibleOnly{3, false}, /*key=*/0, preserveSticky));

  EXPECT_EQ(q.size(), 2);
  auto merged = popNow(q);
  EXPECT_EQ(merged.latest, 3);
  EXPECT_TRUE(merged.sticky);
  auto second = popNow(q);
  EXPECT_EQ(second.latest, 10);
  EXPECT_FALSE(second.sticky);
}

TEST(MergeQueueTest, AppendIndexesCopiedKeyWhenKeyAliasesPayload) {
  MergeQueue<KeyAliasingPayload> q;
  KeyAliasingPayload first{7, 1};
  const auto& aliasedKey = first.key;

  EXPECT_FALSE(q.pushMerge(std::move(first), aliasedKey));
  EXPECT_TRUE(q.pushMerge(KeyAliasingPayload{7, 2}, /*key=*/7));

  EXPECT_EQ(q.size(), 1);
  const auto result = popNow(q);
  EXPECT_EQ(result.key, 7);
  EXPECT_EQ(result.value, 2);
}

TEST(MergeQueueTest, SplicePathIndexesCopiedKeyWhenKeyAliasesPayload) {
  MergeQueue<NonTrivialKeyAliasingPayload, std::string> q;
  NonTrivialKeyAliasingPayload first{"prefix", 1};
  const auto& aliasedKey = first.key;

  EXPECT_FALSE(q.pushMerge(std::move(first), aliasedKey));
  EXPECT_TRUE(q.pushMerge(NonTrivialKeyAliasingPayload{"prefix", 2}, "prefix"));

  EXPECT_EQ(q.size(), 1);
  const auto result = popNow(q);
  EXPECT_EQ(result.key, "prefix");
  EXPECT_EQ(result.value, 2);
}

TEST(MergeQueueTest, TryPopReturnsLatestThenEmpty) {
  MergeQueue<int, std::string> q;
  EXPECT_EQ(q.tryPop(), std::nullopt);
  q.pushMerge(10, "prefix");
  q.pushMerge(20, "prefix");

  EXPECT_EQ(q.tryPop(), std::optional<int>{20});
  EXPECT_EQ(q.tryPop(), std::nullopt);
}

TEST(MergeQueueTest, TryPopPreservesFifoAndAllowsKeyReuse) {
  MergeQueue<int, std::string> q;
  q.pushMerge(10, "prefix-a");
  q.pushMerge(20, "prefix-b");

  EXPECT_EQ(q.tryPop(), std::optional<int>{10});
  EXPECT_FALSE(q.pushMerge(30, "prefix-a"));
  EXPECT_EQ(q.tryPop(), std::optional<int>{20});
  EXPECT_EQ(q.tryPop(), std::optional<int>{30});
  EXPECT_EQ(q.tryPop(), std::nullopt);
}

TEST(MergeQueueTest, TryPopPropagatesPayloadMoveException) {
  MergeQueue<ThrowOnMove> q;
  q.pushMerge(ThrowOnMove{10}, /*key=*/0);

  ThrowOnMove::throwOnMove = true;
  EXPECT_THROW(q.tryPop(), std::runtime_error);
  ThrowOnMove::throwOnMove = false;
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, PopPropagatesPayloadMoveExceptionAndQueueRemainsUsable) {
  MergeQueue<ThrowOnMove> q;
  q.pushMerge(ThrowOnMove{10}, /*key=*/0);

  ThrowOnMove::throwOnMove = true;
  EXPECT_THROW(popNow(q), std::runtime_error);
  ThrowOnMove::throwOnMove = false;

  EXPECT_TRUE(q.empty());
  q.pushMerge(ThrowOnMove{20}, /*key=*/0);
  EXPECT_EQ(popNow(q).value, 20);
  EXPECT_EQ(q.tryPop(), std::nullopt);
}

TEST(MergeQueueTest, PushMergeWithSkipsCallbackWhenAppending) {
  MergeQueue<int> q;
  bool callbackCalled = false;

  EXPECT_FALSE(q.pushMergeWith(
      1,
      /*key=*/0,
      [&](int&, const int&) noexcept { callbackCalled = true; }));
  EXPECT_FALSE(callbackCalled);
  EXPECT_EQ(popNow(q), 1);
}

TEST(MergeQueueTest, NegativeIntegerKeyIsNotReserved) {
  MergeQueue<int> q;

  EXPECT_FALSE(q.pushMerge(1, /*key=*/-1));
  EXPECT_TRUE(q.pushMerge(2, /*key=*/-1));
  EXPECT_EQ(popNow(q), 2);
}

TEST(MergeQueueTest, ConsecutivePurgeAllKeepsLatest) {
  MergeQueue<int> q;
  q.pushPurgeAll(1);
  q.pushPurgeAll(2);

  EXPECT_EQ(q.size(), 1);
  EXPECT_EQ(popNow(q), 2);
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, TryPopReturnsOnlySurvivingPurgeValue) {
  MergeQueue<int> q;
  q.pushMerge(10, /*key=*/0);
  q.pushMerge(20, /*key=*/1);
  q.pushPurgeAll(99);

  EXPECT_EQ(q.tryPop(), std::optional<int>{99});
  EXPECT_EQ(q.tryPop(), std::nullopt);
  q.pushMerge(42, /*key=*/0);
  EXPECT_EQ(q.tryPop(), std::optional<int>{42});
  EXPECT_TRUE(q.empty());
}

TEST(MergeQueueTest, PayloadDestructionCanReenterQueue) {
  {
    MergeQueue<ReentrantDestructionPayload> q;
    auto state = std::make_shared<ReentrantDestructionState>();
    state->callback = [&] { (void)q.size(); };
    q.pushMerge(ReentrantDestructionPayload{1, state}, /*key=*/0);
    state->armed.store(true, std::memory_order_release);

    q.pushMerge(ReentrantDestructionPayload{2}, /*key=*/0);

    EXPECT_GT(state->calls.load(std::memory_order_relaxed), 0);
    state->armed.store(false, std::memory_order_release);
  }

  {
    MergeQueue<ReentrantDestructionPayload> q;
    auto state = std::make_shared<ReentrantDestructionState>();
    state->callback = [&] { (void)q.size(); };
    q.pushMerge(ReentrantDestructionPayload{1, state}, /*key=*/0);
    state->armed.store(true, std::memory_order_release);

    q.pushPurgeAll(ReentrantDestructionPayload{2});

    EXPECT_GT(state->calls.load(std::memory_order_relaxed), 0);
    state->armed.store(false, std::memory_order_release);
  }

  {
    MergeQueue<ReentrantDestructionPayload> q;
    auto state = std::make_shared<ReentrantDestructionState>();
    state->callback = [&] { (void)q.size(); };
    q.pushMerge(ReentrantDestructionPayload{1, state}, /*key=*/0);
    state->armed.store(true, std::memory_order_release);

    auto value = q.tryPop();

    EXPECT_TRUE(value.has_value());
    EXPECT_GT(state->calls.load(std::memory_order_relaxed), 0);
    state->armed.store(false, std::memory_order_release);
  }
}

TEST(MergeQueueTest, ConcurrentCustomMergePurgeAndTryPopRemainLive) {
  MergeQueue<ConcurrentValue, std::string> q;
  constexpr int kProducers = 4;
  constexpr int kUpdatesPerProducer = 2000;
  constexpr uint64_t kFinalValue = std::numeric_limits<uint64_t>::max();
  const std::array<std::string, 4> keys{
      "prefix-a", "prefix-b", "prefix-c", "prefix-d"};

  std::atomic<bool> sawFinal{false};
  std::thread consumer([&]() {
    while (!sawFinal.load(std::memory_order_acquire)) {
      auto value = q.tryPop();
      if (!value.has_value()) {
        std::this_thread::yield();
        continue;
      }
      if (value->latest == kFinalValue) {
        sawFinal.store(true, std::memory_order_release);
      }
    }
  });

  std::vector<std::thread> producers;
  producers.reserve(kProducers);
  for (int producer = 0; producer < kProducers; ++producer) {
    producers.emplace_back([&, producer]() {
      for (int update = 0; update < kUpdatesPerProducer; ++update) {
        q.pushMergeWith(
            ConcurrentValue{
                .latest = static_cast<uint64_t>(
                    producer * kUpdatesPerProducer + update),
                .sticky = update == 0,
                .payload = "payload",
            },
            keys[update % keys.size()],
            [](ConcurrentValue& incoming,
               const ConcurrentValue& pending) noexcept {
              incoming.sticky |= pending.sticky;
            });
      }
    });
  }
  std::thread purger([&]() {
    for (int purge = 0; purge < 500; ++purge) {
      q.pushPurgeAll(
          ConcurrentValue{
              .latest = static_cast<uint64_t>(purge),
              .sticky = false,
              .payload = "purge",
          });
    }
  });

  for (auto& producer : producers) {
    producer.join();
  }
  purger.join();
  q.pushPurgeAll(
      ConcurrentValue{
          .latest = kFinalValue,
          .sticky = false,
          .payload = "final",
      });
  consumer.join();

  EXPECT_TRUE(sawFinal.load(std::memory_order_acquire));
  EXPECT_TRUE(q.empty());
}

} // namespace facebook::bgp::coro
