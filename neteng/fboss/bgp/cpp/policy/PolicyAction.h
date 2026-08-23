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

#include <folly/logging/xlog.h>

#include <boost/regex.hpp>
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Types.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyStructs.h"
#include "neteng/fboss/bgp/cpp/policy/base/PolicyActionBase.h"

namespace facebook::bgp {

namespace {

inline constexpr auto kExtCommunityActionName = "ExtCommunityAction";

}

// PolicyAttributesAction

using PolicyAttributesAction = routing::
    PolicyAttributesActionBase<BgpPath, std::shared_ptr<BgpPolicyActionData>>;

class SetAsPathPrependAction : public PolicyAttributesAction {
 public:
  explicit SetAsPathPrependAction(
      const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(std::string("")),
        asPathPrepend_(*(policyAction.set_as_path_prepend())) {
    validateAsPathPrepend(policyAction);
  }
  virtual ~SetAsPathPrependAction() = default;
  const bgp_policy::SetAsPathPrepend& getAction() const {
    return asPathPrepend_;
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  void validateAsPathPrepend(const bgp_policy::BgpPolicyAction& policyAction);
  const bgp_policy::SetAsPathPrepend asPathPrepend_;
  ;
};

class CommunityAction : public PolicyAttributesAction {
 public:
  explicit CommunityAction(const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(*policyAction.community_action()->name()),
        communityAction_(*(policyAction.community_action())) {
    validateAndSetCommunities(
        policyAction.community_action()->communities()
            ? *(policyAction.community_action()->communities())
            : std::vector<std::string>{});
  }
  virtual ~CommunityAction() = default;
  const std::vector<nettools::bgplib::BgpAttrCommunityC>& getCommunities()
      const {
    return communities_;
  }
  const bgp_policy::CommunityAction& getAction() const {
    return communityAction_;
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;
  void PopulateReferences(
      const folly::F14NodeMap<std::string, bgp_policy::CommunityList>&
          communityListMap);

 private:
  void validateAndSetCommunities(
      const std::vector<std::string>& communitiesFromConfig);
  const bgp_policy::CommunityAction communityAction_;
  // Communities (non-regex)
  nettools::bgplib::BgpAttrCommunitiesC communities_;
  // Community regex (Applicable only for CommunityActionType::REMOVE)
  std::vector<boost::regex> communityRegexs_;
};

class LbwExtCommunityAction : public PolicyAttributesAction {
 public:
  explicit LbwExtCommunityAction(
      const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(""),
        lbwExtCommunityAction_(*(policyAction.lbw_ext_community_action())) {
    switch (*lbwExtCommunityAction_.type()) {
      case bgp_policy::LbwExtCommunityActionType::
          ENCODE_AGGREGATE_RECEIVED_OVERWRITE:
      case bgp_policy::LbwExtCommunityActionType::ENCODE_SWITCH_ID:
      case bgp_policy::LbwExtCommunityActionType::ENCODE_MULTIPATH:
        XCHECK(lbwExtCommunityAction_.encoding_scheme())
            << "encoding_scheme is not set";
        XCHECK(lbwExtCommunityAction_.encoding_id())
            << "encoding_id is not set";
        break;
      default:
        break;
    }
  }
  virtual ~LbwExtCommunityAction() override = default;
  const bgp_policy::LbwExtCommunityAction& getAction() const {
    return lbwExtCommunityAction_;
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  const bgp_policy::LbwExtCommunityAction lbwExtCommunityAction_;
};

class ExtCommunityAction : public PolicyAttributesAction {
 public:
  explicit ExtCommunityAction(
      const bgp_policy::BgpPolicyAction& policyAction,
      const BgpGlobalConfig* config = nullptr)
      : PolicyAttributesAction(kExtCommunityActionName),
        actionType_(*policyAction.action_type()->route_action()),
        ext_communities_(getExtCommunities(
            policyAction.ext_communities_action()->ext_communities().value_or(
                std::vector<bgp_policy::ExtCommunity>{}),
            config)) {
    validateActionType();
  }

  virtual ~ExtCommunityAction() override = default;

  void applyAction(
      std::shared_ptr<BgpPath>& path,
      std::optional<std::shared_ptr<BgpPolicyActionData>>)
      const noexcept override;

  const nettools::bgplib::BgpAttrExtCommunitiesC& getExtCommunitiesC() const {
    return ext_communities_;
  }

 private:
  /*
   * ExtCommunity action to apply on the list of ext communities.
   * Note, we must encode this additional @actionType_ information
   * because, unlike CommunityAction, ExtCommunityAction does not
   * store information about the action.
   */
  const bgp_policy::BgpAttrChangeActionType actionType_;
  // Communities
  const nettools::bgplib::BgpAttrExtCommunitiesC ext_communities_;

  void validateActionType() const;
  static nettools::bgplib::BgpAttrExtCommunitiesC getExtCommunities(
      const std::vector<bgp_policy::ExtCommunity>& ext_communities,
      const BgpGlobalConfig* config);

#ifdef ExtCommunityAction_TEST_FRIENDS
  ExtCommunityAction_TEST_FRIENDS
#endif
};

class SetLocalPreference : public PolicyAttributesAction {
 public:
  explicit SetLocalPreference(const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(*policyAction.set_local_pref()->name()),
        localPref_(*(policyAction.set_local_pref())) {
    ValidateLocalPreference();
  }
  virtual ~SetLocalPreference() = default;
  const bgp_policy::LocalPreference& getAction() const {
    return localPref_;
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  void ValidateLocalPreference() const;
  const bgp_policy::LocalPreference localPref_;
};

class SetOrigin : public PolicyAttributesAction {
 public:
  explicit SetOrigin(const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(""), origin_(*(policyAction.set_origin())) {}
  virtual ~SetOrigin() = default;
  const bgp_policy::Origin& getAction() const {
    return origin_;
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  const bgp_policy::Origin origin_;
};

class SetNexthop : public PolicyAttributesAction {
 public:
  explicit SetNexthop(const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(""),
        nexthop_(validateAndGetNexthop(policyAction)) {}
  virtual ~SetNexthop() = default;
  const folly::IPAddress& getNexthop() const {
    return nexthop_;
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  static folly::IPAddress validateAndGetNexthop(
      const bgp_policy::BgpPolicyAction& policyAction);
  const folly::IPAddress nexthop_;
};

class AddBackupAddr : public PolicyAttributesAction {
 public:
  explicit AddBackupAddr(const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(""),
        backupAddr_(validateAndGetBackupAddr(policyAction)) {}
  ~AddBackupAddr() override = default;
  const folly::IPAddress& getBackupAddr() const {
    return backupAddr_;
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  static folly::IPAddress validateAndGetBackupAddr(
      const bgp_policy::BgpPolicyAction& policyAction);
  folly::IPAddress backupAddr_;
};

class SetMed : public PolicyAttributesAction {
 public:
  explicit SetMed(const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(""), med_(*(policyAction.med_action())) {
    ValidateMed();
  }
  virtual ~SetMed() = default;
  uint32_t getMed() const {
    return *med_.med_value();
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  void ValidateMed() const;
  const bgp_policy::MedAction med_;
};

class SetWeight : public PolicyAttributesAction {
 public:
  explicit SetWeight(const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(""), weight_(*(policyAction.weight_action())) {
    ValidateWeight();
  }
  virtual ~SetWeight() = default;
  uint32_t getWeight() const {
    return *weight_.weight_value();
  }
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  void ValidateWeight() const;
  const bgp_policy::WeightAction weight_;
};

class SetAsPath : public PolicyAttributesAction {
 public:
  explicit SetAsPath(const bgp_policy::BgpPolicyAction& policyAction)
      : PolicyAttributesAction(""),
        overwriteList_(*(policyAction.as_path_overwrite_list())) {
    validateSetAsPath();
  }
  virtual ~SetAsPath() = default;
  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;

 private:
  void validateSetAsPath() const;
  const std::vector<int64_t> overwriteList_;
};

class AsPathToAsSet : public PolicyAttributesAction {
 public:
  explicit AsPathToAsSet() : PolicyAttributesAction("") {}

  virtual void applyAction(
      std::shared_ptr<BgpPath>& attr,
      std::optional<std::shared_ptr<BgpPolicyActionData>> policyActionData =
          std::nullopt) const noexcept override;
};
} // namespace facebook::bgp
