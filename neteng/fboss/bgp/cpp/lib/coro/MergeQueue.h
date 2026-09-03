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

#pragma once

#include <list>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/fibers/Semaphore.h>
#include <folly/synchronization/MicroSpinLock.h>

namespace facebook::bgp::coro {

/**
 * An unbounded, coalescing (merge) queue for a single consumer.
 *
 * Every mergeable item is enqueued under a caller-supplied key. Pushing an item
 * whose key already has a pending (not-yet-popped) item overwrites that item
 * in place. The queue therefore holds at most one pending item per key --
 * always the latest -- while preserving FIFO order relative to items with
 * other keys. Overwriting in place is correct only because a keyed item affects
 * solely its own key, so its position relative to other keys is immaterial.
 *
 * `pushPurgeAll` is the whole-queue merge: it coalesces every pending item into
 * `val` (e.g. a "clear everything" message that supersedes all queued items).
 * Such an item affects every slot, so its FIFO position IS significant and it
 * cannot overwrite in place -- it drops all predecessors and lands at the tail,
 * ordered after everything it supersedes. Consecutive pushPurgeAll thus
 * collapse too: the second drops the first.
 *
 * Concurrency: designed for a single consumer (one `pop()` loop). Multiple
 * producers are safe -- all mutation is under a short-held spin lock. Every
 * newly appended node signals the semaphore once; an in-place merge adds no
 * signal. A purge can remove nodes whose signals are still pending, so
 * consumers skip those stale credits if the queue is already empty.
 *
 * `pushMerge`/`pushMergeWith`/`pushPurgeAll` are noexcept: an allocation
 * failure terminates the process rather than propagating, matching bgpd's
 * crash-on-OOM policy (and the MPMCQueue this replaced).
 */
template <typename T, typename Key = int>
class MergeQueue {
 public:
  MergeQueue() = default;
  ~MergeQueue() = default;

  /*
   * Non-copyable and non-movable: owns a Semaphore and a MicroSpinLock,
   * neither of which is movable.
   */
  MergeQueue(const MergeQueue&) = delete;
  MergeQueue& operator=(const MergeQueue&) = delete;
  MergeQueue(MergeQueue&&) = delete;
  MergeQueue& operator=(MergeQueue&&) = delete;

  /*
   * Enqueue `val` under `key`. If an item for `key` is already pending in the
   * queue, overwrite it in place; otherwise append it and signal the consumer.
   * Returns true if it coalesced into an existing pending item, false if it
   * appended a new one -- lets callers count coalescing.
   */
  template <typename U = T>
  bool pushMerge(U&& val, const Key& key) noexcept {
    return pushMergeWith(
        std::forward<U>(val),
        key,
        [](T& /*incoming*/, const T& /*pending*/) noexcept {});
  }

  /*
   * As pushMerge, but invoke `merge(incoming, pending)` before replacing an
   * existing node. The callback runs under the queue lock and must be short and
   * noexcept; use it only to preserve lightweight sticky metadata.
   */
  template <typename U = T, typename Merge>
  bool pushMergeWith(U&& val, const Key& key, Merge&& merge) noexcept {
    static_assert(
        std::is_nothrow_invocable_v<Merge&, T&, const T&>,
        "merge callback must be noexcept");
    if constexpr (
        std::is_move_assignable_v<T> && std::is_trivially_destructible_v<T>) {
      return updatePendingOrAppend(
          key,
          [&](QueueIterator& pendingIt) {
            T incoming(std::forward<U>(val));
            merge(incoming, pendingIt->val);
            pendingIt->val = std::move(incoming);
          },
          [&]() {
            queue_.push_back(makeKeyedNode(key, std::forward<U>(val)));
            return std::prev(queue_.end());
          });
    } else {
      std::list<Node> incoming;
      incoming.push_back(makeKeyedNode(key, std::forward<U>(val)));
      const auto& ownedKey = nodeKey(incoming.front());

      std::list<Node> retired;
      return updatePendingOrAppend(
          ownedKey,
          [&](QueueIterator& pendingIt) {
            /*
             * Replace the pending node in place, preserving its FIFO position.
             * Splicing lets T be move-constructible but not move-assignable and
             * defers destruction of the superseded value until after unlocking.
             */
            const auto newIt = incoming.begin();
            merge(newIt->val, pendingIt->val);
            queue_.splice(pendingIt, incoming, newIt);
            retired.splice(retired.end(), queue_, pendingIt);
            pendingIt = newIt;
          },
          [&]() {
            const auto newIt = incoming.begin();
            queue_.splice(queue_.end(), incoming, newIt);
            return newIt;
          });
    }
  }

  /*
   * Whole-queue merge: coalesce every pending item into `val`, which lands at
   * the tail. Use when `val` supersedes everything already queued. Unlike a
   * slotted merge, a supersede-all item cannot overwrite in place -- its FIFO
   * position matters, so it must be ordered after all the items it drops.
   */
  template <typename U = T>
  void pushPurgeAll(U&& val) noexcept {
    std::list<Node> incoming;
    incoming.push_back(makeUnkeyedNode(std::forward<U>(val)));
    std::list<Node> retiredQueue;
    KeyMap retiredKeyStates;
    {
      std::lock_guard<folly::MicroSpinLock> guard(lock_);
      retiredQueue.swap(queue_);
      retiredKeyStates.swap(keyIters_);
      queue_.splice(queue_.end(), incoming, incoming.begin());
    }
    sem_.signal();
  }

  // Cancellable async pop; returns items in FIFO order.
  folly::coro::Task<T> pop() {
    while (true) {
      folly::Try<void> result =
          co_await folly::coro::co_awaitTry(sem_.co_wait());
      if (result.hasException()) {
        co_yield folly::coro::co_error(std::move(result).exception());
      }
      if (auto val = dequeue()) {
        co_return std::move(*val);
      }
    }
  }

  /* Nonblocking pop; returns nullopt if no item is immediately available. */
  std::optional<T> tryPop() {
    while (sem_.try_wait()) {
      if (auto val = dequeue()) {
        return val;
      }
    }
    return std::nullopt;
  }

  size_t size() const noexcept {
    std::lock_guard<folly::MicroSpinLock> guard(lock_);
    return queue_.size();
  }

  bool empty() const noexcept {
    std::lock_guard<folly::MicroSpinLock> guard(lock_);
    return queue_.empty();
  }

 private:
  struct Node {
    std::optional<Key> key;
    T val;
  };

  template <typename U>
  static Node makeKeyedNode(const Key& key, U&& val) {
    return Node{key, std::forward<U>(val)};
  }

  template <typename U>
  static Node makeUnkeyedNode(U&& val) {
    return Node{std::nullopt, std::forward<U>(val)};
  }

  static bool nodeHasKey(const Node& node) {
    return node.key.has_value();
  }

  static const Key& nodeKey(const Node& node) {
    return *node.key;
  }

  using Queue = std::list<Node>;
  using QueueIterator = typename Queue::iterator;

  using KeyMap = folly::F14FastMap<Key, QueueIterator>;

  template <typename ReplacePending, typename AppendNew>
  bool updatePendingOrAppend(
      const Key& key,
      ReplacePending&& replacePending,
      AppendNew&& appendNew) noexcept {
    bool coalesced = false;
    {
      std::lock_guard<folly::MicroSpinLock> guard(lock_);
      auto it = keyIters_.find(key);
      if (it != keyIters_.end()) {
        replacePending(it->second);
        coalesced = true;
      } else {
        const auto newIt = appendNew();
        keyIters_.emplace(nodeKey(*newIt), newIt);
      }
    }
    if (!coalesced) {
      sem_.signal();
    }
    return coalesced;
  }

  std::optional<T> dequeue() {
    std::list<Node> popped;
    {
      std::lock_guard<folly::MicroSpinLock> guard(lock_);
      if (queue_.empty()) {
        return std::nullopt;
      }
      const auto frontIt = queue_.begin();
      if (nodeHasKey(*frontIt)) {
        auto it = keyIters_.find(nodeKey(*frontIt));
        if (it != keyIters_.end() && it->second == frontIt) {
          keyIters_.erase(it);
        }
      }
      popped.splice(popped.end(), queue_, frontIt);
    }
    return std::move(popped.front().val);
  }

  mutable folly::MicroSpinLock lock_{};
  Queue queue_;
  KeyMap keyIters_;
  folly::fibers::Semaphore sem_{0};
};

} // namespace facebook::bgp::coro
