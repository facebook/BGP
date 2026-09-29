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

#include <folly/Function.h>
#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/common/Structs.h"

namespace facebook::bgp {

/** Operation represented by a normalized outbound FIB request. */
enum class FibOutOperation : uint8_t {
  /** The submitted request leaves no route contents in the FIB. */
  NONE = 0,
  /** The submitted request programs route contents in the FIB. */
  PROGRAM = 1,
};

/** Role assigned to one nexthop in an outbound FIB request. */
enum class FibOutNexthopRole : uint8_t {
  PRIMARY = 0,
  BACKUP = 1,
};

/** Platform-normalized nexthop sent by BGP to its FIB adapter. */
struct FibOutNexthop {
  folly::IPAddress address;
  // Own the interface name because retained FIB-out can outlive NexthopInfo.
  std::optional<std::string> interfaceName;
  uint32_t weight{1};
  FibOutNexthopRole role{FibOutNexthopRole::PRIMARY};
  std::optional<bool> connected;

  /** Return true when all normalized nexthop fields are equal. */
  bool operator==(const FibOutNexthop& other) const;

  /**
   * Order every equality field so sort-and-unique preserves canonical identity.
   */
  bool operator<(const FibOutNexthop& other) const;
};

/** Canonicalizable collection of nexthops for one outbound route request. */
using FibNexthopSet = std::vector<FibOutNexthop>;

/** Topology fields attached by FBOSS to primary nexthops, keyed by address. */
using FibOutTopologyInfoMap = NexthopTopoInfoMap;

/** Compact per-route metadata for a normalized outbound FIB request. */
class FibOutRouteMetadata {
 public:
  /** Construct metadata for a route programming request. */
  static FibOutRouteMetadata program(
      std::optional<int32_t> adminDistance = std::nullopt,
      std::optional<int32_t> classId = std::nullopt);

  /** Return the outbound operation. */
  FibOutOperation operation() const;

  /** Return the admin distance when the platform request carries one. */
  std::optional<int32_t> adminDistance() const;

  /** Return the class identifier when the platform request carries one. */
  std::optional<int32_t> classId() const;

  /** Return true when all metadata fields and presence bits are equal. */
  bool operator==(const FibOutRouteMetadata& other) const;

 private:
  static constexpr uint8_t kHasAdminDistance{1 << 0};
  static constexpr uint8_t kHasClassId{1 << 1};
  int32_t adminDistance_{0};
  int32_t classId_{0};
  FibOutOperation operation_{FibOutOperation::NONE};
  uint8_t flags_{0};
};

/**
 * Transient normalized representation of one route submitted to the FIB.
 * An empty default value represents a submitted route removal.
 */
struct FibOutRoute {
  FibNexthopSet nexthops;
  FibOutRouteMetadata metadata;
  std::shared_ptr<const FibOutTopologyInfoMap> topologyInfo;
};

/** Canonical FIB-out state retained for one live RIB prefix. */
struct FibOutState {
  std::shared_ptr<const FibNexthopSet> nexthops;
  FibOutRouteMetadata metadata;
  std::shared_ptr<const FibOutTopologyInfoMap> topologyInfo;
};

static_assert(
    sizeof(FibOutRouteMetadata) == 12,
    "Keep metadata compact because every tracked route stores it");

/** Hash normalized nexthop content for canonical-set lookup. */
struct FibOutNexthopHash {
  /** Return a content hash for one normalized nexthop. */
  size_t operator()(const FibOutNexthop& nexthop) const;
};

/** Hash a normalized nexthop set for canonical-set lookup. */
struct FibNexthopSetHash {
  /** Return an order-sensitive hash for an already normalized set. */
  size_t operator()(const FibNexthopSet& nexthops) const;
};

/**
 * Intern immutable normalized nexthop sets under caller-provided serialization.
 *
 * Callers retain aliasing strong references to a make_shared-owned wrapper.
 * The wrapper removes its weak registry entry in its destructor. The registry
 * itself does not keep a set alive. RibBase calls this registry on its
 * event-base thread during normal operation and after its tasks have stopped
 * during shutdown.
 */
class FibNexthopSets {
 public:
  /** Construct an empty canonical-set registry. */
  FibNexthopSets();

  /** Normalize a set and return the canonical immutable shared instance. */
  std::shared_ptr<const FibNexthopSet> getOrCreate(FibNexthopSet nexthops);

  /** Return the number of canonical sets currently registered. */
  size_t size() const;

  /**
   * Visit each canonical set and its current number of strong owners.
   *
   * The caller must serialize this operation with `getOrCreate()` and
   * `clear()`. The registry keeps only weak references, so the reported count
   * is the number of external aliases and the temporary traversal owner is
   * excluded. Iteration is linear in the number of registered sets and does
   * not scan RIB entries or allocate a second collection.
   */
  void forEach(
      folly::FunctionRef<void(const FibNexthopSet&, size_t)> visitor) const;

  /** Remove every weak registry entry. */
  void clear();

 private:
  /** Shared state retained weakly by each canonical set's RAII wrapper. */
  struct State;
  std::shared_ptr<State> state_;
};

} // namespace facebook::bgp
