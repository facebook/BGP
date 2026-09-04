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

#include "ConfigUtils.h"

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/Utils.h"

namespace facebook::bgp {

using nettools::bgplib::BgpAttrAsPathC;
using nettools::bgplib::BgpAttrAsPathSegmentC;
using nettools::bgplib::BgpAttrCommunitiesC;
using nettools::bgplib::BgpAttrCommunity;
using nettools::bgplib::BgpAttrCommunityC;
using nettools::bgplib::DeDuplicatedAsPath;
using namespace neteng::fboss::bgp_attr;

BgpAttrCommunitiesC createBgpAttrCommunitiesC(
    const std::vector<std::string>& communities) {
  BgpAttrCommunitiesC result;
  for (const auto& commStr : communities) {
    auto maybeCommParts = parseCommunityStr(commStr);
    if (maybeCommParts.hasError()) {
      XLOGF(ERR, "{}", maybeCommParts.error());
      continue;
    }
    auto [asn, value] = maybeCommParts.value();
    result.emplace_back(asn, value);
  }
  return result;
}
std::vector<BgpAttrCommunity> createBgpAttrCommunityVec(
    const std::vector<std::string>& communities) {
  std::vector<BgpAttrCommunity> result;
  for (const auto& commStr : communities) {
    auto maybeCommParts = parseCommunityStr(commStr);
    if (maybeCommParts.hasError()) {
      XLOGF(ERR, "{}", maybeCommParts.error());
      continue;
    }
    auto [asn, value] = maybeCommParts.value();
    BgpAttrCommunity community;
    community.asn() = asn;
    community.value() = value;
    result.push_back(std::move(community));
  }
  return result;
}

DeDuplicatedAsPath createBgpAttrAsPathDedup(
    const std::vector<TAsPathSeg>& asPaths) {
  BgpAttrAsPathC result;
  result.reserve(asPaths.size());
  for (const auto& asPath : asPaths) {
    BgpAttrAsPathSegmentC pathSegC;
    /*
     * Backward compatibility: use asns_4_byte if present
     * TODO: deprecate asns T113736668
     */
    std::vector<uint32_t> asns;
    // Python thirft may default to empty list.
    if (asPath.asns_4_byte().is_set() && !asPath.asns_4_byte()->empty()) {
      asns = std::vector<uint32_t>(
          asPath.asns_4_byte()->begin(), asPath.asns_4_byte()->end());
    } else if (asPath.asns().is_set()) {
      asns =
          std::vector<uint32_t>(asPath.asns()->begin(), asPath.asns()->end());
    }
    switch (*asPath.seg_type()) {
      case TAsPathSegType::AS_SET:
        pathSegC.asSet = std::set<uint32_t>(asns.begin(), asns.end());
        break;
      case TAsPathSegType::AS_SEQUENCE:
        pathSegC.asSequence = std::vector<uint32_t>(asns.begin(), asns.end());
        break;
      case TAsPathSegType::AS_CONFED_SEQUENCE:
        pathSegC.asConfedSequence =
            std::vector<uint32_t>(asns.begin(), asns.end());
        break;
      case TAsPathSegType::AS_CONFED_SET:
        pathSegC.asConfedSet = std::set<uint32_t>(asns.begin(), asns.end());
        break;
    }
    result.emplace_back(std::move(pathSegC));
  }
  return DeDuplicatedAsPath{std::move(result)};
}
} // namespace facebook::bgp
