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

#include <folly/Overload.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/watchdog/MonitoredModule.h"

namespace facebook::bgp {

/*
 * Monitor a module with the name moduleName. Log an
 * error message if the moduleName or the module has been monitored.
 */
void MonitoredModule::monitorModule(
    const std::string& moduleName,
    MonitoredModule& module) noexcept {
  monitoredItems_.withWLock([&](auto& monitoredItems) {
    if (!module.isMonitored() &&
        monitoredItems.find(moduleName) == monitoredItems.end()) {
      monitoredItems.emplace(moduleName, std::ref(module));
      module.markMonitored();
    } else {
      XLOGF(INFO, "Module {} is already monitored.", moduleName);
    }
  });
}

/*
 * Monitor a queue with the name queueName. If replace is true, replace the
 * existing queue (if there is one) with the same name. Otherwise, log an
 * error message if the queueName or the queue has been monitored.
 *
 * Notice that we don't reset the replaced queue back to monitorable again.
 * We assume the replaced queue is no longer needed.
 */
void MonitoredModule::monitorQueue(
    const std::string& queueName,
    MonitoredQueueBase& queue,
    MonitorableQueueTrace::Direction direction,
    bool replace) {
  monitoredItems_.withWLock([&](auto& monitoredItems) {
    if (!queue.isMonitored(direction) &&
        (monitoredItems.find(queueName) == monitoredItems.end() || replace)) {
      monitoredItems.insert_or_assign(queueName, std::ref(queue));
      queue.markMonitored(direction);
    } else {
      XLOGF(INFO, "Queue {} is already monitored.", queueName);
    }
  });
}

/*
 * Stop monitoring the item, i.e., remove itemName from monitoredItems_
 */
void MonitoredModule::stopMonitoring(const std::string& itemName) noexcept {
  monitoredItems_.wlock()->erase(itemName);
}

/**
 * Get the queue sizes based on the queryNode
 *
 * @param queryNode represents the queried entities, could be a module or a
 * queue
 * @return The map of queue sizes where the key is the queue path and
 * the value is the size of the queue.
 */
folly::F14FastMap<std::string, int> MonitoredModule::getQueueSizes(
    const QueryNode* queryNode) noexcept {
  folly::F14FastMap<std::string, int> queueSizes;

  monitoredItems_.withRLock([&](const auto& monitoredItems) {
    for (const auto& [itemName, item] : monitoredItems) {
      /*
       * according to whether the item is a module or a queue, populate
       * queueSizes
       */
      folly::variant_match(
          item,
          [queryNode, moduleName = itemName, &queueSizes](
              const std::reference_wrapper<MonitoredModule>& module) {
            auto modulePreifx = moduleName + ".";

            auto querySubNode = queryNode;
            if (!querySubNode->isLeaf) {
              if (!querySubNode->children.contains(moduleName)) {
                // skip if module is not in query tree
                return;
              }
              querySubNode = querySubNode->children.at(moduleName).get();
            }

            auto moduleQueueSizes = module.get().getQueueSizes(querySubNode);
            for (const auto& [queueName, queueSize] : moduleQueueSizes) {
              queueSizes.emplace(modulePreifx + queueName, queueSize);
            }
          },
          [queryNode, queueName = itemName, &queueSizes](
              const std::reference_wrapper<MonitoredQueueBase>& queue) {
            if (!queryNode->isLeaf &&
                !queryNode->children.contains(queueName)) {
              return;
            }
            queueSizes.emplace(queueName, queue.get().size());
          });
    }
  });

  return queueSizes;
}

} // namespace facebook::bgp
