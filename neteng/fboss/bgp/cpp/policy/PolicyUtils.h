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

#include <boost/regex.hpp>
#include <algorithm>

#include <folly/container/F14Set.h>

#include "thrift/lib/cpp/util/EnumUtils.h"

#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "neteng/fboss/bgp/cpp/common/Types.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyStructs.h"

namespace facebook::bgp {

std::vector<std::string> ConvertAttrCommunitiesToStrings(
    const std::vector<nettools::bgplib::BgpAttrCommunityC>& communities);
std::set<std::string> ConvertAttrCommunitiesToStringSet(
    const nettools::bgplib::DeDuplicatedCommunities& communities);

bool hasCommunity(
    const std::vector<nettools::bgplib::BgpAttrCommunityC>& communities,
    const nettools::bgplib::BgpAttrCommunityC& community);
bool hasExtCommunity(
    const std::vector<nettools::bgplib::BgpAttrExtCommunityC>& extCommunities,
    const nettools::bgplib::BgpAttrExtCommunityC& extCommunity);
nettools::bgplib::BgpAttrCommunitiesC addCommunities(
    const nettools::bgplib::BgpAttrCommunitiesC& currentCommunities,
    const nettools::bgplib::BgpAttrCommunitiesC& communitiesToAdd);
nettools::bgplib::BgpAttrExtCommunitiesC addExtCommunities(
    const nettools::bgplib::BgpAttrExtCommunitiesC& currentExtCommunities,
    const nettools::bgplib::BgpAttrExtCommunitiesC& extCommunitiesToAdd);
nettools::bgplib::BgpAttrCommunitiesC removeCommunities(
    const nettools::bgplib::BgpAttrCommunitiesC& inputCommunities,
    const nettools::bgplib::BgpAttrCommunitiesC& communitiesToRemove);
nettools::bgplib::BgpAttrExtCommunitiesC removeExtCommunities(
    const nettools::bgplib::BgpAttrExtCommunitiesC& inputExtCommunities,
    const nettools::bgplib::BgpAttrExtCommunitiesC& extCommunitiesToRemove);
nettools::bgplib::BgpAttrCommunitiesC removeCommunitiesMatchingRegexs(
    const nettools::bgplib::BgpAttrCommunitiesC& inputCommunities,
    const std::vector<boost::regex>& communityRegexs);

routing::PolicyComparisonOperator toPolicyComparisonOperator(
    const routing_policy::ComparisonOperator& tComparisonOperator);

/**
 * Encoded LBW related
 */

// given a value, convert it to an encoded lbw by storing the value in
// the bits referenced by id based on EncodingScheme
std::vector<size_t> encodingSchemeToVector(
    const nsf_policy::NsfTeWeightEncoding& encodingScheme);

/**
 * Validate ENCODE_SWITCH_ID actions reachable from policies used by this BGP
 * instance. The traversal starts at referencedPolicyNames and follows
 * NEXT_POLICY actions; unrelated statements in a shared policy bundle are not
 * validated.
 *
 * Throws BgpError when a reachable action cannot encode switchId, when the
 * switch ID is unavailable, or when its encoding scheme or field ID is
 * incomplete or invalid. This is intended for startup validation before RIB
 * construction, while the policy and resolved BGP configuration are both
 * available.
 */
void validateSwitchIdEncoding(
    const bgp_policy::BgpPolicies& policies,
    const folly::F14FastSet<std::string>& referencedPolicyNames,
    const std::optional<size_t>& switchId);

std::unordered_map<std::string, int64_t> decodeValues(
    uint32_t encodedLbw,
    const nsf_policy::NsfTeWeightEncoding& encodingScheme);

uint32_t encodeValue(
    uint32_t encodedLbw,
    size_t val,
    size_t id,
    const std::vector<size_t>& lengths);

float decodeAndAggregateCapacity(
    uint32_t encodedLbw,
    const nsf_policy::NsfTeWeightEncoding& encodingScheme);

/*
 * @brief: Parse link bandwidth string with optional suffix (K, M, G)
 * and return bandwidth in bits per second.
 *
 * @param lbwStr: Bandwidth string, e.g., "100G", "10M", "1.5G", "1000"
 * @return: Bandwidth in bits per second, or std::nullopt if parsing fails
 *
 * Examples:
 *   "1G"   -> 1,000,000,000 (1 Gbps)
 *   "10M"  -> 10,000,000 (10 Mbps)
 *   "1.5G" -> 1,500,000,000 (1.5 Gbps)
 *   "100"  -> 100 (100 bps, no suffix)
 */
std::optional<int64_t> parseLinkBandwidthBps(const std::string& lbwStr);

/*
 * @brief: Parse link bandwidth string and return bandwidth in Bytes per second.
 * This is the format required by RFC for Link Bandwidth Extended Community.
 *
 * @param lbwStr: Bandwidth string, e.g., "100G", "10M", "1.5G"
 * @return: Bandwidth in Bytes per second as float, or std::nullopt if parsing
 * fails
 *
 * Examples:
 *   "1G"  -> 1.25e+8 (125 MB/s = 1 Gbps / 8)
 *   "10M" -> 1.25e+6 (1.25 MB/s = 10 Mbps / 8)
 */
std::optional<float> parseLinkBandwidthBytesPerSec(const std::string& lbwStr);

/*
 * @brief: Helper function to encode Link Bandwidth Extended Community value
 *
 * @param extCommunity: The extended community containing type and value fields
 * @param localAsn: The local ASN to encode in the community
 * @param rawValueHigh: Reference to the high 32-bit word (bytes 0-3) to update
 * @param rawValueLow: Reference to the low 32-bit word (bytes 4-7) to update
 *
 * @details: Encodes type_high (1 byte) + type_low (1 byte) + ASN (2 bytes) +
 *           bandwidth as IEEE 754 float (4 bytes) using direct byte shifting.
 */
void decodeLinkBandwidthExtCommunity(
    const bgp_policy::ExtCommunity& extCommunity,
    uint32_t localAsn,
    uint32_t& rawValueHigh,
    uint32_t& rawValueLow);

/*
 * @brief: Encodes 8-octet ExtCommunity into BgpAttrExtCommunityC
 * according to RFC 4360.
 *
 * @details:
 * - Type Field  : 1 or 2 octets
 * - Value Field : Remaining octets
 *     0                   1                   2                   3
 *     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *    |  Type high    |  Type low(*)  |                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+          Value                |
 *    |                                                               |
 *    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *    (*) Present for Extended types only, used for the Value field
 *        otherwise.
 */
nettools::bgplib::BgpAttrExtCommunityC getBgpAttrExtCommunityC(
    const bgp_policy::ExtCommunity& extCommunity,
    const BgpGlobalConfig* config);

} // namespace facebook::bgp
