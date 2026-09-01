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

#include <gtest/gtest.h>

#include <folly/CancellationToken.h>
#include <folly/IPAddress.h>
#include <folly/coro/Task.h>
#include <folly/executors/ManualExecutor.h>

#include "neteng/fboss/bgp/cpp/rib/RibBB.h"

namespace facebook::bgp {

class QueueFairnessRibBB final : public RibBB {
 public:
  QueueFairnessRibBB(
      const BgpGlobalConfig& bgpGlobalConfig,
      nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
      MonitoredMPMCQueue<RibOutMessage>& ribOutQ)
      : RibBB(
            {},
            bgpGlobalConfig,
            std::nullopt,
            ribInQ,
            ribOutQ,
            "dev",
            nullptr) {}

  void fillPolicyQueue() {
    enqueueRibPolicyMsg(RibPolicyClearMsg{});
    enqueueRibPolicyMsg(RouteFilterPolicyClearMsg{});
  }

  folly::coro::Task<void> processPolicyQueue() noexcept {
    co_await RibBB::processRibPolicyMsgLoop();
  }

  size_t policyQueueSize() const noexcept {
    return ribPolicyMsgQ_.size();
  }
};

TEST(RibBBInputQueueFairnessTest, PolicyQueueYieldsAfterEachMessage) {
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ{1};
  MonitoredMPMCQueue<RibOutMessage> ribOutQ;
  BgpGlobalConfig bgpGlobalConfig(
      65000,
      folly::IPAddress("10.0.0.1"),
      folly::IPAddress("10.0.0.2"),
      std::chrono::seconds(30),
      std::nullopt,
      std::chrono::seconds(120),
      {},
      {});
  QueueFairnessRibBB rib{bgpGlobalConfig, ribInQ, ribOutQ};
  rib.fillPolicyQueue();

  folly::ManualExecutor executor;
  folly::CancellationSource cancellationSource;
  bool consumerCompleted{false};
  bool consumerCompletedCleanly{false};
  auto consumer =
      folly::coro::co_withExecutor(&executor, rib.processPolicyQueue());
  std::move(consumer).start(
      [&](auto&& result) {
        consumerCompleted = true;
        consumerCompletedCleanly = result.hasValue() ||
            result.template hasException<folly::OperationCancelled>();
      },
      cancellationSource.getToken());

  executor.run();
  EXPECT_EQ(1, rib.policyQueueSize());
  EXPECT_FALSE(consumerCompleted);

  cancellationSource.requestCancellation();
  executor.drain();
  EXPECT_TRUE(consumerCompleted);
  EXPECT_TRUE(consumerCompletedCleanly);
}

} // namespace facebook::bgp
