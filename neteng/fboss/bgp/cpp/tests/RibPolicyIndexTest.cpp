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

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/rib/RibEntry.h"
#include "neteng/fboss/bgp/cpp/rib/RibPolicy.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibUtils.h"

using namespace facebook::bgp;
using namespace facebook::bgp::rib_policy;
using namespace facebook::nettools::bgplib;

namespace facebook::bgp {

class RibPolicyIndexTest : public ::testing::Test {
 protected:
  std::shared_ptr<BgpPath> buildPathWithCommunities(
      const std::vector<std::pair<uint16_t, uint16_t>>& communities) {
    return buildPathWithCommunitiesAndNexthop(
        communities, folly::IPAddress("10.0.0.1"));
  }

  std::shared_ptr<BgpPath> buildPathWithCommunitiesAndNexthop(
      const std::vector<std::pair<uint16_t, uint16_t>>& communities,
      const folly::IPAddress& nexthop) {
    auto path = std::make_shared<BgpPath>(*buildBgpPathFields(4, 4, 4, 4));
    BgpAttrCommunitiesC comms;
    for (const auto& [asn, value] : communities) {
      comms.emplace_back(asn, value);
    }
    path->setCommunities(comms);
    path->setNexthop(nexthop);
    path->publish();
    return path;
  }

  std::shared_ptr<BgpPath> buildPathWithAsPath(
      const std::vector<uint32_t>& asSeq,
      const folly::IPAddress& nexthop = folly::IPAddress("10.0.0.1")) {
    auto path = std::make_shared<BgpPath>(*buildBgpPathFields(4, 4, 4, 4));
    BgpAttrAsPathSegmentC seg;
    for (auto asn : asSeq) {
      seg.asSequence.push_back(asn);
    }
    BgpAttrAsPathC asPath;
    asPath.push_back(seg);
    path->setAsPath(asPath);
    // Also set a community so a community-based BgpPathMatcher can select it.
    BgpAttrCommunitiesC comms;
    comms.emplace_back(100, 200);
    path->setCommunities(comms);
    path->setNexthop(nexthop);
    path->publish();
    return path;
  }

  RibEntry createRibEntryWithPaths(
      const folly::CIDRNetwork& prefix,
      const std::vector<std::shared_ptr<BgpPath>>& paths) {
    RibEntry entry(prefix);
    for (size_t i = 0; i < paths.size(); ++i) {
      const auto& path = paths[i];
      // Distinct peer per path so each becomes its own RouteInfo.
      TinyPeerInfo peer(
          folly::IPAddress::fromLong(0x02000000 + static_cast<uint32_t>(i)),
          1000 + static_cast<uint32_t>(i),
          static_cast<uint32_t>(i),
          BgpSessionType::EBGP,
          false);
      entry.updatePath(peer, path, /*installToFib=*/false);
    }
    return entry;
  }
};

TEST_F(RibPolicyIndexTest, CommunityIndexHitMiss) {
  auto prefix1 = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("2.2.2.0/24");

  auto comm1 = std::make_pair<uint16_t, uint16_t>(100, 200);
  auto comm2 = std::make_pair<uint16_t, uint16_t>(300, 400);

  auto path1 = buildPathWithCommunities({comm1});
  auto path2 = buildPathWithCommunities({comm1});
  auto path3 = buildPathWithCommunities({comm2});

  auto entry1 = createRibEntryWithPaths(prefix1, {path1});
  auto entry2 = createRibEntryWithPaths(prefix2, {path2});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  /*
   * Empty prefix set so only community matching decides (a non-empty set would
   * make both entries match via matchPrefix, bypassing the community path).
   */
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = matcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  /*
   * Index is always on. entry1 (comm1) matches the statement; entry2 (comm1,
   * shared interned community-set) should hit the community index and also
   * match. Both return the correct statement name for the shared community-set.
   */
  bool result1 = raPolicy.overwriteRouteAttributes(entry1, change);
  bool result2 = raPolicy.overwriteRouteAttributes(entry2, change);

  EXPECT_TRUE(result1);
  EXPECT_TRUE(result2);
}

TEST_F(RibPolicyIndexTest, CommunityIndexNewPrefixCorrectness) {
  auto prefix1 = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("2.2.2.0/24");

  auto comm1 = std::make_pair<uint16_t, uint16_t>(100, 200);
  auto path1 = buildPathWithCommunities({comm1});
  auto path2 = buildPathWithCommunities({comm1});

  auto entry1 = createRibEntryWithPaths(prefix1, {path1});
  auto entry2 = createRibEntryWithPaths(prefix2, {path2});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = matcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  /*
   * Index is always on. entry1 (comm1) populates the community index on miss.
   * entry2 (same comm1, distinct prefix) hits the index without rescanning and
   * resolves to the same statement. Correctness: both match.
   */
  bool result1 = raPolicy.overwriteRouteAttributes(entry1, change);
  bool result2 = raPolicy.overwriteRouteAttributes(entry2, change);

  EXPECT_TRUE(result1);
  EXPECT_TRUE(result2);
}

TEST_F(RibPolicyIndexTest, UcmpWeightIndexDeterminism) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");

  auto comm1 = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto comm2 = std::make_pair<uint16_t, uint16_t>(100, 2);
  auto comm3 = std::make_pair<uint16_t, uint16_t>(100, 3);

  auto path1 = buildPathWithCommunities({comm1});
  auto path2 = buildPathWithCommunities({comm2});
  auto path3 = buildPathWithCommunities({comm3});

  auto entry = createRibEntryWithPaths(prefix, {path1, path2, path3});

  TBgpPathMatcher matcher1;
  TBgpCommunityMatch communityMatch1 = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList1 = createTCommunityListMatch(
      {communityMatch1}, routing_policy::BooleanOperator::OR);
  matcher1.community_list() = communityList1;

  TBgpPathMatcher matcher2;
  TBgpCommunityMatch communityMatch2 = createTBgpCommunityMatch(
      100, 2, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList2 = createTCommunityListMatch(
      {communityMatch2}, routing_policy::BooleanOperator::OR);
  matcher2.community_list() = communityList2;

  TNextHopWeightAction weightAction1;
  weightAction1.path_matchers() = {matcher1};
  weightAction1.weight() = 10;

  TNextHopWeightAction weightAction2;
  weightAction2.path_matchers() = {matcher2};
  weightAction2.weight() = 20;

  TRouteAttributeUcmpAction ucmpAction;
  ucmpAction.nexthop_weight_actions() = {weightAction1, weightAction2};
  ucmpAction.apply_all_actions_or_fallback_to_ecmp() = false;

  TRibRouteMatcher routeMatcher = createTRibRouteMatcher({prefix});

  TRouteAttributeActions actions;
  actions.set_ucmp_weights() = ucmpAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = routeMatcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy1(policy);
  RouteAttributePolicy raPolicy2(policy);

  RouteAttributePolicy::RibChange change1;
  RouteAttributePolicy::RibChange change2;

  /*
   * Weight index is always on. Two fresh policies (each with a cold index)
   * applied to identical entries must match and produce identical UCMP weights
   * -- the indexed action is deterministic and semantics-preserving.
   */
  auto entry1 = entry;
  bool result1 = raPolicy1.overwriteRouteAttributes(entry1, change1);

  auto entry2 = entry;
  bool result2 = raPolicy2.overwriteRouteAttributes(entry2, change2);

  EXPECT_TRUE(result1);
  EXPECT_EQ(result1, result2);
  EXPECT_EQ(entry1.getRibPolicyUcmpWeight(), entry2.getRibPolicyUcmpWeight());
}

/*
 * STATEMENT MATCHING: Two distinct community statements with distinct LBW
 * actions
 */
TEST_F(RibPolicyIndexTest, DistinctCommunityStatementsCorrectLbw) {
  auto prefix1 = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("2.2.2.0/24");

  auto commA = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto commB = std::make_pair<uint16_t, uint16_t>(100, 2);

  auto pathA = buildPathWithCommunities({commA});
  auto pathB = buildPathWithCommunities({commB});

  auto entryA = createRibEntryWithPaths(prefix1, {pathA});
  auto entryB = createRibEntryWithPaths(prefix2, {pathB});

  // Statement A: community 100:1 -> LBW 10
  TBgpCommunityMatch communityMatchA = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityListA = createTCommunityListMatch(
      {communityMatchA}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcherA = createTRibRouteMatcher({}, communityListA);

  TRouteAttributeActions actionsA;
  TRouteAttributeLbwAction lbwActionA;
  lbwActionA.lbw() = 10;
  actionsA.set_lbw() = lbwActionA;

  TRouteAttributeStatement stmtA;
  stmtA.matcher() = matcherA;
  stmtA.actions() = actionsA;

  // Statement B: community 100:2 -> LBW 20
  TBgpCommunityMatch communityMatchB = createTBgpCommunityMatch(
      100, 2, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityListB = createTCommunityListMatch(
      {communityMatchB}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcherB = createTRibRouteMatcher({}, communityListB);

  TRouteAttributeActions actionsB;
  TRouteAttributeLbwAction lbwActionB;
  lbwActionB.lbw() = 20;
  actionsB.set_lbw() = lbwActionB;

  TRouteAttributeStatement stmtB;
  stmtB.matcher() = matcherB;
  stmtB.actions() = actionsB;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmtA", stmtA);
  policy.statements()->emplace("stmtB", stmtB);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool resultA = raPolicy.overwriteRouteAttributes(entryA, change);
  bool resultB = raPolicy.overwriteRouteAttributes(entryB, change);

  EXPECT_TRUE(resultA);
  EXPECT_TRUE(resultB);
  EXPECT_EQ(entryA.getRibPolicyUcmpWeight(), 10);
  EXPECT_EQ(entryB.getRibPolicyUcmpWeight(), 20);
}

// STATEMENT MATCHING: Two prefixes sharing same interned community-set
TEST_F(RibPolicyIndexTest, SharedCommunitySetSameStatement) {
  auto prefix1 = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("2.2.2.0/24");

  auto comm = std::make_pair<uint16_t, uint16_t>(100, 200);
  auto path1 = buildPathWithCommunities({comm});
  auto path2 = buildPathWithCommunities({comm});

  auto entry1 = createRibEntryWithPaths(prefix1, {path1});
  auto entry2 = createRibEntryWithPaths(prefix2, {path2});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = matcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result1 = raPolicy.overwriteRouteAttributes(entry1, change);
  bool result2 = raPolicy.overwriteRouteAttributes(entry2, change);

  EXPECT_TRUE(result1);
  EXPECT_TRUE(result2);
  EXPECT_EQ(entry1.getRibPolicyUcmpWeight(), 1000);
  EXPECT_EQ(entry2.getRibPolicyUcmpWeight(), 1000);
}

// STATEMENT MATCHING: Mixed policy (prefix-only and community-only)
TEST_F(RibPolicyIndexTest, MixedPrefixAndCommunityStatements) {
  auto prefix1 = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("2.2.2.0/24");

  auto comm = std::make_pair<uint16_t, uint16_t>(100, 200);
  auto path1 = buildPathWithCommunities({});
  auto path2 = buildPathWithCommunities({comm});

  auto entry1 = createRibEntryWithPaths(prefix1, {path1});
  auto entry2 = createRibEntryWithPaths(prefix2, {path2});

  // Prefix-only statement: prefix1 -> LBW 10
  TRibRouteMatcher matcherPrefix = createTRibRouteMatcher({prefix1});
  TRouteAttributeActions actionsPrefix;
  TRouteAttributeLbwAction lbwActionPrefix;
  lbwActionPrefix.lbw() = 10;
  actionsPrefix.set_lbw() = lbwActionPrefix;

  TRouteAttributeStatement stmtPrefix;
  stmtPrefix.matcher() = matcherPrefix;
  stmtPrefix.actions() = actionsPrefix;

  // Community-only statement: comm 100:200 -> LBW 20
  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcherComm = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actionsCom;
  TRouteAttributeLbwAction lbwActionCom;
  lbwActionCom.lbw() = 20;
  actionsCom.set_lbw() = lbwActionCom;

  TRouteAttributeStatement stmtCom;
  stmtCom.matcher() = matcherComm;
  stmtCom.actions() = actionsCom;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmtPrefix", stmtPrefix);
  policy.statements()->emplace("stmtComm", stmtCom);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result1 = raPolicy.overwriteRouteAttributes(entry1, change);
  bool result2 = raPolicy.overwriteRouteAttributes(entry2, change);

  EXPECT_TRUE(result1);
  EXPECT_TRUE(result2);
  EXPECT_EQ(entry1.getRibPolicyUcmpWeight(), 10);
  EXPECT_EQ(entry2.getRibPolicyUcmpWeight(), 20);
}

// STATEMENT MATCHING: Route with neither prefix nor community match
TEST_F(RibPolicyIndexTest, NoMatchReturnsfalse) {
  auto prefix = folly::IPAddress::createNetwork("3.3.3.0/24");
  auto comm = std::make_pair<uint16_t, uint16_t>(100, 999);
  auto path = buildPathWithCommunities({comm});
  auto entry = createRibEntryWithPaths(prefix, {path});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = matcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result = raPolicy.overwriteRouteAttributes(entry, change);
  EXPECT_FALSE(result);
  EXPECT_FALSE(entry.getRibPolicyUcmpWeight().has_value());
}

// STATEMENT MATCHING: Inactive/expired statement
TEST_F(RibPolicyIndexTest, InactiveStatementNoMatch) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto comm = std::make_pair<uint16_t, uint16_t>(100, 200);
  auto path = buildPathWithCommunities({comm});
  auto entry = createRibEntryWithPaths(prefix, {path});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = matcher;
  stmt.actions() = actions;
  // Set expiration in the past
  stmt.expiration_time_s() = 1;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result = raPolicy.overwriteRouteAttributes(entry, change);
  EXPECT_FALSE(result);
  EXPECT_FALSE(entry.getRibPolicyUcmpWeight().has_value());
}

// WEIGHTS: Two community-keyed actions with distinct weights
TEST_F(RibPolicyIndexTest, UcmpTwoCommunitiesDistinctWeights) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");

  auto nh1 = folly::IPAddress("10.0.0.1");
  auto nh2 = folly::IPAddress("10.0.0.2");

  auto commA = std::make_pair<uint16_t, uint16_t>(100, 10);
  auto commB = std::make_pair<uint16_t, uint16_t>(100, 20);

  auto pathA = buildPathWithCommunitiesAndNexthop({commA}, nh1);
  auto pathB = buildPathWithCommunitiesAndNexthop({commB}, nh2);
  auto entry = createRibEntryWithPaths(prefix, {pathA, pathB});

  // Seed multipath-selected nexthops
  WeightedNexthopMap seed{{nh1, 1}, {nh2, 1}};
  entry.overrideWeightedNexthops(seed);

  // Route matcher: prefix-based
  TRibRouteMatcher routeMatcher = createTRibRouteMatcher({prefix});

  // Action 1: community 100:10 -> weight 10
  TBgpPathMatcher pathMatcher1;
  TBgpCommunityMatch communityMatch1 = createTBgpCommunityMatch(
      100, 10, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList1 = createTCommunityListMatch(
      {communityMatch1}, routing_policy::BooleanOperator::OR);
  pathMatcher1.community_list() = communityList1;

  TNextHopWeightAction weightAction1;
  weightAction1.path_matchers() = {pathMatcher1};
  weightAction1.weight() = 10;

  // Action 2: community 100:20 -> weight 20
  TBgpPathMatcher pathMatcher2;
  TBgpCommunityMatch communityMatch2 = createTBgpCommunityMatch(
      100, 20, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList2 = createTCommunityListMatch(
      {communityMatch2}, routing_policy::BooleanOperator::OR);
  pathMatcher2.community_list() = communityList2;

  TNextHopWeightAction weightAction2;
  weightAction2.path_matchers() = {pathMatcher2};
  weightAction2.weight() = 20;

  TRouteAttributeUcmpAction ucmpAction;
  ucmpAction.nexthop_weight_actions() = {weightAction1, weightAction2};
  ucmpAction.apply_all_actions_or_fallback_to_ecmp() = false;

  TRouteAttributeActions actions;
  actions.set_ucmp_weights() = ucmpAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = routeMatcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result = raPolicy.overwriteRouteAttributes(entry, change);
  EXPECT_TRUE(result);

  auto weights = entry.getMultipathWeightedNexthops();
  ASSERT_NE(nullptr, weights);
  EXPECT_EQ(2, weights->size());
  EXPECT_EQ(10, weights->at(nh1));
  EXPECT_EQ(20, weights->at(nh2));
}

// WEIGHTS: Different attrs (distinct as-path) matching same action
TEST_F(RibPolicyIndexTest, UcmpDifferentAttrsMatchingSameAction) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");

  auto nh1 = folly::IPAddress("10.0.0.1");
  auto nh2 = folly::IPAddress("10.0.0.2");

  auto path1 = buildPathWithAsPath({64981, 1}, nh1);
  auto path2 = buildPathWithAsPath({64981, 2}, nh2);
  auto entry = createRibEntryWithPaths(prefix, {path1, path2});

  // Seed multipath-selected nexthops
  WeightedNexthopMap seed{{nh1, 1}, {nh2, 1}};
  entry.overrideWeightedNexthops(seed);

  TRibRouteMatcher routeMatcher = createTRibRouteMatcher({prefix});

  // Action: as-path-regex ^64981.* -> weight 10
  TBgpPathMatcher pathMatcher;
  pathMatcher.as_path_regex() = "^64981.*";
  TNextHopWeightAction weightAction;
  weightAction.path_matchers() = {pathMatcher};
  weightAction.weight() = 10;

  TRouteAttributeUcmpAction ucmpAction;
  ucmpAction.nexthop_weight_actions() = {weightAction};
  ucmpAction.apply_all_actions_or_fallback_to_ecmp() = false;

  TRouteAttributeActions actions;
  actions.set_ucmp_weights() = ucmpAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = routeMatcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result = raPolicy.overwriteRouteAttributes(entry, change);
  EXPECT_TRUE(result);

  auto weights = entry.getMultipathWeightedNexthops();
  ASSERT_NE(nullptr, weights);
  EXPECT_EQ(2, weights->size());
  EXPECT_EQ(10, weights->at(nh1));
  EXPECT_EQ(10, weights->at(nh2));
}

// WEIGHTS: divide_weights_by_matching_path_count splits weight evenly
TEST_F(RibPolicyIndexTest, UcmpDivideWeightsByMatchingPathCount) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");

  auto nh1 = folly::IPAddress("10.0.0.1");
  auto nh2 = folly::IPAddress("10.0.0.2");

  auto comm = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto path1 = buildPathWithCommunitiesAndNexthop({comm}, nh1);
  auto path2 = buildPathWithCommunitiesAndNexthop({comm}, nh2);
  auto entry = createRibEntryWithPaths(prefix, {path1, path2});

  // Seed multipath-selected nexthops
  WeightedNexthopMap seed{{nh1, 1}, {nh2, 1}};
  entry.overrideWeightedNexthops(seed);

  TRibRouteMatcher routeMatcher = createTRibRouteMatcher({prefix});

  // Action: community 100:1 -> weight 100, divided by matching path count
  TBgpPathMatcher pathMatcher;
  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  pathMatcher.community_list() = communityList;

  TNextHopWeightAction weightAction;
  weightAction.path_matchers() = {pathMatcher};
  weightAction.weight() = 100;

  TRouteAttributeUcmpAction ucmpAction;
  ucmpAction.nexthop_weight_actions() = {weightAction};
  ucmpAction.apply_all_actions_or_fallback_to_ecmp() = false;
  ucmpAction.divide_weights_by_matching_path_count() = true;

  TRouteAttributeActions actions;
  actions.set_ucmp_weights() = ucmpAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = routeMatcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result = raPolicy.overwriteRouteAttributes(entry, change);
  EXPECT_TRUE(result);

  auto weights = entry.getMultipathWeightedNexthops();
  ASSERT_NE(nullptr, weights);
  EXPECT_EQ(2, weights->size());
  // 100 / 2 = 50 per nexthop
  EXPECT_EQ(50, weights->at(nh1));
  EXPECT_EQ(50, weights->at(nh2));
}

/*
 * WEIGHTS: Strict match flag (apply_all_actions_or_fallback_to_ecmp=true)
 * bails when multipath count != action count
 */
TEST_F(RibPolicyIndexTest, UcmpStrictMatchFlag) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");

  auto nh1 = folly::IPAddress("10.0.0.1");
  auto nh2 = folly::IPAddress("10.0.0.2");

  auto comm = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto path1 = buildPathWithCommunitiesAndNexthop({comm}, nh1);
  auto path2 = buildPathWithCommunitiesAndNexthop({comm}, nh2);
  auto entry = createRibEntryWithPaths(prefix, {path1, path2});

  // Seed 2 nexthops, but we'll only have 1 action
  WeightedNexthopMap seed{{nh1, 3}, {nh2, 7}};
  entry.overrideWeightedNexthops(seed);

  TRibRouteMatcher routeMatcher = createTRibRouteMatcher({prefix});

  // Single action: community 100:1 -> weight 100
  TBgpPathMatcher pathMatcher;
  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  pathMatcher.community_list() = communityList;

  TNextHopWeightAction weightAction;
  weightAction.path_matchers() = {pathMatcher};
  weightAction.weight() = 100;

  TRouteAttributeUcmpAction ucmpAction;
  ucmpAction.nexthop_weight_actions() = {weightAction};
  ucmpAction.apply_all_actions_or_fallback_to_ecmp() = true; // strict

  TRouteAttributeActions actions;
  actions.set_ucmp_weights() = ucmpAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = routeMatcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result = raPolicy.overwriteRouteAttributes(entry, change);
  EXPECT_TRUE(result);

  auto weights = entry.getMultipathWeightedNexthops();
  ASSERT_NE(nullptr, weights);
  EXPECT_EQ(2, weights->size());
  // Strict match fails (2 nexthops, 1 action), weights unchanged from seed
  EXPECT_EQ(3, weights->at(nh1));
  EXPECT_EQ(7, weights->at(nh2));
}

/*
 * WEIGHTS: All-nexthops-must-match: bail if any nexthop's path matches no
 * action
 */
TEST_F(RibPolicyIndexTest, UcmpAllNexthopsMustMatch) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");

  auto nh1 = folly::IPAddress("10.0.0.1");
  auto nh2 = folly::IPAddress("10.0.0.2");

  auto comm1 = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto comm2 = std::make_pair<uint16_t, uint16_t>(100, 2);

  auto path1 = buildPathWithCommunitiesAndNexthop({comm1}, nh1);
  // path2 has comm2, which doesn't match any action
  auto path2 = buildPathWithCommunitiesAndNexthop({comm2}, nh2);
  auto entry = createRibEntryWithPaths(prefix, {path1, path2});

  // Seed 2 nexthops
  WeightedNexthopMap seed{{nh1, 5}, {nh2, 5}};
  entry.overrideWeightedNexthops(seed);

  TRibRouteMatcher routeMatcher = createTRibRouteMatcher({prefix});

  /*
   * Single action: community 100:1 -> weight 10
   * path1 matches, path2 doesn't
   */
  TBgpPathMatcher pathMatcher;
  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  pathMatcher.community_list() = communityList;

  TNextHopWeightAction weightAction;
  weightAction.path_matchers() = {pathMatcher};
  weightAction.weight() = 10;

  TRouteAttributeUcmpAction ucmpAction;
  ucmpAction.nexthop_weight_actions() = {weightAction};
  ucmpAction.apply_all_actions_or_fallback_to_ecmp() = false;

  TRouteAttributeActions actions;
  actions.set_ucmp_weights() = ucmpAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = routeMatcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result = raPolicy.overwriteRouteAttributes(entry, change);
  EXPECT_TRUE(result);

  auto weights = entry.getMultipathWeightedNexthops();
  ASSERT_NE(nullptr, weights);
  EXPECT_EQ(2, weights->size());
  // updateUcmpWeight bails because nh2 matches no action, weights unchanged
  EXPECT_EQ(5, weights->at(nh1));
  EXPECT_EQ(5, weights->at(nh2));
}

// WEIGHTS: Lowest action index wins when one nexthop matches multiple actions
TEST_F(RibPolicyIndexTest, UcmpLowestActionIndexWins) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");

  auto nh1 = folly::IPAddress("10.0.0.1");

  // Path with two communities, each matching a different action
  auto comm1 = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto comm2 = std::make_pair<uint16_t, uint16_t>(100, 2);
  auto path = buildPathWithCommunitiesAndNexthop({comm1, comm2}, nh1);
  auto entry = createRibEntryWithPaths(prefix, {path});

  // Seed single nexthop
  WeightedNexthopMap seed{{nh1, 1}};
  entry.overrideWeightedNexthops(seed);

  TRibRouteMatcher routeMatcher = createTRibRouteMatcher({prefix});

  // Action 1 (index 0): community 100:1 -> weight 10
  TBgpPathMatcher pathMatcher1;
  TBgpCommunityMatch communityMatch1 = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList1 = createTCommunityListMatch(
      {communityMatch1}, routing_policy::BooleanOperator::OR);
  pathMatcher1.community_list() = communityList1;

  TNextHopWeightAction weightAction1;
  weightAction1.path_matchers() = {pathMatcher1};
  weightAction1.weight() = 10;

  // Action 2 (index 1): community 100:2 -> weight 20
  TBgpPathMatcher pathMatcher2;
  TBgpCommunityMatch communityMatch2 = createTBgpCommunityMatch(
      100, 2, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList2 = createTCommunityListMatch(
      {communityMatch2}, routing_policy::BooleanOperator::OR);
  pathMatcher2.community_list() = communityList2;

  TNextHopWeightAction weightAction2;
  weightAction2.path_matchers() = {pathMatcher2};
  weightAction2.weight() = 20;

  TRouteAttributeUcmpAction ucmpAction;
  ucmpAction.nexthop_weight_actions() = {weightAction1, weightAction2};
  ucmpAction.apply_all_actions_or_fallback_to_ecmp() = false;

  TRouteAttributeActions actions;
  actions.set_ucmp_weights() = ucmpAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = routeMatcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change;

  bool result = raPolicy.overwriteRouteAttributes(entry, change);
  EXPECT_TRUE(result);

  auto weights = entry.getMultipathWeightedNexthops();
  ASSERT_NE(nullptr, weights);
  EXPECT_EQ(1, weights->size());
  // Path matches both actions, lowest index (action 1, weight 10) wins
  EXPECT_EQ(10, weights->at(nh1));
}

// STALENESS: Policy swap
TEST_F(RibPolicyIndexTest, PolicySwapCorrectLbw) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto comm = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto path = buildPathWithCommunities({comm});
  auto entry = createRibEntryWithPaths(prefix, {path});

  // Policy A: community 100:1 -> LBW 10
  TBgpCommunityMatch communityMatchA = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityListA = createTCommunityListMatch(
      {communityMatchA}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcherA = createTRibRouteMatcher({}, communityListA);

  TRouteAttributeActions actionsA;
  TRouteAttributeLbwAction lbwActionA;
  lbwActionA.lbw() = 10;
  actionsA.set_lbw() = lbwActionA;

  TRouteAttributeStatement stmtA;
  stmtA.matcher() = matcherA;
  stmtA.actions() = actionsA;

  TRouteAttributePolicy policyA;
  policyA.statements()->emplace("stmt1", stmtA);

  RouteAttributePolicy raPolicyA(policyA);
  RouteAttributePolicy::RibChange changeA;

  bool resultA = raPolicyA.overwriteRouteAttributes(entry, changeA);
  EXPECT_TRUE(resultA);
  EXPECT_EQ(entry.getRibPolicyUcmpWeight(), 10);

  // Policy B: same community but LBW 20
  TBgpCommunityMatch communityMatchB = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityListB = createTCommunityListMatch(
      {communityMatchB}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcherB = createTRibRouteMatcher({}, communityListB);

  TRouteAttributeActions actionsB;
  TRouteAttributeLbwAction lbwActionB;
  lbwActionB.lbw() = 20;
  actionsB.set_lbw() = lbwActionB;

  TRouteAttributeStatement stmtB;
  stmtB.matcher() = matcherB;
  stmtB.actions() = actionsB;

  TRouteAttributePolicy policyB;
  policyB.statements()->emplace("stmt1", stmtB);

  RouteAttributePolicy raPolicyB(policyB);
  RouteAttributePolicy::RibChange changeB;

  bool resultB = raPolicyB.overwriteRouteAttributes(entry, changeB);
  EXPECT_TRUE(resultB);
  EXPECT_EQ(entry.getRibPolicyUcmpWeight(), 20);
}

// STALENESS: Route churn with same attrs
TEST_F(RibPolicyIndexTest, RouteChurnSameAttrsCorrect) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto comm = std::make_pair<uint16_t, uint16_t>(100, 1);

  auto path1 = buildPathWithCommunities({comm});
  auto entry = createRibEntryWithPaths(prefix, {path1});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = matcher;
  stmt.actions() = actions;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmt1", stmt);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change1;

  bool result1 = raPolicy.overwriteRouteAttributes(entry, change1);
  EXPECT_TRUE(result1);
  EXPECT_EQ(entry.getRibPolicyUcmpWeight(), 1000);

  // Build a fresh path with same community
  auto path2 = buildPathWithCommunities({comm});
  auto entry2 = createRibEntryWithPaths(prefix, {path2});

  RouteAttributePolicy::RibChange change2;
  bool result2 = raPolicy.overwriteRouteAttributes(entry2, change2);
  EXPECT_TRUE(result2);
  EXPECT_EQ(entry2.getRibPolicyUcmpWeight(), 1000);
}

/*
 * STALENESS: Route churn with different attrs (different prefixes to avoid
 * prefix cache)
 */
TEST_F(RibPolicyIndexTest, RouteChurnDifferentAttrsCorrect) {
  auto prefix1 = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("2.2.2.0/24");
  auto comm1 = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto comm2 = std::make_pair<uint16_t, uint16_t>(100, 2);

  auto path1 = buildPathWithCommunities({comm1});
  auto entry = createRibEntryWithPaths(prefix1, {path1});

  // Statement A: community 100:1 -> LBW 10
  TBgpCommunityMatch communityMatchA = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityListA = createTCommunityListMatch(
      {communityMatchA}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcherA = createTRibRouteMatcher({}, communityListA);

  TRouteAttributeActions actionsA;
  TRouteAttributeLbwAction lbwActionA;
  lbwActionA.lbw() = 10;
  actionsA.set_lbw() = lbwActionA;

  TRouteAttributeStatement stmtA;
  stmtA.matcher() = matcherA;
  stmtA.actions() = actionsA;

  // Statement B: community 100:2 -> LBW 20
  TBgpCommunityMatch communityMatchB = createTBgpCommunityMatch(
      100, 2, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityListB = createTCommunityListMatch(
      {communityMatchB}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcherB = createTRibRouteMatcher({}, communityListB);

  TRouteAttributeActions actionsB;
  TRouteAttributeLbwAction lbwActionB;
  lbwActionB.lbw() = 20;
  actionsB.set_lbw() = lbwActionB;

  TRouteAttributeStatement stmtB;
  stmtB.matcher() = matcherB;
  stmtB.actions() = actionsB;

  TRouteAttributePolicy policy;
  policy.statements()->emplace("stmtA", stmtA);
  policy.statements()->emplace("stmtB", stmtB);

  RouteAttributePolicy raPolicy(policy);
  RouteAttributePolicy::RibChange change1;

  bool result1 = raPolicy.overwriteRouteAttributes(entry, change1);
  EXPECT_TRUE(result1);
  EXPECT_EQ(entry.getRibPolicyUcmpWeight(), 10);

  // Now churn to different community (different prefix)
  auto path2 = buildPathWithCommunities({comm2});
  auto entry2 = createRibEntryWithPaths(prefix2, {path2});

  RouteAttributePolicy::RibChange change2;
  bool result2 = raPolicy.overwriteRouteAttributes(entry2, change2);
  EXPECT_TRUE(result2);
  EXPECT_EQ(entry2.getRibPolicyUcmpWeight(), 20);
}

// STALENESS: Expiration-only moveIndices
TEST_F(RibPolicyIndexTest, ExpirationOnlyMoveIndices) {
  auto prefix1 = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("2.2.2.0/24");

  auto comm = std::make_pair<uint16_t, uint16_t>(100, 1);
  auto path1 = buildPathWithCommunities({comm});
  auto path2 = buildPathWithCommunities({comm});

  auto entry1 = createRibEntryWithPaths(prefix1, {path1});
  auto entry2 = createRibEntryWithPaths(prefix2, {path2});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 1, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt1;
  stmt1.matcher() = matcher;
  stmt1.actions() = actions;
  stmt1.expiration_time_s() = 9999999999;

  TRouteAttributePolicy policy1;
  policy1.statements()->emplace("stmt1", stmt1);

  RouteAttributePolicy raPolicy1(policy1);
  RouteAttributePolicy::RibChange change1;

  // Apply policy1 to entries, filling the indices
  bool result1 = raPolicy1.overwriteRouteAttributes(entry1, change1);
  bool result2 = raPolicy1.overwriteRouteAttributes(entry2, change1);
  EXPECT_TRUE(result1);
  EXPECT_TRUE(result2);
  EXPECT_EQ(entry1.getRibPolicyUcmpWeight(), 1000);
  EXPECT_EQ(entry2.getRibPolicyUcmpWeight(), 1000);

  // Build policy2 identical to policy1 except later expiration
  TRouteAttributeStatement stmt2;
  stmt2.matcher() = matcher;
  stmt2.actions() = actions;
  stmt2.expiration_time_s() = 9999999999 + 1000; // Later expiration

  TRouteAttributePolicy policy2;
  policy2.statements()->emplace("stmt1", stmt2);

  RouteAttributePolicy raPolicy2(policy2);
  // Move indices from policy1 to policy2
  raPolicy2.moveIndices(raPolicy1);

  // Apply policy2 to the same entries (fresh copies)
  auto entry3 = createRibEntryWithPaths(prefix1, {path1});
  auto entry4 = createRibEntryWithPaths(prefix2, {path2});

  RouteAttributePolicy::RibChange change2;
  bool result3 = raPolicy2.overwriteRouteAttributes(entry3, change2);
  bool result4 = raPolicy2.overwriteRouteAttributes(entry4, change2);

  EXPECT_TRUE(result3);
  EXPECT_TRUE(result4);
  EXPECT_EQ(entry3.getRibPolicyUcmpWeight(), 1000);
  EXPECT_EQ(entry4.getRibPolicyUcmpWeight(), 1000);
}

// SCALE: Many community statements with heavy index reuse
TEST_F(RibPolicyIndexTest, ScaleManyCommunityStatementsIndexReuse) {
  const int numStatements = 16;
  const int numPrefixesPerStatement = 25; // 16 * 25 = 400 routes

  TRouteAttributePolicy policy;

  // Create 16 statements, each with a distinct community and LBW
  for (int i = 0; i < numStatements; ++i) {
    TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
        100, i, routing_policy::MatchValueLogicOperator::EQUAL);
    TCommunityListMatch communityList = createTCommunityListMatch(
        {communityMatch}, routing_policy::BooleanOperator::OR);
    TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

    TRouteAttributeActions actions;
    TRouteAttributeLbwAction lbwAction;
    lbwAction.lbw() = (i + 1) * 10; // LBW 10, 20, 30, ..., 160
    actions.set_lbw() = lbwAction;

    TRouteAttributeStatement stmt;
    stmt.matcher() = matcher;
    stmt.actions() = actions;

    policy.statements()->emplace("stmt" + std::to_string(i), stmt);
  }

  RouteAttributePolicy raPolicy(policy);

  // Build ~400 routes, each carrying one community drawn round-robin
  std::vector<RibEntry> entries;
  for (int i = 0; i < numPrefixesPerStatement * numStatements; ++i) {
    int stmtIdx = i % numStatements;
    auto prefix = folly::IPAddress::createNetwork(
        fmt::format(
            "{}.{}.{}.0/24", 1 + i / 256 / 256, (i / 256) % 256, i % 256));
    auto comm = std::make_pair<uint16_t, uint16_t>(100, stmtIdx);
    auto path = buildPathWithCommunities({comm});
    entries.push_back(createRibEntryWithPaths(prefix, {path}));
  }

  // Apply policy to all entries and verify each gets correct LBW
  RouteAttributePolicy::RibChange change;
  for (int i = 0; i < entries.size(); ++i) {
    int stmtIdx = i % numStatements;
    int expectedLbw = (stmtIdx + 1) * 10;

    bool result = raPolicy.overwriteRouteAttributes(entries[i], change);
    EXPECT_TRUE(result) << "Entry " << i << " failed to match";
    EXPECT_EQ(entries[i].getRibPolicyUcmpWeight(), expectedLbw)
        << "Entry " << i << " got wrong LBW";
  }
}

/*
 * DEFENSIVE: moveIndices() swaps the community index (community-set ->
 * statement NAME) into a new policy on an expiration-only change, assuming the
 * name set is unchanged. If that contract is ever violated, a community-index
 * hit must degrade to no-match instead of throwing std::out_of_range and
 * aborting the RIB walk.
 */
TEST_F(RibPolicyIndexTest, CommunityIndexStaleStatementTreatedAsNoMatch) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto comm = std::make_pair<uint16_t, uint16_t>(100, 200);
  auto path = buildPathWithCommunities({comm});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = matcher;
  stmt.actions() = actions;

  // Old policy names the statement "stmtOld"; populate its community index.
  TRouteAttributePolicy oldPolicyThrift;
  oldPolicyThrift.statements()->emplace("stmtOld", stmt);
  RouteAttributePolicy oldPolicy(oldPolicyThrift);
  auto oldEntry = createRibEntryWithPaths(prefix, {path});
  RouteAttributePolicy::RibChange oldChange;
  ASSERT_TRUE(oldPolicy.overwriteRouteAttributes(oldEntry, oldChange));

  // New policy uses a different name, so "stmtOld" is absent from statements_.
  TRouteAttributePolicy newPolicyThrift;
  newPolicyThrift.statements()->emplace("stmtNew", stmt);
  RouteAttributePolicy newPolicy(newPolicyThrift);

  /*
   * Contract violation: swap in the community index keyed to the now-absent
   * "stmtOld". The new policy's match cache stays empty, so a lookup must go
   * through the swapped-in community index.
   */
  newPolicy.moveIndices(oldPolicy);

  /*
   * Same path -> same interned community-set -> hits the swapped-in index
   * entry.
   */
  auto newEntry = createRibEntryWithPaths(prefix, {path});
  RouteAttributePolicy::RibChange newChange;
  EXPECT_FALSE(newPolicy.overwriteRouteAttributes(newEntry, newChange));
  EXPECT_FALSE(newEntry.getRibPolicyUcmpWeight().has_value());
}

/*
 * DEFENSIVE: moveCache() migrates the prefix match cache (prefix -> statement
 * NAME) alongside moveIndices() on the same expiration-only path. A positive
 * cache hit for a statement absent from the new policy must degrade to no-match
 * instead of throwing std::out_of_range.
 */
TEST_F(RibPolicyIndexTest, MatchCacheStaleStatementTreatedAsNoMatch) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");
  auto comm = std::make_pair<uint16_t, uint16_t>(100, 200);
  auto path = buildPathWithCommunities({comm});

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);
  TRibRouteMatcher matcher = createTRibRouteMatcher({}, communityList);

  TRouteAttributeActions actions;
  TRouteAttributeLbwAction lbwAction;
  lbwAction.lbw() = 1000;
  actions.set_lbw() = lbwAction;

  TRouteAttributeStatement stmt;
  stmt.matcher() = matcher;
  stmt.actions() = actions;

  /*
   * Old policy names the statement "stmtOld"; populate its match cache
   * (prefix -> "stmtOld").
   */
  TRouteAttributePolicy oldPolicyThrift;
  oldPolicyThrift.statements()->emplace("stmtOld", stmt);
  RouteAttributePolicy oldPolicy(oldPolicyThrift);
  auto oldEntry = createRibEntryWithPaths(prefix, {path});
  RouteAttributePolicy::RibChange oldChange;
  ASSERT_TRUE(oldPolicy.overwriteRouteAttributes(oldEntry, oldChange));

  // New policy uses a different name, so "stmtOld" is absent from statements_.
  TRouteAttributePolicy newPolicyThrift;
  newPolicyThrift.statements()->emplace("stmtNew", stmt);
  RouteAttributePolicy newPolicy(newPolicyThrift);

  /*
   * Contract violation: swap in the match cache keyed to the now-absent
   * "stmtOld", forcing the positive cache-hit path.
   */
  newPolicy.moveCache(oldPolicy);

  auto newEntry = createRibEntryWithPaths(prefix, {path});
  RouteAttributePolicy::RibChange newChange;
  EXPECT_FALSE(newPolicy.overwriteRouteAttributes(newEntry, newChange));
  EXPECT_FALSE(newEntry.getRibPolicyUcmpWeight().has_value());
}

TEST_F(RibPolicyIndexTest, RejectsMatcherWithBothPrefixAndCommunity) {
  auto prefix = folly::IPAddress::createNetwork("1.1.1.0/24");

  TBgpCommunityMatch communityMatch = createTBgpCommunityMatch(
      100, 200, routing_policy::MatchValueLogicOperator::EQUAL);
  TCommunityListMatch communityList = createTCommunityListMatch(
      {communityMatch}, routing_policy::BooleanOperator::OR);

  /*
   * A route matcher must carry exactly one matcher; specifying both a prefix
   * set and a community list is rejected -- use two separate statements
   * instead.
   */
  TRibRouteMatcher matcher = createTRibRouteMatcher({prefix}, communityList);
  EXPECT_THROW(RibPolicyRouteMatcher{matcher}, BgpError);
}

} // namespace facebook::bgp
