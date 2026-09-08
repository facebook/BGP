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

#include <folly/coro/Task.h>
#include <folly/fibers/Semaphore.h>
#include <folly/logging/xlog.h>

namespace facebook::nettools::bgplib {

/**
 * Bounded hysteresis queue with locks, inspired from
 * centralium::coro::MPMCQueue.
 *
 * Queue maintains internal state bool blocked_.
 *  - If queue was not blocked and its size exceeds hiWm, then the
 *    queue state is set to blocked.
 *  - If queue was blocked and its size decreases below loWm, then the
 *    queue state is set to unblocked.
 *
 * This state is exposed through the public isBlocked method.
 *
 * For an SPSC scenario, this means once the producer pushes #highWm number
 * of objects to the queue, the consumer must consume enough objects
 * until queue size is below #lowWm before the queue becomes 'unblocked'.
 *
 * If the queue size is at capacity, the push call can fail. Producer
 * would need to check if the queue is blocked (or full) before pushing.
 *
 * Producers that choose to wait for the queue to be unblocked
 * can explicitly wait on a semaphore that will signal once the queue becomes
 * unblocked. Producers should ALWAYS CHECK isBlocked() first before waiting.
 */
template <typename T>
class MPMCWatermarkQueue {
 public:
  MPMCWatermarkQueue(const int capacity, const int highWm, const int lowWm)
      : capacity_(capacity), hiWm_(highWm), loWm_(lowWm) {
    CHECK_GT(capacity_, hiWm_);
    CHECK_GT(hiWm_, loWm_);
  }
  ~MPMCWatermarkQueue() {
    close();
  }
  // Allow move constructor and move assignment
  MPMCWatermarkQueue(MPMCWatermarkQueue&&) noexcept = default;
  MPMCWatermarkQueue& operator=(MPMCWatermarkQueue&&) noexcept = default;
  // Delete copy constructor and copy assignment
  MPMCWatermarkQueue(MPMCWatermarkQueue const&) = delete;
  MPMCWatermarkQueue& operator=(MPMCWatermarkQueue const&) = delete;

  /**
   * @brief Push an item to the queue. Return true if succeeded or false
   * if failed.
   *
   * @details: During push, the queue can enter blocked state if the queue
   * size breaches the high watermark. If the queue is at capacity or closed,
   * then the push call will fail.
   */
  template <typename U = T>
  bool push(U&& val) noexcept {
    // Early exit if closed
    if (closed_.load(std::memory_order_acquire)) {
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.size() >= capacity_) {
        return false;
      }
      queue_.emplace_back(std::forward<T>(val));
      if (!blocked_ && queue_.size() >= hiWm_) {
        blocked_ = true;
      }
    }
    readSem_.signal();
    return true;
  }

  /**
   * Cancellable async coro pop. Waits for elements to be available
   * in the queue.
   */
  folly::coro::Task<T> pop() {
    folly::Try<void> result =
        co_await folly::coro::co_awaitTry(readSem_.co_wait());
    if (result.hasException()) {
      // Wait was canceled.
      co_yield folly::coro::co_error(std::move(result).exception());
    }

    std::lock_guard<std::mutex> lock(mutex_);
    co_return popNoLock();
  }

  /**
   * Non-coro get. Waits for elements to be available in the queue.
   * Name is inherited from fibers/Queue.h.
   */
  T get() noexcept {
    readSem_.wait();
    std::lock_guard<std::mutex> lock(mutex_);
    return popNoLock();
  }

  bool full() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() >= capacity_;
  }

  bool empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
  }

  size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  bool isBlocked() {
    std::lock_guard<std::mutex> lock(mutex_);
    return blocked_;
  }

  /**
   * Close the queue, waking up any waiters blocked on waitToPush().
   * After close(), waitToPush() will return immediately (not blocked).
   * This should be called when the queue is being destroyed or when
   * we want to cancel pending waiters.
   *
   * Thread-safe: Multiple calls are safe. Each call signals the semaphore
   * to ensure all waiters eventually wake up.
   */
  void close() {
    // Set closed flag (only first caller wins, but that's fine)
    closed_.store(true, std::memory_order_release);
    /*
     * Always signal - multiple signals are harmless (just increments token
     * count) This ensures ALL waiters eventually wake up, not just one
     */
    writeSem_.signal();
  }

  /**
   * Check if the queue has been closed.
   */
  bool isClosed() const {
    return closed_;
  }

  /**
   * Used by producers to suspend until the queue is genuinely unblocked.
   *
   * Returns false if the queue was closed while waiting.
   *
   * If the semaphore has no tokens available, then the producer is made to
   * wait for both of the following conditions to become true:
   *   1. the writeSem_ is given a token from the popNoLock() API
   *      when queue size decreases below lowWm_,
   *   2. the queue state is NOT blocked.
   *
   * Notes:
   * We should ALWAYS check isBlocked() first before calling. Otherwise
   * this can result in a deadlock, because the producer will be waiting for
   * a queue to unblock when it is already unblocked. In a single producer case,
   * this means the producer will be stuck waiting forever because it does not
   * know it can push to the queue, and can never put the queue into a blocked
   * state again.
   *
   * We should additionally check isBlocked() when the writeSem_ returns
   * a token. The writeSem_ will signal regardless of if the producer was
   * waiting on it. Let's say producer P is putting items into Q.
   *
   *  1. P pushes items to the high watermark.
   *  2. Q becomes blocked but P doesn't wait.
   *  3. Consumer C pops enough items to unblock the Q, and sends a signal
   *     which is stored in the semaphore.
   *  4. P comes back to push more items. Q becomes blocked. P tries to wait;
   *     it sees the previous signal from C at the first block <-> unblock, and
   *     would resume pushing even though Q is blocked.
   * P should not be allowed to push because Q is blocked, so it needs
   * to wait for the next unblock signal.
   *
   * Thread Safety:
   * - closed_ is std::atomic<bool> (sequentially consistent)
   * - We check closed_ BEFORE and AFTER the semaphore wait
   * - The semaphore is counting, so signal() never "loses" a token
   * - If close() happens before we enter co_wait(), the token is waiting
   * - If close() happens during co_wait(), we wake up and see closed_=true
   */
  folly::coro::Task<bool> waitToPush() {
    while (true) {
      // Early exit if already closed (optimization)
      if (closed_.load(std::memory_order_acquire)) {
        co_return false;
      }
      folly::Try<void> result =
          co_await folly::coro::co_awaitTry(writeSem_.co_wait());
      if (result.hasException()) {
        co_yield folly::coro::co_error(std::move(result).exception());
      }
      /*
       * Check if we were woken up due to close()
       * Must check AFTER wait returns to handle race with close()
       */
      if (closed_.load(std::memory_order_acquire)) {
        co_return false;
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!blocked_) {
          co_return true;
        }
      }
    }
  }

 private:
  /**
   * Method to pop from queue and set the blocked_ state.
   * This is called without a lock, but is a private method.
   * Queue consumers can only use the public APIs which
   * grab the lock first.
   */
  T popNoLock() {
    auto val = std::move(queue_.front());
    queue_.pop_front();
    if (blocked_ && queue_.size() <= loWm_) {
      blocked_ = false;
      writeSem_.signal();
    }
    return val;
  }

  /**
   * Maximum size of the queue.
   */
  const int capacity_;

  /**
   * The high water-mark of the queue size. If the queue is not blocked
   * when the queue size exceeds this value, then the queue becomes
   * blocked.
   */
  const int hiWm_{std::numeric_limits<int>::max()};

  /**
   * The low water-mark of the queue size. If the queue is blocked
   * when the queue size decreases below this value, then the
   * queue becomes unblocked.
   */
  const int loWm_{1};

  /**
   * State of the queue. If blocked is true, producers can choose to
   * wait for a signal from writeSem_ (the queue becoming unblocked)
   * before their next action.
   */
  bool blocked_{false};

  /**
   * Flag indicating queue has been closed. When true, waitToPush()
   * returns immediately with false.
   */
  std::atomic<bool> closed_{false};

  /* Signals the consumer when an element is available to be read from queue. */
  folly::fibers::Semaphore readSem_{0};
  /*
   * Returns a semaphore token when queue is unblocked, if queue was previously
   * blocked.
   */
  folly::fibers::Semaphore writeSem_{0};

  mutable std::mutex mutex_;
  std::deque<T> queue_;
};

} // namespace facebook::nettools::bgplib
