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

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>
#include <folly/logging/xlog.h>

#include "configerator/structs/neteng/fboss/bgp/if/gen-cpp2/bgp_attr_types.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalConvert.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_route_types_types.h"

namespace facebook::bgp::canonical {

namespace bgp_attr = ::facebook::neteng::fboss::bgp_attr;
namespace bgp_thrift = ::facebook::neteng::fboss::bgp::thrift;

/** Result of interning one value into a canonical shared pool. */
struct InternResult {
  int64_t id{0};
  bool poolChanged{false};
};

/**
 * Point-in-time resident-slot cardinality of one continuous intern pool.
 *
 * `live` includes expired weak slots until the rate-limited sweep removes
 * them. This keeps collection O(1) on every publication and reports the slots
 * that still consume pool memory. `retired()` counts IDs removed by a sweep;
 * retired IDs are never reused.
 */
struct PoolStats {
  size_t live{0};
  size_t highWater{0};

  /** @return Allocated IDs no longer represented by a tracked pool slot. */
  size_t retired() const {
    XCHECK_LE(live, highWater)
        << "canonical pool live count exceeds its allocation watermark";
    return highWater - live;
  }
};
/*
 * Intern pool for one-shot encoding.
 *
 * The builder receives deduplicated shared objects and finishes synchronously,
 * so this pool owns a shared_ptr for every interned value until the final
 * snapshot is materialized. Pointer identity is value identity because the
 * objects come from BGP's deduplicators. IDs are dense within this one build;
 * no reclamation is needed because the pool is destroyed with the builder.
 */
template <typename ObjT>
class StrongInternPool {
 public:
  /**
   * Return the stable ID for a deduplicated object.
   *
   * @param obj Non-null, deduplicated object retained for this pool's lifetime.
   * @return Stable object ID and whether this call changed the pool.
   */
  InternResult internReporting(const std::shared_ptr<const ObjT>& obj) {
    auto [it, inserted] = byPtr_.try_emplace(obj, nextId_);
    if (inserted) {
      ++nextId_;
    }
    return {.id = it->second, .poolChanged = inserted};
  }

  /**
   * Look up an object already supplied to internReporting().
   *
   * @param obj Deduplicated object whose pointer identity is the lookup key.
   * @return The previously allocated ID.
   * @throws std::out_of_range if the object was not interned.
   */
  int64_t indexOf(const std::shared_ptr<const ObjT>& obj) const {
    return byPtr_.at(obj);
  }

  /**
   * Project every interned source object into an ID-keyed Thrift pool.
   *
   * @tparam ValueT Output value stored for each ID.
   * @param project Callable receiving const ObjT& and returning ValueT.
   * @return A new map containing every live ID and projected value.
   */
  template <typename ValueT, typename ProjectFn>
  folly::F14FastMap<int64_t, ValueT> snapshot(ProjectFn&& project) const {
    folly::F14FastMap<int64_t, ValueT> out;
    out.reserve(byPtr_.size());
    for (const auto& [obj, id] : byPtr_) {
      out.emplace(id, project(*obj));
    }
    return out;
  }

 private:
  folly::F14FastMap<std::shared_ptr<const ObjT>, int64_t> byPtr_;
  int64_t nextId_{0};
};

class PeerPool {
 public:
  /**
   * Intern peer attribution.
   *
   * @param addr Peer address forming part of peer identity.
   * @param routerId Peer router ID forming part of peer identity.
   * @param description Current peer description.
   * @return Stable peer ID and whether the peer pool changed. The latter is
   *     true for a new peer or an existing peer whose description was replaced.
   */
  InternResult internReporting(
      const folly::IPAddress& addr,
      int64_t routerId,
      std::string_view description) {
    auto [it, inserted] = indexByKey_.try_emplace(Key{addr, routerId}, nextId_);
    if (inserted) {
      peers_.emplace(nextId_++, toTCanonicalPeer(addr, routerId, description));
      return {.id = it->second, .poolChanged = true};
    }
    auto& peer = peers_.at(it->second);
    const bool hasStoredDescription = peer.peer_description().has_value();
    const bool descriptionMatches = description.empty()
        ? !hasStoredDescription
        : hasStoredDescription &&
            peer.peer_description().value() == description;
    if (!descriptionMatches) {
      peer = toTCanonicalPeer(addr, routerId, description);
      return {.id = it->second, .poolChanged = true};
    }
    return {.id = it->second, .poolChanged = false};
  }

  /** @return A copy of the complete ID-to-peer map. */
  folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer> snapshot() const& {
    return peers_;
  }

  /** @return The complete ID-to-peer map moved out of an expiring pool. */
  folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer> snapshot() && {
    return std::move(peers_);
  }

 private:
  struct Key {
    folly::IPAddress addr;
    int64_t routerId;
    bool operator==(const Key& other) const {
      return addr == other.addr && routerId == other.routerId;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& key) const {
      return folly::hash::hash_combine(key.addr, key.routerId);
    }
  };
  folly::F14FastMap<Key, int64_t, KeyHash> indexByKey_;
  folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer> peers_;
  int64_t nextId_{0};
};

template <template <typename> class PoolT>
class Encoding {
 public:
  struct BuildEntryResult {
    bgp_thrift::TRibEntryCanonical entry;
    /** True when a referenced shared pool was inserted or refreshed. */
    bool poolsChanged{false};
  };

  /**
   * Encode one prefix without exposing whether shared pools changed.
   *
   * @param prefix Prefix written into the resulting entry.
   * @param ribVersion Loc-RIB version written into the resulting entry.
   * @param paths Candidate inputs. Best-path-only callers may pass the full
   *     candidate set; non-selected inputs are ignored when includePaths=false.
   * @param includeBestPath Populate the entry's separate best_path field. At
   *     most one input may be marked isBestPath; a duplicate selection is
   *     DFATAL in debug builds and the first selection wins in production.
   * @param includePaths Populate grouped path references and intern whole paths
   *     and peers for every input.
   * @return The encoded entry. At least one of includeBestPath or includePaths
   *     should be true for a useful result.
   */
  bgp_thrift::TRibEntryCanonical buildEntry(
      const folly::CIDRNetwork& prefix,
      int64_t ribVersion,
      const std::vector<CanonicalPathInput>& paths,
      bool includeBestPath,
      bool includePaths) {
    return buildEntryReportingChanges(
               prefix, ribVersion, paths, includeBestPath, includePaths)
        .entry;
  }

  /**
   * Encode one prefix and report whether shared pools must be republished.
   * Parameters and entry semantics match buildEntry().
   */
  BuildEntryResult buildEntryReportingChanges(
      const folly::CIDRNetwork& prefix,
      int64_t ribVersion,
      const std::vector<CanonicalPathInput>& paths,
      bool includeBestPath,
      bool includePaths) {
    XCHECK(includeBestPath || includePaths)
        << "canonical entry encoding requires at least one path output";
    BuildEntryResult result;
    auto& entry = result.entry;
    entry.prefix() = createTIpPrefix(prefix);
    entry.rib_version() = ribVersion;
    std::string_view currentGroup;
    std::vector<bgp_thrift::TBgpPathCanonical>* currentPaths{nullptr};
    for (const auto& input : paths) {
      int64_t pathIndex = 0;
      if (includeBestPath && input.isBestPath &&
          entry.best_path().has_value()) {
        XLOGF(
            DFATAL,
            "Multiple canonical paths marked best for prefix {}; keeping the first",
            folly::IPAddress::networkToString(prefix));
      }
      const bool isSelectedBestPath =
          includeBestPath && input.isBestPath && !entry.best_path().has_value();
      if (includePaths) {
        const auto interned = internWholePath(input.path);
        pathIndex = interned.id;
        result.poolsChanged |= interned.poolChanged;
      } else if (isSelectedBestPath) {
        /*
         * A best-path-only entry has no whole-path pool reference, but its
         * attribute indices still need to be present in the dictionaries.
         */
        result.poolsChanged |= internSubAttrs(input.path);
      }

      if (isSelectedBestPath) {
        entry.best_path() = buildPathAttrs(*input.path);
      }

      if (!includePaths) {
        /*
         * Best-path-only encoding intentionally ignores every non-selected
         * input. Callers may pass the complete candidate set so that the same
         * input projection can serve both best-path-only and multipath modes.
         */
        continue;
      }

      bgp_thrift::TBgpPathCanonical path;
      path.path_idx() = pathIndex;
      const auto peer = peerPool_.internReporting(
          input.peerAddr, input.peerRouterId, input.peerDescription);
      path.peer_idx() = peer.id;
      result.poolsChanged |= peer.poolChanged;
      applyPerPathInstanceFields(path, input);
      if (currentPaths == nullptr || input.group != currentGroup) {
        currentGroup = input.group;
        currentPaths = &entry.paths().value()[std::string(currentGroup)];
      }
      currentPaths->push_back(std::move(path));
    }
    return result;
  }

  /**
   * @return A freshly materialized snapshot of every list-valued attribute
   *     pool (AS paths, communities, extended communities, cluster lists).
   */
  bgp_thrift::TBgpAttrDict dictSnapshot() const {
    bgp_thrift::TBgpAttrDict dict;
    dict.as_path_lists() =
        asPathPool_.template snapshot<std::vector<bgp_attr::TAsPathSeg>>(
            toTAsPathSegList);
    dict.community_lists() =
        communitiesPool_
            .template snapshot<std::vector<bgp_attr::TBgpCommunity>>(
                toTCommunityList);
    dict.ext_community_lists() =
        extCommunitiesPool_
            .template snapshot<std::vector<bgp_thrift::TBgpExtCommunity>>(
                toCanonicalExtCommunities);
    dict.cluster_lists() =
        clusterListPool_.template snapshot<std::vector<int64_t>>(
            toTClusterList);
    return dict;
  }

  /**
   * @return A freshly materialized ID-to-deduplicated-path map. Referenced
   *     sub-attributes are expressed as indices into dictSnapshot().
   */
  folly::F14FastMap<int64_t, bgp_thrift::TBgpDedupedPath> pathAttrsSnapshot()
      const {
    return wholePathPool_.template snapshot<bgp_thrift::TBgpDedupedPath>(
        [this](const BgpPath& path) { return buildPathAttrs(path); });
  }

  /** @return A copy of the complete peer pool. */
  folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer> peersSnapshot()
      const& {
    return peerPool_.snapshot();
  }

  /** @return The complete peer pool moved out of an expiring Encoding. */
  folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer> peersSnapshot() && {
    return std::move(peerPool_).snapshot();
  }

  /** @return True when at least one weak object pool reclaimed a slot. */
  bool sweep() {
    bool reclaimed = wholePathPool_.sweep();
    reclaimed |= asPathPool_.sweep();
    reclaimed |= communitiesPool_.sweep();
    reclaimed |= extCommunitiesPool_.sweep();
    reclaimed |= clusterListPool_.sweep();
    return reclaimed;
  }

  /** @return Number of whole-path slots currently tracked. */
  size_t livePathCount() const {
    return wholePathPool_.trackedCount();
  }
  /** @return Total tracked slots across list-valued attribute pools. */
  size_t liveDictEntryCount() const {
    return asPathPool_.trackedCount() + communitiesPool_.trackedCount() +
        extCommunitiesPool_.trackedCount() + clusterListPool_.trackedCount();
  }
  PoolStats wholePathStats() const {
    return wholePathPool_.stats();
  }
  PoolStats asPathStats() const {
    return asPathPool_.stats();
  }
  PoolStats communitiesStats() const {
    return communitiesPool_.stats();
  }
  PoolStats extCommunitiesStats() const {
    return extCommunitiesPool_.stats();
  }
  PoolStats clusterListStats() const {
    return clusterListPool_.stats();
  }

 private:
  bool internSubAttrs(const std::shared_ptr<const BgpPath>& path) {
    bool changed = false;
    if (const auto& value = path->getAsPath().getSharedPtr()) {
      changed |= asPathPool_.internReporting(value).poolChanged;
    }
    if (const auto& value = path->getCommunities().getSharedPtr()) {
      changed |= communitiesPool_.internReporting(value).poolChanged;
    }
    if (const auto& value = path->getExtCommunities().getSharedPtr()) {
      changed |= extCommunitiesPool_.internReporting(value).poolChanged;
    }
    if (const auto& value = path->getClusterList().getSharedPtr()) {
      changed |= clusterListPool_.internReporting(value).poolChanged;
    }
    return changed;
  }

  InternResult internWholePath(const std::shared_ptr<const BgpPath>& path) {
    const auto interned = wholePathPool_.internReporting(path);
    if (interned.poolChanged) {
      internSubAttrs(path);
    }
    return interned;
  }

  bgp_thrift::TBgpDedupedPath buildPathAttrs(const BgpPath& path) const {
    auto attrs = toTBgpDedupedPathBase(path);
    if (const auto& value = path.getAsPath().getSharedPtr()) {
      attrs.as_path_idx() = asPathPool_.indexOf(value);
    }
    if (const auto& value = path.getCommunities().getSharedPtr()) {
      attrs.communities_idx() = communitiesPool_.indexOf(value);
    }
    if (const auto& value = path.getExtCommunities().getSharedPtr()) {
      attrs.ext_communities_idx() = extCommunitiesPool_.indexOf(value);
    }
    if (const auto& value = path.getClusterList().getSharedPtr()) {
      attrs.cluster_list_idx() = clusterListPool_.indexOf(value);
    }
    return attrs;
  }

  PoolT<BgpPath> wholePathPool_;
  PoolT<nettools::bgplib::BgpAttrAsPathC> asPathPool_;
  PoolT<nettools::bgplib::BgpAttrCommunitiesC> communitiesPool_;
  PoolT<nettools::bgplib::BgpAttrExtCommunitiesC> extCommunitiesPool_;
  PoolT<nettools::bgplib::BgpAttrClusterListC> clusterListPool_;
  PeerPool peerPool_;
};

} // namespace facebook::bgp::canonical
