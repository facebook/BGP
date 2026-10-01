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

#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"

#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"

namespace facebook::bgp {

using namespace neteng::fboss::bgp_attr;
using namespace rib_policy;
using namespace folly;

TRouteAttributeStatement createTRouteAttributeStatementLbw(
    const std::vector<CIDRNetwork>& prefixes,
    const int64_t& lbwVal,
    const std::optional<int>& timestamp) {
  TRouteAttributeLbwAction tSetLbw;
  tSetLbw.lbw() = lbwVal;

  TRouteAttributeActions tActions;
  tActions.set_lbw() = std::move(tSetLbw);

  TRouteAttributeStatement tStmt;
  tStmt.matcher() = createTRibRouteMatcher(prefixes);
  tStmt.actions() = std::move(tActions);
  if (timestamp) {
    tStmt.expiration_time_s() = *timestamp;
  }
  return tStmt;
}

TRouteAttributePolicy createTRouteAttributePolicyLbw(
    const std::vector<CIDRNetwork>& prefixes,
    const int64_t& lbwVal,
    const std::string& statementName,
    const std::optional<int>& timestamp) {
  TRouteAttributePolicy tSubPolicy;
  tSubPolicy.statements()->emplace(
      statementName,
      createTRouteAttributeStatementLbw(prefixes, lbwVal, timestamp));
  return tSubPolicy;
}

TRibPolicy createTRibPolicyLbw(
    const std::vector<CIDRNetwork>& prefixes,
    const std::optional<int64_t>& lbwVal,
    const std::string& statementName,
    const std::optional<int>& timestamp) {
  TRibPolicy tPolicy;

  if (lbwVal) {
    tPolicy.route_attribute_policy() = createTRouteAttributePolicyLbw(
        prefixes, *lbwVal, statementName, timestamp);
  }

  return tPolicy;
}

TRouteAttributeStatement createTRouteAttributeStatementUcmp(
    const std::vector<CIDRNetwork>& prefixes,
    TRouteAttributeUcmpAction& tRouteAttributeUcmpAction,
    const std::optional<int>& timestamp) {
  TRouteAttributeActions tActions;
  tActions.set_ucmp_weights() = std::move(tRouteAttributeUcmpAction);

  TRouteAttributeStatement tStmt;
  tStmt.matcher() = createTRibRouteMatcher(prefixes);
  tStmt.actions() = std::move(tActions);
  if (timestamp) {
    tStmt.expiration_time_s() = *timestamp;
  }
  return tStmt;
}

TRouteAttributePolicy createTRouteAttributePolicyUcmp(
    const std::vector<CIDRNetwork>& prefixes,
    TRouteAttributeUcmpAction& tRouteAttributeUcmpAction,
    const std::string& statementName,
    const std::optional<int>& timestamp) {
  TRouteAttributePolicy tSubPolicy;
  tSubPolicy.statements()->emplace(
      statementName,

      createTRouteAttributeStatementUcmp(
          prefixes, tRouteAttributeUcmpAction, timestamp));
  return tSubPolicy;
}

TRibPolicy createTRibPolicyUcmp(
    const std::vector<CIDRNetwork>& prefixes,
    TRouteAttributeUcmpAction& tRouteAttributeUcmpAction,
    const std::string& statementName,
    const std::optional<int>& timestamp) {
  TRibPolicy tPolicy;

  tPolicy.route_attribute_policy() = createTRouteAttributePolicyUcmp(
      prefixes, tRouteAttributeUcmpAction, statementName, timestamp);

  return tPolicy;
}

TPathSelectionStatement createTPathSelectionStatementWithPathSelector(
    const std::vector<folly::CIDRNetwork>& prefixes,
    const rib_policy::TPathSelector& tPathSelector) {
  TPathSelectionStatement tStmt;
  tStmt.matcher() = createTRibRouteMatcher(prefixes);
  tStmt.multi_path_selector() = tPathSelector;
  return tStmt;
}

TPathSelectionPolicy createTPathSelectionPolicyWithPathSelector(
    const std::vector<CIDRNetwork>& prefixes,
    const TPathSelector& tPathSelector) {
  TPathSelectionPolicy tSubPolicy;
  tSubPolicy.version() = 1;
  tSubPolicy.statements()->emplace(
      "stmt1",
      createTPathSelectionStatementWithPathSelector(prefixes, tPathSelector));
  return tSubPolicy;
}

TRibPolicy createTRibPolicyWithPathSelector(
    const std::vector<CIDRNetwork>& prefixes,
    const TPathSelector& tPathSelector) {
  TRibPolicy tPolicy;

  tPolicy.path_selection_policy() =
      createTPathSelectionPolicyWithPathSelector(prefixes, tPathSelector);

  return tPolicy;
}

TPathSelector createTPathSlectorWithOneMatcher(
    const TBgpPathMatcher& matcher,
    std::optional<int32_t> criteriaMinNextHop,
    std::optional<int32_t> defaultMinNextHop,
    std::optional<int64_t> defaultMinAggLbwbps,
    std::optional<bool> relaxMinAggLbwbps) {
  TPathSelectionCriteria tCriteria;
  tCriteria.path_matchers() = {matcher};
  if (criteriaMinNextHop) {
    tCriteria.min_nexthop() = *criteriaMinNextHop;
  }

  TPathSelector tPathSelector;
  tPathSelector.criteria_list() = {tCriteria};
  if (defaultMinNextHop) {
    tPathSelector.bgp_native_path_selection_min_nexthop() = *defaultMinNextHop;
  }
  if (defaultMinAggLbwbps) {
    tPathSelector.bgp_min_aggregate_lbw_bps() = *defaultMinAggLbwbps;
  }
  if (relaxMinAggLbwbps) {
    tPathSelector.relax_bgp_min_aggregate_lbw_bps() = *relaxMinAggLbwbps;
  }
  return tPathSelector;
}

TBgpCommunityMatch createTBgpCommunityMatch(
    const int32_t& asn,
    const int32_t& value,
    const routing_policy::MatchValueLogicOperator& /* matchType */) {
  TBgpCommunity tCommunity;
  tCommunity.asn() = asn;
  tCommunity.value() = value;

  TBgpCommunityMatch tCommMatch;
  tCommMatch.match_type() = routing_policy::MatchValueLogicOperator::EQUAL;
  tCommMatch.community() = std::move(tCommunity);
  return tCommMatch;
}

TCommunityListMatch createTCommunityListMatch(
    const std::vector<TBgpCommunityMatch>& communities,
    const routing_policy::BooleanOperator& booleanOperator) {
  TCommunityListMatch tCommunityListMatch;
  tCommunityListMatch.communities() = communities;
  tCommunityListMatch.boolean_operator() = booleanOperator;

  return tCommunityListMatch;
}

TRibRouteMatcher createTRibRouteMatcher(
    const std::vector<CIDRNetwork>& prefixes,
    const std::optional<rib_policy::TCommunityListMatch>& communityList) {
  TRibRouteMatcher tMatcher;
  if (!prefixes.empty()) {
    tMatcher.prefixes() = {};
    for (const auto& prefix : prefixes) {
      tMatcher.prefixes()->push_back(createTIpPrefix(prefix));
    }
  }
  if (communityList) {
    tMatcher.community_list() = *communityList;
  }
  return tMatcher;
}

TBgpPathMatcher
createCommunityMatch(int32_t asn, int32_t value, bgp_policy::Origin origin) {
  TBgpPathMatcher tMatcher;
  tMatcher.community_list() = createTCommunityListMatch(
      {createTBgpCommunityMatch(asn, value)},
      routing_policy::BooleanOperator::AND);
  tMatcher.origin() = origin;

  return tMatcher;
}

TRouteFilter createTRouteFilter(
    const std::vector<CIDRNetwork>& prefixes,
    bool permissive) {
  TRouteFilter tFilter;
  std::vector<routing_policy::PrefixListEntry> prefixListEntries;
  for (const auto& prefix : prefixes) {
    routing_policy::CompareNumericValue compareStructGE;
    compareStructGE.compare_operator() = routing_policy::ComparisonOperator::GE;
    compareStructGE.value() = prefix.second;

    routing_policy::CompareNumericValue compareStructLE;
    compareStructLE.compare_operator() = routing_policy::ComparisonOperator::LE;
    compareStructLE.value() = prefix.second;
    prefixListEntries.emplace_back(createPrefixListEntry(
        folly::IPAddress::networkToString(prefix),
        {compareStructLE, compareStructGE}));
  }
  tFilter.prefix_list() = createPrefixList(prefixListEntries);
  if (permissive) {
    tFilter.permissive_mode() = permissive;
  }
  return tFilter;
}

TRouteFilterStatement createTRouteFilterStatement(
    const std::vector<CIDRNetwork>& prefixes,
    bool permissive,
    bool egress) {
  TRouteFilterStatement tStmt;
  TRouteFilter tFilter = createTRouteFilter(prefixes, permissive);
  if (egress) {
    tStmt.egress_filter() = tFilter;
  } else {
    tStmt.ingress_filter() = tFilter;
  }
  return tStmt;
}

TRouteFilterStatement createTRouteFilterStatementWithIngressAndEgressFilters(
    const std::vector<CIDRNetwork>& ingressPrefixes,
    const std::vector<CIDRNetwork>& egressPrefixes,
    bool ingressPermissive,
    bool egressPermissive,
    std::optional<facebook::bgp::routing_policy::IPVersion> ingressIPVersion,
    std::optional<facebook::bgp::routing_policy::IPVersion> egressIPVersion) {
  TRouteFilterStatement tStmt;

  if (!ingressPrefixes.empty()) {
    rib_policy::TRouteFilter ingressFilter =
        createTRouteFilter(ingressPrefixes, ingressPermissive);

    // If IP version is specified for ingress, add it
    if (ingressIPVersion.has_value()) {
      facebook::bgp::routing_policy::PrefixList prefixList =
          ingressFilter.prefix_list().value();
      prefixList.ip_version() = ingressIPVersion.value();
      ingressFilter.prefix_list() = std::move(prefixList);
    }

    tStmt.ingress_filter() = std::move(ingressFilter);
  }

  if (!egressPrefixes.empty()) {
    rib_policy::TRouteFilter egressFilter =
        createTRouteFilter(egressPrefixes, egressPermissive);

    // If IP version is specified for egress, add it
    if (egressIPVersion.has_value()) {
      facebook::bgp::routing_policy::PrefixList prefixList =
          egressFilter.prefix_list().value();
      prefixList.ip_version() = egressIPVersion.value();
      egressFilter.prefix_list() = std::move(prefixList);
    }

    tStmt.egress_filter() = std::move(egressFilter);
  }

  return tStmt;
}

rib_policy::TGoldenPrefixPolicy createTGoldenPrefixPolicy(
    const std::vector<std::pair<folly::CIDRNetwork, std::vector<int>>>&
        prefixesAndMaskLengths,
    std::optional<int> maxSubnets,
    const std::optional<std::set<std::string>>& communities) {
  TGoldenPrefixPolicy tPolicy;
  std::vector<routing_policy::PrefixListEntry> prefixListEntries;
  for (const auto& [prefix, maskLens] : prefixesAndMaskLengths) {
    // Create a prefix list entry for each allowed mask length.
    for (int maskLen : maskLens) {
      routing_policy::CompareNumericValue compareStruct;
      compareStruct.compare_operator() = routing_policy::ComparisonOperator::EQ;
      compareStruct.value() = maskLen;
      prefixListEntries.emplace_back(createPrefixListEntry(
          folly::IPAddress::networkToString(prefix),
          {compareStruct},
          maxSubnets,
          communities));
    }
  }
  tPolicy.allowed_prefixes() = createPrefixList(prefixListEntries);
  return tPolicy;
}

TGoldenPrefixPolicy createTGoldenPrefixPolicy(
    const std::vector<CIDRNetwork>& prefixes,
    std::optional<int> maxSubnets,
    const std::vector<int>& allowedMaskLengths,
    const std::optional<std::set<std::string>>& communities) {
  TGoldenPrefixPolicy tPolicy;
  std::vector<routing_policy::PrefixListEntry> prefixListEntries;
  for (const auto& prefix : prefixes) {
    // Create a prefix list entry for each allowed mask length.
    for (int maskLen : allowedMaskLengths) {
      routing_policy::CompareNumericValue compareStruct;
      compareStruct.compare_operator() = routing_policy::ComparisonOperator::EQ;
      compareStruct.value() = maskLen;
      prefixListEntries.emplace_back(createPrefixListEntry(
          folly::IPAddress::networkToString(prefix),
          {compareStruct},
          maxSubnets,
          communities));
    }
  }
  tPolicy.allowed_prefixes() = createPrefixList(prefixListEntries);
  return tPolicy;
}

TRouteFilterPolicy createTRouteFilterPolicy(
    const std::vector<TRouteFilterStatement>& statements,
    int64_t version) {
  TRouteFilterPolicy tPolicy;
  for (int i = 0; i < statements.size(); ++i) {
    tPolicy.statements()->emplace(fmt::format("stmt{}", i), statements[i]);
  }
  tPolicy.version() = version;
  return tPolicy;
}

TRouteFilterPolicy createTRouteFilterPolicyWithKeyType(
    const std::vector<TRouteFilterStatement>& statements,
    int64_t version,
    KeyType keyType) {
  TRouteFilterPolicy tPolicy;
  for (int i = 0; i < statements.size(); ++i) {
    tPolicy.statements()->emplace(fmt::format("stmt{}", i), statements[i]);
  }
  tPolicy.version() = version;
  tPolicy.key_type() = keyType;
  return tPolicy;
}

} // namespace facebook::bgp
