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

#include <boost/regex.hpp>
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/rib_policy_types.h"
#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/common/Types.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/policy/base/PolicyMatchBase.h"

namespace facebook::bgp {
class PrefixTreeMatch
    : public routing::PolicyMatchBase<folly::CIDRNetwork, BgpPolicyMatchData>,
      routing::PrefixTree {
 public:
  explicit PrefixTreeMatch(const bgp_policy::BgpPolicyAtomicMatch& policyMatch)
      : PolicyMatchBase(*policyMatch.prefix_filters()->name()),
        PrefixTree(
            validateAndCreatePrefixTree(*policyMatch.prefix_filters()),
            /* need check order = (bgp does not use custom logic) */ false),
        prefixListNames_(*policyMatch.prefix_filters()->prefix_list_names()) {}

  bool Match(
      const folly::CIDRNetwork& prefix,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override {
    return PrefixTree::Match(prefix);
  }

  void PopulateReferences(
      const folly::F14NodeMap<std::string, routing_policy::PrefixList>&
          prefixListMap);

  static facebook::network::RadixTree<
      folly::IPAddress,
      std::vector<routing::PrefixLenOrRegexComp>>
  validateAndCreatePrefixTree(const routing_policy::PrefixList& tPrefixList);

 private:
  /*
   * match only refers to predefined prefix list by their names,
   * User needs to call PopulateReferences() to populate actual PrefixListEntry
   * in prefixList_
   */
  std::vector<std::string> prefixListNames_{};
};

// AttributesMatch

using AttributesMatch = routing::
    PolicyMatchBase<std::shared_ptr<const BgpPath>, BgpPolicyMatchData>;

class CommunityMatch : public AttributesMatch {
 public:
  explicit CommunityMatch(const bgp_policy::BgpPolicyAtomicMatch& policyMatch);
  // The constructor for TBgpPathMatcher that is used in RibPolicy
  explicit CommunityMatch(const rib_policy::TBgpPathMatcher& matcher)
      : CommunityMatch(
            matcher.community_list()
                ? *matcher.community_list()
                : throw BgpError(
                      "The attribute \"community_list\" is empty.")) {}
  // The constructor for TCommunityListMatch that is used in RibPolicy
  explicit CommunityMatch(const rib_policy::TCommunityListMatch& listMatch);
  virtual ~CommunityMatch() = default;
  const bgp_policy::CommunityList& getMatch() const {
    return communityList_;
  }
  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;
  void PopulateReferences(
      const folly::F14NodeMap<std::string, bgp_policy::CommunityList>&
          communityListMap);

 private:
  /*
   * Returns an empty communityList which sets
   *  - boolean_operator = booleanOperator (default AND)
   *  - exact_match = false
   */
  static bgp_policy::CommunityList getEmptyCommunityList(
      routing_policy::BooleanOperator booleanOperator =
          routing_policy::BooleanOperator::AND);

  /*
   * Check if the two SORTED sets are overlapped
   * unlike std::set_difference that returns all non-overlapped entries,
   * areOverlapped returns as soon as it discovers one overlapped entry
   */
  static bool areOverlapped(
      const std::set<std::string>& vec1,
      const std::set<std::string>& vec2);

  void PopulateCommunities(const bgp_policy::CommunityList& communityList);
  void PopulateCommunities(
      const std::vector<rib_policy::TBgpCommunityMatch>& communityMatches);

  /*
   * Match one of the communityStrings_ or communityRegexs_
   * communityStringsDoNotMatch_ has no effect here.
   */
  bool MatchAny(const std::shared_ptr<const BgpPath>& attrs) const noexcept;
  /*
   * Match all of the communityStrings_ and communityRegexs_
   * and match none of communityStringsDoNotMatch_
   */
  bool MatchAll(const std::shared_ptr<const BgpPath>& attrs) const noexcept;
  /*
   * Ensure every entry matches either communityStrings_ or
   * communityRegexs_. communityStringsDoNotMatch_ is not checked
   */
  bool ExactMatch(
      const std::set<std::string>& communitiesStringsFromAttr) const noexcept;
  const bgp_policy::CommunityList communityList_;
  /*
   * std::set maintains sorted and unique community strings for fast comparison
   * positive match: must have
   */
  std::set<std::string> communityStrings_{};
  // negative match: must not have
  std::set<std::string> communityStringsDoNotMatch_{};
  // storing community regex
  std::vector<boost::regex> communityRegexs_;

/*
 * per class placeholder for code injection
 * only need to setup once
 */
#ifdef CommunityMatch_TEST_FRIENDS
  CommunityMatch_TEST_FRIENDS
#endif
};

class CommunityCountMatch : public AttributesMatch {
 public:
  explicit CommunityCountMatch(
      const bgp_policy::BgpPolicyAtomicMatch& policyMatch)
      : AttributesMatch(*policyMatch.community_count()->name()),
        communityCount_(
            policyMatch.community_count()
                ? *policyMatch.community_count()
                : throw BgpError(
                      "The attribute \"community_count\" is empty.")) {}
  virtual ~CommunityCountMatch() = default;
  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;
  const bgp_policy::CommunityCount& getMatch() const {
    return communityCount_;
  }

 private:
  const bgp_policy::CommunityCount communityCount_;
};

class OriginMatch : public AttributesMatch {
 public:
  explicit OriginMatch(const bgp_policy::BgpPolicyAtomicMatch& policyMatch)
      : AttributesMatch(std::string("")),
        origin_(
            policyMatch.origin()
                ? *policyMatch.origin()
                : throw BgpError("The attribute \"origin\" is empty.")) {}
  explicit OriginMatch(const rib_policy::TBgpPathMatcher& matcher)
      : AttributesMatch(""),
        origin_(
            matcher.origin()
                ? *matcher.origin()
                : throw BgpError("The attribute \"origin\" is empty.")) {}

  virtual ~OriginMatch() = default;
  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;
  const bgp_policy::Origin& getMatch() const {
    return origin_;
  }

 private:
  const bgp_policy::Origin origin_;
};

class WeightMatch : public AttributesMatch {
 public:
  explicit WeightMatch(const bgp_policy::BgpPolicyAtomicMatch& policyMatch)
      : AttributesMatch(std::string("")),
        weightComparitor_(
            policyMatch.weight()
                ? *policyMatch.weight()
                : throw BgpError("The attribute \"weight\" is empty.")),
        weightOperator_(
            toPolicyComparisonOperator(*weightComparitor_.compare_operator())) {
  }

  virtual ~WeightMatch() = default;
  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;
  const routing_policy::CompareNumericValue& getMatch() const {
    return weightComparitor_;
  }

 private:
  const routing_policy::CompareNumericValue weightComparitor_;
  const routing::PolicyComparisonOperator weightOperator_ =
      routing::PolicyComparisonOperator::EQ;
};

class AsPathMatch : public AttributesMatch {
 public:
  explicit AsPathMatch(const bgp_policy::BgpPolicyAtomicMatch& policyMatch)
      : AttributesMatch(*policyMatch.as_path_filters()->name()),
        asPathList_(
            policyMatch.as_path_filters()
                ? *policyMatch.as_path_filters()
                : throw BgpError(
                      "The attribute \"as_path_filters\" is empty.")) {
    if (asPathList_.as_paths() && !asPathList_.as_paths()->empty()) {
      PopulateAsPathRegexs(*(asPathList_.as_paths()));
    }
  }
  explicit AsPathMatch(const rib_policy::TBgpPathMatcher& matcher)
      : AttributesMatch(""), asPathList_(getEmptyAsPathList()) {
    if (matcher.as_path_regex()) {
      PopulateAsPathRegexs({*matcher.as_path_regex()});
    } else {
      throw BgpError("The attribute \"as_path_regex\" is empty.");
    }
  }
  virtual ~AsPathMatch() = default;
  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;
  const bgp_policy::AsPathList& getMatch() const {
    return asPathList_;
  }
  void PopulateReferences(
      const folly::F14NodeMap<std::string, bgp_policy::AsPathList>&
          asPathListMap);

 private:
  /*
   * Returns the empty asPathList for TBgpPathMatcher which sets
   *  - boolean_operator = AND
   *  - exact_match = false
   */
  static bgp_policy::AsPathList getEmptyAsPathList();

  bool MatchAny(const std::shared_ptr<const BgpPath>& attrs) const;
  bool MatchAll(const std::shared_ptr<const BgpPath>& attrs) const;
  void PopulateAsPathRegexs(const std::vector<std::string>& asPaths);
  const bgp_policy::AsPathList asPathList_;
  // storing aspath regex
  std::vector<boost::regex> asPathRegexs_;
  /*
   * This is temporary for few releases. Will go away
   */
  uint32_t matchInterval_{100};
};

class AsPathLenMatch : public AttributesMatch {
 public:
  explicit AsPathLenMatch(const bgp_policy::BgpPolicyAtomicMatch& policyMatch)
      : AttributesMatch(std::string("")),
        asPathLengths_(
            policyMatch.as_path_len_filter()
                ? *policyMatch.as_path_len_filter()
                : throw BgpError("The attribute \"as_path_len\" is empty.")) {}
  explicit AsPathLenMatch(const rib_policy::TBgpPathMatcher& matcher)
      : AttributesMatch(""), asPathLengths_(getAsPathLengths(matcher)) {}

  virtual ~AsPathLenMatch() = default;
  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;
  const std::vector<routing_policy::CompareNumericValue>& getMatch() const {
    return asPathLengths_;
  }

 private:
  std::vector<routing_policy::CompareNumericValue> getAsPathLengths(
      const rib_policy::TBgpPathMatcher& matcher);

  const std::vector<routing_policy::CompareNumericValue> asPathLengths_;
};

class MinLbwBpsMatch : public AttributesMatch {
 public:
  explicit MinLbwBpsMatch(const rib_policy::TBgpPathMatcher& matcher)
      : AttributesMatch(""),
        minLbwBps_(
            matcher.min_lbw_bps()
                ? *matcher.min_lbw_bps()
                : throw BgpError("The attribute \"min_lbw_bps\" is empty.")) {}

  virtual ~MinLbwBpsMatch() override = default;

  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;

 private:
  // minimum link bandwidth in bits per second
  const int64_t minLbwBps_;
};

class AsPathLenWithConfedMatch : public AttributesMatch {
 public:
  explicit AsPathLenWithConfedMatch(
      const bgp_policy::BgpPolicyAtomicMatch& policyMatch)
      : AttributesMatch(std::string("")),
        asPathLengthsWithConfed_(
            policyMatch.as_path_len_with_confed_filter()
                ? *policyMatch.as_path_len_with_confed_filter()
                : throw BgpError(
                      "The attribute \"as_path_len_with_confed_filter\" is empty.")) {
  }
  virtual ~AsPathLenWithConfedMatch() = default;
  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;
  const std::vector<routing_policy::CompareNumericValue>& getMatch() const {
    return asPathLengthsWithConfed_;
  }

 private:
  const std::vector<routing_policy::CompareNumericValue>
      asPathLengthsWithConfed_;
};

// A class that always returns match when route is evaluated against
class AlwaysMatch : public AttributesMatch {
 public:
  explicit AlwaysMatch()
      : AttributesMatch(std::string("")), alwaysMatch_(true) {}
  virtual ~AlwaysMatch() override = default;
  virtual bool Match(
      const std::shared_ptr<const BgpPath>& attrs,
      const std::optional<BgpPolicyMatchData>& =
          std::nullopt) const noexcept override;
  const bool& getMatch() const {
    return alwaysMatch_;
  }

 private:
  const bool alwaysMatch_;
};

} // namespace facebook::bgp
