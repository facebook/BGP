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

/**
 * This test demonstrates the issue with coroutine cancellation and semaphore
 * signaling when using AsyncScope.
 *
 * The bug scenario:
 * 1. A coroutine task is added to an AsyncScope
 * 2. The task loops and processes messages from a queue
 * 3. Cancellation is requested while the task is processing
 * 4. An OperationCancelled exception propagates up
 * 5. The semaphore signal at the end of the function is skipped
 * 6. Code waiting on the semaphore blocks forever (deadlock)
 *
 * The fix:
 * Use SCOPE_EXIT or RAII to guarantee the semaphore is signaled regardless
 * of how the coroutine exits (normal, exception, or cancellation).
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/ScopeGuard.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/fibers/BatchSemaphore.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

namespace facebook::bgp::test {

using namespace std::chrono_literals;

/**
 * Negative test loop - demonstrates the bug WITHOUT SCOPE_EXIT.
 *
 * When cancellation is requested:
 * - The inner co_await (simulated by sleep) throws OperationCancelled
 * - The exception bypasses terminateSemaphore->signal(1)
 * - The semaphore is never signaled
 */
folly::coro::Task<void> negativeTestLoop(
    std::shared_ptr<folly::fibers::BatchSemaphore> terminateSemaphore) {
  XLOG(INFO, "negativeTestLoop: Starting");

  while (true) {
    const auto& token = co_await folly::coro::co_current_cancellation_token;
    if (token.isCancellationRequested()) {
      XLOG(INFO, "negativeTestLoop: Cancellation detected at loop start");
      break;
    }

    /*
     * Simulate processing that can be cancelled
     * This co_await will throw OperationCancelled when cancellation is
     * requested while we're sleeping
     */
    co_await folly::coro::sleep(10ms);

    // Check for safe point cancellation
    co_await folly::coro::co_safe_point;
  }

  // This line is SKIPPED if an exception propagates from above!
  terminateSemaphore->signal(1);
  XLOG(INFO, "negativeTestLoop: Signaled semaphore (normal exit)");
}

/**
 * Positive test loop - demonstrates the fix WITH SCOPE_EXIT.
 *
 * SCOPE_EXIT guarantees the semaphore is signaled regardless of how
 * the function exits (normal return, break, or exception).
 */
folly::coro::Task<void> positiveTestLoop(
    std::shared_ptr<folly::fibers::BatchSemaphore> terminateSemaphore) {
  XLOG(INFO, "positiveTestLoop: Starting");

  // SCOPE_EXIT ensures cleanup happens no matter how we exit
  SCOPE_EXIT {
    terminateSemaphore->signal(1);
    XLOG(INFO, "positiveTestLoop: Signaled semaphore (SCOPE_EXIT)");
  };

  while (true) {
    const auto& token = co_await folly::coro::co_current_cancellation_token;
    if (token.isCancellationRequested()) {
      XLOG(INFO, "positiveTestLoop: Cancellation detected at loop start");
      break;
    }

    // Even if this throws OperationCancelled, SCOPE_EXIT will signal
    try {
      co_await folly::coro::sleep(10ms);
    } catch (const folly::OperationCancelled&) {
      XLOG(INFO, "positiveTestLoop: Caught OperationCancelled in sleep");
      break;
    }

    co_await folly::coro::co_safe_point;
  }

  XLOG(INFO, "positiveTestLoop: Normal exit path");
}

/**
 * Alternative positive test loop - demonstrates the fix with try-catch.
 */
folly::coro::Task<void> positiveTestLoopWithTryCatch(
    std::shared_ptr<folly::fibers::BatchSemaphore> terminateSemaphore) {
  XLOG(INFO, "positiveTestLoopWithTryCatch: Starting");

  try {
    while (true) {
      const auto& token = co_await folly::coro::co_current_cancellation_token;
      if (token.isCancellationRequested()) {
        XLOG(
            INFO,
            "positiveTestLoopWithTryCatch: Cancellation detected at loop start");
        break;
      }

      co_await folly::coro::sleep(10ms);
      co_await folly::coro::co_safe_point;
    }
  } catch (const folly::OperationCancelled& ex) {
    XLOGF(
        INFO,
        "positiveTestLoopWithTryCatch: Caught OperationCancelled: {}",
        ex.what());
  } catch (const std::exception& ex) {
    XLOGF(
        INFO, "positiveTestLoopWithTryCatch: Caught exception: {}", ex.what());
  }

  // This is now guaranteed to execute
  terminateSemaphore->signal(1);
  XLOG(INFO, "positiveTestLoopWithTryCatch: Signaled semaphore");
}

class CoroScopeCancellationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    semaphore_ = std::make_shared<folly::fibers::BatchSemaphore>(0);
  }

  std::shared_ptr<folly::fibers::BatchSemaphore> semaphore_;
  folly::EventBase evb_;
};

/**
 * Negative test: Verify the bug - semaphore NOT signaled when cancellation
 * causes exception to propagate.
 *
 * This test demonstrates the problem:
 * - requestCancellation() is called
 * - The sleep() throws OperationCancelled
 * - The exception propagates and bypasses semaphore->signal()
 */
TEST_F(CoroScopeCancellationTest, NegativeTest_SemaphoreNotSignaled) {
  folly::coro::CancellableAsyncScope asyncScope;

  // Add the negative test loop to the async scope
  asyncScope.add(co_withExecutor(&evb_, negativeTestLoop(semaphore_)));

  // Give the loop time to start and block on sleep
  evb_.loopOnce();
  folly::coro::blockingWait(folly::coro::sleep(50ms));

  // Request cancellation - this will cause sleep to throw OperationCancelled
  asyncScope.requestCancellation();

  // Run event loop to process cancellation
  evb_.loopOnce();

  // Wait for the task to complete - verifies loop exited
  folly::coro::blockingWait(asyncScope.joinAsync());

  /*
   * BUG: The semaphore was NOT signaled because the exception
   * bypassed the signal() call
   */
  bool signaled = semaphore_->try_wait(1);
  EXPECT_FALSE(signaled)
      << "BUG DEMONSTRATED: Semaphore should NOT be signaled "
         "in the negative test when exception propagates";

  XLOG(
      INFO,
      "Negative test confirmed: Semaphore was NOT signaled (bug scenario)");
}

/**
 * Positive test: Verify the fix with SCOPE_EXIT - semaphore IS signaled
 * even when cancellation occurs.
 */
TEST_F(CoroScopeCancellationTest, PositiveTest_SemaphoreSignaledWithScopeExit) {
  folly::coro::CancellableAsyncScope asyncScope;

  // Add the positive test loop to the async scope
  asyncScope.add(co_withExecutor(&evb_, positiveTestLoop(semaphore_)));

  // Give the loop time to start
  evb_.loopOnce();
  folly::coro::blockingWait(folly::coro::sleep(50ms));

  // Request cancellation
  asyncScope.requestCancellation();

  // Run event loop to process cancellation
  evb_.loopOnce();

  // Wait for the task to complete - verifies loop exited
  folly::coro::blockingWait(asyncScope.joinAsync());

  // FIX: The semaphore IS signaled thanks to SCOPE_EXIT
  bool signaled = semaphore_->try_wait(1);
  EXPECT_TRUE(signaled)
      << "FIX VERIFIED: Semaphore should be signaled with SCOPE_EXIT";

  XLOG(
      INFO,
      "Positive test confirmed: Semaphore WAS signaled (fixed with SCOPE_EXIT)");
}

/**
 * Positive test: Verify the alternative fix with try-catch
 */
TEST_F(CoroScopeCancellationTest, PositiveTest_SemaphoreSignaledWithTryCatch) {
  folly::coro::CancellableAsyncScope asyncScope;

  // Add the positive test loop to the async scope
  asyncScope.add(
      co_withExecutor(&evb_, positiveTestLoopWithTryCatch(semaphore_)));

  // Give the loop time to start
  evb_.loopOnce();
  folly::coro::blockingWait(folly::coro::sleep(50ms));

  // Request cancellation
  asyncScope.requestCancellation();

  // Run event loop to process cancellation
  evb_.loopOnce();

  // Wait for the task to complete - verifies loop exited
  folly::coro::blockingWait(asyncScope.joinAsync());

  // FIX: The semaphore IS signaled thanks to try-catch
  bool signaled = semaphore_->try_wait(1);
  EXPECT_TRUE(signaled)
      << "FIX VERIFIED: Semaphore should be signaled with try-catch";

  XLOG(
      INFO,
      "Positive test confirmed: Semaphore WAS signaled (fixed with try-catch)");
}

/**
 * Test: Demonstrate what happens with co_awaitTry and dereferencing
 * a failed Try
 */
TEST_F(CoroScopeCancellationTest, DereferencingFailedTryThrows) {
  // This test shows that *try on a failed Try throws the stored exception
  folly::Try<int> failedTry = folly::Try<int>(
      folly::make_exception_wrapper<folly::OperationCancelled>());

  EXPECT_TRUE(failedTry.hasException());
  EXPECT_TRUE(failedTry.hasException<folly::OperationCancelled>());

  // Dereferencing throws
  EXPECT_THROW(failedTry.value(), folly::OperationCancelled);
  EXPECT_THROW(*failedTry, folly::OperationCancelled);

  XLOG(INFO, "Confirmed: Dereferencing a failed Try throws the exception");
}

} // namespace facebook::bgp::test
