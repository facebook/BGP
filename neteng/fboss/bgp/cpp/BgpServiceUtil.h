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

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <thrift/lib/cpp2/op/Get.h>
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/rib_policy_types.h"
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

namespace facebook::bgp {

/**
 * Type alias for peer-to-policy map used by PeerManagerBase.
 * Maps peer address string -> direction -> optional policy name.
 * - std::nullopt means "clear/unset this policy"
 * - A value means "set to this policy name"
 */
using PeerToPolicyMap = folly::F14FastMap<
    std::string,
    folly::F14FastMap<bgp_policy::DIRECTION, std::optional<std::string>>>;

/**
 * Error codes for peer group configuration validation.
 */
enum class PeerGroupValidationResult {
  SUCCESS = 0,
  PEER_GROUP_NOT_FOUND = 1,
  IPV4_AFI_MISMATCH = 2,
  IPV6_AFI_MISMATCH = 3,
};

/**
 * Error codes for policy validation in setPeersPolicy and setPeerGroupsPolicy
 * APIs.
 */
enum class PolicyValidationResult {
  SUCCESS = 0,
  PEER_NOT_FOUND = 1,
  PEER_GROUP_NOT_FOUND = 2,
  POLICY_NOT_FOUND = 3,
  INTERNAL_ERROR = 4,
};

// default group
constexpr auto kDefaultPathGroup = "default";

constexpr auto kUpdateGroupEgressFilterError =
    "Failed to apply route filter policy: egress route filters are not "
    "supported when update groups are enabled";

/*
 * Used in getRibEntries grouping to indicate a path is selected as part of
 * bestpath/ECMP vs when it is not (in that case it will be grouped
 * under "default").
 */
constexpr auto kBestPathGroup = "best";
constexpr auto kMultiPathGroup = "multiPaths";

/**
 * Utility method to create TIpPrefix from IPAddress
 */
neteng::fboss::bgp_attr::TIpPrefix createTIpPrefix(
    const folly::IPAddress& addr);
neteng::fboss::bgp_attr::TIpPrefix createTIpPrefix(
    const folly::CIDRNetwork& prefix);

std::string TIpPrefixToString(const neteng::fboss::bgp_attr::TIpPrefix& prefix);

/**
 * Utility method to create CIDRNetwork from TIpPrefix
 */
folly::CIDRNetwork tIpPrefixToNetwork(
    const neteng::fboss::bgp_attr::TIpPrefix& prefix);

/**
 * Utility method to create TAsPathSeg from BgpAttrAsPathSegmentC
 */
neteng::fboss::bgp_attr::TAsPathSeg createTAsPathSeg(
    const nettools::bgplib::BgpAttrAsPathSegmentC& path);

/**
 * Utility method to create TBgpPath from BgpPath and prefix of
 * CIDRNetwork type
 */
neteng::fboss::bgp::thrift::TBgpPath createTBgpPath(
    const facebook::bgp::BgpPath& attr);

/**
 * Utility method to convert a deduplicated EXTENDED COMMUNITIES container to
 * its thrift representation. Shared by createTBgpPath and the canonical RIB
 * encoder (which interns ext-communities by pointer and rebuilds the value
 * here at snapshot time).
 */
std::vector<neteng::fboss::bgp::thrift::TBgpExtCommunity> createTExtCommunities(
    const nettools::bgplib::BgpAttrExtCommunitiesC& extCommunities);

/**
 * Utility function to set TResult with success status and optional error
 * message
 */
void setTResult(
    neteng::fboss::bgp::thrift::TResult& result,
    bool success,
    const std::optional<std::string>& errorMessage = std::nullopt);

/**
 * Validates peer group configuration in route filter policy.
 *
 * @return PeerGroupValidationResult::SUCCESS if validation passes,
 *         specific error code if validation fails
 */
PeerGroupValidationResult isPeerGroupConfigValid(
    const rib_policy::TRouteFilterPolicy& policy,
    const folly::F14NodeMap<std::string, thrift::PeerGroup>& peerGroups);

/**
 * Validates peer group configuration in route filter policy for Thrift API
 * usage. Calls the string version and populates TResult accordingly.
 */
void validatePeerGroupConfigInPolicy(
    neteng::fboss::bgp::thrift::TResult& result,
    const rib_policy::TRouteFilterPolicy& policy,
    const folly::F14NodeMap<std::string, thrift::PeerGroup>& peerGroups);

/**
 * Validates whether a route filter policy is supported with update groups.
 */
void validateUpdateGroupRouteFilterPolicy(
    neteng::fboss::bgp::thrift::TResult& result,
    const rib_policy::TRouteFilterPolicy& policy,
    const Config& config);

/**
 * Validates peers and their associated policies for setPeersPolicy API.
 *
 * @param peersPolicy Map from peer address to direction-to-policy mapping
 * @param config BGP configuration for peer existence validation
 * @param policyManager Policy manager for policy existence validation
 * @return PolicyValidationResult indicating success or specific error
 */
PolicyValidationResult validatePeersAndPolicies(
    const std::map<
        std::string,
        std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>>&
        peersPolicy,
    const Config& config,
    const std::shared_ptr<PolicyManager>& policyManager);

/**
 * Validates peer groups and their associated policies for setPeerGroupsPolicy
 * API.
 *
 * @param peerGroupsPolicy Map from peer group name to direction-to-policy
 * mapping
 * @param config BGP configuration for peer group existence validation
 * @param policyManager Policy manager for policy existence validation
 * @return PolicyValidationResult indicating success or specific error
 */
PolicyValidationResult validatePeerGroupsAndPolicies(
    const std::map<
        std::string,
        std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>>&
        peerGroupsPolicy,
    const Config& config,
    const std::shared_ptr<PolicyManager>& policyManager);

/**
 * Validates that all policies in directionToPolicy map exist in policy manager
 * @param directionToPolicy Map from direction to policy name
 * @param policyManager Policy manager for policy existence validation
 * @return PolicyValidationResult indicating success or specific error
 */
PolicyValidationResult validateDirectionToPolicyMap(
    const std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>&
        directionToPolicy,
    const std::shared_ptr<PolicyManager>& policyManager);

/**
 * Resolve effective per-peer policies from Config::getPeerToConfig().
 * Iterates peerToConfig in a single pass, includes peers for which the
 * filter predicate returns true. Returns both directions for each peer
 * using std::optional semantics:
 * - std::nullopt means "clear/unset this policy"
 * - A value means "set to this policy name"
 */
std::unique_ptr<PeerToPolicyMap> resolveEffectivePeerPolicies(
    const Config& config,
    const std::function<bool(const folly::IPAddress&, const BgpPeerConfig&)>&
        filter);

/**
 * Returns the names of all fields set on the peer that are not in
 * allowedFields. Returns an empty vector if only allowed fields are present.
 */
std::vector<std::string> getUnsupportedBgpPeerFields(
    const thrift::BgpPeer& peer,
    const folly::F14FastSet<std::string_view>& allowedFields);

} // namespace facebook::bgp
