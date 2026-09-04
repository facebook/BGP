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

#include <fb303/ServiceData.h>
#include <fboss/lib/LogThriftCall.h>
#include <folly/ExceptionString.h>
#include <folly/ScopeGuard.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <neteng/fboss/bgp/cpp/BgpServiceDC.h>
#include <neteng/fboss/bgp/cpp/BgpServiceUtil.h>
#include <neteng/fboss/bgp/cpp/peer/PeerManagerBase.h>
#include <neteng/fboss/bgp/cpp/rib/RibDC.h>
#include <neteng/fboss/bgp/cpp/rib/RibFileUtils.h>
#include <neteng/fboss/bgp/cpp/stats/StatsDC.h>
#include <neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h>
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/EvbUtils.h"

using namespace facebook::neteng::fboss::bgp_attr;
using namespace facebook::neteng::fboss::bgp::thrift;
using std::chrono::milliseconds;

namespace facebook::bgp {

DEFINE_int32(
    thrift_route_add_delay_ms,
    10,
    "Delay in msec between processing route adds from thrift calls (for testing only)");

static const std::string kExitNullPtrLogPrefix = "BgpServiceDCExitOrNullPtr";

BgpServiceDC::BgpServiceDC(
    PeerManagerBase& peerMgr,
    std::shared_ptr<ConfigManager> configManager,
    RibDC& rib,
    std::shared_ptr<NeighborWatcher> neighborWatcher,
    Watchdog& watchdog,
    bool enable_thrift_protection)
    : BgpServiceBase(
          peerMgr,
          std::move(configManager),
          rib,
          watchdog,
          enable_thrift_protection),
      ribDC_(rib),
      neighborWatcher_(std::move(neighborWatcher)),
      dcRib_(rib) {}

folly::coro::Task<std::unique_ptr<TCanonicalRibState>>
BgpServiceDC::co_getRibEntriesCanonical(TBgpAfi afi) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || !continueExecution(true)) {
    co_return std::make_unique<TCanonicalRibState>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };
  auto result = co_await co_runOnEvbWithTimeout(
      dcRib_.getEventBase(),
      [this, afi]() { return dcRib_.getRibEntriesCanonical(afi); },
      kRibThriftHandlerTimeout);
  if (result.hasValue()) {
    co_return std::make_unique<TCanonicalRibState>(std::move(result.value()));
  }
  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(
        ERR,
        "getRibEntriesCanonical timed out, Rib evb unresponsive, afi={}",
        static_cast<int>(afi));
  } else {
    XLOGF(
        ERR,
        "getRibEntriesCanonical failed: {}, afi={}",
        result.exception().what(),
        static_cast<int>(afi));
  }
  co_return std::make_unique<TCanonicalRibState>();
}

folly::coro::Task<std::unique_ptr<TCanonicalRibState>>
BgpServiceDC::co_getRibPrefixCanonical(std::unique_ptr<std::string> prefix) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || prefix == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty prefix";
    XLOGF(
        ERR,
        "[{}]: Failed to get rib prefix canonical. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    co_return std::make_unique<TCanonicalRibState>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TCanonicalRibState>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      dcRib_.getEventBase(),
      [this, p = std::move(prefix)]() mutable {
        return dcRib_.getRibPrefixCanonical(std::move(p));
      },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TCanonicalRibState>(std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getRibPrefixCanonical timed out, Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getRibPrefixCanonical failed: {}", result.exception().what());
  }
  co_return std::make_unique<TCanonicalRibState>();
}

folly::coro::Task<std::unique_ptr<TCanonicalRibState>>
BgpServiceDC::co_getRibEntriesForCommunitiesCanonical(
    TBgpAfi afi,
    std::unique_ptr<std::vector<std::string>> communities) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || communities == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty communities list";
    XLOGF(
        ERR,
        "[{}]: Failed to get rib entries for communities canonical. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    co_return std::make_unique<TCanonicalRibState>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TCanonicalRibState>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  std::vector<nettools::bgplib::BgpAttrCommunityC> comms;
  for (const auto& community : *communities) {
    std::optional<nettools::bgplib::BgpAttrCommunityC> comm =
        nettools::bgplib::BgpAttrCommunityC::createBgpAttrCommunity(community);
    if (comm == std::nullopt) {
      /*
       * Parity with the legacy co_getRibEntriesForCommunities sibling: surface
       * an unparseable community to the client rather than silently returning
       * an empty result.
       */
      throw std::invalid_argument("Invalid Community Value!");
    }
    comms.emplace_back(*comm);
  }

  auto result = co_await co_runOnEvbWithTimeout(
      dcRib_.getEventBase(),
      [this, afi, comms = std::move(comms)]() {
        return dcRib_.getRibEntriesForCommunitiesCanonical(afi, comms);
      },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TCanonicalRibState>(std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(
        ERR,
        "getRibEntriesForCommunitiesCanonical timed out, Rib evb unresponsive, afi={}",
        static_cast<int>(afi));
  } else {
    XLOGF(
        ERR,
        "getRibEntriesForCommunitiesCanonical failed: {}, afi={}",
        result.exception().what(),
        static_cast<int>(afi));
  }
  co_return std::make_unique<TCanonicalRibState>();
}

folly::coro::Task<std::unique_ptr<TCanonicalRibState>>
BgpServiceDC::co_getRibEntriesForCommunityCanonical(
    TBgpAfi afi,
    std::unique_ptr<std::string> community) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || community == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty community";
    XLOGF(
        ERR,
        "[{}]: Failed to get rib entries for community canonical. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    co_return std::make_unique<TCanonicalRibState>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TCanonicalRibState>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      dcRib_.getEventBase(),
      [this, afi, c = std::move(community)]() mutable {
        return dcRib_.getRibEntriesForCommunityCanonical(afi, std::move(c));
      },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TCanonicalRibState>(std::move(result.value()));
  }

  if (result.exception().is_compatible_with<std::invalid_argument>()) {
    /*
     * The community parse runs on the RIB evb; propagate its parse failure to
     * the client, matching the legacy co_getRibEntriesForCommunity sibling.
     */
    throw std::invalid_argument("Invalid Community Value!");
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(
        ERR,
        "getRibEntriesForCommunityCanonical timed out, Rib evb unresponsive, afi={}",
        static_cast<int>(afi));
  } else {
    XLOGF(
        ERR,
        "getRibEntriesForCommunityCanonical failed: {}, afi={}",
        result.exception().what(),
        static_cast<int>(afi));
  }
  co_return std::make_unique<TCanonicalRibState>();
}

folly::coro::Task<std::unique_ptr<TCanonicalRibState>>
BgpServiceDC::co_getRibSubprefixesCanonical(
    std::unique_ptr<std::string> prefix) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || prefix == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty prefix";
    XLOGF(
        ERR,
        "[{}]: Failed to get rib subprefixes canonical. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    co_return std::make_unique<TCanonicalRibState>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TCanonicalRibState>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      dcRib_.getEventBase(),
      [this, p = std::move(prefix)]() mutable {
        return dcRib_.getRibSubprefixesCanonical(std::move(p));
      },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TCanonicalRibState>(std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getRibSubprefixesCanonical timed out, Rib evb unresponsive");
  } else {
    XLOGF(
        ERR,
        "getRibSubprefixesCanonical failed: {}",
        result.exception().what());
  }
  co_return std::make_unique<TCanonicalRibState>();
}

folly::coro::Task<bool> BgpServiceDC::co_getIsSafeModeOn() {
  auto log = LOG_THRIFT_CALL(DBG2);
  co_return peerMgr_.getIsSafeModeOn();
}

int64_t BgpServiceDC::getGoldenVipsCount() {
  // Get the current RibPolicy instance.
  auto log = LOG_THRIFT_CALL(DBG2);
  return AdjRibPrefixSet::get()->goldenVipSize();
}

folly::coro::Task<std::unique_ptr<TGoldenPrefixesPolicyStatus>>
BgpServiceDC::co_getGoldenPrefixesPolicyStatus() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<TGoldenPrefixesPolicyStatus>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TGoldenPrefixesPolicyStatus>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto goldenPolicy = std::make_unique<TGoldenPrefixesPolicyStatus>();

  auto ribResult = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getRouteFilterPolicy(); },
      kRibThriftHandlerTimeout);

  if (ribResult.hasValue()) {
    goldenPolicy->policy() = std::move(ribResult.value());
  } else {
    if (ribResult.exception().is_compatible_with<folly::FutureTimeout>()) {
      XLOGF(
          ERR,
          "getGoldenPrefixesPolicyStatus timed out — Rib evb unresponsive");
    } else {
      XLOGF(
          ERR,
          "getGoldenPrefixesPolicyStatus failed: {}",
          ribResult.exception().what());
    }
  }

  goldenPolicy->isPolicyActive() = peerMgr_.getIsGoldenPrefixPolicyActive();
  co_return std::move(goldenPolicy);
}

folly::coro::Task<void> BgpServiceDC::co_removeSafeModeFile() {
  auto log = LOG_THRIFT_CALL(DBG2);

  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  peerMgr_.removeSafeModeFile();
  co_return;
}

folly::coro::Task<std::unique_ptr<rib_policy::TRibPolicy>>
BgpServiceDC::co_getRibPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<rib_policy::TRibPolicy>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<rib_policy::TRibPolicy>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getRibPolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<rib_policy::TRibPolicy>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getRibPolicy timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getRibPolicy failed: {}", result.exception().what());
  }
  co_return std::make_unique<rib_policy::TRibPolicy>();
}

folly::coro::Task<void> BgpServiceDC::co_clearRibPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  /*
   * Enqueue directly on the calling thread; the coalescing MergeQueue is
   * thread-safe, so there is no RIB evb hop to queue behind a long walk.
   */
  rib_.clearRibPolicy();
}

/**
 * [Route Attribute Policy]
 */
folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setRouteAttributePolicy(
    std::unique_ptr<rib_policy::TRouteAttributePolicy> policy) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || policy == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty policy";
    XLOGF(
        ERR,
        "[{}]: Failed to set route attribute policy: {}",
        kExitNullPtrLogPrefix,
        errStr);
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = errStr;
    co_return ret;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  /*
   * Log the incoming policy at the service boundary so an empty-vs-full push
   * (numStatements == 0) and its expiration window (min/max over the
   * per-statement expiration_time_s; normally uniform, so min == max) are
   * visible before the RIB evb processes it.
   */
  int64_t minExpirationTimeS = 0;
  int64_t maxExpirationTimeS = 0;
  bool anyExpiration = false;
  for (const auto& statementPair : *policy->statements()) {
    const auto& statement = statementPair.second;
    if (!statement.expiration_time_s().has_value()) {
      continue;
    }
    const int64_t expirationTimeS = *statement.expiration_time_s();
    if (!anyExpiration) {
      minExpirationTimeS = expirationTimeS;
      maxExpirationTimeS = expirationTimeS;
      anyExpiration = true;
    } else {
      if (expirationTimeS < minExpirationTimeS) {
        minExpirationTimeS = expirationTimeS;
      }
      if (expirationTimeS > maxExpirationTimeS) {
        maxExpirationTimeS = expirationTimeS;
      }
    }
  }
  XLOGF(
      INFO,
      "[CTE] co_setRouteAttributePolicy received: numStatements={}, minExpirationTimeS={}, maxExpirationTimeS={}",
      policy->statements()->size(),
      anyExpiration ? std::to_string(minExpirationTimeS) : "N/A",
      anyExpiration ? std::to_string(maxExpirationTimeS) : "N/A");

  /*
   * Enqueue directly on the calling thread; the coalescing MergeQueue is
   * thread-safe, so there is no RIB evb hop to queue behind a long walk.
   */
  co_return std::make_unique<TResult>(
      rib_.setRouteAttributePolicy(std::move(policy)));
}

folly::coro::Task<std::unique_ptr<rib_policy::TRouteAttributePolicy>>
BgpServiceDC::co_getRouteAttributePolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<rib_policy::TRouteAttributePolicy>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<rib_policy::TRouteAttributePolicy>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getRouteAttributePolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<rib_policy::TRouteAttributePolicy>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getRouteAttributePolicy timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getRouteAttributePolicy failed: {}", result.exception().what());
  }
  co_return std::make_unique<rib_policy::TRouteAttributePolicy>();
}

folly::coro::Task<void> BgpServiceDC::co_clearRouteAttributePolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  rib_.clearRouteAttributePolicy();
}

/**
 * [Path Selection Policy]
 */
folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setPathSelectionPolicy(
    std::unique_ptr<rib_policy::TPathSelectionPolicy> policy) {
  auto log = LOG_THRIFT_CALL(DBG2);

  /*
   * Validate the request before taking the lock so a null policy or shutdown
   * keeps its specific error ("Empty policy" / "Session exits") regardless of
   * FILE_MODE, and an invalid call never contends on cpsPolicyMutex_.
   */
  if (exitInitiated_ || policy == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty policy";
    XLOGF(
        ERR,
        "[{}]: Failed to set path selection policy: {}",
        kExitNullPtrLogPrefix,
        errStr);
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = errStr;
    co_return ret;
  }

  [[maybe_unused]] auto cpsLock =
      co_await cpsPolicyMutex_.co_scoped_lock_shared();
  if (dcRib_.isCpsFileModeEnabled()) {
    XLOGF(WARN, "[CPS] CPS policy is in FILE_MODE, cannot set via Thrift RPC");
    BgpStatsDC::incrCpsThriftRpcRejected();
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "CPS policy is in FILE_MODE, cannot set via Thrift RPC";
    co_return ret;
  }

  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  /*
   * Enqueue directly on the calling thread; the coalescing MergeQueue is
   * thread-safe, so there is no RIB evb hop to queue behind a long walk.
   */
  co_return std::make_unique<TResult>(
      dcRib_.setPathSelectionPolicy(std::move(policy)));
}

folly::coro::Task<std::unique_ptr<rib_policy::TPathSelectionPolicy>>
BgpServiceDC::co_getPathSelectionPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<rib_policy::TPathSelectionPolicy>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<rib_policy::TPathSelectionPolicy>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return dcRib_.getPathSelectionPolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<rib_policy::TPathSelectionPolicy>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getPathSelectionPolicy timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getPathSelectionPolicy failed: {}", result.exception().what());
  }
  co_return std::make_unique<rib_policy::TPathSelectionPolicy>();
}

folly::coro::Task<void> BgpServiceDC::co_clearPathSelectionPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);

  if (exitInitiated_) {
    co_return;
  }

  [[maybe_unused]] auto cpsLock =
      co_await cpsPolicyMutex_.co_scoped_lock_shared();
  if (dcRib_.isCpsFileModeEnabled()) {
    XLOGF(WARN, "[CPS] Cannot clear CPS policy while in FILE_MODE");
    BgpStatsDC::incrCpsThriftRpcRejected();
    co_return;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  dcRib_.clearPathSelectionPolicy();
}

folly::coro::Task<std::unique_ptr<std::vector<rib_policy::TPathSelector>>>
BgpServiceDC::co_getActivePathSelectionCriteria(
    std::unique_ptr<std::vector<std::string>> prefixes) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || prefixes == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty prefixes list";
    XLOGF(
        ERR,
        "[{}]: Failed to get path selection criteria. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    co_return std::make_unique<std::vector<rib_policy::TPathSelector>>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<std::vector<rib_policy::TPathSelector>>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this, p = std::move(prefixes)]() mutable {
        return dcRib_.getActivePathSelectionCriteria(std::move(p));
      },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<std::vector<rib_policy::TPathSelector>>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(
        ERR, "getActivePathSelectionCriteria timed out — Rib evb unresponsive");
  } else {
    XLOGF(
        ERR,
        "getActivePathSelectionCriteria failed: {}",
        result.exception().what());
  }
  co_return std::make_unique<std::vector<rib_policy::TPathSelector>>();
}

folly::coro::Task<std::unique_ptr<std::map<std::string, int>>>
BgpServiceDC::co_getGoldenPrefixSubnetCounts() {
  auto log = LOG_THRIFT_CALL(DBG2);
  auto subnetCounts = std::make_unique<std::map<std::string, int>>();
  if (auto& rfPolicy = peerMgr_.getRouteFilterPolicy(); rfPolicy) {
    if (auto goldenPolicy = rfPolicy->getGoldenPrefixPolicy(); goldenPolicy) {
      *subnetCounts = goldenPolicy->getSubnetCounts();
    }
  }
  co_return std::move(subnetCounts);
}

folly::coro::Task<void> BgpServiceDC::co_clearGoldenPrefixesPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }

  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  peerMgr_.clearGoldenPrefixesPolicy();
}

folly::coro::Task<void>
BgpServiceDC::co_clearIngressEgressRouteFiltersPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }

  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  peerMgr_.clearIngressEgressRouteFiltersPolicy();
}

folly::coro::Task<std::unique_ptr<TPartialDrainStatus>>
BgpServiceDC::co_getPartialDrainStatus() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<TPartialDrainStatus>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TPartialDrainStatus>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getPartialDrainStatus(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TPartialDrainStatus>(std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getPartialDrainStatus timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getPartialDrainStatus failed: {}", result.exception().what());
  }
  co_return std::make_unique<TPartialDrainStatus>();
}

folly::coro::Task<std::unique_ptr<TPartialDrainState>>
BgpServiceDC::co_getPartialDrainState() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<TPartialDrainState>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TPartialDrainState>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getPartialDrainState(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TPartialDrainState>(std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getPartialDrainState timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getPartialDrainState failed: {}", result.exception().what());
  }
  co_return std::make_unique<TPartialDrainState>();
}

folly::coro::Task<std::unique_ptr<std::vector<TPartiallyDrainedPrefix>>>
BgpServiceDC::co_getPartiallyDrainedPrefixes() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<std::vector<TPartiallyDrainedPrefix>>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<std::vector<TPartiallyDrainedPrefix>>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getPartiallyDrainedPrefixes(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<std::vector<TPartiallyDrainedPrefix>>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getPartiallyDrainedPrefixes timed out — Rib evb unresponsive");
  } else {
    XLOGF(
        ERR,
        "getPartiallyDrainedPrefixes failed: {}",
        result.exception().what());
  }
  co_return std::make_unique<std::vector<TPartiallyDrainedPrefix>>();
}

void BgpServiceDC::addNetwork(
    std::unique_ptr<TIpPrefix> prefix,
    std::unique_ptr<std::vector<TBgpCommunity>> communities) {
  auto log = LOG_THRIFT_CALL(INFO);
  if (exitInitiated_ || prefix == nullptr || communities == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits"
        : prefix == nullptr      ? "Empty prefix"
                                 : "Empty communities";
    XLOGF(
        ERR,
        "[{}]: Failed to add network. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    return;
  }

  if (!continueExecution(true)) {
    return;
  }

  std::map<TIpPrefix, TBgpAttributes> networks;
  TBgpAttributes attributes;
  attributes.communities() = *communities;
  networks[*prefix] = std::move(attributes);

  rib_.injectLocalRoutes(networks);
  decrRequestsInExecution();
}

void BgpServiceDC::addNetworks(
    std::unique_ptr<std::map<TIpPrefix, TBgpAttributes>> networks) {
  auto log = LOG_THRIFT_CALL(INFO);
  if (exitInitiated_ || networks == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty network map";
    XLOGF(
        ERR,
        "[{}]: Failed to add networks. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    return;
  }

  if (!continueExecution(true)) {
    return;
  }

  XLOGF(INFO, "Adding {} networks", networks->size());

  /*
   * To avoid CPU hogging on one end and avoid excessive context switches on
   * other extreme, split number of routes to be injected and add in chunks.
   */
  std::map<TIpPrefix, TBgpAttributes> chunkOfNetworks;

  for (auto iter = networks->cbegin(); iter != networks->cend(); ++iter) {
    chunkOfNetworks[iter->first] = iter->second;
    if (chunkOfNetworks.size() == kInjectRouteChunkSize) {
      rib_.injectLocalRoutes(chunkOfNetworks);
      chunkOfNetworks.clear();
    }
    if (FLAGS_thrift_route_add_delay_ms != 0) {
      // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_for)
      std::this_thread::sleep_for(
          milliseconds(FLAGS_thrift_route_add_delay_ms));
    }
  }

  if (chunkOfNetworks.size()) {
    rib_.injectLocalRoutes(chunkOfNetworks);
  }

  decrRequestsInExecution();
}

void BgpServiceDC::delNetwork(std::unique_ptr<TIpPrefix> prefix) {
  auto log = LOG_THRIFT_CALL(INFO);
  if (exitInitiated_ || prefix == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty prefix";
    XLOGF(
        ERR,
        "[{}]: Failed to delete network. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    return;
  }

  if (!continueExecution(true)) {
    return;
  }

  std::set<TIpPrefix> chunkOfPrefixes{*prefix};

  rib_.removeLocalRoutes(chunkOfPrefixes);
  decrRequestsInExecution();
}

void BgpServiceDC::delNetworks(std::unique_ptr<std::set<TIpPrefix>> prefixes) {
  auto log = LOG_THRIFT_CALL(INFO);
  if (exitInitiated_ || prefixes == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty prefixes list";
    XLOGF(
        ERR,
        "[{}]: Failed to delete networks. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    return;
  }

  if (!continueExecution(true)) {
    return;
  }

  XLOGF(INFO, "Deleting {} prefixes", prefixes->size());

  /*
   * To avoid CPU hogging on one end and avoid excessive context switches on
   * other extreme, split number of prefixes to be withdrawn and add in chunks.
   */
  std::set<TIpPrefix> chunkOfPrefixes;

  for (auto iter = prefixes->cbegin(); iter != prefixes->cend(); ++iter) {
    chunkOfPrefixes.insert(*iter);
    if (chunkOfPrefixes.size() == kInjectRouteChunkSize) {
      rib_.removeLocalRoutes(chunkOfPrefixes);
      chunkOfPrefixes.clear();
    }
    // Let the service thread sleep without overloading Rib thread
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (chunkOfPrefixes.size()) {
    rib_.removeLocalRoutes(chunkOfPrefixes);
  }

  decrRequestsInExecution();
}

/**
 * [Route Filter Policy — FILE_MODE gating]
 */
folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setRouteFilterPolicy(
    std::unique_ptr<rib_policy::TRouteFilterPolicy> policy) {
  auto crfLock = co_await crfPolicyMutex_.co_scoped_lock_shared();

  if (ribDC_.isCrfFileModeEnabled()) {
    XLOGF(WARN, "[CRF] CRF policy is in FILE_MODE, cannot set via Thrift RPC");
    BgpStatsDC::incrCrfThriftRpcRejected();
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "CRF policy is in FILE_MODE, cannot set via Thrift RPC";
    co_return ret;
  }

  // @lint-ignore CLANGTIDY facebook-thrift-handler-direct-call
  co_return co_await BgpServiceBase::co_setRouteFilterPolicy(std::move(policy));
}

folly::coro::Task<void> BgpServiceDC::co_clearRouteFilterPolicy() {
  auto crfLock = co_await crfPolicyMutex_.co_scoped_lock_shared();

  if (ribDC_.isCrfFileModeEnabled()) {
    XLOGF(WARN, "[CRF] Cannot clear CRF policy while in FILE_MODE");
    BgpStatsDC::incrCrfThriftRpcRejected();
    co_return;
  }

  // @lint-ignore CLANGTIDY facebook-thrift-handler-direct-call
  co_await BgpServiceBase::co_clearRouteFilterPolicy();
}

folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setCrfPolicyFromFile() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Session exits";
    co_return ret;
  }

  auto crfLock = co_await crfPolicyMutex_.co_scoped_lock();

  if (exitInitiated_) {
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Session exits";
    co_return ret;
  }

  /*
   * Read the artifact under the exclusive lock so the read and the RIB enqueue
   * are atomic. COOP can rewrite the artifact file at any time; because
   * forceUpdate=true bypasses the RIB version gate and the RIB policy queue
   * coalesces to the last enqueue, reading outside the lock would let an older
   * artifact read by one refresh be applied after — and so overwrite — a newer
   * one read by a concurrent refresh.
   */
  auto artifact = readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(
      FLAGS_crf_policy_file);
  if (artifact.hasError()) {
    /*
     * Only kError (file present but unreadable/corrupt) is a read failure.
     * kAbsent (no path configured or file not present) still fails the RPC
     * below, but is not counted so bgpd.crf.artifact_read.failure keeps one
     * consistent meaning fleet-wide (genuine read/parse errors only), matching
     * the startup bootstrap path in RibDC.
     */
    if (artifact.error() == ArtifactReadError::kError) {
      BgpStatsDC::incrCrfArtifactReadFailure();
    }
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Failed to read CRF policy artifact file";
    co_return ret;
  }
  BgpStatsDC::incrCrfArtifactReadSuccess();

  bool fileMode = !*artifact->dryrun();

  if (!fileMode) {
    /*
     * Transition to THRIFT_MODE. The previously-applied CRF policy (if any)
     * remains active in PeerManagerBase and RIB — only the gating flag changes.
     * Clearing the policy here would cause unnecessary traffic disruption.
     */
    ribDC_.setCrfFileModeEnabled(false);
    auto ret = std::make_unique<TResult>();
    ret->success() = true;
    ret->err() =
        "CRF artifact dryrun=true, staying in THRIFT_MODE (no policy applied)";
    co_return ret;
  }

  auto policy = std::make_unique<rib_policy::TRouteFilterPolicy>(
      std::move(*artifact->policy()));

  neteng::fboss::bgp::thrift::TResult validationResult;
  auto config = configManager_->getConfig();
  validatePeerGroupConfigInPolicy(
      validationResult, *policy, config->getPeerGroups());
  if (!*validationResult.success()) {
    XLOGF(
        ERR,
        "[CRF] Failed to validate file-based CRF policy. Error: {}",
        *validationResult.err());
    BgpStatsDC::incrCrfPolicyAppliedFailure();
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = *validationResult.err();
    co_return ret;
  }

  validateUpdateGroupRouteFilterPolicy(validationResult, *policy, *config);
  if (!*validationResult.success()) {
    XLOGF(ERR, "[CRF] {}", *validationResult.err());
    BgpStatsDC::incrCrfPolicyAppliedFailure();
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = *validationResult.err();
    co_return ret;
  }

  bool wasFileModeActive = ribDC_.isCrfFileModeEnabled();
  ribDC_.setCrfFileModeEnabled(true);
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };
  try {
    peerMgr_.setRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(*policy), /*forceUpdate=*/true);
  } catch (const BgpError& ex) {
    if (!wasFileModeActive) {
      ribDC_.setCrfFileModeEnabled(false);
    }
    BgpStatsDC::incrCrfPolicyAppliedFailure();
    auto errorMsg = folly::exceptionStr(ex);
    XLOGF(ERR, "[CRF] {}", errorMsg);
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = std::string(errorMsg);
    co_return ret;
  } catch (const std::exception& ex) {
    if (!wasFileModeActive) {
      ribDC_.setCrfFileModeEnabled(false);
    }
    BgpStatsDC::incrCrfPolicyAppliedFailure();
    auto errorMsg = folly::exceptionStr(ex);
    XLOGF(ERR, "[CRF] Unexpected error applying CRF policy: {}", errorMsg);
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = std::string(errorMsg);
    co_return ret;
  }

  /*
   * Enqueue on the calling Thrift thread: setRouteFilterPolicy pushes onto the
   * RIB's thread-safe coalescing policy queue, so there is no RIB evb hop that
   * could time out or run after this handler (and the lock) has released.
   */
  rib_.setRouteFilterPolicy(std::move(policy), /*forceUpdate=*/true);

  /*
   * policy_applied.success is emitted by RibBase::replaceRouteFilterPolicy when
   * the RIB actually applies the policy (hasUpdate), not here at enqueue time:
   * the coalescing MergeQueue can drop superseded refreshes and a no-op
   * identical policy would otherwise inflate the counter.
   */
  auto ret = std::make_unique<TResult>();
  ret->success() = true;
  ret->err() = "CRF policy applied from file (FILE_MODE)";
  co_return ret;
}

folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setCpsPolicyFromFile() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Session exits";
    co_return ret;
  }

  [[maybe_unused]] auto cpsLock = co_await cpsPolicyMutex_.co_scoped_lock();

  if (exitInitiated_) {
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Session exits";
    co_return ret;
  }

  /*
   * Read the artifact under the exclusive lock so the read and the RIB enqueue
   * are atomic. COOP can rewrite the artifact file at any time; because
   * forceUpdate=true bypasses the RIB version gate and the RIB policy queue
   * coalesces to the last enqueue, reading outside the lock would let an older
   * artifact read by one refresh be applied after — and so overwrite — a newer
   * one read by a concurrent refresh.
   */
  auto artifact = readThriftArtifactFromFile<rib_policy::CpsPolicyArtifact>(
      FLAGS_cps_policy_file);
  if (artifact.hasError()) {
    XLOGF(
        ERR,
        "[CPS] Failed to read CPS policy artifact file {} (ArtifactReadError={})",
        FLAGS_cps_policy_file,
        static_cast<int>(artifact.error()));
    /*
     * Only kError (file present but unreadable/corrupt) is a read failure.
     * kAbsent (no path configured or file not present) still fails the RPC
     * below, but is not counted so bgpd.cps.artifact_read.failure keeps one
     * consistent meaning fleet-wide (genuine read/parse errors only), matching
     * the startup bootstrap path in RibDC.
     */
    if (artifact.error() == ArtifactReadError::kError) {
      BgpStatsDC::incrCpsArtifactReadFailure();
    }
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Failed to read CPS policy artifact file";
    co_return ret;
  }
  BgpStatsDC::incrCpsArtifactReadSuccess();

  /*
   * Read the file-mode flag once under the lock; it can only change inside this
   * exclusive-locked handler, so a single read is authoritative for both the
   * dryrun no-op guard and the apply-failure rollback below.
   */
  const bool wasFileModeEnabled = dcRib_.isCpsFileModeEnabled();

  bool fileMode = !*artifact->dryrun();

  if (!fileMode) {
    /*
     * Transition to THRIFT_MODE. The previously-applied CPS policy (if any)
     * remains active in the RIB — only the gating flag changes. Clearing the
     * policy here would cause unnecessary traffic disruption. Only flip the
     * flag when it is actually set, so a steady-state dryrun refresh is a true
     * no-op.
     */
    if (wasFileModeEnabled) {
      dcRib_.setCpsFileModeEnabled(false);
    }
    auto ret = std::make_unique<TResult>();
    ret->success() = true;
    ret->err() =
        "CPS artifact dryrun=true, staying in THRIFT_MODE (no policy applied)";
    co_return ret;
  }

  auto policy = std::make_unique<rib_policy::TPathSelectionPolicy>(
      std::move(*artifact->policy()));

  /*
   * CPS lives entirely in the RIB (no PeerManager involvement) and its
   * path-selection statements key on community lists rather than peer groups,
   * so there is no peer-group validation step here — the CRF equivalent.
   */
  dcRib_.setCpsFileModeEnabled(true);
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  /*
   * Enqueue on the calling Thrift thread: setPathSelectionPolicy pushes onto
   * the RIB's thread-safe coalescing policy queue, so there is no RIB evb hop
   * that could time out or run after this handler (and the lock) has released.
   */
  auto result =
      dcRib_.setPathSelectionPolicy(std::move(policy), /*forceUpdate=*/true);

  if (!*result.success()) {
    if (!wasFileModeEnabled) {
      dcRib_.setCpsFileModeEnabled(false);
    }
    BgpStatsDC::incrCpsPolicyAppliedFailure();
    XLOGF(
        ERR,
        "[CPS] Failed to apply file-based CPS policy. Error: {}",
        *result.err());
    co_return std::make_unique<TResult>(std::move(result));
  }

  /*
   * policy_applied.success is emitted by RibDC::replacePathSelectionPolicy when
   * the RIB actually applies the policy (hasUpdate), not here at enqueue time —
   * enqueues can be coalesced away or be no-ops for an identical policy.
   */
  auto ret = std::make_unique<TResult>();
  ret->success() = true;
  ret->err() = "CPS policy applied from file (FILE_MODE)";
  co_return ret;
}

} // namespace facebook::bgp
