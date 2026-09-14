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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <folly/container/F14Map.h>
#include <folly/small_vector.h>

#include "neteng/fboss/bgp/cpp/fsdb/CanonicalRibUpdateQueue.h"
#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalRibEncoder.h"

namespace facebook::bgp {

using CanonicalRibEntryDeltas = folly::
    F14FastMap<std::string, std::optional<bgp_thrift::TRibEntryCanonical>>;

struct CanonicalRibPoolSnapshot {
  bgp_thrift::TBgpAttrDict attrDict;
  folly::F14FastMap<int64_t, bgp_thrift::TBgpDedupedPath> dedupedPaths;
  folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer> peers;
};

struct CanonicalRibIncrementalPayload {
  CanonicalRibEntryDeltas entries;
  std::optional<CanonicalRibPoolSnapshot> poolSnapshot;
};

struct CanonicalRibFullSnapshot {
  bgp_thrift::TCanonicalRibState state;
};

/*
 * Encoder confined to the FsdbSyncer thread for the canonical Loc-RIB
 * representation. The RIB hands immutable, owning inputs to a queue;
 * FsdbSyncer applies them here and independently decides whether and when to
 * publish the resulting payloads.
 *
 * Continuous entries intentionally omit `rib_version`. That version belongs
 * to Adj-RIB-out processing and is not an ordering signal for Loc-RIB changes.
 * Each queued entry input already carries the producer's best-only versus
 * multipath choice; the exporter encodes that immutable input verbatim.
 */
class CanonicalRibExporter {
 public:
  using TimePoint = CanonicalRibEncoder::TimePoint;

  CanonicalRibExporter() = default;

  /**
   * Encode one prefix update into the current incremental batch. A missing
   * entry is encoded as a withdrawal. Repeated prefixes retain only their
   * latest encoded state in the batch.
   *
   * @param update Complete latest state for one prefix.
   */
  void accumulatePrefixUpdate(CanonicalRibPrefixUpdate update);

  /**
   * Finish the current incremental batch. The returned payload contains all
   * accumulated prefix changes and, when pool membership changed, one complete
   * replacement snapshot of all canonical pools.
   *
   * @param now Current monotonic time used for opportunistic pool reclamation.
   */
  CanonicalRibIncrementalPayload buildIncrementalBatch(
      TimePoint now = CanonicalRibEncoder::Clock::now());

  /*
   * Replace all encoded state and build one authoritative canonical RIB
   * snapshot. Withdrawals in the input are omitted from the replacement state.
   *
   * @param snapshot Complete owning input captured by the RIB thread.
   * @param now Current monotonic time used for opportunistic pool reclamation.
   */
  CanonicalRibFullSnapshot buildFullSnapshot(
      CanonicalRibFullSnapshotInput snapshot,
      TimePoint now = CanonicalRibEncoder::Clock::now());

  /** Discard all encoded entries, retained paths, and canonical pool IDs. */
  void reset();

  /** Return current canonical pool cardinality and high-water marks. */
  CanonicalRibEncoder::PoolStatsSnapshot poolStats() const {
    return encoder_.poolStats();
  }

 private:
  /* Encode one prefix and update its retained path references. */
  void updatePrefix(CanonicalRibPrefixUpdate update);

  CanonicalRibPoolSnapshot materializePools() const;

  /**
   * Make retained path references match one prefix's encoded entry.
   *
   * @param prefix Prefix whose previous references are replaced.
   * @param entryInput Entry just encoded; referenced path ownership is moved
   *     into the exporter.
   */
  void updateRetainedPathsForPrefix(
      const folly::CIDRNetwork& prefix,
      CanonicalRibEntryInput& entryInput);

  CanonicalRibEncoder encoder_;
  CanonicalRibEntryDeltas pendingDeltas_;
  /*
   * Keep paths referenced by exported entries alive until the same prefix is
   * replaced or withdrawn, so weak intern-pool IDs cannot be reclaimed early.
   */
  folly::F14FastMap<
      folly::CIDRNetwork,
      folly::small_vector<std::shared_ptr<const BgpPath>, 1>>
      retainedPathsByPrefix_;
};

} // namespace facebook::bgp
