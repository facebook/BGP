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

#include <folly/String.h>

#include "neteng/fboss/bgp/if/gen-cpp2/policy_thrift_types.h"

#include "PolicyTermBase.h"

namespace facebook {
namespace routing {

template <
    typename Attributes,
    typename PolicyActionData,
    typename PolicyMatchData>
class PolicyBase {
 public:
  explicit PolicyBase(const std::string& policyName)
      : policyName_(policyName) {}

  // getter
  const std::vector<std::shared_ptr<
      PolicyTermBase<Attributes, PolicyActionData, PolicyMatchData>>>&
  getPolicyTerms() const {
    return policyTerms_;
  }
  const std::string& getPolicyName() const {
    return policyName_;
  }

  /*
   * Whether the default action for unmatched prefixes is ALLOW (true)
   * or DENY (false). Defaults to DENY for backwards compatibility.
   */
  bool getDefaultAllow() const {
    return defaultAllow_;
  }

  /*
   * Increase the hit count by the number of count
   * User typically increases by number of prefixes for which policy is
   * being applied.
   */
  void incrementHitCount(uint32_t hitCount) {
    hitCount_ += hitCount;
  }

  // Return hit count for this policy.
  uint64_t getHitCount() const {
    return hitCount_;
  }

  // Maximum policy application time in micro seconds
  uint64_t getMaxPolicyApplyTime() const {
    return maxPolicyApplyTime_.count();
  }

  // Average policy application time in micro seconds
  uint64_t getAvgPolicyApplyTime() const {
    return numOfRuns_ ? totalPolicyApplyTime_.count() / numOfRuns_ : 0;
  }

  // Get number of times this policy is applied
  uint64_t getNumOfRuns() const {
    return numOfRuns_;
  }

  // Update statistics regarding policy execution
  void updatePolicyExecutionStats(
      std::chrono::microseconds timeTaken,
      uint32_t numOfPrefixes) noexcept {
    // Update maximum time taken
    if (timeTaken > maxPolicyApplyTime_) {
      maxPolicyApplyTime_ = timeTaken;
    }
    // Update number of times this policy is executed
    numOfRuns_ += 1;
    // Update total time taken
    totalPolicyApplyTime_ += timeTaken;
    incrementHitCount(numOfPrefixes);
    return;
  }

  neteng::routing::policy::thrift::TPolicyStatementStats
  getPolicyStatementStats() const noexcept {
    neteng::routing::policy::thrift::TPolicyStatementStats stats;
    *stats.name() = getPolicyName();
    *stats.prefix_hit_count() = getHitCount();
    *stats.num_of_runs() = getNumOfRuns();
    *stats.avg_time() = getAvgPolicyApplyTime();
    *stats.max_time() = getMaxPolicyApplyTime();
    for (const auto& term : policyTerms_) {
      stats.term_stats()->emplace_back(term->getTermStats());
    }
    return stats;
  }

 protected:
  /*
   * validatePolicyTerms can be used in child class to do some basic term
   * validations
   */
  void validatePolicyTerms() {
    /*
     * A set of visired term names | A set of goto term names seen so far
     * used for GOTO action validation
     */
    std::unordered_set<std::string> visitedTermNames{}, gotoTermNames{};

    for (const auto& term : policyTerms_) {
      const auto& termName = term->getTermName();
      /*
       * Current term is the goto term of a previous policy, remove it from
       * `gotoTermNames`
       */
      gotoTermNames.erase(termName);

      // add current policy to visitedTermNames if term name is not empty
      if (!termName.empty() && !visitedTermNames.emplace(termName).second) {
        throw std::invalid_argument(
            fmt::format(
                "Found duplicate term name {} in policy {}!",
                termName,
                policyName_));
      }
      // Validate current policy's GOTO term
      auto gotoTermName = term->getMatchGotoTerm();
      if (gotoTermName.size()) {
        /*
         * [ERROR] goto term causes policy circulation: e.g. term2 point to
         * term1
         */
        if (visitedTermNames.count(gotoTermName)) {
          throw std::logic_error(
              fmt::format(
                  "Invalid GOTO config in term {}: goto term {} points to a"
                  " term before current term. This will cause policy circulation.",
                  termName,
                  gotoTermName));
        }
        gotoTermNames.emplace(std::move(gotoTermName));
      }
    }

    /*
     * [ERROR] gotoTermNames should be empty by now, if not, some policies are
     * pointing to undefined terms in their gotoTerm
     */
    if (gotoTermNames.size()) {
      throw std::out_of_range(
          fmt::format(
              "GOTO term(s) {} does not exist!",
              folly::join(",", gotoTermNames)));
    }
  }
  // Maximum time it took for the policy application
  std::chrono::microseconds maxPolicyApplyTime_{0};
  // Total time it took for the policy application
  std::chrono::microseconds totalPolicyApplyTime_{0};
  // Number of times, full policy is executed
  uint64_t numOfRuns_{0};
  // Number of prefixes for which this policy is applied
  uint64_t hitCount_{0};

  // internal policy database, mandatory
  const std::string policyName_;
  // list of PolicyTerms in order
  std::vector<std::shared_ptr<
      PolicyTermBase<Attributes, PolicyActionData, PolicyMatchData>>>
      policyTerms_;
  // Default action for prefixes that don't match any term (default: DENY)
  bool defaultAllow_{false};
};
} // namespace routing
} // namespace facebook
