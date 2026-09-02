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

#include "neteng/fboss/bgp/cpp/health/HealthValidatorBB.h"

#include <fmt/format.h>

#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NetlinkWrapper.h"

namespace facebook::bgp {

using neteng::fboss::bgp::thrift::HealthCheckCategory;
using neteng::fboss::bgp::thrift::HealthCheckId;
using neteng::fboss::bgp::thrift::HealthCheckStatus;
using neteng::fboss::bgp::thrift::THealthCheckResult;
using neteng::fboss::bgp::thrift::TModuleHealthReport;

HealthValidatorBB::HealthValidatorBB(
    PeerManagerBase* peerMgr,
    RibBase* rib,
    Watchdog* watchdog,
    NetlinkWrapper* nlWrapper,
    NexthopHandler* nexthopHandler,
    std::shared_ptr<ConfigManager> configManager)
    : HealthValidator(
          peerMgr,
          rib,
          watchdog,
          nexthopHandler,
          std::move(configManager)),
      nlWrapper_(nlWrapper) {}

THealthCheckResult HealthValidatorBB::checkPlannedExit() {
  return makeResult(
      HealthCheckId::GLOBAL_SYSTEM_PLANNED_EXIT,
      HealthCheckCategory::GLOBAL_SYSTEM,
      HealthCheckStatus::SKIPPED,
      "Not applicable in EBB context");
}

TModuleHealthReport HealthValidatorBB::checkNetlinkWrapper() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::NETLINK_WRAPPER;
  auto& checks = *report.checks();

  /* 1.7.1 Tracked interfaces > 0
   * Use config (enable_next_hop_tracking) as SoT for whether
   * NetlinkWrapper should be active, not the nlWrapper_ pointer. */
  bool nhtEnabled = false;
  if (configManager_) {
    auto config = configManager_->getConfig();
    nhtEnabled = config && config->getBgpGlobalConfig() &&
        config->getBgpGlobalConfig()->enableNextHopTracking;
  }

  if (!nhtEnabled) {
    checks.emplace_back(makeResult(
        HealthCheckId::NETLINK_TRACKED_INTERFACES,
        HealthCheckCategory::NETLINK_WRAPPER,
        HealthCheckStatus::SKIPPED,
        "NetlinkWrapper not enabled in config"));
  } else if (!nlWrapper_) {
    checks.emplace_back(makeResult(
        HealthCheckId::NETLINK_TRACKED_INTERFACES,
        HealthCheckCategory::NETLINK_WRAPPER,
        HealthCheckStatus::FAIL,
        "Config enables NHT but NetlinkWrapper is null"));
  } else {
    neteng::fboss::bgp::thrift::TEntryStats stats;
    nlWrapper_->updateEntryStats(stats);
    int64_t interfaces = *stats.total_netlink_wrapper_interfaces();
    bool passed = (interfaces > 0);
    checks.emplace_back(makeResult(
        HealthCheckId::NETLINK_TRACKED_INTERFACES,
        HealthCheckCategory::NETLINK_WRAPPER,
        passed ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
        fmt::format("trackedInterfaces = {}", interfaces),
        static_cast<double>(interfaces),
        1.0));
  }

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

} // namespace facebook::bgp
