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

#include "PolicyUtils.h"

#include <memory>
#include <unordered_map>

#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/config/ConfigUtils.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

#include <folly/logging/xlog.h>

namespace facebook::bgp {

using bgp_policy::BgpPolicyActionType;
using bgp_policy::BgpPolicyAtomicMatchType;
using folly::IPAddress;
using nettools::bgplib::BgpAttrOrigin;
using routing_policy::BooleanOperator;
using std::string;
using std::vector;

std::array<bgp_policy::Origin, 3> policyOriginTypes = {
    bgp_policy::Origin::IGP,
    bgp_policy::Origin::EGP,
    bgp_policy::Origin::INCOMPLETE};

std::array<nettools::bgplib::BgpAttrOrigin, 3> attrOriginTypes = {
    nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP,
    nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_EGP,
    nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_INCOMPLETE};

bgp_policy::BgpPolicyAtomicMatch createBgpPolicyAtomicMatch(
    const BgpPolicyAtomicMatchType& type,
    const vector<string>& attr_strings,
    const BooleanOperator& boolean_op,
    const string& structName,
    const string& attrsReferenceName) {
  bgp_policy::BgpPolicyAtomicMatch bgpAtomicMatch;
  bgpAtomicMatch.type() = type;
  switch (type) {
    case BgpPolicyAtomicMatchType::COMMUNITY_LIST: {
      bgp_policy::CommunityList communityList;
      communityList.name() = structName;
      communityList.communities() = attr_strings;
      communityList.boolean_operator() = boolean_op;
      if (!attrsReferenceName.empty()) {
        std::vector<std::string> communityReferences = {attrsReferenceName};
        communityList.community_list_names() = communityReferences;
      }
      bgpAtomicMatch.communities_filter() = communityList;
    } break;

    case BgpPolicyAtomicMatchType::AS_PATH: {
      bgp_policy::AsPathList asPathList;
      asPathList.name() = structName;
      asPathList.as_paths() = attr_strings;
      asPathList.boolean_operator() = boolean_op;
      if (!attrsReferenceName.empty()) {
        std::vector<std::string> asPathReferences = {attrsReferenceName};
        asPathList.as_path_list_names() = asPathReferences;
      }
      bgpAtomicMatch.as_path_filters() = asPathList;
    } break;

    case BgpPolicyAtomicMatchType::COMMUNITY_COUNT: {
      bgp_policy::CommunityCount communityCount;
      communityCount.name() = structName;
      bgpAtomicMatch.community_count() = communityCount;
    } break;

    case BgpPolicyAtomicMatchType::AS_PATH_LEN: {
      routing_policy::CompareNumericValue asPathLength;
      std::vector<routing_policy::CompareNumericValue> asPathLengths;
      asPathLengths.emplace_back(asPathLength);
      bgpAtomicMatch.as_path_len_filter() = asPathLengths;
    } break;

    case BgpPolicyAtomicMatchType::WEIGHT: {
      routing_policy::CompareNumericValue weight;
      bgpAtomicMatch.weight() = weight;
    } break;

    case BgpPolicyAtomicMatchType::AS_PATH_LEN_WITH_CONFED: {
      // Create a vector with one element
      routing_policy::CompareNumericValue compVal;
      compVal.value() = 0;
      compVal.compare_operator() = routing_policy::ComparisonOperator::EQ;
      std::vector<routing_policy::CompareNumericValue> asPathLensWithConfed;
      asPathLensWithConfed.emplace_back(compVal);
      bgpAtomicMatch.as_path_len_with_confed_filter() = asPathLensWithConfed;
    } break;

    case BgpPolicyAtomicMatchType::ORIGIN: {
      bgp_policy::Origin origin = bgp_policy::Origin::IGP;
      bgpAtomicMatch.origin() = origin;
    } break;

    default:
      break;
  }

  return bgpAtomicMatch;
}

bgp_policy::BgpPolicyAtomicMatch createCommunityListMatchWithReference(
    const std::vector<std::string>& attr_strings,
    const std::vector<std::string>& attrsReferenceNames) {
  bgp_policy::BgpPolicyAtomicMatch bgpAtomicMatch;
  bgp_policy::CommunityList communityList;
  communityList.name() = "";
  communityList.communities() = attr_strings;
  communityList.boolean_operator() = routing_policy::BooleanOperator::OR;
  if (!attrsReferenceNames.empty()) {
    communityList.community_list_names() = attrsReferenceNames;
  }
  bgpAtomicMatch.communities_filter() = communityList;
  bgpAtomicMatch.type() = BgpPolicyAtomicMatchType::COMMUNITY_LIST;
  return bgpAtomicMatch;
}

bgp_policy::BgpPolicyAtomicMatch createAsPathListMatchWithReference(
    const std::vector<std::string>& attr_strings,
    const std::vector<std::string>& attrsReferenceNames) {
  bgp_policy::BgpPolicyAtomicMatch bgpAtomicMatch;
  bgpAtomicMatch.type() = BgpPolicyAtomicMatchType::AS_PATH;
  bgp_policy::AsPathList asPathList;
  asPathList.name() = "";
  asPathList.as_paths() = attr_strings;
  asPathList.boolean_operator() = routing_policy::BooleanOperator::OR;
  if (!attrsReferenceNames.empty()) {
    asPathList.as_path_list_names() = attrsReferenceNames;
  }
  bgpAtomicMatch.as_path_filters() = asPathList;
  return bgpAtomicMatch;
}

bgp_policy::CommunityList createCommunityList(
    const vector<string>& attr_strings,
    const BooleanOperator& boolean_op,
    const string& structName) {
  bgp_policy::CommunityList communityList;
  communityList.name() = structName;
  communityList.communities() = attr_strings;
  communityList.boolean_operator() = boolean_op;
  return communityList;
}

bgp_policy::AsPathList createAsPathList(
    const vector<string>& attr_strings,
    const BooleanOperator& boolean_op,
    const string& structName) {
  bgp_policy::AsPathList asPathList;
  asPathList.name() = structName;
  asPathList.as_paths() = attr_strings;
  asPathList.boolean_operator() = boolean_op;
  return asPathList;
}

// Constructs an ExtCommunity as according to the thrift
// definition defined in bgp_policy.thrift.
bgp_policy::ExtCommunity createExtCommunity(
    const uint8_t typeHigh,
    const uint8_t typeLow,
    const std::string& value,
    const std::string& name,
    const std::string& description) {
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = typeHigh;
  extCommunity.type_low() = typeLow;
  extCommunity.value() = value;
  extCommunity.name() = name;
  extCommunity.description() = description;
  return extCommunity;
}

// Constructs a BgpPolicyAction that has ext_communities_action
// field populated.
bgp_policy::BgpPolicyAction createBgpPolicyExtCommunityAction(
    const bgp_policy::BgpAttrChangeActionType routeAction,
    const std::vector<bgp_policy::ExtCommunity>& communities) {
  // Construct ExtCommunityAction object from list of
  // ext community definitions.
  bgp_policy::ExtCommunityAction ecAction;
  ecAction.ext_communities() = communities;

  // action_type is of BgpPolicyActionTypes type, so we are wrapping
  // @routeAction here.
  bgp_policy::BgpPolicyActionTypes actionType;
  actionType.set_route_action(routeAction);

  // Construct BgpPolicyAction object.
  bgp_policy::BgpPolicyAction bpAction;
  bpAction.type() = bgp_policy::BgpPolicyActionType::EXT_COMMUNITY_LIST;
  bpAction.action_type() = std::move(actionType);
  bpAction.ext_communities_action() = std::move(ecAction);
  return bpAction;
}

bgp_policy::BgpPolicyAtomicMatch createPrefixListMatch(
    const vector<routing_policy::PrefixListEntry>& prefixListEntries,
    const vector<string>& attrsReferenceNames) {
  bgp_policy::BgpPolicyAtomicMatch bgpAtomicMatch;
  bgpAtomicMatch.type() = BgpPolicyAtomicMatchType::PREFIX_LIST;
  routing_policy::PrefixList prefixlist;
  prefixlist.prefixes() = prefixListEntries;
  if (!attrsReferenceNames.empty()) {
    prefixlist.prefix_list_names() = attrsReferenceNames;
  }
  bgpAtomicMatch.prefix_filters() = prefixlist;
  return bgpAtomicMatch;
}

routing_policy::PrefixList createPrefixList(
    const vector<routing_policy::PrefixListEntry>& prefixListEntries) {
  routing_policy::PrefixList prefixList;
  prefixList.prefixes() = prefixListEntries;
  return prefixList;
}

routing_policy::PrefixListEntry createPrefixListEntry(
    const string& basePrefix,
    const vector<routing_policy::CompareNumericValue>& lenRange,
    std::optional<int> maxSubnets,
    const std::optional<std::set<std::string>>& communities) {
  routing_policy::PrefixListEntry entry;
  entry.base_prefix() = basePrefix;
  entry.prefix_len_ranges() = lenRange;
  if (maxSubnets) {
    entry.max_allowed_golden_prefix_subnet_count() = *maxSubnets;
  }
  if (communities) {
    entry.communities() = *communities;
  }
  return entry;
}

routing_policy::PrefixListEntry createPrefixListEntry(
    const string& basePrefix,
    const string& regex) {
  routing_policy::PrefixListEntry entry;
  entry.base_prefix() = basePrefix;
  entry.regex() = regex;
  return entry;
}

routing_policy::PrefixListEntry createDefaultPrefixListEntry() {
  // create PrefixListEntry which can be modified outside the function
  routing_policy::CompareNumericValue compareStructEQ;
  compareStructEQ.compare_operator() = routing_policy::ComparisonOperator::EQ;
  compareStructEQ.value() = 24;
  routing_policy::PrefixListEntry entry;
  entry.base_prefix() = kV4Prefix1.first.str();
  entry.prefix_len_ranges() = {compareStructEQ};
  return entry;
}

bgp_policy::BgpPolicyAtomicMatch createOriginMatch(
    const bgp_policy::Origin& origin) {
  bgp_policy::BgpPolicyAtomicMatch atomicMatch;
  atomicMatch.type() = BgpPolicyAtomicMatchType::ORIGIN;
  atomicMatch.origin() = origin;
  return atomicMatch;
}

bgp_policy::BgpPolicyAction createBgpPolicyAction(
    const BgpPolicyActionType& type,
    const vector<string>& communities,
    const string& structName,
    const bgp_policy::Origin& origin) {
  bgp_policy::BgpPolicyAction bgpPolicyAction;
  bgpPolicyAction.type() = type;
  if (type == BgpPolicyActionType::COMMUNITY_LIST) {
    bgp_policy::CommunityAction communityAction;
    communityAction.name() = structName;
    communityAction.communities() = communities;
    bgpPolicyAction.community_action() = communityAction;
  } else if (type == BgpPolicyActionType::ORIGIN) {
    bgpPolicyAction.set_origin() = origin;
  } else if (type == BgpPolicyActionType::DENY) {
    // Nothing to do
  } else if (type == BgpPolicyActionType::PERMIT) {
    // Nothing to do
  }

  return bgpPolicyAction;
}

bgp_policy::BgpPolicyAction createBgpPolicyGotoAction(const string& gotoTerm) {
  bgp_policy::BgpPolicyAction action;
  action.type() = BgpPolicyActionType::GOTO;
  action.next_term_id() = gotoTerm;
  return action;
}

bgp_policy::BgpPolicyAction createBgpPolicyNexthopAction(
    const IPAddress& nexthop) {
  bgp_policy::BgpPolicyAction action;
  action.type() = BgpPolicyActionType::NEXT_HOP;
  routing_policy::NextHop tNexthop;
  tNexthop.next_hop_prefix() = nexthop.str();
  bgp_policy::SetNextHop tBgpNexthop;
  tBgpNexthop.next_hop() = tNexthop;
  action.set_nexthop() = tBgpNexthop;
  return action;
}

bgp_policy::BgpPolicyAction createBgpPolicyMedAction(const uint32_t med) {
  bgp_policy::BgpPolicyAction action;
  action.type() = BgpPolicyActionType::MED;

  bgp_policy::MedAction medAction;
  medAction.med_action_type() = bgp_policy::MedActionType::SET;
  medAction.med_value() = med;
  action.med_action() = medAction;

  return action;
}

bgp_policy::BgpPolicyAction createBgpPolicyWeightAction(const uint16_t weight) {
  bgp_policy::BgpPolicyAction action;
  action.type() = BgpPolicyActionType::WEIGHT;

  bgp_policy::WeightAction weightAction;
  weightAction.weight_action_type() = bgp_policy::WeightActionType::SET;
  weightAction.weight_value() = weight;
  action.weight_action() = std::move(weightAction);

  return action;
}

bgp_policy::BgpPolicyAction createBgpPolicyAsPathOverwriteAction(
    const std::vector<int64_t>& asPathOverwriteList) {
  bgp_policy::BgpPolicyAction action;
  action.type() = BgpPolicyActionType::AS_PATH;
  action.as_path_overwrite_list() = asPathOverwriteList;
  return action;
}

bgp_policy::BgpPolicyAction createActionSetLocalPreference(
    const uint32_t localPref) {
  bgp_policy::BgpPolicyAction bgpPolicyAction;
  bgpPolicyAction.type() = BgpPolicyActionType::SET_LOCAL_PREF;
  bgp_policy::LocalPreference bgpLocalPref;
  bgpLocalPref.local_pref() = localPref;
  bgpPolicyAction.set_local_pref() = bgpLocalPref;
  return bgpPolicyAction;
}

bgp_policy::BgpPolicyAction createBgpPolicyCommunityAction(
    const bgp_policy::CommunityActionType& type,
    const vector<string>& communities,
    const vector<string>& communityReferences) {
  bgp_policy::BgpPolicyAction bgpPolicyAction;
  bgpPolicyAction.type() = BgpPolicyActionType::COMMUNITY_LIST;
  bgp_policy::CommunityAction communityAction;
  communityAction.name() = "";
  communityAction.communities() = communities;
  if (!communityReferences.empty()) {
    communityAction.community_action_list_names() = communityReferences;
  }
  communityAction.action_type() = type;
  bgpPolicyAction.community_action() = communityAction;
  return bgpPolicyAction;
}

bgp_policy::BgpPolicyAction createBgpPolicyLbwExtCommunityAction(
    const bgp_policy::LbwExtCommunityActionType& type,
    const std::optional<nsf_policy::NsfTeWeightEncoding>& encodingScheme,
    const std::optional<int32_t> encodingId) {
  bgp_policy::BgpPolicyAction bgpPolicyAction;
  bgp_policy::LbwExtCommunityAction lbwExtCommunityAction;
  lbwExtCommunityAction.type() = type;
  if (encodingScheme) {
    lbwExtCommunityAction.encoding_scheme() = *encodingScheme;
  }
  if (encodingId) {
    lbwExtCommunityAction.encoding_id() = *encodingId;
  }

  bgpPolicyAction.type() = BgpPolicyActionType::LBW_EXT_COMMUNITY;
  bgpPolicyAction.lbw_ext_community_action() = lbwExtCommunityAction;
  return bgpPolicyAction;
}

bgp_policy::BgpPolicyTerm createBgpPolicyTerm(
    const string& name,
    const string& description,
    const vector<bgp_policy::BgpPolicyAtomicMatch>& matches,
    const vector<bgp_policy::BgpPolicyAction>& actions,
    const bgp_policy::FlowControlAction& termMissAction) {
  bgp_policy::BgpPolicyTerm bgpTerm;
  bgpTerm.name() = name;
  bgpTerm.description() = description;
  bgp_policy::BgpPolicyMatch bgpPolicyMatch;
  for (const auto& bgpMatch : matches) {
    bgpPolicyMatch.match_entries()->emplace_back(bgpMatch);
  }
  bgpTerm.policy_match_entries() = bgpPolicyMatch;
  for (const auto& bgpAction : actions) {
    bgpTerm.policy_action_entries()->emplace_back(bgpAction);
  }
  bgpTerm.term_miss_action() = termMissAction;
  return bgpTerm;
}

bgp_policy::BgpPolicyStatement createBgpPolicyStatement(const string& name) {
  bgp_policy::BgpPolicyStatement bgpStatement;
  bgpStatement.name() = name;
  bgpStatement.description() = "";
  bgpStatement.policy_version() = "0";
  return bgpStatement;
}

bgp_policy::BgpPolicyStatement createBgpPolicyStatement(
    const string& name,
    const vector<bgp_policy::BgpPolicyTerm>& terms) {
  auto bgpPolicyStatement = createBgpPolicyStatement(name);
  bgpPolicyStatement.policy_entries() = terms;
  return bgpPolicyStatement;
}

bgp_policy::BgpPolicies createBgpPolicies(
    const string& statementName,
    const vector<bgp_policy::BgpPolicyAtomicMatch>& matches,
    const vector<bgp_policy::BgpPolicyAction>& actions) {
  bgp_policy::BgpPolicyStatement bgpPolicyStatement =
      createBgpPolicyStatement(statementName);
  bgp_policy::BgpPolicyMatch bgpPolicyMatch;
  for (const auto& bgpMatch : matches) {
    bgpPolicyMatch.match_entries()->emplace_back(bgpMatch);
  }
  auto bgpTerm1 = createBgpPolicyTerm("term1");
  bgpTerm1.policy_match_entries() = bgpPolicyMatch;
  for (const auto& bgpAction : actions) {
    bgpTerm1.policy_action_entries()->emplace_back(bgpAction);
  }
  bgpPolicyStatement.policy_entries()->emplace_back(bgpTerm1);

  bgp_policy::BgpPolicies bgpPolicies;
  bgpPolicies.bgp_policy_statements()->emplace_back(bgpPolicyStatement);
  return bgpPolicies;
}

// Create a policy, taking vector of terms (each term must be fully constructed)
bgp_policy::BgpPolicies createBgpPolicies(
    const string& statementName,
    const vector<bgp_policy::BgpPolicyTerm>& terms) {
  bgp_policy::BgpPolicyStatement bgpPolicyStatement =
      createBgpPolicyStatement(statementName);
  bgpPolicyStatement.policy_entries() = terms;
  bgp_policy::BgpPolicies bgpPolicies;
  bgpPolicies.bgp_policy_statements()->emplace_back(bgpPolicyStatement);
  return bgpPolicies;
}

// Create a policy, taking vector of terms, vector of community lists,
// aspathLists, and prefixLists,
bgp_policy::BgpPolicies createBgpPoliciesWithReferences(
    const string& statementName,
    const vector<bgp_policy::BgpPolicyAtomicMatch>& matches,
    const vector<bgp_policy::BgpPolicyAction>& actions,
    const vector<bgp_policy::CommunityList>& communityLists,
    const vector<bgp_policy::AsPathList>& asPathLists,
    const vector<routing_policy::PrefixList>& prefixLists) {
  auto bgpPolicies = createBgpPolicies(statementName, matches, actions);
  bgpPolicies.community_lists() = communityLists;
  bgpPolicies.aspath_lists() = asPathLists;
  bgpPolicies.prefix_lists() = prefixLists;
  return bgpPolicies;
}

std::shared_ptr<BgpPath> createBgpPath(
    const vector<string>& communities,
    const vector<nettools::bgplib::BgpAttrAsPathSegmentC>& aspaths,
    const std::shared_ptr<facebook::bgp::BgpPathFields>& attrFields,
    const BgpAttrOrigin& origin) {
  auto attr = std::make_shared<facebook::bgp::BgpPath>();
  if (attrFields) {
    attr = std::make_shared<facebook::bgp::BgpPath>(*attrFields);
  }
  if (!communities.empty()) {
    attr->setCommunities(facebook::bgp::createBgpAttrCommunitiesC(communities));
  }
  if (!aspaths.empty()) {
    attr->setAsPath(static_cast<nettools::bgplib::BgpAttrAsPathC>(aspaths));
  }
  attr->setOrigin(origin);
  return attr;
}

bool compareBgpAttr(
    const nettools::bgplib::BgpAttributes& actual,
    const facebook::bgp::BgpPath& expected) {
  if (expected.getOrigin() != *actual.origin()) {
    XLOG(INFO, "Origin not equal ");
    return false;
  }

  if (auto pref = actual.localPref()) {
    if (expected.getLocalPref() != *pref) {
      XLOG(INFO, "LocalPref not equal ");
      return false;
    }
  } else {
    if (expected.getLocalPref() != std::nullopt) {
      XLOG(INFO, "Expected not empty but actual is empty.");
      return false;
    }
  }

  auto expectedAsPathSize =
      expected.getAsPath().nullOrEmpty() ? 0 : expected.getAsPath()->size();

  if (expectedAsPathSize != actual.asPath()->size()) {
    XLOG(INFO, "AsPath Size not equal ");
    return false;
  }
  for (int i = 0; i < expectedAsPathSize; i++) {
    if ((*expected.getAsPath())[i] != actual.asPath()[i]) {
      XLOG(INFO, "AsPath not equal: ");
      return false;
    }
  }

  auto expectedCommunitiesSize = expected.getCommunities().nullOrEmpty()
      ? 0
      : expected.getCommunities()->size();
  if (expectedCommunitiesSize != actual.communities()->size()) {
    XLOG(INFO, "Community Size not equal");
    return false;
  }
  for (int i = 0; i < expectedCommunitiesSize; i++) {
    if ((*expected.getCommunities())[i] != actual.communities()[i]) {
      XLOG(INFO, "Community not equal");
      return false;
    }
  }
  return true;
}

std::vector<nettools::bgplib::BgpAttrAsPathSegmentC> createAsPathSequence(
    const std::vector<uint32_t>& asSeq) {
  return {nettools::bgplib::BgpAttrAsPathSegmentC{.asSequence = asSeq}};
}

std::vector<nettools::bgplib::BgpAttrAsPathSegmentC>
createAsPathSequenceWithConfedAs(
    const std::vector<uint32_t>& asSeq,
    const std::vector<uint32_t>& asConfedSeq) {
  return {nettools::bgplib::BgpAttrAsPathSegmentC{
      .asSequence = asSeq, .asConfedSequence = asConfedSeq}};
}

std::vector<nettools::bgplib::BgpAttrAsPathSegmentC>
createAsPathSequenceWithConfedAsSeparate(
    const std::vector<uint32_t>& asSeq,
    const std::vector<uint32_t>& asConfedSeq) {
  return {
      nettools::bgplib::BgpAttrAsPathSegmentC{.asConfedSequence = asConfedSeq},
      nettools::bgplib::BgpAttrAsPathSegmentC{.asSequence = asSeq}};
}

std::optional<std::string> parseActionConfigGetError(
    const bgp_policy::BgpPolicyAction& action) {
  const string policyName = "Policy Statement";
  const auto& policyConfig = createBgpPolicies(policyName, {}, {action});
  try {
    PolicyManager policyManager(policyConfig, createTestBgpGlobalConfig());
  } catch (const BgpError& error) {
    return *error.message();
  }
  return std::nullopt;
}

std::optional<std::string> parseMatchConfigGetError(
    const bgp_policy::BgpPolicyAtomicMatch& match) {
  const string policyName = "Policy Statement";
  const auto& policyConfig = createBgpPolicies(policyName, {match}, {});
  try {
    PolicyManager policyManager(policyConfig, createTestBgpGlobalConfig());
  } catch (const BgpError& error) {
    return *error.message();
  }
  return std::nullopt;
}

// Create BgpPath with a specific origin value
std::shared_ptr<facebook::bgp::BgpPath> createBgpPathWithOrigin(
    const BgpAttrOrigin& origin) {
  auto attrFields = std::make_shared<facebook::bgp::BgpPathFields>();
  facebook::nettools::bgplib::BgpAttributesC mutableAttrs;
  mutableAttrs.origin = origin;
  attrFields->attrs = std::move(mutableAttrs);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrFields);
  return attrs;
}

std::shared_ptr<facebook::bgp::BgpPath> createBgpPathWithWeight(
    const uint16_t& weight) {
  auto attrFields = std::make_shared<facebook::bgp::BgpPathFields>();
  facebook::nettools::bgplib::BgpAttributesC mutableAttrs;
  mutableAttrs.weight = weight;
  attrFields->attrs = std::move(mutableAttrs);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrFields);
  return attrs;
}

// Helper to create a minimal BgpGlobalConfig for testing
BgpGlobalConfig* createTestBgpGlobalConfig() {
  static BgpGlobalConfig config{
      kAsn1, // localAsn
      kLocalAddr1, // routerId
      kPeerAddr3, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      {}, // networksV4
      {}, // networksV6
  };
  return &config;
}

// Helper to create BgpGlobalConfig with specific ASN for testing
BgpGlobalConfig createConfigWithAsn(uint32_t asn) {
  return BgpGlobalConfig(
      asn, // localAsn
      kLocalAddr1, // routerId
      kPeerAddr3, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      std::
          unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>{}, // networksV4
      std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>{} // networksV6
  );
}

// Helper to create BgpGlobalConfig with enablePolicyDefaultAction=true
BgpGlobalConfig createConfigWithPolicyDefaultAction() {
  return BgpGlobalConfig(
      kAsn1, // localAsn
      kLocalAddr1, // routerId
      kPeerAddr3, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      std::
          unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>{}, // networksV4
      std::
          unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>{}, // networksV6
      std::nullopt, // localConfedAsn
      ComputeUcmpFromLbwComm{false},
      0, // ucmpWidth
      std::nullopt, // ucmpQuantizer
      ValidateRemoteAs{true},
      SupportStatefulGr{true},
      EnableServerSocket{true},
      AllowLoopbackReflection{false},
      CountConfedsInAsPathLen{false},
      std::nullopt, // deviceName
      std::nullopt, // switchLimitConfig
      std::nullopt, // dynamicPeerLimit
      std::nullopt, // streamSubscriberLimit
      EnableNexthopTracking{false},
      {}, // includeInterfaceRegexes
      EnableDynamicPolicyEvaluation{false},
      std::nullopt, // thriftServerConfig
      false, // enableEgressQueueBackpressure
      false, // enableUpdateGroup
      UpdateGroupConfig{}, // updateGroupConfig
      false, // enableRibAllocatedPathId
      false, // enableOptimizedGR
      true // enablePolicyDefaultAction
  );
}

bgp_policy::BgpPolicyAction createPolicySetAsPathPrependAction(
    const uint32_t asn,
    const uint32_t repeated_times) {
  bgp_policy::BgpPolicyAction action;
  action.type() = BgpPolicyActionType::AS_PATH_PREPEND;

  bgp_policy::SetAsPathPrepend setAsPathPrepend;
  setAsPathPrepend.asn() = asn;
  setAsPathPrepend.repeat_times() = repeated_times;
  action.set_as_path_prepend() = setAsPathPrepend;
  return action;
}

std::shared_ptr<PolicyManager> setup3TermPolicy(const std::string& policyName) {
  // Create a policy with three terms
  // Term1 match kV4Prefix1, kV4Prefix2 and apply origin action (EGP) & as path
  // overwrite action as_path_overwrite_list set to {0, 0}, AdjRib will override
  // 0 asns based on ingress or egress routes;
  // Term2 match kV4Prefix3 and discard
  // Term3 match kV4Prefix4 and PERMIT (do not modify any attributes)

  // Creating TERM1 (match kV4Prefix1, kV4Prefix2 and apply origin action (EGP))
  routing_policy::CompareNumericValue compareStructEQ;
  compareStructEQ.compare_operator() = routing_policy::ComparisonOperator::EQ;
  compareStructEQ.value() = kV4Prefix1.second;
  const auto& prefixListEntry1 = createPrefixListEntry(
      IPAddress::networkToString(kV4Prefix1), {compareStructEQ});

  compareStructEQ.compare_operator() = routing_policy::ComparisonOperator::EQ;
  compareStructEQ.value() = kV4Prefix2.second;
  const auto& prefixListEntry2 = createPrefixListEntry(
      IPAddress::networkToString(kV4Prefix2), {compareStructEQ});
  const auto& match1 =
      createPrefixListMatch({prefixListEntry1, prefixListEntry2});
  auto actionEgp = createBgpPolicyAction(
      BgpPolicyActionType::ORIGIN, {}, "", bgp_policy::Origin::EGP);
  auto actionAsOverwrite = createBgpPolicyAction(BgpPolicyActionType::AS_PATH);
  actionAsOverwrite.as_path_overwrite_list() = {0, 0};
  auto term1 = createBgpPolicyTerm(
      "Term1", "", {match1}, {actionEgp, actionAsOverwrite});

  // Creating TERM2 (match kV4Prefix3 and DENY)
  compareStructEQ.compare_operator() = routing_policy::ComparisonOperator::EQ;
  compareStructEQ.value() = kV4Prefix3.second;
  const auto& prefixListEntry3 = createPrefixListEntry(
      IPAddress::networkToString(kV4Prefix3), {compareStructEQ});
  const auto& match2 = createPrefixListMatch({prefixListEntry3});
  auto actionDeny = createBgpPolicyAction(BgpPolicyActionType::DENY);
  auto term2 = createBgpPolicyTerm("Term2", "", {match2}, {actionDeny});

  // Creating TERM3 (match kV4Prefix4 and PERMIT)
  compareStructEQ.compare_operator() = routing_policy::ComparisonOperator::EQ;
  compareStructEQ.value() = kV4Prefix4.second;
  const auto& prefixListEntry4 = createPrefixListEntry(
      IPAddress::networkToString(kV4Prefix4), {compareStructEQ});
  const auto& match3 = createPrefixListMatch({prefixListEntry4});
  auto term3 = createBgpPolicyTerm("Term3", "", {match3}, {});

  // Create policy
  const auto& policyConfig =
      createBgpPolicies(policyName, {term1, term2, term3});
  auto policyManager = std::make_shared<PolicyManager>(
      policyConfig, createTestBgpGlobalConfig());

  return policyManager;
}

std::shared_ptr<PolicyManager> setupDenyIgpOriginAcceptAllPolicy(
    const std::string& policyName) {
  // Create a policy with two terms
  // Term1 match origin IGP and deny
  // Term2 permit all

  // Creating TERM1 (match origin IGP and DENY)
  const auto& match1 = createOriginMatch(bgp_policy::Origin::IGP);
  auto actionDeny = createBgpPolicyAction(BgpPolicyActionType::DENY);
  auto term1 = createBgpPolicyTerm("Term1", "", {match1}, {actionDeny});

  // Creating TERM2 (match all and PERMIT)
  auto term2 = createBgpPolicyTerm("Term2", "", {}, {});

  // Create policy
  const auto& policyConfig =
      createBgpPolicies(policyName, {std::move(term1), std::move(term2)});
  auto policyManager = std::make_shared<PolicyManager>(
      policyConfig, createTestBgpGlobalConfig());

  return policyManager;
}

std::shared_ptr<PolicyManager> setupAcceptAllPolicy(
    const std::string& policyName) {
  // Create a policy with one term
  // Term1 permits all

  // Creating TERM1 (match all and PERMIT)
  auto term1 = createBgpPolicyTerm("Term1", "", {}, {});

  // Create policy
  const auto& policyConfig = createBgpPolicies(policyName, {std::move(term1)});
  auto policyManager = std::make_shared<PolicyManager>(
      policyConfig, createTestBgpGlobalConfig());

  return policyManager;
}

std::shared_ptr<PolicyManager> setupMatchAllSetOriginIgpPolicy(
    const std::string& policyName) {
  // Create a policy with one term
  // Term1 match all, set action origin IGP

  // Creating TERM1 (match all set action origin IGP)
  auto actionIgp = createBgpPolicyAction(
      BgpPolicyActionType::ORIGIN, {}, "", bgp_policy::Origin::IGP);
  auto term1 = createBgpPolicyTerm("Term1", "", {}, {actionIgp});

  // Create policy
  const auto& policyConfig = createBgpPolicies(policyName, {term1});
  auto policyManager = std::make_shared<PolicyManager>(
      policyConfig, createTestBgpGlobalConfig());

  return policyManager;
}

std::shared_ptr<PolicyManager> setupMatchAllSetCommunityPolicy(
    const std::string& policyName) {
  // Create a policy with one term
  // Term1 match all, set action community list

  // Creating TERM1 (match all set action to add community)
  auto communityAction = createBgpPolicyCommunityAction(
      bgp_policy::CommunityActionType::SET, {kCommunity1});

  // Create policy
  auto policyManager = std::make_shared<PolicyManager>(
      createBgpPolicies(policyName, {}, {std::move(communityAction)}),
      createTestBgpGlobalConfig());

  return policyManager;
}

std::shared_ptr<PolicyManager> setupMatchAllSetMedPolicy(
    const std::string& policyName) {
  // Create a policy with one term
  // Term1 match all, set action med

  // Creating TERM1 (match all set action to add community)
  auto actionSetMed = createBgpPolicyMedAction(kMed);

  // Create policy
  auto policyManager = std::make_shared<PolicyManager>(
      createBgpPolicies(policyName, {}, {std::move(actionSetMed)}),
      createTestBgpGlobalConfig());

  return policyManager;
}

std::shared_ptr<PolicyManager> setupMatchAllSetWeightPolicy(
    const std::string& policyName) {
  // Create a policy with one term
  // Term1 match all, set action weight

  // Creating TERM1 (match all set action to add weight)
  auto actionSetWeight = createBgpPolicyWeightAction(kWeight);

  // Create policy
  auto policyManager = std::make_shared<PolicyManager>(
      createBgpPolicies(policyName, {}, {std::move(actionSetWeight)}),
      createTestBgpGlobalConfig());

  return policyManager;
}

std::shared_ptr<PolicyManager> setupMatchEgpOriginSetCommunityPolicy(
    const std::string& policyName) {
  // Create a policy with one term
  // Term1 match origin, set action community list
  auto originMatch = createOriginMatch(bgp_policy::Origin::EGP);

  // Creating TERM1 (match EGP origin action to add community)
  auto communityAction = createBgpPolicyCommunityAction(
      bgp_policy::CommunityActionType::SET, {kCommunity1});

  // Create policy
  auto policyManager = std::make_shared<PolicyManager>(
      createBgpPolicies(
          policyName, {std::move(originMatch)}, {std::move(communityAction)}),
      createTestBgpGlobalConfig());
  return policyManager;
}

std::shared_ptr<PolicyManager> setupMatchEgpOriginSetMedAcceptAllPolicy(
    const std::string& policyName) {
  // Create policy with term1 (match EGP set Med) and term2 (match all PERMIT).
  auto originMatch = createOriginMatch(bgp_policy::Origin::EGP);
  auto actionSetMed = createBgpPolicyMedAction(kMed);

  // Creating TERM1 (match EGP origin action to add community)
  auto term1 = createBgpPolicyTerm(
      "Term1", "", {std::move(originMatch)}, {std::move(actionSetMed)});
  // Creating TERM2 (match all and PERMIT)
  auto term2 = createBgpPolicyTerm("Term2", "", {}, {});

  // Create policy
  auto policyManager = std::make_shared<PolicyManager>(
      createBgpPolicies(policyName, {std::move(term1), std::move(term2)}),
      createTestBgpGlobalConfig());
  return policyManager;
}

std::shared_ptr<PolicyManager> setupSimpleTwoPolicyManager(
    const std::string& policyName1,
    const std::string& policyName2) {
  // Creating match all and PERMIT
  auto term = createBgpPolicyTerm("Term1", "", {}, {});

  // Create two policies that both are match all PERMIT.
  auto policy1 = createBgpPolicyStatement(policyName1);
  policy1.policy_entries() = {term};

  auto policy2 = createBgpPolicyStatement(policyName2);
  policy2.policy_entries() = {std::move(term)};

  bgp_policy::BgpPolicies bgpPolicy;
  bgpPolicy.bgp_policy_statements() = {std::move(policy1), std::move(policy2)};

  return std::make_shared<PolicyManager>(
      bgpPolicy, createTestBgpGlobalConfig());
}

std::shared_ptr<PolicyManager> setupPolicyManagerWithMultiplePolicies(
    const std::vector<std::string>& policyNames) {
  // Create accept-all term that can be reused
  auto baseTerm = createBgpPolicyTerm("Term1", "", {}, {});

  bgp_policy::BgpPolicies bgpPolicies;

  for (const auto& policyName : policyNames) {
    auto policy = createBgpPolicyStatement(policyName);
    policy.policy_entries() = {baseTerm}; // All policies accept all
    bgpPolicies.bgp_policy_statements()->emplace_back(std::move(policy));
  }

  return std::make_shared<PolicyManager>(
      bgpPolicies, createTestBgpGlobalConfig());
}

} // namespace facebook::bgp
