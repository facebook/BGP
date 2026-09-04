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

#include "neteng/fboss/bgp/cpp/rib/Utils.h"

#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/common/FeatureFlags.h"

using namespace facebook::neteng::fboss::bgp_attr;
using namespace facebook::neteng::fboss::bgp::thrift;

namespace facebook::bgp {

int32_t getMinSupportRoutes(
    const facebook::bgp::thrift::BgpNetwork& bgpNetwork) {
  return bgpNetwork.minimum_supporting_routes().value_or(0);
}

bool getInstallToFib(const facebook::bgp::thrift::BgpNetwork& bgpNetwork) {
  return bgpNetwork.install_to_fib().value_or(false);
}

bool getRequireNhResolution(
    const facebook::bgp::thrift::BgpNetwork& bgpNetwork) {
  return bgpNetwork.require_nexthop_resolution().value_or(false);
}

std::string formatRibOutAnnouncementLog(
    const facebook::bgp::RibOutAnnouncement& announcement) {
  return fmt::format(
      "Sending prefixes with {} entries and {} addPathEntries, with EoR = {} and Initial dump = {}",
      announcement.entries.size(),
      announcement.addPathEntries.size(),
      announcement.sendWithEoR,
      announcement.initialDump);
}

std::string formatRibOutWithdrawalLog(
    const facebook::bgp::RibOutWithdrawal& withdrawal,
    bool addPath) {
  return fmt::format(
      "Sending {} withdrawals to all{}BGP peers",
      addPath ? withdrawal.addPathEntries.size() : withdrawal.entries.size(),
      addPath ? " add-path " : " ");
}

/*
 * this is a relatively simple way to find a new path ID interval to use once
 * an interval has no more free path IDs. This should be very rarely
 * invoked given that the maximum path ID is quite large and BGPd's release
 * cadence will naturally reset them back to starting from 0 anyway.
 */
std::pair<uint32_t, uint32_t> findLargestFreePathIdInterval(
    const folly::F14NodeMap<
        nettools::bgplib::BgpPeerId,
        folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>>>&
        peerIdToRouteInfos,
    uint32_t minPathId,
    uint32_t maxPathId) {
  XCHECK(minPathId < maxPathId)
      << fmt::format("minPathId={}>=maxPathId={}", minPathId, maxPathId);
  /*
   * Store all ids already in use in the requested range. Use the requested
   * limits as guard posts.
   */
  std::vector<int64_t> usedIds = {
      static_cast<int64_t>(minPathId) - 1, static_cast<int64_t>(maxPathId) + 1};
  for (const auto& [peerId, routeInfos] : peerIdToRouteInfos) {
    for (const auto& [recvPathId, routeInfo] : routeInfos) {
      if (routeInfo->pathIdToSend.has_value() &&
          routeInfo->pathIdToSend.value() >= minPathId &&
          routeInfo->pathIdToSend.value() <= maxPathId) {
        usedIds.push_back(
            static_cast<int64_t>(routeInfo->pathIdToSend.value()));
      }
    }
  }
  // Iterate over the consecutive used ids to find the maximum gap.
  std::sort(usedIds.begin(), usedIds.end());
  uint32_t left = minPathId;
  uint32_t right = maxPathId;
  int64_t maxGap = -1;
  for (int i = 0; i < usedIds.size() - 1; i++) {
    int64_t thisGap = usedIds[i + 1] - usedIds[i] - 1;
    // update max gap vars if this gap is the new max
    if (thisGap > maxGap) {
      left = static_cast<int64_t>(usedIds[i] + 1);
      right = static_cast<int64_t>(usedIds[i + 1] - 1);
      maxGap = thisGap;
    }
  }
  return {left, right};
}

TIpPrefix buildTPrefix(const folly::CIDRNetwork& prefix) {
  TIpPrefix tPrefix;
  tPrefix.afi() = prefix.first.isV4() ? TBgpAfi::AFI_IPV4 : TBgpAfi::AFI_IPV6;
  tPrefix.num_bits() = prefix.second;
  tPrefix.prefix_bin() =
      facebook::network::toBinaryAddress(prefix.first).addr()->toStdString();
  return tPrefix;
}

TBgpPath toTBgpPath(
    const std::shared_ptr<RouteInfo>& routeinfo,
    const std::shared_ptr<const WeightedNexthopMap>& weightedNexthops) {
  auto tPath = createTBgpPath(*(routeinfo->attrs));
  tPath.router_id() = routeinfo->peer.routerId;
  tPath.peer_id() = createTIpPrefix(routeinfo->peer.addr);
  tPath.peer_description() = routeinfo->peer.description;
  if (weightedNexthops) {
    auto it = weightedNexthops->find(routeinfo->peer.addr);
    if (it != weightedNexthops->end()) {
      tPath.next_hop_weight() = it->second;
    }
  }
  if (routeinfo->isNextHopReachable()) {
    tPath.igp_cost() = routeinfo->getIgpCostValue();
  }
  tPath.bestpath_filter_descr() = routeinfo->getBestPathFilterDescr();
  tPath.last_modified_time() = routeinfo->lastModifiedTime_;
  tPath.path_id() = routeinfo->receivedPathId;
  if (routeinfo->pathIdToSend.has_value()) {
    tPath.path_id_to_send() = routeinfo->pathIdToSend.value();
  }
  setInactiveFlag(tPath, *routeinfo);
  return tPath;
}

void setInactiveFlag(
    neteng::fboss::bgp::thrift::TBgpPath& tPath,
    const RouteInfo& routeinfo) {
  if (FeatureFlags::getBgpBestpathFeatures().enableNextHopTracking &&
      !routeinfo.isResolvedForSelection()) {
    tPath.is_inactive() = true;
  }
}

} // namespace facebook::bgp
