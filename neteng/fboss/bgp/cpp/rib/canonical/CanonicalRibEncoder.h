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

#include <chrono>
#include <cstdint>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>

#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalRibEncoding.h"
#include "neteng/fboss/bgp/cpp/rib/canonical/WeakInternPool.h"

namespace facebook::bgp {

namespace bgp_thrift = ::facebook::neteng::fboss::bgp::thrift;

/**
 * Stateful encoder for continuous Loc-RIB export.
 *
 * The encoder assigns monotonic IDs to BGP's deduplicated path values while
 * retaining only weak references to them. It is intended to be confined to the
 * RIB EventBase; its methods do not provide internal synchronization. Callers
 * encode changed prefixes, use consumeDirtyAndSweep() to choose patch scope,
 * and materialize pool snapshots only when that method returns true.
 */
class CanonicalRibEncoder {
 public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using PoolStats = canonical::PoolStats;
  /** Per-pool cardinality and high-water marks for exporter diagnostics. */
  struct PoolStatsSnapshot {
    PoolStats wholePath;
    PoolStats asPath;
    PoolStats communities;
    PoolStats extCommunities;
    PoolStats clusterList;
  };

  /**
   * Construct an empty encoder epoch.
   *
   * @param now Initial monotonic time used by reclamation rate limiting. The
   *     default is production behavior; tests may inject a deterministic time.
   */
  explicit CanonicalRibEncoder(TimePoint now = Clock::now())
      : lastSweepTime_(now) {}

  /**
   * Encode one Loc-RIB prefix and update shared intern pools.
   *
   * @param prefix Prefix represented by the entry.
   * @param ribVersion Loc-RIB version associated with the update.
   * @param paths Best path and candidate paths projected from the Loc-RIB.
   * @param exportMultipaths When true, emit grouped path references for every
   *     input; when false, emit only the separate best_path value.
   * @return Canonical representation of the supplied prefix.
   *
   * Any new pool value makes the next consumeDirtyAndSweep() return true so
   * the entry and its referenced pools can be published atomically.
   */
  bgp_thrift::TRibEntryCanonical buildEntry(
      const folly::CIDRNetwork& prefix,
      int64_t ribVersion,
      const std::vector<CanonicalPathInput>& paths,
      bool exportMultipaths);

  /**
   * Record that a path removal may leave reclaimable weak-pool slots.
   * Reclamation remains rate-limited and occurs from consumeDirtyAndSweep().
   */
  void markReclamationPending();

  /**
   * Consume pool mutations and perform a due reclamation sweep.
   *
   * @param now Current monotonic time; injectable for deterministic tests.
   * @return True when the caller must include all pool snapshots in the same
   *     patch as its entry changes. This covers both newly interned values and
   *     reclaimed slots. The accumulated mutation bit is cleared.
   */
  bool consumeDirtyAndSweep(TimePoint now = Clock::now());

  /** @return A fresh snapshot of all list-valued attribute pools. */
  bgp_thrift::TBgpAttrDict dictSnapshot() const;

  /** @return A fresh snapshot of the deduplicated whole-path pool. */
  folly::F14FastMap<int64_t, bgp_thrift::TBgpDedupedPath> pathAttrsSnapshot()
      const;

  /** @return A fresh snapshot of canonical peer attribution. */
  folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer> peersSnapshot() const;

  /** @return Number of whole-path slots currently tracked. */
  size_t livePathAttrsCount() const {
    return encoding_.livePathCount();
  }
  /** @return Total slots currently tracked by list-valued attribute pools. */
  size_t liveDictEntryCount() const {
    return encoding_.liveDictEntryCount();
  }
  /** @return Whether an upstream reclamation request awaits a due sweep. */
  bool reclamationPending() const {
    return reclamationPending_;
  }
  /** @return Current cardinality and high-water mark for each weak pool. */
  PoolStatsSnapshot poolStats() const;

 private:
  canonical::Encoding<canonical::WeakInternPool> encoding_;
  bool poolDirty_{false};
  bool reclamationPending_{false};
  TimePoint lastSweepTime_;
};

} // namespace facebook::bgp
