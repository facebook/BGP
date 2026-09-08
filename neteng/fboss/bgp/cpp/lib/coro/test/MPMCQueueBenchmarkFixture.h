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
#include <thread>
#include <vector>

#include <folly/Benchmark.h>
#include <folly/Synchronized.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/Collect.h>
#include <folly/fibers/BatchSemaphore.h>
#include <folly/io/async/EventBase.h>
#include <folly/synchronization/Baton.h>

namespace facebook::nettools::bgplib {

enum class ThreadModel {
  // Each producer/consumer runs on its own thread with its own event base
  SEPARATE_THREAD,
  /*
   * All producers run on a single thread, all consumers run on another single
   * thread
   */
  SINGLE_THREAD,
  // All producers and consumers run on the same single thread
  ALL_IN_ONE,
};

/*
 * MPMCQueueBenchmarkFixture is a test fixture for MPMCQueue benchmark tests.
 *
 * This fixture runs concurrent benchmarks to test Multi-Producer Multi-Consumer
 * (MPMC) queue performance:
 *
 * 1. Creates producer and consumer threads based on the ThreadModel:
 *    - SEPARATE_THREAD: Each producer/consumer on its own thread
 *    - SINGLE_THREAD: All producers on one thread, all consumers on another
 *    - ALL_IN_ONE: All producers and consumers share a single thread
 *
 * 2. Executes concurrent operations where:
 *    - Producer threads run a provided producer function to enqueue items
 *    - Consumer threads run a provided consumer function to dequeue items
 *    - All operations are executed concurrently using coroutines
 *
 * 3. Measures queue throughput by excluding thread setup/teardown overhead
 *    from timing using BENCHMARK_SUSPEND macros
 *
 * 4. Key characteristics:
 *    - Templated on queue type and item type (defaults to unsigned int)
 *    - Uses folly::coro::AsyncScope to manage multiple concurrent coroutines
 *    - Supports custom producer and consumer functions for flexibility
 *    - Configurable threading model for different concurrency patterns
 *
 * This enables benchmarking different MPMC queue implementations under various
 * producer/consumer ratios and threading patterns.
 */

template <typename QueueType, typename ItemType = unsigned int>
class MPMCQueueBenchmarkFixture {
 public:
  MPMCQueueBenchmarkFixture(
      uint32_t nProducers,
      uint32_t nConsumers,
      uint32_t itemsPerProducer,
      ThreadModel threadModel = ThreadModel::SEPARATE_THREAD)
      : nProducers_(nProducers),
        nConsumers_(nConsumers),
        itemsPerProducer_(itemsPerProducer),
        threadModel_(threadModel) {}

  virtual ~MPMCQueueBenchmarkFixture() = default;

  void setupThreads();
  void teardownThreads();

  template <typename ProducerFunc, typename ConsumerFunc>
  folly::coro::Task<void> runBenchmarkWithProducerConsumer(
      QueueType& queue,
      ProducerFunc producerFunc,
      ConsumerFunc consumerFunc);

  template <typename EnqueueFunc, typename DequeueFunc>
  folly::coro::Task<void>
  runBenchmark(QueueType& queue, EnqueueFunc enqueueOp, DequeueFunc dequeueOp);

 protected:
  folly::coro::AsyncScope asyncScope_;

  const uint32_t nProducers_;
  const uint32_t nConsumers_;
  const uint32_t itemsPerProducer_;

  std::vector<std::unique_ptr<folly::EventBase>> producerEvbs_;
  std::vector<std::unique_ptr<folly::EventBase>> consumerEvbs_;

  std::vector<std::unique_ptr<std::thread>> producerThreads_;
  std::vector<std::unique_ptr<std::thread>> consumerThreads_;

  /*
   * ThreadModel determines threading behavior:
   * SEPARATE_THREAD: producerThreads_.size() == nProducers_ &&
   *                  consumerThreads_.size() == nConsumers_
   * SINGLE_THREAD: producerThreads_.size() == 1 &&
   *                consumerThreads_.size() == 1
   * ALL_IN_ONE: producerThreads_.size() == 0 &&
   *             consumerThreads_.size() == 1 (shared with producers)
   */
  ThreadModel threadModel_{ThreadModel::SEPARATE_THREAD};

  // for test concurrency
  folly::fibers::BatchSemaphore threadReady_{0};
  folly::Baton<> testStartBaton_;
};

template <typename QueueType, typename ItemType>
void MPMCQueueBenchmarkFixture<QueueType, ItemType>::setupThreads() {
  /*
   * Setup consumer threads based on thread model
   * SEPARATE_THREAD: nConsumers_ threads, SINGLE_THREAD/ALL_IN_ONE: 1 thread
   */
  int numberOfThreads =
      (threadModel_ == ThreadModel::SEPARATE_THREAD) ? nConsumers_ : 1;
  for (auto i = 0; i < numberOfThreads; i++) {
    auto evb = std::make_unique<folly::EventBase>();
    consumerEvbs_.emplace_back(std::move(evb));

    auto consumerThread = std::make_unique<std::thread>(
        [evbPtr = consumerEvbs_.back().get(), this]() {
          this->threadReady_.signal(1);
          this->testStartBaton_.wait();
          evbPtr->loopForever();
        });

    consumerThreads_.emplace_back(std::move(consumerThread));
  }

  /*
   * Setup producer threads based on thread model
   * SEPARATE_THREAD: nProducers_ threads, SINGLE_THREAD: 1 thread, ALL_IN_ONE:
   * 0 (reuses consumer thread)
   */
  switch (threadModel_) {
    case ThreadModel::SEPARATE_THREAD:
      numberOfThreads = nProducers_;
      break;
    case ThreadModel::SINGLE_THREAD:
      numberOfThreads = 1;
      break;
    case ThreadModel::ALL_IN_ONE:
      numberOfThreads = 0;
      break;
  }
  for (auto i = 0; i < numberOfThreads; i++) {
    auto evb = std::make_unique<folly::EventBase>();
    producerEvbs_.emplace_back(std::move(evb));

    auto producerThread = std::make_unique<std::thread>(
        [evbPtr = producerEvbs_.back().get(), this]() {
          this->threadReady_.signal(1);
          this->testStartBaton_.wait();
          evbPtr->loopForever();
        });

    producerThreads_.emplace_back(std::move(producerThread));
  }
}

template <typename QueueType, typename ItemType>
void MPMCQueueBenchmarkFixture<QueueType, ItemType>::teardownThreads() {
  // Teardown producer threads
  for (size_t i = 0; i < producerThreads_.size(); i++) {
    producerEvbs_.at(i)->terminateLoopSoon();
    producerThreads_.at(i)->join();
  }
  producerEvbs_.clear();
  producerThreads_.clear();

  // Teardown consumer threads
  for (size_t i = 0; i < consumerThreads_.size(); i++) {
    consumerEvbs_.at(i)->terminateLoopSoon();
    consumerThreads_.at(i)->join();
  }
  consumerEvbs_.clear();
  consumerThreads_.clear();
}

template <typename QueueType, typename ItemType>
template <typename ProducerFunc, typename ConsumerFunc>
folly::coro::Task<void> MPMCQueueBenchmarkFixture<QueueType, ItemType>::
    runBenchmarkWithProducerConsumer(
        QueueType& queue,
        ProducerFunc producer,
        ConsumerFunc consumer) {
  folly::BenchmarkSuspender suspender;
  setupThreads();

  if (threadModel_ == ThreadModel::ALL_IN_ONE) {
    // All producers and consumers share the same event base
    folly::EventBase* sharedEvb = consumerEvbs_.at(0).get();
    for (auto i = 0; i < nConsumers_; i++) {
      asyncScope_.add(co_withExecutor(sharedEvb, consumer(queue)));
    }
    for (auto i = 0; i < nProducers_; i++) {
      asyncScope_.add(co_withExecutor(sharedEvb, producer(queue)));
    }
  } else {
    // SINGLE_THREAD or SEPARATE_THREAD modes
    for (auto i = 0; i < nConsumers_; i++) {
      int index = threadModel_ == ThreadModel::SINGLE_THREAD ? 0 : i;
      asyncScope_.add(
          co_withExecutor(consumerEvbs_.at(index).get(), consumer(queue)));
    }

    for (auto i = 0; i < nProducers_; i++) {
      int index = threadModel_ == ThreadModel::SINGLE_THREAD ? 0 : i;
      asyncScope_.add(
          co_withExecutor(producerEvbs_.at(index).get(), producer(queue)));
    }
  }

  threadReady_.wait(producerThreads_.size() + consumerThreads_.size());

  suspender.dismiss();

  testStartBaton_.post();

  co_await asyncScope_.joinAsync();

  suspender.rehire();

  teardownThreads();
}

template <typename QueueType, typename ItemType>
template <typename EnqueueFunc, typename DequeueFunc>
folly::coro::Task<void>
MPMCQueueBenchmarkFixture<QueueType, ItemType>::runBenchmark(
    QueueType& queue,
    EnqueueFunc enqueueOp,
    DequeueFunc dequeueOp) {
  folly::BenchmarkSuspender suspender;

  // setup producer and consumer coroutines
  auto producer = [this,
                   enqueueOp](QueueType& queue) -> folly::coro::Task<void> {
    for (auto i = 0; i < itemsPerProducer_; i++) {
      co_await enqueueOp(queue, ItemType(i));
    }
    co_return;
  };

  std::atomic<int> itemsPopped{0};
  int totalItems = nProducers_ * itemsPerProducer_;
  auto consumer = [&itemsPopped, totalItems, dequeueOp](
                      QueueType& queue) -> folly::coro::Task<void> {
    while (++itemsPopped <= totalItems) {
      co_await dequeueOp(queue);
    }
    co_return;
  };

  suspender.dismiss();

  co_await runBenchmarkWithProducerConsumer(queue, producer, consumer);

  suspender.rehire();
}

} // namespace facebook::nettools::bgplib
