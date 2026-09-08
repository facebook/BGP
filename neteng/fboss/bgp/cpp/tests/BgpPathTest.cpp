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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/lib/BgpUtil.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook::bgp {
using namespace facebook::nettools::bgplib;

/*
 * This function get a list of BgpPathFields with
 * different optional field unset to test all possiible scenarios
 */
std::vector<std::shared_ptr<facebook::bgp::BgpPathFields>>
getDifferentAttrsFields() {
  std::vector<std::shared_ptr<facebook::bgp::BgpPathFields>> list;
  // default every field is set
  for (int i = 0; i < 2; i++) {
    list.emplace_back(buildBgpPathFields(4, 4, 4, 4));
  }

  auto mutableAttrs = list[1]->attrs.get();
  // unset localPref
  mutableAttrs.localPref = std::nullopt;
  list[1]->attrs = std::move(mutableAttrs);
  return list;
}

TEST(BgpPath, BasicTest) {
  for (auto& attrsFields : getDifferentAttrsFields()) {
    facebook::bgp::BgpPath attrs(*attrsFields);
    EXPECT_FALSE(attrs.isPublished());

    // check all fields
    EXPECT_EQ(attrsFields->attrs->origin, attrs.getOrigin());
    EXPECT_EQ(attrsFields->attrs->asPath, attrs.getAsPath());
    EXPECT_EQ(attrsFields->nexthop, attrs.getNexthop());
    EXPECT_EQ(attrsFields->attrs->med, attrs.getMed());
    EXPECT_EQ(attrsFields->attrs->localPref, attrs.getLocalPref());
    EXPECT_EQ(attrsFields->attrs->atomicAggregate, attrs.getAtomicAggregate());
    EXPECT_EQ(attrsFields->attrs->aggregator, attrs.getAggregator());
    EXPECT_EQ(attrsFields->attrs->originatorId, attrs.getOriginatorId());
    EXPECT_EQ(attrsFields->attrs->clusterList, attrs.getClusterList());
    EXPECT_EQ(attrsFields->attrs->extCommunities, attrs.getExtCommunities());
    EXPECT_EQ(attrsFields->attrs->weight, attrs.getWeight());

    // attrs is writable before publish()
    EXPECT_NO_FATAL_FAILURE(attrs.setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP));
    EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_IGP, attrs.getOrigin());
    // publish attrs
    attrs.publish();
    EXPECT_TRUE(attrs.isPublished());
    // FATAL on change
    EXPECT_DEATH(
        attrs.setOrigin(BgpAttrOrigin::BGP_ORIGIN_EGP),
        "Check failed: !isPublished()");

    // clone will give us a new object
    auto attrClone = attrs.clone();
    EXPECT_FALSE(attrClone->isPublished());
    // check all fields
    EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_IGP, attrClone->getOrigin());
    EXPECT_EQ(attrsFields->attrs->asPath, attrClone->getAsPath());
    EXPECT_EQ(attrsFields->nexthop, attrClone->getNexthop());
    EXPECT_EQ(attrsFields->attrs->med, attrClone->getMed());
    EXPECT_EQ(attrsFields->attrs->localPref, attrClone->getLocalPref());
    EXPECT_EQ(
        attrsFields->attrs->atomicAggregate, attrClone->getAtomicAggregate());
    EXPECT_EQ(attrsFields->attrs->aggregator, attrClone->getAggregator());
    EXPECT_EQ(attrsFields->attrs->originatorId, attrClone->getOriginatorId());
    EXPECT_EQ(attrsFields->attrs->clusterList, attrClone->getClusterList());
    EXPECT_EQ(
        attrsFields->attrs->extCommunities, attrClone->getExtCommunities());
    EXPECT_EQ(attrsFields->attrs->weight, attrClone->getWeight());
  }
}

// Verify by varying different fields different hash values are generated
TEST(BgpPath, VerifyBgpPathHashTest) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  facebook::bgp::BgpPath::Hash(attrHash);
  // Collect all hash values and verify that there was no hash collision
  std::unordered_set<std::size_t> hashValues;
  auto ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);

  // Modify origin only and check
  auto oldOrigin = attrs->getOrigin();
  EXPECT_NE(BgpAttrOrigin::BGP_ORIGIN_IGP, oldOrigin);
  attrs->setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setOrigin(oldOrigin);

  // Modify med only and check
  auto oldMed = attrs->getMed();
  attrs->setMed(oldMed + 1);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setMed(oldMed);

  // Modify local preference and check
  auto oldLocalPref = attrs->getLocalPref();
  attrs->setLocalPref(*oldLocalPref + 1);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setLocalPref(oldLocalPref);

  // Unset localPref and check
  attrs->setLocalPref(std::nullopt);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setLocalPref(oldLocalPref);

  // Modify nexthop and check
  auto oldNexthop = attrs->getNexthop();
  EXPECT_NE(kV4Nexthop2, oldNexthop);
  attrs->setNexthop(kV4Nexthop2);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setNexthop(oldNexthop);

  // Modify originator and check
  auto oldOriginatorId = attrs->getOriginatorId();
  attrs->setOriginatorId(oldOriginatorId + 1);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setOriginatorId(oldOriginatorId);

  // Modify Aggregator and check
  auto oldAggregator = attrs->getAggregator();
  auto newAggregator = oldAggregator;
  newAggregator.asn += 1;
  newAggregator.ip = kAggregatorAddr;
  attrs->setAggregator(newAggregator);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setAggregator(oldAggregator);

  // Add AS-Path elements (Verify lambda's in hash functor)
  BgpAttrAsPathC oldAsPath = *attrs->getAsPath(); // copy
  auto newAsPath = oldAsPath;
  BgpAttrAsPathSegmentC segment1;
  segment1.asSequence.push_back(123);
  newAsPath.push_back(segment1);
  attrs->setAsPath(newAsPath);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);

  // Verifying for more than one segment of asSequence
  BgpAttrAsPathSegmentC segment2;
  segment2.asSequence.push_back(124);
  newAsPath.push_back(segment2);
  attrs->setAsPath(newAsPath);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);

  // Modify asSet
  BgpAttrAsPathSegmentC segment3;
  segment3.asSet.insert(125);
  newAsPath.push_back(segment3);
  attrs->setAsPath(newAsPath);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);

  // Modify asConfedSequence
  BgpAttrAsPathSegmentC segment4;
  segment4.asConfedSequence.push_back(126);
  newAsPath.push_back(segment4);
  attrs->setAsPath(newAsPath);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);

  // Modify asConfedSet
  BgpAttrAsPathSegmentC segment5;
  segment5.asSet.insert(127);
  newAsPath.push_back(segment5);
  attrs->setAsPath(newAsPath);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setAsPath(std::move(oldAsPath));

  // Add a community element (Verify lambda's in hash functor)
  BgpAttrCommunitiesC oldComms = attrs->getCommunities().get();
  auto newComms = oldComms;
  BgpAttrCommunityC comm;
  comm.asn += 1;
  comm.value += 1;
  newComms.push_back(comm);
  attrs->setCommunities(newComms);
  hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);

  // Add an ext community element
  BgpAttrExtCommunitiesC oldExtComms = attrs->getExtCommunities().get();
  auto newExtComms = oldExtComms;
  facebook::nettools::bgplib::BgpAttrExtCommunityC extComm(0x2272a, 0x232f);
  newExtComms.push_back(extComm);
  attrs->setExtCommunities(newExtComms);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);

  // Modify weight only and check
  auto oldWeight = attrs->getWeight();
  attrs->setWeight(oldWeight + 1);
  ret = hashValues.insert(attrHash(attrs));
  EXPECT_TRUE(ret.second);
  attrs->setWeight(oldWeight);
}

// Verify BGP attributes compare function
TEST(BgpPath, verifyBgpPathCompare) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs1 = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  auto attrs2 = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  EXPECT_EQ(*attrs1, *attrs2);

  // Verify that IPv4 nexthop != IPv6 mapped IPv4 nexthop
  attrs1->setNexthop(kV4Nexthop1);
  attrs2->setNexthop(folly::IPAddress::createIPv6(kV4Nexthop1));
  EXPECT_NE(*attrs1, *attrs2);

  // Verify compare function
  facebook::bgp::BgpPath::Compare(compare);
  EXPECT_FALSE(compare(attrs1, attrs2));
}

TEST(BgpPath, VerifyBgpPathCompareTopologyInfoTest) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs1 = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  auto attrs2 = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  EXPECT_EQ(*attrs1, *attrs2);
  facebook::bgp::BgpPath::Compare(compare);
  EXPECT_TRUE(compare(attrs1, attrs2));

  /*
   * Verify std::nullopt is the default value of topologyInfo field in
   * BgpPath
   */
  attrs2->setTopologyInfo(std::nullopt);
  EXPECT_EQ(*attrs1, *attrs2);
  EXPECT_TRUE(compare(attrs1, attrs2));

  // Update topologyInfo field in attrs2
  std::unordered_map<std::string, int64_t> topologyInfo;
  attrs2->setTopologyInfo(topologyInfo);
  EXPECT_NE(*attrs1, *attrs2);
  EXPECT_FALSE(compare(attrs1, attrs2));
}

TEST(BgpPath, verifyBgpAsPathLenWithConfed) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  {
    std::vector<BgpAttrAsPathSegmentC> asPath{
        {{kAsn1, kAsn2}, {}, {}, {}}, // segment with asSet
        {{}, {kAsn3, kAsn3, kAsn3}, {}, {}}, // segment with asSequence
        {{}, {}, {kAsn4, kAsn4}, {}}, // segment with asConfedSequence
        {{}, {}, {}, {kAsn5, kAsn6}}}; // segment with asConfedSet
    attrs->setAsPath(static_cast<BgpAttrAsPathC>(asPath));
    // 1 (asSet) + 3 (asSequence) + 2 (asConfedSequence) + 1 (asConfedSet)
    EXPECT_EQ(7, attrs->getBgpAsPathLenWithConfed());
  }
  {
    std::vector<BgpAttrAsPathSegmentC> asPath{
        {{kAsn1, kAsn2}, {}, {}, {}}, // segment with asSet
        {{}, {}, {}, {kAsn5, kAsn6}}}; // segment with asConfedSet
    attrs->setAsPath(static_cast<BgpAttrAsPathC>(asPath));
    // 1 (asSet) + 1 (asConfedSet)
    EXPECT_EQ(2, attrs->getBgpAsPathLenWithConfed());
  }
}

TEST(BgpPath, setLbwComm) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  {
    // There should be no extCommunities to begin with
    EXPECT_TRUE(attrs->getExtCommunities().nullOrEmpty());
    EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_FALSE(attrs->getNonTransitiveLbw().has_value());

    // Set LBW Ext community and verify the results
    attrs->setNonTransitiveLbwExtCommunity(uint16_t(kLocalAs1), kLbw10G);
    EXPECT_EQ(1, attrs->getExtCommunities()->size());
    EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_EQ(uint16_t(kLocalAs1), attrs->getNonTransitiveLbwAsn().value());
    EXPECT_EQ(kLbw10G, attrs->getNonTransitiveLbwValue().value());
    {
      auto lbw = attrs->getNonTransitiveLbw();
      EXPECT_TRUE(lbw.has_value());
      EXPECT_EQ(uint16_t(kLocalAs1), lbw->first);
      EXPECT_EQ(kLbw10G, lbw->second);
    }

    /*
     * Set a different LBW community.  This should result in replacing the
     * previous one with the new one
     */
    attrs->setNonTransitiveLbwExtCommunity(uint16_t(kLocalAs2), kLbw100G);
    EXPECT_EQ(1, attrs->getExtCommunities()->size());
    EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_EQ(uint16_t(kLocalAs2), attrs->getNonTransitiveLbwAsn().value());
    EXPECT_EQ(kLbw100G, attrs->getNonTransitiveLbwValue().value());
    {
      auto lbw = attrs->getNonTransitiveLbw();
      EXPECT_TRUE(lbw.has_value());
      EXPECT_EQ(uint16_t(kLocalAs2), lbw->first);
      EXPECT_EQ(kLbw100G, lbw->second);
    }

    /*
     * Prune the LBW community and make sure we are back with no ext
     * communities
     */
    attrs->pruneNonTransitiveLbwExtCommunity();
    EXPECT_TRUE(attrs->getExtCommunities().nullOrEmpty());
    EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_FALSE(attrs->getNonTransitiveLbw().has_value());
  }
}

TEST(BgpPath, setRawLbwComm) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  {
    // There should be no extCommunities to begin with
    EXPECT_TRUE(attrs->getExtCommunities().nullOrEmpty());
    EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_FALSE(attrs->getNonTransitiveLbw().has_value());

    // Set encoded LBW Ext community and verify the results
    attrs->setNonTransitiveRawLbwExtCommunity(uint16_t(kLocalAs1), kEncodedLbw);
    EXPECT_EQ(1, attrs->getExtCommunities()->size());
    EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_EQ(uint16_t(kLocalAs1), attrs->getNonTransitiveLbwAsn().value());
    EXPECT_EQ(kEncodedLbw, attrs->getNonTransitiveRawLbwValue().value());
    {
      auto encodedLbw = attrs->getNonTransitiveRawLbw();
      EXPECT_TRUE(encodedLbw.has_value());
      EXPECT_EQ(uint16_t(kLocalAs1), encodedLbw->first);
      EXPECT_EQ(kEncodedLbw, encodedLbw->second);
    }

    /*
     * Set a different encoded LBW community.  This should result in replacing
     * the previous one with the new one
     */
    attrs->setNonTransitiveRawLbwExtCommunity(uint16_t(kLocalAs2), UINT32_MAX);
    EXPECT_EQ(1, attrs->getExtCommunities()->size());
    EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_EQ(uint16_t(kLocalAs2), attrs->getNonTransitiveLbwAsn().value());
    EXPECT_EQ(UINT32_MAX, attrs->getNonTransitiveRawLbwValue().value());
    {
      auto encodedLbw = attrs->getNonTransitiveRawLbw();
      EXPECT_TRUE(encodedLbw.has_value());
      EXPECT_EQ(uint16_t(kLocalAs2), encodedLbw->first);
      EXPECT_EQ(UINT32_MAX, encodedLbw->second);
    }

    /*
     * Prune the LBW community and make sure we are back with no ext
     * communities
     */
    attrs->pruneNonTransitiveLbwExtCommunity();
    EXPECT_TRUE(attrs->getExtCommunities().nullOrEmpty());
    EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_FALSE(attrs->getNonTransitiveLbw().has_value());
  }
}

/*
 * We wanna verify if we can convert encoded lbw (uint32_t) to regular lbw and
 * vice versa without data corruption
 * Reason we want to test this is to find out whether we can reuse the existing
 * ACCEPT action for regular lbw for encoded lbw
 */
TEST(BgpPath, lbwCommTypeConversionTest) {
  // getNonTransitiveRawLbwValue() has no conversion, so it's the SoT

  /*
   * confirm union trick is reversible
   * also confirm float -> uint32_t with static_cast doesn't work
   */
  {
    auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

    // initialize LBW Ext community with 0xffffffff
    attrs->setNonTransitiveRawLbwExtCommunity(uint16_t(kLocalAs1), UINT32_MAX);
    // get float lbw using union trick
    auto lbw = attrs->getNonTransitiveLbwValue();

    /*
     if we use static_cast to get uint32_t, it won't work:
     auto rawLbw = static_cast<uint32_t>(lbw.value());
     runtime error: -nan is outside the range of representable values of type
     'unsigned int'
    */

    /*
     * but if we write this float lbw back to ext community using
     * union trick, the value is correct
     */
    attrs->setNonTransitiveLbwExtCommunity(uint16_t(kLocalAs1), lbw.value());
    EXPECT_EQ(UINT32_MAX, attrs->getNonTransitiveRawLbwValue().value());
  }
  // confirm uint32_t -> float through static_cast loses data too
  {
    auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

    auto lbw = static_cast<float>(UINT32_MAX);
    // Set ext community with the static casted lbw
    attrs->setNonTransitiveLbwExtCommunity(uint16_t(kLocalAs1), lbw);
    // confirm that the uint32_t is NOT the one we want to set
    EXPECT_NE(UINT32_MAX, attrs->getNonTransitiveRawLbwValue().value());
  }
}

// Verify logic that removes "transitive" lbw from ext community
TEST(BgpPath, pruneUnnecessaryComm) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);
  {
    // There should be no extCommunities to begin with
    EXPECT_TRUE(attrs->getExtCommunities().nullOrEmpty());
    EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_FALSE(attrs->getNonTransitiveLbw().has_value());

    // Set LBW Ext community and verify the results
    attrs->setNonTransitiveLbwExtCommunity(uint16_t(kLocalAs1), kLbw10G);
    EXPECT_EQ(1, attrs->getExtCommunities()->size());
    EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_EQ(uint16_t(kLocalAs1), attrs->getNonTransitiveLbwAsn().value());
    EXPECT_EQ(kLbw10G, attrs->getNonTransitiveLbwValue().value());
    {
      auto lbw = attrs->getNonTransitiveLbw();
      EXPECT_TRUE(lbw.has_value());
      EXPECT_EQ(uint16_t(kLocalAs1), lbw->first);
      EXPECT_EQ(kLbw10G, lbw->second);
    }

    // Set "transitive" LBW Ext community
    {
      auto extCommunities = attrs->getExtCommunities().get();
      auto lbw = extCommunities.at(0);
      // get the correct values from lbw
      auto [rawHighVal, rawLowVal] = lbw.getRawValueInWords();
      // flip the type from non-transitive to transitive aka 0x40 to 0x00
      EXPECT_EQ(
          BgpExtCommunityAsSpecificExtTypeC::kBgpExtCommASNonTransitiveType,
          lbw.attr->getType());
      uint32_t newHighVal =
          (BgpExtCommunityAsSpecificExtTypeC::kBgpExtCommASTransitiveType
           << 24) +
          (rawHighVal - (lbw.attr->getType() << 24));
      BgpAttrExtCommunityC transitiveLBW{newHighVal, rawLowVal};
      extCommunities.emplace_back(transitiveLBW);
      attrs->setExtCommunities(extCommunities);
      EXPECT_EQ(2, attrs->getExtCommunities()->size());
      EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    }

    /*
     * Prune the LBW community would only prune the correct lbw
     * and leave "transitive" lbw community untouched
     */
    attrs->pruneNonTransitiveLbwExtCommunity();
    EXPECT_EQ(1, attrs->getExtCommunities()->size());
    EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_FALSE(attrs->getNonTransitiveLbw().has_value());

    // Prune invalid community will do the ultimate cleanup
    attrs->pruneTransitiveLbwExtCommunity();
    EXPECT_TRUE(attrs->getExtCommunities().nullOrEmpty());
    EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_FALSE(attrs->getNonTransitiveLbw().has_value());
  }
}

/*
 * Verify getNonTransitiveLbwExtCommunity returns the lowest LBW value
 * when multiple LBW ext communities are present
 */
TEST(BgpPath, getLowestLbwExtCommunity) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

  // There should be no extCommunities to begin with
  EXPECT_TRUE(attrs->getExtCommunities().nullOrEmpty());
  EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());

  /*
   * Manually add multiple LBW ext communities with different bandwidth values
   * We can't use setNonTransitiveLbwExtCommunity since it prunes existing ones
   */
  BgpAttrExtCommunitiesC extCommunities;

  // Add LBW with 100G bandwidth (higher value)
  extCommunities.emplace_back(
      BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs1), kLbw100G));

  // Add LBW with 5G bandwidth (lower value)
  extCommunities.emplace_back(
      BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs2), kLbw5G));

  // Add LBW with 10G bandwidth (middle value)
  extCommunities.emplace_back(
      BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs3), kLbw10G));

  attrs->setExtCommunities(std::move(extCommunities));

  // Verify all three LBW communities are present
  EXPECT_EQ(3, attrs->getExtCommunities()->size());
  EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());

  // getNonTransitiveLbwValue should return the lowest value (5G)
  EXPECT_EQ(kLbw5G, attrs->getNonTransitiveLbwValue().value());

  // getNonTransitiveLbwAsn should return the ASN associated with lowest LBW
  EXPECT_EQ(uint16_t(kLocalAs2), attrs->getNonTransitiveLbwAsn().value());

  // getNonTransitiveLbw should return both ASN and lowest LBW value
  auto lbw = attrs->getNonTransitiveLbw();
  EXPECT_TRUE(lbw.has_value());
  EXPECT_EQ(uint16_t(kLocalAs2), lbw->first);
  EXPECT_EQ(kLbw5G, lbw->second);
}

// Verify getNonTransitiveLbwExtCommunity handles edge cases correctly
TEST(BgpPath, getLowestLbwExtCommunityEdgeCases) {
  // Test case 1: Single LBW community should just return that one
  {
    auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

    attrs->setNonTransitiveLbwExtCommunity(uint16_t(kLocalAs1), kLbw10G);
    EXPECT_EQ(1, attrs->getExtCommunities()->size());
    EXPECT_EQ(kLbw10G, attrs->getNonTransitiveLbwValue().value());
    EXPECT_EQ(uint16_t(kLocalAs1), attrs->getNonTransitiveLbwAsn().value());
  }

  // Test case 2: Multiple LBW communities with same value should return one
  {
    auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

    BgpAttrExtCommunitiesC extCommunities;
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs1), kLbw10G));
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs2), kLbw10G));
    attrs->setExtCommunities(std::move(extCommunities));

    EXPECT_EQ(2, attrs->getExtCommunities()->size());
    EXPECT_EQ(kLbw10G, attrs->getNonTransitiveLbwValue().value());
  }

  // Test case 3: Zero bandwidth should be selected as lowest
  {
    auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

    BgpAttrExtCommunitiesC extCommunities;
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs1), kLbw10G));
    extCommunities.emplace_back(BgpExtCommunityLinkBandWidthTypeC(
        uint16_t(kLocalAs2), 0.0f)); // zero bandwidth
    attrs->setExtCommunities(std::move(extCommunities));

    EXPECT_EQ(2, attrs->getExtCommunities()->size());
    EXPECT_EQ(0.0f, attrs->getNonTransitiveLbwValue().value());
    EXPECT_EQ(uint16_t(kLocalAs2), attrs->getNonTransitiveLbwAsn().value());
  }

  // Test case 4: Mixed ext communities (non-LBW and LBW)
  {
    auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

    BgpAttrExtCommunitiesC extCommunities;
    // Add a non-LBW ext community first
    extCommunities.emplace_back(BgpAttrExtCommunityC(
        kExtCommASTypeFirstWord, kExtCommASTypeSecondWord));
    // Add LBW with 100G
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs1), kLbw100G));
    // Add another non-LBW ext community
    extCommunities.emplace_back(BgpAttrExtCommunityC(
        kExtCommRegularTypeFirstWord, kExtCommRegularTypeSecondWord));
    // Add LBW with 5G (lowest)
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs2), kLbw5G));
    attrs->setExtCommunities(std::move(extCommunities));

    EXPECT_EQ(4, attrs->getExtCommunities()->size());
    EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    EXPECT_EQ(kLbw5G, attrs->getNonTransitiveLbwValue().value());
    EXPECT_EQ(uint16_t(kLocalAs2), attrs->getNonTransitiveLbwAsn().value());
  }

  // Test case 5: Negative LBW values should be ignored
  {
    auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

    BgpAttrExtCommunitiesC extCommunities;
    // Add LBW with negative bandwidth (should be ignored)
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs1), -10.0f));
    // Add LBW with 10G (should be selected as lowest valid)
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs2), kLbw10G));
    // Add another negative LBW (should be ignored)
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs3), -1.0f));
    attrs->setExtCommunities(std::move(extCommunities));

    EXPECT_EQ(3, attrs->getExtCommunities()->size());
    EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    // Only the positive 10G value should be returned
    EXPECT_EQ(kLbw10G, attrs->getNonTransitiveLbwValue().value());
    EXPECT_EQ(uint16_t(kLocalAs2), attrs->getNonTransitiveLbwAsn().value());
  }

  // Test case 6: All negative LBW values should return nullopt
  {
    auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

    BgpAttrExtCommunitiesC extCommunities;
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs1), -10.0f));
    extCommunities.emplace_back(
        BgpExtCommunityLinkBandWidthTypeC(uint16_t(kLocalAs2), -5.0f));
    attrs->setExtCommunities(std::move(extCommunities));

    EXPECT_EQ(2, attrs->getExtCommunities()->size());
    // hasNonTransitiveLbwExtCommunity checks for presence, not validity
    EXPECT_TRUE(attrs->hasNonTransitiveLbwExtCommunity());
    // All public accessors should return nullopt since all LBW are negative
    EXPECT_FALSE(attrs->getNonTransitiveLbwValue().has_value());
    EXPECT_FALSE(attrs->getNonTransitiveLbwAsn().has_value());
    EXPECT_FALSE(attrs->getNonTransitiveLbw().has_value());
  }
}

// Verify BgpPath COW is converted to BgpUpdate2 properly
TEST(BgpPath, BgpPathtoBgpUpdate2) {
  // Sample update with all fields filled in
  BgpUpdate2 updateGiven;
  *updateGiven.attrs()->origin() = BgpAttrOrigin::BGP_ORIGIN_IGP;
  BgpAttrAsPathSegment segment1;
  segment1.asSequence()->push_back(32934);
  updateGiven.attrs()->asPath()->push_back(segment1);
  BgpAttrAsPathSegment segment2;
  segment2.asConfedSequence()->push_back(1234);
  updateGiven.attrs()->asPath()->push_back(segment2);
  BgpAttrAsPathSegment segment3;
  segment3.asSet()->emplace(456);
  updateGiven.attrs()->asPath()->push_back(segment3);
  BgpAttrAsPathSegment segment4;
  segment4.asConfedSet()->emplace(789);
  updateGiven.attrs()->asPath()->push_back(segment2);
  *updateGiven.attrs()->nexthop() = "1.2.3.4";
  updateGiven.attrs()->med() = 32;
  updateGiven.attrs()->isMedSet() = true;
  *updateGiven.attrs()->atomicAggregate() = true;
  *updateGiven.attrs()->aggregator()->asn() = 4660;
  *updateGiven.attrs()->aggregator()->ip() = "3.4.5.6";
  BgpAttrCommunity community;
  *community.asn() = 65530;
  *community.value() = 15800;
  updateGiven.attrs()->communities()->push_back(community);
  *updateGiven.attrs()->originatorId() = 0x86070000; // ip: "134.7.0.0""
  updateGiven.attrs()->clusterList()->push_back(0x10010000); // ip: "16.1.0.0"
  updateGiven.attrs()->clusterList()->push_back(0x86070000); // ip: "134.7.0.0"
  BgpAttrExtCommunity extCommunity;
  *extCommunity.firstWord() = 0x2272a;
  *extCommunity.secondWord() = 0x232f;
  updateGiven.attrs()->extCommunities()->push_back(extCommunity);

  // test with optional fields set/unset
  {
    // default attrs
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(
        facebook::bgp::BgpPathFields(*BgpUpdate2toBgpPathC(updateGiven)));
    auto updateReturned = attrs->getBgpUpdate2();
    EXPECT_EQ(*updateGiven.attrs(), *updateReturned->attrs());
  }
  {
    auto updateGivenCopy = updateGiven;
    // set localPref
    updateGivenCopy.attrs()->localPref() = 100;
    // Converting BgpUpdate2 to BgpPath
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(
        facebook::bgp::BgpPathFields(*BgpUpdate2toBgpPathC(updateGivenCopy)));
    // Convert BgpPath to BgpUpdate2
    auto updateReturned = attrs->getBgpUpdate2();
    // Verifying all attributes match the input after converting and back
    EXPECT_EQ(*updateGivenCopy.attrs(), *updateReturned->attrs());
  }
}

TEST(BgpPath, getNonTransitiveLbwExtCommunityEmpty) {
  auto attrsFields = buildBgpPathFields(1, 1, 0, 0);
  auto attrs = std::make_shared<facebook::bgp::BgpPath>(*attrsFields);

  // There should be no extCommunities to begin with
  EXPECT_TRUE(attrs->getExtCommunities().nullOrEmpty());
  EXPECT_FALSE(attrs->hasNonTransitiveLbwExtCommunity());

  // This should return nullptr when there are no LBW ext communities
  auto lbwCommunity = attrs->getNonTransitiveLbwExtCommunity();
  EXPECT_EQ(lbwCommunity, nullptr);

  // Verify that std::dynamic_pointer_cast of nullptr returns nullptr
  std::shared_ptr<BgpExtCommunityBaseTypeC> nullPtr = nullptr;
  auto castedNull =
      std::dynamic_pointer_cast<BgpExtCommunityLinkBandWidthTypeC>(nullPtr);
  EXPECT_EQ(castedNull, nullptr);
}
} // namespace facebook::bgp
