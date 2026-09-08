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

#include <memory>
#include <optional>
#include <utility>

#include <folly/Function.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/fibers/TimedMutex.h>
#include <folly/fibers/WhenN.h>

#include "neteng/fboss/bgp/cpp/lib/fibers/Utils.h"

namespace facebook {
namespace nettools {
namespace bgplib {

// forward-declare queue implementation
namespace detail {
template <typename T>
class Queue;
}

/*
 * Read-only view of the queue. Multiple of these can exist,
 * they effectively ref-count the underlying Queue. This is
 * suppoed to be produced by calling getReader() on RWQueue.
 * Copying this object simply copies underlying pointer, but
 * keep the underlying queue the same.
 */
template <typename T>
class RQueue {
 public:
  using ElementT = T;

  // to allow for value semantics
  RQueue() = default;
  // movable
  RQueue(RQueue&&) = default;
  RQueue& operator=(RQueue&&) = default;
  // wrap existing Queue
  explicit RQueue(std::shared_ptr<detail::Queue<T>> queue)
      : queue_{std::move(queue)} {}
  ~RQueue() = default;

  /*
   * See documentation in detail::Queue
   */

  std::optional<T> get() noexcept {
    CHECK(queue_);
    return queue_->get();
  }

  std::optional<T> sleepAndGet(
      std::chrono::milliseconds duration =
          std::chrono::milliseconds(1)) noexcept {
    CHECK(queue_);
    return queue_->sleepAndGet(duration);
  }

  bool empty() noexcept {
    CHECK(queue_);
    return queue_->empty();
  }

  bool full() noexcept {
    CHECK(queue_);
    return queue_->full();
  }

  auto size() noexcept {
    CHECK(queue_);
    return queue_->size();
  }

  RQueue<T> share() noexcept {
    CHECK(queue_);
    return RQueue(queue_);
  }

  /*
   * TODO: those need to be private, need to figure out proper friending here
   * transformQueue needs those to put back values in RQueue (unread)
   */
  bool putNullFront() noexcept {
    CHECK(queue_);
    return queue_->putNullFront();
  }

  bool putFront(T&& value) noexcept {
    CHECK(queue_);
    return queue_->putFront(std::forward<T>(value));
  }

 private:
  /*
   * non-copyable. use is expected to call getReader()
   * on RWQueue to obtain additional readers.
   */
  RQueue(RQueue const&) = delete;
  RQueue& operator=(RQueue const&) = delete;

  // the underlying queue we share with writer
  const std::shared_ptr<detail::Queue<T>> queue_;
};

/*
 * Write-only view to the queue. Similar to RQueue<T>, supposed
 * to be produced by calling getWriter() on RWQueue. Notice that
 * copying this objcect create another view of the same underlying
 * queue, not new queue.
 */
template <typename T>
class WQueue {
 public:
  using ElementT = T;

  WQueue() = default;
  // movable
  WQueue(WQueue&&) = default;
  WQueue& operator=(WQueue&&) = default;
  explicit WQueue(std::shared_ptr<detail::Queue<T>> queue)
      : queue_{std::move(queue)} {}
  ~WQueue() = default;

  /*
   * See documentation in detail::Queue
   */

  bool put(T&& value) noexcept {
    CHECK(queue_);
    return queue_->put(std::forward<T>(value));
  }

  bool putNull() noexcept {
    CHECK(queue_);
    return queue_->putNull();
  }

  bool putNullFront() noexcept {
    CHECK(queue_);
    return queue_->putNullFront();
  }

  bool putFront(T&& value) noexcept {
    CHECK(queue_);
    return queue_->putFront(std::forward<T>(value));
  }

  bool empty() noexcept {
    CHECK(queue_);
    return queue_->empty();
  }

  bool full() noexcept {
    CHECK(queue_);
    return queue_->full();
  }

  auto size() noexcept {
    CHECK(queue_);
    return queue_->size();
  }

  void close() noexcept {
    CHECK(queue_);
    queue_->close();
  }

  WQueue<T> share() noexcept {
    CHECK(queue_);
    return WQueue(queue_);
  }

 private:
  /*
   * non-copyable. use getWriter() on RWQueue to obtain
   * a new writer for the queue.
   */
  WQueue(WQueue const&) = delete;
  WQueue& operator=(WQueue const&) = delete;

  // the underlying queue we share with the others
  const std::shared_ptr<detail::Queue<T>> queue_;
};

/*
 * RW view of the queue. Relies on Queue hidden underneath
 * a shared_ptr. This allows the same Queue to be shared
 * by multiple front-end wrapper objects. As long as at least
 * one of them is alive the queue is live and ready to take/emit
 * data out.
 */
template <typename T>
class RWQueue {
 public:
  using ElementT = T;

  // infinity queue size
  RWQueue() : RWQueue(0) {}

  // movable
  RWQueue(RWQueue&&) = default;
  RWQueue& operator=(RWQueue&&) = default;
  explicit RWQueue(uint64_t capacity)
      : queue_{std::make_shared<detail::Queue<T>>(capacity)} {}
  explicit RWQueue(std::shared_ptr<detail::Queue<T>> queue)
      : queue_{std::move(queue)} {}
  ~RWQueue() = default;

  /*
   * See documentation in detail::Queue
   */

  std::optional<T> get() noexcept {
    CHECK(queue_);
    return queue_->get();
  }

  size_t get_num_reads() noexcept {
    CHECK(queue_);
    return queue_->get_num_reads();
  }

  size_t get_num_writes() noexcept {
    CHECK(queue_);
    return queue_->get_num_writes();
  }

  std::optional<T> sleepAndGet(
      std::chrono::milliseconds duration =
          std::chrono::milliseconds(1)) noexcept {
    CHECK(queue_);
    return queue_->sleepAndGet(duration);
  }

  bool put(T&& value) noexcept {
    CHECK(queue_);
    return queue_->put(std::forward<T>(value));
  }

  bool putNull() noexcept {
    CHECK(queue_);
    return queue_->putNull();
  }

  bool empty() noexcept {
    CHECK(queue_);
    return queue_->empty();
  }

  bool full() noexcept {
    CHECK(queue_);
    return queue_->full();
  }

  auto size() noexcept {
    CHECK(queue_);
    return queue_->size();
  }

  bool putNullFront() noexcept {
    CHECK(queue_);
    return queue_->putNullFront();
  }

  bool putFront(T&& value) noexcept {
    CHECK(queue_);
    return queue_->putFront(std::forward<T>(value));
  }

  void close() noexcept {
    CHECK(queue_);
    queue_->close();
  }

  /*
   * get reader for this queue, may be called multiple times
   * wraps this into shared_ptr
   */
  RQueue<T> getReader() noexcept {
    return RQueue<T>(queue_);
  }

  // same as above, but for the writer
  WQueue<T> getWriter() noexcept {
    return WQueue<T>(queue_);
  }

  // share this queue in RW mode
  RWQueue<T> share() noexcept {
    return RWQueue(queue_);
  }

 private:
  // non-copyable, use share() to obtain another RW view
  RWQueue(RWQueue const&) = delete;
  RWQueue& operator=(RWQueue const&) = delete;

  // the underlying queue we share with the reader
  const std::shared_ptr<detail::Queue<T>> queue_;
};

namespace detail {
/*
 * Simple bounded queue, supports multiple producers/consumers
 * All operations must happen within single thread, there is no
 * thread safety logic
 */
template <typename T>
class Queue {
 public:
  // convenience types
  using RQueueT = RQueue<T>;
  using WQueueT = WQueue<T>;
  using ElementT = T;

  // supposed to be only used with make_shared
  explicit Queue(uint64_t capacity) : capacity_{capacity} {}
  ~Queue() = default;

  // allow consumer to check for empty queue
  bool empty() noexcept {
    std::lock_guard<folly::fibers::TimedMutex> g(lock_);
    return queue_.empty();
  }

  auto size() noexcept {
    std::lock_guard<folly::fibers::TimedMutex> g(lock_);
    return queue_.size();
  }

  bool full() noexcept {
    std::lock_guard<folly::fibers::TimedMutex> g(lock_);
    // has to be <=, cause we can putFront and bypass size limit
    return (capacity_ <= queue_.size());
  }

  size_t get_num_reads() noexcept {
    std::lock_guard<folly::fibers::TimedMutex> g(lock_);
    return reads_;
  }

  size_t get_num_writes() noexcept {
    std::lock_guard<folly::fibers::TimedMutex> g(lock_);
    return writes_;
  }

  void close() noexcept {
    std::lock_guard<folly::fibers::TimedMutex> g(lock_);
    if (closed_) {
      return;
    }
    closed_ = true;

    queue_.clear();
    for (auto& baton : publishers_) {
      baton->post();
    }

    for (auto& baton : consumers_) {
      baton->post();
    }
  }

  /*
   * put new value on the queue, block if we exceed queue message capacity
   */
  bool put(T&& value) noexcept(std::is_nothrow_move_constructible<T>::value) {
    return putImpl([this, &value]() {
      queue_.emplace_back(std::optional<T>(std::forward<T>(value)));
    });
  }

  /*
   * Put a null value in the the queue. This could be used by the
   * consumers as a signal to terminate.
   */
  bool putNull() noexcept {
    return putImpl([this]() { queue_.emplace_back(std::optional<T>()); });
  }

  /*
   * This is a signal to stop the queue immediately. The consumer
   * that will wake up will swallow the poisonous pill immediately
   * Notice that we skip the waiting limit, this operation basically
   * violates all ordering.
   */
  bool putNullFront() noexcept {
    return putImpl(
        [this]() { queue_.emplace_front(std::optional<T>()); },
        false /* waitForPut */);
  }

  /*
   * Put a value at the head of the queue, bypassing capacity constraints
   */
  bool putFront(T&& value) noexcept(
      std::is_nothrow_move_constructible<T>::value) {
    return putImpl(
        [this, &value]() {
          queue_.emplace_front(std::optional<T>(std::forward<T>(value)));
        },
        false /* waitForPut */);
  }

  /*
   * Get next value from the queue, wait if not available
   * The consumers are served in FIFO order, later joiners
   * join the line
   */
  std::optional<T> get() noexcept(
      std::is_nothrow_move_constructible<T>::value) {
    if (closed_) {
      return std::nullopt;
    }

    // if there are no waiters and msg queue is non empty, grab it
    std::unique_lock<folly::fibers::TimedMutex> g(lock_);
    // always check closed_ after lock
    if (closed_) {
      return std::nullopt;
    }

    bool canSkipWait = !queue_.empty() && consumers_.empty();
    if (canSkipWait) {
      return getNowaitLocked();
    }

    // put ourselves on the waiting queue
    folly::fibers::Baton baton;
    consumers_.push_back(&baton);

    // unlock before wait
    g.unlock();

    baton.wait();

    // done waiting, grab lock again
    g.lock();

    // always check closed_ after lock
    if (closed_) {
      return std::nullopt;
    }

    consumers_.pop_front();

    // since we woke up, message queue should be non-empty
    return getNowaitLocked();
  }

  std::optional<T> sleepAndGet(
      std::chrono::milliseconds duration = std::chrono::milliseconds(
          1)) noexcept(std::is_nothrow_move_constructible<T>::value) {
    fiberSleepFor(duration);
    return get();
  }

 private:
  Queue() = delete;

  // non-copyable
  Queue(Queue const&) = delete;
  Queue& operator=(Queue const&) = delete;

  /*
   * capacity limit: we would start queueing publishers
   * when we hit this
   */
  const uint64_t capacity_{0};

  void wakeNext() {
    if (!consumers_.empty()) {
      // pop next consumer and wake the consumer up
      consumers_.front()->post();
    }
  }

  /*
   * lock_ might be unlocked for wait, and then later acquired
   * so always check closed_ after waitForPut
   */
  void waitForPut(std::unique_lock<folly::fibers::TimedMutex>& g) {
    // no limit for capacity
    if (!capacity_) {
      return;
    }

    if (queue_.size() < capacity_ && publishers_.empty()) {
      return;
    }

    // if we hit the capacity limit, wait
    folly::fibers::Baton baton;
    publishers_.emplace_back(&baton);

    // unlock before wait
    g.unlock();

    baton.wait();

    // done waiting, grab lock again
    g.lock();

    publishers_.pop_front();
  }

  // This function does not acquire lock_, caller is expected to handle lock_
  std::optional<T> getNowaitLocked() {
    DCHECK(!queue_.empty());

    auto result = std::move(queue_.front());
    queue_.pop_front();
    reads_++;

    // notify next publisher if needed
    if (!publishers_.empty() && (queue_.size() < capacity_)) {
      publishers_.front()->post();
    }

    /*
     * wake up the next consumer - this is important
     * because otherwise there may be no one else
     * to wake them up
     */
    if (!queue_.empty() && !consumers_.empty()) {
      consumers_.front()->post();
    }

    return result;
  }

  template <typename Func>
  bool putImpl(Func func, bool wait = true) {
    if (closed_) {
      return false;
    }

    std::unique_lock<folly::fibers::TimedMutex> g(lock_);
    // always check closed_ after lock
    if (closed_) {
      return false;
    }

    if (wait) {
      waitForPut(g);

      // waitForPut might reaquired lock, check closed_ here
      if (closed_) {
        return false;
      }
    }

    func();
    writes_++;

    // notify next waiting consumer if any
    wakeNext();

    // notify next publisher if still has room available
    if (capacity_ && queue_.size() < capacity_ && !publishers_.empty()) {
      publishers_.front()->post();
    }

    return true;
  }

  /*
   * this can only be set once, via close() call. After this, all get()
   * operations on queue will return Null, and all put() will never block
   * and retun false
   */
  std::atomic<bool> closed_{false};

  folly::fibers::TimedMutex lock_;

  // the messages passed from publishers to  consumers
  std::deque<std::optional<T>> queue_;

  // the queue publishers wait on when capacity is exceeded
  std::deque<folly::fibers::Baton*> publishers_;

  // the queue consumers wait on when the message queue is empty
  std::deque<folly::fibers::Baton*> consumers_;

  size_t reads_{0};
  size_t writes_{0};
};

} // namespace detail

/*
 * The logic is useful for merging single producer/single consumer queues.
 * It assumes the producers would put NULL in the queue when they finish.
 * This could apply to multiple producer/consumer queues, but normally
 * the merger assumes its the only one consuming the multiple inputs
 */
template <typename Iterator, typename T, template <typename> class Queue>
void mergeQueues(Iterator first, Iterator last, Queue<T>& queue) {
  static_assert(
      std::is_same<
          typename std::decay<T>::type,
          typename std::iterator_traits<Iterator>::value_type::ElementT>::value,
      "Output queue element type != Iterated queues element type");
  // create worker for each queue we feed from
  std::vector<folly::Function<void()>> workers;
  for (auto it = first; it != last; ++it) {
    workers.emplace_back([&queue, it]() mutable {
      while (true) {
        auto val = it->get();
        // null detected, stop
        if (!val) {
          break;
        }
        // writer is wrapped in move wrapper
        queue.put(std::move(*val));
      }
    });
  }

  /*
   * wait for all queues to drain, then put null in the queue we returned
   * to the caller
   */
  folly::fibers::collectAll(workers.begin(), workers.end());
  queue.putNull();
}

/*
 * Helpers to implement static queue merge
 */
namespace detail {

/*
 * Generate code applying function F to all elements of tuple.
 * Blatantly stolen from:
 * http://codereview.stackexchange.com/questions/51407/
 */
template <typename Tuple, typename F, std::size_t... Indices>
void for_each_impl(Tuple&& tuple, F&& f, std::index_sequence<Indices...>) {
  using swallow = int[];
  (void)swallow{
      1,
      (f(Indices, std::get<Indices>(std::forward<Tuple>(tuple))),
       void(),
       int{})...};
}

template <typename Tuple, typename F>
void for_each(Tuple&& tuple, F&& f) {
  constexpr std::size_t N =
      std::tuple_size<std::remove_reference_t<Tuple>>::value;
  for_each_impl(
      std::forward<Tuple>(tuple),
      std::forward<F>(f),
      std::make_index_sequence<N>{});
}

} // namespace detail

/*
 * Merge N queues, generate merging code at compile time
 * (actual merging happens at runtime). Nice benefit -
 * you can merge queues of element types T1, T2, T3...
 * into queue with type std::variant<T1, T2, T3>
 */
template <
    typename... InputQueues,
    typename Combined = std::variant<typename InputQueues::ElementT...>,
    template <typename> class Queue>
void mergeQueuesStatic(Queue<Combined>& output, InputQueues... inputs) {
  std::array<folly::Function<void()>, sizeof...(InputQueues)> workers;
  auto inputTuple = std::make_tuple(std::forward<InputQueues>(inputs)...);
  detail::for_each(
      std::move(inputTuple),
      [&workers, &output](std::size_t index, auto input) {
        workers[index] = [&output, input = std::move(input)]() mutable {
          while (true) {
            auto val = input.get();
            // null detected, stop
            if (!val) {
              break;
            }
            output.put(std::move(*val));
          }
        };
      });
  folly::fibers::collectAll(workers.begin(), workers.end());
  output.putNull();
}

/*
 * Timer is set with a given duration. It would be re-set,
 * or stopped. After stopping it needs to explicitly re-start,
 */
template <typename T>
class Timer {
 public:
  static_assert(
      std::is_default_constructible<T>::value,
      "T is not default cosntructible");

  explicit Timer(std::chrono::steady_clock::duration period)
      : period_{period} {}

  static std::unique_ptr<Timer<T>> make(
      std::chrono::steady_clock::duration period) {
    return std::make_unique<Timer<T>>(period);
  }

  RQueue<T> getQueue() {
    return queue_.getReader();
  }

  /*
   * Run the wait loop, wake up on timeout or
   * when time has been explicitly stopped
   */
  void run() {
    // Don't allow concurrent run() invocation.
    if (isRunnable_ > 0) {
      return;
    }
    ++isRunnable_;

    auto evb = getFiberEventBase();
    // if not runnable, abort run loop
    while (isRunnable_) {
      baton_.reset();
      reset_ = false;
      timer_ = folly::AsyncTimeout::schedule(
          std::chrono::duration_cast<std::chrono::milliseconds>(period_),
          *evb,
          [this]() noexcept { baton_.post(); });
      baton_.wait();
      // reset flag not being set means we timed out
      if (isRunnable_ && !reset_) {
        queue_.put(T{});
        break;
      }
    }
    isRunnable_ = 0;
  }

  /*
   * Stop the run loop - notify the waiter
   */
  void stop() {
    /*
     * notify consumer regardless
     * this is here to prevent run function is never called but
     * we have a consumer loop running
     */
    queue_.putNull();
    if (timer_) {
      timer_->cancelTimeout();
    }
    if (isRunnable_ >= 0) {
      --isRunnable_;
    }
    baton_.post();
  }

  /*
   * Abort and re-start current waiting
   */
  void reset() {
    reset(period_);
  }

  /*
   * Abort and re-start with new time period
   */
  void reset(std::chrono::steady_clock::duration period) {
    if (timer_) {
      timer_->cancelTimeout();
    }
    reset_ = true;
    period_ = period;
    baton_.post();
  }

  bool stopped() {
    return isRunnable_ <= 0;
  }

  std::chrono::steady_clock::duration getPeriod() {
    return period_;
  }

 private:
  Timer(Timer const&) = delete;
  Timer& operator=(Timer const&) = delete;

  std::unique_ptr<folly::AsyncTimeout> timer_;
  std::chrono::steady_clock::duration period_;
  RWQueue<T> queue_;
  folly::fibers::Baton baton_;
  std::atomic<int> isRunnable_{0};
  std::atomic<bool> reset_{false};
};

} // namespace bgplib
} // namespace nettools
} // namespace facebook
