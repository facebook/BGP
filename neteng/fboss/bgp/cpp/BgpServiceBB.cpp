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

#include "neteng/fboss/bgp/cpp/BgpServiceBB.h"

#include <fb303/ServiceData.h>
#include <fmt/ranges.h>
#include <folly/IPAddress.h>
#include <folly/ScopeGuard.h>
#include <folly/logging/xlog.h>

#include "fboss/lib/LogThriftCall.h"
#include "magic_enum/magic_enum.hpp"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/health/HealthValidatorBB.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBB.h"

using namespace facebook::neteng::fboss::bgp::thrift;

namespace {
static const std::string kExitNullPtrLogPrefix = "BgpServiceBBExitOrNullPtr";
}

namespace facebook::bgp {

BgpServiceBB::BgpServiceBB(
    PeerManagerBase& peerMgr,
    std::shared_ptr<ConfigManager> configManager,
    RibBase& rib,
    Watchdog& watchdog,
    std::shared_ptr<NetlinkWrapper> nlWrapper,
    bool enable_thrift_protection)
    : BgpServiceBase(
          peerMgr,
          std::move(configManager),
          rib,
          watchdog,
          enable_thrift_protection),
      nlWrapper_(std::move(nlWrapper)) {
  /*
   * Replace the base's NetlinkWrapper-agnostic HealthValidator with a
   * BB-aware one that runs the real NETLINK_TRACKED_INTERFACES check.
   */
  healthValidator_ = std::make_unique<HealthValidatorBB>(
      &peerMgr_,
      &rib_,
      &watchdog_,
      nlWrapper_.get(),
      /*nexthopHandler=*/nullptr,
      configManager_);
}

void BgpServiceBB::augmentEntryStatsForPlatform(TEntryStats& stats) {
  if (nlWrapper_) {
    nlWrapper_->updateEntryStats(stats);
  }
}

void BgpServiceBB::setNetlinkLinkUpHold(
    TResult& result,
    int32_t initialMs,
    int32_t maxMs) {
  auto log = LOG_THRIFT_CALL(DBG2);

  if (!nlWrapper_) {
    result.success() = false;
    result.err() = "This build has no NetlinkWrapper";
    return;
  }
  if (initialMs <= 0) {
    result.success() = false;
    result.err() =
        fmt::format("initial_ms must be more than 0, got {}", initialMs);
    return;
  }
  if (maxMs < initialMs) {
    result.success() = false;
    result.err() = fmt::format(
        "max_ms must be at least initial_ms, got max {} and initial {}",
        maxMs,
        initialMs);
    return;
  }

  nlWrapper_->setLinkUpHoldTimes(
      std::chrono::milliseconds(initialMs), std::chrono::milliseconds(maxMs));
  result.success() = true;
}

void BgpServiceBB::getNetlinkLinkUpHold(TNetlinkLinkUpHold& result) {
  auto log = LOG_THRIFT_CALL(DBG2);

  if (!nlWrapper_) {
    result.enabled() = false;
    return;
  }
  result.enabled() = nlWrapper_->isNetlinkDampeningEnabled();
  const auto times = nlWrapper_->getLinkUpHoldTimes();
  result.initial_ms() = static_cast<int32_t>(times.initial.count());
  result.max_ms() = static_cast<int32_t>(times.max.count());
}

folly::coro::Task<BgpPolicyChangeResult> BgpServiceBB::co_setPeersPolicy(
    std::unique_ptr<std::map<
        std::string,
        std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>>>
        peersPolicy) {
  auto log = LOG_THRIFT_CALL(DBG2);

  if (exitInitiated_ || !peersPolicy) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty peers policy";
    XLOGF(
        ERR,
        "[{}]: Failed to set peers policy. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    BgpStatsBB::incrSetPeersPolicyFailure();
    co_return BgpPolicyChangeResult::INPUT_ERROR;
  }

  if (!peerMgr_.getIsInitialized()) {
    XLOG(ERR, "setPeersPolicy: rejected (BGP is not initialized)");
    BgpStatsBB::incrSetPeersPolicyFailure();
    co_return BgpPolicyChangeResult::INPUT_ERROR;
  }

  auto currentConfig = configManager_->getConfig();
  auto validationResult = validatePeersAndPolicies(
      *peersPolicy, *currentConfig, peerMgr_.getPolicyManager());
  if (validationResult != PolicyValidationResult::SUCCESS) {
    XLOGF(
        ERR,
        "[{}]: Failed to set peers policy. Error: {}",
        kExitNullPtrLogPrefix,
        magic_enum::enum_name(validationResult));
    BgpStatsBB::incrSetPeersPolicyFailure();
    co_return BgpPolicyChangeResult::INPUT_ERROR;
  }

  if (!continueExecution(true)) {
    XLOG(ERR, "setPeersPolicy: request rejected by thrift request dampening");
    BgpStatsBB::incrSetPeersPolicyFailure();
    co_return BgpPolicyChangeResult::INTERNAL_ERROR;
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  try {
    XLOGF(
        INFO,
        "setPeersPolicy validation passed for {} peers",
        peersPolicy->size());

    auto newConfig = configManager_->updatePeerPolicies(*peersPolicy);

    auto resolvedPeerPolicies = resolveEffectivePeerPolicies(
        *newConfig,
        [&peersPolicy](const folly::IPAddress& peerAddr, const BgpPeerConfig&) {
          return peersPolicy->contains(peerAddr.str());
        });

    peerMgr_.updateIngressEgressPolicyNames(std::move(resolvedPeerPolicies));

    BgpStatsBB::incrSetPeersPolicySuccess();
    co_return BgpPolicyChangeResult::POLICIES_APPLIED;
  } catch (const std::exception& error) {
    auto errorMsg = folly::exceptionStr(error);
    XLOGF(ERR, "Failed to update peer policies: {}", errorMsg);
    BgpStatsBB::incrSetPeersPolicyFailure();
    co_return BgpPolicyChangeResult::INTERNAL_ERROR;
  }
}

folly::coro::Task<BgpPolicyChangeResult> BgpServiceBB::co_setPeerGroupsPolicy(
    std::unique_ptr<std::map<
        std::string,
        std::map<facebook::bgp::bgp_policy::DIRECTION, std::string>>>
        peerGroupsPolicy) {
  auto log = LOG_THRIFT_CALL(DBG2);

  if (exitInitiated_ || !peerGroupsPolicy) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty peer groups policy";
    XLOGF(
        ERR,
        "[{}]: Failed to set peer groups policy. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    BgpStatsBB::incrSetPeerGroupsPolicyFailure();
    co_return BgpPolicyChangeResult::INPUT_ERROR;
  }

  if (!peerMgr_.getIsInitialized()) {
    XLOG(ERR, "setPeerGroupsPolicy: rejected (BGP is not initialized)");
    BgpStatsBB::incrSetPeerGroupsPolicyFailure();
    co_return BgpPolicyChangeResult::INPUT_ERROR;
  }

  auto currentConfig = configManager_->getConfig();
  auto validationResult = validatePeerGroupsAndPolicies(
      *peerGroupsPolicy, *currentConfig, peerMgr_.getPolicyManager());
  if (validationResult != PolicyValidationResult::SUCCESS) {
    XLOGF(
        ERR,
        "[{}]: Failed to set peer groups policy. Error: {}",
        kExitNullPtrLogPrefix,
        magic_enum::enum_name(validationResult));
    BgpStatsBB::incrSetPeerGroupsPolicyFailure();
    co_return BgpPolicyChangeResult::INPUT_ERROR;
  }

  if (!continueExecution(true)) {
    XLOG(
        ERR,
        "setPeerGroupsPolicy: request rejected by thrift request dampening");
    BgpStatsBB::incrSetPeerGroupsPolicyFailure();
    co_return BgpPolicyChangeResult::INTERNAL_ERROR;
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  try {
    XLOGF(
        INFO,
        "setPeerGroupsPolicy validation passed for {} peer groups",
        peerGroupsPolicy->size());

    auto newConfig = configManager_->updatePeerGroupPolicies(*peerGroupsPolicy);

    auto resolvedPeerPolicies = resolveEffectivePeerPolicies(
        *newConfig,
        [&peerGroupsPolicy](
            const folly::IPAddress&, const BgpPeerConfig& peerConfig) {
          const auto& pgName = peerConfig.commonPeerGroupConfig.peerGroupName;
          return pgName.has_value() && peerGroupsPolicy->contains(*pgName);
        });

    peerMgr_.updateIngressEgressPolicyNames(std::move(resolvedPeerPolicies));

    BgpStatsBB::incrSetPeerGroupsPolicySuccess();
    co_return BgpPolicyChangeResult::POLICIES_APPLIED;
  } catch (const std::exception& error) {
    auto errorMsg = folly::exceptionStr(error);
    XLOGF(ERR, "Failed to update peer group policies: {}", errorMsg);
    BgpStatsBB::incrSetPeerGroupsPolicyFailure();
    co_return BgpPolicyChangeResult::INTERNAL_ERROR;
  }
}

folly::coro::Task<BgpPolicyChangeResult> BgpServiceBB::co_unsetPeersPolicy(
    std::unique_ptr<
        std::map<std::string, std::set<facebook::bgp::bgp_policy::DIRECTION>>>
        peersToUnset) {
  auto log = LOG_THRIFT_CALL(DBG1);

  if (exitInitiated_ || !peersToUnset) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty peers to unset";
    XLOGF(
        ERR,
        "[{}]: Failed to unset peers policy. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    BgpStatsBB::incrUnsetPeersPolicyFailure();
    co_return BgpPolicyChangeResult::INPUT_ERROR;
  }

  auto currentConfig = configManager_->getConfig();
  for (const auto& [peerAddr, _] : *peersToUnset) {
    if (!currentConfig->validatePeerExists(peerAddr)) {
      XLOGF(ERR, "Peer {} not found in config", peerAddr);
      BgpStatsBB::incrUnsetPeersPolicyFailure();
      co_return BgpPolicyChangeResult::INPUT_ERROR;
    }
  }

  if (!continueExecution(true)) {
    XLOG(ERR, "unsetPeersPolicy: request rejected by thrift request dampening");
    BgpStatsBB::incrUnsetPeersPolicyFailure();
    co_return BgpPolicyChangeResult::INTERNAL_ERROR;
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  try {
    XLOGF(
        INFO,
        "unsetPeersPolicy: unsetting policy for {} peers",
        peersToUnset->size());

    auto newConfig = configManager_->unsetPeerPolicies(*peersToUnset);

    auto resolvedPeerPolicies = resolveEffectivePeerPolicies(
        *newConfig,
        [&peersToUnset](
            const folly::IPAddress& peerAddr, const BgpPeerConfig&) {
          return peersToUnset->count(peerAddr.str()) > 0;
        });

    peerMgr_.updateIngressEgressPolicyNames(std::move(resolvedPeerPolicies));

    BgpStatsBB::incrUnsetPeersPolicySuccess();
    co_return BgpPolicyChangeResult::POLICIES_APPLIED;
  } catch (const std::exception& error) {
    auto errorMsg = folly::exceptionStr(error);
    XLOGF(ERR, "Failed to unset peer policies: {}", errorMsg);
    BgpStatsBB::incrUnsetPeersPolicyFailure();
    co_return BgpPolicyChangeResult::INTERNAL_ERROR;
  }
}

folly::coro::Task<BgpConfigChangeResult> BgpServiceBB::co_addPeers(
    std::unique_ptr<std::vector<facebook::bgp::thrift::BgpPeer>> peers) {
  auto log = LOG_THRIFT_CALL(INFO);

  if (exitInitiated_ || !peers) {
    XLOG(ERR, "addPeers: rejected (exit initiated or null input)");
    BgpStatsBB::incrAddPeersRejected();
    co_return BgpConfigChangeResult::INPUT_ERROR;
  }

  if (!peerMgr_.getIsInitialized()) {
    XLOG(ERR, "addPeers: rejected (BGP is not initialized)");
    BgpStatsBB::incrAddPeersRejected();
    co_return BgpConfigChangeResult::INPUT_ERROR;
  }

  if (peerMgr_.getIsSafeModeOn()) {
    XLOG(ERR, "addPeers: rejected (safe mode is on)");
    BgpStatsBB::incrAddPeersRejected();
    co_return BgpConfigChangeResult::INPUT_ERROR;
  }

  if (peers->empty()) {
    BgpStatsBB::incrAddPeersSuccess();
    co_return BgpConfigChangeResult::CONFIG_APPLIED;
  }

  auto currentConfig = configManager_->getConfig();
  std::set<folly::IPAddress> newPeerAddrs;

  static const folly::F14FastSet<std::string_view> kAllowedAddPeerFields = {
      "local_addr",
      "peer_addr",
      "next_hop4",
      "next_hop6",
      "description",
      "peer_id",
      "ingress_policy_name",
      "egress_policy_name",
      "remote_as_4_byte",
      "peer_group_name",
  };

  for (const auto& peer : *peers) {
    if (!validateAddedPeer(
            peer, kAllowedAddPeerFields, *currentConfig, newPeerAddrs)) {
      BgpStatsBB::incrAddPeersRejected();
      co_return BgpConfigChangeResult::INPUT_ERROR;
    }
  }

  if (!continueExecution(true)) {
    XLOG(ERR, "addPeers: request rejected by thrift request dampening");
    BgpStatsBB::incrAddPeersRejected();
    co_return BgpConfigChangeResult::INTERNAL_ERROR;
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  try {
    XLOGF(INFO, "addPeers validation passed for {} peers", peers->size());

    auto newConfig = configManager_->addPeersToConfig(*peers);

    std::vector<std::shared_ptr<BgpPeerConfig>> peerConfigs;
    for (const auto& peer : *peers) {
      auto peerAddr = folly::IPAddress(*peer.peer_addr());
      peerConfigs.emplace_back(newConfig->getPeerToConfig().at(peerAddr));
    }

    auto result = co_await peerMgr_.addPeers(peerConfigs);
    if (result.hasError()) {
      XLOG(ERR, "addPeers: PeerManagerBase::addPeers failed");
      BgpStatsBB::incrAddPeersRejected();
      co_return BgpConfigChangeResult::INTERNAL_ERROR;
    }

    BgpStatsBB::incrAddPeersSuccess();
    co_return BgpConfigChangeResult::CONFIG_APPLIED;
  } catch (const std::exception& error) {
    XLOGF(ERR, "addPeers: exception: {}", folly::exceptionStr(error));
    BgpStatsBB::incrAddPeersRejected();
    co_return BgpConfigChangeResult::INTERNAL_ERROR;
  }
}

std::optional<folly::IPAddress> BgpServiceBB::validatePeerAddress(
    const std::string& addrStr) {
  if (addrStr.find('/') != std::string::npos) {
    XLOGF(ERR, "dynamic peer (CIDR) not supported: '{}'", addrStr);
    return std::nullopt;
  }

  if (!folly::IPAddress::validate(addrStr)) {
    XLOGF(ERR, "invalid peer address '{}'", addrStr);
    return std::nullopt;
  }

  return folly::IPAddress(addrStr);
}

bool BgpServiceBB::validateAddedPeer(
    const thrift::BgpPeer& peer,
    const folly::F14FastSet<std::string_view>& allowedFields,
    const Config& currentConfig,
    std::set<folly::IPAddress>& newPeerAddrs) {
  // Reject unsupported fields
  auto unsupported = getUnsupportedBgpPeerFields(peer, allowedFields);
  if (!unsupported.empty()) {
    XLOGF(
        ERR,
        "addPeers: unsupported fields [{}] set for peer {}",
        fmt::join(unsupported, ", "),
        *peer.peer_addr());
    return false;
  }

  auto maybePeerAddr = validatePeerAddress(*peer.peer_addr());
  if (!maybePeerAddr) {
    return false;
  }
  auto peerAddr = *maybePeerAddr;

  // Check for duplicates within the new batch
  if (!newPeerAddrs.insert(peerAddr).second) {
    XLOGF(
        ERR, "addPeers: duplicate peer {} within the request", peerAddr.str());
    return false;
  }

  const auto& existingPeers = currentConfig.getPeerToConfig();
  if (existingPeers.count(peerAddr)) {
    XLOGF(ERR, "addPeers: peer {} already exists in config", peerAddr.str());
    return false;
  }

  // Validate peer-group reference exists
  if (peer.peer_group_name().has_value() &&
      !currentConfig.validatePeerGroupExists(peer.peer_group_name().value())) {
    XLOGF(
        ERR,
        "addPeers: peer-group '{}' does not exist for peer {}",
        peer.peer_group_name().value(),
        *peer.peer_addr());
    return false;
  }

  // Validate policy names exist
  const auto& policyManager = peerMgr_.getPolicyManager();
  if (peer.ingress_policy_name().has_value() && policyManager &&
      !policyManager->isPolicyPresent(peer.ingress_policy_name().value())) {
    XLOGF(
        ERR,
        "addPeers: ingress policy '{}' does not exist for peer {}",
        peer.ingress_policy_name().value(),
        *peer.peer_addr());
    return false;
  }
  if (peer.egress_policy_name().has_value() && policyManager &&
      !policyManager->isPolicyPresent(peer.egress_policy_name().value())) {
    XLOGF(
        ERR,
        "addPeers: egress policy '{}' does not exist for peer {}",
        peer.egress_policy_name().value(),
        *peer.peer_addr());
    return false;
  }

  return true;
}

bool BgpServiceBB::validateDeletedPeer(
    const std::string& addrStr,
    std::vector<folly::IPAddress>& validatedAddrs) {
  auto maybePeerAddr = validatePeerAddress(addrStr);
  if (!maybePeerAddr) {
    return false;
  }

  // Non-existent peer is a no-op, not an error
  validatedAddrs.emplace_back(std::move(*maybePeerAddr));
  return true;
}

folly::coro::Task<BgpConfigChangeResult> BgpServiceBB::co_delPeers(
    std::unique_ptr<std::vector<std::string>> peerAddrs) {
  auto log = LOG_THRIFT_CALL(INFO);

  if (exitInitiated_ || !peerAddrs) {
    XLOG(ERR, "delPeers: rejected (exit initiated or null input)");
    BgpStatsBB::incrDelPeersRejected();
    co_return BgpConfigChangeResult::INPUT_ERROR;
  }

  if (!peerMgr_.getIsInitialized()) {
    XLOG(ERR, "delPeers: rejected (BGP is not initialized)");
    BgpStatsBB::incrDelPeersRejected();
    co_return BgpConfigChangeResult::INPUT_ERROR;
  }

  if (peerMgr_.getIsSafeModeOn()) {
    XLOG(ERR, "delPeers: rejected (safe mode is on)");
    BgpStatsBB::incrDelPeersRejected();
    co_return BgpConfigChangeResult::NOT_IMPLEMENTED;
  }

  if (peerAddrs->empty()) {
    BgpStatsBB::incrDelPeersSuccess();
    co_return BgpConfigChangeResult::CONFIG_APPLIED;
  }

  std::vector<folly::IPAddress> validatedAddrs;
  validatedAddrs.reserve(peerAddrs->size());

  for (const auto& addrStr : *peerAddrs) {
    if (!validateDeletedPeer(addrStr, validatedAddrs)) {
      BgpStatsBB::incrDelPeersRejected();
      co_return BgpConfigChangeResult::INPUT_ERROR;
    }
  }

  if (!continueExecution(true)) {
    XLOG(ERR, "delPeers: request rejected by thrift request dampening");
    BgpStatsBB::incrDelPeersRejected();
    co_return BgpConfigChangeResult::INTERNAL_ERROR;
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  try {
    XLOGF(
        INFO, "delPeers validation passed for {} peers", validatedAddrs.size());

    configManager_->removePeersFromConfig(validatedAddrs);

    auto result = co_await peerMgr_.delPeers(validatedAddrs);
    if (result.hasError()) {
      XLOG(ERR, "delPeers: PeerManagerBase::delPeers failed");
      BgpStatsBB::incrDelPeersRejected();
      co_return BgpConfigChangeResult::INTERNAL_ERROR;
    }

    BgpStatsBB::incrDelPeersSuccess();
    co_return BgpConfigChangeResult::CONFIG_APPLIED;
  } catch (const std::exception& error) {
    XLOGF(ERR, "delPeers: exception: {}", folly::exceptionStr(error));
    BgpStatsBB::incrDelPeersRejected();
    co_return BgpConfigChangeResult::INTERNAL_ERROR;
  }
}

} // namespace facebook::bgp
