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

#include <folly/coro/CurrentExecutor.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/rib/FibDev.h"
#include "neteng/fboss/bgp/cpp/rib/FibEbb.h"
#include "neteng/fboss/bgp/cpp/rib/RibBB.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBB.h"

namespace facebook::bgp {

RibBB::RibBB(
    const std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>&
        localRoutes,
    const BgpGlobalConfig& globalConfig,
    const std::optional<bgp_policy::BgpPolicies>& policyConfig,
    nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
    MonitoredMPMCQueue<RibOutMessage>& ribOutQ,
    const std::string& platform,
    std::shared_ptr<NexthopCache> nexthopCache,
    uint16_t fibAgentPort,
    uint32_t fibAgentRecvTimeout)
    : RibBase(
          localRoutes,
          globalConfig,
          policyConfig,
          ribInQ,
          ribOutQ,
          platform,
          nexthopCache,
          fibAgentPort,
          fibAgentRecvTimeout) {
  /*
   * Read previous RibPolicy from disk to restore policy and trigger fib
   * programming. This must happen in the subclass constructor since
   * replaceRibPolicy() is pure virtual in RibBase.
   */
  replaceRibPolicy(readRibPolicyState(), /*isBootstrap=*/true);
}

void RibBB::createFib() {
  if (platform_ == kDevPlatform) {
    XLOG(DBG1, "Creating Fib Dev with no actual route programming");
    fib_ = FibDev::createFibDev(fromFibMessageQ_);
  } else {
    XLOG(DBG1, "Creating Fib for EBB Agent");
    fib_ = FibEbb::createFibEbb(
        &evb_,
        asyncScope_,
        fromFibMessageQ_,
        fibAgentPort_,
        fibAgentRecvTimeout_);
  }
}

/* BB processRibPolicyMsgLoop: handles CRF only. CTE/CPS messages should never
   arrive — log error and increment ODS counter if they do. */
folly::coro::Task<void> RibBB::processRibPolicyMsgLoop() noexcept {
  while (true) {
    co_await folly::coro::co_safe_point;

    auto msg = co_await co_awaitTry(ribPolicyMsgQ_.pop());
    if (!msg.hasValue()) {
      XLOG(
          INFO,
          "[Exit] Coro task cancelled. Terminating processRibPolicyMsgLoop");
      break;
    }

    folly::variant_match(
        *msg,
        [this](const RibPolicyClearMsg& /* req */) {
          handleRibPolicyClearMsg();
        },
        [this](const RouteFilterPolicySetMsg& req) {
          handleRouteFilterPolicySetMsg(req);
        },
        [this](const RouteFilterPolicyClearMsg& /* req */) {
          handleRouteFilterPolicyClearMsg();
        },
        [](const RouteAttributePolicySetMsg& /* req */) {
          XLOG(ERR, "Unexpected RouteAttributePolicySetMsg on BB platform");
          RibStatsBB::STATS_unsupportedPolicyMsg.add(1);
        },
        [](const RouteAttributePolicyClearMsg& /* req */) {
          XLOG(ERR, "Unexpected RouteAttributePolicyClearMsg on BB platform");
          RibStatsBB::STATS_unsupportedPolicyMsg.add(1);
        },
        [](const RouteAttributePolicyTimerMsg& /* req */) {
          XLOG(ERR, "Unexpected RouteAttributePolicyTimerMsg on BB platform");
          RibStatsBB::STATS_unsupportedPolicyMsg.add(1);
        },
        [](const PathSelectionPolicySetMsg& /* req */) {
          XLOG(ERR, "Unexpected PathSelectionPolicySetMsg on BB platform");
          RibStatsBB::STATS_unsupportedPolicyMsg.add(1);
        },
        [](const PathSelectionPolicyClearMsg& /* req */) {
          XLOG(ERR, "Unexpected PathSelectionPolicyClearMsg on BB platform");
          RibStatsBB::STATS_unsupportedPolicyMsg.add(1);
        });

    /*
     * Yield after each message so no input queue can dominate the RIB thread.
     */
    co_await folly::coro::co_reschedule_on_current_executor;
  }
}

void RibBB::replaceRibPolicy(
    std::unique_ptr<RibPolicy> newRibPolicy,
    bool isBootstrap) {
  std::unique_ptr<RouteFilterPolicy> newRouteFilterPolicy = nullptr;
  if (newRibPolicy && newRibPolicy->hasRouteFilterPolicy()) {
    newRouteFilterPolicy = folly::copy_to_unique_ptr(
        std::move(*newRibPolicy->getRouteFilterPolicy()));
  }

  bool hasUpdateRF =
      replaceRouteFilterPolicy(std::move(newRouteFilterPolicy), isBootstrap);

  if (isBootstrap) {
    XLOG(DBG1, "restored RibPolicy from cache (BB: CRF only)");
  } else if (hasUpdateRF) {
    XLOGF(
        DBG1,
        "Replace RibPolicy with a new one (BB: CRF only). "
        "hasUpdateRF = {}",
        hasUpdateRF);
  }
}

void RibBB::cleanupPlatform() noexcept {
  // BB has no platform-specific stop resources; nothing to tear down.
}

} // namespace facebook::bgp
