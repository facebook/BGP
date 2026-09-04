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
#include <functional>
#include <typeindex>

#include "fboss/lib/RadixTree.h"
#include "neteng/fboss/bgp/cpp/policy/base/PolicyStructsBase.h"
#include "re2/re2.h"

namespace facebook {
namespace routing {

template <typename MatchInput, typename AddtionalMatchInput>
class PolicyMatchBase {
 public:
  explicit PolicyMatchBase(const std::string& matchName)
      : matchName_(matchName) {}
  virtual ~PolicyMatchBase() = default;

  virtual bool Match(
      const MatchInput&,
      const std::optional<AddtionalMatchInput>& = std::nullopt) const {
    /*
     * this function is needed because we create REFERENCE type
     * but it won't be called since REFERENCE type will be replaced by
     * the actual inline struct during initialization
     */
    throw std::bad_function_call{};
  }

  const std::string& getMatchName() const {
    return matchName_;
  }

  /*
   * return run time class type
   * e.g. class PrefixMatch : public PolicyMatchBase<T> {}
   *      PrefixMatch p;
   *      p.getClassType() returns std::type_index(typeid(PrefixMatch)
   */
  std::type_index getClassType() {
    return std::type_index(typeid(*this));
  }

 private:
  // name of match struct, optional
  const std::string matchName_;
};

struct PrefixListEntry {
  folly::CIDRNetwork prefix;
  std::vector<std::pair<PolicyComparisonOperator, int>> prefixLenRanges;
};

struct RankedMatch {
  size_t order;
  bool match;
};

struct IPWithRankedMatch {
  folly::CIDRNetwork ip;
  RankedMatch rankedMatch;
};

using PrefixLenRanges =
    std::vector<std::pair<routing::PolicyComparisonOperator, int>>;

using PrefixLenOrRegexComp =
    std::variant<PrefixLenRanges, std::shared_ptr<re2::RE2>>;

class PrefixTree {
 public:
  explicit PrefixTree(
      facebook::network::RadixTree<
          folly::IPAddress,
          std::vector<PrefixLenOrRegexComp>> radixTree,
      bool needCheckOrder = true)
      : radixTree_(std::move(radixTree)), needCheckOrder_(needCheckOrder) {}

  PrefixTree() noexcept = default;
  ~PrefixTree() noexcept = default;

  explicit PrefixTree(PrefixTree&& a) = default;
  PrefixTree& operator=(PrefixTree&& r) noexcept = default;

  bool Match(const folly::CIDRNetwork& prefix) const noexcept;

 protected:
  facebook::network::
      RadixTree<folly::IPAddress, std::vector<PrefixLenOrRegexComp>>
          radixTree_{};
  // preserve original policy order
  std::unordered_map<folly::CIDRNetwork, RankedMatch> origOrder_;
  bool needCheckOrder_ = true;
};

} // namespace routing
} // namespace facebook
