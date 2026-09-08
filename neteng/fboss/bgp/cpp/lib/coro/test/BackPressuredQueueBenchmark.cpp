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

#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"
#include "neteng/fboss/bgp/cpp/lib/coro/test/MPMCQueueBenchmarkFixture.h"

#include <folly/init/Init.h>

#include <folly/coro/BlockingWait.h>

using namespace facebook::nettools::bgplib;

using QueueItemType = unsigned int;

folly::coro::Task<void> BM_BackPressuredQueue_Async(
    uint32_t iters,
    uint32_t nProducers,
    uint32_t nConsumers,
    uint32_t itemsPerProducer,
    ThreadModel threadModel = ThreadModel::SINGLE_THREAD,
    uint32_t maxQueueCapacity = 10) {
  folly::BenchmarkSuspender suspender;

  /*
   * Set the capacity to the min of half of the number of items to be enqueued
   * by one producer or the maxQueueCapacity, whichever is smaller.
   * This ensures backpressure is triggered during benchmarking.
   */
  auto capacity = std::min(itemsPerProducer / 2, maxQueueCapacity);

  auto enqueueOp = [](auto& queue,
                      QueueItemType&& item) -> folly::coro::Task<void> {
    co_await queue.push(std::move(item));
  };

  auto dequeueOp = [](auto& queue) -> folly::coro::Task<void> {
    co_await queue.pop();
  };

  for (int i = 0; i < iters; ++i) {
    BackPressuredQueue<QueueItemType> queue(capacity);

    // all producers are on one thread, and all consumers are on another thread
    MPMCQueueBenchmarkFixture<BackPressuredQueue<QueueItemType>, QueueItemType>
        fixture(nProducers, nConsumers, itemsPerProducer, threadModel);

    suspender.dismiss();

    co_await fixture.runBenchmark(queue, enqueueOp, dequeueOp);

    suspender.rehire();
  }
}

void BM_BackPressuredQueue(
    uint32_t iters,
    uint32_t nProducers,
    uint32_t nConsumers,
    uint32_t itemsPerProducer,
    ThreadModel threadModel = ThreadModel::SINGLE_THREAD) {
  folly::coro::blockingWait(BM_BackPressuredQueue_Async(
      iters, nProducers, nConsumers, itemsPerProducer, threadModel));
}

/*
 * Benchmark: Single producer and single consumer on separate threads
 * Tests basic back pressure behavior with 10000/20000 items
 */
BENCHMARK_NAMED_PARAM(
    BM_BackPressuredQueue,
    1_to_1_10000_single_threaded,
    1,
    1,
    10000);
BENCHMARK_NAMED_PARAM(
    BM_BackPressuredQueue,
    1_to_1_20000_single_threaded,
    1,
    1,
    20000);

/*
 * Benchmark: Single producer and single consumer on the same thread
 * Tests coroutine switching overhead within a single thread
 */
BENCHMARK_NAMED_PARAM(
    BM_BackPressuredQueue,
    1_to_1_10000_all_in_one_thread,
    1,
    1,
    10000,
    ThreadModel::ALL_IN_ONE);
BENCHMARK_NAMED_PARAM(
    BM_BackPressuredQueue,
    1_to_1_20000_all_in_one_thread,
    1,
    1,
    20000,
    ThreadModel::ALL_IN_ONE);

/*
 * Benchmark: Multiple producers (16) and single consumer on separate threads
 * Tests back pressure coordination with multiple concurrent producers
 */
BENCHMARK_NAMED_PARAM(
    BM_BackPressuredQueue,
    16_to_1_10000_single_threaded,
    16,
    1,
    10000);
BENCHMARK_NAMED_PARAM(
    BM_BackPressuredQueue,
    16_to_1_20000_single_threaded,
    16,
    1,
    20000);

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
