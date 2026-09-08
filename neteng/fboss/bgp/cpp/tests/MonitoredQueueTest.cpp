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

#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"

using namespace ::testing;

namespace facebook::bgp {

TEST(MonitoredQueueTest, MPMCQueueSizeTest) {
  MonitoredMPMCQueue<int> testQueue;

  testQueue.push(1);
  testQueue.push(1);

  EXPECT_EQ(testQueue.size(), 2);
}

TEST(MonitoredQueueTest, RQueueSizeTest) {
  auto queue = std::make_shared<nettools::bgplib::detail::Queue<int>>(0);
  MonitoredRQueue<int> testQueue(queue);

  testQueue.putNullFront();
  testQueue.putNullFront();

  EXPECT_EQ(testQueue.size(), 2);
}

TEST(MonitoredQueueTest, WQueueSizeTest) {
  auto queue = std::make_shared<nettools::bgplib::detail::Queue<int>>(0);
  MonitoredWQueue<int> testQueue(queue);

  testQueue.putNull();
  testQueue.putNull();

  EXPECT_EQ(testQueue.size(), 2);
}

TEST(MonitoredQueueTest, RWQueueSizeTest) {
  MonitoredRWQueue<int> testQueue;

  testQueue.put(1);
  testQueue.put(1);

  EXPECT_EQ(testQueue.size(), 2);
}

/*
 * Test that MonitoredQueue would call the explicit constructor
 * of its parent queue class
 */
TEST(MonitoredQueueTest, ExplicitConstructorTest) {
  class MockQueue {
   public:
    explicit MockQueue(int num) : _num(num) {}

    int size() noexcept {
      return 0;
    }

    int _num{0};
  };

  MonitoredQueue<MockQueue> testQueue(12);

  EXPECT_EQ(testQueue._num, 12);
  EXPECT_EQ(testQueue.size(), 0);
}

/*
 * Test that MonitoredQueue copy assignment works when the underlying class
 * has a copy assignment operator
 */
TEST(MonitoredQueueTest, CopyAssignmentWithAssignableClassTest) {
  class AssignableMockQueue {
   public:
    explicit AssignableMockQueue(int num) : _num(num) {}

    AssignableMockQueue(const AssignableMockQueue& other) : _num(other._num) {}
    AssignableMockQueue& operator=(const AssignableMockQueue& other) noexcept {
      _num = other._num;
      return *this;
    }

    int size() noexcept {
      return _num;
    }

    int _num{0};
  };

  MonitoredQueue<AssignableMockQueue> queue1(10);
  MonitoredQueue<AssignableMockQueue> queue2(20);

  EXPECT_EQ(queue1.size(), 10);
  EXPECT_EQ(queue2.size(), 20);

  // Copy assign queue2 to queue1
  queue1 = queue2;

  EXPECT_EQ(queue1.size(), 20);
  // Verify queue2 is unchanged after copy
  EXPECT_EQ(queue2.size(), 20);
}

/*
 * Test that MonitoredQueue compiles when the underlying class does not have
 * an assignment operator (SFINAE disables the copy assignment operator)
 */
TEST(MonitoredQueueTest, NoAssignmentOperatorClassTest) {
  class NonAssignableMockQueue {
   public:
    explicit NonAssignableMockQueue(int num) : _num(num) {}

    // Explicitly delete copy constructor and assignment operators
    NonAssignableMockQueue(const NonAssignableMockQueue&) = delete;
    NonAssignableMockQueue& operator=(const NonAssignableMockQueue&) = delete;

    int size() noexcept {
      return _num;
    }

    int _num{0};
  };

  // Verify that std::is_assignable is false for NonAssignableMockQueue
  static_assert(
      !std::is_assignable<
          NonAssignableMockQueue&,
          const NonAssignableMockQueue&>::value,
      "NonAssignableMockQueue should not be copy assignable");

  /*
   * Verify that std::is_assignable is also false for
   * MonitoredQueue<NonAssignableMockQueue>
   */
  static_assert(
      !std::is_assignable<
          MonitoredQueue<NonAssignableMockQueue>&,
          const MonitoredQueue<NonAssignableMockQueue>&>::value,
      "MonitoredQueue<NonAssignableMockQueue> should not be copy assignable");

  // The code should compile and create the queue successfully
  MonitoredQueue<NonAssignableMockQueue> testQueue(42);
  EXPECT_EQ(testQueue.size(), 42);
}

} // namespace facebook::bgp
