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

#include <folly/String.h>
#include <folly/gen/Base.h>
#include <folly/logging/xlog.h>
#include "magic_enum/magic_enum.hpp"

#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyTerm.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "thrift/lib/cpp/util/EnumUtils.h"

using facebook::bgp::bgp_policy::BgpPolicyActionType;
using facebook::bgp::bgp_policy::BgpPolicyAtomicMatchType;

namespace facebook::bgp {

// Determine if a particular match type is per prefix
std::set<BgpPolicyAtomicMatchType> prefixMatchTypes{
    BgpPolicyAtomicMatchType::PREFIX_LIST};

// define helper function for createPolicyMatchItem/createPolicyActionItem
std::shared_ptr<AttributesMatch> createPolicyAttributeMatchItem(
    const bgp_policy::BgpPolicyAtomicMatch& matchAtom,
    PolicyAttributesMask& mask) {
  switch (*matchAtom.type()) {
    case BgpPolicyAtomicMatchType::COMMUNITY_LIST:
      if (matchAtom.communities_filter()) {
        mask.communities = true;
        return std::make_shared<CommunityMatch>(matchAtom);
      }
      break;
    case BgpPolicyAtomicMatchType::COMMUNITY_COUNT:
      if (matchAtom.community_count()) {
        mask.communities = true;
        return std::make_shared<CommunityCountMatch>(matchAtom);
      }
      break;
    case BgpPolicyAtomicMatchType::ORIGIN:
      if (matchAtom.origin()) {
        mask.origin = true;
        return std::make_shared<OriginMatch>(matchAtom);
      }
      break;
    case BgpPolicyAtomicMatchType::AS_PATH:
      if (matchAtom.as_path_filters()) {
        mask.asPath = true;
        return std::make_shared<AsPathMatch>(matchAtom);
      }
      break;
    case BgpPolicyAtomicMatchType::AS_PATH_LEN:
      if (matchAtom.as_path_len_filter() &&
          !matchAtom.as_path_len_filter()->empty()) {
        mask.asPath = true;
        return std::make_shared<AsPathLenMatch>(matchAtom);
      }
      break;
    case BgpPolicyAtomicMatchType::AS_PATH_LEN_WITH_CONFED:
      if (matchAtom.as_path_len_with_confed_filter() &&
          !matchAtom.as_path_len_with_confed_filter()->empty()) {
        mask.asPath = true;
        return std::make_shared<AsPathLenWithConfedMatch>(matchAtom);
      }
      break;
    case BgpPolicyAtomicMatchType::WEIGHT:
      if (matchAtom.weight()) {
        mask.weight = true;
        return std::make_shared<WeightMatch>(matchAtom);
      }
      break;
    case BgpPolicyAtomicMatchType::ALWAYS:
      return std::make_shared<AlwaysMatch>();
    default:
      XLOG(ERR, "Unsupported BgpPolicyAtomicMatchType");
      throw BgpError(
          "BgpPolicyAtomicMatch Config input error for type: ",
          *matchAtom.type());
  }

  // If user doesn't set the required optional fields
  throw BgpError(
      "BgpPolicyAtomicMatch Config input error for type: ", *matchAtom.type());
}

// define helper function for createPolicyMatchItem/createPolicyActionItem
std::shared_ptr<PrefixTreeMatch> createPolicyPrefixMatchItem(
    const bgp_policy::BgpPolicyAtomicMatch& matchAtom,
    PolicyAttributesMask& mask) {
  switch (*matchAtom.type()) {
    case BgpPolicyAtomicMatchType::PREFIX_LIST:
      if (matchAtom.prefix_filters()) {
        mask.prefix = true;
        return std::make_shared<PrefixTreeMatch>(matchAtom);
      }
      break;
    default:
      XLOG(ERR, "Unsupported BgpPolicyAtomicMatchType");
      throw BgpError(
          "BgpPolicyAtomicMatch Config input error for type: ",
          *matchAtom.type());
  }

  // If user doesn't set the required optional fields
  throw BgpError(
      "BgpPolicyAtomicMatch Config input error for type: ", *matchAtom.type());
}

std::shared_ptr<PolicyAttributesAction> createPolicyAttributesActionItem(
    const bgp_policy::BgpPolicyAction& action,
    PolicyAttributesMask& mask,
    const BgpGlobalConfig* config) {
  auto type = *action.type();
  switch (type) {
    case BgpPolicyActionType::AS_PATH_PREPEND:
      if (action.set_as_path_prepend()) {
        mask.asPath = true;
        return std::make_shared<SetAsPathPrependAction>(action);
      }
      break;
    case BgpPolicyActionType::COMMUNITY_LIST:
      if (action.community_action()) {
        mask.communities = true;
        return std::make_shared<CommunityAction>(action);
      }
      break;
    case BgpPolicyActionType::LBW_EXT_COMMUNITY:
      if (action.lbw_ext_community_action()) {
        mask.extCommunities = true;
        mask.customizedLbwEnabled = true;
        return std::make_shared<LbwExtCommunityAction>(action);
      }
      break;
    case BgpPolicyActionType::EXT_COMMUNITY_LIST: {
      if (action.ext_communities_action()) {
        mask.extCommunities = true;
        return std::make_shared<ExtCommunityAction>(action, config);
      }
      auto& err =
          "Expected ExtCommunityAction set on BgpPolicyAction for "
          "EXT_COMMUNITY_LIST action type.";
      XLOGF(ERR, "{}", err);
      throw BgpError(err);
    }
    case BgpPolicyActionType::SET_LOCAL_PREF:
      if (action.set_local_pref()) {
        mask.localPref = true;
        return std::make_shared<SetLocalPreference>(action);
      }
      break;
    case BgpPolicyActionType::ORIGIN:
      if (action.set_origin()) {
        mask.origin = true;
        return std::make_shared<SetOrigin>(action);
      }
      break;
    case BgpPolicyActionType::NEXT_HOP:
      if (action.set_nexthop()) {
        mask.nexthop = true;
        return std::make_shared<SetNexthop>(action);
      }
      break;
    case BgpPolicyActionType::AS_PATH:
      if (action.as_path_overwrite_list()) {
        mask.asPath = true;
        return std::make_shared<SetAsPath>(action);
      }
      break;
    case BgpPolicyActionType::MED:
      if (action.med_action()) {
        mask.med = true;
        return std::make_shared<SetMed>(action);
      }
      break;
    case BgpPolicyActionType::WEIGHT:
      if (action.weight_action()) {
        mask.weight = true;
        return std::make_shared<SetWeight>(action);
      }
      break;
    case BgpPolicyActionType::AS_PATH_TO_AS_SET:
      mask.asPath = true;
      return std::make_shared<AsPathToAsSet>();
    default:
      XLOGF(
          ERR,
          "Unsupported BgpPolicyActionType: {}",
          magic_enum::enum_name(type));
      throw BgpError("BgpPolicyAction Config input error for type: ", type);
  }
  // If user doesn't set required set_xxx for the type
  throw BgpError("BgpPolicyAction Config input error for type: ", type);
}

std::shared_ptr<routing::PolicyActionBase> createPolicyFlowControlActionItem(
    const bgp_policy::FlowControlAction& action) {
  switch (action) {
    case bgp_policy::FlowControlAction::NEXT_TERM:
      return std::make_shared<routing::ContinueAction>();
    case bgp_policy::FlowControlAction::DENY:
      return std::make_shared<routing::DenyAction>();
    case bgp_policy::FlowControlAction::LOG_AND_NEXT_TERM:
      return std::make_shared<routing::LogContinueAction>(true);
    case bgp_policy::FlowControlAction::LOG_AND_DENY:
      return std::make_shared<routing::DenyAction>(true);
    case bgp_policy::FlowControlAction::LOG_AND_ACCEPT:
      return std::make_shared<routing::AllowAction>(true);
    default:
      XLOG(ERR, "Unsupported FlowControlAction");
      throw BgpError(
          "FlowControlAction Config input error for type: ",
          apache::thrift::util::enumNameSafe(action));
  }
}

bool isFlowControlActionType(const bgp_policy::BgpPolicyAction& action) {
  switch (*action.type()) {
    case BgpPolicyActionType::GOTO:
    case BgpPolicyActionType::DENY:
    case BgpPolicyActionType::PERMIT:
    case BgpPolicyActionType::CONTINUE:
      return true;
    default:
      return false;
  }
}

std::shared_ptr<routing::PolicyActionBase> createPolicyFlowControlActionItem(
    const bgp_policy::BgpPolicyAction& action) {
  switch (*action.type()) {
    case BgpPolicyActionType::GOTO:
      if (action.next_term_id()) {
        return std::make_shared<routing::GotoAction>(*action.next_term_id());
      }
      break;
    case BgpPolicyActionType::DENY:
      return std::make_shared<routing::DenyAction>();
    case BgpPolicyActionType::PERMIT:
      return std::make_shared<routing::AllowAction>();
    case BgpPolicyActionType::CONTINUE:
      return std::make_shared<routing::ContinueAction>();
    default:
      XLOG(ERR, "Unsupported FlowControlAction");
      throw BgpError(
          "FlowControlAction Config input error for type: ",
          apache::thrift::util::enumNameSafe(*action.type()));
  }
  // If user doesn't set required set_xxx for the type
  throw BgpError(
      "BgpPolicyAction Config input error for type: ", *action.type());
}

PolicyTerm::PolicyTerm(
    const bgp_policy::BgpPolicyTerm& term,
    PolicyAttributesMask& mask,
    const BgpGlobalConfig* config)
    : PolicyTermBase(
          *term.name(),
          *term.description(),
          createPolicyFlowControlActionItem(*term.term_miss_action()),
          std::make_shared<routing::AllowAction>()) {
  if (term.policy_match_entries()) {
    // As of now, we only support match logic type AND
    if ((*term.policy_match_entries()->match_logic_type() !=
         routing_policy::BooleanOperator::AND) &&
        ((term.policy_match_entries())->match_entries()->size() > 1)) {
      throw BgpError(
          "Unsupported match_logic_type BooleanOperator. "
          "Only AND is supported");
    }

    // populate matches
    for (const auto& matchAtom :
         *(term.policy_match_entries())->match_entries()) {
      if (prefixMatchTypes.contains(*matchAtom.type())) {
        const auto& matchPtr = createPolicyPrefixMatchItem(matchAtom, mask);
        if (matchPtr) {
          prefixMatches_.emplace_back(matchPtr);
        }
      } else {
        const auto& matchPtr = createPolicyAttributeMatchItem(matchAtom, mask);
        if (matchPtr) {
          attributesMatches_.emplace_back(matchPtr);
        }
      }
    }
  }

  // populate actions
  int flowControlActionCnt{0};
  for (const auto& action : *term.policy_action_entries()) {
    if (isFlowControlActionType(action)) {
      termMatchFlowControlAction_ = createPolicyFlowControlActionItem(action);
      flowControlActionCnt++;
    } else {
      const auto& actionPtr =
          createPolicyAttributesActionItem(action, mask, config);
      actions_.emplace_back(actionPtr);
    }
  }
  // There should be only 1 FlowControlAction
  // 0 is fine as we default to permit.
  if (flowControlActionCnt > 1) {
    throw BgpError(
        "More than one FlowControlAction(PERMIT/DENY/GOTO) filled in term ",
        getTermName());
  }
}

void PolicyTerm::copyAttr(std::shared_ptr<BgpPath>& attr) {
  // clone attribute and modify attr ptr if attribute is published
  // else return old attributes
  if (attr->isPublished()) {
    attr = attr->clone();
  }
}

std::optional<std::string> PolicyTerm::hasInvalidAttrsPostAction(
    const std::shared_ptr<BgpPath>& /*attr*/,
    const routing::PolicyEntryBase<
        BgpPath,
        std::shared_ptr<BgpPolicyActionData>,
        BgpPolicyMatchData>& entry,
    const std::unordered_set<folly::CIDRNetwork>& prefixesSet,
    const std::string& policyName) const noexcept {
  auto actionData = entry.policyActionData();
  if (actionData.has_value() && *actionData && (*actionData)->isLbwRejected) {
    auto prefixStrs = folly::gen::from(prefixesSet) |
        folly::gen::map([](const auto& p) {
                        return folly::IPAddress::networkToString(p);
                      }) |
        folly::gen::as<std::vector<std::string>>();
    XLOGF(
        ERR,
        "Rejecting route(s) [{}] due to zero/missing GAR weights "
        "after policy action in policy {} term {}",
        folly::join(", ", prefixStrs),
        policyName,
        getTermName());
    PeerStats::incrEmptyGarWeightsRejects();
    return std::string(kInvalidGarWeightsDenyReason);
  }
  return std::nullopt;
}

} // namespace facebook::bgp
