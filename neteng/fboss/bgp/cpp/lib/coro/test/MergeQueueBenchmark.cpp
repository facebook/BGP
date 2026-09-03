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

#include "neteng/fboss/bgp/cpp/lib/coro/MPMCQueue.h"
#include "neteng/fboss/bgp/cpp/lib/coro/MergeQueue.h"

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include <cstdint>
#include <string>

using namespace facebook::bgp::coro;

namespace {

struct StickyValue {
  uint64_t latest{0};
  bool requestCleanup{false};
};

struct NonTrivialValue {
  uint64_t latest{0};
  std::string payload;
};

} // namespace

/*
 * All pushes to one slot: every push after the first merges in place, so the
 * queue never grows beyond a single node. This is the empty->full thrash
 * workload the merge queue is meant to collapse.
 */
BENCHMARK(MergeQueue_SameSlotMerge, n) {
  MergeQueue<int> q;
  for (unsigned i = 0; i < n; ++i) {
    q.pushMerge(static_cast<int>(i), /*key=*/0);
  }
  folly::doNotOptimizeAway(q.size());
}

/*
 * Baseline: plain FIFO push of the same items with no coalescing. The queue
 * grows to n nodes, and a consumer would have to process all n.
 */
BENCHMARK_RELATIVE(MPMCQueue_PlainPush, n) {
  MPMCQueue<int> q;
  for (unsigned i = 0; i < n; ++i) {
    q.push(static_cast<int>(i));
  }
  folly::doNotOptimizeAway(q.size());
}

BENCHMARK_DRAW_LINE();

/* Generic key lookup and ownership on the same-key merge path. */
BENCHMARK(MergeQueue_StringKeySameKeyMerge, n) {
  MergeQueue<StickyValue, std::string> q;
  const std::string key{"2001:db8::/64"};
  for (unsigned i = 0; i < n; ++i) {
    q.pushMerge(StickyValue{.latest = i}, key);
  }
  folly::doNotOptimizeAway(q.tryPop());
}

/* Cost of preserving sticky metadata when replacing a pending value. */
BENCHMARK_RELATIVE(MergeQueue_StringKeyCustomMerge, n) {
  MergeQueue<StickyValue, std::string> q;
  const std::string key{"2001:db8::/64"};
  for (unsigned i = 0; i < n; ++i) {
    q.pushMergeWith(
        StickyValue{.latest = i, .requestCleanup = i == 0},
        key,
        [](StickyValue& incoming, const StickyValue& pending) noexcept {
          incoming.requestCleanup |= pending.requestCleanup;
        });
  }
  folly::doNotOptimizeAway(q.tryPop());
}

/* Production-like splice path for a nontrivially destructible payload. */
BENCHMARK(MergeQueue_NonTrivialSameKeyMerge, n) {
  MergeQueue<NonTrivialValue, std::string> q;
  const std::string key{"2001:db8::/64"};
  for (unsigned i = 0; i < n; ++i) {
    q.pushMerge(NonTrivialValue{.latest = i, .payload = "canonical-path"}, key);
  }
  folly::doNotOptimizeAway(q.tryPop());
}

BENCHMARK_DRAW_LINE();

/*
 * Distinct slots: every push appends (no merge) -- measures the append path
 * overhead relative to the plain queue.
 */
BENCHMARK(MergeQueue_DistinctSlotsAppend, n) {
  MergeQueue<int> q;
  for (unsigned i = 0; i < n; ++i) {
    q.pushMerge(static_cast<int>(i), /*key=*/static_cast<int>(i));
  }
  folly::doNotOptimizeAway(q.size());
}

BENCHMARK_RELATIVE(MPMCQueue_DistinctAppend, n) {
  MPMCQueue<int> q;
  for (unsigned i = 0; i < n; ++i) {
    q.push(static_cast<int>(i));
  }
  folly::doNotOptimizeAway(q.size());
}

BENCHMARK_DRAW_LINE();

/* Nonblocking consumer cost with distinct pending keys. */
BENCHMARK(MergeQueue_TryPopDrain, n) {
  folly::BenchmarkSuspender suspender;
  MergeQueue<uint64_t, uint64_t> q;
  for (uint64_t i = 0; i < n; ++i) {
    q.pushMerge(i, i);
  }

  suspender.dismiss();
  for (unsigned i = 0; i < n; ++i) {
    folly::doNotOptimizeAway(q.tryPop());
  }
  suspender.rehire();
  folly::doNotOptimizeAway(q.empty());
}

BENCHMARK(MergeQueue_TryPopEmpty, n) {
  MergeQueue<uint64_t, uint64_t> q;
  for (unsigned i = 0; i < n; ++i) {
    folly::doNotOptimizeAway(q.tryPop());
  }
}

void BM_MergeQueue_PurgeAll(unsigned iterations, unsigned pendingItems) {
  folly::BenchmarkSuspender suspender;
  const std::string payload(128, 'x');
  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    MergeQueue<std::string> q;
    for (unsigned item = 0; item < pendingItems; ++item) {
      q.pushMerge(payload, static_cast<int>(item));
    }

    suspender.dismiss();
    q.pushPurgeAll(payload);
    suspender.rehire();
    folly::doNotOptimizeAway(q.size());
  }
}

BENCHMARK_NAMED_PARAM(BM_MergeQueue_PurgeAll, 0_pending, 0);
BENCHMARK_NAMED_PARAM(BM_MergeQueue_PurgeAll, 1_pending, 1);
BENCHMARK_NAMED_PARAM(BM_MergeQueue_PurgeAll, 64_pending, 64);
BENCHMARK_NAMED_PARAM(BM_MergeQueue_PurgeAll, 4096_pending, 4096);

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
