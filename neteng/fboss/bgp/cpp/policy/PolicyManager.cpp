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

#include <folly/FileUtil.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"

using facebook::routing::classType;
using std::string;

namespace facebook::bgp {

void PolicyManager::setPolicyFromFile(const string& policyFile) {
  string contents;
  if (!folly::readFile(policyFile.c_str(), contents)) {
    throw BgpError("Fail to read the BGP Policy file: ", policyFile);
  }

  auto jsonSerializer = apache::thrift::SimpleJSONSerializer();
  try {
    jsonSerializer.deserialize(contents, policy_);
  } catch (const std::exception& ex) {
    throw BgpError(
        "Could not parse BgpPolicyStatement struct: ", folly::exceptionStr(ex));
  }
}

void PolicyManager::populatePolicyDatabase(const BgpGlobalConfig* config) {
  for (const auto& policyStatement : *policy_.bgp_policy_statements()) {
    if (policyStatement.name()->empty()) {
      throw BgpError("PolicyStatement cannot have empty name");
    }
    auto policy = Policy(policyStatement, config);
    policyNameToAttrsMask_.insert(
        {*policyStatement.name(), policy.getPolicyAttributesMask()});
    auto ret = policyMap_.insert({*policyStatement.name(), policy});
    if (!ret.second) {
      throw BgpError(
          "Duplicate PolicyStatement name: ", *policyStatement.name());
    }
  }

  if (!policy_.community_lists()->empty()) {
    for (const auto& communityList : *policy_.community_lists()) {
      if (communityList.name()->empty()) {
        throw BgpError("CommunityList cannot have empty name");
      }
      if (communityList.community_list_names() &&
          !communityList.community_list_names()->empty()) {
        throw BgpError(
            "CommunityList recursive references not allowed: ",
            *communityList.name());
      }
      auto ret =
          communityListMap_.insert({*communityList.name(), communityList});
      if (!ret.second) {
        throw BgpError("Duplicate CommunityList name: ", *communityList.name());
      }
    }
  }

  if (!policy_.aspath_lists()->empty()) {
    for (const auto& asPathList : *policy_.aspath_lists()) {
      if (asPathList.name()->empty()) {
        throw BgpError("AsPathList cannot have empty name");
      }
      if (asPathList.as_path_list_names() &&
          !asPathList.as_path_list_names()->empty()) {
        throw BgpError(
            "AsPathList recursive references not allowed: ",
            *asPathList.name());
      }
      auto ret = asPathListMap_.insert({*asPathList.name(), asPathList});
      if (!ret.second) {
        throw BgpError("Duplicate AsPathList name: ", *asPathList.name());
      }
    }
  }

  if (!policy_.prefix_lists()->empty()) {
    for (const auto& prefixList : *policy_.prefix_lists()) {
      if (prefixList.name()->empty()) {
        throw BgpError("PrefixList cannot have empty name");
      }
      if (!prefixList.prefix_list_names()->empty()) {
        throw BgpError(
            "PrefixList recursive references not allowed: ",
            *prefixList.name());
      }
      auto ret = prefixListMap_.insert({*prefixList.name(), prefixList});
      if (!ret.second) {
        throw BgpError("Duplicate PrefixList name: ", *prefixList.name());
      }
    }
  }
}

void PolicyManager::populateAttrsReferences() {
  /*
   * find the reference name in Match attributes, replace the reference
   * name with the actual inline definitions in the global struct
   */
  for (const auto& iterPolicy : policyMap_) {
    auto& policy = iterPolicy.second;
    const auto& terms = policy.getPolicyTerms();
    for (const auto& term : terms) {
      // replace reference with actual struct in match sub classes
      for (auto& attrMatch : term->getPolicyAttributeMatches()) {
        auto type = attrMatch->getClassType();

        if (type == classType<CommunityMatch>()) {
          dynamic_cast<CommunityMatch&>(*attrMatch)
              .PopulateReferences(communityListMap_);
        } else if (type == classType<AsPathMatch>()) {
          dynamic_cast<AsPathMatch&>(*attrMatch)
              .PopulateReferences(asPathListMap_);
        }
      }
      for (auto& prefixMatch : term->getPolicyPrefixMatches()) {
        if (prefixMatch->getClassType() == classType<PrefixTreeMatch>()) {
          dynamic_cast<PrefixTreeMatch&>(*prefixMatch)
              .PopulateReferences(prefixListMap_);
        }
      }
      // populate reference used in actions
      for (auto& action : term->getPolicyActions()) {
        if (action->getClassType() == classType<CommunityAction>()) {
          dynamic_cast<CommunityAction&>(*action).PopulateReferences(
              communityListMap_);
        }
      }
    }
  }
} // namespace bgp

void PolicyManager::populateReferences() {
  /*
   * replace names with reference structs, flattening the rule mnemonics
   * find the reference pointer, replace the reference pointer with the
   * actual inline pointer for as path list, community list, prefix list etc
   */
  populateAttrsReferences();
}

const PolicyAttributesMask* FOLLY_NULLABLE
PolicyManager::getPolicyAttributesMask(const std::string& policyName) {
  auto it = policyNameToAttrsMask_.find(policyName);
  if (it == policyNameToAttrsMask_.end()) {
    return nullptr;
  }
  return &it->second;
}

} // namespace facebook::bgp
