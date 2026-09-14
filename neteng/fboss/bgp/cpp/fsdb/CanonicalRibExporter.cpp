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

#include "neteng/fboss/bgp/cpp/fsdb/CanonicalRibExporter.h"

#include <utility>

#include <folly/IPAddress.h>

namespace facebook::bgp {
namespace {

std::string prefixKey(const folly::CIDRNetwork& prefix) {
  return folly::IPAddress::networkToString(prefix);
}

} // namespace

void CanonicalRibExporter::updatePrefix(CanonicalRibPrefixUpdate update) {
  const auto key = prefixKey(update.prefix);
  if (!update.entry.has_value()) {
    retainedPathsByPrefix_.erase(update.prefix);
    pendingDeltas_[key] = std::nullopt;
  } else {
    auto& entryInput = update.entry.value();
    pendingDeltas_[key] = encoder_.buildEntry(
        update.prefix,
        /*ribVersion=*/std::nullopt,
        entryInput.paths,
        entryInput.includePaths,
        entryInput.fields);
    updateRetainedPathsForPrefix(update.prefix, entryInput);
  }
}

void CanonicalRibExporter::accumulatePrefixUpdate(
    CanonicalRibPrefixUpdate update) {
  updatePrefix(std::move(update));
}

CanonicalRibIncrementalPayload CanonicalRibExporter::buildIncrementalBatch(
    TimePoint now) {
  const bool poolsChanged = encoder_.consumeDirtyAndSweep(now);
  CanonicalRibIncrementalPayload payload{
      .entries = std::move(pendingDeltas_),
  };
  pendingDeltas_.clear();
  if (poolsChanged) {
    payload.poolSnapshot = materializePools();
  }
  return payload;
}

void CanonicalRibExporter::reset() {
  pendingDeltas_.clear();
  retainedPathsByPrefix_.clear();
  encoder_ = CanonicalRibEncoder{};
}

CanonicalRibFullSnapshot CanonicalRibExporter::buildFullSnapshot(
    CanonicalRibFullSnapshotInput snapshotInput,
    TimePoint now) {
  reset();
  for (auto& update : snapshotInput.entries) {
    updatePrefix(std::move(update));
  }
  encoder_.consumeDirtyAndSweep(now);

  CanonicalRibFullSnapshot snapshot;
  snapshot.state.attr_dict() = encoder_.dictSnapshot();
  snapshot.state.deduped_paths() = encoder_.pathAttrsSnapshot();
  snapshot.state.peers() = encoder_.peersSnapshot();
  snapshot.state.rib_entries()->reserve(pendingDeltas_.size());
  for (auto it = pendingDeltas_.begin(); it != pendingDeltas_.end();) {
    auto prefix = it->first;
    auto entry = std::move(it->second);
    it = pendingDeltas_.erase(it);
    if (entry.has_value()) {
      snapshot.state.rib_entries()->emplace(
          std::move(prefix), std::move(entry.value()));
    }
  }
  return snapshot;
}

CanonicalRibPoolSnapshot CanonicalRibExporter::materializePools() const {
  return CanonicalRibPoolSnapshot{
      .attrDict = encoder_.dictSnapshot(),
      .dedupedPaths = encoder_.pathAttrsSnapshot(),
      .peers = encoder_.peersSnapshot(),
  };
}

void CanonicalRibExporter::updateRetainedPathsForPrefix(
    const folly::CIDRNetwork& prefix,
    CanonicalRibEntryInput& entryInput) {
  folly::small_vector<std::shared_ptr<const BgpPath>, 1> references;
  references.reserve(entryInput.includePaths ? entryInput.paths.size() : 1);
  for (auto& pathInput : entryInput.paths) {
    if (!entryInput.includePaths && !pathInput.isBestPath) {
      continue;
    }
    references.push_back(std::move(pathInput.path));
  }
  if (references.empty()) {
    retainedPathsByPrefix_.erase(prefix);
  } else {
    retainedPathsByPrefix_.insert_or_assign(prefix, std::move(references));
  }
}

} // namespace facebook::bgp
