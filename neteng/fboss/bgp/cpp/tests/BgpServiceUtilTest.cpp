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

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

using namespace ::testing;
using namespace neteng::fboss::bgp::thrift;
using namespace nettools::bgplib;
using namespace neteng::fboss::bgp_attr;

class BgpServiceUtilTest : public ::testing::Test {};

/**
 * Check if all the fields in BgpPathC are included in TBgpPath
 */
TEST_F(BgpServiceUtilTest, BasicTest) {
  // Build a BgpPath
  auto attr =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(4, 4, 4, 4));
  attr->setAtomicAggregate(true);

  BgpAttrAggregatorC aggregator;
  aggregator.asn = 1234567;
  aggregator.ip = folly::IPAddress("10.1.2.126");
  attr->setAggregator(aggregator);

  const TBgpPath& path = createTBgpPath(*attr);

  // Check against each item in struct BgpPathC
  EXPECT_EQ(*path.origin(), static_cast<int>(attr->getOrigin()));

  // Check each element in the AS paths
  auto asPathIt = path.as_path()->begin();
  for (const auto& seg : *attr->getAsPath()) {
    EXPECT_EQ(*asPathIt, createTAsPathSeg(seg));
    ++asPathIt;
  }
  EXPECT_EQ(asPathIt, path.as_path()->end());

  EXPECT_EQ(*path.next_hop(), createTIpPrefix(attr->getNexthop()));
  EXPECT_EQ(*path.med(), attr->getMed());
  EXPECT_EQ(*path.local_pref(), attr->getLocalPref());
  EXPECT_EQ(*path.atomic_aggregate(), attr->getAtomicAggregate());

  EXPECT_EQ(*(path.aggregator()->asn()), aggregator.asn);
  EXPECT_EQ(*(path.aggregator()->ip()), aggregator.ip.str());

  // Check each element in the communities
  auto communitiesIt = path.communities()->begin();
  for (const auto& comm : attr->getCommunities().get()) {
    const auto& item = *communitiesIt;
    EXPECT_EQ(*item.asn(), comm.asn);
    EXPECT_EQ(*item.value(), comm.value);
    EXPECT_EQ(*item.community(), ((int64_t)comm.asn << 16) + comm.value);
    ++communitiesIt;
  }
  EXPECT_EQ(communitiesIt, path.communities()->end());

  EXPECT_EQ(*path.originator_id(), attr->getOriginatorId());

  // Check each element in the cluster list
  auto clusterListIt = path.cluster_list()->begin();
  for (const auto& cluster : attr->getClusterList().get()) {
    EXPECT_EQ(*clusterListIt, cluster);
    ++clusterListIt;
  }
  EXPECT_EQ(clusterListIt, path.cluster_list()->end());

  // Simple check: ensure the same extCommunities lengths
  EXPECT_EQ(path.extCommunities()->size(), attr->getExtCommunities()->size());

  EXPECT_EQ(path.weight(), attr->getWeight());
}

// Helper function to create a peer group with AFI settings for testing
thrift::PeerGroup createTestPeerGroup(
    bool disableIpv4 = false,
    bool disableIpv6 = false) {
  thrift::PeerGroup peerGroup;
  if (disableIpv4) {
    peerGroup.disable_ipv4_afi() = true;
  }
  if (disableIpv6) {
    peerGroup.disable_ipv6_afi() = true;
  }
  return peerGroup;
}

// Test validatePeerGroupConfigInPolicy success case when key_type is not set
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicySuccessNoKeyTypeTest) {
  rib_policy::TRouteFilterPolicy policy;
  // key_type not set - should default to success
  policy.statements()->emplace(
      "fa.*", createTRouteFilterStatement({}, false, true));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_TRUE(*result.success());
}

// Test validatePeerGroupConfigInPolicy success case when policy has no
// statements
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicySuccessNoStatementsTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  // No statements - should succeed

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_TRUE(*result.success());
}

// Test validatePeerGroupConfigInPolicy failure case when peer group name
// doesn't exist
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicyPeerGroupNotFoundTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "nonexistent-peer-group", createTRouteFilterStatement({}, false, true));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  // Don't add the peer group - should fail

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_FALSE(*result.success());
  EXPECT_THAT(*result.err(), HasSubstr("PEER_GROUP_NOT_FOUND"));
}

// Test validatePeerGroupConfigInPolicy failure case for IPv4 version mismatch
// in ingress filter
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicyIpv4IngressMismatchTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {kV4Prefix1},
          {kV4Prefix2},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          std::nullopt));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(true, false); // IPv4 disabled, IPv6 enabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_FALSE(*result.success());
  EXPECT_THAT(*result.err(), HasSubstr("IPV4_AFI_MISMATCH"));
}

// Test validatePeerGroupConfigInPolicy failure case for IPv6 version mismatch
// in egress filter
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicyIpv6EgressMismatchTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {kV4Prefix4},
          {kV4Prefix4},
          false,
          false,
          std::nullopt,
          facebook::bgp::routing_policy::IPVersion::V6));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(false, true); // IPv4 enabled, IPv6 disabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_FALSE(*result.success());
  EXPECT_THAT(*result.err(), HasSubstr("IPV6_AFI_MISMATCH"));
}

// Test validatePeerGroupConfigInPolicy success case when IP version is not set
// in prefix list
TEST_F(BgpServiceUtilTest, ValidatePeerGroupConfigInPolicyNoIpVersionTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatement({}, false, true)); // No IP version set

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(true, true); // Both AFIs disabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_TRUE(*result.success()); // Should succeed when IP version is not set
}

// Test validatePeerGroupConfigInPolicy success case when IPv4 policy with
// IPv4-enabled peer group
TEST_F(BgpServiceUtilTest, ValidatePeerGroupConfigInPolicyIpv4SuccessTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {},
          {},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          std::nullopt));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(false, true); // IPv4 enabled, IPv6 disabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_TRUE(*result.success());
}

// Test validatePeerGroupConfigInPolicy success case when IPv6 policy with
// IPv6-enabled peer group
TEST_F(BgpServiceUtilTest, ValidatePeerGroupConfigInPolicyIpv6SuccessTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {},
          {},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V6,
          std::nullopt));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(true, false); // IPv4 disabled, IPv6 enabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_TRUE(*result.success());
}

// Test validatePeerGroupConfigInPolicy with both ingress and egress filters
TEST_F(BgpServiceUtilTest, ValidatePeerGroupConfigInPolicyBothFiltersTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;

  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {},
          {},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          facebook::bgp::routing_policy::IPVersion::V6));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(false, false); // Both AFIs enabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_TRUE(*result.success());
}

// Test validatePeerGroupConfigInPolicy with both filters but ingress fails
// validation
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicyBothFiltersIngressFailTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;

  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {kV4Prefix1},
          {kV4Prefix1},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          facebook::bgp::routing_policy::IPVersion::V6));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(true, false); // IPv4 disabled, IPv6 enabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_FALSE(*result.success());
  EXPECT_THAT(*result.err(), HasSubstr("IPV4_AFI_MISMATCH"));
}

// Test validatePeerGroupConfigInPolicy with both filters but egress fails
// validation
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicyBothFiltersEgressFailTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;

  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {kV4Prefix4},
          {kV4Prefix4},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V6,
          facebook::bgp::routing_policy::IPVersion::V4));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(true, false); // IPv4 disabled, IPv6 enabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_FALSE(*result.success());
  EXPECT_THAT(*result.err(), HasSubstr("IPV4_AFI_MISMATCH"));
}

// Test validatePeerGroupConfigInPolicy with multiple peer groups - all valid
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicyMultiplePeerGroupsSuccessTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "peer-group-1",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {},
          {},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          std::nullopt));
  policy.statements()->emplace(
      "peer-group-2",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {},
          {},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V6,
          std::nullopt));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["peer-group-1"] =
      createTestPeerGroup(false, true); // IPv4 enabled, IPv6 disabled
  peerGroups["peer-group-2"] =
      createTestPeerGroup(true, false); // IPv4 disabled, IPv6 enabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_TRUE(*result.success());
}

// Test validatePeerGroupConfigInPolicy with multiple peer groups - one invalid
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicyMultiplePeerGroupsOneFailTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "peer-group-1",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {},
          {},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          std::nullopt));
  policy.statements()->emplace(
      "peer-group-2",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {kV4Prefix4},
          {kV4Prefix4},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          std::nullopt)); // This will fail

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["peer-group-1"] =
      createTestPeerGroup(false, true); // IPv4 enabled, IPv6 disabled
  peerGroups["peer-group-2"] =
      createTestPeerGroup(true, false); // IPv4 disabled, IPv6 enabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_FALSE(*result.success());
  EXPECT_THAT(*result.err(), HasSubstr("IPV4_AFI_MISMATCH"));
}

// Test validatePeerGroupConfigInPolicy with default AFI settings (both enabled)
TEST_F(
    BgpServiceUtilTest,
    ValidatePeerGroupConfigInPolicyDefaultAfiSettingsTest) {
  rib_policy::TRouteFilterPolicy policy;
  policy.key_type() = rib_policy::KeyType::PEER_GROUP_NAME;
  policy.statements()->emplace(
      "test-peer-group",
      createTRouteFilterStatementWithIngressAndEgressFilters(
          {},
          {},
          false,
          false,
          facebook::bgp::routing_policy::IPVersion::V4,
          std::nullopt));

  folly::F14NodeMap<std::string, thrift::PeerGroup> peerGroups;
  peerGroups["test-peer-group"] =
      createTestPeerGroup(); // Default: both AFIs enabled

  TResult result;
  validatePeerGroupConfigInPolicy(result, policy, peerGroups);

  EXPECT_TRUE(*result.success());
}

// Test setTResult utility function - success case
TEST_F(BgpServiceUtilTest, SetTResultSuccessTest) {
  TResult result;
  setTResult(result, true);

  EXPECT_TRUE(*result.success());
}

// Test setTResult utility function - error case
TEST_F(BgpServiceUtilTest, SetTResultErrorTest) {
  TResult result;
  setTResult(result, false, "Test error message");

  EXPECT_FALSE(*result.success());
  EXPECT_EQ(*result.err(), "Test error message");
}

// Test setTResult utility function - error case with no message
TEST_F(BgpServiceUtilTest, SetTResultErrorNoMessageTest) {
  TResult result;
  setTResult(result, false);

  EXPECT_FALSE(*result.success());
  // err() should not be set when no error message is provided
}

// Test topologyInfo field handling in createTBgpPath
TEST_F(BgpServiceUtilTest, TopologyInfoTest) {
  // Build a BgpPath
  auto attr =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(4, 4, 4, 4));

  // Create and set topologyInfo
  std::unordered_map<std::string, int64_t> topoInfo;
  topoInfo["plane_id"] = 123;
  topoInfo["rack_id"] = 456;
  attr->setTopologyInfo(topoInfo);

  const TBgpPath& path = createTBgpPath(*attr);

  // Verify topologyInfo was correctly transferred
  ASSERT_TRUE(path.topologyInfo().has_value());
  auto& pathTopoInfo = path.topologyInfo().value();
  EXPECT_EQ(pathTopoInfo.size(), topoInfo.size());
  EXPECT_EQ(pathTopoInfo.at("plane_id"), 123);
  EXPECT_EQ(pathTopoInfo.at("rack_id"), 456);
}

TEST_F(BgpServiceUtilTest, BackupAddrTest) {
  auto attr =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(4, 4, 4, 4));
  const auto backupAddr = folly::IPAddress("2001:db8::1");
  attr->setBackupAddr(backupAddr);

  const auto path = createTBgpPath(*attr);

  ASSERT_TRUE(path.backup_addr().has_value());
  EXPECT_EQ(createTIpPrefix(backupAddr), *path.backup_addr());
}

// test TIpPrefixToString function
TEST_F(BgpServiceUtilTest, TIpPrefixToStringTest) {
  auto prefix = createTIpPrefix(folly::IPAddress("10.1.2.126"));
  EXPECT_EQ(TIpPrefixToString(prefix), "10.1.2.126/32");
}

// test createTAsPathSeg for 2 bytes ASN
TEST_F(BgpServiceUtilTest, CreateTAsPathSegShortAsnTest) {
  BgpAttrAsPathSegmentC asSet, asSequence, asConfedSet, asConfedSequence;
  std::vector<int> asSetArr = {64512, 65534};
  std::vector<int> asSequenceArr = {65000, 65000, 65000};
  std::vector<int> asConfedSetArr = {65000, 64511, 65000, 65000};
  std::vector<int> asConfedSequenceArr = {65000, 64511, 65000, 12345};
  asSet.asSet.insert(asSetArr.begin(), asSetArr.end());
  asSequence.asSequence.assign(asSequenceArr.begin(), asSequenceArr.end());
  asConfedSet.asConfedSet.insert(asConfedSetArr.begin(), asConfedSetArr.end());
  asConfedSequence.asConfedSequence.assign(
      asConfedSequenceArr.begin(), asConfedSequenceArr.end());

  auto asSetSeg = createTAsPathSeg(asSet);
  auto asSequenceSeg = createTAsPathSeg(asSequence);
  auto asConfedSetSeg = createTAsPathSeg(asConfedSet);
  auto asConfedSequenceSeg = createTAsPathSeg(asConfedSequence);

  EXPECT_EQ(*asSetSeg.seg_type(), TAsPathSegType::AS_SET);
  EXPECT_THAT(*asSetSeg.asns(), UnorderedElementsAreArray(asSetArr));
  EXPECT_THAT(*asSetSeg.asns_4_byte(), UnorderedElementsAreArray(asSetArr));
  EXPECT_EQ(*asSequenceSeg.seg_type(), TAsPathSegType::AS_SEQUENCE);
  EXPECT_THAT(*asSequenceSeg.asns(), UnorderedElementsAreArray(asSequenceArr));
  EXPECT_THAT(
      *asSequenceSeg.asns_4_byte(), UnorderedElementsAreArray(asSequenceArr));
  EXPECT_EQ(*asConfedSetSeg.seg_type(), TAsPathSegType::AS_CONFED_SET);
  // asConfedSetSeg should remove duplicated as number
  EXPECT_THAT(*asConfedSetSeg.asns(), UnorderedElementsAre(64511, 65000));
  EXPECT_THAT(
      *asConfedSetSeg.asns_4_byte(), UnorderedElementsAre(64511, 65000));
  EXPECT_EQ(
      *asConfedSequenceSeg.seg_type(), TAsPathSegType::AS_CONFED_SEQUENCE);
  EXPECT_THAT(
      *asConfedSequenceSeg.asns(),
      UnorderedElementsAreArray(asConfedSequenceArr));
  EXPECT_THAT(
      *asConfedSequenceSeg.asns_4_byte(),
      UnorderedElementsAreArray(asConfedSequenceArr));
}

// test createTAsPathSeg for 4 bytes ASN
TEST_F(BgpServiceUtilTest, CreateTAsPathSegLongAsnTestTest) {
  BgpAttrAsPathSegmentC asSet, asSequence, asConfedSet, asConfedSequence;
  std::vector<int64_t> asSetArr = {987654321, 987654320};
  std::vector<int64_t> asSequenceArr = {987654322, 987654321, 987654320};
  std::vector<int64_t> asConfedSetArr = {
      987654323, 987654322, 987654321, 987654320};
  std::vector<int64_t> asConfedSequenceArr = {
      987654324, 987654323, 987654322, 987654321};
  asSet.asSet.insert(asSetArr.begin(), asSetArr.end());
  asConfedSequence.asConfedSequence.assign(
      asConfedSequenceArr.begin(), asConfedSequenceArr.end());
  asConfedSet.asConfedSet.insert(asConfedSetArr.begin(), asConfedSetArr.end());
  asSequence.asSequence.assign(asSequenceArr.begin(), asSequenceArr.end());

  auto asSetSeg = createTAsPathSeg(asSet);
  auto asSequenceSeg = createTAsPathSeg(asSequence);
  auto asConfedSetSeg = createTAsPathSeg(asConfedSet);
  auto asConfedSequenceSeg = createTAsPathSeg(asConfedSequence);

  EXPECT_EQ(*asSetSeg.seg_type(), TAsPathSegType::AS_SET);
  EXPECT_THAT(*asSetSeg.asns(), UnorderedElementsAreArray(asSetArr));
  EXPECT_THAT(*asSetSeg.asns_4_byte(), UnorderedElementsAreArray(asSetArr));
  EXPECT_EQ(*asSequenceSeg.seg_type(), TAsPathSegType::AS_SEQUENCE);
  EXPECT_THAT(
      *asSequenceSeg.asns_4_byte(), UnorderedElementsAreArray(asSequenceArr));
  EXPECT_THAT(*asSequenceSeg.asns(), UnorderedElementsAreArray(asSequenceArr));
  EXPECT_EQ(*asConfedSetSeg.seg_type(), TAsPathSegType::AS_CONFED_SET);
  EXPECT_THAT(
      *asConfedSetSeg.asns_4_byte(), UnorderedElementsAreArray(asConfedSetArr));
  EXPECT_THAT(
      *asConfedSetSeg.asns(), UnorderedElementsAreArray(asConfedSetArr));
  EXPECT_EQ(
      *asConfedSequenceSeg.seg_type(), TAsPathSegType::AS_CONFED_SEQUENCE);
  EXPECT_THAT(
      *asConfedSequenceSeg.asns_4_byte(),
      UnorderedElementsAreArray(asConfedSequenceArr));
  EXPECT_THAT(
      *asConfedSequenceSeg.asns(),
      UnorderedElementsAreArray(asConfedSequenceArr));
}
// Test validateDirectionToPolicyMap function - Success case
TEST_F(BgpServiceUtilTest, ValidateDirectionToPolicyMapSuccessTest) {
  // Create a map of direction to policy names
  std::map<facebook::bgp::bgp_policy::DIRECTION, std::string> directionToPolicy;
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::IN] =
      "TestIngressPolicy";
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::OUT] =
      "TestEgressPolicy";

  // Create a policy manager with test policies
  auto policyStatement1 = createBgpPolicyStatement("TestIngressPolicy");
  auto policyStatement2 = createBgpPolicyStatement("TestEgressPolicy");

  bgp_policy::BgpPolicies policies;
  policies.bgp_policy_statements()->emplace_back(policyStatement1);
  policies.bgp_policy_statements()->emplace_back(policyStatement2);

  auto policyManager =
      std::make_shared<PolicyManager>(policies, createTestBgpGlobalConfig());

  // Test validation - should succeed
  auto result = validateDirectionToPolicyMap(directionToPolicy, policyManager);
  EXPECT_EQ(result, PolicyValidationResult::SUCCESS);
}

// Test validateDirectionToPolicyMap function - Policy not found case
TEST_F(BgpServiceUtilTest, ValidateDirectionToPolicyMapPolicyNotFoundTest) {
  // Create a map of direction to policy names with non-existent policy
  std::map<facebook::bgp::bgp_policy::DIRECTION, std::string> directionToPolicy;
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::IN] =
      "NonExistentIngressPolicy";
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::OUT] =
      "TestEgressPolicy";

  // Create a policy manager with only one policy
  auto policyStatement = createBgpPolicyStatement("TestEgressPolicy");

  bgp_policy::BgpPolicies policies;
  policies.bgp_policy_statements()->emplace_back(policyStatement);

  auto policyManager =
      std::make_shared<PolicyManager>(policies, createTestBgpGlobalConfig());

  // Test validation - should fail for missing ingress policy
  auto result = validateDirectionToPolicyMap(directionToPolicy, policyManager);
  EXPECT_EQ(result, PolicyValidationResult::POLICY_NOT_FOUND);
}

// Test validateDirectionToPolicyMap function - Empty policy manager case
TEST_F(BgpServiceUtilTest, ValidateDirectionToPolicyMapEmptyPolicyManagerTest) {
  // Create a map of direction to policy names
  std::map<facebook::bgp::bgp_policy::DIRECTION, std::string> directionToPolicy;
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::IN] =
      "TestIngressPolicy";

  // Test with null policy manager
  std::shared_ptr<PolicyManager> nullPolicyManager = nullptr;

  auto result =
      validateDirectionToPolicyMap(directionToPolicy, nullPolicyManager);
  EXPECT_EQ(result, PolicyValidationResult::POLICY_NOT_FOUND);
}

// Test validateDirectionToPolicyMap function - Empty map case
TEST_F(BgpServiceUtilTest, ValidateDirectionToPolicyMapEmptyMapTest) {
  // Create empty map
  std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>
      emptyDirectionToPolicy;

  // Create a simple policy manager
  auto policyStatement = createBgpPolicyStatement("TestPolicy");
  bgp_policy::BgpPolicies policies;
  policies.bgp_policy_statements()->emplace_back(policyStatement);
  auto policyManager =
      std::make_shared<PolicyManager>(policies, createTestBgpGlobalConfig());

  // Test validation with empty map - should succeed
  auto result =
      validateDirectionToPolicyMap(emptyDirectionToPolicy, policyManager);
  EXPECT_EQ(result, PolicyValidationResult::SUCCESS);
}

// Test validateDirectionToPolicyMap function - Both directions case
TEST_F(BgpServiceUtilTest, ValidateDirectionToPolicyMapBothDirectionsTest) {
  // Create a map with both IN and OUT policies
  std::map<facebook::bgp::bgp_policy::DIRECTION, std::string> directionToPolicy;
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::IN] = "IngressPolicy";
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::OUT] = "EgressPolicy";

  // Create a policy manager with both policies
  auto ingressPolicy = createBgpPolicyStatement("IngressPolicy");
  auto egressPolicy = createBgpPolicyStatement("EgressPolicy");

  bgp_policy::BgpPolicies policies;
  policies.bgp_policy_statements()->emplace_back(ingressPolicy);
  policies.bgp_policy_statements()->emplace_back(egressPolicy);

  auto policyManager =
      std::make_shared<PolicyManager>(policies, createTestBgpGlobalConfig());

  // Test validation - should succeed
  auto result = validateDirectionToPolicyMap(directionToPolicy, policyManager);
  EXPECT_EQ(result, PolicyValidationResult::SUCCESS);
}

// Test validateDirectionToPolicyMap function - One valid, one invalid policy
TEST_F(BgpServiceUtilTest, ValidateDirectionToPolicyMapOneValidOneInvalidTest) {
  // Create a map with one valid and one invalid policy
  std::map<facebook::bgp::bgp_policy::DIRECTION, std::string> directionToPolicy;
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::IN] =
      "ValidIngressPolicy";
  directionToPolicy[facebook::bgp::bgp_policy::DIRECTION::OUT] =
      "InvalidEgressPolicy";

  // Create a policy manager with only the ingress policy
  auto ingressPolicy = createBgpPolicyStatement("ValidIngressPolicy");

  bgp_policy::BgpPolicies policies;
  policies.bgp_policy_statements()->emplace_back(ingressPolicy);

  auto policyManager =
      std::make_shared<PolicyManager>(policies, createTestBgpGlobalConfig());

  // Test validation - should fail for missing egress policy
  auto result = validateDirectionToPolicyMap(directionToPolicy, policyManager);
  EXPECT_EQ(result, PolicyValidationResult::POLICY_NOT_FOUND);
}

// Test fixture for resolveEffectivePeerPolicies tests
class ResolveEffectivePeerPoliciesTest : public ::testing::Test {
 protected:
  thrift::BgpConfig config_;
  thrift::PeerGroup peerGroup1_;

  void SetUp() override {
    // Create a peer group with policies
    peerGroup1_.name() = "TEST_PEER_GROUP";
    peerGroup1_.ingress_policy_name() = "GROUP_INGRESS_POLICY";
    peerGroup1_.egress_policy_name() = "GROUP_EGRESS_POLICY";

    // Set up base config
    config_.router_id() = "10.0.0.1";
    config_.local_as_4_byte() = 65000;
    config_.listen_port() = 179;

    std::vector<thrift::PeerGroup> peerGroups;
    peerGroups.push_back(peerGroup1_);
    config_.peer_groups() = std::move(peerGroups);
  }

  // Helper to create a simple peer
  thrift::BgpPeer createPeer(
      const std::string& peerAddr,
      const std::optional<std::string>& ingressPolicy = std::nullopt,
      const std::optional<std::string>& egressPolicy = std::nullopt,
      const std::optional<std::string>& peerGroupName = std::nullopt) {
    thrift::BgpPeer peer;
    peer.peer_addr() = peerAddr;
    peer.remote_as_4_byte() = 65001;
    peer.local_addr() = "10.0.0.1";
    peer.next_hop4() = "10.0.0.1";
    peer.next_hop6() = "::1";
    if (ingressPolicy.has_value()) {
      peer.ingress_policy_name() = ingressPolicy.value();
    }
    if (egressPolicy.has_value()) {
      peer.egress_policy_name() = egressPolicy.value();
    }
    if (peerGroupName.has_value()) {
      peer.peer_group_name() = peerGroupName.value();
    }
    return peer;
  }
};

// Test resolveEffectivePeerPolicies - basic case with peer-level policies
TEST_F(ResolveEffectivePeerPoliciesTest, BasicPeerLevelPolicies) {
  // Add peer with explicit ingress and egress policies
  auto peer1 = createPeer("192.168.1.1", "PEER1_INGRESS", "PEER1_EGRESS");
  config_.peers() = {peer1};

  auto configObj = std::make_shared<const Config>(config_);

  // Filter that accepts all peers
  auto allPeersFilter = [](const folly::IPAddress&, const BgpPeerConfig&) {
    return true;
  };

  auto result = resolveEffectivePeerPolicies(*configObj, allPeersFilter);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->size(), 1);
  ASSERT_TRUE(result->count("192.168.1.1") > 0);

  const auto& peerPolicies = result->at("192.168.1.1");
  EXPECT_EQ(peerPolicies.size(), 2);
  EXPECT_EQ(
      peerPolicies.at(bgp_policy::DIRECTION::IN).value(), "PEER1_INGRESS");
  EXPECT_EQ(
      peerPolicies.at(bgp_policy::DIRECTION::OUT).value(), "PEER1_EGRESS");
}

// Test resolveEffectivePeerPolicies - peer inherits from peer group
TEST_F(ResolveEffectivePeerPoliciesTest, PeerInheritsPeerGroupPolicies) {
  // Add peer that uses peer group (no local policy overwrites)
  auto peer1 =
      createPeer("192.168.1.1", std::nullopt, std::nullopt, "TEST_PEER_GROUP");
  config_.peers() = {peer1};

  auto configObj = std::make_shared<const Config>(config_);

  auto allPeersFilter = [](const folly::IPAddress&, const BgpPeerConfig&) {
    return true;
  };

  auto result = resolveEffectivePeerPolicies(*configObj, allPeersFilter);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->size(), 1);
  ASSERT_TRUE(result->count("192.168.1.1") > 0);

  const auto& peerPolicies = result->at("192.168.1.1");
  // Should have inherited policies from peer group
  EXPECT_EQ(
      peerPolicies.at(bgp_policy::DIRECTION::IN).value(),
      "GROUP_INGRESS_POLICY");
  EXPECT_EQ(
      peerPolicies.at(bgp_policy::DIRECTION::OUT).value(),
      "GROUP_EGRESS_POLICY");
}

// Test resolveEffectivePeerPolicies - peer overrides peer group policies
TEST_F(ResolveEffectivePeerPoliciesTest, PeerOverridesPeerGroupPolicies) {
  // Add peer with peer group but local policy overwrites
  auto peer1 = createPeer(
      "192.168.1.1",
      "PEER_OVERRIDE_INGRESS",
      "PEER_OVERRIDE_EGRESS",
      "TEST_PEER_GROUP");
  config_.peers() = {peer1};

  auto configObj = std::make_shared<const Config>(config_);

  auto allPeersFilter = [](const folly::IPAddress&, const BgpPeerConfig&) {
    return true;
  };

  auto result = resolveEffectivePeerPolicies(*configObj, allPeersFilter);

  ASSERT_NE(result, nullptr);
  const auto& peerPolicies = result->at("192.168.1.1");
  // Peer-level policies should override peer group
  EXPECT_EQ(
      peerPolicies.at(bgp_policy::DIRECTION::IN).value(),
      "PEER_OVERRIDE_INGRESS");
  EXPECT_EQ(
      peerPolicies.at(bgp_policy::DIRECTION::OUT).value(),
      "PEER_OVERRIDE_EGRESS");
}

// Test resolveEffectivePeerPolicies - peer without any policies
TEST_F(ResolveEffectivePeerPoliciesTest, PeerWithNoPolicies) {
  // Add peer without any policies or peer group
  auto peer1 = createPeer("192.168.1.1");
  config_.peers() = {peer1};

  auto configObj = std::make_shared<const Config>(config_);

  auto allPeersFilter = [](const folly::IPAddress&, const BgpPeerConfig&) {
    return true;
  };

  auto result = resolveEffectivePeerPolicies(*configObj, allPeersFilter);

  ASSERT_NE(result, nullptr);
  const auto& peerPolicies = result->at("192.168.1.1");
  // Both directions should be nullopt (no policy set)
  EXPECT_FALSE(peerPolicies.at(bgp_policy::DIRECTION::IN).has_value());
  EXPECT_FALSE(peerPolicies.at(bgp_policy::DIRECTION::OUT).has_value());
}

// Test resolveEffectivePeerPolicies - filter excludes some peers
TEST_F(ResolveEffectivePeerPoliciesTest, FilterExcludesPeers) {
  auto peer1 = createPeer("192.168.1.1", "PEER1_INGRESS", "PEER1_EGRESS");
  auto peer2 = createPeer("192.168.1.2", "PEER2_INGRESS", "PEER2_EGRESS");
  auto peer3 = createPeer("192.168.1.3", "PEER3_INGRESS", "PEER3_EGRESS");
  config_.peers() = {peer1, peer2, peer3};

  auto configObj = std::make_shared<const Config>(config_);

  // Filter that only accepts peer1 and peer3
  auto selectivePeersFilter = [](const folly::IPAddress& addr,
                                 const BgpPeerConfig&) {
    return addr.str() == "192.168.1.1" || addr.str() == "192.168.1.3";
  };

  auto result = resolveEffectivePeerPolicies(*configObj, selectivePeersFilter);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->size(), 2);
  EXPECT_TRUE(result->count("192.168.1.1") > 0);
  EXPECT_FALSE(result->count("192.168.1.2") > 0); // excluded by filter
  EXPECT_TRUE(result->count("192.168.1.3") > 0);
}

// Test resolveEffectivePeerPolicies - empty config (no peers)
TEST_F(ResolveEffectivePeerPoliciesTest, EmptyConfig) {
  config_.peers() = {};

  auto configObj = std::make_shared<const Config>(config_);

  auto allPeersFilter = [](const folly::IPAddress&, const BgpPeerConfig&) {
    return true;
  };

  auto result = resolveEffectivePeerPolicies(*configObj, allPeersFilter);

  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->empty());
}

// Test resolveEffectivePeerPolicies - filter rejects all peers
TEST_F(ResolveEffectivePeerPoliciesTest, FilterRejectsAllPeers) {
  auto peer1 = createPeer("192.168.1.1", "PEER1_INGRESS", "PEER1_EGRESS");
  auto peer2 = createPeer("192.168.1.2", "PEER2_INGRESS", "PEER2_EGRESS");
  config_.peers() = {peer1, peer2};

  auto configObj = std::make_shared<const Config>(config_);

  // Filter that rejects all peers
  auto rejectAllFilter = [](const folly::IPAddress&, const BgpPeerConfig&) {
    return false;
  };

  auto result = resolveEffectivePeerPolicies(*configObj, rejectAllFilter);

  ASSERT_NE(result, nullptr);
  EXPECT_TRUE(result->empty());
}

// Test resolveEffectivePeerPolicies - multiple peers with different scenarios
TEST_F(ResolveEffectivePeerPoliciesTest, MultiplePeersMixedScenarios) {
  // peer1: has its own policies
  auto peer1 = createPeer("192.168.1.1", "PEER1_IN", "PEER1_OUT");
  // peer2: inherits from peer group
  auto peer2 =
      createPeer("192.168.1.2", std::nullopt, std::nullopt, "TEST_PEER_GROUP");
  // peer3: no policies at all
  auto peer3 = createPeer("192.168.1.3");
  // peer4: partial override (only egress)
  auto peer4 = createPeer(
      "192.168.1.4", std::nullopt, "PEER4_EGRESS_OVERRIDE", "TEST_PEER_GROUP");

  config_.peers() = {peer1, peer2, peer3, peer4};

  auto configObj = std::make_shared<const Config>(config_);

  auto allPeersFilter = [](const folly::IPAddress&, const BgpPeerConfig&) {
    return true;
  };

  auto result = resolveEffectivePeerPolicies(*configObj, allPeersFilter);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->size(), 4);

  // Verify peer1 - own policies
  EXPECT_EQ(
      result->at("192.168.1.1").at(bgp_policy::DIRECTION::IN).value(),
      "PEER1_IN");
  EXPECT_EQ(
      result->at("192.168.1.1").at(bgp_policy::DIRECTION::OUT).value(),
      "PEER1_OUT");

  // Verify peer2 - inherited from peer group
  EXPECT_EQ(
      result->at("192.168.1.2").at(bgp_policy::DIRECTION::IN).value(),
      "GROUP_INGRESS_POLICY");
  EXPECT_EQ(
      result->at("192.168.1.2").at(bgp_policy::DIRECTION::OUT).value(),
      "GROUP_EGRESS_POLICY");

  // Verify peer3 - no policies
  EXPECT_FALSE(
      result->at("192.168.1.3").at(bgp_policy::DIRECTION::IN).has_value());
  EXPECT_FALSE(
      result->at("192.168.1.3").at(bgp_policy::DIRECTION::OUT).has_value());

  // Verify peer4 - partial override (ingress from group, egress overridden)
  EXPECT_EQ(
      result->at("192.168.1.4").at(bgp_policy::DIRECTION::IN).value(),
      "GROUP_INGRESS_POLICY");
  EXPECT_EQ(
      result->at("192.168.1.4").at(bgp_policy::DIRECTION::OUT).value(),
      "PEER4_EGRESS_OVERRIDE");
}

// Test resolveEffectivePeerPolicies - filter based on peer config
TEST_F(ResolveEffectivePeerPoliciesTest, FilterBasedOnPeerConfig) {
  auto peer1 =
      createPeer("192.168.1.1", std::nullopt, std::nullopt, "TEST_PEER_GROUP");
  auto peer2 = createPeer("192.168.1.2", "PEER2_IN", "PEER2_OUT");
  config_.peers() = {peer1, peer2};

  auto configObj = std::make_shared<const Config>(config_);

  // Filter that only accepts peers with a peer group
  auto peerGroupFilter = [](const folly::IPAddress&,
                            const BgpPeerConfig& peerConfig) {
    return peerConfig.commonPeerGroupConfig.peerGroupName.has_value();
  };

  auto result = resolveEffectivePeerPolicies(*configObj, peerGroupFilter);

  ASSERT_NE(result, nullptr);
  EXPECT_EQ(result->size(), 1);
  EXPECT_TRUE(result->count("192.168.1.1") > 0);
  EXPECT_FALSE(result->count("192.168.1.2") > 0);
}

// Test getUnsupportedBgpPeerFields - all fields allowed
TEST_F(BgpServiceUtilTest, GetUnsupportedBgpPeerFieldsAllAllowedTest) {
  thrift::BgpPeer peer;
  peer.peer_addr() = "10.0.0.1";
  peer.local_addr() = "10.0.0.2";

  folly::F14FastSet<std::string_view> allowedFields = {
      "peer_addr", "local_addr"};

  auto result = getUnsupportedBgpPeerFields(peer, allowedFields);
  EXPECT_TRUE(result.empty());
}

// Test getUnsupportedBgpPeerFields - unsupported field detected
TEST_F(BgpServiceUtilTest, GetUnsupportedBgpPeerFieldsUnsupportedTest) {
  thrift::BgpPeer peer;
  peer.peer_addr() = "10.0.0.1";
  peer.remote_as() = 65000;

  folly::F14FastSet<std::string_view> allowedFields = {"peer_addr"};

  auto result = getUnsupportedBgpPeerFields(peer, allowedFields);
  EXPECT_THAT(result, ElementsAre("remote_as"));
}

// Test getUnsupportedBgpPeerFields - multiple unsupported fields detected
TEST_F(BgpServiceUtilTest, GetUnsupportedBgpPeerFieldsMultipleTest) {
  thrift::BgpPeer peer;
  peer.peer_addr() = "10.0.0.1";
  peer.remote_as() = 65000;
  peer.local_addr() = "10.0.0.2";

  folly::F14FastSet<std::string_view> allowedFields = {"peer_addr"};

  auto result = getUnsupportedBgpPeerFields(peer, allowedFields);
  EXPECT_THAT(result, UnorderedElementsAre("remote_as", "local_addr"));
}

// Test getUnsupportedBgpPeerFields - no fields set
TEST_F(BgpServiceUtilTest, GetUnsupportedBgpPeerFieldsNoFieldsSetTest) {
  thrift::BgpPeer peer;

  folly::F14FastSet<std::string_view> allowedFields = {"peer_addr"};

  auto result = getUnsupportedBgpPeerFields(peer, allowedFields);
  EXPECT_TRUE(result.empty());
}

// Test getUnsupportedBgpPeerFields - empty allowlist rejects any set field
TEST_F(BgpServiceUtilTest, GetUnsupportedBgpPeerFieldsEmptyAllowlistTest) {
  thrift::BgpPeer peer;
  peer.peer_addr() = "10.0.0.1";

  folly::F14FastSet<std::string_view> allowedFields = {};

  auto result = getUnsupportedBgpPeerFields(peer, allowedFields);
  EXPECT_FALSE(result.empty());
}

// Test getUnsupportedBgpPeerFields - non-optional field explicitly set and not
// in allowlist is detected (local_addr is a non-optional string field)
TEST_F(BgpServiceUtilTest, GetUnsupportedBgpPeerFieldsNonOptionalSetTest) {
  thrift::BgpPeer peer;
  peer.peer_addr() = "10.0.0.1";
  peer.local_addr() = "10.0.0.2"; // non-optional field, explicitly set

  // Only peer_addr is allowed; local_addr is not in the allowlist
  folly::F14FastSet<std::string_view> allowedFields = {"peer_addr"};

  auto result = getUnsupportedBgpPeerFields(peer, allowedFields);
  EXPECT_THAT(result, Contains("local_addr"));
}

// Test getUnsupportedBgpPeerFields - non-optional field NOT explicitly set
// should not be flagged even if not in allowlist
TEST_F(BgpServiceUtilTest, GetUnsupportedBgpPeerFieldsNonOptionalUnsetTest) {
  thrift::BgpPeer peer;
  peer.peer_addr() = "10.0.0.1";
  // Do NOT set local_addr (non-optional string field)

  // Allowlist only contains peer_addr. local_addr is non-optional and not in
  // the allowlist, but was never explicitly set so should not be flagged.
  folly::F14FastSet<std::string_view> allowedFields = {"peer_addr"};

  auto result = getUnsupportedBgpPeerFields(peer, allowedFields);
  EXPECT_THAT(result, Not(Contains("local_addr")));
}

} // namespace facebook::bgp
