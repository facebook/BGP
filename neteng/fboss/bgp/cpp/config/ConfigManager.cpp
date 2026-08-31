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

#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"

#include <algorithm>

#include <boost/filesystem.hpp>
#include <folly/FileUtil.h>
#include <folly/container/F14Set.h>
#include <folly/json/json.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

DEFINE_string(
    bgp_config_backup,
    "",
    "File path for backing up BGP config on updates");

namespace facebook::bgp {

using namespace facebook::neteng::fboss::bgp;

// Helper to backup existing config file
void backupExistingConfig(const std::string& filepath) {
  try {
    if (!boost::filesystem::exists(filepath)) {
      XLOGF(DBG2, "No existing config file at {} to backup", filepath);
      return;
    }

    boost::filesystem::path backupPath(FLAGS_bgp_config_backup);

    // Copy the file to backup location (overwrite if exists)
    boost::filesystem::copy_file(
        boost::filesystem::path(filepath),
        backupPath,
        boost::filesystem::copy_option::overwrite_if_exists);

    XLOGF(
        INFO,
        "Backed up existing config from {} to {}",
        filepath,
        backupPath.string());
  } catch (const std::exception& ex) {
    XLOGF(
        ERR,
        "Failed to backup existing config file. Exception: {}",
        folly::exceptionStr(ex));
  }
}

// Helper to serialize and write config atomically
void writeConfigToFile(
    const std::string& filepath,
    const thrift::BgpConfig& config) {
  try {
    // Serialize config to compact JSON, then pretty-print for readability
    std::string compactJson =
        apache::thrift::SimpleJSONSerializer::serialize<std::string>(config);
    std::string configStr = folly::toPrettyJson(folly::parseJson(compactJson));

    // Write atomically using folly::writeFileAtomic
    folly::writeFileAtomic(filepath, configStr);

    XLOGF(INFO, "Successfully wrote config to {}", filepath);
  } catch (const std::exception& ex) {
    XLOGF(
        ERR,
        "Failed to write config to file. Exception: {}",
        folly::exceptionStr(ex));
    throw; // Re-throw after logging to ensure caller knows write failed
  }
}

std::shared_ptr<const Config> ConfigManager::updatePeerPolicies(
    const std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>&
        peerPolicies) {
  auto currentConfig = getConfig();
  auto newConfig =
      createConfigWithUpdatedPeerPolicies(peerPolicies, *currentConfig);
  updateConfig(newConfig);
  return newConfig;
}

std::shared_ptr<const Config> ConfigManager::updatePeerGroupPolicies(
    const std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>&
        peerGroupPolicies) {
  auto currentConfig = getConfig();
  auto newConfig = createConfigWithUpdatedPeerGroupPolicies(
      peerGroupPolicies, *currentConfig);
  updateConfig(newConfig);
  return newConfig;
}

std::shared_ptr<const Config> ConfigManager::unsetPeerPolicies(
    const std::map<std::string, std::set<bgp_policy::DIRECTION>>&
        peersToUnset) {
  auto currentConfig = getConfig();
  auto newConfig =
      createConfigWithUnsetPeerPolicies(peersToUnset, *currentConfig);
  updateConfig(newConfig);
  return newConfig;
}

bool ConfigManager::updatePolicyName(
    apache::thrift::optional_field_ref<std::string&> currentPolicy,
    const std::string& newPolicy) {
  if (!currentPolicy.has_value() || currentPolicy.value() != newPolicy) {
    currentPolicy = newPolicy;
    return true;
  }
  return false;
}

void ConfigManager::updateIngressEgressPeerPolicies(
    thrift::BgpPeer& peer,
    const std::map<bgp_policy::DIRECTION, std::string>&
        directionToPolicyName) noexcept {
  for (const auto& [direction, policyName] : directionToPolicyName) {
    switch (direction) {
      case bgp_policy::DIRECTION::IN:
        updatePolicyName(peer.ingress_policy_name(), policyName);
        break;
      case bgp_policy::DIRECTION::OUT:
        updatePolicyName(peer.egress_policy_name(), policyName);
        break;
      default:
        break;
    }
  }
}

void ConfigManager::unsetIngressEgressPeerPolicies(
    thrift::BgpPeer& peer,
    const std::set<bgp_policy::DIRECTION>& directionsToUnset) noexcept {
  for (const auto& direction : directionsToUnset) {
    switch (direction) {
      case bgp_policy::DIRECTION::IN:
        peer.ingress_policy_name().reset();
        break;
      case bgp_policy::DIRECTION::OUT:
        peer.egress_policy_name().reset();
        break;
      default:
        break;
    }
  }
}

void ConfigManager::updateIngressEgressPeerGroupPolicies(
    thrift::PeerGroup& peerGroup,
    const std::map<bgp_policy::DIRECTION, std::string>&
        directionToPolicyName) noexcept {
  for (const auto& [direction, policyName] : directionToPolicyName) {
    switch (direction) {
      case bgp_policy::DIRECTION::IN:
        updatePolicyName(peerGroup.ingress_policy_name(), policyName);
        break;
      case bgp_policy::DIRECTION::OUT:
        updatePolicyName(peerGroup.egress_policy_name(), policyName);
        break;
      default:
        break;
    }
  }
}

std::shared_ptr<const Config>
ConfigManager::createConfigWithUpdatedPeerPolicies(
    const std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>&
        peerPolicies,
    const Config& currentConfig) {
  // Create mutable copy of current config
  auto thriftConfig = currentConfig.getConfig();

  // Update peer policies using the new structured approach
  for (auto& peer : *thriftConfig.peers()) {
    const auto& peerAddr = *peer.peer_addr();
    if (peerPolicies.contains(peerAddr)) {
      const auto& policies = peerPolicies.at(peerAddr);
      updateIngressEgressPeerPolicies(peer, policies);
    }
  }

  // Create new immutable Config
  return currentConfig.createConfig(std::move(thriftConfig));
}

std::shared_ptr<const Config>
ConfigManager::createConfigWithUpdatedPeerGroupPolicies(
    const std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>&
        peerGroupPolicies,
    const Config& currentConfig) {
  // Create mutable copy of current config
  auto thriftConfig = currentConfig.getConfig();

  // Update peer group policies only
  // Peers inherit group policy via getValue() resolution in Config constructor.
  if (thriftConfig.peer_groups().has_value()) {
    for (auto& peerGroup : thriftConfig.peer_groups().value()) {
      const auto& peerGroupName = *peerGroup.name();
      if (peerGroupPolicies.contains(peerGroupName)) {
        const auto& policies = peerGroupPolicies.at(peerGroupName);
        updateIngressEgressPeerGroupPolicies(peerGroup, policies);
      }
    }
  }

  // Create new immutable Config
  return currentConfig.createConfig(std::move(thriftConfig));
}

void ConfigManager::updateConfig(std::shared_ptr<const Config> newConfig) {
  auto wlock = config_.wlock();

  // Write to file before updating in-memory config. If file write fails,
  // exception propagates and neither config nor version is modified,
  // keeping the system in a consistent, retryable state.
  if (!configFilePath_.empty()) {
    auto configToWrite = newConfig->getConfig();
    if (splitConfigPolicy_) {
      Config::resetPolicyConfig(configToWrite);
    }
    backupExistingConfig(configFilePath_);
    writeConfigToFile(configFilePath_, configToWrite);
  }

  // Update in-memory config and increment version only after
  // successful file write (or when no file path is configured).
  wlock->first = std::move(newConfig);
  ++wlock->second;
}

std::shared_ptr<const Config> ConfigManager::createConfigWithUnsetPeerPolicies(
    const std::map<std::string, std::set<bgp_policy::DIRECTION>>& peersToUnset,
    const Config& currentConfig) {
  auto thriftConfig = currentConfig.getConfig();

  for (auto& peer : *thriftConfig.peers()) {
    const auto& peerAddr = *peer.peer_addr();
    auto it = peersToUnset.find(peerAddr);
    if (it != peersToUnset.end()) {
      unsetIngressEgressPeerPolicies(peer, it->second);
    }
  }

  return currentConfig.createConfig(std::move(thriftConfig));
}

std::shared_ptr<const Config> ConfigManager::addPeersToConfig(
    const std::vector<thrift::BgpPeer>& newPeers) {
  auto currentConfig = getConfig();
  auto thriftConfig = currentConfig->getConfig();
  const auto& existingPeers = currentConfig->getPeerToConfig();

  folly::F14FastSet<std::string> addedPeerAddrs;
  for (const auto& peer : newPeers) {
    const auto& peerAddr = *peer.peer_addr();

    if (!existingPeers.contains(folly::IPAddress(peerAddr)) &&
        addedPeerAddrs.insert(peerAddr).second) {
      thriftConfig.peers()->push_back(peer);
    }
  }

  auto newConfig = currentConfig->createConfig(std::move(thriftConfig));
  updateConfig(newConfig);
  return newConfig;
}

std::shared_ptr<const Config> ConfigManager::removePeersFromConfig(
    const std::vector<folly::IPAddress>& peerAddrs) {
  auto currentConfig = getConfig();
  auto thriftConfig = currentConfig->getConfig();

  std::erase_if(*thriftConfig.peers(), [&peerAddrs](const auto& peer) {
    auto maybeAddr = folly::IPAddress::tryFromString(*peer.peer_addr());
    return maybeAddr.hasValue() &&
        std::find(peerAddrs.begin(), peerAddrs.end(), maybeAddr.value()) !=
        peerAddrs.end();
  });

  auto newConfig = currentConfig->createConfig(std::move(thriftConfig));
  updateConfig(newConfig);
  return newConfig;
}

} // namespace facebook::bgp
