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

#include <folly/container/F14Map.h>
#include <functional>

#include "neteng/fboss/bgp/cpp/watchdog/MonitorableTrace.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"
#include "neteng/fboss/bgp/cpp/watchdog/QueryTree.h"

namespace facebook::bgp {

class MonitoredModule : public MonitorableTrace {
 public:
  MonitoredModule() = default;

  /*
   * Monitor a module with the name moduleName. Log an
   * error message if the moduleName or the module has been monitored.
   */
  void monitorModule(
      const std::string& moduleName,
      MonitoredModule& module) noexcept;

  /*
   * Monitor a queue with the name queueName. If replace is true, replace the
   * existing queue (if there is one) with the same name. Otherwise, log an
   * error message if the queueName or the queue has been monitored.
   *
   * Notice that we don't reset the replaced queue back to monitorable again.
   * We assume the replaced queue is no longer needed.
   */
  void monitorQueue(
      const std::string& queueName,
      MonitoredQueueBase& queue,
      MonitorableQueueTrace::Direction direction,
      bool replace = false);

  /*
   * Stop monitoring the item, i.e., remove itemName from monitoredItems_
   */
  void stopMonitoring(const std::string& itemName) noexcept;

  /**
   * Get the queue sizes based on the queryNode
   *
   * @param queryNode represents the queried entities, could be a module or a
   * queue
   * @return The map of queue sizes where the key is the queue path and
   * the value is the size of the queue.
   */
  folly::F14FastMap<std::string, int> getQueueSizes(
      const QueryNode* queryNode) noexcept;

 protected:
  folly::Synchronized<folly::F14NodeMap<
      std::string,
      std::variant<
          std::reference_wrapper<MonitoredModule>,
          std::reference_wrapper<MonitoredQueueBase>>>>
      monitoredItems_;

/*
 * per class placeholder for test code injection
 * only need to be setup once here
 */
#ifdef MonitoredModule_TEST_FRIENDS
  MonitoredModule_TEST_FRIENDS
#endif
};

} // namespace facebook::bgp
