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

#include <fmt/format.h>
#include <folly/logging/xlog.h>
#include <numeric>

#include <configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h>
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfo.h"
#include "neteng/fboss/bgp/cpp/rib/RibEntry.h"
#include "neteng/fboss/bgp/cpp/rib/RibPolicy.h"
#include "neteng/fboss/bgp/cpp/rib/Utils.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"

using std::make_pair;
using std::make_shared;
using std::shared_ptr;
using std::vector;

namespace facebook::bgp {
bool RibEntry::updatePath(
    const TinyPeerInfo& peer,
    shared_ptr<const BgpPath> attrs,
    const bool installToFib,
    const uint32_t receivedPathId,
    std::optional<facebook::bgp::NexthopInfo*> nexthopInfo) noexcept {
  nettools::bgplib::BgpPeerId peerId{peer.addr, peer.routerId};
  // withdrawn path
  if (!attrs) {
    if (routeInfos_.find(peerId) != routeInfos_.end()) {
      // nexthopInfo is present only when enableNexthopTracking is set to True
      if (nexthopInfo.has_value() && nexthopInfo.value() != nullptr) {
        unlinkRouteInfoFromNexthop(peerId, receivedPathId, *nexthopInfo);
      }
      auto ret = routeInfos_[peerId].erase(receivedPathId);
      needPathSelection_ |= ret;
      if (routeInfos_[peerId].empty()) {
        RibStats::decrRibPaths();
        routeInfos_.erase(peerId);
      }
      return ret;
    }
    return false;
  }

  // announced path
  auto iter = routeInfos_.find(peerId);
  std::optional<uint32_t> pathIdToSend = std::nullopt;

  // attributes did not change, no need for update
  if (iter != routeInfos_.end()) {
    auto pathIt = iter->second.find(receivedPathId);
    if (pathIt != iter->second.end()) {
      if (pathIt->second->attrs == attrs && pathIt->second->peer == peer) {
        return false;
      }
      // otherwise, proceed with update. Re-use allocated pathID
      pathIdToSend = pathIt->second->pathIdToSend;
    }
  }

  // Create the RouteInfo with ribEntry reference
  auto routeInfo = std::make_shared<RouteInfo>(
      prefix_, peer, attrs, receivedPathId, *this, pathIdToSend, installToFib);

  // Link the RouteInfo to the NexthopInfo if provided
  linkRouteInfoToNexthop(*routeInfo, nexthopInfo);

  if (iter == routeInfos_.end()) {
    folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>> info{
        {receivedPathId, std::move(routeInfo)}};
    // first time receive a route from peer for this prefix
    RibStats::incrRibPaths();
    routeInfos_.emplace(peerId, std::move(info));
  } else {
    iter->second.insert_or_assign(receivedPathId, std::move(routeInfo));
  }
  needPathSelection_ = true;
  return true;
}

/*
 * Path selection (selectBestPath + 7 phase helpers + PathSelectionInput /
 * MultiPathSelectionResult structs) lives on RibBase as static methods
 * taking RibEntry& by reference. See RibBase::selectBestPath in RibBase.cpp.
 */

std::string RibEntry::getMultipathNexthopsStr() const {
  std::stringstream sstream;
  sstream << "[";
  if (weightedNexthops_) {
    for (const auto& nhwtPair : *weightedNexthops_) {
      if (&nhwtPair != &(*weightedNexthops_->cbegin())) {
        sstream << ", ";
      }
      sstream << "(" << nhwtPair.first.str() << ":" << nhwtPair.second << ")";
    }
  }
  sstream << "]";
  return sstream.str();
}

/**
 * @brief Override route's weighted next-hops with the supplied value.
 *
 * @param weightedNexthops Pre-computed collection of next-hop addresses and
 * corresponding weights.
 */
void RibEntry::overrideWeightedNexthops(WeightedNexthopMap& weigtedNexthops) {
  weightedNexthops_ =
      make_shared<WeightedNexthopMap>(std::move(weigtedNexthops));
}

void RibEntry::unlinkRouteInfoFromNexthop(
    const nettools::bgplib::BgpPeerId& peerId,
    const uint32_t receivedPathId,
    std::optional<facebook::bgp::NexthopInfo*> nexthopInfo) {
  // Only proceed if nexthopInfo is valid
  if (!nexthopInfo.has_value() || *nexthopInfo == nullptr) {
    return;
  }

  // Get the routeInfo before erasing it
  auto routeInfoIter = routeInfos_[peerId].find(receivedPathId);
  if (routeInfoIter != routeInfos_[peerId].end()) {
    auto& routeInfo = routeInfoIter->second;
    // Unlink the RouteInfo from the NexthopInfo
    (*nexthopInfo)->unlinkRouteInfo(*routeInfo);
    XLOGF(
        DBG2,
        "Unlinked RouteInfo for prefix {} from NexthopInfo for {}",
        folly::IPAddress::networkToString(prefix_),
        (*nexthopInfo)->getNextHop().str());
  }
}

void RibEntry::linkRouteInfoToNexthop(
    RouteInfo& routeInfo,
    std::optional<facebook::bgp::NexthopInfo*> nexthopInfo) {
  if (nexthopInfo.has_value() && *nexthopInfo != nullptr) {
    (*nexthopInfo)->linkRouteInfo(routeInfo);
    XLOGF(
        DBG2,
        "Linked RouteInfo for prefix {} to NexthopInfo for {}",
        folly::IPAddress::networkToString(prefix_),
        (*nexthopInfo)->getNextHop().str());
  }
}

/*
 * when a path ID is requested, return the left element of the Rib Entry's free
 * interval and then increment it. The interval is exhausted when the left
 * element exceeds the right one, in which case a new free interval would need
 * to be determined
 */
uint32_t RibEntry::getPathIdToSend() {
  /*
   * if the interval has no remaining free path IDs, we need to find the largest
   * free interval among assigned path IDs
   */
  if (freePathIdInterval_.first > freePathIdInterval_.second) {
    freePathIdInterval_ = findLargestFreePathIdInterval(
        routeInfos_, kMinPathIDToSend, kMaxPathIDToSend);
  }

  // take the ID out of the interval and return it
  return freePathIdInterval_.first++;
}

} // namespace facebook::bgp
