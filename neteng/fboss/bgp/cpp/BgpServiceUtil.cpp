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

#include "BgpServiceUtil.h"

#include <thrift/lib/cpp2/op/Get.h>
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/rib_policy_types.h"
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/routing_policy_types.h"
#include "fboss/agent/AddressUtil.h"
#include "folly/logging/xlog.h"
#include "magic_enum/magic_enum.hpp"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "thrift/lib/cpp/util/EnumUtils.h"

namespace facebook::bgp {

using namespace neteng::fboss::bgp::thrift;
using namespace neteng::fboss::bgp_attr;
using namespace nettools::bgplib;
using namespace network;

using std::vector;

TIpPrefix createTIpPrefix(const folly::CIDRNetwork& prefix) {
  auto addr = prefix.first;
  auto prefixLen = prefix.second;

  TIpPrefix tPrefix;
  if (addr.isV4()) {
    tPrefix.afi() = TBgpAfi::AFI_IPV4;
  } else {
    tPrefix.afi() = TBgpAfi::AFI_IPV6;
  }
  tPrefix.num_bits() = prefixLen;

  // fboss cli use num_bits to distiguish local routes from other routes
  if (addr == kLocalRouteV4Nexthop || addr == kLocalRouteV6Nexthop) {
    tPrefix.num_bits() = 0;
  }

  tPrefix.prefix_bin() = toBinaryAddress(addr).addr()->toStdString();
  return tPrefix;
}

TIpPrefix createTIpPrefix(const folly::IPAddress& addr) {
  if (addr.isV4()) {
    return createTIpPrefix(std::make_pair(addr, 32));
  }
  return createTIpPrefix(std::make_pair(addr, 128));
}

folly::CIDRNetwork tIpPrefixToNetwork(const TIpPrefix& prefix) {
  const auto prefixBin = *prefix.prefix_bin();
  const auto prefixLen = *prefix.num_bits();
  network::thrift::BinaryAddress addr;
  addr.addr() = folly::to<folly::fbstring>(prefixBin);
  return std::make_pair(facebook::network::toIPAddress(addr), prefixLen);
}

std::string TIpPrefixToString(
    const neteng::fboss::bgp_attr::TIpPrefix& prefix) {
  const auto& network = tIpPrefixToNetwork(prefix);
  return folly::IPAddress::networkToString(network);
}

TAsPathSeg createTAsPathSeg(const BgpAttrAsPathSegmentC& seg) {
  TAsPathSeg tSeg;

  // TODO: deprecate this T113736668
  vector<int32_t> asVec;
  // uint32_t
  vector<int64_t> asVec4Byte;

  if (seg.asSet.size()) {
    for (const auto& as : seg.asSet) {
      asVec.push_back(as);
      asVec4Byte.push_back(as);
    }
    tSeg.seg_type() = TAsPathSegType::AS_SET;
  } else if (seg.asSequence.size()) {
    for (const auto& as : seg.asSequence) {
      asVec.push_back(as);
      asVec4Byte.push_back(as);
    }
    tSeg.seg_type() = TAsPathSegType::AS_SEQUENCE;
  } else if (seg.asConfedSequence.size()) {
    for (const auto& as : seg.asConfedSequence) {
      asVec.push_back(as);
      asVec4Byte.push_back(as);
    }
    tSeg.seg_type() = TAsPathSegType::AS_CONFED_SEQUENCE;
  } else if (seg.asConfedSet.size()) {
    for (const auto& as : seg.asConfedSet) {
      asVec.push_back(as);
      asVec4Byte.push_back(as);
    }
    tSeg.seg_type() = TAsPathSegType::AS_CONFED_SET;
  }

  tSeg.asns_4_byte() = asVec4Byte;
  tSeg.asns() = asVec;
  return tSeg;
}

vector<TBgpExtCommunity> createTExtCommunities(
    const BgpAttrExtCommunitiesC& extCommunities) {
  vector<TBgpExtCommunity> tExtComms;
  for (const auto& extComm : extCommunities) {
    TBgpExtCommunity tExtComm;
    const BgpExtCommunityAsSpecificExtTypeC* asExtComm =
        dynamic_cast<const BgpExtCommunityAsSpecificExtTypeC*>(
            extComm.attr.get());
    TBgpExtCommUnion extCommUnion;
    if (asExtComm) {
      TBgpTwoByteAsnExtComm twoByteAsn;
      twoByteAsn.type() = asExtComm->getType();
      twoByteAsn.sub_type() = *asExtComm->getSubType();
      twoByteAsn.asn() = asExtComm->getAsn();
      twoByteAsn.value() = asExtComm->getValue();
      extCommUnion.two_byte_asn() = twoByteAsn;
    } else {
      TBgpRawExtComm rawComm;
      rawComm.value_low() = extComm.getRawValueInWords().first;
      rawComm.value_high() = extComm.getRawValueInWords().second;
      // Pending fix for D union implementation
      // extCommUnion.raw_values() = rawComm;
    }
    tExtComm.u() = extCommUnion;
    tExtComms.emplace_back(tExtComm);
  }
  return tExtComms;
}

TBgpPath createTBgpPath(const facebook::bgp::BgpPath& attr) {
  TBgpPath path;

  // Note that TBgpPath fields are populated in host byte order

  auto tNexthop = createTIpPrefix(attr.getNexthop());
  path.next_hop() = std::move(tNexthop);

  TAsPath tAsPath;
  for (const auto& seg : attr.getAsPath().get()) {
    tAsPath.emplace_back(createTAsPathSeg(seg));
  }

  if (auto topoInfo = attr.getTopologyInfo()) {
    path.topologyInfo() = std::move(*topoInfo);
  }
  if (const auto& backupAddr = attr.getBackupAddr()) {
    path.backup_addr() = createTIpPrefix(*backupAddr);
  }

  path.as_path() = std::move(tAsPath);

  vector<TBgpCommunity> tComms;
  for (const auto& comm : attr.getCommunities().get()) {
    TBgpCommunity tComm;
    tComm.asn() = comm.asn;
    tComm.value() = comm.value;
    tComm.community() = ((int64_t)comm.asn << 16) + comm.value;

    tComms.push_back(tComm);
  }
  path.communities() = std::move(tComms);

  if (attr.getOriginatorId()) {
    path.originator_id() = attr.getOriginatorId();
  }

  vector<int64_t> clusterList;
  for (const auto& cluster : attr.getClusterList().get()) {
    clusterList.emplace_back(cluster);
  }
  path.cluster_list() = std::move(clusterList);

  // TODO: need to change to bit test
  // BgpUpdate2 doesn't populate if a value is set or not
  // so need to change library code (same above)
  if (attr.getLocalPref()) {
    path.local_pref() = *attr.getLocalPref();
  }

  // TODO: populate router id

  // O'r definition and fboss Thrift definitions are same
  path.origin() = static_cast<int>(attr.getOrigin());

  path.extCommunities() = createTExtCommunities(attr.getExtCommunities().get());

  path.med() = attr.getMed();

  path.atomic_aggregate() = attr.getAtomicAggregate();

  const auto& aggregator = attr.getAggregator();
  TBgpAggregator tAggregator;
  tAggregator.asn() = aggregator.asn;
  tAggregator.ip() = aggregator.ip.str();
  path.aggregator() = std::move(tAggregator);

  path.weight() = attr.getWeight();

  return path;
}

void setTResult(
    neteng::fboss::bgp::thrift::TResult& result,
    bool success,
    const std::optional<std::string>& errorMessage) {
  result.success() = success;
  if (!success && errorMessage.has_value()) {
    result.err() = errorMessage.value();
  }
}

PeerGroupValidationResult isPeerGroupConfigValid(
    const rib_policy::TRouteFilterPolicy& policy,
    const folly::F14NodeMap<std::string, thrift::PeerGroup>& peerGroups) {
  // Only validate if key_type is PEER_GROUP_NAME
  if (!policy.key_type().has_value() ||
      policy.key_type().value() != rib_policy::KeyType::PEER_GROUP_NAME) {
    return PeerGroupValidationResult::SUCCESS; // No validation needed for
                                               // other key types
  }

  // Helper function to validate IP version compatibility
  auto validateIpVersionCompatibility =
      [&](const facebook::bgp::routing_policy::PrefixList& prefixList,
          const std::string& filterType,
          const std::string& stmtName,
          bool isAfiIpv4Configured,
          bool isAfiIpv6Configured) -> PeerGroupValidationResult {
    // IP_version unset, no validation needed
    if (!prefixList.ip_version().has_value()) {
      return PeerGroupValidationResult::SUCCESS;
    }

    const auto ipVersion = prefixList.ip_version().value();
    bool isSupported =
        (ipVersion == facebook::bgp::routing_policy::IPVersion::V4)
        ? isAfiIpv4Configured
        : isAfiIpv6Configured;

    if (!isSupported) {
      std::string ipVersionStr = apache::thrift::util::enumNameSafe(ipVersion);
      XLOGF(
          ERR,
          "Peer group '{}' has {} disabled but policy requires {} support in {} filter",
          stmtName,
          ipVersionStr,
          ipVersionStr,
          filterType);

      return (ipVersion == facebook::bgp::routing_policy::IPVersion::V4)
          ? PeerGroupValidationResult::IPV4_AFI_MISMATCH
          : PeerGroupValidationResult::IPV6_AFI_MISMATCH;
    }

    return PeerGroupValidationResult::SUCCESS;
  };

  // Validate each statement key (peer group name) exists in peerGroups_
  for (const auto& [stmtName, stmt] : *policy.statements()) {
    auto peerGroupIt = peerGroups.find(stmtName);
    if (peerGroupIt == peerGroups.end()) {
      XLOGF(ERR, "Peer group '{}' not found in configuration", stmtName);
      return PeerGroupValidationResult::PEER_GROUP_NOT_FOUND;
    }

    // Get the peer group config
    const auto& peerGroup = peerGroupIt->second;

    // Define AFI configuration flags using same logic as Config.cpp
    bool isAfiIpv4Configured = !peerGroup.disable_ipv4_afi().value_or(false);
    bool isAfiIpv6Configured = !peerGroup.disable_ipv6_afi().value_or(false);

    // Validate IP version compatibility in ingress_filter
    if (stmt.ingress_filter()->prefix_list().has_value()) {
      auto error = validateIpVersionCompatibility(
          stmt.ingress_filter()->prefix_list().value(),
          "ingress",
          stmtName,
          isAfiIpv4Configured,
          isAfiIpv6Configured);
      if (error != PeerGroupValidationResult::SUCCESS) {
        return error;
      }
    }

    // Validate IP version compatibility in egress_filter
    if (stmt.egress_filter()->prefix_list().has_value()) {
      auto error = validateIpVersionCompatibility(
          stmt.egress_filter()->prefix_list().value(),
          "egress",
          stmtName,
          isAfiIpv4Configured,
          isAfiIpv6Configured);
      if (error != PeerGroupValidationResult::SUCCESS) {
        return error;
      }
    }
  }

  return PeerGroupValidationResult::SUCCESS; // All validations passed
}

void validatePeerGroupConfigInPolicy(
    neteng::fboss::bgp::thrift::TResult& result,
    const rib_policy::TRouteFilterPolicy& policy,
    const folly::F14NodeMap<std::string, thrift::PeerGroup>& peerGroups) {
  PeerGroupValidationResult error = isPeerGroupConfigValid(policy, peerGroups);

  if (error != PeerGroupValidationResult::SUCCESS) {
    std::string errorMessage(magic_enum::enum_name(error));
    setTResult(result, false, errorMessage);
  } else {
    setTResult(result, true);
  }
}

void validateUpdateGroupRouteFilterPolicy(
    neteng::fboss::bgp::thrift::TResult& result,
    const rib_policy::TRouteFilterPolicy& policy,
    const Config& config) {
  if (!config.isUpdateGroupEnabled()) {
    setTResult(result, true);
    return;
  }

  for (const auto& statement : *policy.statements()) {
    const auto& egressFilter = *statement.second.egress_filter();
    if (egressFilter.prefix_list().has_value() ||
        egressFilter.permissive_mode().has_value()) {
      setTResult(result, false, kUpdateGroupEgressFilterError);
      return;
    }
  }
  setTResult(result, true);
}

PolicyValidationResult validatePeersAndPolicies(
    const std::map<
        std::string,
        std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>>&
        peersPolicy,
    const Config& config,
    const std::shared_ptr<PolicyManager>& policyManager) {
  for (const auto& [peerAddr, directionToPolicy] : peersPolicy) {
    // 1. Validate peer exists
    if (!config.validatePeerExists(peerAddr)) {
      XLOGF(ERR, "Peer '{}' does not exist in BGP configuration", peerAddr);
      return PolicyValidationResult::PEER_NOT_FOUND;
    }

    // 2. Validate policies exist in policy manager
    auto result =
        validateDirectionToPolicyMap(directionToPolicy, policyManager);
    if (result != PolicyValidationResult::SUCCESS) {
      return result;
    }
  }

  return PolicyValidationResult::SUCCESS;
}

PolicyValidationResult validatePeerGroupsAndPolicies(
    const std::map<
        std::string,
        std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>>&
        peerGroupsPolicy,
    const Config& config,
    const std::shared_ptr<PolicyManager>& policyManager) {
  for (const auto& [peerGroupName, directionToPolicy] : peerGroupsPolicy) {
    // 1. Validate peer group exists
    if (!config.validatePeerGroupExists(peerGroupName)) {
      XLOGF(
          ERR,
          "Peer group '{}' does not exist in BGP configuration",
          peerGroupName);
      return PolicyValidationResult::PEER_GROUP_NOT_FOUND;
    }

    // 2. Validate policies exist in policy manager
    auto result =
        validateDirectionToPolicyMap(directionToPolicy, policyManager);
    if (result != PolicyValidationResult::SUCCESS) {
      return result;
    }
  }

  return PolicyValidationResult::SUCCESS;
}

PolicyValidationResult validateDirectionToPolicyMap(
    const std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>&
        directionToPolicy,
    const std::shared_ptr<PolicyManager>& policyManager) {
  if (!policyManager) {
    return PolicyValidationResult::POLICY_NOT_FOUND;
  }

  for (const auto& [direction, policyName] : directionToPolicy) {
    if (!policyManager->isPolicyPresent(policyName)) {
      std::string directionStr =
          (direction == facebook::bgp::bgp_policy::DIRECTION::IN) ? "IN"
                                                                  : "OUT";
      XLOGF(
          ERR,
          "Policy '{}' direction '{}' does not exist in policy configuration",
          policyName,
          directionStr);
      return PolicyValidationResult::POLICY_NOT_FOUND;
    }
  }

  return PolicyValidationResult::SUCCESS;
}

std::unique_ptr<PeerToPolicyMap> resolveEffectivePeerPolicies(
    const Config& config,
    const std::function<bool(const folly::IPAddress&, const BgpPeerConfig&)>&
        filter) {
  auto result = std::make_unique<PeerToPolicyMap>();

  for (const auto& [peerAddr, peerConfig] : config.getPeerToConfig()) {
    if (!filter(peerAddr, *peerConfig)) {
      continue;
    }
    const auto& resolved = peerConfig->commonPeerGroupConfig;
    auto& dirMap = (*result)[peerAddr.str()];
    // Always include both directions with optional semantics:
    // - std::nullopt means "clear/unset this policy"
    // - A value means "set to this policy name"
    dirMap[facebook::bgp::bgp_policy::DIRECTION::IN] =
        resolved.ingressPolicyName;
    dirMap[facebook::bgp::bgp_policy::DIRECTION::OUT] =
        resolved.egressPolicyName;
  }

  return result;
}

std::vector<std::string> getUnsupportedBgpPeerFields(
    const thrift::BgpPeer& peer,
    const folly::F14FastSet<std::string_view>& allowedFields) {
  std::vector<std::string> result;
  apache::thrift::op::for_each_field_id<thrift::BgpPeer>([&]<class Id>(Id) {
    auto name =
        folly::StringPiece(apache::thrift::op::get_name_v<thrift::BgpPeer, Id>);
    if (allowedFields.contains(name)) {
      return;
    }
    auto&& ref = apache::thrift::op::get<Id>(peer);
    bool isSet = false;
    // The lambda is instantiated for every field, and
    // is_non_optional_field_set_manually_or_by_serializer only compiles for
    // non-optional field_ref (not optional_field_ref).
    if constexpr (requires {
                    apache::thrift::
                        is_non_optional_field_set_manually_or_by_serializer(
                            ref);
                  }) {
      isSet =
          apache::thrift::is_non_optional_field_set_manually_or_by_serializer(
              ref);
    } else {
      isSet = ref.has_value();
    }
    if (isSet) {
      result.push_back(name.str());
    }
  });
  return result;
}

} // namespace facebook::bgp
