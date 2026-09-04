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
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/policy/Policy.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyStructs.h"
#include "neteng/fboss/bgp/cpp/policy/base/PolicyManagerBase.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"
#include "neteng/fboss/bgp/if/gen-cpp2/policy_thrift_types.h"

namespace facebook::bgp {

class PolicyManager : public routing::PolicyManagerBase<
                          BgpPath,
                          std::shared_ptr<BgpPolicyActionData>,
                          BgpPolicyMatchData> {
 public:
  explicit PolicyManager(
      const std::string& policyFileName,
      const BgpGlobalConfig* config = nullptr)
      : PolicyManagerBase<
            BgpPath,
            std::shared_ptr<BgpPolicyActionData>,
            BgpPolicyMatchData>() {
    setPolicyFromFile(policyFileName);
    populatePolicyDatabase(config);
    populateReferences();
  }

  explicit PolicyManager(
      bgp_policy::BgpPolicies policy,
      const BgpGlobalConfig* config = nullptr)
      : PolicyManagerBase<
            BgpPath,
            std::shared_ptr<BgpPolicyActionData>,
            BgpPolicyMatchData>(),
        policy_(policy) {
    populatePolicyDatabase(config);
    populateReferences();
  }

  // get policy thrift serialized object
  const bgp_policy::BgpPolicies& getPolicy() const {
    return policy_;
  }

  const PolicyAttributesMask* getPolicyAttributesMask(
      const std::string& policyName);

 private:
  // populate internal policy database from the BgpPolicyStatement thrift
  void populatePolicyDatabase(const BgpGlobalConfig* config);
  void setPolicyFromFile(const std::string& policyFile);
  // populate reference with actual inline definition
  void populateReferences();
  void populateAttrsReferences();

  // BgpPolicyStatement thrift
  bgp_policy::BgpPolicies policy_;

  // map for references
  folly::F14NodeMap<std::string, bgp_policy::CommunityList> communityListMap_;
  folly::F14NodeMap<std::string, bgp_policy::AsPathList> asPathListMap_;
  folly::F14NodeMap<std::string, routing_policy::PrefixList> prefixListMap_;
  /*
   * This is stored separately on bgp++ policy manager because underlying
   * @policyMap_'s value type does not expose the mask.
   */
  folly::F14NodeMap<std::string, const PolicyAttributesMask>
      policyNameToAttrsMask_;
};
} // namespace facebook::bgp
