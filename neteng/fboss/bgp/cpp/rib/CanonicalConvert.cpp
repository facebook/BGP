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

#include "neteng/fboss/bgp/cpp/rib/CanonicalConvert.h"

#include "configerator/structs/neteng/fboss/bgp/if/gen-cpp2/bgp_attr_types.h"
#include "folly/logging/xlog.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"

namespace facebook::bgp {

using namespace neteng::fboss::bgp::thrift;
using namespace neteng::fboss::bgp_attr;
using namespace nettools::bgplib;

using std::vector;

vector<TAsPathSeg> toTAsPathSegList(const BgpAttrAsPathC& asPath) {
  vector<TAsPathSeg> out;
  out.reserve(asPath.size());
  for (const auto& seg : asPath) {
    out.emplace_back(createTAsPathSeg(seg));
  }
  return out;
}

vector<TBgpCommunity> toTCommunityList(const BgpAttrCommunitiesC& communities) {
  vector<TBgpCommunity> out;
  out.reserve(communities.size());
  for (const auto& comm : communities) {
    TBgpCommunity tComm;
    tComm.asn() = comm.asn;
    tComm.value() = comm.value;
    tComm.community() = (static_cast<int64_t>(comm.asn) << 16) + comm.value;
    out.emplace_back(std::move(tComm));
  }
  return out;
}

vector<int64_t> toTClusterList(const BgpAttrClusterListC& clusterList) {
  vector<int64_t> out;
  out.reserve(clusterList.size());
  for (const auto& cluster : clusterList) {
    out.emplace_back(cluster);
  }
  return out;
}

vector<TBgpExtCommunity> toCanonicalExtCommunities(
    const BgpAttrExtCommunitiesC& extCommunities) {
  vector<TBgpExtCommunity> out;
  out.reserve(extCommunities.size());
  for (const auto& extComm : extCommunities) {
    const auto* asExtComm =
        dynamic_cast<const BgpExtCommunityAsSpecificExtTypeC*>(
            extComm.attr.get());
    if (asExtComm) {
      TBgpExtCommunity tExtComm;
      TBgpExtCommUnion u;
      TBgpTwoByteAsnExtComm twoByteAsn;
      const auto subType = asExtComm->getSubType();
      XCHECK(subType.has_value());
      twoByteAsn.type() = asExtComm->getType();
      twoByteAsn.sub_type() = subType.value();
      twoByteAsn.asn() = asExtComm->getAsn();
      twoByteAsn.value() = asExtComm->getValue();
      u.two_byte_asn() = twoByteAsn;
      tExtComm.u() = u;
      out.emplace_back(std::move(tExtComm));
    } else {
      /*
       * IPv4-specific, Opaque, and other non-AS-specific ext-community types
       * cannot be represented without TBgpExtCommUnion.raw_values. Skip with a
       * warning instead of pushing an empty union (empty unions fail thrift
       * validation).
       */
      const auto rawValue = extComm.getRawValueInWords();
      XLOGF_EVERY_N(
          WARN,
          1000,
          "Skipping non-AS-specific extended community (type {}) with raw values ({}, {})",
          extComm.attr->getType(),
          rawValue.first,
          rawValue.second);
    }
  }
  return out;
}

TBgpDedupedPath toTBgpDedupedPathBase(const BgpPath& path) {
  TBgpDedupedPath attrs;

  /* NEXT_HOP inline (not deduplicated upstream, so no pointer to key). */
  attrs.next_hop() = createTIpPrefix(path.getNexthop());

  // Scalars inline.
  attrs.origin() = static_cast<int32_t>(path.getOrigin());
  if (auto lp = path.getLocalPref()) {
    attrs.local_pref() = lp.value();
  }
  attrs.med() = path.getMed();
  attrs.atomic_aggregate() = path.getAtomicAggregate();
  /*
   * A 0 originator_id is treated as unset and omitted (0 is a valid but unused
   * BGP identifier), mirroring RouteInfo::getBgpRouterId.
   */
  if (auto oid = path.getOriginatorId()) {
    attrs.originator_id() = oid;
  }
  const auto& agg = path.getAggregator();
  if (agg.asn != 0 || !agg.ip.empty()) {
    TBgpAggregator tAgg;
    tAgg.asn() = agg.asn;
    tAgg.ip() = agg.ip.str();
    attrs.aggregator() = std::move(tAgg);
  }
  if (const auto& topoInfo = path.getTopologyInfo()) {
    attrs.topology_info() = topoInfo.value();
  }
  if (const auto& backupAddr = path.getBackupAddr()) {
    attrs.backup_addr() = createTIpPrefix(*backupAddr);
  }
  /* weight is optional; 0 is the default (non-UCMP) value, so omit it then. */
  if (auto weight = path.getWeight()) {
    attrs.weight() = weight;
  }
  return attrs;
}

TCanonicalPeer toTCanonicalPeer(
    const folly::IPAddress& addr,
    int64_t routerId,
    std::string_view description) {
  TCanonicalPeer peer;
  peer.peer_id() = createTIpPrefix(addr);
  peer.router_id() = routerId;
  if (!description.empty()) {
    peer.peer_description() = std::string(description);
  }
  return peer;
}

void applyPerPathInstanceFields(
    bgp_thrift::TBgpPathCanonical& p,
    const CanonicalPathInput& in) {
  if (in.isBestPath) {
    p.is_best_path() = true;
  }
  if (in.nextHopWeight.has_value()) {
    p.next_hop_weight() = in.nextHopWeight.value();
  }
  if (in.pathId.has_value()) {
    p.path_id() = in.pathId.value();
  }
  if (in.igpCost.has_value()) {
    p.igp_cost() = in.igpCost.value();
  }
  if (in.lastModifiedTime.has_value()) {
    p.last_modified_time() = in.lastModifiedTime.value();
  }
  if (in.pathIdToSend.has_value()) {
    p.path_id_to_send() = in.pathIdToSend.value();
  }
  if (in.bestPathFilterDescr.has_value()) {
    p.bestpath_filter_descr() = in.bestPathFilterDescr.value();
  }
  if (in.policyName.has_value()) {
    p.policy_name() = in.policyName.value();
  }
  if (in.isInactive) {
    p.is_inactive() = true;
  }
}

} // namespace facebook::bgp
