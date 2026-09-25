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

// TODO: deprecate this class once ADD-PATH changes are completed

#include "neteng/fboss/bgp/cpp/adjrib/PathIdGenerator.h"
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/Consts.h"

namespace facebook::bgp {
uint32_t PathIdGenerator::getPathId(
    const folly::CIDRNetwork& prefix,
    const folly::IPAddress& nextHop) {
  if (!sendAddPath_) {
    return kDefaultPathID;
  }

  // check if we have any paths received for this prefix
  auto PrefixIter = PathIdCache_.find(prefix);
  if (PrefixIter == PathIdCache_.end()) {
    folly::F14NodeMap<folly::IPAddress, uint32_t> nextHopToPathId;
    PathIdCache_[prefix] = std::move(nextHopToPathId);
    PrefixIter = PathIdCache_.find(prefix);
    pathIdOption_[prefix] = 0;
  }

  // check if we have path id cache for this nexthop and this prefix.
  auto pathIdIter = PrefixIter->second.find(nextHop);
  if (pathIdIter == PrefixIter->second.end()) {
    pathIdIter =
        PrefixIter->second.insert({nextHop, pathIdOption_[prefix]++}).first;
  }
  return pathIdIter->second;
}

std::optional<uint32_t> PathIdGenerator::findPathId(
    const folly::CIDRNetwork& prefix,
    const folly::IPAddress& nextHop) const {
  if (!sendAddPath_) {
    return kDefaultPathID;
  }
  const auto prefixIter = PathIdCache_.find(prefix);
  if (prefixIter == PathIdCache_.end()) {
    return std::nullopt;
  }
  const auto pathIdIter = prefixIter->second.find(nextHop);
  if (pathIdIter == prefixIter->second.end()) {
    return std::nullopt;
  }
  return pathIdIter->second;
}

} // namespace facebook::bgp
