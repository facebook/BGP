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

#include "neteng/fboss/bgp/cpp/adjrib/WellKnownCommunityFilter.h"

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"

DEFINE_bool(
    enable_well_known_community_filter,
    false,
    "Enable RFC 1997 well-known community egress filtering. When true, "
    "NO_EXPORT / NO_ADVERTISE / NO_EXPORT_SUBCONFED on a path suppress "
    "advertisement per the RFC's session-type rules. Default OFF for "
    "bisectable rollout; flip to true to enable, remove the flag once "
    "the feature is permanently on.");

namespace facebook::bgp {

const char* toString(RFC1997Community c) {
  switch (c) {
    case RFC1997Community::NoAdvertise:
      return "NO_ADVERTISE";
    case RFC1997Community::NoExport:
      return "NO_EXPORT";
    case RFC1997Community::NoExportSubconfed:
      return "NO_EXPORT_SUBCONFED";
  }
  return "UNKNOWN";
}

std::optional<RFC1997Community> shouldSuppressByWellKnownCommunity(
    const std::shared_ptr<const BgpPath>& path,
    BgpSessionType sessionType) noexcept {
  /*
   * A null BgpPath here would indicate upstream state corruption. Use
   * DCHECK rather than silently returning nullopt so an erroneously
   * advertised route does not mask the bug. In opt builds the noexcept
   * contract is preserved by falling through to the empty-communities
   * path below.
   */
  DCHECK(path) << "shouldSuppressByWellKnownCommunity: null BgpPath";
  if (!path) {
    return std::nullopt;
  }

  const auto& communities = path->getCommunities().get();
  if (communities.empty()) {
    return std::nullopt;
  }

  bool hasNoExport = false;
  bool hasNoExportSubconfed = false;

  for (const auto& comm : communities) {
    if (comm.asn != WellKnownCommunity::kReservedAsn) {
      continue;
    }
    switch (comm.value) {
      case WellKnownCommunity::kNoAdvertise:
        /*
         * NO_ADVERTISE always wins: never advertise to any peer regardless
         * of session type.
         */
        return RFC1997Community::NoAdvertise;
      case WellKnownCommunity::kNoExport:
        hasNoExport = true;
        break;
      case WellKnownCommunity::kNoExportSubconfed:
        hasNoExportSubconfed = true;
        break;
      default:
        break;
    }
  }

  /*
   * NO_EXPORT_SUBCONFED suppresses on any external peer (EBGP and
   * ConfedEBGP). Only IBGP within the local sub-AS may receive the route.
   */
  if (hasNoExportSubconfed &&
      (sessionType == BgpSessionType::EBGP ||
       sessionType == BgpSessionType::ConfedEBGP)) {
    return RFC1997Community::NoExportSubconfed;
  }

  /*
   * NO_EXPORT suppresses only outside the local AS confederation, i.e. on
   * pure EBGP sessions. ConfedEBGP peers (different sub-AS within the same
   * confederation) and IBGP peers may receive the route.
   */
  if (hasNoExport && sessionType == BgpSessionType::EBGP) {
    return RFC1997Community::NoExport;
  }

  return std::nullopt;
}

void incrementSuppressionStat(RFC1997Community community) {
  switch (community) {
    case RFC1997Community::NoAdvertise:
      BgpStats::incrementWellKnownCommunityNoAdvertiseSuppressed();
      return;
    case RFC1997Community::NoExport:
      BgpStats::incrementWellKnownCommunityNoExportSuppressed();
      return;
    case RFC1997Community::NoExportSubconfed:
      BgpStats::incrementWellKnownCommunityNoExportSubconfedSuppressed();
      return;
  }
  /*
   * Unreachable unless a new enum value is added without updating the
   * switch. Log ERROR and bail gracefully — never crash production on a
   * statistic miss.
   */
  XLOGF(
      ERR,
      "incrementSuppressionStat: unknown RFC1997Community value {}",
      static_cast<int>(community));
}

bool applyWellKnownCommunityFilter(
    const std::shared_ptr<const BgpPath>& attrs,
    BgpSessionType sessionType,
    folly::StringPiece contextLabel,
    const folly::CIDRNetwork& prefix) noexcept {
  auto suppressed = shouldSuppressByWellKnownCommunity(attrs, sessionType);
  if (!suppressed.has_value()) {
    return false;
  }
  const char* communityName = toString(*suppressed);
  XLOGF_EVERY_MS(
      DBG3,
      1000,
      "[WellKnownCommunity] {} suppressing advertisement of {} "
      "(sessionType={}). Reason: {} community present on path",
      contextLabel,
      folly::IPAddress::networkToString(prefix),
      toString(sessionType),
      communityName);
  XLOGF_EVERY_MS(
      INFO,
      60000,
      "[WellKnownCommunity] {} suppressing routes carrying {} "
      "(sessionType={})",
      contextLabel,
      communityName,
      toString(sessionType));
  incrementSuppressionStat(*suppressed);
  return true;
}

} // namespace facebook::bgp
