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

#include <limits>

#include <boost/algorithm/string/trim.hpp>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/op/Get.h>

#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyUtils.h"

namespace facebook::bgp {

using nettools::bgplib::BgpAttrCommunitiesC;
using nettools::bgplib::BgpAttrCommunityC;
using routing::PolicyComparisonOperator;

// helper function to convert commmunities to vector of strings
std::vector<std::string> ConvertAttrCommunitiesToStrings(
    const std::vector<BgpAttrCommunityC>& communities) {
  std::vector<std::string> convertedCommunities = {};
  for (const auto& commAttr : communities) {
    convertedCommunities.emplace_back(
        fmt::format("{}:{}", commAttr.asn, commAttr.value));
  }
  return convertedCommunities;
}

// helper function to convert commmunities to set of strings
std::set<std::string> ConvertAttrCommunitiesToStringSet(
    const nettools::bgplib::DeDuplicatedCommunities& communities) {
  std::set<std::string> convertedCommunities{};
  for (const auto& commAttr : communities.get()) {
    convertedCommunities.emplace(
        std::to_string(commAttr.asn) + ":" + std::to_string(commAttr.value));
  }
  return convertedCommunities;
}

// Determine if communities vector has community
bool hasCommunity(
    const std::vector<BgpAttrCommunityC>& communities,
    const BgpAttrCommunityC& communityToFind) {
  return std::find(communities.begin(), communities.end(), communityToFind) !=
      communities.end();
}

// Determine if extended communities vector has extended community
bool hasExtCommunity(
    const std::vector<nettools::bgplib::BgpAttrExtCommunityC>& extCommunities,
    const nettools::bgplib::BgpAttrExtCommunityC& extCommunityToFind) {
  return std::find(
             extCommunities.begin(),
             extCommunities.end(),
             extCommunityToFind) != extCommunities.end();
}

/*
 * Append all communities
 * Add community only if it doesn't already exist. Add to END of community list
 */
BgpAttrCommunitiesC addCommunities(
    const BgpAttrCommunitiesC& currentCommunities,
    const BgpAttrCommunitiesC& communitiesToAdd) {
  BgpAttrCommunitiesC outputCommunities = currentCommunities;
  for (const auto& community : communitiesToAdd) {
    if (!hasCommunity(outputCommunities, community)) {
      outputCommunities.push_back(community);
    }
  }
  return outputCommunities;
}

/*
 * Append all extended communities
 * Add extended community only if it doesn't already exist. Add to END of list
 */
nettools::bgplib::BgpAttrExtCommunitiesC addExtCommunities(
    const nettools::bgplib::BgpAttrExtCommunitiesC& currentExtCommunities,
    const nettools::bgplib::BgpAttrExtCommunitiesC& extCommunitiesToAdd) {
  nettools::bgplib::BgpAttrExtCommunitiesC outputExtCommunities =
      currentExtCommunities;
  for (const auto& extCommunity : extCommunitiesToAdd) {
    if (!hasExtCommunity(outputExtCommunities, extCommunity)) {
      outputExtCommunities.push_back(extCommunity);
    }
  }
  return outputExtCommunities;
}

/*
 * Remove communities. Removes communitiesToRemove from inputCommunities
 * if they exist
 */
BgpAttrCommunitiesC removeCommunities(
    const BgpAttrCommunitiesC& inputCommunities,
    const BgpAttrCommunitiesC& communitiesToRemove) {
  BgpAttrCommunitiesC outputCommunities;
  for (const auto& community : inputCommunities) {
    if (!hasCommunity(communitiesToRemove, community)) {
      outputCommunities.push_back(community);
    }
  }
  return outputCommunities;
}

/*
 * Remove extended communities. Removes extCommunitiesToRemove from
 * inputExtCommunities if they exist
 */
nettools::bgplib::BgpAttrExtCommunitiesC removeExtCommunities(
    const nettools::bgplib::BgpAttrExtCommunitiesC& inputExtCommunities,
    const nettools::bgplib::BgpAttrExtCommunitiesC& extCommunitiesToRemove) {
  nettools::bgplib::BgpAttrExtCommunitiesC outputExtCommunities;
  for (const auto& extCommunity : inputExtCommunities) {
    if (!hasExtCommunity(extCommunitiesToRemove, extCommunity)) {
      outputExtCommunities.push_back(extCommunity);
    }
  }
  return outputExtCommunities;
}

// Remove communities matching regExs
BgpAttrCommunitiesC removeCommunitiesMatchingRegexs(
    const BgpAttrCommunitiesC& inputCommunities,
    const std::vector<boost::regex>& communityRegexs) {
  BgpAttrCommunitiesC outputCommunities;

  auto communitiesStringsFromAttr =
      ConvertAttrCommunitiesToStrings(inputCommunities);

  auto inputItr = inputCommunities.begin();
  for (const auto& community : communitiesStringsFromAttr) {
    bool match = false;
    /*
     * Remove community if it matches any of the regExs, i.e. OR operator
     * For now requirement is to only support one regEx which can implicitly
     * support operators.
     * TODO: AND operator across list of action regExs if needed
     */
    for (const auto& commRegex : communityRegexs) {
      if (regex_match(community, commRegex)) {
        match = true;
        break;
      }
    }

    if (!match) {
      outputCommunities.push_back(*inputItr);
    }
    inputItr++;
  }
  return outputCommunities;
}

PolicyComparisonOperator toPolicyComparisonOperator(
    const routing_policy::ComparisonOperator& tComparisonOperator) {
  switch (tComparisonOperator) {
    case routing_policy::ComparisonOperator::EQ:
      return PolicyComparisonOperator::EQ;
    case routing_policy::ComparisonOperator::GE:
      return PolicyComparisonOperator::GE;
    case routing_policy::ComparisonOperator::LE:
      return PolicyComparisonOperator::LE;
    case routing_policy::ComparisonOperator::NE:
      return PolicyComparisonOperator::NE;
    case routing_policy::ComparisonOperator::GT:
      return PolicyComparisonOperator::GT;
    case routing_policy::ComparisonOperator::LT:
      return PolicyComparisonOperator::LT;
    default:
      throw std::invalid_argument(
          fmt::format(
              "Unsupported PolicyComparisonOperator: {}",
              apache::thrift::util::enumNameSafe(tComparisonOperator)));
  }
}

std::vector<size_t> encodingSchemeToVector(
    const nsf_policy::NsfTeWeightEncoding& encodingScheme) {
  std::vector<size_t> lengths;

  apache::thrift::op::visit_union_with_tag(
      encodingScheme,
      [&lengths]<class Scheme>(auto&&, const Scheme& scheme) {
        apache::thrift::op::for_each_field_id<Scheme>(
            [&lengths, &scheme]<class Id>(Id) {
              lengths.emplace_back(*apache::thrift::op::get<Id>(scheme));
            });
      },
      []() { folly::assume_unreachable(); });
  return lengths;
}

namespace {

void validateSwitchIdAction(
    const std::string& policyName,
    const std::string& termName,
    const bgp_policy::LbwExtCommunityAction& action,
    const std::optional<size_t>& switchId) {
  if (!switchId) {
    throw BgpError(
        fmt::format(
            "Policy '{}', term '{}' encodes switch_id, but the local switch ID is unavailable",
            policyName,
            termName));
  }
  if (!action.encoding_scheme() || !action.encoding_id()) {
    throw BgpError(
        fmt::format(
            "Policy '{}', term '{}' has an incomplete switch_id encoding action",
            policyName,
            termName));
  }

  /*
   * encoding_id selects a field by its position in the encoding scheme. The
   * selected field may be rack_id or plane_id; compatibility depends on its
   * width, not on a hard-coded field name.
   */
  const auto fieldWidths = encodingSchemeToVector(*action.encoding_scheme());
  const auto encodingId = *action.encoding_id();
  if (encodingId < 0 || static_cast<size_t>(encodingId) >= fieldWidths.size()) {
    throw BgpError(
        fmt::format(
            "Policy '{}', term '{}' has invalid switch_id encoding_id {}",
            policyName,
            termName,
            encodingId));
  }

  const auto fieldWidth = fieldWidths[encodingId];
  if (fieldWidth < std::numeric_limits<size_t>::digits &&
      *switchId >= (size_t{1} << fieldWidth)) {
    throw BgpError(
        fmt::format(
            "Policy '{}', term '{}' allocates {} bits for switch_id, but switch ID {} does not fit",
            policyName,
            termName,
            fieldWidth,
            *switchId));
  }
}

} // namespace

void validateSwitchIdEncoding(
    const bgp_policy::BgpPolicies& policies,
    const folly::F14FastSet<std::string>& referencedPolicyNames,
    const std::optional<size_t>& switchId) {
  folly::F14FastMap<std::string, const bgp_policy::BgpPolicyStatement*>
      policiesByName;
  for (const auto& policy : *policies.bgp_policy_statements()) {
    policiesByName.emplace(*policy.name(), &policy);
  }

  std::vector<std::string> policiesToCheck(
      referencedPolicyNames.begin(), referencedPolicyNames.end());
  folly::F14FastSet<std::string> checkedPolicies;
  while (!policiesToCheck.empty()) {
    auto policyName = std::move(policiesToCheck.back());
    policiesToCheck.pop_back();
    if (!checkedPolicies.emplace(policyName).second) {
      continue;
    }
    const auto policy = policiesByName.find(policyName);
    if (policy == policiesByName.end()) {
      // Config::verifyIfPoliciesExist reports missing root policy names.
      continue;
    }
    for (const auto& term : *policy->second->policy_entries()) {
      for (const auto& action : *term.policy_action_entries()) {
        /*
         * Policies form a graph through NEXT_POLICY. checkedPolicies prevents
         * repeated work and makes cycles safe.
         */
        if (action.next_policy_id()) {
          policiesToCheck.push_back(*action.next_policy_id());
        }
        if (!action.lbw_ext_community_action() ||
            *action.lbw_ext_community_action()->type() !=
                bgp_policy::LbwExtCommunityActionType::ENCODE_SWITCH_ID) {
          continue;
        }
        validateSwitchIdAction(
            policyName,
            *term.name(),
            *action.lbw_ext_community_action(),
            switchId);
      }
    }
  }
}

std::unordered_map<std::string, int64_t> decodeValues(
    uint32_t encodedLbw,
    const nsf_policy::NsfTeWeightEncoding& encodingScheme) {
  std::unordered_map<std::string, int64_t> ret;
  apache::thrift::op::visit_union_with_tag(
      encodingScheme,
      [&encodedLbw, &ret]<class Scheme>(auto&&, const Scheme& scheme) {
        size_t startingIdx = 0;
        apache::thrift::op::for_each_field_id<Scheme>(
            [&encodedLbw, &ret, &startingIdx, &scheme]<class Id>(Id) {
              size_t endingIdx =
                  startingIdx + *apache::thrift::op::get<Id>(scheme);
              int64_t value = 0;
              for (auto i = startingIdx; i < endingIdx; i++) {
                value |= ((encodedLbw & (1 << i)) >> startingIdx);
              }
              ret.emplace(apache::thrift::op::get_name_v<Scheme, Id>, value);
              startingIdx = endingIdx;
            });
      },
      []() { folly::assume_unreachable(); });
  return ret;
}

uint32_t encodeValue(
    uint32_t encodedLbw,
    size_t val,
    size_t id,
    const std::vector<size_t>& lengths) {
  XCHECK_GT(lengths.size(), id) << "encoding_id exceeds size of lengths vector";
  XCHECK_LT(val, 1 << lengths[id]) << "value to encode exceeds number of bits";
  size_t startingIdx = 0;
  for (auto i = 0; i < id; i++) {
    startingIdx += lengths[i];
  }
  // mask of the number of bits for encoding
  uint32_t mask = ~(~0U << lengths[id]);
  // clear the bits in encodedLbw
  encodedLbw &= ~(mask << startingIdx);
  // set the bits in encodedLbw
  encodedLbw |= (val & mask) << startingIdx;
  return encodedLbw;
}

float decodeAndAggregateCapacity(
    uint32_t encodedLbw,
    const nsf_policy::NsfTeWeightEncoding& encodingScheme) {
  auto ret = 1.0f;
  apache::thrift::op::visit_union_with_tag(
      encodingScheme,
      [&encodedLbw, &ret]<class Scheme>(auto&&, const Scheme& scheme) {
        size_t startingIdx = 0;
        apache::thrift::op::for_each_field_id<Scheme>(
            [&encodedLbw, &ret, &startingIdx, &scheme]<class Id>(Id) {
              size_t endingIdx =
                  startingIdx + *apache::thrift::op::get<Id>(scheme);
              using AnnotationType =
                  nsf_policy::NsfTeWeightEncodingFieldAnnotation;
              const AnnotationType* annotation = apache::thrift::
                  get_field_annotation<AnnotationType, Scheme, Id>();
              if (annotation && *annotation->is_ucmp_capacity()) {
                size_t value = 0;
                for (auto i = startingIdx; i < endingIdx; i++) {
                  value |= ((encodedLbw & (1 << i)) >> startingIdx);
                }
                ret *= value;
              }
              startingIdx = endingIdx;
            });
      },
      []() { folly::assume_unreachable(); });
  return ret;
}

std::optional<int64_t> parseLinkBandwidthBps(const std::string& lbwStr) {
  folly::F14FastMap<char, int64_t> multiplier = {
      {'K', 1000}, {'M', 1000 * 1000}, {'G', 1000 * 1000 * 1000}};

  // Make a copy for modification
  std::string lbwStrCopy = lbwStr;

  // Trim whitespace from beginning and end of string
  boost::algorithm::trim(lbwStrCopy);

  // Make sure we don't have more than one decimal point in the string
  if (std::count(lbwStrCopy.begin(), lbwStrCopy.end(), '.') > 1) {
    XLOGF(ERR, "invalid lbw string {}: too many decimal points", lbwStr);
    return std::nullopt;
  }

  // Get multiplier if one was provided
  auto mult = 1;
  // Find the first character that is not decimal point or number.
  std::size_t index = lbwStrCopy.find_first_not_of(".0123456789");
  if (index != std::string::npos) {
    // Suffix, if present, must be the last letter
    if (index != lbwStrCopy.length() - 1) {
      XLOGF(ERR, "invalid lbw string {} ", lbwStr);
      return std::nullopt;
    }
    auto suffix = std::toupper(lbwStrCopy[index]);
    auto iter = multiplier.find(suffix);
    if (iter == multiplier.end()) {
      XLOGF(
          ERR,
          "invalid suffix {} for lbw string {}",
          static_cast<char>(suffix),
          lbwStr);
      return std::nullopt;
    }
    mult = iter->second;
    // Remove the suffix
    lbwStrCopy.pop_back();
  }

  try {
    return static_cast<int64_t>(std::stod(lbwStrCopy) * mult);
  } catch (const std::exception& ex) {
    XLOGF(ERR, "Failed to parse bandwidth string {}: {}", lbwStr, ex.what());
    return std::nullopt;
  }
}

std::optional<float> parseLinkBandwidthBytesPerSec(const std::string& lbwStr) {
  auto linkBps = parseLinkBandwidthBps(lbwStr);
  if (linkBps.has_value()) {
    // Convert from bits per second to Bytes per second (per RFC)
    return static_cast<float>(*linkBps) / 8.0f;
  }
  return std::nullopt;
}

/*
 * Helper function to encode Link Bandwidth Extended Community value
 * Encodes type_high (1 byte) + type_low (1 byte) + ASN (2 bytes) + bandwidth as
 * IEEE 754 float (4 bytes) Updates rawValueHigh and rawValueLow directly using
 * bit shifting
 */
void decodeLinkBandwidthExtCommunity(
    const bgp_policy::ExtCommunity& extCommunity,
    uint32_t localAsn,
    uint32_t& rawValueHigh,
    uint32_t& rawValueLow) {
  /* Validate that this is a Link Bandwidth Extended Community */
  using nettools::bgplib::BgpAttrExtCommunityC;
  using nettools::bgplib::BgpExtCommunityAsSpecificExtTypeC;

  uint8_t typeHigh = static_cast<uint8_t>(extCommunity.type_high().value_or(0));
  uint8_t typeLow = static_cast<uint8_t>(extCommunity.type_low().value_or(0));

  /* Verify type_low is Link Bandwidth subtype */
  const uint8_t expectedSubtype =
      static_cast<uint8_t>(BgpAttrExtCommunityC::BGP_EXT_COMMUNITY_SUBTYPES::
                               LINK_BW_COMMUNITY_SUBTYPE);
  if (typeLow != expectedSubtype) {
    XLOGF(
        ERR,
        "Invalid type_low for Link Bandwidth Extended Community: expected 0x{:02X}, got 0x{:02X}",
        expectedSubtype,
        typeLow);
  }

  /* Verify type_high is either transitive or non-transitive */
  if (typeHigh !=
          BgpExtCommunityAsSpecificExtTypeC::kBgpExtCommASTransitiveType &&
      typeHigh !=
          BgpExtCommunityAsSpecificExtTypeC::kBgpExtCommASNonTransitiveType) {
    XLOGF(
        ERR,
        "Unexpected type_high for Link Bandwidth Extended Community: expected 0x{:02X} or 0x{:02X}, got 0x{:02X}",
        BgpExtCommunityAsSpecificExtTypeC::kBgpExtCommASTransitiveType,
        BgpExtCommunityAsSpecificExtTypeC::kBgpExtCommASNonTransitiveType,
        typeHigh);
  }

  /* Initialize rawValueHigh with type_high (byte 0) */
  rawValueHigh = typeHigh;

  /* Add type_low (byte 1) */
  rawValueHigh = (rawValueHigh << 8) | typeLow;

  /* Encode ASN as 2 bytes in rawValueHigh (bytes 2-3, network byte order).
   * For 4-byte ASNs, use AS_TRANS per RFC 6793. */
  uint16_t asn;
  if (localAsn > std::numeric_limits<uint16_t>::max()) {
    XLOGF(
        WARN,
        "4-byte ASN {} cannot be encoded in Link Bandwidth Extended Community. "
        "Using AS_TRANS ({}) per RFC 6793.",
        localAsn,
        kAsTrans);
    asn = kAsTrans;
  } else {
    asn = static_cast<uint16_t>(localAsn);
  }
  rawValueHigh = (rawValueHigh << 16) | asn;

  /* Parse bandwidth value */
  float bandwidthBytesPerSec = 0.0f;
  if (extCommunity.value() && !extCommunity.value()->empty()) {
    const auto& val = *extCommunity.value();

    /* Try parsing as string (e.g., "100G") */
    auto parsedBandwidth = parseLinkBandwidthBytesPerSec(val);
    if (parsedBandwidth.has_value()) {
      /* Successfully parsed as string (e.g., "100G") */
      bandwidthBytesPerSec = *parsedBandwidth;

      /* Convert float to IEEE 754 bytes (big-endian) */
      union {
        float f;
        uint32_t intVal;
      } converter{};
      converter.f = bandwidthBytesPerSec;

      /* Store as rawValueLow (big-endian byte order) */
      rawValueLow = converter.intVal;
    } else {
      /* Invalid format */
      XLOGF(ERR, "Invalid bandwidth value: {} (size: {})", val, val.size());
      rawValueLow = 0;
    }
  } else {
    /* If no value, set to zero */
    rawValueLow = 0;
  }
}

nettools::bgplib::BgpAttrExtCommunityC getBgpAttrExtCommunityC(
    const bgp_policy::ExtCommunity& extCommunity,
    const BgpGlobalConfig* config) {
  /* Maybe this doesn't need to be a crash check? */
  XLOG_IF(
      ERR,
      !extCommunity.type_high(),
      "Unexpected empty type_high; expected type_high to be set on ExtCommunity");

  const uint32_t localAsn = config ? config->localAsn : 0;

  /*
   * Check if this is a Link Bandwidth Extended Community
   * Type: 0x40 (non-transitive) or 0x00 (transitive)
   * Sub-Type: 0x04 (Link Bandwidth)
   */
  const bool isLinkBandwidth = extCommunity.type_low() &&
      (*extCommunity.type_low() ==
       static_cast<uint8_t>(
           nettools::bgplib::BgpAttrExtCommunityC::BGP_EXT_COMMUNITY_SUBTYPES::
               LINK_BW_COMMUNITY_SUBTYPE));

  /*
   * Initialize the two 32-bit values
   * rawValueHigh contains bytes 0-3
   * rawValueLow contains bytes 4-7
   */
  uint32_t rawValueHigh = 0;
  uint32_t rawValueLow = 0;

  /* Only Link Bandwidth Extended Communities are supported */
  if (isLinkBandwidth) {
    decodeLinkBandwidthExtCommunity(
        extCommunity, localAsn, rawValueHigh, rawValueLow);
  } else {
    throw BgpError(
        fmt::format(
            "Unsupported ExtCommunity type: type_high={}, type_low={}. Only Link Bandwidth Extended Communities are supported.",
            extCommunity.type_high().value_or(0),
            extCommunity.type_low().value_or(0)));
  }

  return nettools::bgplib::BgpAttrExtCommunityC(rawValueHigh, rawValueLow);
}

} // namespace facebook::bgp
