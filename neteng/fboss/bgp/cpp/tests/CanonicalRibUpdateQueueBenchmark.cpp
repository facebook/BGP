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

#include "neteng/fboss/bgp/cpp/fsdb/CanonicalRibUpdateQueue.h"

#include <folly/Benchmark.h>
#include <folly/IPAddress.h>
#include <folly/init/Init.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace facebook::bgp {
namespace {

constexpr uint32_t kBaseIpv4 = 0x0a000000U;

CanonicalRibPrefixUpdate makeWithdrawal(const folly::CIDRNetwork& prefix) {
  return {
      .prefix = prefix,
      .entry = std::nullopt,
  };
}

CanonicalRibPrefixUpdate makeEntryUpdate(
    const folly::CIDRNetwork& prefix,
    unsigned pathCount) {
  CanonicalRibEntryInput entryInput;
  entryInput.paths.reserve(pathCount);
  for (unsigned i = 0; i < pathCount; ++i) {
    entryInput.paths.push_back(
        CanonicalPathInput{
            .group = "benchmark",
            .peerAddr = folly::IPAddress("192.0.2.1"),
            .peerDescription = std::string(64, 'p'),
        });
  }
  return {
      .prefix = prefix,
      .entry = std::move(entryInput),
  };
}

std::vector<folly::CIDRNetwork> makePrefixes(unsigned count) {
  std::vector<folly::CIDRNetwork> prefixes;
  prefixes.reserve(count);
  for (unsigned i = 0; i < count; ++i) {
    prefixes.emplace_back(
        folly::IPAddress::fromLongHBO(kBaseIpv4 + i), uint8_t{32});
  }
  return prefixes;
}

BENCHMARK(CanonicalRibUpdateQueue_SamePrefixCoalesce, n) {
  folly::BenchmarkSuspender suspender;
  CanonicalRibUpdateQueue queue;
  const auto prefix = folly::IPAddress::createNetwork("10.0.0.0/32");

  suspender.dismiss();
  for (unsigned i = 0; i < n; ++i) {
    folly::doNotOptimizeAway(queue.pushPrefixUpdate(makeWithdrawal(prefix)));
  }
  suspender.rehire();

  const auto update = queue.tryPop();
  if (update) {
    folly::doNotOptimizeAway(
        std::get<CanonicalRibPrefixUpdate>(*update).prefix);
  }
}

BENCHMARK(CanonicalRibUpdateQueue_DistinctPrefixAppend, n) {
  folly::BenchmarkSuspender suspender;
  const auto prefixes = makePrefixes(n);
  CanonicalRibUpdateQueue queue;

  suspender.dismiss();
  for (unsigned i = 0; i < n; ++i) {
    folly::doNotOptimizeAway(
        queue.pushPrefixUpdate(makeWithdrawal(prefixes[i])));
  }
  suspender.rehire();
  folly::doNotOptimizeAway(queue.size());
}

void BM_CanonicalRibUpdateQueue_PopulatedCoalesce(
    unsigned iterations,
    unsigned pathCount) {
  folly::BenchmarkSuspender suspender;
  const auto prefix = folly::IPAddress::createNetwork("10.0.0.0/32");

  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    CanonicalRibUpdateQueue queue;
    queue.pushPrefixUpdate(makeEntryUpdate(prefix, pathCount));
    auto replacement = makeEntryUpdate(prefix, pathCount);

    suspender.dismiss();
    folly::doNotOptimizeAway(queue.pushPrefixUpdate(std::move(replacement)));
    suspender.rehire();
    folly::doNotOptimizeAway(queue.size());
  }
}

BENCHMARK_NAMED_PARAM(BM_CanonicalRibUpdateQueue_PopulatedCoalesce, OnePath, 1);
BENCHMARK_NAMED_PARAM(
    BM_CanonicalRibUpdateQueue_PopulatedCoalesce,
    EightPaths,
    8);
BENCHMARK_NAMED_PARAM(
    BM_CanonicalRibUpdateQueue_PopulatedCoalesce,
    OneHundredTwentyPaths,
    120);

BENCHMARK_DRAW_LINE();

BENCHMARK(CanonicalRibUpdateQueue_TryPopDrain, n) {
  folly::BenchmarkSuspender suspender;
  const auto prefixes = makePrefixes(n);
  CanonicalRibUpdateQueue queue;
  for (unsigned i = 0; i < n; ++i) {
    queue.pushPrefixUpdate(makeWithdrawal(prefixes[i]));
  }

  suspender.dismiss();
  for (unsigned i = 0; i < n; ++i) {
    folly::doNotOptimizeAway(queue.tryPop());
  }
  suspender.rehire();
  folly::doNotOptimizeAway(queue.size());
}

BENCHMARK(CanonicalRibUpdateQueue_TryPopEmpty, n) {
  folly::BenchmarkSuspender suspender;
  CanonicalRibUpdateQueue queue;

  suspender.dismiss();
  for (unsigned i = 0; i < n; ++i) {
    folly::doNotOptimizeAway(queue.tryPop());
  }
}

BENCHMARK_DRAW_LINE();

void BM_CanonicalRibUpdateQueue_FullSnapshotPurge(
    unsigned iterations,
    unsigned pendingPrefixes) {
  folly::BenchmarkSuspender suspender;
  const auto prefixes = makePrefixes(pendingPrefixes);

  for (unsigned iteration = 0; iteration < iterations; ++iteration) {
    CanonicalRibUpdateQueue queue;
    for (const auto& prefix : prefixes) {
      queue.pushPrefixUpdate(makeWithdrawal(prefix));
    }

    suspender.dismiss();
    queue.pushFullSnapshot(CanonicalRibFullSnapshotInput{});
    suspender.rehire();
    folly::doNotOptimizeAway(queue.size());
  }
}

BENCHMARK_NAMED_PARAM(
    BM_CanonicalRibUpdateQueue_FullSnapshotPurge,
    NoPending,
    0);
BENCHMARK_NAMED_PARAM(
    BM_CanonicalRibUpdateQueue_FullSnapshotPurge,
    Pending64,
    64);
BENCHMARK_NAMED_PARAM(
    BM_CanonicalRibUpdateQueue_FullSnapshotPurge,
    Pending4096,
    4096);

} // namespace
} // namespace facebook::bgp

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
