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

#include <folly/IPAddress.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/common/Structs.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

namespace facebook::bgp {

int32_t getMinSupportRoutes(
    const facebook::bgp::thrift::BgpNetwork& bgpNetwork);

bool getInstallToFib(const facebook::bgp::thrift::BgpNetwork& bgpNetwork);

bool getRequireNhResolution(
    const facebook::bgp::thrift::BgpNetwork& bgpNetwork);

std::string formatRibOutAnnouncementLog(
    const facebook::bgp::RibOutAnnouncement& announcement);

std::string formatRibOutWithdrawalLog(
    const facebook::bgp::RibOutWithdrawal& withdrawal,
    bool addPath = false);

// finds the largest free interval given current pathID assignments pulled
// from routeInfos
std::pair<uint32_t, uint32_t> findLargestFreePathIdInterval(
    const folly::F14NodeMap<
        nettools::bgplib::BgpPeerId,
        folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>>>& routeInfos,
    uint32_t minPathId,
    uint32_t maxPathId);

// Build the thrift TIpPrefix (afi / num_bits / prefix_bin) for a CIDRNetwork.
neteng::fboss::bgp_attr::TIpPrefix buildTPrefix(
    const folly::CIDRNetwork& prefix);

/*
 * Build the thrift TBgpPath for a single RouteInfo. Does not set is_best_path;
 * the caller sets that on the selected best path.
 */
neteng::fboss::bgp::thrift::TBgpPath toTBgpPath(
    const std::shared_ptr<RouteInfo>& routeinfo,
    const std::shared_ptr<const WeightedNexthopMap>& weightedNexthops);

/*
 * Mark paths that the loc-RIB selection predicate excludes. Shared by every
 * loc-RIB serializer; leaves the field unset for active paths.
 */
void setInactiveFlag(
    neteng::fboss::bgp::thrift::TBgpPath& tPath,
    const RouteInfo& routeinfo);
} // namespace facebook::bgp
