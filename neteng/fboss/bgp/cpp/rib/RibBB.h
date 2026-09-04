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

#include "neteng/fboss/bgp/cpp/common/platform/bb/PlatformConstant.h"
#include "neteng/fboss/bgp/cpp/rib/RibBase.h"

namespace facebook::bgp {

class RibBB : public RibBase {
 public:
  RibBB(
      const std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>&
          localRoutes,
      const BgpGlobalConfig& globalConfig,
      const std::optional<bgp_policy::BgpPolicies>& policyConfig,
      nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
      MonitoredMPMCQueue<RibOutMessage>& ribOutQ,
      const std::string& platform,
      std::shared_ptr<NexthopCache> nextHopCache = nullptr,
      uint16_t fibAgentPort = kPlatformFibAgentPort,
      uint32_t fibAgentRecvTimeout = kPlatformFibAgentRecvTimeout);
  ~RibBB() override = default;

 protected:
  void createFib() override;

  /*
   * [Exit] BB has no platform-specific stop resources; this is a no-op. See
   * RibBase::cleanupPlatform().
   */
  void cleanupPlatform() noexcept override;
  folly::coro::Task<void> processRibPolicyMsgLoop() noexcept override;
  void replaceRibPolicy(
      std::unique_ptr<RibPolicy> newRibPolicy,
      bool isBootstrap = false) override;
};

} // namespace facebook::bgp
