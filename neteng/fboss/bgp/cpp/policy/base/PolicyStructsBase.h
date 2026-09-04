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

#include <folly/IPAddress.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "neteng/fboss/bgp/cpp/policy/base/PolicyActionBase.h"

namespace facebook {
namespace routing {

/*
 * Input message to policy
 * Take const reference of attributes to avoid accidental modification on pre
 * policy attributes.
 */
template <
    typename Attributes,
    typename PolicyActionData,
    typename PolicyMatchData>
struct PolicyInMessageBase {
  const std::vector<folly::CIDRNetwork>& prefixes{};
  const std::shared_ptr<Attributes>& attrs{nullptr};
  const std::optional<PolicyActionData> policyActionData{std::nullopt};
  const std::optional<PolicyMatchData> policyMatchData{std::nullopt};

  PolicyInMessageBase(
      const std::vector<folly::CIDRNetwork>& prefixes,
      const std::shared_ptr<Attributes>& attrs,
      const std::optional<PolicyActionData>& policyActionData = std::nullopt,
      const std::optional<PolicyMatchData>& policyMatchData = std::nullopt)
      : prefixes(prefixes),
        attrs(attrs),
        policyActionData(policyActionData),
        policyMatchData(policyMatchData) {}
};

// Attributes and Match Policy Term Name used in PolicyOutMessage
template <typename Attributes>
struct AttributesAndPolicy {
  std::shared_ptr<Attributes> attrs;
  const std::string policyName;

  AttributesAndPolicy(
      std::shared_ptr<Attributes> attrs,
      const std::string& policyName)
      : attrs(attrs), policyName(policyName) {}
};

/*
 * Output message from policy
 * Only accepted prefixes are returned with modified attributes.
 * Many prefixes can share same attributes.
 */
template <typename Attributes>
struct PolicyOutMessageBase {
  std::unordered_map<
      folly::CIDRNetwork,
      std::shared_ptr<AttributesAndPolicy<Attributes>>>
      result;
};

enum class PolicyComparisonOperator {
  EQ,
  GE,
  LE,
  NE,
  GT,
  LT,
};

} // namespace routing
} // namespace facebook
