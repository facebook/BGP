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

#include <folly/logging/xlog.h>

#include "fboss/agent/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"

using namespace facebook::neteng::fboss::bgp_attr;
using namespace facebook::neteng::fboss::bgp::thrift;
using namespace facebook::nettools::bgplib;

using facebook::nettools::bgplib::getCurrentTimeMs;
using facebook::network::toBinaryAddress;
using folly::CIDRNetwork;
using folly::IPAddress;
using std::vector;

namespace facebook::bgp {

// As convention we try to avoid returning using 'pass by reference'
// instead we return the value as return object. But as thrift
// service passes us reference to map and not expect return
// to avoid a copy, follow same style we are passing prefixToPath&
void AdjRib::getNetworks(
    std::map<TIpPrefix, TBgpPath>& prefixToPath,
    const RouteFilterType& type) noexcept {
  switch (type) {
    case RouteFilterType::PRE_FILTER_RECEIVED:
    case RouteFilterType::POST_FILTER_RECEIVED:
      if (!recAddPath_) {
        for (auto itr = adjRibInLiteTree_.begin();
             itr != adjRibInLiteTree_.end();
             itr++) {
          const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
          auto pfxAndPath = convertEntryToPath(prefix, *itr->value(), type);
          if (pfxAndPath.has_value()) {
            prefixToPath[pfxAndPath.value().first] = pfxAndPath.value().second;
          }
        }
      } else {
        for (auto itr = adjRibInPathTree_.begin();
             itr != adjRibInPathTree_.end();
             itr++) {
          const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
          for (const auto& [_, adjRibEntry] : itr->value()) {
            auto pfxAndPath = convertEntryToPath(prefix, *adjRibEntry, type);
            if (pfxAndPath.has_value()) {
              prefixToPath[pfxAndPath.value().first] =
                  pfxAndPath.value().second;
            }
          }
        }
      }
      break;
    case RouteFilterType::PRE_FILTER_ADVERTISED:
    case RouteFilterType::POST_FILTER_ADVERTISED: {
      // With update groups, an in-sync peer's RIB-OUT entries live under the
      // group owner key; resolve the entry visible to this peer (peer-owned,
      // shared group, or omitted) via the version-gated resolver.
      //
      // A peer unregistered from its update group (session down) has no
      // RIB-OUT left to report. See AdjRibOutGroup::unregisterPeer.
      if (!adjRibOutGroup_) {
        break;
      }
      const auto sharingVersion = getRibOutSharingVersion();
      if (!sendAddPath_) {
        for (auto itr = adjRibOutGroup_->LiteTree_.begin();
             itr != adjRibOutGroup_->LiteTree_.end();
             itr++) {
          const auto* adjRibEntry = adjRibOutGroup_->resolveLiteEntryForPeer(
              itr->value(), getPeerOwnerKey(), sharingVersion);
          if (!adjRibEntry) {
            continue;
          }
          const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
          auto pfxAndPath = convertEntryToPath(prefix, *adjRibEntry, type);
          if (pfxAndPath.has_value()) {
            prefixToPath[pfxAndPath.value().first] = pfxAndPath.value().second;
          }
        }
      } else {
        for (auto itr = adjRibOutGroup_->PathTree_.begin();
             itr != adjRibOutGroup_->PathTree_.end();
             itr++) {
          const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
          adjRibOutGroup_->resolvePathEntriesForPeer(
              itr->value(),
              getPeerOwnerKey(),
              sharingVersion,
              [&](uint32_t /*pathId*/, const AdjRibEntry& adjRibEntry) {
                auto pfxAndPath = convertEntryToPath(prefix, adjRibEntry, type);
                if (pfxAndPath.has_value()) {
                  prefixToPath[pfxAndPath.value().first] =
                      pfxAndPath.value().second;
                }
              });
        }
      }
      break;
    }
    default:
      return;
  }
}

// Get pre/post, in/out networks for fboss thrift service (show CLI)
void AdjRib::getNetworks2(
    std::map<
        neteng::fboss::bgp_attr::TIpPrefix,
        std::vector<neteng::fboss::bgp::thrift::TBgpPath>>& prefixToPath,
    const RouteFilterType& type) noexcept {
  switch (type) {
    case RouteFilterType::PRE_FILTER_RECEIVED:
    case RouteFilterType::POST_FILTER_RECEIVED:
      if (!recAddPath_) {
        for (auto itr = adjRibInLiteTree_.begin();
             itr != adjRibInLiteTree_.end();
             itr++) {
          const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
          auto pfxAndPath =
              convertEntryToPath(prefix, *itr->value(), type, kDefaultPathID);
          if (pfxAndPath.has_value()) {
            prefixToPath[pfxAndPath.value().first].push_back(
                pfxAndPath.value().second);
          }
        }
      } else {
        for (auto itr = adjRibInPathTree_.begin();
             itr != adjRibInPathTree_.end();
             itr++) {
          const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
          for (const auto& [network, adjRibEntry] : itr->value()) {
            auto pfxAndPath =
                convertEntryToPath(prefix, *adjRibEntry, type, network);
            if (pfxAndPath.has_value()) {
              prefixToPath[pfxAndPath.value().first].emplace_back(
                  pfxAndPath.value().second);
            }
          }
        }
      }
      break;
    case RouteFilterType::PRE_FILTER_ADVERTISED:
    case RouteFilterType::POST_FILTER_ADVERTISED: {
      // With update groups, an in-sync peer's RIB-OUT entries live under the
      // group owner key; resolve the entry visible to this peer (peer-owned,
      // shared group, or omitted) via the version-gated resolver.
      //
      // A peer unregistered from its update group (session down) has no
      // RIB-OUT left to report. See AdjRibOutGroup::unregisterPeer.
      if (!adjRibOutGroup_) {
        break;
      }
      const auto sharingVersion = getRibOutSharingVersion();
      if (!sendAddPath_) {
        for (auto itr = adjRibOutGroup_->LiteTree_.begin();
             itr != adjRibOutGroup_->LiteTree_.end();
             itr++) {
          const auto* adjRibEntry = adjRibOutGroup_->resolveLiteEntryForPeer(
              itr->value(), getPeerOwnerKey(), sharingVersion);
          if (!adjRibEntry) {
            continue;
          }
          const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
          auto pfxAndPath =
              convertEntryToPath(prefix, *adjRibEntry, type, kDefaultPathID);
          if (pfxAndPath.has_value()) {
            prefixToPath[pfxAndPath.value().first].push_back(
                pfxAndPath.value().second);
          }
        }
      } else {
        for (auto itr = adjRibOutGroup_->PathTree_.begin();
             itr != adjRibOutGroup_->PathTree_.end();
             itr++) {
          const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
          adjRibOutGroup_->resolvePathEntriesForPeer(
              itr->value(),
              getPeerOwnerKey(),
              sharingVersion,
              [&](uint32_t pathId, const AdjRibEntry& adjRibEntry) {
                auto pfxAndPath =
                    convertEntryToPath(prefix, adjRibEntry, type, pathId);
                if (pfxAndPath.has_value()) {
                  prefixToPath[pfxAndPath.value().first].emplace_back(
                      pfxAndPath.value().second);
                }
              });
        }
      }
      break;
    }
    default:
      return;
  }
}

std::optional<std::pair<TIpPrefix, TBgpPath>> AdjRib::convertEntryToPath(
    const folly::CIDRNetwork& prefix,
    const AdjRibEntry& adjRibEntry,
    const RouteFilterType& type,
    const std::optional<uint32_t>& pathId) noexcept {
  TBgpPath path;
  TIpPrefix tPrefix;
  std::shared_ptr<const std::string> postInPolicy;
  std::shared_ptr<const std::string> postOutPolicy;
  switch (type) {
    case RouteFilterType::PRE_FILTER_RECEIVED:
      if (!adjRibEntry.getPreIn()) {
        // Not a learnt route from this peer
        return std::nullopt;
      }
      path = createTBgpPath(*(adjRibEntry.getPreIn()));
      postInPolicy = adjRibEntry.getPostInPolicy();
      if (postInPolicy) {
        path.policy_name() = *postInPolicy;
      }
      break;
    case RouteFilterType::POST_FILTER_RECEIVED:
      if (!adjRibEntry.getPostAttr()) {
        return std::nullopt;
      }
      path = createTBgpPath(*(adjRibEntry.getPostAttr()));
      postInPolicy = adjRibEntry.getPostInPolicy();
      if (postInPolicy) {
        path.policy_name() = *postInPolicy;
      }
      break;
    case RouteFilterType::PRE_FILTER_ADVERTISED:
      if (!adjRibEntry.getPreOut()) {
        return std::nullopt;
      }
      path = createTBgpPath(*(adjRibEntry.getPreOut()));
      postOutPolicy = adjRibEntry.getPostOutPolicy();
      if (postOutPolicy) {
        path.policy_name() = *postOutPolicy;
      }
      break;
    case RouteFilterType::POST_FILTER_ADVERTISED:
      if (!adjRibEntry.getPostAttr()) {
        return std::nullopt;
      }
      auto postOutAttrs = adjRibEntry.getPostAttr();
      if (postOutAttrs) {
        auto nextHop = getNewNexthopFromAttributesOut(
            prefix.first.isV4(),
            postOutAttrs,
            adjRibEntry.isNexthopSetByPolicy());
        path = createTBgpPath(*postOutAttrs);
        if (!nextHop.empty()) {
          auto tNexthop = createTIpPrefix(nextHop);
          path.next_hop() = std::move(tNexthop);
        }
      }
      postOutPolicy = adjRibEntry.getPostOutPolicy();
      if (postOutPolicy) {
        path.policy_name() = *postOutPolicy;
      }
      break;
  }
  if (pathId.has_value()) {
    path.path_id() = pathId.value();
  }
  path.last_modified_time() = adjRibEntry.getLastUpdateRcvdTime();

  auto binAddr = toBinaryAddress(prefix.first);

  if (prefix.first.family() == AF_INET) {
    tPrefix.afi() = TBgpAfi::AFI_IPV4;
  } else {
    tPrefix.afi() = TBgpAfi::AFI_IPV6;
  }
  tPrefix.num_bits() = prefix.second;
  tPrefix.prefix_bin() = binAddr.addr()->toStdString();
  return std::make_pair(tPrefix, path);
}

// As convention we try to avoid returning using 'pass by reference'
// instead we return the value as return object. But as thrift
// service passes us reference to map and not expect return
// to avoid a copy, follow same style we are passing prefixToPath&
void AdjRib::getDryRunNetworks(
    std::map<TIpPrefix, TBgpPath>& prefixToPath,
    const std::unique_ptr<std::string>& file_name,
    const RouteFilterType& type) noexcept {
  prefixToPath.clear();
  std::shared_ptr<facebook::bgp::Config> config;
  std::shared_ptr<facebook::bgp::PolicyManager> policyManager;
  try {
    // Create policy manager using new config file
    config = Config::createDryRunConfig(file_name);
    policyManager = Config::createPolicyManager(config);
    if (!policyManager) {
      // No policies in the configuration file, nothing to dryrun and verify OR
      // Error while parsing config file and policies.
      return;
    }
  } catch (const std::exception& ex) {
    // This is a giant catch all. Config parsing, policy parsing does lot of
    // sanity checks on config file and throw errors so that main() exits for
    // errors in config file. For dryRun we do not want to exit or crash
    // for any mistakes in config files. Hence this giant catch all.
    XLOGF(
        ERR,
        "DryRun failed: Verify config file {}. {}",
        *file_name,
        folly::exceptionStr(ex));
    return;
  }

  auto peerConfig = config->getConfigOfAPeer(getPeerAddress());
  if (!peerConfig) {
    XLOGF(
        ERR,
        "DryRun failed: No config exists for peer {}",
        getPeerAddress().str());
    return;
  }

  switch (type) {
    case RouteFilterType::POST_FILTER_RECEIVED:
      for (auto itr = adjRibInLiteTree_.begin(); itr != adjRibInLiteTree_.end();
           itr++) {
        const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
        auto pfxAndPath = getDryRunPaths(
            policyManager, peerConfig, prefix, *itr->value(), type);
        if (pfxAndPath.has_value()) {
          prefixToPath[pfxAndPath.value().first] = pfxAndPath.value().second;
        }
      }
      for (auto itr = adjRibInPathTree_.begin(); itr != adjRibInPathTree_.end();
           itr++) {
        const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
        for (const auto& [_, adjRibEntry] : itr->value()) {
          TBgpPath tPath;
          TIpPrefix tPrefix;

          auto pfxAndPath = getDryRunPaths(
              policyManager, peerConfig, prefix, *adjRibEntry, type);
          if (pfxAndPath.has_value()) {
            prefixToPath[pfxAndPath.value().first] = pfxAndPath.value().second;
          }
        }
      }
      break;
    case RouteFilterType::POST_FILTER_ADVERTISED: {
      // With update groups, an in-sync peer's RIB-OUT entries live under the
      // group owner key; resolve the entry visible to this peer (peer-owned,
      // shared group, or omitted) via the version-gated resolver.
      //
      // A peer unregistered from its update group (session down) has no
      // RIB-OUT left to report. See AdjRibOutGroup::unregisterPeer.
      if (!adjRibOutGroup_) {
        break;
      }
      const auto sharingVersion = getRibOutSharingVersion();
      for (auto itr = adjRibOutGroup_->LiteTree_.begin();
           itr != adjRibOutGroup_->LiteTree_.end();
           itr++) {
        const auto* adjRibEntry = adjRibOutGroup_->resolveLiteEntryForPeer(
            itr->value(), getPeerOwnerKey(), sharingVersion);
        if (!adjRibEntry) {
          continue;
        }
        const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
        auto pfxAndPath = getDryRunPaths(
            policyManager, peerConfig, prefix, *adjRibEntry, type);
        if (pfxAndPath.has_value()) {
          prefixToPath[pfxAndPath.value().first] = pfxAndPath.value().second;
        }
      }
      for (auto itr = adjRibOutGroup_->PathTree_.begin();
           itr != adjRibOutGroup_->PathTree_.end();
           itr++) {
        const folly::CIDRNetwork prefix = {itr.ipAddress(), itr.masklen()};
        adjRibOutGroup_->resolvePathEntriesForPeer(
            itr->value(),
            getPeerOwnerKey(),
            sharingVersion,
            [&](uint32_t /*pathId*/, const AdjRibEntry& adjRibEntry) {
              auto pfxAndPath = getDryRunPaths(
                  policyManager, peerConfig, prefix, adjRibEntry, type);
              if (pfxAndPath.has_value()) {
                prefixToPath[pfxAndPath.value().first] =
                    pfxAndPath.value().second;
              }
            });
      }
      break;
    }
    default:
      return;
  }
}

std::optional<std::pair<TIpPrefix, TBgpPath>> AdjRib::getDryRunPaths(
    const std::shared_ptr<facebook::bgp::PolicyManager>& policyManager,
    const std::optional<const BgpCommonPeerGroupConfig>& peerConfig,
    const folly::CIDRNetwork& prefix,
    const AdjRibEntry& adjRibEntry,
    const RouteFilterType& type) noexcept {
  TBgpPath tPath;
  TIpPrefix tPrefix;

  switch (type) {
    case RouteFilterType::POST_FILTER_RECEIVED:
      if (!adjRibEntry.getPreIn()) {
        // There is no preIn, so we can't generate postIn
        return std::nullopt;
      }

      // We do not rely on adjRib policy name, but instead use the policy
      // name from config parsing so that we can handle the case where
      // policy names have changed between running config and dry run
      // config.
      if (peerConfig->ingressPolicyName) {
        auto policyOut = policyManager->applyPolicy(
            *peerConfig->ingressPolicyName,
            PolicyInMessage({prefix}, adjRibEntry.getPreIn()->clone()));

        const auto& search = policyOut.result.find(prefix);
        CHECK(search != policyOut.result.end());
        if (search->second->attrs) {
          tPath = createTBgpPath(*(search->second->attrs));
        } else {
          return std::nullopt;
        }
      } else {
        tPath = createTBgpPath(*(adjRibEntry.getPreIn()));
      }
      break;

    case RouteFilterType::POST_FILTER_ADVERTISED:
      if (!adjRibEntry.getPreOut()) {
        // There is no preOut, so we can't generate postOut
        return std::nullopt;
      }

      // This handles the case where policy names have changed between
      // running config and dry run config.
      if (peerConfig->egressPolicyName) {
        auto policyOut = policyManager->applyPolicy(
            *peerConfig->egressPolicyName,
            PolicyInMessage({prefix}, adjRibEntry.getPreOut()->clone()));
        const auto& search = policyOut.result.find(prefix);
        CHECK(search != policyOut.result.end());
        if (search->second->attrs) {
          tPath = createTBgpPath(*(search->second->attrs));
        } else {
          return std::nullopt;
        }
      } else {
        tPath = createTBgpPath(*(adjRibEntry.getPreOut()));
      }
      break;

    default:
      return std::nullopt;
  }

  auto binAddr = toBinaryAddress(prefix.first);

  if (prefix.first.family() == AF_INET) {
    tPrefix.afi() = TBgpAfi::AFI_IPV4;
  } else {
    tPrefix.afi() = TBgpAfi::AFI_IPV6;
  }
  tPrefix.num_bits() = prefix.second;
  tPrefix.prefix_bin() = binAddr.addr()->toStdString();

  return std::make_pair(tPrefix, tPath);
}

} // namespace facebook::bgp
