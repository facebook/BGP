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

#include <folly/coro/AsyncGenerator.h>
#include <folly/coro/Task.h>
#include <openr/if/gen-cpp2/FibService.h>
#include <thrift/lib/cpp2/async/ClientBufferedStream.h>

#include "neteng/fboss/bgp/cpp/common/BgpModuleBase.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopCache.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"

namespace facebook::bgp {

/**
 * NexthopHandler is responsible for:
 * 1. Creating and maintaining a connection to the OpenR FIB agent
 * 2. Handling stream response of nexthop status updates from the FIB agent and
 *    updating the NexthopCache with the latest nexthop status information
 * 3. Notifying Rib with updated nextHops to perform bestpath calculation
 */
class NexthopHandler : public BgpModuleBase {
 public:
  /**
   * @brief Constructor
   * @param nexthopCache Shared pointer to the NexthopCache to update
   * @param port Port number to connect to OpenR FIB agent
   */
  explicit NexthopHandler(
      std::shared_ptr<NexthopCache> nexthopCache,
      nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
      const int32_t openrFibAgentPort)
      : BgpModuleBase(kModuleNexthopHandler),
        nexthopCache_(std::move(nexthopCache)),
        ribInQ_(ribInQ),
        openrFibAgentPort_(openrFibAgentPort) {}

  /**
   * @brief Routine to continuously handle stream responses from client_'s
   * co_streamNextHopStatus call
   */
  folly::coro::Task<void> handleNextHopStatusStreamRoutine();

  void run() noexcept override;
  void stop() noexcept override;

 private:
  /**
   * @brief Process stream updates from the AsyncGenerator
   * @param asyncGen The AsyncGenerator containing stream updates
   */
  folly::coro::Task<void> handleStream(
      folly::coro::AsyncGenerator<openr::thrift::StreamNextHopStatusResponse&&>
          asyncGen);

  /**
   * @brief Process nexthop status updates from the OpenR FIB agent
   * @param response The response containing nexthop status updates
   */
  folly::coro::Task<void> co_processUpdates(
      const openr::thrift::StreamNextHopStatusResponse& response);

  /**
   * @brief Method to create a thrift client and connect to OpenR FIB agent
   */
  void connectOpenrFibAgent();

  /**
   * @brief Method to disconnect OpenR FIB agent
   */
  void disconnectOpenrFibAgent();

  /**
   * Update nexthop cache and push changed statuses to ribInQ.
   */
  folly::coro::Task<void> co_updateCacheAndNotifyRib(
      const std::vector<NexthopStatus>& updates);

  // Shared pointer to the NexthopCache to update
  std::shared_ptr<NexthopCache> nexthopCache_{nullptr};

  // Reference to the RibInMessage queue to send nexthop updates
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ_;

  // OpenR FIB agent client
  std::unique_ptr<apache::thrift::Client<openr::thrift::FibService>> client_;

  // Port number to connect to OpenR FIB agent
  const int32_t openrFibAgentPort_;

  friend class NexthopHandlerTestFixture;

/*
 * per class placeholder for test code injection
 * only need to be setup once here
 */
#ifdef NexthopHandler_TEST_FRIENDS
  NexthopHandler_TEST_FRIENDS
#endif
};

} // namespace facebook::bgp
