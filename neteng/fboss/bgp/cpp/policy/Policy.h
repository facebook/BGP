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
#include "neteng/fboss/bgp/cpp/policy/PolicyAttributesMask.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyTerm.h"
#include "neteng/fboss/bgp/cpp/policy/base/PolicyBase.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook::bgp {

class Policy : public routing::PolicyBase<
                   BgpPath,
                   std::shared_ptr<BgpPolicyActionData>,
                   BgpPolicyMatchData> {
 public:
  explicit Policy(
      const bgp_policy::BgpPolicyStatement& policy,
      const BgpGlobalConfig* config = nullptr)
      : PolicyBase<
            BgpPath,
            std::shared_ptr<BgpPolicyActionData>,
            BgpPolicyMatchData>(*policy.name()),
        policy_(policy) {
    populatePolicyTerms(config);
    populateDefaultAction(config);
    validatePolicyTerms();
  }
  // getter
  const bgp_policy::BgpPolicyStatement& getPolicyStatement() const {
    return policy_;
  }

  const PolicyAttributesMask& getPolicyAttributesMask() const {
    return mask_;
  }

 private:
  // populate internal policy database from the BgpPolicyStatement thrift
  void populatePolicyTerms(const BgpGlobalConfig* config);
  // populate default action from BgpPolicyStatement::result
  void populateDefaultAction(const BgpGlobalConfig* config);
  // override base class validation to add BGP-specific checks
  void validatePolicyTerms();
  // BgpPolicyStatement thrift
  const bgp_policy::BgpPolicyStatement policy_;

  /*
   * Defines the attributes in BgpPath that are relevant given the
   * input policy config from @policy_.
   */
  PolicyAttributesMask mask_;

#ifdef Policy_TEST_FRIENDS
  Policy_TEST_FRIENDS
#endif
};
} // namespace facebook::bgp
