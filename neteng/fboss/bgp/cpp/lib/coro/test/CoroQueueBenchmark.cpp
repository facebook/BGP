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

#include "neteng/fboss/bgp/cpp/lib/coro/CoroQueue.h"
#include "neteng/fboss/bgp/cpp/lib/coro/test/MPMCQueueBenchmarkFixture.h"

#include <folly/init/Init.h>

#include <folly/coro/BlockingWait.h>

using namespace facebook::nettools::bgplib;

using QueueItemType = unsigned int;

folly::coro::Task<void> BM_CoroQueue_MPMC_Async(
    uint32_t iters,
    uint32_t nProducers,
    uint32_t nConsumers,
    uint32_t itemsPerProducer,
    ThreadModel threadModel) {
  folly::BenchmarkSuspender suspender;

  auto enqueueOp = [](auto& queue,
                      QueueItemType&& item) -> folly::coro::Task<void> {
    co_await queue.push(std::move(item));
  };

  auto dequeueOp = [](auto& queue) -> folly::coro::Task<void> {
    co_await queue.wait_and_pop();
  };

  for (int i = 0; i < iters; ++i) {
    CoroQueue<QueueItemType> queue;
    MPMCQueueBenchmarkFixture<CoroQueue<QueueItemType>, QueueItemType> fixture(
        nProducers, nConsumers, itemsPerProducer, threadModel);

    suspender.dismiss();

    co_await fixture.runBenchmark(queue, enqueueOp, dequeueOp);

    suspender.rehire();

    co_await queue.close();
  }
}

void BM_CoroQueue_MPMC(
    uint32_t iters,
    uint32_t nProducers,
    uint32_t nConsumers,
    uint32_t itemsPerProducer,
    ThreadModel threadModel = ThreadModel::SEPARATE_THREAD) {
  folly::coro::blockingWait(BM_CoroQueue_MPMC_Async(
      iters, nProducers, nConsumers, itemsPerProducer, threadModel));
}

BENCHMARK_NAMED_PARAM(BM_CoroQueue_MPMC, 1_to_1_1000_synchronized, 1, 1, 1000);
BENCHMARK_NAMED_PARAM(BM_CoroQueue_MPMC, 1_to_2_1000_synchronized, 1, 2, 1000);
BENCHMARK_NAMED_PARAM(BM_CoroQueue_MPMC, 2_to_2_1000_synchronized, 2, 2, 1000);
BENCHMARK_NAMED_PARAM(BM_CoroQueue_MPMC, 4_to_4_1000_synchronized, 4, 4, 1000);
BENCHMARK_NAMED_PARAM(
    BM_CoroQueue_MPMC,
    16_to_16_1000_synchronized,
    16,
    16,
    1000);

/*
 * single producer and single consumer run on their corresponding threads
 * the producer sends 10000 items across the queue
 * The results should be the same as there is only one producer and one consumer
 */
BENCHMARK_NAMED_PARAM(
    BM_CoroQueue_MPMC,
    1_to_1_10000_single_threaded,
    1,
    1,
    10000,
    ThreadModel::SINGLE_THREAD);
BENCHMARK_NAMED_PARAM(
    BM_CoroQueue_MPMC,
    1_to_1_10000_separate_threaded,
    1,
    1,
    10000,
    ThreadModel::SEPARATE_THREAD);
/*
 * when there are multiple producers, running them in parallel or not would have
 * some impact due to inter-thread locking
 */
BENCHMARK_NAMED_PARAM(
    BM_CoroQueue_MPMC,
    16_to_1_10000_single_threaded,
    16,
    1,
    1000,
    ThreadModel::SINGLE_THREAD);
BENCHMARK_NAMED_PARAM(
    BM_CoroQueue_MPMC,
    16_to_1_10000_separate_threaded,
    16,
    1,
    1000,
    ThreadModel::SEPARATE_THREAD);

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
