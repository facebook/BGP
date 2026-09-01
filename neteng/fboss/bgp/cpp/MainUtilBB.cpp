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

#include "neteng/fboss/bgp/cpp/MainUtilBB.h"

#include <chrono>

#include <openr/if/gen-cpp2/FibService.h>

#include "neteng/fboss/bgp/cpp/MainUtil.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/ThriftClientUtils.h"

namespace facebook::bgp {
namespace {

using FibServiceClient = apache::thrift::Client<openr::thrift::FibService>;

bool isFibServiceConfigured(
    folly::EventBase& evb,
    int32_t port,
    std::chrono::milliseconds connectTimeout,
    std::chrono::milliseconds sendTimeout,
    std::chrono::milliseconds recvTimeout) {
  auto client = createThriftClient<FibServiceClient>(
      evb, kLoopBackAddressV6, port, connectTimeout, sendTimeout, recvTimeout);
  return client->sync_getSwitchRunState() ==
      openr::thrift::SwitchRunState::CONFIGURED;
}

} // namespace

bool waitForFibService(
    const folly::EventBase& signalHandlerEvb,
    int32_t fibEbbPort,
    int32_t recvTimeoutMs,
    int32_t openrPort) {
  folly::EventBase evb;
  const auto readyCallback = [&]() {
    const bool fibEbbReady = isFibServiceConfigured(
        evb,
        fibEbbPort,
        kFibEbbConnTimeout,
        kFibEbbSendTimeout,
        std::chrono::milliseconds(recvTimeoutMs));
    const bool openrReady = isFibServiceConfigured(
        evb,
        openrPort,
        kFibOpenrConnTimeout,
        kFibOpenrSendTimeout,
        kFibOpenrRecvTimeout);
    return fibEbbReady && openrReady;
  };
  return detail::waitUntilFibServiceReady(
      signalHandlerEvb, "FibEbbService", readyCallback);
}

} // namespace facebook::bgp
