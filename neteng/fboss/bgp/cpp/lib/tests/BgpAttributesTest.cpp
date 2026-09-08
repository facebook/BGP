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

#include <folly/init/Init.h>
#include "neteng/fboss/bgp/cpp/lib/BgpAttributesSmartSet.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"

namespace facebook {
namespace nettools {
namespace bgplib {

TEST(BgpAttributesSmartSet, BgpAttributeStrTest) {
  const uint16_t asn{65527};
  const uint16_t value{10};
  const uint32_t localPref{100};

  bgplib::BgpAttrCommunityC attrCommunity{asn, value};

  bgplib::BgpAttributesC attrs;
  // localPref is an optional field
  attrs.localPref = 100;
  attrs.communities = bgplib::BgpAttrCommunitiesC{{attrCommunity}};

  const auto str1 =
      fmt::format("origin: BGP_ORIGIN_INCOMPLETE localPref: {} ", localPref);
  const auto str2 = "atomicAggregate: false aggregator: asn 0 ip  ";
  const auto str3 =
      fmt::format("communities: {}:{} originatorId: 0 ", asn, value);
  const auto str4 = "weight: 0 ";
  EXPECT_EQ(attrs.str(), fmt::format("{}{}{}{}", str1, str2, str3, str4));
}

TEST(BgpAttributesSmartSet, BgpAttributeStrTestWithMed) {
  const uint16_t asn{65527};
  const uint16_t value{10};
  const uint32_t localPref{100};
  const uint32_t med{100};

  bgplib::BgpAttrCommunityC attrCommunity{asn, value};

  bgplib::BgpAttributesC attrs;
  // localPref is an optional field
  attrs.localPref = 100;
  attrs.communities = bgplib::BgpAttrCommunitiesC{{attrCommunity}};
  attrs.med = 100;
  attrs.isMedSet = true;

  const auto str0 = "origin: BGP_ORIGIN_INCOMPLETE ";
  const auto str1 = fmt::format("med: {} ", med);
  const auto str2 = fmt::format("localPref: {} ", localPref);
  const auto str3 = "atomicAggregate: false aggregator: asn 0 ip  ";
  const auto str4 =
      fmt::format("communities: {}:{} originatorId: 0 ", asn, value);
  const auto str5 = "weight: 0 ";
  EXPECT_EQ(
      attrs.str(),
      fmt::format("{}{}{}{}{}{}", str0, str1, str2, str3, str4, str5));
}

TEST(BgpAttributesSmartSet, BgpAttributeStrTestWitHWeight) {
  const uint16_t asn{65527};
  const uint16_t value{10};
  const uint32_t localPref{100};
  const uint32_t weight{100};

  bgplib::BgpAttrCommunityC attrCommunity{asn, value};

  bgplib::BgpAttributesC attrs;
  // localPref is an optional field
  attrs.localPref = 100;
  attrs.communities = bgplib::BgpAttrCommunitiesC{{attrCommunity}};
  attrs.weight = 100;

  const auto str0 = "origin: BGP_ORIGIN_INCOMPLETE ";
  const auto str1 = fmt::format("localPref: {} ", localPref);
  const auto str2 = "atomicAggregate: false aggregator: asn 0 ip  ";
  const auto str3 =
      fmt::format("communities: {}:{} originatorId: 0 ", asn, value);
  const auto str4 = fmt::format("weight: {} ", weight);

  EXPECT_EQ(
      attrs.str(), fmt::format("{}{}{}{}{}", str0, str1, str2, str3, str4));
}

TEST(BgpAttributesSmartSet, AddRemoveTest) {
  BgpAttributesSmartSet attrSet;

  /*
   * bgplib::BgpAttributes has many fields.
   * Only set med for simplicity.
   */
  bgplib::BgpAttributes attrsA;
  attrsA.med() = 10;
  attrsA.isMedSet() = true;
  bgplib::BgpAttributes attrsB;
  attrsB.med() = 20;
  attrsB.isMedSet() = true;
  bgplib::BgpAttributes attrsC;
  attrsC.med() = 30;
  attrsC.isMedSet() = true;

  {
    /*
     * Add attrsA
     * {A} in set
     */
    auto attrsARef = attrSet.addEntry(attrsA);
    EXPECT_EQ(attrSet.size(), 1);
    EXPECT_TRUE(attrSet.containsEntry(attrsA));
    EXPECT_EQ(*attrsARef.get().med(), 10);

    {
      /*
       * Add attrsB
       * {A, B} in set
       */
      auto attrsBRef = attrSet.addEntry(attrsB);
      EXPECT_EQ(attrSet.size(), 2);
      EXPECT_TRUE(attrSet.containsEntry(attrsA));
      EXPECT_TRUE(attrSet.containsEntry(attrsB));
      EXPECT_EQ(*attrsARef.get().med(), 10);
      EXPECT_EQ(*attrsBRef.get().med(), 20);

      {
        /*
         * Add attrsA (again) and attrsC
         * attrsA already in set, the hash function should handle duplication
         * {A, B, C} in set
         */
        auto attrsARef2 = attrSet.addEntry(attrsA);
        auto attrsCRef = attrSet.addEntry(attrsC);
        EXPECT_EQ(attrSet.size(), 3);
        EXPECT_TRUE(attrSet.containsEntry(attrsA));
        EXPECT_TRUE(attrSet.containsEntry(attrsB));
        EXPECT_TRUE(attrSet.containsEntry(attrsC));
        EXPECT_EQ(*attrsARef.get().med(), 10);
        EXPECT_EQ(*attrsARef2.get().med(), 10);
        EXPECT_EQ(*attrsBRef.get().med(), 20);
        EXPECT_EQ(*attrsCRef.get().med(), 30);
      }

      /*
       * Out of the scope of attrsARef2 and attrsCRef
       * attrsCRef's destructor removed attrsC from the set
       * attrsA still in set because attrsARef still holds it
       * {A, B} in set
       */
      EXPECT_EQ(attrSet.size(), 2);
      EXPECT_TRUE(attrSet.containsEntry(attrsA));
      EXPECT_TRUE(attrSet.containsEntry(attrsB));
      EXPECT_EQ(*attrsARef.get().med(), 10);
      EXPECT_EQ(*attrsBRef.get().med(), 20);
    }

    /*
     * Out of the scope of attrsBRef
     * attrsBRef's destructor removed attrsB from the set
     * {A} in set
     */
    EXPECT_EQ(attrSet.size(), 1);
    EXPECT_TRUE(attrSet.containsEntry(attrsA));
    EXPECT_EQ(*attrsARef.get().med(), 10);
  }

  /*
   * Out of the scope of attrsARef
   * attrsARef's destructor removed attrsA from the set
   * {} in set
   */
  EXPECT_EQ(attrSet.size(), 0);
}

TEST(BgpAttributesC, HashTest) {
  BgpAttributesC attrs;

  {
    BgpAttributesC attrsSame;

    EXPECT_EQ(attrs.hash(), attrsSame.hash());
  }

  // Test each field
  {
    BgpAttributesC attrsDifferent{
        .origin = nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttrAsPathC asPath{{BgpAttrAsPathSegmentC{.asSequence{1}}}};
    BgpAttributesC attrsDifferent{.asPath{std::move(asPath)}};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttributesC attrsDifferent{.med = 1};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttributesC attrsDifferent{.localPref = 200};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttributesC attrsDifferent{.atomicAggregate = true};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttributesC attrsDifferent{.aggregator = {.asn = 100}};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttrCommunitiesC communities{{BgpAttrCommunityC{1, 2}}};
    BgpAttributesC attrsDifferent{.communities{std::move(communities)}};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttributesC attrsDifferent{.originatorId = 100};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttrClusterListC clusterList{{1, 2}};
    BgpAttributesC attrsDifferent{.clusterList{std::move(clusterList)}};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }

  {
    BgpAttrExtCommunitiesC extCommunities{{BgpAttrExtCommunityC{1, 2}}};
    BgpAttributesC attrsDifferent{.extCommunities{std::move(extCommunities)}};

    EXPECT_NE(attrs.hash(), attrsDifferent.hash());
  }
}

TEST(BgpPathC, TopoInfoToStrTest) {
  std::unordered_map<std::string, int64_t> topoInfo;
  EXPECT_EQ(BgpPathC::topoInfoToStr(topoInfo), "topologyInfo: [] ");

  topoInfo.emplace("rack_id", 1);
  EXPECT_EQ(BgpPathC::topoInfoToStr(topoInfo), "topologyInfo: [rack_id:1] ");

  topoInfo.emplace("plane_id", 2);
  EXPECT_EQ(
      BgpPathC::topoInfoToStr(topoInfo),
      "topologyInfo: [plane_id:2,rack_id:1] ");
}

TEST(BgpAttrAsPathC, HashTest) {
  BgpAttrAsPathC asPath1{{BgpAttrAsPathSegmentC{.asSequence{1}}}};
  BgpAttrAsPathC asPath2{{BgpAttrAsPathSegmentC{.asSet{1}}}};
  EXPECT_NE(asPath1.hash(), asPath2.hash());

  BgpAttrAsPathC asPath3{{BgpAttrAsPathSegmentC{.asSequence{1}}}};
  EXPECT_EQ(asPath1.hash(), asPath3.hash());
}

TEST(DeDuplicatedAsPath, EmptyPointerTest) {
  // default initialized to nullptr
  DeDuplicatedAsPath asPath1;
  // empty as path would also lead to nullptr
  DeDuplicatedAsPath asPath2{BgpAttrAsPathC{}};
  // explicitly setting to nullptr
  DeDuplicatedAsPath asPath3{nullptr};

  EXPECT_EQ(asPath1, asPath2);
  EXPECT_EQ(asPath1, asPath3);
}

TEST(BgpAttrAggregatorC, HashTest) {
  BgpAttrAggregatorC aggregator1{.asn = 1, .ip = folly::IPAddress{"1.1.1.1"}};
  BgpAttrAggregatorC aggregator2{.asn = 10, .ip = folly::IPAddress{"1.1.1.1"}};
  BgpAttrAggregatorC aggregator3{.asn = 1, .ip = folly::IPAddress{"1.1.1.2"}};

  EXPECT_NE(aggregator1.hash(), aggregator2.hash());
  EXPECT_NE(aggregator1.hash(), aggregator3.hash());
}

TEST(BgpAttrCommunityC, HashTest) {
  BgpAttrCommunityC community1{1, 2};
  BgpAttrCommunityC community2{4, 3};

  EXPECT_NE(community1.hash(), community2.hash());

  BgpAttrCommunityC community3{1, 2};

  EXPECT_EQ(community1.hash(), community3.hash());
}

TEST(BgpAttrCommunitiesC, HashTest) {
  BgpAttrCommunityC community1{1, 2};
  BgpAttrCommunityC community2{4, 3};

  BgpAttrCommunitiesC communities1{{community1}};
  BgpAttrCommunitiesC communities2{{community1, community2}};

  EXPECT_NE(communities1.hash(), communities2.hash());

  BgpAttrCommunitiesC communities3{{community1}};
  EXPECT_EQ(communities1.hash(), communities3.hash());
}

TEST(DeDuplicatedCommunities, EmptyPointerTest) {
  // default initialized to nullptr
  DeDuplicatedCommunities communities1;
  // empty as path would also lead to nullptr
  DeDuplicatedCommunities communities2{BgpAttrCommunitiesC{}};
  // explicitly setting to nullptr
  DeDuplicatedCommunities communities3{nullptr};

  EXPECT_EQ(communities1, communities2);
  EXPECT_EQ(communities1, communities3);
}

TEST(DeDuplicatedCommunities, SortedCommunitiesTest) {
  BgpAttrCommunityC community1{1, 2};
  BgpAttrCommunityC community2{4, 3};
  BgpAttrCommunityC community3{1, 4};

  DeDuplicatedCommunities communities1{
      BgpAttrCommunitiesC{{community1, community2}}};
  DeDuplicatedCommunities communities2{
      BgpAttrCommunitiesC{{community2, community1}}};
  DeDuplicatedCommunities communities3{
      BgpAttrCommunitiesC{{community1, community2, community3}}};

  EXPECT_EQ(communities1, communities2);
  EXPECT_NE(communities1, communities3);
}

TEST(BgpAttrClusterListC, HashTest) {
  BgpAttrClusterListC clusterList1{{1, 2}};
  BgpAttrClusterListC clusterList2{{4, 3}};

  EXPECT_NE(clusterList1.hash(), clusterList2.hash());

  BgpAttrClusterListC clusterList3{{1, 2}};
  EXPECT_EQ(clusterList1.hash(), clusterList3.hash());
}

TEST(DeDuplicatedClusterList, NotSortedClusterListTest) {
  DeDuplicatedClusterList clusterList1{BgpAttrClusterListC{{1, 2}}};
  DeDuplicatedClusterList clusterList2{BgpAttrClusterListC{{4, 3}}};
  DeDuplicatedClusterList clusterList3{BgpAttrClusterListC{{2, 1}}};

  EXPECT_NE(clusterList1, clusterList2);
  EXPECT_NE(clusterList1, clusterList3);
}

TEST(BgpAttrExtCommunityC, HashTest) {
  BgpAttrExtCommunityC community1{1, 2};
  BgpAttrExtCommunityC community2{4, 3};

  EXPECT_NE(community1.hash(), community2.hash());
}

TEST(BgpAttrExtCommunityC, LessOperatorTest) {
  BgpAttrExtCommunityC community1{1, 2};
  BgpAttrExtCommunityC community2{4, 3};

  EXPECT_LT(community1, community2);
}

TEST(BgpAttrExtCommunitiesC, HashTest) {
  BgpAttrExtCommunityC community1{1, 2};
  BgpAttrExtCommunityC community2{4, 3};

  BgpAttrExtCommunitiesC communities1{{community1}};
  BgpAttrExtCommunitiesC communities2{{community1, community2}};

  EXPECT_NE(communities1.hash(), communities2.hash());

  BgpAttrExtCommunitiesC communities3{{community1}};
  EXPECT_EQ(communities1.hash(), communities3.hash());
}

} // namespace bgplib
} // namespace nettools
} // namespace facebook

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // run the unittests
  return RUN_ALL_TESTS();
}
