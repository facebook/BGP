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

#include <memory>
#include <optional>
#include <set>
#include <tuple>
#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "folly/Synchronized.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"

DECLARE_string(bgp_config_backup);

namespace facebook::bgp {

/**
 * Thread-safe configuration manager that provides atomic config updates
 * and consistent reads across multiple components (BgpServiceBase,
 * PeerManagerBase)
 */
class ConfigManager {
 public:
  /*
   * Pass splitConfigPolicy = true when bgpd was started with a --policy file.
   * The policy body (communities, localprefs, policies) then belongs to that
   * file, so it is removed from the config before the config is written to
   * disk.
   *
   * If it is left unset, the value is read from
   * initialConfig->splitConfigPolicy().
   */
  explicit ConfigManager(
      std::shared_ptr<const Config> initialConfig,
      std::string configFilePath = "",
      std::optional<bool> splitConfigPolicy = std::nullopt)
      : splitConfigPolicy_(
            splitConfigPolicy.has_value() ? *splitConfigPolicy
                                          : initialConfig->splitConfigPolicy()),
        config_(std::make_pair(std::move(initialConfig), 0)),
        configFilePath_(std::move(configFilePath)) {}

  // Thread-safe config access
  std::shared_ptr<const Config> getConfig() const {
    return config_.rlock()->first;
  }

  // Get current config version (for race avoidance in PeerManagerBase)
  uint64_t getConfigVersion() const {
    return config_.rlock()->second;
  }

  // Thread-safe config update with file backup and write
  void updateConfig(std::shared_ptr<const Config> newConfig);

  // Specialized update for peer policies
  std::shared_ptr<const Config> updatePeerPolicies(
      const std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>&
          peerPolicies);

  // Specialized update for peer group policies
  std::shared_ptr<const Config> updatePeerGroupPolicies(
      const std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>&
          peerGroupPolicies);

  // Unset peer-level policies, causing peers to fall back to group policy
  std::shared_ptr<const Config> unsetPeerPolicies(
      const std::map<std::string, std::set<bgp_policy::DIRECTION>>&
          peersToUnset);

  // Add new peers to the config, returning the updated config.
  std::shared_ptr<const Config> addPeersToConfig(
      const std::vector<facebook::bgp::thrift::BgpPeer>& newPeers);

  // Remove peers from the config by address, returning the updated config.
  std::shared_ptr<const Config> removePeersFromConfig(
      const std::vector<folly::IPAddress>& peerAddrs);

 private:
  /*
   * splitConfigPolicy_ is the default mode (separate policy file via
   * --policy flag). Declared first so it is initialized before config_,
   * allowing std::move of initialConfig in the constructor initializer list.
   */
  bool splitConfigPolicy_{false};
  folly::Synchronized<std::pair<std::shared_ptr<const Config>, uint64_t>>
      config_;
  std::string configFilePath_;

  std::shared_ptr<const Config> createConfigWithUpdatedPeerPolicies(
      const std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>&
          peerPolicies,
      const Config& currentConfig);

  std::shared_ptr<const Config> createConfigWithUpdatedPeerGroupPolicies(
      const std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>&
          peerGroupPolicies,
      const Config& currentConfig);

  // Helper methods following AdjRib pattern
  bool updatePolicyName(
      apache::thrift::optional_field_ref<std::string&> currentPolicy,
      const std::string& newPolicy);

  void updateIngressEgressPeerPolicies(
      thrift::BgpPeer& peer,
      const std::map<bgp_policy::DIRECTION, std::string>&
          directionToPolicyName) noexcept;

  void unsetIngressEgressPeerPolicies(
      thrift::BgpPeer& peer,
      const std::set<bgp_policy::DIRECTION>& directionsToUnset) noexcept;

  void updateIngressEgressPeerGroupPolicies(
      thrift::PeerGroup& peerGroup,
      const std::map<bgp_policy::DIRECTION, std::string>&
          directionToPolicyName) noexcept;

  std::shared_ptr<const Config> createConfigWithUnsetPeerPolicies(
      const std::map<std::string, std::set<bgp_policy::DIRECTION>>&
          peersToUnset,
      const Config& currentConfig);
};

} // namespace facebook::bgp
