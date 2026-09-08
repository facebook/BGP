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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define BgpModuleBase_TEST_FRIENDS                           \
  friend class DerivedBgpModule;                             \
  FRIEND_TEST(BgpModuleBaseTest, HeartbeatLoopTest);         \
  FRIEND_TEST(BgpModuleBaseTest, ScheduleHeartbeatLoopTest); \
  FRIEND_TEST(BgpModuleBaseTest, RunInThreadTest);

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <folly/system/ThreadName.h>

#include <fb303/ThreadCachedServiceData.h>
#include "neteng/fboss/bgp/cpp/common/BgpModuleBase.h"

namespace facebook::bgp {

class DerivedBgpModule : public BgpModuleBase {
 public:
  using BgpModuleBase::BgpModuleBase;

  void run() noexcept override {
    evb_.loop();
  }
  void stop() noexcept override {}

  folly::coro::Task<void> checkHeartbeat() noexcept;

  folly::coro::Task<void> checkCurrentThreadName() noexcept;

  void scheduleHeartbeatLoop() noexcept override {
    heartBeatScheduled_ = true;
    BgpModuleBase::scheduleHeartbeatLoop();
  }

  /*
   * use std::atomic to avoid some tricky tests in the future that
   * could suffer race conditions
   */
  std::atomic<bool> heartBeatScheduled_{false};
};

folly::coro::Task<void> DerivedBgpModule::checkHeartbeat() noexcept {
  auto heartbeatKey = fmt::format("bgpd.heartbeat.{}", moduleName_);

  for (int i = 0; i < 5; i++) {
    fb303::ThreadCachedServiceData::get()->publishStats();

    XLOGF(INFO, "Check heartbeat counter: {}", i);
    EXPECT_EQ(
        fb303::ThreadCachedServiceData::get()->getCounter(heartbeatKey), i);

    co_await folly::coro::sleep(std::chrono::seconds(1));

    co_await folly::coro::co_safe_point;
  }

  asyncScope_.requestCancellation();
}

/*
 * Test heartbeatLoop() function
 */
TEST(BgpModuleBaseTest, HeartbeatLoopTest) {
  DerivedBgpModule module("test");

  folly::EventBase evb;

  module.asyncScope_.add(co_withExecutor(&evb, module.heartbeatLoop()));
  module.asyncScope_.add(co_withExecutor(&evb, module.checkHeartbeat()));

  evb.loop();

  // Cancel the heartbeatLoop() task scheduled by constructor
  module.evb_.loop();

  folly::coro::blockingWait(module.asyncScope_.joinAsync());
}

/*
 * Test scheduleHeartbeatLoop()
 */
TEST(BgpModuleBaseTest, ScheduleHeartbeatLoopTest) {
  DerivedBgpModule module("test");

  EXPECT_FALSE(module.heartBeatScheduled_);

  module.scheduleHeartbeatLoop();
  EXPECT_EQ(module.asyncScope_.remaining(), 1);

  // clean up
  module.asyncScope_.requestCancellation();
  module.evb_.loop();

  folly::coro::blockingWait(module.asyncScope_.joinAsync());

  EXPECT_TRUE(module.heartBeatScheduled_);
}

folly::coro::Task<void> DerivedBgpModule::checkCurrentThreadName() noexcept {
  EXPECT_EQ(*folly::getCurrentThreadName(), "bgpcpp-test");
  EXPECT_EQ(strcmp(google::GetLogThreadName(), "bgpcpp-test"), 0);
  co_return;
}

TEST(BgpModuleBaseTest, RunInThreadTest) {
  DerivedBgpModule module("test");

  module.asyncScope_.add(
      co_withExecutor(&module.evb_, module.checkCurrentThreadName()));

  auto thread = module.runInThread();

  // clean up
  folly::coro::blockingWait(module.asyncScope_.cancelAndJoinAsync());
  thread.join();

  EXPECT_TRUE(module.heartBeatScheduled_);
}

TEST(BgpModuleBaseTest, GetModuleNameTest) {
  DerivedBgpModule module("test");

  EXPECT_EQ(module.getModuleName(), "test");
}

class CoroModule : public BgpModuleBase {
 public:
  using BgpModuleBase::BgpModuleBase;

  void run() noexcept override {
    evb_.loopForever();
  }
  void stop() noexcept override {
    folly::coro::blockingWait(asyncScope_.cancelAndJoinAsync());
    evb_.terminateLoopSoon();
  }

  int getUltimateAnswer(int additional) noexcept {
    return 42 + additional;
  }

  MAKE_CORO_FUNCTION(getUltimateAnswer)
};

TEST(BgpModuleBaseTest, MakeCoroFunctionTest) {
  CoroModule module("test");

  auto thread = module.runInThread();

  // Run the coro on the evb thread and wait on the test thread
  auto ret = folly::coro::blockingWait(module.co_getUltimateAnswer(3));
  EXPECT_EQ(ret, 45);

  // clean up
  module.stop();

  thread.join();
}

}; // namespace facebook::bgp
