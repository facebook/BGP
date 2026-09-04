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

#include <fmt/format.h>
#include <folly/IPAddress.h>
#include <folly/IntrusiveList.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Structs.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfoBase.h"
#include "neteng/fboss/bgp/cpp/routelib/RouteBase.h"
#include "neteng/fboss/bgp/cpp/routelib/RouteFilter.h"

namespace facebook::bgp {

// Forward declaration to avoid circular dependency
class RibEntry;

enum class RouteStateFlags : unsigned {
  NONE = 0x00,
  DELETED = 0x01,
  PREFERRED = 0x02,
  LOCAL = 0x04, //  Locally-originated
  EXTERNAL = 0x08, // EBGP-learned
  CONFED_EXTERNAL = 0x10, // Confed-EBGP-learned
};

/**
 * Enable bitwise operations on RouteStateFlags.
 */
constexpr inline RouteStateFlags operator|(
    RouteStateFlags lhs,
    RouteStateFlags rhs) {
  return static_cast<RouteStateFlags>(
      static_cast<std::underlying_type<RouteStateFlags>::type>(lhs) |
      static_cast<std::underlying_type<RouteStateFlags>::type>(rhs));
}
constexpr inline RouteStateFlags operator&(
    RouteStateFlags lhs,
    RouteStateFlags rhs) {
  return static_cast<RouteStateFlags>(
      static_cast<std::underlying_type<RouteStateFlags>::type>(lhs) &
      static_cast<std::underlying_type<RouteStateFlags>::type>(rhs));
}
constexpr inline RouteStateFlags operator~(RouteStateFlags rhs) {
  return static_cast<RouteStateFlags>(
      ~static_cast<std::underlying_type<RouteStateFlags>::type>(rhs));
}
inline std::ostream& operator<<(
    std::ostream& os,
    const RouteStateFlags& flags) {
  return os << fmt::format(
             "{0:04x}",
             static_cast<std::underlying_type_t<RouteStateFlags>>(flags));
}

struct RouteInfo : nettools::edge::RouteBase {
  const folly::CIDRNetwork prefix; // v4 or v6 prefix
  const TinyPeerInfo peer;
  const std::shared_ptr<const BgpPath> attrs;
  const uint32_t receivedPathId;
  // only has a value when this path has been selected in best path selection
  std::optional<uint32_t> pathIdToSend;
  /*
   * false if local route's install_to_fib is false to avoid fib programming.
   * true, otherwise.
   */
  bool installToFib;
  RouteStateFlags status{RouteStateFlags::NONE};
  /*
   * Captures information about Bgp BestPathSelection Algo's filter for which
   * this route was not selected as best path.
   */
  std::optional<nettools::edge::RouteFilterConfig> bestPathFilterConfig;

  /*
   * The timestamp of the last modification for route in microseconds.
   * XXX: Since we keep track of lastUpdateRcvdTime in AdjRib, this is now
   * redundant and can be removed if we have memory constraints.
   */
  int64_t lastModifiedTime_;

  RouteInfo(
      const folly::CIDRNetwork& prefixIn, // v4 or v6 prefix
      const TinyPeerInfo& peerIn,
      std::shared_ptr<const BgpPath> attrsIn,
      uint32_t receivedPathId,
      RibEntry& ribEntry,
      std::optional<uint32_t> pathIdToSend = std::nullopt,
      bool installToFib = true);

  folly::CIDRNetwork getPrefix() const;
  uint8_t getBgpPrefixLength() const override;
  int64_t getBgpLocalPreference() const override;
  int64_t getBgpAsPathLen() const override;
  int64_t getBgpAsPathLenWithConfed() const override;
  int64_t getBgpOriginCode() const override;
  int64_t getBgpMedValue() const override;
  uint16_t getBgpWeightValue() const override;
  float getUcmpWeight() const override;
  bool getIsRoutePreferred() const override;
  void setRoutePreferred() override;
  void clearRoutePreferred() override;
  uint64_t getBgpRouterId() const override;
  __uint128_t getBgpPeerIPAsInt() const override;
  __uint128_t getBgpNexthopAsInt() const override;
  int64_t getBgpClusterListLen() const override;
  std::vector<uint32_t> getBgpClusterList() const override;
  uint32_t getIgpCostValue() const override;
  bool getIsRouteExternal() const override;
  void setRouteExternal();
  bool getIsRouteConfedExternal() const override;
  void setRouteConfedExternal();
  bool getIsRouteDeleted() const override;
  void setRouteDeleted() override;
  bool getIsRouteLocal() const;
  void setRouteLocal();
  std::pair<uint32_t /* origin asn */, uint32_t /* peer asn */>
  getOriginAsnAndPeerAsn() const override;
  void clearBestPathFilterCriteria() override;
  void setBestPathFilterCriteria(
      const nettools::edge::RouteFilterConfig& filterConfig) override;
  virtual std::string getBestPathFilterDescr() override;

  // not used
  int64_t getRouterLevelPreferenceFromControllerCommunities() const override;
  int64_t getMetroLevelPreferenceFromControllerCommunities() const override;
  std::vector<uint32_t> getBgpAsPath() const override;

  // convert to string
  std::string str() const;

  // nexthop tracking related declarations
  folly::IntrusiveListHook nextHopListHook_;
  bool isOnNextHopList() const;
  void setNexthopInfo(const NexthopInfoBase* nexthopInfo);
  bool isNextHopReachable() const;
  /*
   * Whether this route's nexthop is resolved for best-path selection. Differs
   * from isNextHopReachable() for a reachable nexthop with no IGP cost: such a
   * nexthop is reported unreachable (and to ODS/CLI as cost-not-set) but stays
   * eligible for selection when the source disables excludeNexthopWithoutCost
   * (FBOSS). See NexthopStatus::isResolvedForSelection().
   */
  bool isResolvedForSelection() const;

  // Get the ribEntry reference
  RibEntry& getRibEntry() const;

 private:
  /*
   * UCMP weight derived from the Link-Bandwidth extended community.
   * Reference b/w used is 1MBytes (not bits!).
   */
  std::optional<float> ucmpWeight_{std::nullopt};

  __uint128_t transformIP2Int(const folly::IPAddress& addr) const;

  /*
   * Non-owning pointer to the NexthopInfo to get the IGP cost
   * Lifetime managed by NexthopInfo via explicit link/unlink calls
   * MUST be null-checked before use
   */
  const NexthopInfoBase* nexthopInfo_{nullptr};

  /*
   * reference to the ribEntry to avoid lookups when this path is accessed
   * independent of RibEntry
   */
  RibEntry& ribEntry;
};

} // namespace facebook::bgp
