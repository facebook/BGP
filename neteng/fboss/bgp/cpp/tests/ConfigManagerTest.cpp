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

#include <boost/filesystem.hpp>
#include <folly/FileUtil.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/tests/ConfigTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/PeerManagerTestUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

using namespace facebook::nettools::bgplib;
using namespace facebook::bgp::thrift;

// Helper function for ConfigManager tests that returns non-optional policy map.
// ConfigManager's updatePeerPolicies/updatePeerGroupPolicies take non-optional
// strings because they're about setting policies (not clearing).
// Empty string means "don't update this direction".
std::unique_ptr<std::map<
    std::string,
    std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>>>
createConfigManagerPolicyMap(
    const std::vector<std::tuple<std::string, std::string, std::string>>&
        policyMapEntries) {
  auto policyMap = std::make_unique<std::map<
      std::string,
      std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>>>();

  for (const auto& [key, ingressPolicy, egressPolicy] : policyMapEntries) {
    if (!ingressPolicy.empty()) {
      (*policyMap)[key][facebook::bgp::bgp_policy::DIRECTION::IN] =
          ingressPolicy;
    }
    if (!egressPolicy.empty()) {
      (*policyMap)[key][facebook::bgp::bgp_policy::DIRECTION::OUT] =
          egressPolicy;
    }
  }

  return policyMap;
}

// Test fixture for file operations with automatic directory management
class ConfigManagerFileTestFixture : public ConfigTestFixture {
 protected:
  boost::filesystem::path testDir_;
  boost::filesystem::path configFilePath_;
  boost::filesystem::path backupFilePath_;

  void SetUp() override {
    ConfigTestFixture::SetUp();

    // Create temporary directory for testing
    testDir_ = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("bgp_config_test_%%%%-%%%%-%%%%-%%%%");
    boost::filesystem::create_directories(testDir_);

    // Create a temporary config file path
    configFilePath_ = testDir_ / "test_config.json";

    // Set backup file path
    backupFilePath_ = testDir_ / "test_config_backup";
    FLAGS_bgp_config_backup = backupFilePath_.string();
  }

  void TearDown() override {
    // Cleanup temporary directory
    if (boost::filesystem::exists(testDir_)) {
      boost::filesystem::remove_all(testDir_);
    }

    ConfigTestFixture::TearDown();
  }

  // Helper method to write initial config to file
  void writeInitialConfigToFile() {
    auto initialConfig = std::make_shared<const Config>(defaultConfig_);
    std::string initialConfigStr =
        apache::thrift::SimpleJSONSerializer::serialize<std::string>(
            defaultConfig_);
    folly::writeFileAtomic(configFilePath_.string(), initialConfigStr);
  }
};

TEST_F(ConfigTestFixture, ConfigManagerBasicConstructionAndGetConfig) {
  // Create initial config using defaultConfig_ from ConfigTestFixture
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);

  // Create ConfigManager
  ConfigManager configManager(initialConfig);

  // Test basic getConfig functionality
  auto retrievedConfig = configManager.getConfig();
  ASSERT_NE(retrievedConfig, nullptr);
  EXPECT_EQ(
      retrievedConfig->getConfig().router_id(), defaultConfig_.router_id());
  EXPECT_EQ(
      retrievedConfig->getConfig().local_as_4_byte(),
      defaultConfig_.local_as_4_byte());
}

TEST_F(ConfigTestFixture, ConfigManagerUpdatePeerPoliciesBasic) {
  // Create initial config using defaultConfig_ from ConfigTestFixture
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Prepare peer policy updates using createPolicyMap helper
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr3.str(), "NEW_INGRESS_POLICY", "NEW_EGRESS_POLICY"},
       {kPeerAddr4.str(), "UPDATED_PEER2_INGRESS", ""}});

  // Call updatePeerPolicies
  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);

  ASSERT_NE(updatedConfig, nullptr);

  // Verify the config was updated in the manager
  auto retrievedConfig = configManager.getConfig();
  EXPECT_EQ(retrievedConfig, updatedConfig);

  // Check that policies were updated correctly
  auto& peers = *retrievedConfig->getConfig().peers();

  // Define expected policies per peer address
  struct ExpectedPeerPolicy {
    std::string ingressPolicy;
    std::string egressPolicy;
    bool found = false;
  };

  std::map<std::string, ExpectedPeerPolicy> expectedPolicies = {
      {kPeerAddr3.str(), {"NEW_INGRESS_POLICY", "NEW_EGRESS_POLICY"}},
      // Egress policy should remain unchanged (staticPeer2_ has its own
      // policies)
      {kPeerAddr4.str(), {"UPDATED_PEER2_INGRESS", kEgressPolicyName}},
  };

  for (const auto& peer : peers) {
    auto it = expectedPolicies.find(*peer.peer_addr());
    if (it == expectedPolicies.end()) {
      continue;
    }

    auto& expected = it->second;
    expected.found = true;
    EXPECT_EQ(*peer.ingress_policy_name(), expected.ingressPolicy);
    EXPECT_EQ(*peer.egress_policy_name(), expected.egressPolicy);
  }

  // Verify all expected peers were found
  for (const auto& [peerAddr, expected] : expectedPolicies) {
    EXPECT_TRUE(expected.found) << "Peer " << peerAddr << " was not found";
  }
}

TEST_F(ConfigTestFixture, ConfigManagerUpdatePeerPoliciesIngressOnly) {
  // Create initial config
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Update only ingress policy for one peer using createPolicyMap helper
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr3.str(), "INGRESS_ONLY_UPDATE", ""}});

  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);

  // Verify only ingress policy was updated
  auto& peers = *updatedConfig->getConfig().peers();
  for (const auto& peer : peers) {
    if (*peer.peer_addr() == kPeerAddr3.str()) {
      EXPECT_EQ(*peer.ingress_policy_name(), "INGRESS_ONLY_UPDATE");
      // Egress policy should remain from peer (staticPeer1_ has its own
      // policies)
      EXPECT_EQ(*peer.egress_policy_name(), kEgressPolicyName);
      break;
    }
  }
}

TEST_F(ConfigTestFixture, ConfigManagerUpdatePeerPoliciesEgressOnly) {
  // Create initial config
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Update only egress policy for one peer using createPolicyMap helper
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr4.str(), "", "EGRESS_ONLY_UPDATE"}});

  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);

  // Verify only egress policy was updated
  auto& peers = *updatedConfig->getConfig().peers();
  for (const auto& peer : peers) {
    if (*peer.peer_addr() == kPeerAddr4.str()) {
      // Ingress policy should remain unchanged (staticPeer2_ has no ingress
      // policy by default)
      EXPECT_FALSE(peer.ingress_policy_name().has_value());
      EXPECT_EQ(*peer.egress_policy_name(), "EGRESS_ONLY_UPDATE");
      break;
    }
  }
}

TEST_F(ConfigTestFixture, ConfigManagerUpdatePeerGroupPoliciesBasic) {
  // Create initial config
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Prepare peer group policy updates using createPolicyMap helper
  auto policyMap = createConfigManagerPolicyMap(
      {{"PEERGROUP_RSW_CSW_V4", "NEW_GROUP1_INGRESS", "NEW_GROUP1_EGRESS"}});

  // Call updatePeerGroupPolicies
  auto updatedConfig = configManager.updatePeerGroupPolicies(*policyMap);

  ASSERT_NE(updatedConfig, nullptr);

  // Verify the config was updated in the manager
  auto retrievedConfig = configManager.getConfig();
  EXPECT_EQ(retrievedConfig, updatedConfig);

  // Check that peer group policies were updated correctly
  auto& peerGroups = *retrievedConfig->getConfig().peer_groups();

  bool foundGroup = false;

  for (const auto& peerGroup : peerGroups) {
    if (*peerGroup.name() == "PEERGROUP_RSW_CSW_V4") {
      foundGroup = true;
      EXPECT_EQ(*peerGroup.ingress_policy_name(), "NEW_GROUP1_INGRESS");
      EXPECT_EQ(*peerGroup.egress_policy_name(), "NEW_GROUP1_EGRESS");
    }
  }

  EXPECT_TRUE(foundGroup);
}

TEST_F(ConfigTestFixture, ConfigManagerUpdatePeerGroupPoliciesIngressOnly) {
  // Create initial config
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Update only ingress policy for one peer group using createPolicyMap helper
  auto policyMap = createConfigManagerPolicyMap(
      {{"PEERGROUP_RSW_CSW_V4", "GROUP1_INGRESS_ONLY", ""}});

  auto updatedConfig = configManager.updatePeerGroupPolicies(*policyMap);

  // Verify only ingress policy was updated
  auto& peerGroups = *updatedConfig->getConfig().peer_groups();
  for (const auto& peerGroup : peerGroups) {
    if (*peerGroup.name() == "PEERGROUP_RSW_CSW_V4") {
      EXPECT_EQ(*peerGroup.ingress_policy_name(), "GROUP1_INGRESS_ONLY");
      // Egress policy should remain unchanged
      EXPECT_EQ(*peerGroup.egress_policy_name(), "RSW_CSW_OUT");
      break;
    }
  }
}

TEST_F(ConfigTestFixture, ConfigManagerUpdatePeerGroupPoliciesUpdatesPeers) {
  // Create initial config
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Update peer group policy -
  // only the group config should be updated. Peers inherit via getValue()
  // resolution in Config constructor.
  auto policyMap = createConfigManagerPolicyMap(
      {{"PEERGROUP_RSW_CSW_V4", "PROPAGATED_INGRESS", "PROPAGATED_EGRESS"}});

  auto updatedConfig = configManager.updatePeerGroupPolicies(*policyMap);

  ASSERT_NE(updatedConfig, nullptr);

  // Verify peer group policies were updated
  auto& peerGroups = *updatedConfig->getConfig().peer_groups();
  bool foundGroup = false;
  for (const auto& peerGroup : peerGroups) {
    if (*peerGroup.name() == "PEERGROUP_RSW_CSW_V4") {
      foundGroup = true;
      EXPECT_EQ(*peerGroup.ingress_policy_name(), "PROPAGATED_INGRESS");
      EXPECT_EQ(*peerGroup.egress_policy_name(), "PROPAGATED_EGRESS");
    }
  }
  EXPECT_TRUE(foundGroup);

  // Verify peer config fields are NOT modified (no group-to-peer
  // propagation). Peers inherit group policy via getValue() in Config
  // constructor.
  auto& peers = *updatedConfig->getConfig().peers();

  struct ExpectedPeerPolicy {
    std::optional<std::string> ingressPolicy;
    std::optional<std::string> egressPolicy;
    bool found = false;
  };

  std::map<std::string, ExpectedPeerPolicy> expectedRawPolicies = {
      // staticPeer3_ belongs to group, has NO peer-level policies
      {kPeerAddr5.str(), {std::nullopt, std::nullopt}},
      // staticPeer4_ belongs to group, HAS explicit peer-level policies
      {kPeerAddr6.str(), {{kIngressPolicyName}, {kEgressPolicyName}}},
      // staticPeer1_ and staticPeer2_ do NOT belong to the peer group
      {kPeerAddr3.str(), {{kIngressPolicyName}, {kEgressPolicyName}}},
      {kPeerAddr4.str(), {std::nullopt, {kEgressPolicyName}}},
  };

  for (const auto& peer : peers) {
    auto it = expectedRawPolicies.find(*peer.peer_addr());
    if (it == expectedRawPolicies.end()) {
      continue;
    }

    auto& expected = it->second;
    expected.found = true;

    if (expected.ingressPolicy.has_value()) {
      ASSERT_TRUE(peer.ingress_policy_name().has_value())
          << "Peer " << *peer.peer_addr() << " missing ingress policy";
      EXPECT_EQ(*peer.ingress_policy_name(), *expected.ingressPolicy);
    } else {
      EXPECT_FALSE(peer.ingress_policy_name().has_value())
          << "Peer " << *peer.peer_addr()
          << " should not have ingress policy set";
    }

    if (expected.egressPolicy.has_value()) {
      ASSERT_TRUE(peer.egress_policy_name().has_value())
          << "Peer " << *peer.peer_addr() << " missing egress policy";
      EXPECT_EQ(*peer.egress_policy_name(), *expected.egressPolicy);
    } else {
      EXPECT_FALSE(peer.egress_policy_name().has_value())
          << "Peer " << *peer.peer_addr()
          << " should not have egress policy set";
    }
  }

  // Verify all expected peers were found
  for (const auto& [peerAddr, expected] : expectedRawPolicies) {
    EXPECT_TRUE(expected.found) << "Peer " << peerAddr << " was not found";
  }

  // Verify resolved policies via getPeerToConfig() (getValue() resolution).
  // staticPeer3_ (no peer-level policy) should resolve to group's new policy.
  // staticPeer4_ (has peer-level policy) should keep its explicit policy.
  auto& peerToConfig = updatedConfig->getPeerToConfig();

  auto peer3It = peerToConfig.find(kPeerAddr5);
  ASSERT_NE(peer3It, peerToConfig.end());
  EXPECT_EQ(
      peer3It->second->commonPeerGroupConfig.ingressPolicyName,
      "PROPAGATED_INGRESS");
  EXPECT_EQ(
      peer3It->second->commonPeerGroupConfig.egressPolicyName,
      "PROPAGATED_EGRESS");

  auto peer4It = peerToConfig.find(kPeerAddr6);
  ASSERT_NE(peer4It, peerToConfig.end());
  EXPECT_EQ(
      peer4It->second->commonPeerGroupConfig.ingressPolicyName,
      kIngressPolicyName);
  EXPECT_EQ(
      peer4It->second->commonPeerGroupConfig.egressPolicyName,
      kEgressPolicyName);
}

TEST_F(ConfigTestFixture, ConfigManagerConfigIntegrityAfterUpdates) {
  // Create initial config
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Perform series of updates and verify config integrity using createPolicyMap
  // helper
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr3.str(), "INTEGRITY_TEST_INGRESS", ""}});

  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);

  // Verify the config is still valid and all original fields are preserved
  EXPECT_EQ(
      *updatedConfig->getConfig().router_id(), *defaultConfig_.router_id());
  EXPECT_EQ(
      *updatedConfig->getConfig().local_as_4_byte(),
      *defaultConfig_.local_as_4_byte());
  EXPECT_EQ(
      *updatedConfig->getConfig().hold_time(), *defaultConfig_.hold_time());
  EXPECT_EQ(
      *updatedConfig->getConfig().listen_addr(), *defaultConfig_.listen_addr());
  EXPECT_EQ(
      *updatedConfig->getConfig().listen_port(), *defaultConfig_.listen_port());

  // Verify peer groups are preserved
  EXPECT_EQ(
      updatedConfig->getConfig().peer_groups()->size(),
      defaultConfig_.peer_groups()->size());

  // Verify all peers are preserved
  EXPECT_EQ(
      updatedConfig->getConfig().peers()->size(),
      defaultConfig_.peers()->size());
}

TEST_F(ConfigTestFixture, ConfigManagerEmptyPolicyUpdates) {
  // Create initial config
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Test empty peer policy updates using createPolicyMap helper
  auto emptyPeerPolicyMap = createConfigManagerPolicyMap({});
  auto updatedConfig1 = configManager.updatePeerPolicies(*emptyPeerPolicyMap);

  // Should return a new config but with no changes
  ASSERT_NE(updatedConfig1, nullptr);
  EXPECT_NE(updatedConfig1, initialConfig); // Different objects

  // Test empty peer group policy updates using createPolicyMap helper
  auto emptyGroupPolicyMap = createConfigManagerPolicyMap({});
  auto updatedConfig2 =
      configManager.updatePeerGroupPolicies(*emptyGroupPolicyMap);

  // Should return a new config but with no changes
  ASSERT_NE(updatedConfig2, nullptr);
  EXPECT_NE(updatedConfig2, updatedConfig1); // Different objects
}

TEST_F(ConfigTestFixture, ConfigManagerThreadSafetySimulation) {
  // Create initial config
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Simulate multiple threads accessing and updating config
  // This is a basic test - in real scenarios you'd use std::thread

  // First "thread" updates peer policies using createPolicyMap helper
  auto peerPolicyMap =
      createConfigManagerPolicyMap({{kPeerAddr3.str(), "THREAD1_POLICY", ""}});
  auto config1 = configManager.updatePeerPolicies(*peerPolicyMap);

  // Second "thread" updates peer group policies using createPolicyMap helper
  auto groupPolicyMap = createConfigManagerPolicyMap(
      {{"PEERGROUP_RSW_CSW_V4", "", "THREAD2_POLICY"}});
  auto config2 = configManager.updatePeerGroupPolicies(*groupPolicyMap);

  // Both should have returned valid configs
  ASSERT_NE(config1, nullptr);
  ASSERT_NE(config2, nullptr);

  // The current config should be the most recently set one
  auto currentConfig = configManager.getConfig();
  EXPECT_EQ(currentConfig, config2);
}

// Test that no file operations occur when configFilePath is empty
TEST_F(ConfigTestFixture, ConfigManagerNoFileOperationsWithEmptyPath) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, ""); // Empty config file path

  // Update peer policies
  auto policyMap =
      createConfigManagerPolicyMap({{kPeerAddr3.str(), "TEST_POLICY", ""}});
  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);

  // Should succeed without any file operations
  ASSERT_NE(updatedConfig, nullptr);
  auto retrievedConfig = configManager.getConfig();
  EXPECT_EQ(retrievedConfig, updatedConfig);
}

// Test that backup file is created and config file is written
TEST_F(ConfigManagerFileTestFixture, ConfigManagerFileBackupAndWrite) {
  // Write initial config to file
  writeInitialConfigToFile();

  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  // Create ConfigManager with config file path
  ConfigManager configManager(initialConfig, configFilePath_.string());

  // Update peer policies which should trigger file backup and write
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr3.str(), "BACKUP_TEST_POLICY", ""}});

  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);

  // Verify config was updated
  ASSERT_NE(updatedConfig, nullptr);

  // Verify the config file still exists and has been updated
  EXPECT_TRUE(boost::filesystem::exists(configFilePath_));

  // Read the updated config file
  std::string updatedConfigContent;
  EXPECT_TRUE(
      folly::readFile(configFilePath_.string().c_str(), updatedConfigContent));

  // Deserialize and verify the policy was updated
  auto deserializedConfig =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::BgpConfig>(
          updatedConfigContent);

  bool foundUpdatedPeer = false;
  for (const auto& peer : *deserializedConfig.peers()) {
    if (*peer.peer_addr() == kPeerAddr3.str()) {
      EXPECT_EQ(*peer.ingress_policy_name(), "BACKUP_TEST_POLICY");
      foundUpdatedPeer = true;
      break;
    }
  }
  EXPECT_TRUE(foundUpdatedPeer);

  // Verify backup file was created
  EXPECT_TRUE(boost::filesystem::exists(backupFilePath_));

  // Verify backup file contains the original config
  std::string backupContent;
  EXPECT_TRUE(folly::readFile(backupFilePath_.string().c_str(), backupContent));

  // The backup should contain the initial config (before policy update)
  auto backupConfig =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::BgpConfig>(
          backupContent);

  // The backup should have the original policy (not the updated one)
  bool foundOriginalPeer = false;
  for (const auto& peer : *backupConfig.peers()) {
    if (*peer.peer_addr() == kPeerAddr3.str()) {
      // Original config from ConfigTestFixture has kIngressPolicyName
      EXPECT_EQ(*peer.ingress_policy_name(), kIngressPolicyName);
      foundOriginalPeer = true;
      break;
    }
  }
  EXPECT_TRUE(foundOriginalPeer);
}

// Test that config file is overwritten with updated content
TEST_F(ConfigManagerFileTestFixture, ConfigManagerOverwritesConfigFile) {
  // Write initial config to file
  writeInitialConfigToFile();

  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  // Create ConfigManager with config file path
  ConfigManager configManager(initialConfig, configFilePath_.string());

  // Update peer policies
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr3.str(), "PEERGROUP_RSW_CSW_V4", ""}});
  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);
  ASSERT_NE(updatedConfig, nullptr);

  // Verify the file exists and was modified
  EXPECT_TRUE(boost::filesystem::exists(configFilePath_));

  // Read the file and verify it contains the updated policy
  std::string fileContent;
  EXPECT_TRUE(folly::readFile(configFilePath_.string().c_str(), fileContent));

  auto deserializedConfig =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::BgpConfig>(
          fileContent);

  bool foundUpdatedPeer = false;
  for (const auto& peer : *deserializedConfig.peers()) {
    if (*peer.peer_addr() == kPeerAddr3.str()) {
      EXPECT_EQ(*peer.ingress_policy_name(), "PEERGROUP_RSW_CSW_V4");
      foundUpdatedPeer = true;
      break;
    }
  }
  EXPECT_TRUE(foundUpdatedPeer);
}

// Test that config file is overwritten with updated peer group policy content
TEST_F(
    ConfigManagerFileTestFixture,
    ConfigManagerOverwritesConfigFileWithPeerGroupPolicy) {
  // Write initial config to file
  writeInitialConfigToFile();

  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  // Create ConfigManager with config file path
  ConfigManager configManager(initialConfig, configFilePath_.string());

  // Update peer group policies
  auto policyMap = createConfigManagerPolicyMap(
      {{"PEERGROUP_RSW_CSW_V4",
        "OVERWRITE_PEERGROUP_INGRESS_POLICY",
        "OVERWRITE_PEERGROUP_EGRESS_POLICY"}});
  auto updatedConfig = configManager.updatePeerGroupPolicies(*policyMap);
  ASSERT_NE(updatedConfig, nullptr);

  // Verify the file exists and was modified
  EXPECT_TRUE(boost::filesystem::exists(configFilePath_));

  // Read the file and verify it contains the updated policy
  std::string fileContent;
  EXPECT_TRUE(folly::readFile(configFilePath_.string().c_str(), fileContent));

  auto deserializedConfig =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::BgpConfig>(
          fileContent);

  bool foundUpdatedPeerGroup = false;
  if (deserializedConfig.peer_groups().has_value()) {
    for (const auto& peerGroup : *deserializedConfig.peer_groups()) {
      if (*peerGroup.name() == "PEERGROUP_RSW_CSW_V4") {
        EXPECT_EQ(
            *peerGroup.ingress_policy_name(),
            "OVERWRITE_PEERGROUP_INGRESS_POLICY");
        EXPECT_EQ(
            *peerGroup.egress_policy_name(),
            "OVERWRITE_PEERGROUP_EGRESS_POLICY");
        foundUpdatedPeerGroup = true;
        break;
      }
    }
  }
  EXPECT_TRUE(foundUpdatedPeerGroup);
}

/******************************************************************************
 *      START   -   Config Version Tracking Tests                             *
 ******************************************************************************/

// Test that getConfigVersion returns 0 initially
TEST_F(ConfigTestFixture, ConfigManagerGetConfigVersionInitiallyZero) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  EXPECT_EQ(0, configManager.getConfigVersion());
}

// Test that getConfigVersion increments after updateConfig
TEST_F(ConfigTestFixture, ConfigManagerVersionIncrementsAfterUpdateConfig) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, ""); // No file path

  EXPECT_EQ(0, configManager.getConfigVersion());

  // Update config - version should increment
  auto policyMap =
      createConfigManagerPolicyMap({{kPeerAddr3.str(), "TEST_POLICY", ""}});
  configManager.updatePeerPolicies(*policyMap);

  EXPECT_EQ(1, configManager.getConfigVersion());

  // Update config again - version should increment again
  auto policyMap2 =
      createConfigManagerPolicyMap({{kPeerAddr3.str(), "TEST_POLICY_2", ""}});
  configManager.updatePeerPolicies(*policyMap2);

  EXPECT_EQ(2, configManager.getConfigVersion());
}

// Test that version increments even when configFilePath_ is empty (early return
// path)
TEST_F(ConfigTestFixture, ConfigManagerVersionIncrementsWithEmptyFilePath) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, ""); // Empty config file path

  EXPECT_EQ(0, configManager.getConfigVersion());

  // Update peer policies
  auto policyMap =
      createConfigManagerPolicyMap({{kPeerAddr3.str(), "POLICY_V1", ""}});
  configManager.updatePeerPolicies(*policyMap);

  EXPECT_EQ(1, configManager.getConfigVersion());

  // Update peer group policies
  auto groupPolicyMap = createConfigManagerPolicyMap(
      {{"PEERGROUP_RSW_CSW_V4", "GROUP_INGRESS", "GROUP_EGRESS"}});
  configManager.updatePeerGroupPolicies(*groupPolicyMap);

  EXPECT_EQ(2, configManager.getConfigVersion());
}

// Test that version increments after file write in
// ConfigManagerFileTestFixture
TEST_F(
    ConfigManagerFileTestFixture,
    ConfigManagerVersionIncrementsAfterFileWrite) {
  writeInitialConfigToFile();

  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, configFilePath_.string());

  EXPECT_EQ(0, configManager.getConfigVersion());

  // Update peer policies - should write to file and increment version
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr3.str(), "FILE_TEST_POLICY", ""}});
  configManager.updatePeerPolicies(*policyMap);

  EXPECT_EQ(1, configManager.getConfigVersion());

  // Verify file was written
  EXPECT_TRUE(boost::filesystem::exists(configFilePath_));
}

// Test that multiple rapid updates each increment the version
TEST_F(ConfigTestFixture, ConfigManagerVersionIncrementsOnEachUpdate) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, "");

  EXPECT_EQ(0, configManager.getConfigVersion());

  // Simulate rapid succession of updates
  for (int i = 1; i <= 5; i++) {
    auto policyMap = createConfigManagerPolicyMap(
        {{kPeerAddr3.str(), "POLICY_" + std::to_string(i), ""}});
    configManager.updatePeerPolicies(*policyMap);
    EXPECT_EQ(i, configManager.getConfigVersion());
  }
}

// Test that version and config are atomically consistent
TEST_F(ConfigTestFixture, ConfigManagerVersionAndConfigAreConsistent) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, "");

  // Initial state
  auto config1 = configManager.getConfig();
  auto version1 = configManager.getConfigVersion();
  EXPECT_EQ(0, version1);

  // Update config
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr3.str(), "CONSISTENT_POLICY", ""}});
  configManager.updatePeerPolicies(*policyMap);

  // After update - config and version should both reflect the update
  auto config2 = configManager.getConfig();
  auto version2 = configManager.getConfigVersion();
  EXPECT_EQ(1, version2);
  EXPECT_NE(config1, config2); // Different config objects

  // Verify the policy was actually updated
  auto& peers = *config2->getConfig().peers();
  bool foundUpdatedPeer = false;
  for (const auto& peer : peers) {
    if (*peer.peer_addr() == kPeerAddr3.str()) {
      EXPECT_EQ(*peer.ingress_policy_name(), "CONSISTENT_POLICY");
      foundUpdatedPeer = true;
      break;
    }
  }
  EXPECT_TRUE(foundUpdatedPeer);
}

// Test that version and config remain unchanged when file write fails
TEST_F(
    ConfigManagerFileTestFixture,
    ConfigManagerVersionAndConfigUnchangedOnFileWriteFailure) {
  // Use a path to a non-existent directory so writeConfigToFile will throw
  std::string invalidPath = "/nonexistent/dir/config.json";
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, invalidPath);

  EXPECT_EQ(0, configManager.getConfigVersion());
  auto configBefore = configManager.getConfig();

  // Update peer policies - file write should fail, exception caught here
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr3.str(), "FAIL_WRITE_POLICY", ""}});
  EXPECT_THROW(configManager.updatePeerPolicies(*policyMap), std::exception);

  // Version should NOT be incremented
  EXPECT_EQ(0, configManager.getConfigVersion());

  // In-memory config should also be UNCHANGED (same pointer)
  EXPECT_EQ(configBefore, configManager.getConfig());
}

/******************************************************************************
 *      END   -   Config Version Tracking Tests                               *
 ******************************************************************************/

/******************************************************************************
 *      START   -   hasEgressPolicyOverride Tests                             *
 ******************************************************************************/

TEST_F(ConfigTestFixture, HasEgressPolicyOverrideAtStartup) {
  // Verify that hasEgressPolicyOverride is correctly set from the raw thrift
  // config at construction time.
  auto config = std::make_shared<const Config>(defaultConfig_);

  // staticPeer1_ (kPeerAddr3) has peer-level egress_policy_name set
  EXPECT_TRUE(config->getConfigOfAPeer(kPeerAddr3)->hasEgressPolicyOverride);

  // staticPeer2_ (kPeerAddr4) has peer-level egress_policy_name set
  EXPECT_TRUE(config->getConfigOfAPeer(kPeerAddr4)->hasEgressPolicyOverride);

  // staticPeer3_ (kPeerAddr5) has NO peer-level policies, inherits from group
  EXPECT_FALSE(config->getConfigOfAPeer(kPeerAddr5)->hasEgressPolicyOverride);

  // staticPeer4_ (kPeerAddr6) has peer-level egress_policy_name override
  EXPECT_TRUE(config->getConfigOfAPeer(kPeerAddr6)->hasEgressPolicyOverride);
}

TEST_F(ConfigTestFixture, HasEgressPolicyOverrideAfterSetPeersPolicy) {
  // After setPeersPolicy (updatePeerPolicies), peers that get an egress
  // policy set should have hasEgressPolicyOverride = true.
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, "");

  // staticPeer3_ (kPeerAddr5) starts with no peer-level policy
  auto config = configManager.getConfig();
  auto peerConfig = config->getConfigOfAPeer(kPeerAddr5);
  EXPECT_FALSE(peerConfig->hasEgressPolicyOverride);

  // Set a peer-level egress policy on staticPeer3_
  auto policyMap = createConfigManagerPolicyMap(
      {{kPeerAddr5.str(), "", "PEER_OVERRIDE_EGRESS"}});
  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);

  // Now hasEgressPolicyOverride should be true
  auto updatedPeerConfig = updatedConfig->getConfigOfAPeer(kPeerAddr5);
  EXPECT_TRUE(updatedPeerConfig->hasEgressPolicyOverride);
}

TEST_F(ConfigTestFixture, HasEgressPolicyOverrideAfterUnsetPeersPolicy) {
  // After unsetPeersPolicy, peers that had a peer-level egress policy
  // should have hasEgressPolicyOverride = false.
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, "");

  // staticPeer1_ (kPeerAddr3) starts with peer-level egress policy
  auto config = configManager.getConfig();
  auto peerConfig = config->getConfigOfAPeer(kPeerAddr3);
  EXPECT_TRUE(peerConfig->hasEgressPolicyOverride);

  // Unset the peer-level egress policy
  std::map<std::string, std::set<bgp_policy::DIRECTION>> peersToUnset;
  peersToUnset[kPeerAddr3.str()].insert(bgp_policy::DIRECTION::OUT);
  auto updatedConfig = configManager.unsetPeerPolicies(peersToUnset);

  // Now hasEgressPolicyOverride should be false
  auto updatedPeerConfig = updatedConfig->getConfigOfAPeer(kPeerAddr3);
  EXPECT_FALSE(updatedPeerConfig->hasEgressPolicyOverride);
}

TEST_F(ConfigTestFixture, HasEgressPolicyOverrideUnchangedByGroupPolicyUpdate) {
  // Updating peer group policies should NOT change hasEgressPolicyOverride
  // on any peer — it only reflects peer-level overrides.
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, "");

  // Verify initial state before update
  EXPECT_FALSE(
      initialConfig->getConfigOfAPeer(kPeerAddr5)->hasEgressPolicyOverride);
  EXPECT_TRUE(
      initialConfig->getConfigOfAPeer(kPeerAddr6)->hasEgressPolicyOverride);

  auto policyMap = createConfigManagerPolicyMap(
      {{"PEERGROUP_RSW_CSW_V4", "", "NEW_GROUP_EGRESS"}});
  auto updatedConfig = configManager.updatePeerGroupPolicies(*policyMap);

  // staticPeer3_ (kPeerAddr5) — no peer-level policy, should stay false
  auto peer3Config = updatedConfig->getConfigOfAPeer(kPeerAddr5);
  EXPECT_FALSE(peer3Config->hasEgressPolicyOverride);

  // staticPeer4_ (kPeerAddr6) — has peer-level override, should stay true
  auto peer4Config = updatedConfig->getConfigOfAPeer(kPeerAddr6);
  EXPECT_TRUE(peer4Config->hasEgressPolicyOverride);
}

/******************************************************************************
 *      END   -   hasEgressPolicyOverride Tests                               *
 ******************************************************************************/

/******************************************************************************
 *      START   -   addPeersToConfig Tests                                    *
 ******************************************************************************/

// Verify adding a single peer: count increases, fields are correct, existing
// peers are preserved, version increments, and config manager returns updated.
TEST_F(ConfigTestFixture, AddPeersToConfigBasic) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  auto initialPeerCount = initialConfig->getConfig().peers()->size();
  EXPECT_EQ(0, configManager.getConfigVersion());

  thrift::BgpPeer newPeer;
  newPeer.remote_as_4_byte() = kAsn1;
  newPeer.local_addr() = kLocalAddr7.str();
  newPeer.peer_addr() = kPeerAddr7.str();
  newPeer.next_hop4() = kNextHopV4_7.str();
  newPeer.next_hop6() = kNextHopV6_7.str();

  std::vector<thrift::BgpPeer> newPeers = {newPeer};
  auto updatedConfig = configManager.addPeersToConfig(newPeers);

  ASSERT_NE(updatedConfig, nullptr);
  EXPECT_EQ(updatedConfig->getConfig().peers()->size(), initialPeerCount + 1);
  EXPECT_EQ(1, configManager.getConfigVersion());
  EXPECT_EQ(configManager.getConfig(), updatedConfig);

  // Verify new peer fields
  bool foundNewPeer = false;
  bool foundStaticPeer1 = false;
  bool foundStaticPeer2 = false;
  for (const auto& peer : *updatedConfig->getConfig().peers()) {
    if (*peer.peer_addr() == kPeerAddr7.str()) {
      foundNewPeer = true;
      EXPECT_EQ(*peer.remote_as_4_byte(), kAsn1);
      EXPECT_EQ(*peer.local_addr(), kLocalAddr7.str());
    }
    if (*peer.peer_addr() == kPeerAddr3.str()) {
      foundStaticPeer1 = true;
      EXPECT_EQ(*peer.ingress_policy_name(), kIngressPolicyName);
      EXPECT_EQ(*peer.egress_policy_name(), kEgressPolicyName);
    }
    if (*peer.peer_addr() == kPeerAddr4.str()) {
      foundStaticPeer2 = true;
    }
  }
  EXPECT_TRUE(foundNewPeer);
  EXPECT_TRUE(foundStaticPeer1);
  EXPECT_TRUE(foundStaticPeer2);

  // Verify global config integrity
  EXPECT_EQ(
      *updatedConfig->getConfig().router_id(), *defaultConfig_.router_id());
  EXPECT_EQ(
      *updatedConfig->getConfig().local_as_4_byte(),
      *defaultConfig_.local_as_4_byte());
}

// Verify adding multiple peers at once increases peer count by the batch size.
TEST_F(ConfigTestFixture, AddPeersToConfigMultiplePeers) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  auto initialPeerCount = initialConfig->getConfig().peers()->size();

  thrift::BgpPeer newPeer1;
  newPeer1.remote_as_4_byte() = kAsn1;
  newPeer1.local_addr() = kLocalAddr7.str();
  newPeer1.peer_addr() = kPeerAddr7.str();
  newPeer1.next_hop4() = kNextHopV4_7.str();
  newPeer1.next_hop6() = kNextHopV6_7.str();

  thrift::BgpPeer newPeer2;
  newPeer2.remote_as_4_byte() = kAsn2;
  newPeer2.local_addr() = kLocalAddr8.str();
  newPeer2.peer_addr() = kPeerAddr8.str();
  newPeer2.next_hop4() = kNextHopV4_8.str();
  newPeer2.next_hop6() = kNextHopV6_8.str();

  std::vector<thrift::BgpPeer> newPeers = {newPeer1, newPeer2};
  auto updatedConfig = configManager.addPeersToConfig(newPeers);

  ASSERT_NE(updatedConfig, nullptr);
  EXPECT_EQ(updatedConfig->getConfig().peers()->size(), initialPeerCount + 2);

  bool foundPeer1 = false;
  bool foundPeer2 = false;
  for (const auto& peer : *updatedConfig->getConfig().peers()) {
    if (*peer.peer_addr() == kPeerAddr7.str()) {
      foundPeer1 = true;
    }
    if (*peer.peer_addr() == kPeerAddr8.str()) {
      foundPeer2 = true;
    }
  }
  EXPECT_TRUE(foundPeer1);
  EXPECT_TRUE(foundPeer2);
}

// Verify peer group inheritance and explicit policy assignment on new peers.
TEST_F(ConfigTestFixture, AddPeersToConfigWithPeerGroupAndPolicies) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  // Peer with peer group — should inherit group policies
  thrift::BgpPeer groupPeer;
  groupPeer.remote_as_4_byte() = kAsn1;
  groupPeer.local_addr() = kLocalAddr7.str();
  groupPeer.peer_addr() = kPeerAddr7.str();
  groupPeer.next_hop4() = kNextHopV4_7.str();
  groupPeer.next_hop6() = kNextHopV6_7.str();
  groupPeer.peer_group_name() = "PEERGROUP_RSW_CSW_V4";

  // Peer with explicit policies
  thrift::BgpPeer policyPeer;
  policyPeer.remote_as_4_byte() = kAsn2;
  policyPeer.local_addr() = kLocalAddr8.str();
  policyPeer.peer_addr() = kPeerAddr8.str();
  policyPeer.next_hop4() = kNextHopV4_8.str();
  policyPeer.next_hop6() = kNextHopV6_8.str();
  policyPeer.ingress_policy_name() = "MY_INGRESS_POLICY";
  policyPeer.egress_policy_name() = "MY_EGRESS_POLICY";

  std::vector<thrift::BgpPeer> newPeers = {groupPeer, policyPeer};
  auto updatedConfig = configManager.addPeersToConfig(newPeers);

  ASSERT_NE(updatedConfig, nullptr);

  // Verify peer group is set in raw config
  bool foundGroupPeer = false;
  bool foundPolicyPeer = false;
  for (const auto& peer : *updatedConfig->getConfig().peers()) {
    if (*peer.peer_addr() == kPeerAddr7.str()) {
      foundGroupPeer = true;
      ASSERT_TRUE(peer.peer_group_name().has_value());
      EXPECT_EQ(*peer.peer_group_name(), "PEERGROUP_RSW_CSW_V4");
    }
    if (*peer.peer_addr() == kPeerAddr8.str()) {
      foundPolicyPeer = true;
      EXPECT_EQ(*peer.ingress_policy_name(), "MY_INGRESS_POLICY");
      EXPECT_EQ(*peer.egress_policy_name(), "MY_EGRESS_POLICY");
    }
  }
  EXPECT_TRUE(foundGroupPeer);
  EXPECT_TRUE(foundPolicyPeer);

  // Verify group peer inherits policies via getPeerToConfig() resolution
  auto& peerToConfig = updatedConfig->getPeerToConfig();
  auto peerIt = peerToConfig.find(kPeerAddr7);
  ASSERT_NE(peerIt, peerToConfig.end());
  EXPECT_EQ(
      peerIt->second->commonPeerGroupConfig.ingressPolicyName, "RSW_CSW_IN");
  EXPECT_EQ(
      peerIt->second->commonPeerGroupConfig.egressPolicyName, "RSW_CSW_OUT");
}

// Verify adding an empty peer list is a no-op (peer count unchanged).
TEST_F(ConfigTestFixture, AddPeersToConfigEmptyList) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  auto initialPeerCount = initialConfig->getConfig().peers()->size();

  std::vector<thrift::BgpPeer> newPeers = {};
  auto updatedConfig = configManager.addPeersToConfig(newPeers);

  ASSERT_NE(updatedConfig, nullptr);
  EXPECT_EQ(updatedConfig->getConfig().peers()->size(), initialPeerCount);
}

// Verify config is written to file and backup is created after adding peers.
TEST_F(ConfigManagerFileTestFixture, AddPeersToConfigPersistsToFile) {
  writeInitialConfigToFile();

  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, configFilePath_.string());

  auto initialPeerCount = initialConfig->getConfig().peers()->size();

  // Create a new peer
  thrift::BgpPeer newPeer;
  newPeer.remote_as_4_byte() = kAsn1;
  newPeer.local_addr() = kLocalAddr7.str();
  newPeer.peer_addr() = kPeerAddr7.str();
  newPeer.next_hop4() = kNextHopV4_7.str();
  newPeer.next_hop6() = kNextHopV6_7.str();

  std::vector<thrift::BgpPeer> newPeers = {newPeer};
  auto updatedConfig = configManager.addPeersToConfig(newPeers);

  ASSERT_NE(updatedConfig, nullptr);

  // Verify config file was updated
  EXPECT_TRUE(boost::filesystem::exists(configFilePath_));

  std::string fileContent;
  EXPECT_TRUE(folly::readFile(configFilePath_.string().c_str(), fileContent));

  auto deserializedConfig =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::BgpConfig>(
          fileContent);

  // Verify peer count in file
  EXPECT_EQ(deserializedConfig.peers()->size(), initialPeerCount + 1);

  // Verify the new peer is in the file
  bool foundNewPeer = false;
  for (const auto& peer : *deserializedConfig.peers()) {
    if (*peer.peer_addr() == kPeerAddr7.str()) {
      foundNewPeer = true;
      break;
    }
  }
  EXPECT_TRUE(foundNewPeer);

  // Verify backup was created
  EXPECT_TRUE(boost::filesystem::exists(backupFilePath_));
}

/******************************************************************************
 *      END   -   addPeersToConfig Tests                                      *
 ******************************************************************************/

/******************************************************************************
 *      START   -   removePeersFromConfig Tests                               *
 ******************************************************************************/

// Verify empty list and non-existent peer are no-ops.
TEST_F(ConfigTestFixture, RemovePeersFromConfigNoOps) {
  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig);

  auto initialPeerCount = initialConfig->getConfig().peers()->size();

  // Empty list is a no-op
  auto config1 = configManager.removePeersFromConfig({});
  ASSERT_NE(config1, nullptr);
  EXPECT_EQ(config1->getConfig().peers()->size(), initialPeerCount);

  // Non-existent peer is a no-op
  auto config2 =
      configManager.removePeersFromConfig({folly::IPAddress("192.168.99.99")});
  EXPECT_EQ(config2->getConfig().peers()->size(), initialPeerCount);
}

// Remove multiple peers (including a peer-group member) and verify the updated
// config is correct in memory and persisted to disk with a backup.
TEST_F(ConfigManagerFileTestFixture, RemovePeersFromConfig) {
  writeInitialConfigToFile();

  auto initialConfig = std::make_shared<const Config>(defaultConfig_);
  ConfigManager configManager(initialConfig, configFilePath_.string());

  auto initialPeerCount = initialConfig->getConfig().peers()->size();

  // Remove two peers at once: staticPeer1_ (kPeerAddr3) and a peer-group
  // member staticPeer3_ (kPeerAddr5)
  auto updatedConfig =
      configManager.removePeersFromConfig({kPeerAddr3, kPeerAddr5});

  ASSERT_NE(updatedConfig, nullptr);
  EXPECT_EQ(updatedConfig->getConfig().peers()->size(), initialPeerCount - 2);
  EXPECT_EQ(configManager.getConfig(), updatedConfig);

  // Verify removed peers are gone and others are preserved
  bool foundStaticPeer2 = false;
  for (const auto& peer : *updatedConfig->getConfig().peers()) {
    EXPECT_NE(*peer.peer_addr(), kPeerAddr3.str());
    EXPECT_NE(*peer.peer_addr(), kPeerAddr5.str());
    if (*peer.peer_addr() == kPeerAddr4.str()) {
      foundStaticPeer2 = true;
    }
  }
  EXPECT_TRUE(foundStaticPeer2);

  // Verify peer groups are unchanged
  EXPECT_EQ(
      updatedConfig->getConfig().peer_groups()->size(),
      defaultConfig_.peer_groups()->size());
  auto& peerGroups = *updatedConfig->getConfig().peer_groups();
  bool foundGroup = false;
  for (const auto& peerGroup : peerGroups) {
    if (*peerGroup.name() == "PEERGROUP_RSW_CSW_V4") {
      foundGroup = true;
      EXPECT_EQ(*peerGroup.ingress_policy_name(), "RSW_CSW_IN");
      EXPECT_EQ(*peerGroup.egress_policy_name(), "RSW_CSW_OUT");
    }
  }
  EXPECT_TRUE(foundGroup);

  // Verify global config integrity
  EXPECT_EQ(
      *updatedConfig->getConfig().router_id(), *defaultConfig_.router_id());
  EXPECT_EQ(
      *updatedConfig->getConfig().local_as_4_byte(),
      *defaultConfig_.local_as_4_byte());

  // Verify config file was updated
  EXPECT_TRUE(boost::filesystem::exists(configFilePath_));

  std::string fileContent;
  EXPECT_TRUE(folly::readFile(configFilePath_.string().c_str(), fileContent));

  auto deserializedConfig =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::BgpConfig>(
          fileContent);

  EXPECT_EQ(deserializedConfig.peers()->size(), initialPeerCount - 2);
  for (const auto& peer : *deserializedConfig.peers()) {
    EXPECT_NE(*peer.peer_addr(), kPeerAddr3.str());
    EXPECT_NE(*peer.peer_addr(), kPeerAddr5.str());
  }

  // Verify backup was created
  EXPECT_TRUE(boost::filesystem::exists(backupFilePath_));
}

/******************************************************************************
 *      END   -   removePeersFromConfig Tests                                 *
 ******************************************************************************/

/******************************************************************************
 *      START   -   Split Config Policy Stripping Tests                       *
 ******************************************************************************/

// Test that policy body fields are stripped from written config file in
// split-config mode (separate policy file via --policy flag)
TEST_F(ConfigManagerFileTestFixture, SplitConfigPolicyFieldsStrippedFromFile) {
  // Add policy body fields to defaultConfig_
  BgpCommunity community;
  community.name() = "TEST_COMMUNITY";
  community.communities() = {"65000:1"};
  defaultConfig_.communities() = {community};

  BgpLocalPref localpref;
  localpref.localpref() = 200;
  localpref.name() = "TEST_LOCALPREF";
  defaultConfig_.localprefs() = {localpref};

  // Write policy file for split-config
  auto policyFilePath = testDir_ / "test_policy.json";
  thrift::BgpConfig policyConfig;
  policyConfig.communities().copy_from(defaultConfig_.communities());
  policyConfig.localprefs().copy_from(defaultConfig_.localprefs());
  std::string policyStr =
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          policyConfig);
  folly::writeFileAtomic(policyFilePath.string(), policyStr);

  // Create Config and load split policy
  writeInitialConfigToFile();
  auto config = std::make_shared<Config>(defaultConfig_);
  config->setPolicyConfigFromFile(policyFilePath.string());

  // Verify split-config mode is active and policy body is merged
  ASSERT_TRUE(config->splitConfigPolicy());
  ASSERT_FALSE(config->getConfig().communities()->empty());

  // Create ConfigManager with split-config Config
  ConfigManager configManager(config, configFilePath_.string());

  // Trigger a runtime config update
  auto policyMap =
      createConfigManagerPolicyMap({{kPeerAddr3.str(), "UPDATED_POLICY", ""}});
  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);
  ASSERT_NE(updatedConfig, nullptr);

  // Read written config file
  std::string fileContent;
  ASSERT_TRUE(folly::readFile(configFilePath_.string().c_str(), fileContent));
  auto deserializedConfig =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::BgpConfig>(
          fileContent);

  // Policy body fields must be stripped from written file
  EXPECT_TRUE(deserializedConfig.communities()->empty());
  EXPECT_TRUE(deserializedConfig.localprefs()->empty());
  EXPECT_TRUE(
      !deserializedConfig.policies().has_value() ||
      deserializedConfig.policies()->bgp_policy_statements()->empty());

  // Settings data (peer policy names) must still be present
  bool foundUpdatedPeer = false;
  for (const auto& peer : *deserializedConfig.peers()) {
    if (*peer.peer_addr() == kPeerAddr3.str()) {
      EXPECT_EQ(*peer.ingress_policy_name(), "UPDATED_POLICY");
      foundUpdatedPeer = true;
      break;
    }
  }
  EXPECT_TRUE(foundUpdatedPeer);
}

// Test that policy body fields are stripped from written config file in
// split-config mode (separate policy file via ConfigManager::setPolicyConfig)
TEST_F(
    ConfigManagerFileTestFixture,
    SplitConfigPolicyFieldsStrippedInProductionOrder) {
  // Add policy body fields to defaultConfig_
  BgpCommunity community;
  community.name() = "TEST_COMMUNITY";
  community.communities() = {"65000:1"};
  defaultConfig_.communities() = {community};

  BgpLocalPref localpref;
  localpref.localpref() = 200;
  localpref.name() = "TEST_LOCALPREF";
  defaultConfig_.localprefs() = {localpref};

  // Write policy file for split-config
  auto policyFilePath = testDir_ / "test_policy.json";
  thrift::BgpConfig policyConfig;
  policyConfig.communities().copy_from(defaultConfig_.communities());
  policyConfig.localprefs().copy_from(defaultConfig_.localprefs());
  std::string policyStr =
      apache::thrift::SimpleJSONSerializer::serialize<std::string>(
          policyConfig);
  folly::writeFileAtomic(policyFilePath.string(), policyStr);

  writeInitialConfigToFile();
  auto config = std::make_shared<Config>(defaultConfig_);

  // construct the ConfigManager first, load the policy file
  // afterwards (the reverse of SplitConfigPolicyFieldsStrippedFromFile). The
  // split-config decision is passed explicitly, mirroring the entrypoints.
  ConfigManager configManager(
      config, configFilePath_.string(), /*splitConfigPolicy=*/true);
  config->setPolicyConfigFromFile(policyFilePath.string());

  // Verify split-config mode is active and policy body is merged
  ASSERT_TRUE(config->splitConfigPolicy());
  ASSERT_FALSE(config->getConfig().communities()->empty());

  // Trigger a runtime config update
  auto policyMap =
      createConfigManagerPolicyMap({{kPeerAddr3.str(), "UPDATED_POLICY", ""}});
  auto updatedConfig = configManager.updatePeerPolicies(*policyMap);
  ASSERT_NE(updatedConfig, nullptr);

  // Read written config file
  std::string fileContent;
  ASSERT_TRUE(folly::readFile(configFilePath_.string().c_str(), fileContent));
  auto deserializedConfig =
      apache::thrift::SimpleJSONSerializer::deserialize<thrift::BgpConfig>(
          fileContent);

  // Policy body fields must be stripped from written file
  EXPECT_TRUE(deserializedConfig.communities()->empty());
  EXPECT_TRUE(deserializedConfig.localprefs()->empty());
  EXPECT_TRUE(
      !deserializedConfig.policies().has_value() ||
      deserializedConfig.policies()->bgp_policy_statements()->empty());

  // Settings data (peer policy names) must still be present
  bool foundUpdatedPeer = false;
  for (const auto& peer : *deserializedConfig.peers()) {
    if (*peer.peer_addr() == kPeerAddr3.str()) {
      EXPECT_EQ(*peer.ingress_policy_name(), "UPDATED_POLICY");
      foundUpdatedPeer = true;
      break;
    }
  }
  EXPECT_TRUE(foundUpdatedPeer);
}

/******************************************************************************
 *      END   -   Split Config Policy Stripping Tests                         *
 ******************************************************************************/
} // namespace facebook::bgp
