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

#include <gtest/gtest.h>

#define BackPressuredQueue_TEST_FRIENDS                   \
  FRIEND_TEST(BackPressuredQueueTest, PopTest);           \
  FRIEND_TEST(BackPressuredQueueTest, CloseTest);         \
  FRIEND_TEST(BackPressuredQueueTest, OpenTest);          \
  FRIEND_TEST(BackPressuredQueueTest, CloseReopenTest);   \
  FRIEND_TEST(BackPressuredQueueTest, ConsumerScopeTest); \
  FRIEND_TEST(BackPressuredQueueTest, FiberPushBlocksWithoutFiberManagerTest);

#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"

#include <folly/CancellationToken.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/WithCancellation.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/Semaphore.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <thread>

namespace facebook::nettools::bgplib {

TEST(BackPressuredQueueTest, CopyConstructorTest) {
  // Create a queue with capacity 10
  BackPressuredQueue<int> queue1(10);

  // Push something to queue1 first
  for (int i = 0; i < 5; i++) {
    auto item = i;
    queue1.fiberPush(std::move(item));
  }

  // Confirm that something is in queue1
  EXPECT_EQ(queue1.size(), 5);

  // Copy construct queue2 from queue1 - should copy capacity
  BackPressuredQueue<int> queue2(queue1);

  // Verify queue2 has capacity 10 (copied from queue1) by pushing 10 items
  for (int i = 0; i < 10; i++) {
    auto item = i;
    queue2.fiberPush(std::move(item));
  }

  /*
   * Check size - queue2 should have 10 items (copy constructor only copies
   * capacity, not data)
   */
  EXPECT_EQ(queue2.size(), 10);

  // Verify queue1 is unchanged
  EXPECT_EQ(queue1.size(), 5);
}

TEST(BackPressuredQueueTest, CopyAssignmentTest) {
  // Create two queues with different capacities
  BackPressuredQueue<int> queue1(10);
  BackPressuredQueue<int> queue2(20);

  // Push something to queue1 first
  for (int i = 0; i < 5; i++) {
    auto item = i;
    queue1.fiberPush(std::move(item));
  }

  // Confirm that something is in queue1
  EXPECT_EQ(queue1.size(), 5);

  // Copy assign to queue2 - should copy capacity from queue2
  queue1 = queue2;

  /*
   * Verify capacity was copied by pushing more than queue1's original capacity
   * would allow without blocking.
   */
  for (int i = 0; i < 10; i++) {
    auto item = i;
    queue1.fiberPush(std::move(item));
  }

  // Check size - queue1 should have 15 items
  EXPECT_EQ(queue1.size(), 15);
}

TEST(BackPressuredQueueTest, PushTest) {
  // Create a queue with capacity 10
  BackPressuredQueue<int> queue(10);

  auto producer = [&queue]() -> folly::coro::Task<void> {
    for (int i = 0; i < 5; i++) {
      XLOGF(DBG1, "Pushing: {}", i);
      auto item = i; // to be moved
      co_await queue.push(std::move(item));
      XLOGF(DBG1, "Pushed: {}", i);
    }
  };

  // Use blockingWait to execute the coroutine on the main thread
  folly::coro::blockingWait(producer());

  // Verify queue has the expected number of items
  EXPECT_EQ(queue.size(), 5);
}

TEST(BackPressuredQueueTest, PushExceptionTest) {
  // Create a queue with capacity 1
  BackPressuredQueue<int> queue(1);

  // Fill the queue to capacity by directly injecting an item
  folly::coro::blockingWait(queue.push(0));
  EXPECT_EQ(queue.size(), 1);

  /*
   * Now try to push another item - it should wait on the semaphore
   * We'll cancel this operation immediately to trigger the exception path
   */
  folly::CancellationSource cancellationSource;
  // Request cancellation BEFORE starting the push task
  cancellationSource.requestCancellation();

  auto cancellableTask = [&]() -> folly::coro::Task<void> {
    co_await folly::coro::co_withCancellation(
        cancellationSource.getToken(), queue.push(99));
  };

  // Wait for the task to complete and verify it throws an exception
  bool exceptionCaught = false;
  try {
    folly::coro::blockingWait(cancellableTask());
  } catch (const folly::OperationCancelled&) {
    // Expected - the push operation was cancelled while waiting on semaphore
    exceptionCaught = true;
  }

  EXPECT_TRUE(exceptionCaught)
      << "Expected cancellation exception to be thrown";

  // Verify queue is still at capacity (the cancelled push didn't complete)
  EXPECT_EQ(queue.size(), 1);
}

TEST(BackPressuredQueueTest, NonCancellablePushTest) {
  // Create a queue with capacity 10
  BackPressuredQueue<int> queue(10);

  auto producer = [&queue]() -> folly::coro::Task<void> {
    for (int i = 0; i < 5; i++) {
      XLOGF(DBG1, "Pushing: {}", i);
      auto item = i; // to be moved
      co_await queue.nonCancellablePush(std::move(item));
      XLOGF(DBG1, "Pushed: {}", i);
    }
  };

  // Use blockingWait to execute the coroutine on the main thread
  folly::coro::blockingWait(producer());

  // Verify queue has the expected number of items
  EXPECT_EQ(queue.size(), 5);
}

TEST(BackPressuredQueueTest, NonCancellablePushNoExceptionTest) {
  // Create a queue with capacity 1
  BackPressuredQueue<int> queue(1);
  folly::EventBase evb;

  // Fill the queue to capacity by directly injecting an item
  folly::coro::blockingWait(queue.nonCancellablePush(0));
  EXPECT_EQ(queue.size(), 1);

  folly::CancellationSource cancellationSource;
  // Request cancellation BEFORE starting the push task
  cancellationSource.requestCancellation();

  std::atomic<bool> exceptionCaught{false};

  /*
   * noncancellable push would block waiting for space, so we need to schedule
   * two coros: one that does the push (which will block) and one that pops
   * from the queue to unblock the push
   */
  auto nonCancellableTask = [&]() -> folly::coro::Task<void> {
    try {
      co_await folly::coro::co_withCancellation(
          cancellationSource.getToken(), queue.nonCancellablePush(99));
    } catch (const folly::OperationCancelled&) {
      // nonCancellablePush should not throw this exception
      exceptionCaught = true;
    }
  };

  auto popTask = [&]() -> folly::coro::Task<void> {
    // Pop to unblock the noncancellable push
    co_await queue.pop();
  };

  // Schedule both tasks on the event base
  auto pushFuture =
      folly::coro::co_withExecutor(&evb, nonCancellableTask()).start();
  auto popFuture = folly::coro::co_withExecutor(&evb, popTask()).start();

  std::thread evbThread([&]() { evb.loop(); });

  // Wait for both tasks to complete
  folly::coro::blockingWait(
      folly::coro::collectAll(std::move(pushFuture), std::move(popFuture)));

  evb.terminateLoopSoon();
  evbThread.join();

  EXPECT_FALSE(exceptionCaught.load())
      << "No cancellation exception should be thrown";

  // Verify queue still has 1 item (the 99 that was pushed after pop 0)
  EXPECT_EQ(queue.size(), 1);
  EXPECT_EQ(folly::coro::blockingWait(queue.pop()), 99);
}

TEST(BackPressuredQueueTest, FiberPushTest) {
  // Create a queue with capacity 10
  BackPressuredQueue<int> queue(10);

  for (int i = 0; i < 5; i++) {
    XLOGF(DBG1, "Pushing: {}", i);
    auto item = i; // to be moved
    queue.fiberPush(std::move(item));
    XLOGF(DBG1, "Pushed: {}", i);
  }

  // Verify queue has the expected number of items
  EXPECT_EQ(queue.size(), 5);
}

TEST(BackPressuredQueueTest, ForcePushTest) {
  // Create a queue with capacity 10
  BackPressuredQueue<int> queue(10);

  for (int i = 0; i < 5; i++) {
    XLOGF(DBG1, "Pushing: {}", i);
    auto item = i; // to be moved
    queue.forcePush(std::move(item));
    XLOGF(DBG1, "Pushed: {}", i);
  }

  // Verify queue has the expected number of items
  EXPECT_EQ(queue.size(), 5);

  // Keep pushing above the queue capacity
  for (int i = 5; i < 15; i++) {
    XLOGF(DBG1, "Pushing: {}", i);
    auto item = i; // to be moved
    queue.forcePush(std::move(item));
    XLOGF(DBG1, "Pushed: {}", i);
  }

  // Verify queue has the expected number of items
  EXPECT_EQ(queue.size(), 15);
}

TEST(BackPressuredQueueTest, PopTest) {
  // Create a queue with capacity 10
  BackPressuredQueue<int> queue(10);

  // Directly inject items into the underlying queue
  for (int i = 0; i < 5; i++) {
    queue.dataQueue_.emplace_back(i);
    queue.dataQueueReadSem_.signal();
    XLOGF(DBG1, "Injected: {}", i);
  }

  // Verify queue has the expected number of items
  EXPECT_EQ(queue.size(), 5);

  // Now test the pop function on the main thread
  auto consumer = [&queue]() -> folly::coro::Task<void> {
    for (int i = 0; i < 5; i++) {
      int value = co_await queue.pop();
      XLOGF(DBG1, "Popped: {}", value);
      EXPECT_EQ(value, i);
    }
  };

  // Use blockingWait to execute the coroutine on the main thread
  folly::coro::blockingWait(consumer());

  // Verify queue is empty after popping all items
  EXPECT_EQ(queue.size(), 0);
}

TEST(BackPressuredQueueTest, PopExceptionTest) {
  // Create a queue with capacity 0
  BackPressuredQueue<int> queue(0);

  /*
   * Pop from the queue - it should wait on the semaphore
   * We'll cancel this operation immediately to trigger the exception path
   */
  folly::CancellationSource cancellationSource;
  // Request cancellation BEFORE starting the push task
  cancellationSource.requestCancellation();

  auto cancellableTask = [&]() -> folly::coro::Task<void> {
    co_await folly::coro::co_withCancellation(
        cancellationSource.getToken(), queue.pop());
  };

  // Wait for the task to complete and verify it throws an exception
  bool exceptionCaught = false;
  try {
    folly::coro::blockingWait(cancellableTask());
  } catch (const folly::OperationCancelled&) {
    // Expected - the push operation was cancelled while waiting on semaphore
    exceptionCaught = true;
  }

  EXPECT_TRUE(exceptionCaught)
      << "Expected cancellation exception to be thrown";
}

TEST(BackPressuredQueueTest, SizeAndEmptyTest) {
  // Create a queue with capacity 10
  BackPressuredQueue<int> queue(10);

  // Verify size and empty methods
  EXPECT_EQ(queue.size(), 0);
  EXPECT_TRUE(queue.empty());

  // Directly inject items into the underlying queue
  for (int i = 0; i < 5; i++) {
    auto item = i;
    queue.fiberPush(std::move(item));
    EXPECT_EQ(queue.size(), i + 1);
    EXPECT_FALSE(queue.empty());
  }
}

/**
 * Test close() functionality with back-pressured waiters.
 *
 * This test verifies that close() properly wakes up all producers that are
 * blocked waiting on the push request semaphore when the queue is full.
 *
 * Test sequence:
 * 1. Fill queue to capacity (2 items)
 * 2. Start 3 producers that will block on the full queue (back pressure)
 * 3. Wait deterministically for all producers to be blocked on the semaphore
 * 4. Close the queue - should wake up all blocked producers
 * 5. Verify all blocked producers complete successfully (pushes are dropped)
 * 6. Verify existing items can still be popped from the closed queue
 *
 * Key behaviors verified:
 * - close() signals all waiters on the push request semaphore
 * - Blocked producers wake up and complete without hanging
 * - Pushes to a closed queue are dropped (queue size unchanged)
 * - Existing items remain accessible after close
 */
TEST(BackPressuredQueueTest, CloseTest) {
  BackPressuredQueue<int> queue(2);
  folly::EventBase evb;

  // Fill the queue to capacity
  folly::coro::blockingWait(queue.push(1));
  folly::coro::blockingWait(queue.push(2));
  EXPECT_EQ(queue.size(), 2);

  // Start multiple producers that will be blocked due to back pressure
  constexpr int kNumProducers = 3;
  std::atomic<int> completedPushes{0};

  auto blockedProducer = [&](int value) -> folly::coro::Task<void> {
    co_await queue.push(std::move(value));
    completedPushes++;
  };

  // Start 3 producers that will be blocked waiting for space
  auto producer1 =
      folly::coro::co_withExecutor(&evb, blockedProducer(3)).start();
  auto producer2 =
      folly::coro::co_withExecutor(&evb, blockedProducer(4)).start();
  auto producer3 =
      folly::coro::co_withExecutor(&evb, blockedProducer(5)).start();

  std::thread evbThread([&]() { evb.loop(); });

  while (true) {
    // Spin until all producers are blocked
    {
      std::lock_guard<std::mutex> lock(queue.mutex_);
      if (queue.pushRequestQueueSize_ == kNumProducers) {
        break;
      }
    }
    std::this_thread::yield();
  }

  // Verify queue is still at capacity and producers haven't completed
  EXPECT_EQ(queue.size(), 2);
  EXPECT_EQ(completedPushes.load(), 0);
  EXPECT_EQ(queue.pushRequestQueueSize_, kNumProducers);

  // Close the queue - this should wake up all blocked producers
  queue.close();

  // Verify that the queue is closed
  EXPECT_TRUE(queue.closed_);

  // Wait for all producers to complete
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producer1), std::move(producer2), std::move(producer3)));

  evb.terminateLoopSoon();
  evbThread.join();

  // All producers should have completed (woken up by close)
  EXPECT_EQ(completedPushes.load(), 3);

  // Queue size should remain 2 (blocked pushes were dropped)
  EXPECT_EQ(queue.size(), 2);

  // Verify that existing items can still be popped
  auto value1 = folly::coro::blockingWait(queue.pop());
  EXPECT_EQ(value1, 1);

  auto value2 = folly::coro::blockingWait(queue.pop());
  EXPECT_EQ(value2, 2);

  EXPECT_EQ(queue.size(), 0);
}

/**
 * Test open() functionality to re-enable a closed queue.
 *
 * This test verifies that open() properly restores a closed queue to a
 * functional state, allowing new push operations to succeed.
 *
 * Test sequence:
 * 1. Push an item to the queue
 * 2. Close the queue
 * 3. Attempt to push while closed - should be dropped
 * 4. Reopen the queue using open()
 * 5. Push should now work again
 * 6. Verify all items can be popped in correct order
 *
 * Key behaviors verified:
 * - Pushes to closed queue are dropped
 * - open() resets the closed_ flag
 * - Queue becomes fully functional after reopening
 * - Existing and new items are accessible after reopening
 */
TEST(BackPressuredQueueTest, OpenTest) {
  BackPressuredQueue<int> queue(2);

  // Push some items to the queue
  folly::coro::blockingWait(queue.push(1));
  EXPECT_EQ(queue.size(), 1);

  // Close the queue
  queue.close();
  EXPECT_TRUE(queue.closed_);

  // Try to push to a closed queue - should be dropped
  folly::coro::blockingWait(queue.push(2));
  EXPECT_EQ(queue.size(), 1);

  // Reopen the queue
  queue.open();
  EXPECT_FALSE(queue.closed_);

  // Now push should work again
  folly::coro::blockingWait(queue.push(2));
  EXPECT_EQ(queue.size(), 2);

  // Verify both items can be popped
  auto value1 = folly::coro::blockingWait(queue.pop());
  EXPECT_EQ(value1, 1);

  auto value2 = folly::coro::blockingWait(queue.pop());
  EXPECT_EQ(value2, 2);

  EXPECT_EQ(queue.size(), 0);
}

/**
 * Test complete close/reopen lifecycle with back pressure.
 *
 * This test verifies that a queue can be closed, reopened, and continue to
 * function correctly with back pressure working as expected throughout the
 * entire lifecycle.
 *
 * Test sequence:
 * Phase 1 - Initial back pressure and close:
 * 1. Fill queue to capacity (2 items)
 * 2. Start 3 producers that block due to back pressure
 * 3. Close the queue - wakes up all blocked producers (pushes dropped)
 * 4. Attempt push to closed queue - correctly dropped
 *
 * Phase 2 - Reopen and verify back pressure still works:
 * 5. Reopen the queue using open()
 * 6. Start 2 new producers that block due to back pressure (queue still full)
 * 7. Pop items to unblock the new producers
 * 8. Verify new producers complete successfully with correct values
 *
 * Key behaviors verified:
 * - close() wakes up all back-pressured waiters
 * - Pushes to closed queue are dropped
 * - open() restores queue to fully functional state
 * - Back pressure mechanism works correctly after reopening
 * - Queue maintains data integrity through close/reopen cycle
 * - New producers can be back-pressured after reopening
 */
TEST(BackPressuredQueueTest, CloseReopenTest) {
  BackPressuredQueue<int> queue(2);
  folly::EventBase evb;

  // Fill the queue to capacity
  folly::coro::blockingWait(queue.push(1));
  folly::coro::blockingWait(queue.push(2));
  EXPECT_EQ(queue.size(), 2);

  std::atomic<int> completedPushes{0};

  auto blockedProducer = [&](int value) -> folly::coro::Task<void> {
    co_await queue.push(std::move(value));
    completedPushes++;
  };

  // Close the queue - this should wake up all blocked producers
  queue.close();
  EXPECT_TRUE(queue.closed_);

  // Try to push to a closed queue - should be dropped
  folly::coro::blockingWait(queue.push(3));
  EXPECT_EQ(queue.size(), 2); // Still 2, push was dropped

  // Reopen the queue
  queue.open();
  EXPECT_FALSE(queue.closed_);

  /*
   * Now push should work again
   * Start new producers that will be blocked due to back pressure
   */
  auto producer4 =
      folly::coro::co_withExecutor(&evb, blockedProducer(4)).start();
  auto producer5 =
      folly::coro::co_withExecutor(&evb, blockedProducer(5)).start();

  std::thread evbThread([&]() { evb.loop(); });

  // Wait for producers to be blocked on the semaphore
  while (true) {
    {
      std::lock_guard<std::mutex> lock(queue.mutex_);
      if (queue.pushRequestQueueSize_ == 2) {
        break;
      }
    }
    std::this_thread::yield();
  }

  // Verify queue is still at capacity and new producers are blocked
  EXPECT_EQ(queue.size(), 2);
  EXPECT_EQ(completedPushes.load(), 0);
  EXPECT_EQ(queue.pushRequestQueueSize_, 2);

  // Pop items to unblock the producers
  auto value1 = folly::coro::blockingWait(queue.pop());
  EXPECT_EQ(value1, 1);

  auto value2 = folly::coro::blockingWait(queue.pop());
  EXPECT_EQ(value2, 2);

  // Wait for the new producers to complete
  folly::coro::blockingWait(
      folly::coro::collectAll(std::move(producer4), std::move(producer5)));

  evb.terminateLoopSoon();
  evbThread.join();

  // Both new producers should have completed
  EXPECT_EQ(completedPushes.load(), 2);

  // Queue should have the new items
  EXPECT_EQ(queue.size(), 2);

  auto value3 = folly::coro::blockingWait(queue.pop());
  EXPECT_EQ(value3, 4);

  auto value4 = folly::coro::blockingWait(queue.pop());
  EXPECT_EQ(value4, 5);

  EXPECT_EQ(queue.size(), 0);
}

/**
 * Test ConsumerScope RAII functionality.
 *
 * This test verifies that ConsumerScope properly manages the queue's open/close
 * state using RAII semantics:
 * - Queue is open by default when constructed
 * - After close(), the queue is closed
 * - Creating a ConsumerScope opens the queue
 * - Destroying the ConsumerScope closes the queue
 *
 * Key behaviors verified:
 * - Default queue state is open (closed_ = false)
 * - close() sets closed_ to true
 * - ConsumerScope constructor calls open() to set closed_ = false
 * - ConsumerScope destructor calls close() to set closed_ = true
 */
TEST(BackPressuredQueueTest, ConsumerScopeTest) {
  BackPressuredQueue<int> queue(2);

  // By default, the queue is open
  EXPECT_FALSE(queue.closed_);

  // Close the queue
  queue.close();
  EXPECT_TRUE(queue.closed_);

  // Create a ConsumerScope, which should open the queue
  {
    BackPressuredQueue<int>::ConsumerScope scope(queue);

    // Within the scope, the queue should be open
    EXPECT_FALSE(queue.closed_);
  }

  // Outside the scope, the queue should be closed again
  EXPECT_TRUE(queue.closed_);

  // Create a ConsumerScope by getConsumerScope()
  {
    auto consumerScope = queue.getConsumerScope();

    // Within the scope, the queue should be open
    EXPECT_FALSE(queue.closed_);
  }

  // Outside the scope, the queue should be closed again
  EXPECT_TRUE(queue.closed_);
}

/**
 * Test fixture for BackPressuredQueue back pressure mechanism.
 *
 * Tests the core back pressure functionality by simulating a
 * producer-consumer scenario where the producer generates items faster than
 * the consumer can process them:
 * - Queue capacity: 2 items (kCapacity)
 * - Items to push: 5 items (kNumItems)
 *
 * This creates back pressure, forcing the producer to block when the queue
 * is full and resume when the consumer frees space.
 *
 * Verifies:
 * 1. Queue never exceeds capacity (always <= kCapacity)
 * 2. Producers block when queue is full
 * 3. Consumers unblock producers by freeing space (each pop() signals one
 * push())
 * 4. Items are delivered in order (0, 1, 2, 3, 4)
 * 5. Back pressure works correctly across different threading models:
 *    - Separate threads (BackpressureTest)
 *    - Same thread with coroutines (BackpressureSameThreadTest)
 *    - Fiber-based API (BackpressureFiberPushTest)
 */
class BackPressureTestFixture : public ::testing::Test {
 protected:
  // Common parameters
  static constexpr size_t kCapacity = 2;
  static constexpr int kNumItems = 5;

  // Member variables - initialized in each test's SetUp or constructor
  BackPressuredQueue<int> queue_{kCapacity};
  folly::fibers::Semaphore queueFullSemaphore_{0};

  /*
   * Create producer coroutine that pushes items and signals when queue is
   * full
   */
  auto createProducer() {
    return [this]() -> folly::coro::Task<void> {
      for (int i = 0; i < kNumItems; i++) {
        XLOGF(DBG1, "Pushing: {}", i);
        auto item = i;
        co_await queue_.push(std::move(item));
        XLOGF(DBG1, "Pushed: {}", i);
        // Verify we never exceed capacity
        EXPECT_LE(queue_.size(), kCapacity);
        if (queue_.size() == kCapacity) {
          queueFullSemaphore_.signal();
        }
      }
    };
  }

  // Create non-cancellable producer coroutine that uses nonCancellablePush
  auto createNonCancellableProducer() {
    return [this]() -> folly::coro::Task<void> {
      for (int i = 0; i < kNumItems; i++) {
        XLOGF(DBG1, "Pushing: {}", i);
        auto item = i;
        co_await queue_.nonCancellablePush(std::move(item));
        XLOGF(DBG1, "Pushed: {}", i);
        // Verify we never exceed capacity
        EXPECT_LE(queue_.size(), kCapacity);
        if (queue_.size() == kCapacity) {
          queueFullSemaphore_.signal();
        }
      }
    };
  }

  // Create fiber-based producer that uses fiberPush
  auto createFiberProducer() {
    return [this]() -> void {
      for (int i = 0; i < kNumItems; i++) {
        XLOGF(DBG1, "Pushing: {}", i);
        auto item = i;
        queue_.fiberPush(std::move(item));
        XLOGF(DBG1, "Pushed: {}", i);
        // Verify we never exceed capacity
        EXPECT_LE(queue_.size(), kCapacity);
        if (queue_.size() == kCapacity) {
          queueFullSemaphore_.signal();
        }
      }
    };
  }

  // Create consumer coroutine that pops items and verifies values
  auto createConsumer() {
    return [this]() -> folly::coro::Task<void> {
      for (int i = 0; i < kNumItems; i++) {
        if (i < 3) {
          // Wait for producer to fill the queue
          co_await queueFullSemaphore_.co_wait();
        }
        // Verify we never exceed capacity
        EXPECT_LE(queue_.size(), kCapacity);

        int value = co_await queue_.pop();
        XLOGF(DBG1, "Popped: {}", value);
        EXPECT_EQ(value, i);
      }
    };
  }
};

TEST_F(BackPressureTestFixture, BackpressureTest) {
  // Test back pressure with producer/consumer on separate threads
  folly::EventBase producerEvb;
  folly::EventBase consumerEvb;

  auto producer = createProducer();
  auto consumer = createConsumer();

  // Run producer and consumer on separate threads to test back pressure
  auto producerFuture = folly::coro::co_withExecutor(&producerEvb, producer());
  std::thread producerThread([&]() { producerEvb.loop(); });

  auto consumerFuture = folly::coro::co_withExecutor(&consumerEvb, consumer());
  std::thread consumerThread([&]() { consumerEvb.loop(); });

  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb.terminateLoopSoon();
  consumerEvb.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify queue is empty after all operations
  EXPECT_EQ(queue_.size(), 0);
}

TEST_F(BackPressureTestFixture, BackpressureSameThreadTest) {
  // Same as BackpressureTest but runs producer/consumer on the same thread
  folly::EventBase evb;

  auto producer = createProducer();
  auto consumer = createConsumer();

  // Run producer and consumer on the same thread to test back pressure
  auto producerFuture = folly::coro::co_withExecutor(&evb, producer());
  auto consumerFuture = folly::coro::co_withExecutor(&evb, consumer());
  std::thread runThread([&]() { evb.loop(); });

  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  evb.terminateLoopSoon();
  runThread.join();

  // Verify queue is empty after all operations
  EXPECT_EQ(queue_.size(), 0);
}

TEST_F(BackPressureTestFixture, BackpressureNonCancellablePushTest) {
  // Test back pressure with nonCancellablePush on separate threads
  folly::EventBase producerEvb;
  folly::EventBase consumerEvb;

  auto producer = createNonCancellableProducer();
  auto consumer = createConsumer();

  // Run producer and consumer on separate threads to test back pressure
  auto producerFuture = folly::coro::co_withExecutor(&producerEvb, producer());
  std::thread producerThread([&]() { producerEvb.loop(); });

  auto consumerFuture = folly::coro::co_withExecutor(&consumerEvb, consumer());
  std::thread consumerThread([&]() { consumerEvb.loop(); });

  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb.terminateLoopSoon();
  consumerEvb.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify queue is empty after all operations
  EXPECT_EQ(queue_.size(), 0);
}

TEST_F(BackPressureTestFixture, BackpressureFiberPushTest) {
  // Test backpressure with fiberPush
  folly::EventBase producerEvb;
  folly::EventBase consumerEvb;

  // Set up fiber manager for the producer
  folly::fibers::FiberManager::Options options;
  options.stackSize = 64 * 1024;
  auto manager = std::make_unique<folly::fibers::FiberManager>(
      std::make_unique<folly::fibers::EventBaseLoopController>(), options);
  static_cast<folly::fibers::EventBaseLoopController&>(
      manager->loopController())
      .attachEventBase(producerEvb);

  auto producer = createFiberProducer();
  auto consumer = createConsumer();

  // Run producer and consumer on separate threads to test back pressure
  auto producerFuture = manager->addTaskFuture([&]() { producer(); });
  std::thread producerThread([&]() { producerEvb.loop(); });

  auto consumerFuture = folly::coro::co_withExecutor(&consumerEvb, consumer());
  std::thread consumerThread([&]() { consumerEvb.loop(); });

  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb.terminateLoopSoon();
  consumerEvb.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify queue is empty after all operations
  EXPECT_EQ(queue_.size(), 0);
}

/**
 * Test that fiberPush() blocks the entire EventBase thread when called
 * from a non-fiber context (plain EventBase callback, no FiberManager).
 *
 * This verifies the dangerous behavior that caused SERVER_OVERLOAD in
 * production: fiberPush() uses folly::fibers::Semaphore::wait() which
 * requires a FiberManager to yield cooperatively. Without one, it blocks
 * the OS thread, freezing the EventBase and all its callbacks.
 *
 * Test sequence:
 * 1. Create a capacity-1 queue and fill it
 * 2. From a plain EventBase callback (no FiberManager), call fiberPush()
 * 3. Verify the EventBase thread is blocked (fiberPush doesn't return)
 * 4. Pop from another thread to unblock
 * 5. Verify fiberPush completes and the item was pushed
 */
TEST(BackPressuredQueueTest, FiberPushBlocksWithoutFiberManagerTest) {
  BackPressuredQueue<int> queue(1);

  /* Fill the queue to capacity */
  folly::coro::blockingWait(queue.push(42));
  EXPECT_EQ(queue.size(), 1);

  folly::EventBase evb;
  std::atomic<bool> fiberPushCompleted{false};

  /*
   * Schedule fiberPush from a plain EventBase callback — no FiberManager.
   * This should block the EventBase thread when the queue is full.
   */
  evb.runInEventBaseThread([&]() {
    auto item = 99;
    queue.fiberPush(std::move(item));
    fiberPushCompleted.store(true);
  });

  std::thread evbThread([&]() { evb.loopForever(); });

  /*
   * Use a separate orchestration EventBase to schedule timed events.
   * The fiberPush-blocked EventBase can't run callbacks, so we need
   * a second EventBase to drive the test sequence.
   */
  folly::EventBase orchestrator;

  /*
   * After 200ms: verify fiberPush is still blocked (the EventBase thread
   * is frozen), then pop to unblock it.
   */
  orchestrator.scheduleAt(
      [&]() {
        EXPECT_FALSE(fiberPushCompleted.load())
            << "fiberPush should block the EventBase thread when queue is "
               "full and no FiberManager is present";

        /* Pop to unblock the fiberPush */
        auto value = folly::coro::blockingWait(queue.pop());
        EXPECT_EQ(value, 42);
      },
      orchestrator.now() + std::chrono::milliseconds(200));

  /*
   * After 400ms: verify fiberPush completed and the item was pushed.
   */
  orchestrator.scheduleAt(
      [&]() {
        EXPECT_TRUE(fiberPushCompleted.load())
            << "fiberPush should complete after space becomes available";

        EXPECT_EQ(queue.size(), 1);
        auto pushedValue = folly::coro::blockingWait(queue.pop());
        EXPECT_EQ(pushedValue, 99);

        evb.terminateLoopSoon();
      },
      orchestrator.now() + std::chrono::milliseconds(400));

  orchestrator.loop();
  evbThread.join();
}

} // namespace facebook::nettools::bgplib
