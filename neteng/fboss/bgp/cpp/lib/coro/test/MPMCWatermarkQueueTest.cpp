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

#include <folly/coro/AsyncScope.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/GmockHelpers.h>
#include <folly/coro/Sleep.h>
#include <folly/fibers/Baton.h>
#include <folly/fibers/FiberManager.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <gtest/gtest.h>
#include <chrono>
#include <thread>

#include "neteng/fboss/bgp/cpp/lib/coro/MPMCWatermarkQueue.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/Utils.h"

using namespace facebook::nettools::bgplib;
using namespace std::chrono_literals;

class MPMCWatermarkQueueTest : public ::testing::Test {
 public:
  virtual folly::coro::Task<void> runCoroProducer(
      int numObjects,
      MPMCWatermarkQueue<std::optional<int>>& queue) {
    for (int i = 1; i <= numObjects; ++i) {
      EXPECT_TRUE(queue.push(i));

      // Check if queue became blocked and increment counter
      if (queue.isBlocked()) {
        ++blockedCount_;
      }
    }
    // Push termination signal
    EXPECT_TRUE(queue.push(std::nullopt));
    co_return;
  }

  folly::coro::Task<void> runCoroConsumer(
      int taskSleepMs,
      MPMCWatermarkQueue<std::optional<int>>& queue) {
    while (true) {
      auto item = co_await queue.pop();

      if (!item.has_value()) {
        break;
      }

      // Increment counter for actual objects (not termination signal)
      ++objectsRead_;
      /*
       * We are passing indices as the object, so the number of objects read
       * should be equal to the value of the item.
       */
      EXPECT_EQ(*item, objectsRead_);

      if (taskSleepMs > 0) {
        // Use sleepReturnEarlyOnCancel for cancellation-friendly sleep
        co_await folly::coro::sleepReturnEarlyOnCancel(
            std::chrono::milliseconds(taskSleepMs));
      }
    }
    co_return;
  }

  // The producer should run on this event base only.
  folly::EventBase producerEvb_;
  // The consumer should run on this event base only.
  folly::EventBase consumerEvb_;

  // Track how many times queue was blocked during test run.
  int blockedCount_ = 0;

  /*
   * Track number of objects read (excluding termination signal) during test
   * run.
   */
  int objectsRead_ = 0;
};

TEST_F(MPMCWatermarkQueueTest, QueueConstructorTest) {
  int capacity = 3;
  int highWm = 2;
  int lowWm = 1;

  EXPECT_DEATH(MPMCWatermarkQueue<int>(lowWm, highWm, lowWm), "");
  EXPECT_DEATH(MPMCWatermarkQueue<int>(capacity, highWm, highWm), "");
  EXPECT_DEATH(MPMCWatermarkQueue<int>(capacity, capacity, highWm), "");
}

TEST_F(MPMCWatermarkQueueTest, BasicProducerConsumer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 10;
  int taskSleepMs = 1;

  // High watermark much larger than numObjects to avoid blocking
  MPMCWatermarkQueue<std::optional<int>> queue(
      101 /* capacity */, 100 /* highWm */, 10 /* lowWm */);

  // Schedule coroutines and capture futures to wait for completion
  auto producerFuture =
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue));
  auto consumerFuture =
      co_withExecutor(&consumerEvb_, runCoroConsumer(taskSleepMs, queue));

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Wait for both coroutines to complete concurrently
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify queue was never blocked
  EXPECT_EQ(blockedCount_, 0);

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

TEST_F(MPMCWatermarkQueueTest, PushExceedsCapacityTest) {
  MPMCWatermarkQueue<int> queue(
      2 /* capacity */, 1 /* highWm */, 0 /* lowWm */);

  EXPECT_FALSE(queue.full());
  EXPECT_FALSE(queue.isBlocked());

  EXPECT_TRUE(queue.push(1));
  EXPECT_FALSE(queue.full());
  EXPECT_TRUE(queue.isBlocked());

  EXPECT_TRUE(queue.push(2));
  EXPECT_TRUE(queue.full());
  EXPECT_TRUE(queue.isBlocked());

  EXPECT_FALSE(queue.push(3));
  EXPECT_TRUE(queue.full());
  EXPECT_TRUE(queue.isBlocked());
  EXPECT_EQ(2, queue.size());
}

TEST_F(MPMCWatermarkQueueTest, SlowConsumerBlocksProducer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 1000;
  int taskSleepMs = 1;

  /*
   * Set watermarks to trigger blocking due to highWm being lower than
   * number of objects.
   */
  MPMCWatermarkQueue<std::optional<int>> queue(
      2000 /* capacity */, 100 /* highWm */, 10 /* lowWm */);

  // Schedule coroutines and capture futures to wait for completion
  auto producerFuture =
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue));
  auto consumerFuture =
      co_withExecutor(&consumerEvb_, runCoroConsumer(taskSleepMs, queue));

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Wait for both coroutines to complete concurrently
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify that producer experienced blocking due to slow consumer
  EXPECT_GT(blockedCount_, 0);

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

TEST_F(MPMCWatermarkQueueTest, ZeroObjectsProduced) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 0;
  int taskSleepMs = 10;

  MPMCWatermarkQueue<std::optional<int>> queue(
      101 /* capacity */, 100 /* highWm */, 10 /* lowWm */);

  // Schedule coroutines and capture futures to wait for completion
  auto producerFuture =
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue));
  auto consumerFuture =
      co_withExecutor(&consumerEvb_, runCoroConsumer(taskSleepMs, queue));

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Wait for both coroutines to complete concurrently
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Should never block with zero objects
  EXPECT_EQ(blockedCount_, 0);

  // Should read exactly zero objects
  EXPECT_EQ(objectsRead_, numObjects);
}

TEST_F(MPMCWatermarkQueueTest, ZeroConsumerSleep) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 100;
  int taskSleepMs = 0;

  MPMCWatermarkQueue<std::optional<int>> queue(
      201 /* capacity */, 200 /* highWm */, 20 /* lowWm */);

  // Schedule coroutines and capture futures to wait for completion
  auto producerFuture =
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue));
  auto consumerFuture =
      co_withExecutor(&consumerEvb_, runCoroConsumer(taskSleepMs, queue));

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Wait for both coroutines to complete concurrently
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  /*
   * With zero sleep, consumer should be very fast, likely no blocking
   * But we don't assert this since it depends on timing
   */

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

class MPMCWatermarkQueueSuspendProducerTest : public MPMCWatermarkQueueTest {
 public:
  folly::coro::Task<void> runCoroProducer(
      int numObjects,
      MPMCWatermarkQueue<std::optional<int>>& queue) override {
    for (int i = 1; i <= numObjects; ++i) {
      EXPECT_TRUE(queue.push(i));

      // Check if queue became blocked after pushing and wait if so
      if (queue.isBlocked()) {
        ++blockedCount_;

        /*
         * For any tests that might want to wait for the queue to
         * enter blocked state.
         */
        producerBaton_.post();

        bool canContinue = co_await queue.waitToPush();
        if (!canContinue) {
          /*
           * Queue was closed, stop producing
           */
          co_return;
        }
      }
    }
    // Push termination signal
    EXPECT_TRUE(queue.push(std::nullopt));
    co_return;
  }

  folly::fibers::Baton producerBaton_;
};

TEST_F(MPMCWatermarkQueueSuspendProducerTest, BasicProducerConsumer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 10;
  int taskSleepMs = 1;

  // High watermark much larger than numObjects to avoid blocking
  MPMCWatermarkQueue<std::optional<int>> queue(
      101 /* capacity */, 100 /* highWm */, 10 /* lowWm */);

  // Schedule coroutines and capture futures to wait for completion
  auto producerFuture =
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue));
  auto consumerFuture =
      co_withExecutor(&consumerEvb_, runCoroConsumer(taskSleepMs, queue));

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Wait for both coroutines to complete concurrently
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify queue was never blocked
  EXPECT_EQ(blockedCount_, 0);

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

TEST_F(MPMCWatermarkQueueSuspendProducerTest, SlowConsumerSuspendsProducer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 2;
  int taskSleepMs = 1;

  // Set watermarks to trigger blocking on every push.
  MPMCWatermarkQueue<std::optional<int>> queue(
      2 /* capacity */, 1 /* highWm */, 0 /* lowWm */);

  // Schedule coroutines and capture futures to wait for completion
  auto producerFuture =
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue));
  auto consumerFuture =
      co_withExecutor(&consumerEvb_, runCoroConsumer(taskSleepMs, queue));

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Wait for both coroutines to complete concurrently
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify that producer experienced due to slow consumer twice.
  EXPECT_EQ(blockedCount_, numObjects);

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

TEST_F(MPMCWatermarkQueueSuspendProducerTest, ZeroObjectsProduced) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 0;
  int taskSleepMs = 10;

  MPMCWatermarkQueue<std::optional<int>> queue(
      101 /* capacity */, 100 /* highWm */, 10 /* lowWm */);

  // Schedule coroutines and capture futures to wait for completion
  auto producerFuture =
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue));
  auto consumerFuture =
      co_withExecutor(&consumerEvb_, runCoroConsumer(taskSleepMs, queue));

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Wait for both coroutines to complete concurrently
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Should never block with zero objects
  EXPECT_EQ(blockedCount_, 0);

  // Should read exactly zero objects
  EXPECT_EQ(objectsRead_, numObjects);
}

TEST_F(MPMCWatermarkQueueSuspendProducerTest, ZeroConsumerSleep) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 100;
  int taskSleepMs = 0;

  MPMCWatermarkQueue<std::optional<int>> queue(
      201 /* capacity */, 200 /* highWm */, 20 /* lowWm */);

  // Schedule coroutines and capture futures to wait for completion
  auto producerFuture =
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue));
  auto consumerFuture =
      co_withExecutor(&consumerEvb_, runCoroConsumer(taskSleepMs, queue));

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Wait for both coroutines to complete concurrently
  folly::coro::blockingWait(
      folly::coro::collectAll(
          std::move(producerFuture), std::move(consumerFuture)));

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  /*
   * With zero sleep, consumer should be very fast, likely no blocking
   * But we don't assert this since it depends on timing
   */

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

TEST_F(
    MPMCWatermarkQueueSuspendProducerTest,
    SuspendedProducerWithCancellationToken) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 2;

  // Create queue with hiWm=1 to trigger blocking quickly
  MPMCWatermarkQueue<std::optional<int>> queue(
      4 /* capacity */, 1 /* highWm */, 0 /* lowWm */);

  // Create AsyncScope to manage the producer coroutine
  folly::coro::CancellableAsyncScope asyncScope;

  // Add the producer to the asyncScope with producerEvb
  asyncScope.add(
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue)));

  // Run the producerEvb in a separate thread
  std::thread producerThread([&]() { producerEvb_.loop(); });

  // Wait for the queue to be blocked.
  producerBaton_.wait();

  // Verify that there is 1 job remaining on the asyncScope
  EXPECT_EQ(asyncScope.remaining(), 1);

  // Check the queue is blocked.
  EXPECT_TRUE(queue.isBlocked());

  // Cancel the asyncScope
  folly::coro::blockingWait(asyncScope.cancelAndJoinAsync());

  // Stop the event loop
  producerEvb_.terminateLoopSoon();

  // Wait for thread to finish
  producerThread.join();

  /*
   * With the close() API, when the queue is destroyed (destructor calls
   * close()), the waitToPush() returns false, causing the producer to exit
   * early. So only the first object will cause blocking.
   */
  EXPECT_EQ(blockedCount_, 1);

  // Note: We don't schedule any consumer, so objectsRead_ should be 0
  EXPECT_EQ(objectsRead_, 0);
}

TEST_F(MPMCWatermarkQueueSuspendProducerTest, CloseWakesUpBlockedProducer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 2;

  // Create queue with hiWm=1 to trigger blocking quickly
  MPMCWatermarkQueue<std::optional<int>> queue(
      4 /* capacity */, 1 /* highWm */, 0 /* lowWm */);

  // Create AsyncScope to manage the producer coroutine
  folly::coro::CancellableAsyncScope asyncScope;

  // Add the producer to the asyncScope with producerEvb
  asyncScope.add(
      co_withExecutor(&producerEvb_, runCoroProducer(numObjects, queue)));

  // Run the producerEvb in a separate thread
  std::thread producerThread([&]() { producerEvb_.loop(); });

  // Wait for the queue to be blocked.
  producerBaton_.wait();

  // Verify that there is 1 job remaining on the asyncScope
  EXPECT_EQ(asyncScope.remaining(), 1);

  // Check the queue is blocked.
  EXPECT_TRUE(queue.isBlocked());

  // Close the queue (should wake up the blocked producer)
  queue.close();

  // Cancel and join the asyncScope
  folly::coro::blockingWait(asyncScope.cancelAndJoinAsync());

  // Stop the event loop
  producerEvb_.terminateLoopSoon();

  // Wait for thread to finish
  producerThread.join();

  /*
   * The producer blocked once after the first push.
   * Then close() was called, which caused waitToPush() to return false,
   * so the producer exited without pushing the second object.
   */
  EXPECT_EQ(blockedCount_, 1);

  // No consumer, so objectsRead_ should be 0
  EXPECT_EQ(objectsRead_, 0);

  // Queue should be closed
  EXPECT_TRUE(queue.isClosed());
}

class CoroProducerFiberConsumerQueueTest : public MPMCWatermarkQueueTest {
 public:
  void runFiberConsumer(
      int taskSleepMs,
      MPMCWatermarkQueue<std::optional<int>>& queue) {
    while (true) {
      auto item = queue.get();

      if (!item) {
        // Unexpected empty value.
        break;
      }

      if (!item.value()) {
        // Received termination signal.
        break;
      }

      // Increment counter for actual objects (not termination signal)
      ++objectsRead_;

      /*
       * The item enumerates its own idx; so we can use objectsRead_
       * to check the value of the item on each read.
       */
      EXPECT_EQ(*item, objectsRead_);

      if (taskSleepMs > 0) {
        fiberSleepFor(std::chrono::milliseconds(taskSleepMs));
      }
    }
  }
};

CO_TEST_F(CoroProducerFiberConsumerQueueTest, BasicProducerConsumer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 10;
  int taskSleepMs = 1;

  // High watermark much larger than numObjects to avoid blocking
  MPMCWatermarkQueue<std::optional<int>> queue(
      101 /* capacity */, 100 /* highWm */, 10 /* lowWm */);

  // Schedule producer coroutine
  auto producerFuture = folly::coro::co_withExecutor(
      &producerEvb_, runCoroProducer(numObjects, queue));

  // Get FiberManager for the consumer event base and schedule fiber consumer
  auto& fiberManager = folly::fibers::getFiberManager(consumerEvb_);
  auto consumerFuture = fiberManager.addTaskFuture(
      [this, taskSleepMs, &queue]() { runFiberConsumer(taskSleepMs, queue); });

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Ensure producer and consumer both finish async before loop termination.
  co_await std::move(producerFuture);
  co_await std::move(consumerFuture);

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify queue was never blocked
  EXPECT_EQ(blockedCount_, 0);

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

CO_TEST_F(CoroProducerFiberConsumerQueueTest, SlowConsumerBlocksProducer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 1000;
  int taskSleepMs = 1;

  /*
   * Set watermarks to trigger blocking due to highWm being lower than
   * number of objects.
   */
  MPMCWatermarkQueue<std::optional<int>> queue(
      2000 /* capacity */, 100 /* highWm */, 10 /* lowWm */);

  // Schedule producer coroutine
  auto producerFuture = folly::coro::co_withExecutor(
      &producerEvb_, runCoroProducer(numObjects, queue));

  // Get FiberManager for the consumer event base and schedule fiber consumer
  auto& fiberManager = folly::fibers::getFiberManager(consumerEvb_);
  auto consumerFuture = fiberManager.addTaskFuture(
      [this, taskSleepMs, &queue]() { runFiberConsumer(taskSleepMs, queue); });

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Ensure producer and consumer both finish async before loop termination.
  co_await std::move(producerFuture);
  co_await std::move(consumerFuture);

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify that producer experienced blocking due to slow consumer
  EXPECT_GT(blockedCount_, 0);

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

class SuspendCoroProducerFiberConsumerQueueTest
    : public MPMCWatermarkQueueSuspendProducerTest {
 public:
  void runFiberConsumer(
      int taskSleepMs,
      MPMCWatermarkQueue<std::optional<int>>& queue) {
    while (true) {
      auto item = queue.get();

      if (!item) {
        // Received invalid value.
        break;
      }

      if (!item.value()) {
        // Received termination signal.
        break;
      }

      ++objectsRead_;

      /*
       * The item enumerates its own idx; so we can use objectsRead_
       * to check the value of the item on each read.
       */
      EXPECT_EQ(*item, objectsRead_);

      if (taskSleepMs > 0) {
        fiberSleepFor(std::chrono::milliseconds(taskSleepMs));
      }
    }
  }
};

CO_TEST_F(SuspendCoroProducerFiberConsumerQueueTest, BasicProducerConsumer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 10;
  int taskSleepMs = 1;

  // High watermark much larger than numObjects to avoid blocking
  MPMCWatermarkQueue<std::optional<int>> queue(
      101 /* capacity */, 100 /* highWm */, 10 /* lowWm */);

  // Schedule suspending producer coroutine
  auto producerFuture = folly::coro::co_withExecutor(
      &producerEvb_, runCoroProducer(numObjects, queue));

  // Get FiberManager for the consumer event base and schedule fiber consumer
  auto& fiberManager = folly::fibers::getFiberManager(consumerEvb_);
  auto consumerFuture = fiberManager.addTaskFuture(
      [this, taskSleepMs, &queue]() { runFiberConsumer(taskSleepMs, queue); });

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Ensure producer and consumer both finish async before loop termination.
  co_await std::move(producerFuture);
  co_await std::move(consumerFuture);

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify queue was never blocked
  EXPECT_EQ(blockedCount_, 0);

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

CO_TEST_F(
    SuspendCoroProducerFiberConsumerQueueTest,
    SlowConsumerSuspendsProducer) {
  blockedCount_ = 0;
  objectsRead_ = 0;

  int numObjects = 10;
  int taskSleepMs = 1;

  // Set watermarks to trigger blocking on every push.
  MPMCWatermarkQueue<std::optional<int>> queue(
      2 /* capacity */, 1 /* highWm */, 0 /* lowWm */);

  // Schedule suspending producer coroutine
  auto producerFuture = folly::coro::co_withExecutor(
      &producerEvb_, runCoroProducer(numObjects, queue));

  // Get FiberManager for the consumer event base and schedule fiber consumer
  auto& fiberManager = folly::fibers::getFiberManager(consumerEvb_);
  auto consumerFuture = fiberManager.addTaskFuture(
      [this, taskSleepMs, &queue]() { runFiberConsumer(taskSleepMs, queue); });

  // Run both event bases in separate threads
  std::thread producerThread([&]() { producerEvb_.loop(); });
  std::thread consumerThread([&]() { consumerEvb_.loop(); });

  // Ensure producer and consumer both finish async before loop termination.
  co_await std::move(producerFuture);
  co_await std::move(consumerFuture);

  producerEvb_.terminateLoopSoon();
  consumerEvb_.terminateLoopSoon();
  producerThread.join();
  consumerThread.join();

  // Verify that suspending producer experienced blocking due to slow consumer
  EXPECT_EQ(blockedCount_, numObjects);

  // Verify all objects were consumed
  EXPECT_EQ(objectsRead_, numObjects);
}

/*
 * Tests for close() API
 */

TEST_F(MPMCWatermarkQueueTest, CloseBasicFunctionality) {
  MPMCWatermarkQueue<int> queue(
      10 /* capacity */, 5 /* highWm */, 2 /* lowWm */);

  // Queue should not be closed initially
  EXPECT_FALSE(queue.isClosed());

  // Push should succeed before close
  EXPECT_TRUE(queue.push(1));
  EXPECT_EQ(queue.size(), 1);

  // Close the queue
  queue.close();

  // Queue should be closed
  EXPECT_TRUE(queue.isClosed());

  // Push should fail after close
  EXPECT_FALSE(queue.push(2));

  // Size should remain unchanged
  EXPECT_EQ(queue.size(), 1);
}

TEST_F(MPMCWatermarkQueueTest, CloseMultipleCalls) {
  MPMCWatermarkQueue<int> queue(
      10 /* capacity */, 5 /* highWm */, 2 /* lowWm */);

  EXPECT_FALSE(queue.isClosed());

  // First close
  queue.close();
  EXPECT_TRUE(queue.isClosed());

  // Second close should be safe
  queue.close();
  EXPECT_TRUE(queue.isClosed());

  // Third close should also be safe
  queue.close();
  EXPECT_TRUE(queue.isClosed());

  // Push should still fail
  EXPECT_FALSE(queue.push(1));
}

CO_TEST_F(MPMCWatermarkQueueTest, CloseWakesUpWaitingProducer) {
  /*
   * Test that close() wakes up a producer blocked on waitToPush()
   */
  MPMCWatermarkQueue<int> queue(
      4 /* capacity */, 1 /* highWm */, 0 /* lowWm */);

  // Push one item to trigger blocking
  EXPECT_TRUE(queue.push(1));
  EXPECT_TRUE(queue.isBlocked());

  std::atomic<bool> waitReturnedFalse{false};

  // Create a producer that will wait
  auto producerTask = [&]() -> folly::coro::Task<void> {
    bool result = co_await queue.waitToPush();
    if (!result) {
      waitReturnedFalse = true;
    }
    co_return;
  };

  // Schedule the producer on producerEvb_
  auto producerFuture =
      folly::coro::co_withExecutor(&producerEvb_, producerTask());

  // Run producer event base in separate thread
  std::thread producerThread([&]() { producerEvb_.loop(); });

  // Close the queue (will wake up the producer when it waits)
  queue.close();

  // Wait for producer to complete
  co_await std::move(producerFuture);

  // waitToPush() should have returned false
  EXPECT_TRUE(waitReturnedFalse.load());

  producerEvb_.terminateLoopSoon();
  producerThread.join();

  EXPECT_TRUE(queue.isClosed());
}

CO_TEST_F(MPMCWatermarkQueueTest, CloseWakesUpMultipleWaitingProducers) {
  /*
   * Test that close() wakes up multiple producers blocked on waitToPush()
   */
  MPMCWatermarkQueue<int> queue(
      4 /* capacity */, 1 /* highWm */, 0 /* lowWm */);

  // Push one item to trigger blocking
  EXPECT_TRUE(queue.push(1));
  EXPECT_TRUE(queue.isBlocked());

  // Create multiple producers that will wait
  auto producerTask = [&]() -> folly::coro::Task<bool> {
    bool result = co_await queue.waitToPush();
    co_return result;
  };

  // Schedule three producers
  auto producer1Future =
      folly::coro::co_withExecutor(&producerEvb_, producerTask());
  auto producer2Future =
      folly::coro::co_withExecutor(&producerEvb_, producerTask());
  auto producer3Future =
      folly::coro::co_withExecutor(&producerEvb_, producerTask());

  // Run producer event base in separate thread
  std::thread producerThread([&]() { producerEvb_.loop(); });

  // Close the queue (will wake up all producers when they wait)
  queue.close();

  // Wait for all producers to complete
  auto results = co_await folly::coro::collectAll(
      std::move(producer1Future),
      std::move(producer2Future),
      std::move(producer3Future));

  // All waitToPush() calls should return false because queue was closed
  EXPECT_FALSE(std::get<0>(results));
  EXPECT_FALSE(std::get<1>(results));
  EXPECT_FALSE(std::get<2>(results));

  producerEvb_.terminateLoopSoon();
  producerThread.join();

  EXPECT_TRUE(queue.isClosed());
}

CO_TEST_F(MPMCWatermarkQueueTest, WaitToPushReturnsImmediatelyWhenClosed) {
  MPMCWatermarkQueue<int> queue(
      10 /* capacity */, 5 /* highWm */, 2 /* lowWm */);

  // Close the queue first
  queue.close();
  EXPECT_TRUE(queue.isClosed());

  // waitToPush() should return immediately with false
  auto producerTask = [&]() -> folly::coro::Task<bool> {
    bool result = co_await queue.waitToPush();
    co_return result;
  };

  auto producerFuture =
      folly::coro::co_withExecutor(&producerEvb_, producerTask());

  std::thread producerThread([&]() { producerEvb_.loop(); });

  // Wait for producer to complete
  bool waitResult = co_await std::move(producerFuture);

  // Should return false immediately without blocking
  EXPECT_FALSE(waitResult);

  producerEvb_.terminateLoopSoon();
  producerThread.join();
}

TEST_F(MPMCWatermarkQueueTest, DestructorCallsClose) {
  // Create queue in a scope that will destroy it
  auto queuePtr = std::make_unique<MPMCWatermarkQueue<int>>(
      10 /* capacity */, 5 /* highWm */, 2 /* lowWm */);

  EXPECT_FALSE(queuePtr->isClosed());

  // Add some items
  EXPECT_TRUE(queuePtr->push(1));
  EXPECT_TRUE(queuePtr->push(2));

  // Destroy the queue (destructor should call close)
  queuePtr.reset();

  // Test passes if no crashes or hangs occur during destruction
}

TEST_F(MPMCWatermarkQueueTest, StaleTokenFixVerification) {
  // Capacity 4, High 3, Low 1
  MPMCWatermarkQueue<int> queue(4, 3, 1);
  folly::EventBase evb;

  // 1. Fill queue to block it
  EXPECT_TRUE(queue.push(1));
  EXPECT_TRUE(queue.push(2));
  EXPECT_TRUE(queue.push(3));
  EXPECT_TRUE(queue.push(4));
  EXPECT_TRUE(queue.isBlocked());

  // 2. Drain queue to unblock it (generates Stale Token)
  EXPECT_EQ(queue.get(), 1);
  EXPECT_EQ(queue.get(), 2);
  EXPECT_EQ(queue.get(), 3);
  EXPECT_FALSE(queue.isBlocked());

  // 3. Fill queue again to block it
  EXPECT_TRUE(queue.push(5));
  EXPECT_TRUE(queue.push(6));
  EXPECT_TRUE(queue.push(7));
  EXPECT_TRUE(queue.isBlocked());

  /*
   * 4. Call waitToPush() in a separate thread.
   * WITH FIX: It should ignore the stale token and BLOCK.
   */

  std::atomic<bool> done{false};
  std::atomic<bool> result{false};

  std::thread waiter([&]() {
    result = folly::coro::blockingWait(queue.waitToPush());
    done = true;
  });

  // Give it time to consume the stale token and loop back to wait
  /* sleep override */
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  EXPECT_FALSE(done) << "waitToPush() should be blocked despite stale token";

  // 5. Now drain the queue to actually unblock it
  EXPECT_EQ(queue.get(), 4);
  EXPECT_EQ(queue.get(), 5);
  EXPECT_EQ(queue.get(), 6); // Drained below lowWm (1)

  // 6. Now the waiter should complete
  waiter.join();

  EXPECT_TRUE(done) << "waitToPush() should complete after valid drain";
  EXPECT_TRUE(result) << "waitToPush() should return true";

  // Verify we can push now
  EXPECT_TRUE(queue.push(99));
}
