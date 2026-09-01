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

#include "neteng/fboss/bgp/cpp/MainUtilDC.h"

#include <chrono>

#include <fboss/agent/if/gen-cpp2/ctrl_clients.h>
#include <fboss/agent/if/gen-cpp2/ctrl_types.h>

#include "neteng/fboss/bgp/cpp/MainUtil.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/ThriftClientUtils.h"

namespace facebook::bgp {

bool waitForFibService(
    const folly::EventBase& signalHandlerEvb,
    int32_t agentThriftPort,
    int32_t agentThriftRecvTimeoutMs) {
  folly::EventBase evb;
  const auto readyCallback = [&]() {
    auto client =
        createThriftClient<apache::thrift::Client<facebook::fboss::FbossCtrl>>(
            evb,
            kLoopBackAddressV6,
            agentThriftPort,
            kFbossAgentConnTimeout,
            kFbossAgentSendTimeout,
            std::chrono::milliseconds(agentThriftRecvTimeoutMs));
    return client->sync_getSwitchRunState() ==
        facebook::fboss::SwitchRunState::CONFIGURED;
  };
  return detail::waitUntilFibServiceReady(
      signalHandlerEvb, "FibService", readyCallback);
}

} // namespace facebook::bgp
