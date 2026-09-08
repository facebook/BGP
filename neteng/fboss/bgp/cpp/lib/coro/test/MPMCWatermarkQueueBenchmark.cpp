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

#include "neteng/fboss/bgp/cpp/lib/coro/MPMCWatermarkQueue.h"
#include "neteng/fboss/bgp/cpp/lib/coro/test/MPMCQueueBenchmarkFixture.h"

#include <folly/init/Init.h>

#include <folly/coro/BlockingWait.h>

using namespace facebook::nettools::bgplib;

using QueueItemType = unsigned int;

folly::coro::Task<void> BM_MPMCWatermarkQueue_Async(
    uint32_t iters,
    uint32_t nProducers,
    uint32_t nConsumers,
    uint32_t itemsPerProducer,
    ThreadModel threadModel = ThreadModel::SINGLE_THREAD,
    uint32_t maxQueueCapacity = 10) {
  folly::BenchmarkSuspender suspender;

  /*
   * set the capacity to the min of half of the number of items to be enqueued
   * by one producer or the maxQueueCapacity, whichever smaller
   * this ensures backpressure is triggered
   */
  auto capacity = std::min(itemsPerProducer / 2, maxQueueCapacity);
  auto highWatermark = capacity * 0.8;
  auto lowWatermark = capacity * 0.2;

  auto enqueueOp = [](auto& queue,
                      QueueItemType&& item) -> folly::coro::Task<void> {
    /*
     * We waited for the queue to be unblocked before building the message;
     * guaranteed to succeed writing.
     */
    if (queue.isBlocked()) {
      co_await queue.waitToPush();
    }
    queue.push(std::move(item));
  };

  auto dequeueOp = [](auto& queue) -> folly::coro::Task<void> {
    co_await queue.pop();
  };

  for (int i = 0; i < iters; ++i) {
    MPMCWatermarkQueue<QueueItemType> queue(
        capacity, highWatermark, lowWatermark);

    // all producers are on one thread, and all consumers are on another thread
    MPMCQueueBenchmarkFixture<MPMCWatermarkQueue<QueueItemType>, QueueItemType>
        fixture(nProducers, nConsumers, itemsPerProducer, threadModel);

    suspender.dismiss();

    co_await fixture.runBenchmark(queue, enqueueOp, dequeueOp);

    suspender.rehire();
  }
}

void BM_MPMCWatermarkQueue(
    uint32_t iters,
    uint32_t nProducers,
    uint32_t nConsumers,
    uint32_t itemsPerProducer,
    ThreadModel threadModel = ThreadModel::SINGLE_THREAD) {
  folly::coro::blockingWait(BM_MPMCWatermarkQueue_Async(
      iters, nProducers, nConsumers, itemsPerProducer, threadModel));
}

/*
 * single producer and single consumer run on their corresponding threads
 * the producer sends 10000 items across the queue
 */
BENCHMARK_NAMED_PARAM(
    BM_MPMCWatermarkQueue,
    1_to_1_10000_single_threaded,
    1,
    1,
    10000);
BENCHMARK_NAMED_PARAM(
    BM_MPMCWatermarkQueue,
    1_to_1_20000_single_threaded,
    1,
    1,
    20000);
// single producer and single consumer run on the same thread
BENCHMARK_NAMED_PARAM(
    BM_MPMCWatermarkQueue,
    1_to_1_10000_all_in_one_thread,
    1,
    1,
    10000,
    ThreadModel::ALL_IN_ONE);
BENCHMARK_NAMED_PARAM(
    BM_MPMCWatermarkQueue,
    1_to_1_20000_all_in_one_thread,
    1,
    1,
    20000,
    ThreadModel::ALL_IN_ONE);
// MPMCWatermarkQueue currently only supports single producer

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
