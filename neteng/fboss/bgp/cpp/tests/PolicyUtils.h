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

#include <gmock/gmock.h>

#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/config/ConfigUtils.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

bgp_policy::BgpPolicyAtomicMatch createBgpPolicyAtomicMatch(
    const bgp_policy::BgpPolicyAtomicMatchType& type,
    const std::vector<std::string>& attr_strings = {},
    const routing_policy::BooleanOperator& boolean_op =
        routing_policy::BooleanOperator::OR,
    const std::string& structName = "",
    const std::string& attrsReferenceName = "");

bgp_policy::BgpPolicyAtomicMatch createCommunityListMatchWithReference(
    const std::vector<std::string>& attr_strings = {},
    const std::vector<std::string>& attrsReferenceNames = {});

bgp_policy::BgpPolicyAtomicMatch createAsPathListMatchWithReference(
    const std::vector<std::string>& attr_strings = {},
    const std::vector<std::string>& attrsReferenceNames = {});

bgp_policy::CommunityList createCommunityList(
    const std::vector<std::string>& attr_strings = {},
    const routing_policy::BooleanOperator& boolean_op =
        routing_policy::BooleanOperator::OR,
    const std::string& structName = "");

bgp_policy::AsPathList createAsPathList(
    const std::vector<std::string>& attr_strings = {},
    const routing_policy::BooleanOperator& boolean_op =
        routing_policy::BooleanOperator::OR,
    const std::string& structName = "");

bgp_policy::ExtCommunity createExtCommunity(
    const uint8_t typeHigh,
    const uint8_t typeLow,
    const std::string& value,
    const std::string& name = {},
    const std::string& description = {});

bgp_policy::BgpPolicyAction createBgpPolicyExtCommunityAction(
    const bgp_policy::BgpAttrChangeActionType routeAction,
    const std::vector<bgp_policy::ExtCommunity>& communities);

bgp_policy::BgpPolicyAtomicMatch createPrefixListMatch(
    const std::vector<routing_policy::PrefixListEntry>& prefixListEntries = {},
    const std::vector<std::string>& attrsReferenceNames = {});

routing_policy::PrefixList createPrefixList(
    const std::vector<routing_policy::PrefixListEntry>& prefixListEntries);

routing_policy::PrefixListEntry createPrefixListEntry(
    const std::string& basePrefix,
    const std::vector<routing_policy::CompareNumericValue>& lenRange,
    std::optional<int> maxSubnets = std::nullopt,
    const std::optional<std::set<std::string>>& communities = std::nullopt);

routing_policy::PrefixListEntry createPrefixListEntry(
    const std::string& basePrefix,
    const std::string& regex);

routing_policy::PrefixListEntry createDefaultPrefixListEntry();

bgp_policy::BgpPolicyAtomicMatch createOriginMatch(
    const bgp_policy::Origin& origin);

bgp_policy::BgpPolicyAction createBgpPolicyAction(
    const bgp_policy::BgpPolicyActionType& type,
    const std::vector<std::string>& communities = {},
    const std::string& structName = "",
    const bgp_policy::Origin& origin = bgp_policy::Origin::IGP);

bgp_policy::BgpPolicyAction createBgpPolicyGotoAction(
    const std::string& gotoTerm);

bgp_policy::BgpPolicyAction createBgpPolicyNexthopAction(
    const folly::IPAddress& nexthop = kV4Nexthop1);

bgp_policy::BgpPolicyAction createBgpPolicyMedAction(const uint32_t med = kMed);
bgp_policy::BgpPolicyAction createBgpPolicyWeightAction(
    const uint16_t weight = kWeight);

bgp_policy::BgpPolicyAction createBgpPolicyAsPathOverwriteAction(
    const std::vector<int64_t>& asPathOverwriteList);

bgp_policy::BgpPolicyAction createActionSetLocalPreference(
    const uint32_t localPref = kLocalPref);

bgp_policy::BgpPolicyAction createBgpPolicyCommunityAction(
    const bgp_policy::CommunityActionType& type =
        bgp_policy::CommunityActionType::SET,
    const std::vector<std::string>& communities = {kCommunity1},
    const std::vector<std::string>& communityReferences = {});

bgp_policy::BgpPolicyAction createBgpPolicyLbwExtCommunityAction(
    const bgp_policy::LbwExtCommunityActionType& type,
    const std::optional<nsf_policy::NsfTeWeightEncoding>& encodingScheme =
        std::nullopt,
    const std::optional<int32_t> encodingId = std::nullopt);

bgp_policy::BgpPolicyTerm createBgpPolicyTerm(
    const std::string& name = "",
    const std::string& description = "",
    const std::vector<bgp_policy::BgpPolicyAtomicMatch>& matches = {},
    const std::vector<bgp_policy::BgpPolicyAction>& actions = {},
    const bgp_policy::FlowControlAction& termMissAction =
        bgp_policy::FlowControlAction::NEXT_TERM);

bgp_policy::BgpPolicyStatement createBgpPolicyStatement(
    const std::string& name);

bgp_policy::BgpPolicyStatement createBgpPolicyStatement(
    const std::string& name,
    const std::vector<bgp_policy::BgpPolicyTerm>& terms);

bgp_policy::BgpPolicies createBgpPolicies(
    const std::string& statementName,
    const std::vector<bgp_policy::BgpPolicyAtomicMatch>& matches,
    const std::vector<bgp_policy::BgpPolicyAction>& actions = {});

// Create a policy, taking vector of terms (each term must be fully constructed)
bgp_policy::BgpPolicies createBgpPolicies(
    const std::string& statementName,
    const std::vector<bgp_policy::BgpPolicyTerm>& terms);

bgp_policy::BgpPolicies createBgpPoliciesWithReferences(
    const std::string& statementName,
    const std::vector<bgp_policy::BgpPolicyAtomicMatch>& matches,
    const std::vector<bgp_policy::BgpPolicyAction>& actions = {},
    const std::vector<bgp_policy::CommunityList>& communityLists = {},
    const std::vector<bgp_policy::AsPathList>& asPathLists = {},
    const std::vector<routing_policy::PrefixList>& prefixLists = {});

std::shared_ptr<BgpPath> createBgpPath(
    const std::vector<std::string>& communities = {},
    const std::vector<nettools::bgplib::BgpAttrAsPathSegmentC>& aspaths = {},
    const std::shared_ptr<facebook::bgp::BgpPathFields>& attrFields = nullptr,
    const nettools::bgplib::BgpAttrOrigin& origin =
        nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_EGP);

std::vector<nettools::bgplib::BgpAttrAsPathSegmentC> createAsPathSequence(
    const std::vector<uint32_t>& asSeq);

std::vector<nettools::bgplib::BgpAttrAsPathSegmentC>
createAsPathSequenceWithConfedAs(
    const std::vector<uint32_t>& asSeq,
    const std::vector<uint32_t>& asConfedSeq);

std::vector<nettools::bgplib::BgpAttrAsPathSegmentC>
createAsPathSequenceWithConfedAsSeparate(
    const std::vector<uint32_t>& asSeq,
    const std::vector<uint32_t>& asConfedSeq);

std::optional<std::string> parseActionConfigGetError(
    const bgp_policy::BgpPolicyAction& action);

std::optional<std::string> parseMatchConfigGetError(
    const bgp_policy::BgpPolicyAtomicMatch& action);

// Create BgpPath with a specific origin value
std::shared_ptr<facebook::bgp::BgpPath> createBgpPathWithOrigin(
    const nettools::bgplib::BgpAttrOrigin& origin =
        nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_EGP);

std::shared_ptr<facebook::bgp::BgpPath> createBgpPathWithWeight(
    const uint16_t& weight = 0);

// Helper to create a minimal BgpGlobalConfig for testing
BgpGlobalConfig* createTestBgpGlobalConfig();

// Helper to create BgpGlobalConfig with specific ASN for testing
BgpGlobalConfig createConfigWithAsn(uint32_t asn);

// Helper to create BgpGlobalConfig with enablePolicyDefaultAction=true
BgpGlobalConfig createConfigWithPolicyDefaultAction();

bgp_policy::BgpPolicyAction createPolicySetAsPathPrependAction(
    const uint32_t asn = kAsn1,
    const uint32_t repeated_times = kRepeatedTimes1);

// There is no meaningful name I can give for this filter.
// So naming it based on number of terms.
// Create a policy with three terms
// Term1 match kV4Prefix1, kV4Prefix2 and apply origin action (EGP) & as path
// overwrite action as_path_overwrite_list set to {0, 0}, AdjRib will override 0
// asns based on ingress or egress routes;
// Term2 match kV4Prefix3 and discard
// Term3 match kV4Prefix4 and PERMIT (do not modify any attributes)
std::shared_ptr<PolicyManager> setup3TermPolicy(const std::string& policyName);

// Create a policy with two terms
// Term1 match origin IGP and deny
// Term2 permit all
std::shared_ptr<PolicyManager> setupDenyIgpOriginAcceptAllPolicy(
    const std::string& policyName);

// Create a policy with one term
// Term1 permits all
std::shared_ptr<PolicyManager> setupAcceptAllPolicy(
    const std::string& policyName);

// Create a policy with one term
// Term1 match all, set action origin IGP
std::shared_ptr<PolicyManager> setupMatchAllSetOriginIgpPolicy(
    const std::string& policyName);

// Create a policy with one term
// Term1 match all, set action to change community list
std::shared_ptr<PolicyManager> setupMatchAllSetCommunityPolicy(
    const std::string& policyName);

// Create a policy with one term
// Term1 match all, set action to set Med
std::shared_ptr<PolicyManager> setupMatchAllSetMedPolicy(
    const std::string& policyName);

// Create a policy with one term
// Term1 match origin EGP, set action to change community list
std::shared_ptr<PolicyManager> setupMatchEgpOriginSetCommunityPolicy(
    const std::string& policyName);

/*
 * Create a policy with two terms
 * Term1 match origin EGP, set Med to kMed.
 * Term2 accepts all.
 */
std::shared_ptr<PolicyManager> setupMatchEgpOriginSetMedAcceptAllPolicy(
    const std::string& policyName);

/*
 * Create a policy manager with two policies;
 * each policy is match any accept all.
 */
std::shared_ptr<PolicyManager> setupSimpleTwoPolicyManager(
    const std::string& policyName1 = kIngressPolicyName,
    const std::string& policyName2 = kEgressPolicyName);

/*
 * Create a policy manager with multiple policies;
 * each policy is match any accept all.
 */
std::shared_ptr<PolicyManager> setupPolicyManagerWithMultiplePolicies(
    const std::vector<std::string>& policyNames);

bool compareBgpAttr(
    const nettools::bgplib::BgpAttributes& actual,
    const facebook::bgp::BgpPath& expected);

extern std::array<bgp_policy::Origin, 3> policyOriginTypes;
extern std::array<nettools::bgplib::BgpAttrOrigin, 3> attrOriginTypes;

} // namespace facebook::bgp
