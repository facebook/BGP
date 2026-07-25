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

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>

#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook::bgp {

// In-place lower-case conversion.
// Replaces common/strings::toLower without pulling in that dependency.
inline void toLower(std::string& s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return std::tolower(c);
  });
}

int64_t getCurrentTimeMicroSec();

/**
 * Extract the complete trailing decimal switch ID from the first DNS label.
 * For example, rtsw016.u001.example.com maps to 16 and
 * rtsw1024.u001.example.com maps to 1024. Names without a domain suffix or a
 * trailing decimal ID return std::nullopt.
 */
std::optional<size_t> getSwitchId(const std::optional<std::string>& deviceName);

/**
 * Utility method to build BgpEndOfRib
 */
nettools::bgplib::BgpEndOfRib buildEndOfRib(
    const nettools::bgplib::BgpUpdateAfi& afi);

/**
 * Utility method to build BgpRouteRefresh
 */
nettools::bgplib::BgpRouteRefresh buildRouteRefresh(
    const nettools::bgplib::BgpUpdateAfi& afi,
    const nettools::bgplib::BgpRouteRefreshMessageSubtype& subtype,
    const nettools::bgplib::BgpUpdateSafi& safi);

/**
 * Utility method to check if subPrefix is a subnet of parentPrefix
 */
bool isSubnet(
    const folly::CIDRNetwork& subPfx,
    const folly::CIDRNetwork& parentPfx) noexcept;

// Parse a given community string (ex) "123:456" into pair<uint16_t, uint16_t>.
// The first contains asn and the second contains value. If unsuccessful,
// folly::none is returned.
folly::Expected<std::pair<uint16_t, uint16_t>, std::string> parseCommunityStr(
    const std::string& commStr);

// extract add path capability to two bool.
// first bool: true if send add path
// second bool: trye if receive add path
std::pair<bool, bool> getAddPathCapa(
    const std::optional<nettools::bgplib::BgpAddPathSendRec>& capa);

// This logRouteWithNexthops function is used to log the route with nexthops.
// This should be called with the DBG3 level, since the log messages could be
// overwhelm. For example, an FSW has 36 SSW as their next hops i.e. 36 spines.
std::string logRouteWithNexthops(
    const folly::CIDRNetwork& prefix,
    const std::shared_ptr<
        const folly::F14NodeMap<folly::IPAddress, unsigned int>>&
        weightedNexthops);

void writeFileAtomic(folly::StringPiece path);

/**
 * Utility method to list all IP addresses in a CIDR prefix
 *
 * @param prefix The CIDR prefix to enumerate
 * @param maxIPs Maximum number of IPs allowed in the prefix. If the prefix
 *               contains more IPs than this limit, a warning will be logged
 *               and an empty vector will be returned. Defaults to
 *               kDefaultMaxIPsInCIDR.
 */
std::vector<folly::IPAddress> listAllIPsInCIDR(
    const folly::CIDRNetwork& prefix,
    uint64_t maxIPs = kDefaultMaxIPsInCIDR);

} // namespace facebook::bgp
