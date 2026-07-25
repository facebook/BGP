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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/rib/CanonicalConvert.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_route_types_types.h"

namespace facebook::bgp {

namespace bgp_thrift = ::facebook::neteng::fboss::bgp::thrift;

/*
 * One-shot converter from a set of RIB entries to the canonical, deduplicated
 * Thrift form (TCanonicalRibState). Canonical RIB, shadow-RIB, and change-list
 * getters create one builder per snapshot: feed entries via addEntry(), then
 * call build() exactly once.
 *
 * Deduplication piggybacks on BGP's existing interning: BgpPath and the
 * DeDuplicatedAttribute sub-attrs each hand out one shared_ptr per distinct
 * value, so an object's address is a value-unique key. We intern on those
 * addresses (pointer identity == value identity) and only materialize the
 * shared values once, at build(). Because the builder is single-use (built
 * once, then discarded), the intern pools live only for the duration of one
 * conversion and need no liveness tracking, sweeping, or slot reuse.
 *
 * Peers are not interned upstream (RouteInfo holds them by value), so the peer
 * pool keys on (addr, routerId) -- bounded by the device's peer count.
 *
 * Not thread safe: drive from the owning RIB / PeerManager EventBase.
 */
class CanonicalRibBuilder {
 public:
  void addEntry(
      const folly::CIDRNetwork& prefix,
      int64_t ribVersion,
      const std::vector<CanonicalPathInput>& paths,
      const CanonicalEntryFields& entryFields = {});

  bgp_thrift::TCanonicalRibState build();

 private:
  /* Append-only shared_ptr->index intern pool. */
  template <typename ObjT>
  class InternPool {
   public:
    /* Returns {index, wasNewlyInserted}. */
    std::pair<int64_t, bool> internReporting(
        const std::shared_ptr<const ObjT>& obj) {
      auto [it, inserted] = byPtr_.try_emplace(obj, nextIdx_);
      if (inserted) {
        ++nextIdx_;
      }
      return {it->second, inserted};
    }
    int64_t intern(const std::shared_ptr<const ObjT>& obj) {
      return internReporting(obj).first;
    }
    /*
     * The key must have been interned first: internWholePath() interns every
     * sub-attr before buildDedupedPath() looks it up here. at() is the
     * deliberate backstop -- a missing key means an interning bug and throws
     * rather than silently yielding a bogus index.
     */
    int64_t indexOf(const std::shared_ptr<const ObjT>& key) const {
      return byPtr_.at(key);
    }
    template <typename OutT, typename Fn>
    folly::F14FastMap<int64_t, OutT> snapshot(const Fn& toThrift) const {
      folly::F14FastMap<int64_t, OutT> out;
      out.reserve(byPtr_.size());
      for (const auto& [obj, id] : byPtr_) {
        out.emplace(id, toThrift(*obj));
      }
      return out;
    }

   private:
    folly::F14FastMap<std::shared_ptr<const ObjT>, int64_t> byPtr_;
    int64_t nextIdx_{0};
  };

  /*
   * Interns the whole path + its list-valued sub-attrs; returns deduped_paths
   * index. Empty sub-attrs have no deduplicated pointer and carry no dict
   * entry.
   */
  int64_t internWholePath(const std::shared_ptr<const BgpPath>& path);
  int64_t internPeer(
      const folly::IPAddress& addr,
      int64_t routerId,
      std::string_view description);
  bgp_thrift::TBgpDedupedPath buildDedupedPath(const BgpPath& path) const;

  InternPool<BgpPath> wholePathPool_;
  struct PeerKey {
    folly::IPAddress addr;
    int64_t routerId;
    bool operator==(const PeerKey& other) const {
      return addr == other.addr && routerId == other.routerId;
    }
  };
  struct PeerKeyHash {
    std::size_t operator()(const PeerKey& key) const {
      return folly::hash::hash_combine(key.addr, key.routerId);
    }
  };

  InternPool<nettools::bgplib::BgpAttrAsPathC> asPathPool_;
  InternPool<nettools::bgplib::BgpAttrCommunitiesC> communitiesPool_;
  InternPool<nettools::bgplib::BgpAttrExtCommunitiesC> extCommunitiesPool_;
  InternPool<nettools::bgplib::BgpAttrClusterListC> clusterListPool_;

  folly::F14FastMap<PeerKey, int64_t, PeerKeyHash> peerIdxByKey_;
  folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer> peers_;
  int64_t nextPeerId_{0};

  folly::F14FastMap<std::string, bgp_thrift::TRibEntryCanonical> ribEntries_;
  bool built_{false};
};

} // namespace facebook::bgp
