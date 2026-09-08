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

#include <folly/Benchmark.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"

/**
Tests to benchmark performance of prefix-list based policies.

Cost of prefix-list based policy evaluation depends on two parameters
 - # of prefixes in the prefix-list and
 - # of routes we're evaluating against the policy.
Benchmarking is done by keeping one parameter constant while scaling the other
one. We exercise the worst case in evaluation i.e. the routes to be evaluated
do no match any prefix in the prefix-list. In one set of tests, the # of routes
to be evaluated are kept constant and the size of the prefix-list is increased.
In particular, we evaluate 2 routes against a prefix-list of size 5, 50, 500,
5000 & 5000.In another set of tests, the size of the prefix-list is kept
constant and the # of routes to be evaluated is increased. In particular,2, 20,
200, 2000 & 20000 routes are evaluted against a prefix-list of size 200.
*/

using namespace std;
using namespace folly;

namespace facebook::bgp {

using bgp_policy::BgpPolicyActionType;
using nettools::bgplib::BgpAttrOrigin;

// Global variables.

// First octes of all prefixes in the prefix-list.
const std::string basePrefix = "13";

// First octets of prefixes to be evaluated.
const std::string nonMatchingbasePrefix = "14";

/*
 * We'll add two routes that will always match, to ensure correctness
 * of policy evaluation.
 */
const auto matchingPrefix1 =
    folly::CIDRNetwork(folly::IPAddress(basePrefix + ".0.0.1"), 32);
const auto matchingPrefix2 =
    folly::CIDRNetwork(folly::IPAddress(basePrefix + ".0.0.2"), 32);

/**
Create a set of prefixes given the number of prefixes and starting two octets
of the prefixes.

Args:
    numPrefixes: Number of prefixes that are requested.
    startPrefix: Const reference to string repesentation of the first two
octets.

Returns:
    std::vector<folly::CIDRNetwork>: A vector of all prefixes in CIDR format.
*/
std::vector<folly::CIDRNetwork> createPrefixSet(
    uint32_t numPrefixes,
    const std::string& startPrefix) {
  std::vector<folly::CIDRNetwork> prefixes = {};
  std::string prefix;
  folly::CIDRNetwork cidr;

  // No need to support more than 2^24 prefixes.
  assert(numPrefixes <= 16777216);

  // All possible values of an octet in an IPv4 address.
  std::vector<uint32_t> possibleOctets = {};
  for (uint32_t i = 0; i < 256; i++) {
    possibleOctets.push_back(i);
  }

  // Create our prefix list with the specified length.
  for (uint32_t x : possibleOctets) {
    for (uint32_t y : possibleOctets) {
      for (uint32_t z : possibleOctets) {
        // Requisite prefixes are generated.
        if (numPrefixes == 0) {
          break;
        }
        numPrefixes--;
        prefix = fmt::format("{}.{}.{}.{}", startPrefix, x, y, z);
        cidr = folly::CIDRNetwork(folly::IPAddress(prefix), 32);
        prefixes.push_back(cidr);
      }
      // We're done.
      if (numPrefixes == 0) {
        break;
      }
    }
    // We're done.
    if (numPrefixes == 0) {
      break;
    }
  }
  return prefixes;
}

/**
Create a prefix-list in policy format, given the size of the prefix-list.

Args:
    prefixListSize: Size of prefix-list to be created.

Returns:
    std::vector<routing_policy::PrefixListEntry>: A vector of all prefix list
entries, in policy format.
*/
std::vector<routing_policy::PrefixListEntry> createPrefixList(
    uint32_t prefixListSize) {
  routing_policy::PrefixListEntry prefixListEntry;
  std::vector<routing_policy::PrefixListEntry> prefixList;
  routing_policy::CompareNumericValue compareStructEQ;

  // Setup for exact match with prefix len of 32.
  *compareStructEQ.compare_operator() = routing_policy::ComparisonOperator::EQ;
  *compareStructEQ.value() = 32;

  // Obtain the set of prefixes to put in the list.
  std::vector<folly::CIDRNetwork> prefix_set =
      createPrefixSet(prefixListSize, basePrefix);

  // Create policy styled prefix-list.
  for (folly::CIDRNetwork prefix : prefix_set) {
    prefixListEntry = createPrefixListEntry(
        IPAddress::networkToString(prefix), {compareStructEQ});
    prefixList.push_back(prefixListEntry);
  }

  return prefixList;
}

/**
Create a set of prefixes to evaluate against the prefix-list base policy, given
the size of requested prefixes. These prefixes will not match the prefix-list
in policy.

Args:
    numPrefixes: Number of prefixes that are requested.

Returns:
    std::vector<folly::CIDRNetwork>: A vector of all prefixes in CIDR format.
*/
std::vector<folly::CIDRNetwork> createPrefixesToEvalute(uint32_t numPrefixes) {
  std::vector<folly::CIDRNetwork> prefixesToBeEvaluated = {};

  // Create our prefix list with the specified length.
  prefixesToBeEvaluated = createPrefixSet(numPrefixes, nonMatchingbasePrefix);

  // Lets add two prefixes that we know will match the policy.
  prefixesToBeEvaluated.push_back(matchingPrefix1);
  prefixesToBeEvaluated.push_back(matchingPrefix2);

  return prefixesToBeEvaluated;
}

/**
Create a prefix-list based policy and routes to be evaluated against the policy.
Finally, evaluate routes against the policy.

Args:
    prefixListSize: Size of prefix-list to be created.
    numPrefixes: Number of prefixes that are requested to be evaluated against
    the policy
*/
void prefix_list_scale_test(uint32_t prefixListSize, uint32_t numPrefixes) {
  const std::vector<routing_policy::PrefixListEntry>& prefixList =
      createPrefixList(prefixListSize);

  /*
   * Creating a term that matches against prefix-list and PERMITs. No attribute
   * modification.
   */
  const bgp_policy::BgpPolicyAtomicMatch& match1 =
      createPrefixListMatch(prefixList);
  bgp_policy::BgpPolicyAction actionPermit =
      createBgpPolicyAction(BgpPolicyActionType::PERMIT);
  bgp_policy::BgpPolicyTerm term1 =
      createBgpPolicyTerm("Term1", "", {match1}, {actionPermit});

  // Create a policy with the above term.
  const string policyName = "Policy Statement";
  const bgp_policy::BgpPolicies& policyConfig =
      createBgpPolicies(policyName, {term1});
  PolicyManager policyManager(policyConfig, createTestBgpGlobalConfig());

  // Create prefixes to be evaluated against the policy.
  const std::vector<folly::CIDRNetwork>& prefixSetIn =
      createPrefixesToEvalute(numPrefixes);

  // BGP Atributes with incomplete origin.
  std::shared_ptr<facebook::bgp::BgpPath> attrsIn =
      createBgpPathWithOrigin(BgpAttrOrigin::BGP_ORIGIN_INCOMPLETE);
  attrsIn->publish();

  // Apply policy.
  PolicyInMessage policyIn(prefixSetIn, attrsIn);
  auto policyOut = policyManager.applyPolicy(policyName, policyIn);

  // Verify that the two matching prefixes are permitted, while the rest aren't.
  int count = 0;
  for (auto& kv : policyOut.result) {
    const auto& str = kv.second->policyName;
    if (str.find("Accepted") != std::string::npos) {
      count++;
    }
  }
  ASSERT_EQ(2, count);

  // Verify actions.

  // Verify matching prefixes accepted and the attributes are not modified.
  ASSERT_NE(policyOut.result.find(matchingPrefix1), policyOut.result.end());
  ASSERT_NE(policyOut.result.find(matchingPrefix2), policyOut.result.end());
  EXPECT_EQ(attrsIn, policyOut.result[matchingPrefix1]->attrs);
  EXPECT_EQ(attrsIn, policyOut.result[matchingPrefix2]->attrs);

  // Verify two matching prefixes share the same attribute.
  EXPECT_EQ(
      policyOut.result[matchingPrefix1]->attrs,
      policyOut.result[matchingPrefix2]->attrs);
  auto attrsOut = policyOut.result[matchingPrefix1];
  EXPECT_TRUE(attrsOut->attrs->isPublished());
}

} // namespace facebook::bgp

BENCHMARK(BM_PrefixList_Var_5_Routes_Fixed_2000) {
  // Evalutate 2000 routes against a policy with 5 prefixes.
  facebook::bgp::prefix_list_scale_test(5, 2);
}

BENCHMARK(BM_PrefixList_Var_50_Routes_Fixed_2000) {
  // Evalutate 2000 routes against a policy with 50 prefixes.
  facebook::bgp::prefix_list_scale_test(50, 2000);
}

BENCHMARK(BM_PrefixList_Var_500_Routes_Fixed_2000) {
  // Evalutate 2000 routes against a policy with 500 prefixes.
  facebook::bgp::prefix_list_scale_test(500, 2000);
}

BENCHMARK(BM_PrefixList_Var_5000_Routes_Fixed_2000) {
  // Evalutate 2000 routes against a policy with 5000 prefixes.
  facebook::bgp::prefix_list_scale_test(5000, 2000);
}

BENCHMARK(BM_PrefixList_Var_50000_Routes_Fixed_2000) {
  // Evalutate 2000 routes against a policy with 50000 prefixes.
  facebook::bgp::prefix_list_scale_test(50000, 2000);
}

BENCHMARK(BM_PrefixList_Var_500000_Routes_Fixed_2000) {
  // Evalutate 2000 routes against a policy with 50000 prefixes.
  facebook::bgp::prefix_list_scale_test(500000, 2000);
}

BENCHMARK(BM_PrefixList_Fixed_500_Routes_Var_20) {
  // Evalutate 20 routes against a policy with 500 prefixes
  facebook::bgp::prefix_list_scale_test(500, 20);
}

BENCHMARK(BM_PrefixList_Fixed_500_Routes_Var_200) {
  // Evalutate 200 routes against a policy with 500 prefixes.
  facebook::bgp::prefix_list_scale_test(500, 200);
}

BENCHMARK(BM_PrefixList_Fixed_500_Routes_Var_2000) {
  // Evalutate 2000 routes against a policy with 500 prefixes.
  facebook::bgp::prefix_list_scale_test(500, 2000);
}

BENCHMARK(BM_PrefixList_Fixed_500_Routes_Var_20000) {
  // Evalutate 20000 routes against a policy with 500 prefixes.
  facebook::bgp::prefix_list_scale_test(500, 20000);
}

BENCHMARK(BM_PrefixList_Fixed_1000_Routes_Var_20000) {
  // Evalutate 20000 routes against a policy with 1000 prefixes.
  facebook::bgp::prefix_list_scale_test(1000, 20000);
}

BENCHMARK(BM_PrefixList_Fixed_200_Routes_Var_20000) {
  // Evalutate 20000 routes against a policy with 200 prefixes.
  facebook::bgp::prefix_list_scale_test(200, 20000);
}

BENCHMARK(BM_PrefixList_Fixed_100_Routes_Var_20000) {
  // Evalutate 20000 routes against a policy with 100 prefixes.
  facebook::bgp::prefix_list_scale_test(100, 20000);
}

BENCHMARK(BM_PrefixList_Fixed_20000_Routes_Var_20000) {
  // Evalutate 20000 routes against a policy with 20000 prefixes.
  facebook::bgp::prefix_list_scale_test(20000, 20000);
}

int main() {
  runBenchmarks();
}
