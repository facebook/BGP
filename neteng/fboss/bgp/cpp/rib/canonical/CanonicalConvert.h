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

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_route_types_types.h"

/*
 * Shared canonical-RIB conversion primitives: the per-path input struct plus
 * the interning-independent projections from a deduplicated BgpPath / its
 * sub-attributes / a peer to their canonical Thrift form. Used by both the
 * one-shot CanonicalRibBuilder and the stateful CanonicalRibEncoder. Each
 * converter layers its own interning and index-lifetime policy on top of these
 * projections; FSDB batching and publication are handled separately by
 * CanonicalRibExporter.
 */

namespace facebook::bgp {

namespace bgp_thrift = ::facebook::neteng::fboss::bgp::thrift;

/*
 * One per-path input to the canonical converters: the deduplicated BgpPath plus
 * the per-(prefix, path)-instance fields that TBgpPathCanonical carries.
 *
 * group and peerDescription are non-owning views; the storage they reference
 * (the static path-group constants and the live RouteInfo's peer) must outlive
 * the build/encode call that consumes them.
 */
struct CanonicalPathInput {
  std::shared_ptr<const BgpPath> path;
  std::string_view
      group; // kBestPathGroup / kMultiPathGroup / kDefaultPathGroup
  bool isBestPath{false};
  std::optional<int64_t> pathId; // RFC 7911 received path id
  std::optional<int64_t> nextHopWeight; // UCMP weight
  folly::IPAddress peerAddr; // advertising peer
  int64_t peerRouterId{0};
  std::string_view peerDescription;
  /* Per-instance operational fields, mirrors TBgpPath */
  std::optional<int64_t> igpCost;
  std::optional<int64_t> lastModifiedTime;
  std::optional<int64_t> pathIdToSend;
  std::optional<std::string> bestPathFilterDescr;
  std::optional<std::string> policyName;
  bool isInactive{false};
};

/*
 * Per-entry fields that TRibEntryCanonical carries.
 */
struct CanonicalEntryFields {
  std::optional<bool> pathSelectionPending;
  std::optional<::facebook::bgp::rib_policy::TPathSelector> activeCpsCriteria;
  std::optional<::facebook::bgp::rib_policy::TRouteAttributeUcmpAction>
      activeCteUcmpAction;
};

/**
 * Convert a deduplicated AS_PATH attribute to its canonical list value.
 *
 * @param asPath Source BGP attribute.
 * @return AS path segments in canonical Thrift form.
 */
std::vector<neteng::fboss::bgp_attr::TAsPathSeg> toTAsPathSegList(
    const nettools::bgplib::BgpAttrAsPathC& asPath);

/**
 * Convert a deduplicated COMMUNITIES attribute to its canonical list value.
 *
 * @param communities Source BGP attribute.
 * @return Communities in canonical Thrift form.
 */
std::vector<neteng::fboss::bgp_attr::TBgpCommunity> toTCommunityList(
    const nettools::bgplib::BgpAttrCommunitiesC& communities);

/**
 * Convert a deduplicated CLUSTER_LIST attribute to its canonical list value.
 *
 * @param clusterList Source BGP attribute.
 * @return Cluster IDs represented as signed Thrift integers.
 */
std::vector<int64_t> toTClusterList(
    const nettools::bgplib::BgpAttrClusterListC& clusterList);

/*
 * EXTENDED COMMUNITIES projection for the canonical form. Only AS-specific
 * ext-communities are representable today (two_byte_asn); others are skipped
 * with a warning rather than pushed as an empty union, because
 * TBgpExtCommUnion.raw_values is not yet available (commented out pending the
 * thrift union fix). Distinct from createTBgpPath's createTExtCommunities,
 * which keeps the legacy placeholder behavior.
 *
 * @param extCommunities Source BGP attribute.
 * @return Representable extended communities in canonical Thrift form.
 */
std::vector<neteng::fboss::bgp::thrift::TBgpExtCommunity>
toCanonicalExtCommunities(
    const nettools::bgplib::BgpAttrExtCommunitiesC& extCommunities);

/*
 * Deduplication-identity fields of a canonical deduped path: next_hop plus the
 * inline scalars (origin, local_pref, med, atomic_aggregate, originator_id,
 * aggregator, topology_info, weight). The list-valued sub-attribute indices are
 * left unset for the caller's interning layer to fill.
 *
 * @param path Deduplicated BGP path to project.
 * @return Partially populated canonical path value.
 */
neteng::fboss::bgp::thrift::TBgpDedupedPath toTBgpDedupedPathBase(
    const BgpPath& path);

/**
 * Build the canonical value for one advertising peer.
 *
 * @param addr Peer address.
 * @param routerId Peer router ID.
 * @param description Current peer description; omitted when empty.
 * @return Canonical peer attribution.
 */
neteng::fboss::bgp::thrift::TCanonicalPeer toTCanonicalPeer(
    const folly::IPAddress& addr,
    int64_t routerId,
    std::string_view description);

/*
 * Apply the per-(prefix, path)-instance fields carried by CanonicalPathInput to
 * a TBgpPathCanonical: is_best_path, next_hop_weight, path_id, and the optional
 * operational fields. path_idx and peer_idx are interning-specific and stay
 * with each converter.
 *
 * @param p Canonical path reference to update. Existing pool indices are
 *     preserved.
 * @param in Source per-route fields.
 */
void applyPerPathInstanceFields(
    bgp_thrift::TBgpPathCanonical& p,
    const CanonicalPathInput& in);

} // namespace facebook::bgp
