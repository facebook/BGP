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

#include <folly/Benchmark.h>
#include <folly/coro/BlockingWait.h>
#include <folly/init/Init.h>

// Forward declaration in the correct namespace
namespace facebook::bgp {
class NeighborWatcherBenchmarkFixture;
} // namespace facebook::bgp

// Friend declaration for benchmark access to private methods
#define NeighborWatcher_TEST_FRIENDS \
  friend class facebook::bgp::NeighborWatcherBenchmarkFixture;

#include "neteng/fboss/bgp/cpp/peer/NeighborWatcher.h"

namespace facebook::bgp {

class NeighborWatcherBenchmarkFixture {
 public:
  NeighborWatcherBenchmarkFixture() {
    fsdbNbrWatcher_ = std::make_shared<FsdbNeighborWatcher>(
        neighborEventQ_,
        ribInQ_,
        &evb_,
        asyncScope_,
        0 /* fsdbPort - not used in benchmark */);
  }

  /*
   * Create an interfaceMap with specified number of interfaces and entries per
   * interface
   */
  std::map<int32_t, fboss::state::InterfaceFields> createInterfaceMap(
      size_t numInterfaces,
      size_t entriesPerInterface,
      bool resolved) {
    std::map<int32_t, fboss::state::InterfaceFields> interfaceMap;
    int ipCounter = 0;

    for (size_t v = 0; v < numInterfaces; ++v) {
      int32_t interfaceId = static_cast<int32_t>(5001 + v);
      std::map<std::string, fboss::state::NeighborEntryFields> arpTable;

      for (size_t e = 0; e < entriesPerInterface; ++e) {
        // Generate unique IP addresses
        int octet2 = (ipCounter / (256 * 256)) % 256;
        int octet3 = (ipCounter / 256) % 256;
        int octet4 = ipCounter % 256;
        std::string ipaddr = fmt::format(
            "10.{}.{}.{}",
            (octet2 + 1) % 256,
            octet3 % 256,
            (octet4 + 1) % 256);
        ipCounter++;

        fboss::state::NeighborEntryFields nef;
        nef.ipaddress() = ipaddr;
        nef.portId()->portId() = resolved ? 100 : 0;
        arpTable[ipaddr] = nef;
      }

      fboss::state::InterfaceFields interfaceFields;
      interfaceFields.interfaceId() = interfaceId;
      interfaceFields.arpTable() = arpTable;
      interfaceFields.ndpTable() = {};
      interfaceMap[interfaceId] = std::move(interfaceFields);
    }

    return interfaceMap;
  }

  void setInterfaceMap(
      std::unique_ptr<std::map<int32_t, fboss::state::InterfaceFields>>
          interfaceMap) {
    fsdbNbrWatcher_->interfaceMap_ = std::move(interfaceMap);
  }

  void processInterfaceMapChanges(
      const std::map<int32_t, fboss::state::InterfaceFields>& newInterfaceMap) {
    folly::coro::blockingWait(
        fsdbNbrWatcher_->processInterfaceMapChanges(newInterfaceMap));
  }

  void drainQueue() {
    while (!ribInQ_.empty()) {
      folly::coro::blockingWait(ribInQ_.pop());
    }
    while (!neighborEventQ_.empty()) {
      folly::coro::blockingWait(neighborEventQ_.pop());
    }
  }

 private:
  folly::EventBase evb_;
  folly::coro::CancellableAsyncScope asyncScope_;
  MonitoredMPMCQueue<NeighborWatcherMessage> neighborEventQ_;
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_;
  std::shared_ptr<FsdbNeighborWatcher> fsdbNbrWatcher_;
};

// Benchmark processInterfaceMapChanges with no changes (same interfaceMap)
void BM_ProcessInterfaceMapChanges_NoChanges(
    uint32_t iters,
    size_t numEntries) {
  NeighborWatcherBenchmarkFixture fixture;

  // Create an interfaceMap with specified entries
  auto interfaceMap = fixture.createInterfaceMap(1, numEntries, true);
  fixture.setInterfaceMap(
      std::make_unique<std::map<int32_t, fboss::state::InterfaceFields>>(
          interfaceMap));
  fixture.drainQueue();

  while (iters--) {
    fixture.processInterfaceMapChanges(interfaceMap);
    BENCHMARK_SUSPEND {
      fixture.drainQueue();
    }
  }
}

// Benchmark processInterfaceMapChanges with all entries added (first update)
void BM_ProcessInterfaceMapChanges_AllAdded(uint32_t iters, size_t numEntries) {
  while (iters--) {
    NeighborWatcherBenchmarkFixture fixture;

    // interfaceMap_ is null initially, so all entries will be added
    auto interfaceMap = fixture.createInterfaceMap(1, numEntries, true);

    fixture.processInterfaceMapChanges(interfaceMap);

    BENCHMARK_SUSPEND {
      fixture.drainQueue();
    }
  }
}

/*
 * Benchmark processInterfaceMapChanges with all entries deleted (interfaceId
 * removed)
 */
void BM_ProcessInterfaceMapChanges_AllDeleted(
    uint32_t iters,
    size_t numEntries) {
  NeighborWatcherBenchmarkFixture fixture;

  // Set up initial interfaceMap
  auto oldInterfaceMap = fixture.createInterfaceMap(1, numEntries, true);
  fixture.setInterfaceMap(
      std::make_unique<std::map<int32_t, fboss::state::InterfaceFields>>(
          oldInterfaceMap));
  fixture.drainQueue();

  while (iters--) {
    // Process with empty interfaceMap (all entries deleted)
    std::map<int32_t, fboss::state::InterfaceFields> emptyInterfaceMap;
    fixture.processInterfaceMapChanges(emptyInterfaceMap);

    BENCHMARK_SUSPEND {
      // Reset state for next iteration
      fixture.setInterfaceMap(
          std::make_unique<std::map<int32_t, fboss::state::InterfaceFields>>(
              oldInterfaceMap));
      fixture.drainQueue();
    }
  }
}

// Benchmark processInterfaceMapChanges with entries becoming unresolved
void BM_ProcessInterfaceMapChanges_BecomeUnresolved(
    uint32_t iters,
    size_t numEntries) {
  NeighborWatcherBenchmarkFixture fixture;

  // Set up initial interfaceMap with resolved entries
  auto resolvedInterfaceMap = fixture.createInterfaceMap(1, numEntries, true);
  auto unresolvedInterfaceMap =
      fixture.createInterfaceMap(1, numEntries, false);

  fixture.setInterfaceMap(
      std::make_unique<std::map<int32_t, fboss::state::InterfaceFields>>(
          resolvedInterfaceMap));
  fixture.drainQueue();

  while (iters--) {
    // Process with unresolved entries
    fixture.processInterfaceMapChanges(unresolvedInterfaceMap);

    BENCHMARK_SUSPEND {
      // Reset state for next iteration
      fixture.setInterfaceMap(
          std::make_unique<std::map<int32_t, fboss::state::InterfaceFields>>(
              resolvedInterfaceMap));
      fixture.drainQueue();
    }
  }
}

// Benchmark processInterfaceMapChanges with multiple interfaces
void BM_ProcessInterfaceMapChanges_MultipleInterfaces(
    uint32_t iters,
    size_t numInterfaces,
    size_t entriesPerInterface) {
  NeighborWatcherBenchmarkFixture fixture;

  auto interfaceMap =
      fixture.createInterfaceMap(numInterfaces, entriesPerInterface, true);
  fixture.setInterfaceMap(
      std::make_unique<std::map<int32_t, fboss::state::InterfaceFields>>(
          interfaceMap));
  fixture.drainQueue();

  while (iters--) {
    fixture.processInterfaceMapChanges(interfaceMap);
    BENCHMARK_SUSPEND {
      fixture.drainQueue();
    }
  }
}

} // namespace facebook::bgp

using namespace facebook::bgp;

// No changes benchmarks
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_NoChanges,
    Entries_100,
    100);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_NoChanges,
    Entries_500,
    500);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_NoChanges,
    Entries_1000,
    1000);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_NoChanges,
    Entries_5000,
    5000);

BENCHMARK_DRAW_LINE();

// All entries added benchmarks
BENCHMARK_NAMED_PARAM(BM_ProcessInterfaceMapChanges_AllAdded, Entries_100, 100);
BENCHMARK_NAMED_PARAM(BM_ProcessInterfaceMapChanges_AllAdded, Entries_500, 500);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_AllAdded,
    Entries_1000,
    1000);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_AllAdded,
    Entries_5000,
    5000);

BENCHMARK_DRAW_LINE();

// All entries deleted benchmarks
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_AllDeleted,
    Entries_100,
    100);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_AllDeleted,
    Entries_500,
    500);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_AllDeleted,
    Entries_1000,
    1000);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_AllDeleted,
    Entries_5000,
    5000);

BENCHMARK_DRAW_LINE();

// Entries become unresolved benchmarks
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_BecomeUnresolved,
    Entries_100,
    100);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_BecomeUnresolved,
    Entries_500,
    500);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_BecomeUnresolved,
    Entries_1000,
    1000);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_BecomeUnresolved,
    Entries_5000,
    5000);

BENCHMARK_DRAW_LINE();

// Multiple interfaces benchmarks
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_MultipleInterfaces,
    Interfaces_10_Entries_100,
    10,
    100);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_MultipleInterfaces,
    Interfaces_50_Entries_100,
    50,
    100);
BENCHMARK_NAMED_PARAM(
    BM_ProcessInterfaceMapChanges_MultipleInterfaces,
    Interfaces_100_Entries_100,
    100,
    100);

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
