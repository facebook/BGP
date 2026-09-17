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

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/fsdb/CanonicalRibExporter.h"
#include "neteng/fboss/bgp/cpp/fsdb/CanonicalRibUpdateQueue.h"
#include "neteng/fboss/bgp/cpp/rib/RibBase.h"
#include "neteng/fboss/bgp/cpp/rib/RibEntry.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {
namespace {

/*
 * This is a CPU-stage microbenchmark, not an end-to-end FSDB convergence
 * measurement. It times synthetic RIB input construction/queueing and
 * FsdbSyncer-thread encoding/materialization for either a full snapshot or a
 * stream of distinct-prefix incremental updates split at the production batch
 * deadline. It intentionally excludes prepareFibProgramming, FsdbSyncer
 * scheduling, patch construction, and transport enqueue latency.
 */

DEFINE_uint32(num_prefixes, 960 * 252, "Number of RIB prefixes to rebuild");
DEFINE_uint32(paths_per_prefix, 1, "Selected ECMP paths per prefix");
DEFINE_uint32(
    prefixes_per_path_group,
    252,
    "Prefixes that share the same set of path attributes");
DEFINE_bool(
    reuse_nexthops_across_groups,
    false,
    "Reuse the same per-switch nexthops in every path-attribute group");
DEFINE_bool(
    incremental_batch,
    false,
    "Measure distinct-prefix incremental batching instead of a full snapshot");
DEFINE_uint32(
    incremental_batch_duration_ms,
    200,
    "Flush an incremental batch after this much drain/encoding time");

std::vector<std::shared_ptr<const BgpPath>> makePaths(
    uint32_t group,
    uint32_t count) {
  std::vector<std::shared_ptr<const BgpPath>> paths;
  paths.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t nextHopOrdinal = FLAGS_reuse_nexthops_across_groups
        ? i
        : static_cast<uint64_t>(group) * count + i;
    XCHECK_LT(nextHopOrdinal, 0x00ffffff);
    const auto nextHop = folly::IPAddress(
        folly::IPAddressV4::fromLongHBO(
            (uint32_t{192} << 24) | static_cast<uint32_t>(nextHopOrdinal + 1)));
    auto fields = buildBgpPathFields(
        /*as_count=*/0,
        /*community_count=*/1,
        /*ext_community_count=*/0,
        /*cluster_list_count=*/0,
        /*confed_as_count=*/0,
        nextHop);
    nettools::bgplib::BgpAttrAsPathSegmentC segment;
    segment.asSequence = {65000 + group, 66000 + i};
    auto path = std::make_shared<BgpPath>(*fields);
    path->setAsPath(nettools::bgplib::BgpAttrAsPathC{{std::move(segment)}});
    path->publish();
    paths.push_back(std::move(path));
  }
  return paths;
}

std::unique_ptr<RibEntry> makeEntry(
    uint32_t ordinal,
    const std::vector<std::shared_ptr<const BgpPath>>& paths) {
  XCHECK_LT(ordinal, 0x01000000);
  const auto prefixAddress = folly::IPAddressV4::fromLongHBO(
      (uint32_t{10} << 24) | (ordinal & 0x00ffffff));
  auto entry = std::make_unique<RibEntry>(
      folly::CIDRNetwork{folly::IPAddress(prefixAddress), 32});
  uint32_t pathId = 0;
  for (const auto& path : paths) {
    const auto& nextHop = path->getNexthop();
    TinyPeerInfo peer(
        nextHop,
        kPeerAsn1,
        /*routerId=*/nextHop.asV4().toLongHBO(),
        BgpSessionType::EBGP,
        /*isRrClient=*/false);
    entry->updatePath(
        peer, path, /*installToFib=*/true, /*receivedPathId=*/pathId++);
  }
  RibBase::selectBestPath(
      *entry,
      multipathSelector,
      bestpathSelector,
      /*computeUcmp=*/false,
      /*ucmpWidth=*/0);
  return entry;
}

std::vector<CanonicalPathInput> buildPathInputs(const RibEntry& entry) {
  std::vector<CanonicalPathInput> inputs;
  const auto* best = entry.getBestPathRaw();
  const auto& multipaths = entry.getMultipaths();
  const auto allPaths = entry.getAllPaths();
  inputs.reserve(allPaths.size());
  for (const auto& routeInfo : allPaths) {
    CanonicalPathInput input;
    input.path = routeInfo->attrs;
    input.peerAddr = routeInfo->peer.addr;
    input.peerRouterId = routeInfo->peer.routerId;
    input.peerDescription = routeInfo->peer.description;
    input.pathId = routeInfo->receivedPathId;
    input.isBestPath = routeInfo.get() == best;
    input.group = routeInfo->pathIdToSend.has_value() &&
            multipaths.contains(routeInfo->pathIdToSend.value())
        ? kBestPathGroup
        : kDefaultPathGroup;
    inputs.push_back(std::move(input));
  }
  return inputs;
}

CanonicalRibPrefixUpdate makeUpdate(const RibEntry& entry) {
  return CanonicalRibPrefixUpdate{
      .prefix = entry.getPrefix(),
      .entry =
          CanonicalRibEntryInput{
              .paths = buildPathInputs(entry),
              .fields = {},
              .includePaths = true,
          },
  };
}

} // namespace
} // namespace facebook::bgp

int main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  using namespace facebook::bgp;

  XCHECK_GT(FLAGS_num_prefixes, 0);
  XCHECK_GT(FLAGS_paths_per_prefix, 0);
  XCHECK_GT(FLAGS_prefixes_per_path_group, 0);
  XCHECK_LE(FLAGS_paths_per_prefix, 254);
  const auto numGroups =
      (FLAGS_num_prefixes + FLAGS_prefixes_per_path_group - 1) /
      FLAGS_prefixes_per_path_group;
  std::vector<std::vector<std::shared_ptr<const BgpPath>>> pathGroups;
  pathGroups.reserve(numGroups);
  for (uint32_t group = 0; group < numGroups; ++group) {
    pathGroups.push_back(makePaths(group, FLAGS_paths_per_prefix));
  }
  std::vector<std::unique_ptr<RibEntry>> entries;
  entries.reserve(FLAGS_num_prefixes);
  for (uint32_t i = 0; i < FLAGS_num_prefixes; ++i) {
    entries.push_back(
        makeEntry(i, pathGroups.at(i / FLAGS_prefixes_per_path_group)));
  }

  CanonicalRibUpdateQueue queue;
  const auto producerStart = std::chrono::steady_clock::now();
  if (FLAGS_incremental_batch) {
    for (const auto& entry : entries) {
      if (!entry) {
        throw std::logic_error("benchmark entry construction returned null");
      }
      queue.pushPrefixUpdate(makeUpdate(*entry));
    }
  } else {
    CanonicalRibFullSnapshotInput fullSnapshot;
    fullSnapshot.entries.reserve(entries.size());
    for (const auto& entry : entries) {
      if (!entry) {
        throw std::logic_error("benchmark entry construction returned null");
      }
      fullSnapshot.entries.push_back(makeUpdate(*entry));
    }
    queue.pushFullSnapshot(std::move(fullSnapshot));
  }
  const auto producerDuration =
      std::chrono::steady_clock::now() - producerStart;
  XCHECK_EQ(
      queue.size(), FLAGS_incremental_batch ? FLAGS_num_prefixes : size_t{1});

  CanonicalRibExporter exporter;
  const auto syncerStart = std::chrono::steady_clock::now();
  size_t payloadCount{0};
  if (FLAGS_incremental_batch) {
    size_t drained{0};
    size_t materializedEntries{0};
    size_t poolSnapshotCount{0};
    std::optional<std::chrono::steady_clock::time_point> batchStart;
    while (auto queued = queue.tryPop()) {
      XCHECK(std::holds_alternative<CanonicalRibPrefixUpdate>(*queued));
      if (!batchStart.has_value()) {
        batchStart = std::chrono::steady_clock::now();
      }
      exporter.accumulatePrefixUpdate(
          std::get<CanonicalRibPrefixUpdate>(std::move(*queued)));
      ++drained;
      if (queue.empty() ||
          std::chrono::steady_clock::now() - *batchStart >=
              std::chrono::milliseconds{FLAGS_incremental_batch_duration_ms}) {
        auto payload = exporter.buildIncrementalBatch();
        materializedEntries += payload.entries.size();
        poolSnapshotCount += payload.poolSnapshot.has_value();
        ++payloadCount;
        batchStart.reset();
      }
    }
    XCHECK_EQ(drained, FLAGS_num_prefixes);
    XCHECK_EQ(materializedEntries, FLAGS_num_prefixes);
    XCHECK_GT(poolSnapshotCount, 0);
    XCHECK(!batchStart.has_value());
  } else {
    auto queued = queue.tryPop();
    XCHECK(queued.has_value());
    auto snapshotInput =
        std::get<CanonicalRibFullSnapshotInput>(std::move(*queued));
    auto snapshot = exporter.buildFullSnapshot(std::move(snapshotInput));
    XCHECK_EQ(snapshot.state.rib_entries()->size(), FLAGS_num_prefixes);
    ++payloadCount;
  }
  const auto syncerDuration = std::chrono::steady_clock::now() - syncerStart;
  const auto producerMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(producerDuration);
  const auto syncerMs =
      std::chrono::duration_cast<std::chrono::milliseconds>(syncerDuration);
  const auto producerMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(producerDuration);
  const auto syncerMicros =
      std::chrono::duration_cast<std::chrono::microseconds>(syncerDuration);
  XCHECK_GT(producerMicros.count(), 0);
  XCHECK_GT(syncerMicros.count(), 0);
  const auto* mode = FLAGS_incremental_batch ? "canonical incremental batch"
                                             : "canonical full snapshot";

  std::printf(
      "%s producer input construction/enqueue: %u prefixes x %u "
      "paths = %llu paths, %u groups, %llu distinct BgpPath objects in "
      "%lld ms (%.0f paths/s)\n",
      mode,
      FLAGS_num_prefixes,
      FLAGS_paths_per_prefix,
      static_cast<unsigned long long>(FLAGS_num_prefixes) *
          FLAGS_paths_per_prefix,
      numGroups,
      static_cast<unsigned long long>(numGroups) * FLAGS_paths_per_prefix,
      static_cast<long long>(producerMs.count()),
      1000000.0 * FLAGS_num_prefixes * FLAGS_paths_per_prefix /
          producerMicros.count());
  std::printf(
      "%s FsdbSyncer-thread drain/encode/materialize: %u prefixes "
      "x %u paths in %lld ms (%.0f paths/s), %zu publication payloads\n",
      mode,
      FLAGS_num_prefixes,
      FLAGS_paths_per_prefix,
      static_cast<long long>(syncerMs.count()),
      1000000.0 * FLAGS_num_prefixes * FLAGS_paths_per_prefix /
          syncerMicros.count(),
      payloadCount);
  return 0;
}
