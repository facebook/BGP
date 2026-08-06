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

#define RibBase_TEST_FRIENDS                                                   \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTest);              \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestBadFileTermination);    \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyTermPolicy);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestMalformedContent);      \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyFile);     \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTestMissingFile);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestBackwardCompatibility); \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyPolicy);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadRibPolicy);    \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyFile);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestMissingFile);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture,                                             \
      ReadSaveRibPolicyStateTestBadTerminationString);                         \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadFileContent);  \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTest);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, SaveTRibPolicyStorePrettyPrintsJson);       \
  FRIEND_TEST(RibPolicyRestoreTestFixture, CrfFileModeToggle);                 \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactNoFile);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactValidFile);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactMalformedJson);    \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfDryrunFalse);  \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfDryrunTrue);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture,                                             \
      ReadRibPolicyStateCrfArtifactNoPolicyStore);                             \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactEmptyFile);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfNoCachedPolicy);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactDryrunTrue);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfFullIntegration);      \
  FRIEND_TEST(RibPolicyRestoreTestFixture, CpsFileModeToggle);                 \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactNoFile);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactValidFile);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactMalformedJson);    \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadRibPolicyStateCpsDryrunFalse);  \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadRibPolicyStateCpsDryrunTrue);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture,                                             \
      ReadRibPolicyStateCpsArtifactNoPolicyStore);                             \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactEmptyFile);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadRibPolicyStateCpsNoCachedPolicy);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactDryrunTrue);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactEmptyPath);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadRibPolicyStateCpsFullIntegration);

/*
 * pathSelectionPolicy_ moved from RibBase to RibDC in diff 9/10. Test that
 * accesses the field directly needs RibDC friendship too.
 */
#define RibDC_TEST_FRIENDS \
  FRIEND_TEST(             \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyPolicy);

#define RouteAttributePolicy_TEST_FRIENDS                                      \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTest);              \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestBadFileTermination);    \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyTermPolicy);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestMalformedContent);      \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyFile);     \
  FRIEND_TEST(RibPolicyRestoreTestFixture, RibPolicyRestoreTestMissingFile);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, RibPolicyRestoreTestBackwardCompatibility); \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyPolicy);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadRibPolicy);    \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyFile);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestMissingFile);     \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture,                                             \
      ReadSaveRibPolicyStateTestBadTerminationString);                         \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadFileContent);  \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTest);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, SaveTRibPolicyStorePrettyPrintsJson);       \
  FRIEND_TEST(RibPolicyRestoreTestFixture, CrfFileModeToggle);                 \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactNoFile);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactValidFile);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactMalformedJson);    \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfDryrunFalse);  \
  FRIEND_TEST(RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfDryrunTrue);   \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture,                                             \
      ReadRibPolicyStateCrfArtifactNoPolicyStore);                             \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactEmptyFile);        \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfNoCachedPolicy);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactDryrunTrue);       \
  FRIEND_TEST(                                                                 \
      RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfFullIntegration);

#include <unistd.h>

#include <boost/filesystem.hpp>
#include <fb303/ServiceData.h>
#include <fmt/format.h>

#include <folly/FileUtil.h>
#include <folly/IPAddress.h>
#include <folly/json/json.h>

#include "neteng/fboss/bgp/cpp/rib/RibFileUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibUtils.h"

using namespace facebook::bgp;
using namespace facebook::bgp::rib_policy;
using namespace facebook::neteng::fboss::bgp::thrift;
using namespace std::chrono;

namespace facebook::bgp {

class RibPolicyRestoreTestFixture : public RibFixture {
  // Delete routing policy state file (if it exists) before and after each test
  // to ensure idempotency.
  void SetUp() override {
    RibFixture::SetUp();
    boost::filesystem::remove(FLAGS_rp_state_file);
  }
  void TearDown() override {
    RibFixture::TearDown();
    boost::filesystem::remove(FLAGS_rp_state_file);
  }
};

/*
 * Test read rib policy state file with below cases:
 * - read file with stale timestamp
 * - read file with unmatch version
 * - read file with wrong term line
 * - read file with invalid rib-policy content
 * - read file with random content
 * - read empty file
 * - read non-existent file
 * - backward combatibility test for read/savePathSelectionPolicyState
 */
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTest) {
  const auto prefix1 = folly::IPAddress::createNetwork("1::/64");
  auto tRibPolicy = createTRibPolicyLbw({prefix1}, kLbw10G);
  RibPolicy expectedPolicy{tRibPolicy};

  // save for testing
  {
    TRibPolicyStore tRibPolicyStore;
    tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
    tRibPolicyStore.policy() = tRibPolicy;
    rib_->saveTRibPolicyStore(tRibPolicyStore);
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));
  }

  // call read rib policy from file and compare
  {
    auto ribPolicy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, ribPolicy);
    EXPECT_EQ(expectedPolicy, *ribPolicy);
  }

  // the retrieved policy should be resumed from its saved time
  {
    auto storedTime = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count() -
        100;
    TRibPolicyStore oldPolicy;
    // make sure the timestamp is stale
    oldPolicy.storedTime() = storedTime;
    oldPolicy.fileTermination() = kRibPolicyFileTermination;

    // save a policy that valids for 20 seconds
    auto policy20s = tRibPolicy;
    auto& tSubPolicy = *policy20s.route_attribute_policy();
    auto& tStmt = tSubPolicy.statements()->find("stmt1")->second;
    tStmt.expiration_time_s() =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count() -
        80;

    policy20s.path_selection_policy() =
        createTPathSelectionPolicyWithPathSelector(
            {kV4Prefix1}, TPathSelector());

    oldPolicy.policy() = policy20s;

    // write to the file
    folly::writeFileAtomic(
        FLAGS_rp_state_file,
        apache::thrift::SimpleJSONSerializer::serialize<std::string>(
            oldPolicy));

    // verify reading rib policy has expired
    auto policy = rib_->readRibPolicyState();
    EXPECT_EQ(
        *tStmt.expiration_time_s(),
        policy->getRouteAttributePolicy()
            ->statements_.at("stmt1")
            .getExpirationTime());
    EXPECT_FALSE(
        policy->getRouteAttributePolicy()->statements_.at("stmt1").isActive());

    // Ensure that we do have the path selection policy loaded
    EXPECT_TRUE(policy->hasPathSelectionPolicy());
  }
}

// make saved file with bad termination string and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestBadFileTermination) {
  TRibPolicyStore badTermPolicy;
  auto badTerm = "bad ending line";

  // make sure the timestamp is stale
  badTermPolicy.storedTime() =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  badTermPolicy.fileTermination() = badTerm;
  badTermPolicy.policy() = createTRibPolicyLbw({kV4Prefix1}, kLbw10G);

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          badTermPolicy));

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// make saved file with empty statement in rib policy and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyTermPolicy) {
  TRibPolicyStore badTermPolicy;
  TRibPolicy emptyPolicy = createTRibPolicyLbw({kV4Prefix1}, kLbw10G);
  emptyPolicy.route_attribute_policy().reset();

  // make sure the timestamp is stale
  badTermPolicy.storedTime() =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  badTermPolicy.fileTermination() = kRibPolicyFileTermination;
  badTermPolicy.policy() = emptyPolicy;

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          badTermPolicy));

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// make saved file with random content and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestMalformedContent) {
  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file, "anything other than rib policy state");

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// make empty saved file and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestEmptyFile) {
  // write to the file
  folly::writeFileAtomic(FLAGS_rp_state_file, "");

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// file does not exist and read
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestMissingFile) {
  // remove file
  boost::filesystem::remove(FLAGS_rp_state_file);

  // verify reading rib policy will return nullptr
  auto policy = rib_->readRibPolicyState();
  EXPECT_EQ(nullptr, policy);
}

// backward combatibility test for read/savePathSelectionPolicyState
// test the case when bgpd save RibPolicy by saveRibPolicyState, and we
// retrieve PathSelectionPolicy by readPathSelectionPolicyState
TEST_F(RibPolicyRestoreTestFixture, RibPolicyRestoreTestBackwardCompatibility) {
  {
    // Create the tPathSelectionPolicy and tRibPolicy for testing
    auto tMatcher = createCommunityMatch(200, 666, bgp_policy::Origin::EGP);
    auto tPathSelector = createTPathSlectorWithOneMatcher(tMatcher);

    auto tRibPolicy = createTRibPolicyLbw({kV4Prefix1}, kLbw10G);
    TPathSelectionPolicy tPathSelectionPolicy =
        createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector);
    // update the RibPolicy
    tRibPolicy.path_selection_policy() = tPathSelectionPolicy;
    PathSelectionPolicy expectedPathSelectionPolicy{tPathSelectionPolicy};

    // save the tRibPolicy as saveRibPolicyState
    TRibPolicyStore tRibPolicyStore;
    tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
    tRibPolicyStore.policy() = tRibPolicy;
    rib_->saveTRibPolicyStore(tRibPolicyStore);
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));

    // read the path selection policy from file
    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_EQ(expectedPathSelectionPolicy, *policy->getPathSelectionPolicy());
  }
  // clean up file during stress run
  boost::filesystem::remove(FLAGS_rp_state_file);
}

// empty rib policy state would not result in file saved
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyPolicy) {
  EXPECT_EQ(nullptr, rib_->pathSelectionPolicy_);
  EXPECT_EQ(nullptr, rib_->routeAttributePolicy_);
  EXPECT_EQ(nullptr, rib_->routeFilterPolicy_);
  rib_->saveRibPolicyState();
  EXPECT_FALSE(boost::filesystem::exists(FLAGS_rp_state_file));
}

// read a bad rib policy
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadRibPolicy) {
  TRibPolicyStore badPolicyStore;

  TPathSelectionPolicy tEmptyPolicy;
  tEmptyPolicy.statements()->emplace("stmt1", TPathSelectionStatement{});

  badPolicyStore.policy()->path_selection_policy() = tEmptyPolicy;

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          badPolicyStore));

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

// make saved file with bad termination string and read
TEST_F(
    RibPolicyRestoreTestFixture,
    ReadSaveRibPolicyStateTestBadTerminationString) {
  auto tMatcher = createCommunityMatch(200, 666, bgp_policy::Origin::EGP);
  auto tPathSelector = createTPathSlectorWithOneMatcher(tMatcher);
  auto tPathSelectionPolicy =
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector);

  TRibPolicyStore badPolicyStore;
  auto badTerm = "bad ending line";

  // make sure the timestamp is stale
  badPolicyStore.fileTermination() = badTerm;
  badPolicyStore.policy()->path_selection_policy() = tPathSelectionPolicy;

  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          badPolicyStore));

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

// make saved file with random content and read
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestBadFileContent) {
  // write to the file
  folly::writeFileAtomic(
      FLAGS_rp_state_file, "anything other than rib policy state");

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

// make empty saved file and read
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestEmptyFile) {
  // write to the file
  folly::writeFileAtomic(FLAGS_rp_state_file, "");

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

// file does not exist and read
TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTestMissingFile) {
  // remove file
  boost::filesystem::remove(FLAGS_rp_state_file);

  // verify reading rib policy will return nullptr
  EXPECT_EQ(nullptr, rib_->readRibPolicyState());
}

TEST_F(RibPolicyRestoreTestFixture, ReadSaveRibPolicyStateTest) {
  // Create the tPathSelectionPolicy for testing
  auto tMatcher = createCommunityMatch(200, 666, bgp_policy::Origin::EGP);
  auto tPathSelector = createTPathSlectorWithOneMatcher(tMatcher);

  auto tPathSelectionPolicy =
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector);
  PathSelectionPolicy expectedPsPolicy{tPathSelectionPolicy};

  // Create the tRouteAttributePolicy for testing
  auto prefix1 = folly::IPAddress::createNetwork("::/0");
  auto tRouteAttributePolicy = createTRouteAttributePolicyLbw(
      {prefix1},
      kLbw10G,
      "stmt1",
      seconds(std::time(nullptr)).count() + 60 /* 60s */);
  RouteAttributePolicy expectedRaPolicy{tRouteAttributePolicy};

  // Create RouteFilterPolicy for testing
  auto tRouteFilterPolicy =
      createTRouteFilterPolicy({createTRouteFilterStatement({prefix1})}, 12345);
  RouteFilterPolicy expectedRfPolicy{tRouteFilterPolicy};

  // make sure no file was generated at first place
  EXPECT_FALSE(boost::filesystem::exists(FLAGS_rp_state_file));

  // call set path selection policy and then save rib policy to file
  {
    auto ribResult = sendPathSelectionPolicySet(tPathSelectionPolicy);
    EXPECT_TRUE(*ribResult.success());

    // wait till policy output queue got item
    rib_->waitForPathSelectionPolicyUpdate();

    // save the policy to file
    rib_->saveRibPolicyState();
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));
  }

  // read path selection policy from file and compare
  {
    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_EQ(expectedPsPolicy, *policy->getPathSelectionPolicy());
    // no ra policy
    EXPECT_FALSE(policy->hasRouteAttributePolicy());
    EXPECT_FALSE(policy->hasRouteFilterPolicy());
  }

  // call set route attribute policy and then save rib policy to file
  {
    auto ribResult = sendRouteAttributePolicySet(tRouteAttributePolicy);
    EXPECT_TRUE(*ribResult.success());

    // wait till policy output queue got item
    rib_->waitForRouteAttributePolicyUpdate();

    // save the policy to file
    rib_->saveRibPolicyState();
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));
  }

  // read route attribute policy from file and compare
  {
    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_EQ(expectedPsPolicy, *policy->getPathSelectionPolicy());
    EXPECT_EQ(expectedRaPolicy, *policy->getRouteAttributePolicy());
    EXPECT_FALSE(policy->hasRouteFilterPolicy());
  }

  // call set route filter policy and then save rib policy to file
  {
    sendRouteFilterPolicySet(tRouteFilterPolicy);

    // wait till policy output queue got item
    rib_->waitForRouteFilterPolicyUpdate();

    // save the policy to file
    rib_->saveRibPolicyState();
    EXPECT_TRUE(boost::filesystem::exists(FLAGS_rp_state_file));
  }

  // read route filter policy from file and compare
  {
    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_EQ(expectedPsPolicy, *policy->getPathSelectionPolicy());
    EXPECT_EQ(expectedRaPolicy, *policy->getRouteAttributePolicy());
    EXPECT_EQ(expectedRfPolicy, *policy->getRouteFilterPolicy());
  }

  // clear path selection policy, verify other policies are untouched
  {
    rib_->clearPathSelectionPolicy();
    // wait till policy output queue got item
    rib_->waitForPathSelectionPolicyClear();

    rib_->saveRibPolicyState();

    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_FALSE(policy->hasPathSelectionPolicy());
    EXPECT_EQ(expectedRaPolicy, *policy->getRouteAttributePolicy());
    EXPECT_EQ(expectedRfPolicy, *policy->getRouteFilterPolicy());
  }

  // clear route attribute policy, verify other policies are untouched
  {
    rib_->clearRouteAttributePolicy();
    // wait till policy output queue got item
    rib_->waitForRouteAttributePolicyClear();

    rib_->saveRibPolicyState();

    auto policy = rib_->readRibPolicyState();
    EXPECT_NE(nullptr, policy);
    EXPECT_FALSE(policy->hasPathSelectionPolicy());
    EXPECT_FALSE(policy->hasRouteAttributePolicy());
    EXPECT_EQ(expectedRfPolicy, *policy->getRouteFilterPolicy());
  }
  // clear route filter policy, verify rib policy file is gone
  {
    rib_->clearRouteFilterPolicy();
    // wait till policy output queue got item
    rib_->waitForRouteFilterPolicyClear();

    rib_->saveRibPolicyState();

    auto policy = rib_->readRibPolicyState();
    EXPECT_EQ(nullptr, policy);
  }
}
TEST_F(RibPolicyRestoreTestFixture, SaveTRibPolicyStorePrettyPrintsJson) {
  const auto prefix1 = folly::IPAddress::createNetwork("1::/64");
  auto tRibPolicy = createTRibPolicyLbw({prefix1}, kLbw10G);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy() = tRibPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Read raw file content and verify it is pretty-printed (contains newlines)
  std::string fileContent;
  ASSERT_TRUE(folly::readFile(FLAGS_rp_state_file.c_str(), fileContent));
  EXPECT_NE(fileContent.find('\n'), std::string::npos);

  // Verify it is valid JSON that parses successfully
  auto parsed = folly::parseJson(fileContent);
  EXPECT_TRUE(parsed.isObject());

  // Verify round-trip: deserialized content matches original
  auto readBack =
      apache::thrift::SimpleJSONSerializer::deserialize<TRibPolicyStore>(
          fileContent);
  EXPECT_EQ(*tRibPolicyStore.fileTermination(), *readBack.fileTermination());
}

// Test isCrfFileModeEnabled / setCrfFileModeEnabled toggle
TEST_F(RibPolicyRestoreTestFixture, CrfFileModeToggle) {
  // default should be false (THRIFT_MODE)
  EXPECT_FALSE(rib_->isCrfFileModeEnabled());

  // set to FILE_MODE
  rib_->setCrfFileModeEnabled(true);
  EXPECT_TRUE(rib_->isCrfFileModeEnabled());

  // set back to THRIFT_MODE
  rib_->setCrfFileModeEnabled(false);
  EXPECT_FALSE(rib_->isCrfFileModeEnabled());

  // setting to same value should be a no-op (no crash)
  rib_->setCrfFileModeEnabled(false);
  EXPECT_FALSE(rib_->isCrfFileModeEnabled());
}

/*
 * The fb303 gauge bgpd.crf.file_mode_enabled must track the real CRF mode so
 * monitoring can distinguish FILE_MODE (1) from THRIFT_MODE (0). Regression
 * test: the gauge previously was never updated off its init value (0) because
 * the mode-flip path did not call BgpStatsDC::setCrfFileModeEnabled.
 */
TEST_F(RibPolicyRestoreTestFixture, CrfFileModeGaugeFollowsMode) {
  auto gauge = [] {
    auto* serviceData = facebook::fb303::ServiceData::get();
    return serviceData ? serviceData->getCounter("bgpd.crf.file_mode_enabled")
                       : 0;
  };

  rib_->setCrfFileModeEnabled(true);
  EXPECT_TRUE(rib_->isCrfFileModeEnabled());
  EXPECT_EQ(1, gauge());

  rib_->setCrfFileModeEnabled(false);
  EXPECT_FALSE(rib_->isCrfFileModeEnabled());
  EXPECT_EQ(0, gauge());

  /*
   * Re-setting the same value keeps the gauge consistent (the gauge is set
   * unconditionally, not only on transition).
   */
  rib_->setCrfFileModeEnabled(false);
  EXPECT_EQ(0, gauge());
}

/*
 * readThriftArtifactFromFile reports the read outcome on the error side of
 * folly::Expected so the bootstrap path can classify success/failure (an absent
 * artifact vs. a genuine read error) from a single read.
 */
TEST_F(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactReportsStatus) {
  // kAbsent: empty (unconfigured) path.
  {
    auto result = readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>("");
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(ArtifactReadError::kAbsent, result.error());
  }

  // kAbsent: configured path, but file does not exist.
  {
    auto tmpFile =
        fmt::format("/tmp/crf_test_{}_status_absent.json", ::getpid());
    boost::filesystem::remove(tmpFile);
    auto result =
        readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(ArtifactReadError::kAbsent, result.error());
  }

  // Success: file present and parseable.
  {
    auto tmpFile = fmt::format("/tmp/crf_test_{}_status_ok.json", ::getpid());
    rib_policy::CrfPolicyArtifact artifact;
    artifact.dryrun() = true;
    rib_policy::TRouteFilterPolicy policy;
    policy.version() = 7;
    artifact.policy() = policy;
    folly::writeFileAtomic(
        tmpFile,
        apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));
    auto result =
        readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
    EXPECT_TRUE(result.hasValue());
    boost::filesystem::remove(tmpFile);
  }

  // kError: file present but corrupt (unparseable).
  {
    auto tmpFile =
        fmt::format("/tmp/crf_test_{}_status_error.json", ::getpid());
    folly::writeFileAtomic(tmpFile, "not valid json");
    auto result =
        readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(ArtifactReadError::kError, result.error());
    boost::filesystem::remove(tmpFile);
  }
}

// readCrfPolicyFromArtifact: file does not exist
TEST_F(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactNoFile) {
  auto tmpFile = fmt::format("/tmp/crf_test_{}_nonexistent.json", ::getpid());
  boost::filesystem::remove(tmpFile);

  auto result =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  EXPECT_FALSE(result.hasValue());
}

// readCrfPolicyFromArtifact: valid artifact file
TEST_F(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactValidFile) {
  auto tmpFile = fmt::format("/tmp/crf_test_{}_valid.json", ::getpid());

  rib_policy::CrfPolicyArtifact artifact;
  artifact.dryrun() = false;
  rib_policy::TRouteFilterPolicy policy;
  policy.version() = 42;
  artifact.policy() = policy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  auto result =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(*result->dryrun());
  EXPECT_EQ(42, *result->policy()->version());

  boost::filesystem::remove(tmpFile);
}

// readCrfPolicyFromArtifact: malformed JSON content
TEST_F(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactMalformedJson) {
  auto tmpFile = fmt::format("/tmp/crf_test_{}_malformed.json", ::getpid());

  folly::writeFileAtomic(tmpFile, "this is not valid json");

  auto result =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  EXPECT_FALSE(result.hasValue());

  boost::filesystem::remove(tmpFile);
}

// readRibPolicyState with CRF artifact dryrun=false -> FILE_MODE
TEST_F(RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfDryrunFalse) {
  // Write a valid RibPolicyStore with route_filter_policy
  auto prefix = folly::IPAddress::createNetwork("::/0");
  auto tRouteFilterPolicy =
      createTRouteFilterPolicy({createTRouteFilterStatement({prefix})}, 100);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy()->route_filter_policy() = tRouteFilterPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Write CRF artifact with dryrun=false (FILE_MODE)
  auto tmpFile = fmt::format("/tmp/crf_test_{}_dryrun_false.json", ::getpid());

  rib_policy::CrfPolicyArtifact artifact;
  artifact.dryrun() = false;
  rib_policy::TRouteFilterPolicy crfPolicy;
  crfPolicy.version() = 999;
  artifact.policy() = crfPolicy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Resolve CRF policy using production helper
  auto crfRead =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  std::optional<rib_policy::CrfPolicyArtifact> tCrfArtifact;
  if (crfRead.hasValue()) {
    tCrfArtifact = std::move(crfRead.value());
  }
  auto [ribPolicy, crfFileMode] =
      RibDC::resolveCrfPolicy(rib_->readRibPolicyState(), tCrfArtifact);
  EXPECT_NE(nullptr, ribPolicy);

  // Route filter policy should come from artifact (version 999), not cache
  // (100)
  EXPECT_EQ(999, ribPolicy->getRouteFilterPolicy()->getVersion());

  rib_->setCrfFileModeEnabled(crfFileMode);
  EXPECT_TRUE(rib_->isCrfFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// readRibPolicyState with CRF artifact dryrun=true -> THRIFT_MODE
TEST_F(RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfDryrunTrue) {
  // Write a valid RibPolicyStore with route_filter_policy
  auto prefix = folly::IPAddress::createNetwork("::/0");
  auto tRouteFilterPolicy =
      createTRouteFilterPolicy({createTRouteFilterStatement({prefix})}, 200);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy()->route_filter_policy() = tRouteFilterPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Write CRF artifact with dryrun=true (THRIFT_MODE)
  auto tmpFile = fmt::format("/tmp/crf_test_{}_dryrun_true.json", ::getpid());

  rib_policy::CrfPolicyArtifact artifact;
  artifact.dryrun() = true;
  rib_policy::TRouteFilterPolicy crfPolicy;
  crfPolicy.version() = 888;
  artifact.policy() = crfPolicy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Resolve CRF policy using production helper
  auto crfRead =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  std::optional<rib_policy::CrfPolicyArtifact> tCrfArtifact;
  if (crfRead.hasValue()) {
    tCrfArtifact = std::move(crfRead.value());
  }
  auto [ribPolicy, crfFileMode] =
      RibDC::resolveCrfPolicy(rib_->readRibPolicyState(), tCrfArtifact);
  EXPECT_NE(nullptr, ribPolicy);

  // Route filter policy should come from cache (version 200), not artifact
  EXPECT_EQ(200, ribPolicy->getRouteFilterPolicy()->getVersion());

  rib_->setCrfFileModeEnabled(crfFileMode);
  // CRF file mode should NOT be enabled (dryrun=true)
  EXPECT_FALSE(rib_->isCrfFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// CRF artifact exists with dryrun=false but no rib policy store file.
// readRibPolicyState should return nullptr, and crfFileModeEnabled should
// remain false (not FILE_MODE with no active policy).
TEST_F(
    RibPolicyRestoreTestFixture,
    ReadRibPolicyStateCrfArtifactNoPolicyStore) {
  // Ensure no policy store file exists
  boost::filesystem::remove(FLAGS_rp_state_file);

  // Write CRF artifact with dryrun=false
  auto tmpFile = fmt::format("/tmp/crf_test_{}_no_store.json", ::getpid());

  rib_policy::CrfPolicyArtifact artifact;
  artifact.dryrun() = false;
  rib_policy::TRouteFilterPolicy crfPolicy;
  crfPolicy.version() = 777;
  artifact.policy() = crfPolicy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Read artifact — should succeed
  auto crfRead =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  ASSERT_TRUE(crfRead.hasValue());
  std::optional<rib_policy::CrfPolicyArtifact> tCrfArtifact =
      std::move(crfRead.value());
  EXPECT_FALSE(*tCrfArtifact->dryrun());

  // Resolve CRF policy using production helper
  auto [ribPolicy, crfFileMode] =
      RibDC::resolveCrfPolicy(rib_->readRibPolicyState(), tCrfArtifact);

  ASSERT_NE(nullptr, ribPolicy);
  EXPECT_TRUE(ribPolicy->hasRouteFilterPolicy());
  EXPECT_EQ(777, ribPolicy->getRouteFilterPolicy()->getVersion());

  rib_->setCrfFileModeEnabled(crfFileMode);
  EXPECT_TRUE(rib_->isCrfFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// readCrfPolicyFromArtifact: empty artifact file (0 bytes)
TEST_F(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactEmptyFile) {
  auto tmpFile = fmt::format("/tmp/crf_test_{}_empty.json", ::getpid());

  folly::writeFileAtomic(tmpFile, "");

  auto result =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  EXPECT_FALSE(result.hasValue());

  boost::filesystem::remove(tmpFile);
}

// readCrfPolicyFromArtifact: valid artifact with dryrun=true
TEST_F(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactDryrunTrue) {
  auto tmpFile =
      fmt::format("/tmp/crf_test_{}_dryrun_true_standalone.json", ::getpid());

  rib_policy::CrfPolicyArtifact artifact;
  artifact.dryrun() = true;
  rib_policy::TRouteFilterPolicy policy;
  policy.version() = 55;
  artifact.policy() = policy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  auto result =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(*result->dryrun());
  EXPECT_EQ(55, *result->policy()->version());

  boost::filesystem::remove(tmpFile);
}

// readRibPolicyState with CRF artifact dryrun=false but policy store has no
// route_filter_policy. The artifact's CRF should be injected into the policy.
TEST_F(RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfNoCachedPolicy) {
  // Write a valid RibPolicyStore with only path_selection_policy (no CRF)
  auto tPathSelectionPolicy =
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, TPathSelector());

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy()->path_selection_policy() = tPathSelectionPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Write CRF artifact with dryrun=false
  auto tmpFile = fmt::format("/tmp/crf_test_{}_no_cached_crf.json", ::getpid());

  rib_policy::CrfPolicyArtifact artifact;
  artifact.dryrun() = false;
  rib_policy::TRouteFilterPolicy crfPolicy;
  crfPolicy.version() = 500;
  artifact.policy() = crfPolicy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Resolve CRF policy using production helper
  auto crfRead =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  std::optional<rib_policy::CrfPolicyArtifact> tCrfArtifact;
  if (crfRead.hasValue()) {
    tCrfArtifact = std::move(crfRead.value());
  }
  auto [ribPolicy, crfFileMode] =
      RibDC::resolveCrfPolicy(rib_->readRibPolicyState(), tCrfArtifact);

  // CRF should be injected from artifact (version 500) even though store
  // had no route_filter_policy
  EXPECT_TRUE(ribPolicy->hasRouteFilterPolicy());
  EXPECT_EQ(500, ribPolicy->getRouteFilterPolicy()->getVersion());
  // Path selection policy from store should still be present
  EXPECT_TRUE(ribPolicy->hasPathSelectionPolicy());

  rib_->setCrfFileModeEnabled(crfFileMode);
  EXPECT_TRUE(rib_->isCrfFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// Full integration test replicating the Rib constructor's CRF startup sequence:
// readCrfPolicyFromArtifact -> readRibPolicyState -> setCrfFileModeEnabled
TEST_F(RibPolicyRestoreTestFixture, ReadRibPolicyStateCrfFullIntegration) {
  // Set up policy store with a CRF (version 100)
  auto prefix = folly::IPAddress::createNetwork("::/0");
  auto tRouteFilterPolicy =
      createTRouteFilterPolicy({createTRouteFilterStatement({prefix})}, 100);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy()->route_filter_policy() = tRouteFilterPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Set up CRF artifact with dryrun=false (version 300)
  auto tmpFile = fmt::format("/tmp/crf_test_{}_integration.json", ::getpid());

  rib_policy::CrfPolicyArtifact artifact;
  artifact.dryrun() = false;
  rib_policy::TRouteFilterPolicy crfPolicy;
  crfPolicy.version() = 300;
  artifact.policy() = crfPolicy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Resolve CRF policy using production helper
  auto crfRead =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  std::optional<rib_policy::CrfPolicyArtifact> tCrfArtifact;
  if (crfRead.hasValue()) {
    tCrfArtifact = std::move(crfRead.value());
  }
  auto [ribPolicy, crfFileMode] =
      RibDC::resolveCrfPolicy(rib_->readRibPolicyState(), tCrfArtifact);

  rib_->setCrfFileModeEnabled(crfFileMode);

  // Verify final state
  ASSERT_NE(nullptr, ribPolicy);
  EXPECT_EQ(300, ribPolicy->getRouteFilterPolicy()->getVersion());
  EXPECT_TRUE(rib_->isCrfFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// readThriftArtifactFromFile: empty file path -> kAbsent
TEST_F(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactEmptyPath) {
  auto result = readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>("");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(ArtifactReadError::kAbsent, result.error());
}

// readCrfPolicyFromArtifact: file exists but is unreadable (permissions)
TEST_F(RibPolicyRestoreTestFixture, ReadCrfPolicyFromArtifactUnreadableFile) {
  if (::geteuid() == 0) {
    GTEST_SKIP()
        << "chmod-based permission test is bypassed when running as root";
  }
  auto tmpFile = fmt::format("/tmp/crf_test_{}_unreadable.json", ::getpid());

  folly::writeFileAtomic(tmpFile, "valid content");
  boost::filesystem::permissions(tmpFile, boost::filesystem::perms::no_perms);

  /*
   * File is present but unreadable -> kError (a genuine read failure), not
   * kAbsent.
   */
  auto result =
      readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(tmpFile);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(ArtifactReadError::kError, result.error());

  boost::filesystem::permissions(tmpFile, boost::filesystem::perms::owner_all);
  boost::filesystem::remove(tmpFile);
}

namespace {
// Build a CPS policy for one prefix with the given version. The shared
// createTPathSelectionPolicyWithPathSelector helper hardcodes version 1, so we
// override the version field to drive the artifact-vs-cache selection in the
// resolveCpsPolicy tests.
rib_policy::TPathSelectionPolicy makeCpsPolicy(
    const folly::CIDRNetwork& prefix,
    int64_t version) {
  auto policy = createTPathSelectionPolicyWithPathSelector(
      {prefix}, rib_policy::TPathSelector());
  policy.version() = version;
  return policy;
}
} // namespace

// setCpsFileModeEnabled / isCpsFileModeEnabled toggle.
TEST_F(RibPolicyRestoreTestFixture, CpsFileModeToggle) {
  // default should be false (THRIFT_MODE)
  EXPECT_FALSE(rib_->isCpsFileModeEnabled());

  // set to FILE_MODE
  rib_->setCpsFileModeEnabled(true);
  EXPECT_TRUE(rib_->isCpsFileModeEnabled());

  // set back to THRIFT_MODE
  rib_->setCpsFileModeEnabled(false);
  EXPECT_FALSE(rib_->isCpsFileModeEnabled());

  // setting to same value should be a no-op (no crash)
  rib_->setCpsFileModeEnabled(false);
  EXPECT_FALSE(rib_->isCpsFileModeEnabled());
}

/*
 * The fb303 gauge bgpd.cps.file_mode_enabled must track the real CPS mode so
 * monitoring can distinguish FILE_MODE (1) from THRIFT_MODE (0). Mirrors the
 * CRF gauge regression test (CrfFileModeGaugeFollowsMode).
 */
TEST_F(RibPolicyRestoreTestFixture, CpsFileModeGaugeFollowsMode) {
  auto gauge = [] {
    auto* serviceData = facebook::fb303::ServiceData::get();
    return serviceData ? serviceData->getCounter("bgpd.cps.file_mode_enabled")
                       : 0;
  };

  rib_->setCpsFileModeEnabled(true);
  EXPECT_TRUE(rib_->isCpsFileModeEnabled());
  EXPECT_EQ(1, gauge());

  rib_->setCpsFileModeEnabled(false);
  EXPECT_FALSE(rib_->isCpsFileModeEnabled());
  EXPECT_EQ(0, gauge());

  /*
   * Re-setting the same value keeps the gauge consistent (the gauge is set
   * unconditionally, not only on transition).
   */
  rib_->setCpsFileModeEnabled(false);
  EXPECT_EQ(0, gauge());
}

// readCpsPolicyFromArtifact: file does not exist
TEST_F(RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactNoFile) {
  auto tmpFile = fmt::format("/tmp/cps_test_{}_nonexistent.json", ::getpid());
  boost::filesystem::remove(tmpFile);

  auto result =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  EXPECT_FALSE(result.hasValue());
}

// readCpsPolicyFromArtifact: valid artifact file
TEST_F(RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactValidFile) {
  auto tmpFile = fmt::format("/tmp/cps_test_{}_valid.json", ::getpid());

  rib_policy::CpsPolicyArtifact artifact;
  artifact.dryrun() = false;
  rib_policy::TPathSelectionPolicy policy;
  policy.version() = 42;
  artifact.policy() = policy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  auto result =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  ASSERT_TRUE(result.hasValue());
  EXPECT_FALSE(*result->dryrun());
  EXPECT_EQ(42, *result->policy()->version());

  boost::filesystem::remove(tmpFile);
}

// readCpsPolicyFromArtifact: malformed JSON content
TEST_F(RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactMalformedJson) {
  auto tmpFile = fmt::format("/tmp/cps_test_{}_malformed.json", ::getpid());

  folly::writeFileAtomic(tmpFile, "this is not valid json");

  auto result =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  EXPECT_FALSE(result.hasValue());

  boost::filesystem::remove(tmpFile);
}

// readCpsPolicyFromArtifact: empty artifact file (0 bytes)
TEST_F(RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactEmptyFile) {
  auto tmpFile = fmt::format("/tmp/cps_test_{}_empty.json", ::getpid());

  folly::writeFileAtomic(tmpFile, "");

  auto result =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  EXPECT_FALSE(result.hasValue());

  boost::filesystem::remove(tmpFile);
}

// readCpsPolicyFromArtifact: valid artifact with dryrun=true
TEST_F(RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactDryrunTrue) {
  auto tmpFile =
      fmt::format("/tmp/cps_test_{}_dryrun_true_standalone.json", ::getpid());

  rib_policy::CpsPolicyArtifact artifact;
  artifact.dryrun() = true;
  rib_policy::TPathSelectionPolicy policy;
  policy.version() = 55;
  artifact.policy() = policy;

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  auto result =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  ASSERT_TRUE(result.hasValue());
  EXPECT_TRUE(*result->dryrun());
  EXPECT_EQ(55, *result->policy()->version());

  boost::filesystem::remove(tmpFile);
}

// readThriftArtifactFromFile: empty file path -> kAbsent
TEST_F(RibPolicyRestoreTestFixture, ReadCpsPolicyFromArtifactEmptyPath) {
  auto result = readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>("");
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(ArtifactReadError::kAbsent, result.error());
}

// resolveCpsPolicy with CPS artifact dryrun=false -> FILE_MODE: the artifact
// policy replaces the cached one.
TEST_F(RibPolicyRestoreTestFixture, ReadRibPolicyStateCpsDryrunFalse) {
  // Write a valid RibPolicyStore with path_selection_policy (version 100)
  auto tPathSelectionPolicy = makeCpsPolicy(kV4Prefix1, 100);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy()->path_selection_policy() = tPathSelectionPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Write CPS artifact with dryrun=false (FILE_MODE), version 999
  auto tmpFile = fmt::format("/tmp/cps_test_{}_dryrun_false.json", ::getpid());

  rib_policy::CpsPolicyArtifact artifact;
  artifact.dryrun() = false;
  artifact.policy() = makeCpsPolicy(kV4Prefix1, 999);

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Resolve CPS policy using production helper
  auto cpsRead =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  std::optional<rib_policy::CpsPolicyArtifact> tCpsArtifact;
  if (cpsRead.hasValue()) {
    tCpsArtifact = std::move(cpsRead.value());
  }
  auto [ribPolicy, cpsFileMode] =
      RibDC::resolveCpsPolicy(rib_->readRibPolicyState(), tCpsArtifact);
  EXPECT_NE(nullptr, ribPolicy);

  // Path selection policy should come from artifact (version 999), not cache
  // (100)
  EXPECT_EQ(999, ribPolicy->getPathSelectionPolicy()->getVersion());

  rib_->setCpsFileModeEnabled(cpsFileMode);
  EXPECT_TRUE(rib_->isCpsFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// resolveCpsPolicy with CPS artifact dryrun=true -> THRIFT_MODE: the cached
// policy is kept unchanged.
TEST_F(RibPolicyRestoreTestFixture, ReadRibPolicyStateCpsDryrunTrue) {
  // Write a valid RibPolicyStore with path_selection_policy (version 200)
  auto tPathSelectionPolicy = makeCpsPolicy(kV4Prefix1, 200);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy()->path_selection_policy() = tPathSelectionPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Write CPS artifact with dryrun=true (THRIFT_MODE), version 888
  auto tmpFile = fmt::format("/tmp/cps_test_{}_dryrun_true.json", ::getpid());

  rib_policy::CpsPolicyArtifact artifact;
  artifact.dryrun() = true;
  artifact.policy() = makeCpsPolicy(kV4Prefix1, 888);

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Resolve CPS policy using production helper
  auto cpsRead =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  std::optional<rib_policy::CpsPolicyArtifact> tCpsArtifact;
  if (cpsRead.hasValue()) {
    tCpsArtifact = std::move(cpsRead.value());
  }
  auto [ribPolicy, cpsFileMode] =
      RibDC::resolveCpsPolicy(rib_->readRibPolicyState(), tCpsArtifact);
  EXPECT_NE(nullptr, ribPolicy);

  // Path selection policy should come from cache (version 200), not artifact
  EXPECT_EQ(200, ribPolicy->getPathSelectionPolicy()->getVersion());

  rib_->setCpsFileModeEnabled(cpsFileMode);
  // CPS file mode should NOT be enabled (dryrun=true)
  EXPECT_FALSE(rib_->isCpsFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// CPS artifact exists with dryrun=false but no rib policy store file.
// readRibPolicyState returns nullptr; resolveCpsPolicy should create a fresh
// policy holding only the artifact CPS and enable FILE_MODE.
TEST_F(
    RibPolicyRestoreTestFixture,
    ReadRibPolicyStateCpsArtifactNoPolicyStore) {
  // Ensure no policy store file exists
  boost::filesystem::remove(FLAGS_rp_state_file);

  // Write CPS artifact with dryrun=false (version 777)
  auto tmpFile = fmt::format("/tmp/cps_test_{}_no_store.json", ::getpid());

  rib_policy::CpsPolicyArtifact artifact;
  artifact.dryrun() = false;
  artifact.policy() = makeCpsPolicy(kV4Prefix1, 777);

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Read artifact — should succeed
  auto cpsRead =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  ASSERT_TRUE(cpsRead.hasValue());
  std::optional<rib_policy::CpsPolicyArtifact> tCpsArtifact =
      std::move(cpsRead.value());
  EXPECT_FALSE(*tCpsArtifact->dryrun());

  // Resolve CPS policy using production helper
  auto [ribPolicy, cpsFileMode] =
      RibDC::resolveCpsPolicy(rib_->readRibPolicyState(), tCpsArtifact);

  ASSERT_NE(nullptr, ribPolicy);
  EXPECT_TRUE(ribPolicy->hasPathSelectionPolicy());
  EXPECT_EQ(777, ribPolicy->getPathSelectionPolicy()->getVersion());

  rib_->setCpsFileModeEnabled(cpsFileMode);
  EXPECT_TRUE(rib_->isCpsFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// resolveCpsPolicy with CPS artifact dryrun=false but policy store has no
// path_selection_policy (only route_filter_policy). The artifact's CPS should
// be injected while the cached CRF is preserved.
TEST_F(RibPolicyRestoreTestFixture, ReadRibPolicyStateCpsNoCachedPolicy) {
  // Write a valid RibPolicyStore with only route_filter_policy (no CPS)
  auto prefix = folly::IPAddress::createNetwork("::/0");
  auto tRouteFilterPolicy =
      createTRouteFilterPolicy({createTRouteFilterStatement({prefix})}, 100);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy()->route_filter_policy() = tRouteFilterPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Write CPS artifact with dryrun=false (version 500)
  auto tmpFile = fmt::format("/tmp/cps_test_{}_no_cached_cps.json", ::getpid());

  rib_policy::CpsPolicyArtifact artifact;
  artifact.dryrun() = false;
  artifact.policy() = makeCpsPolicy(kV4Prefix1, 500);

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Resolve CPS policy using production helper
  auto cpsRead =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  std::optional<rib_policy::CpsPolicyArtifact> tCpsArtifact;
  if (cpsRead.hasValue()) {
    tCpsArtifact = std::move(cpsRead.value());
  }
  auto [ribPolicy, cpsFileMode] =
      RibDC::resolveCpsPolicy(rib_->readRibPolicyState(), tCpsArtifact);

  // CPS should be injected from artifact (version 500) even though store had no
  // path_selection_policy
  EXPECT_TRUE(ribPolicy->hasPathSelectionPolicy());
  EXPECT_EQ(500, ribPolicy->getPathSelectionPolicy()->getVersion());
  // Route filter policy from store should still be present
  EXPECT_TRUE(ribPolicy->hasRouteFilterPolicy());

  rib_->setCpsFileModeEnabled(cpsFileMode);
  EXPECT_TRUE(rib_->isCpsFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

// Full integration test replicating the Rib constructor's CPS startup sequence:
// read artifact -> readRibPolicyState -> resolveCpsPolicy ->
// setCpsFileModeEnabled
TEST_F(RibPolicyRestoreTestFixture, ReadRibPolicyStateCpsFullIntegration) {
  // Set up policy store with a CPS (version 100)
  auto tPathSelectionPolicy = makeCpsPolicy(kV4Prefix1, 100);

  TRibPolicyStore tRibPolicyStore;
  tRibPolicyStore.fileTermination() = kRibPolicyFileTermination;
  tRibPolicyStore.policy()->path_selection_policy() = tPathSelectionPolicy;
  rib_->saveTRibPolicyStore(tRibPolicyStore);

  // Set up CPS artifact with dryrun=false (version 300)
  auto tmpFile = fmt::format("/tmp/cps_test_{}_integration.json", ::getpid());

  rib_policy::CpsPolicyArtifact artifact;
  artifact.dryrun() = false;
  artifact.policy() = makeCpsPolicy(kV4Prefix1, 300);

  folly::writeFileAtomic(
      tmpFile,
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(artifact));

  // Resolve CPS policy using production helper
  auto cpsRead =
      readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(tmpFile);
  std::optional<rib_policy::CpsPolicyArtifact> tCpsArtifact;
  if (cpsRead.hasValue()) {
    tCpsArtifact = std::move(cpsRead.value());
  }
  auto [ribPolicy, cpsFileMode] =
      RibDC::resolveCpsPolicy(rib_->readRibPolicyState(), tCpsArtifact);

  rib_->setCpsFileModeEnabled(cpsFileMode);

  // Verify final state
  ASSERT_NE(nullptr, ribPolicy);
  EXPECT_EQ(300, ribPolicy->getPathSelectionPolicy()->getVersion());
  EXPECT_TRUE(rib_->isCpsFileModeEnabled());

  boost::filesystem::remove(tmpFile);
}

} // namespace facebook::bgp
