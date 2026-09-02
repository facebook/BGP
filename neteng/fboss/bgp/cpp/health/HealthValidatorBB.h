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

#include "neteng/fboss/bgp/cpp/health/HealthValidator.h"

namespace facebook::bgp {

class NetlinkWrapper;

/**
 * BB-specific HealthValidator. Owns a NetlinkWrapper pointer and
 * overrides checkNetlinkWrapper() to run the real kernel-FIB-side
 * check (NETLINK_TRACKED_INTERFACES). All other health checks are
 * inherited from the base.
 *
 * Splitting this out of HealthValidator keeps the base free of any
 * NetlinkWrapper compile-time dependency so DC and OSS binaries do
 * not link //neteng/fboss/bgp/cpp/nexthopTracker:netlink_wrapper.
 */
class HealthValidatorBB : public HealthValidator {
 public:
  HealthValidatorBB(
      PeerManagerBase* peerMgr,
      RibBase* rib,
      Watchdog* watchdog,
      NetlinkWrapper* nlWrapper,
      NexthopHandler* nexthopHandler = nullptr,
      std::shared_ptr<ConfigManager> configManager = nullptr);

  ~HealthValidatorBB() override = default;

 protected:
  TModuleHealthReport checkNetlinkWrapper() override;
  THealthCheckResult checkPlannedExit() override;

 private:
  NetlinkWrapper* nlWrapper_;
};

} // namespace facebook::bgp
