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
#include <string>
#include "neteng/fboss/bgp/cpp/policy/base/PolicyActionBase.h"
#include "neteng/fboss/bgp/cpp/policy/base/PolicyMatchBase.h"

namespace facebook {
namespace routing {

template <typename Attributes, typename PolicyMatchData>
class PolicyEvaluationLoggerBase {
 public:
  using PolicyPrefixMatch =
      PolicyMatchBase<folly::CIDRNetwork, PolicyMatchData>;

  PolicyEvaluationLoggerBase() = default;
  virtual ~PolicyEvaluationLoggerBase() = default;

  virtual bool log(
      [[maybe_unused]] const folly::CIDRNetwork& prefix,
      [[maybe_unused]] const Attributes& attr,
      [[maybe_unused]] const PolicyPrefixMatch& match,
      [[maybe_unused]] const PolicyActionBase& missAction,
      [[maybe_unused]] const std::string& termName,
      [[maybe_unused]] const std::string& policyName) noexcept {
    /*
     * This function will never be called. Compiler requires templated
     * base class to have an implementation
     */
    return false;
  }

  // We can overload log with AttributeMatch later
};
} // namespace routing
} // namespace facebook
