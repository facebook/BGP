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

#include <folly/fibers/Semaphore.h>
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"

namespace facebook::nettools::bgplib {

/*
 * BackPressuredQueue is a bounded Multi-Producer Multi-Consumer (MPMC) queue
 * that implements flow control using back pressure based on the design doc at
 * https://fburl.com/gdoc/1fem7cxy
 *
 * This class extends MonitoredMPMCQueue as the Data Queue and enforces
 * capacity-based back pressure through a semaphore-based Push Request Queue:
 *
 * 1. Bounded Capacity: The queue has a configurable maximum capacity that
 *    limits the number of items that can be queued at any time.
 *
 * 2. Push Request: push() operation is a request to reserve space atomically
 *    before actually pushing data to prevent race conditions. When at capacity,
 *    push() operation is asynchronously blocked, waiting on pushRequestQueue_
 *    semaphore until space becomes available.
 *
 * 3. Flow Control: Uses folly::fibers::Semaphore (pushRequestQueue_) to
 *    coordinate between producers and consumers:
 *    - Blocked push() operations wait on the semaphore
 *    - Each pop() signals the semaphore once, waking up one waiting producer
 *    - Separate dataQueueSize_ counter tracks reserved space independently
 *
 * 4. Graceful Cancellation: Push operations can be cancelled via co_error
 *    if the semaphore wait is interrupted.
 *
 * This queue is ideal for producer-consumer scenarios where producers may
 * generate items faster than consumers can process them, and you want to
 * prevent unbounded memory growth by applying back pressure to slow down
 * producers.
 *
 * Example usage:
 *   BackPressuredQueue<int> queue(100);  // Max 100 items
 *   co_await queue.push(42);             // Blocks if queue is full
 *   int value = co_await queue.pop();    // Signals space available
 */

#define CANCELLABLE_SEMAPHORE_WAIT(sem)                            \
  auto result = co_await folly::coro::co_awaitTry(sem.co_wait());  \
  if (result.hasException()) {                                     \
    co_yield folly::coro::co_error(std::move(result).exception()); \
  }

/*
 * workflow:
 * - Request to push
 * - Once the push request is granted (no matter pushed or not),
 *   the val is moved and no longer valid
 * - When the data queue is full, wait for space to become available
 * - If anything is pushed to the data queue, signal to pop
 */
#define PUSH_IMPL(waitOperation)                                              \
  bool granted, pushed;                                                       \
  while (true) {                                                              \
    std::tie(granted, pushed) = requestPushToDataQueue(std::forward<T>(val)); \
    if (granted) {                                                            \
      break;                                                                  \
    }                                                                         \
    waitOperation;                                                            \
  }                                                                           \
  if (pushed) {                                                               \
    dataQueueReadSem_.signal();                                               \
  }

template <typename T>
class BackPressuredQueue {
  // Declare the scope when this queue has a consumer
  class ConsumerScope {
   public:
    explicit ConsumerScope(BackPressuredQueue& queue) : queue_(queue) {
      queue_.open();
    }
    ~ConsumerScope() {
      queue_.close();
    }

    // no copy or move
    ConsumerScope(ConsumerScope&& other) = delete;
    ConsumerScope(const ConsumerScope&) = delete;
    ConsumerScope& operator=(const ConsumerScope&) = delete;
    ConsumerScope& operator=(ConsumerScope&&) = delete;

   private:
    BackPressuredQueue& queue_;
  };

 public:
  explicit BackPressuredQueue(size_t capacity = 1) : capacity_(capacity) {}

  BackPressuredQueue(const BackPressuredQueue& other)
      : capacity_(other.capacity_) {}

  /*
   * The copy assignment only copies the capacity, leaving everything else the
   * same
   */
  BackPressuredQueue& operator=(const BackPressuredQueue& other) {
    capacity_ = other.capacity_;
    return *this;
  }

  folly::coro::Task<void> push(T&& val) noexcept {
    PUSH_IMPL(CANCELLABLE_SEMAPHORE_WAIT(pushRequestQueue_););
  }

  // asynchronous push for coro, no cancellation
  folly::coro::Task<void> nonCancellablePush(T&& val) noexcept {
    PUSH_IMPL(
        /*
         * co_wait itself can be cancelled. Therefore we need to use a dummy
         * CancellationToken to avoid co_wait being cancelled
         */
        co_await folly::coro::co_withCancellation(
            folly::CancellationToken{}, pushRequestQueue_.co_wait()));
  }

  // synchoronous push for fiber, no cancellation support
  void fiberPush(T&& val) noexcept {
    PUSH_IMPL(pushRequestQueue_.wait());
  }

  /*
   * Force push, no back pressure, ignoring open/close status.
   * MUST only be used for one-time control signals (e.g., stop/EoR
   * during termination or initialization) where blocking is not
   * acceptable. MUST NOT be used for data-path operations at scale
   * as it bypasses queue capacity and can cause unbounded growth.
   *
   * ATTENTION: forcePush() will bypass the queue capacity limit.
   * Make sure you understand the consequence before using it.
   */
  void forcePush(T&& val) noexcept {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      dataQueue_.emplace_back(std::forward<T>(val));
    }
    dataQueueReadSem_.signal();
  }

  folly::coro::Task<T> pop() {
    // wait until something to pop
    CANCELLABLE_SEMAPHORE_WAIT(dataQueueReadSem_);

    auto [val, wakeUpPushRequest] = popDataQueue();
    if (wakeUpPushRequest) {
      pushRequestQueue_.signal();
    }
    co_return val;
  }

  size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dataQueue_.size();
  }

  bool empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return dataQueue_.empty();
  }

  /*
   * Close the queue and wake up all existing waiting push requests
   * When the queue is closed, nothing will be pushed to the queue
   */
  void close() noexcept {
    size_t pushRequestQueueSize;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
      pushRequestQueueSize = pushRequestQueueSize_;
      pushRequestQueueSize_ = 0;
    }
    // signal all push request queue
    while (pushRequestQueueSize-- > 0) {
      pushRequestQueue_.signal();
    }
  }

  // reopen the queue
  void open() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = false;
  }

  ConsumerScope getConsumerScope() {
    return ConsumerScope(*this);
  }

 protected:
  /*
   * request to push to the data queue, three outcomes represented by (granted,
   * pushed):
   * - queue closed, the request is granted but the data is dropped
   * - request granted, at this moment, val is moved to the queue
   * - request failed, need to wait in the push request queue later
   */
  inline std::pair<bool, bool> requestPushToDataQueue(T&& val) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      // no one will be listening, drop the data
      return std::make_pair(true, false);
    }
    if (dataQueue_.size() < capacity_) {
      // Request granted
      dataQueue_.emplace_back(std::forward<T>(val));
      return std::make_pair(true, true);
    }
    // Request failed, need to wait in the request queue
    ++pushRequestQueueSize_;
    return std::make_pair(false, false);
  }

  inline std::pair<T, bool> popDataQueue() {
    bool wakeUpPushRequest = false;
    std::lock_guard<std::mutex> lock(mutex_);
    T val = std::move(dataQueue_.front());
    dataQueue_.pop_front();

    if (pushRequestQueueSize_ > 0) {
      wakeUpPushRequest = true;
      --pushRequestQueueSize_;
    }
    return std::make_pair(std::move(val), wakeUpPushRequest);
  }

  size_t capacity_;

  // Mutex protects dataQueueSize_ and pushRequestQueue_ operations
  mutable std::mutex mutex_;

  bool closed_{false};

  std::deque<T> dataQueue_;
  folly::fibers::Semaphore dataQueueReadSem_{0};

  /*
   * Semaphore that blocks push operations when queue is at capacity
   * Initialized with 0 tokens - producers wait here when queue is full
   * and are signaled by pop() operations when space becomes available.
   */
  folly::fibers::Semaphore pushRequestQueue_{0};
  size_t pushRequestQueueSize_{0};

#ifdef BackPressuredQueue_TEST_FRIENDS
  BackPressuredQueue_TEST_FRIENDS
#endif
};

template <class C>
using MonitoredBackPressuredQueue = bgp::MonitoredQueue<BackPressuredQueue<C>>;

} // namespace facebook::nettools::bgplib
