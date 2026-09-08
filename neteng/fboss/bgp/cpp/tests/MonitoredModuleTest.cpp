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

#define MonitoredModule_TEST_FRIENDS                         \
  FRIEND_TEST(MonitoredModuleTest, MonitorModuleTest);       \
  FRIEND_TEST(MonitoredModuleTest, MonitorQueueTest);        \
  FRIEND_TEST(MonitoredModuleTest, MonitorQueueReplaceTest); \
  FRIEND_TEST(MonitoredModuleTest, StopMonitoringTest);      \
  FRIEND_TEST(MonitoredModuleTest, TwoModulesMonitorQueueTest);

#include <barrier>

#include "neteng/fboss/bgp/cpp/lib/fibers/Queue.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredModule.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"

using namespace ::testing;

namespace facebook::bgp {

// helper macros
#define GET_MODULE_POINTER(module, name)              \
  &std::get<std::reference_wrapper<MonitoredModule>>( \
       module.monitoredItems_.rlock()->at(name))      \
       .get()

#define GET_QUEUE_POINTER(module, name)                  \
  &std::get<std::reference_wrapper<MonitoredQueueBase>>( \
       module.monitoredItems_.rlock()->at(name))         \
       .get()

/*
 * Ensure that MonitoredModule would be added to MonitoredModule
 * Also test the error message when the module name is already monitored
 */
TEST(MonitoredModuleTest, MonitorModuleTest) {
  MonitoredModule module, testModule;

  auto& messages = subscribeToLogMessages("");

  {
    messages.clear();

    module.monitorModule("test", testModule);

    EXPECT_EQ(GET_MODULE_POINTER(module, "test"), &testModule);
    EXPECT_EQ(0, messages.size());
  }

  // testModule is already monitored, the name does not matter
  {
    messages.clear();

    module.monitorModule("test-other-name", testModule);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(),
        "Module test-other-name is already monitored.");

    // not changed
    EXPECT_EQ(GET_MODULE_POINTER(module, "test"), &testModule);
  }

  // "test" is already monitored
  {
    messages.clear();

    MonitoredModule otherModule;

    module.monitorModule("test", otherModule);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(), "Module test is already monitored.");

    // not changed
    EXPECT_EQ(GET_MODULE_POINTER(module, "test"), &testModule);
  }

  // different name "test-other" could then be monitored
  {
    messages.clear();

    MonitoredModule otherModule;

    module.monitorModule("test-other", otherModule);

    EXPECT_EQ(GET_MODULE_POINTER(module, "test-other"), &otherModule);
    EXPECT_EQ(0, messages.size());
  }
}

/*
 * Ensure that MonitoredQueue would be added to MonitoredModule
 * Also test the error message when the queue name is already monitored
 */
TEST(MonitoredModuleTest, MonitorQueueTest) {
  MonitoredModule module;
  MonitoredMPMCQueue<int> testQueue;

  auto& messages = subscribeToLogMessages("");

  {
    messages.clear();

    module.monitorQueue(
        "test", testQueue, MonitorableQueueTrace::Direction::IN);

    EXPECT_EQ(GET_QUEUE_POINTER(module, "test"), &testQueue);
    EXPECT_EQ(0, messages.size());
  }

  // testQueue is already monitored, the name does not matter
  {
    messages.clear();

    module.monitorQueue(
        "test-other-name", testQueue, MonitorableQueueTrace::Direction::IN);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(),
        "Queue test-other-name is already monitored.");

    // not changed
    EXPECT_EQ(GET_QUEUE_POINTER(module, "test"), &testQueue);
  }

  // "test" is already monitored
  {
    messages.clear();

    MonitoredQueue<nettools::bgplib::RWQueue<int>> otherQueue;

    module.monitorQueue(
        "test", otherQueue, MonitorableQueueTrace::Direction::IN);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(), "Queue test is already monitored.");

    // not changed
    EXPECT_EQ(GET_QUEUE_POINTER(module, "test"), &testQueue);
  }

  // different name "test-other" could then be monitored
  {
    messages.clear();

    MonitoredQueue<nettools::bgplib::RWQueue<int>> otherQueue;

    module.monitorQueue(
        "test-other", otherQueue, MonitorableQueueTrace::Direction::IN);

    EXPECT_EQ(GET_QUEUE_POINTER(module, "test-other"), &otherQueue);
    EXPECT_EQ(0, messages.size());
  }
}

// Ensure that a queue could be replaced by another queue
TEST(MonitoredModuleTest, MonitorQueueReplaceTest) {
  MonitoredModule module;
  MonitoredMPMCQueue<int> testQueue1, testQueue2;

  auto& messages = subscribeToLogMessages("");

  // monitor testQueue1 by "test"
  {
    messages.clear();

    module.monitorQueue(
        "test", testQueue1, MonitorableQueueTrace::Direction::IN);

    EXPECT_EQ(GET_QUEUE_POINTER(module, "test"), &testQueue1);
    EXPECT_EQ(0, messages.size());
  }

  // replace "test" to monitor testQueue2
  {
    messages.clear();

    module.monitorQueue(
        "test", testQueue2, MonitorableQueueTrace::Direction::IN, true);

    EXPECT_EQ(GET_QUEUE_POINTER(module, "test"), &testQueue2);
    EXPECT_EQ(0, messages.size());
  }

  // now, testQueue1 IN is not reset, so we cannot use it to replace testQueue2
  {
    messages.clear();

    module.monitorQueue(
        "test", testQueue1, MonitorableQueueTrace::Direction::IN, true);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(), "Queue test is already monitored.");
  }
}

/*
 * Check that a queue could be monitored in two directions by two
 * modules
 */
TEST(MonitoredModuleTest, TwoModulesMonitorQueueTest) {
  MonitoredModule module1, module2;
  MonitoredMPMCQueue<int> testQueue;

  auto& messages = subscribeToLogMessages("");

  // module1 monitors testQueue::IN
  {
    messages.clear();

    module1.monitorQueue(
        "test", testQueue, MonitorableQueueTrace::Direction::IN);

    EXPECT_EQ(GET_QUEUE_POINTER(module1, "test"), &testQueue);
    EXPECT_EQ(0, messages.size());
  }

  // The same direction cannot be monitored twice by two modules
  {
    messages.clear();

    module2.monitorQueue(
        "test", testQueue, MonitorableQueueTrace::Direction::IN);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(), "Queue test is already monitored.");
  }

  // module2 could still monitor testQueue::OUT
  {
    messages.clear();

    module2.monitorQueue(
        "test", testQueue, MonitorableQueueTrace::Direction::OUT);

    EXPECT_EQ(GET_QUEUE_POINTER(module2, "test"), &testQueue);
    EXPECT_EQ(0, messages.size());
  }
}

/*
 * Ensure that the same name could be only used to a monitored item, be it a
 * module or a queue
 */
TEST(MonitoredModuleTest, MonitoredNameTest) {
  MonitoredModule module, testModule;
  MonitoredMPMCQueue<int> testQueue;

  module.monitorModule("test-module", testModule);
  module.monitorQueue(
      "test-queue", testQueue, MonitorableQueueTrace::Direction::IN);

  auto& messages = subscribeToLogMessages("");

  // "test-module" is already monitored as a module
  {
    messages.clear();

    MonitoredMPMCQueue<int> otherTestQueue;

    module.monitorQueue(
        "test-module", otherTestQueue, MonitorableQueueTrace::Direction::IN);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(),
        "Queue test-module is already monitored.");
  }

  // "test-queue" is already monitored as a queue
  {
    messages.clear();

    MonitoredModule otherTestModule;

    module.monitorModule("test-queue", otherTestModule);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(),
        "Module test-queue is already monitored.");
  }
}

/*
 * Test the function getQueueSizes with 2 scenarios:
 *   1. Query all monitored queues
 *   2. Query a specfic monitored queue
 */
TEST(MonitoredModuleTest, GetQueueSizesTest) {
  MonitoredModule module;
  MonitoredMPMCQueue<int> testQueue1;
  MonitoredQueue<nettools::bgplib::RWQueue<int>> testQueue2;

  module.monitorQueue(
      "queue1", testQueue1, MonitorableQueueTrace::Direction::IN);
  module.monitorQueue(
      "queue2", testQueue2, MonitorableQueueTrace::Direction::IN);

  // Different queue types increment in different ways
  testQueue1.push(1);
  testQueue1.push(1);
  testQueue2.put(1);

  // Query all
  {
    QueryNode queryNode;
    queryNode.isLeaf = true;

    folly::F14FastMap<std::string, int> expectedResults = {
        {"queue1", 2}, {"queue2", 1}};

    EXPECT_EQ(module.getQueueSizes(&queryNode), expectedResults);
  }

  // Query queue1
  {
    QueryNode queryNode;
    queryNode.isLeaf = false;
    queryNode.children.emplace("queue1", std::make_unique<QueryNode>());

    folly::F14FastMap<std::string, int> expectedResults = {{"queue1", 2}};

    EXPECT_EQ(module.getQueueSizes(&queryNode), expectedResults);
  }
}

/*
 * Test the function stopMonitoring
 */
TEST(MonitoredModuleTest, StopMonitoringTest) {
  MonitoredModule module;
  MonitoredMPMCQueue<int> testQueue;

  // monitor testQueue1 by "test"
  module.monitorQueue("test", testQueue, MonitorableQueueTrace::Direction::IN);

  EXPECT_EQ(GET_QUEUE_POINTER(module, "test"), &testQueue);

  module.stopMonitoring("test");

  // removed from monitoredItems_
  EXPECT_EQ(
      module.monitoredItems_.rlock()->find("test"),
      module.monitoredItems_.rlock()->end());
}

/*
 * Test the function getQueueSizes with nested monitored modules
 *   module2 -> module1 -> queue1 (size 2), queue2 (size 1)
 *           -> queue3 (size 3)
 */
TEST(MonitoredModuleTest, GetQueueSizesNestedModuleTest) {
  MonitoredModule module1, module2;
  MonitoredMPMCQueue<int> testQueue1;
  MonitoredQueue<nettools::bgplib::RWQueue<int>> testQueue2, testQueue3;

  module1.monitorQueue(
      "queue1", testQueue1, MonitorableQueueTrace::Direction::IN);
  module1.monitorQueue(
      "queue2", testQueue2, MonitorableQueueTrace::Direction::IN);

  // Different queue types increment in different ways
  testQueue1.push(1);
  testQueue1.push(1);
  testQueue2.put(1);

  module2.monitorModule("module1", module1);
  module2.monitorQueue(
      "queue3", testQueue3, MonitorableQueueTrace::Direction::OUT);

  testQueue3.put(1);
  testQueue3.put(2);
  testQueue3.put(3);

  // Query all
  QueryNode queryNode;
  queryNode.isLeaf = true;

  folly::F14FastMap<std::string, int> expectedResults = {
      {"module1.queue1", 2}, {"module1.queue2", 1}, {"queue3", 3}};

  EXPECT_EQ(module2.getQueueSizes(&queryNode), expectedResults);
}

/*
 * Two threads try to update and query the monitored items at the same time
 * They shouldn't crash
 */
TEST(MonitoredModuleTest, InterThreadMonitoringTest) {
  MonitoredModule module;

  int totalRuns = 100;
  std::barrier barrier(2);

  auto thread1 = std::thread([&]() {
    for (int i = 0; i < totalRuns; ++i) {
      MonitoredModule subModule;

      MonitoredMPMCQueue<int> testQueue;

      subModule.monitorQueue(
          "queue", testQueue, MonitorableQueueTrace::Direction::IN);

      module.monitorModule(fmt::format("test{}", i), subModule);

      barrier.arrive_and_wait(); // Wait for the other thread to arrive

      module.stopMonitoring(fmt::format("test{}", i));
    }
  });

  auto thread2 = std::thread([&]() {
    // query all
    QueryTree queryTree;
    queryTree.root.markLeaf();

    for (int i = 0; i < totalRuns; ++i) {
      barrier.arrive_and_wait(); // Wait for the other thread to arrive

      auto queueSizes = module.getQueueSizes(&queryTree.root);
    }
  });

  thread1.join();
  thread2.join();
}

} // namespace facebook::bgp
