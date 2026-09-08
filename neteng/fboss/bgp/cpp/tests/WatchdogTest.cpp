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

#define Watchdog_TEST_FRIENDS                                                  \
  FRIEND_TEST(WatchdogTest, MonitorModuleTest);                                \
  FRIEND_TEST(WatchdogTest, SystemMetricsUpdateBasicTest);                     \
  FRIEND_TEST(WatchdogTestFixture, SystemMetricsLoopTest);                     \
  FRIEND_TEST(WatchdogTestFixture, BasicTest);                                 \
  FRIEND_TEST(WatchdogMemoryProfilingTestFixture, MemoryProfilingEnabledTest); \
  FRIEND_TEST(WatchdogTestFixture, MemoryProfilingDisabledTest);

#include <folly/coro/BlockingWait.h>

#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"

using namespace ::testing;

namespace facebook::bgp {

class WatchdogTestFixture : public ::testing::Test {
 protected:
  /*
   * Create a minimal Config for testing
   * Subclasses can override this to provide different configurations
   */
  virtual std::shared_ptr<const Config> createConfig() {
    thrift::BgpConfig bgpConfig;
    // Set minimal required fields for a valid config
    bgpConfig.local_as_4_byte() = 65000;
    bgpConfig.router_id() = "1.1.1.1";

    return std::make_shared<const Config>(bgpConfig);
  }

  void SetUp() override {
    testConfig_ = createConfig();
    watchdog_ = std::make_unique<Watchdog>(testConfig_);
    watchdogThread_ = watchdog_->runInThread();
  }

  void TearDown() override {
    watchdog_->stop();
    watchdogThread_.join();
  }

  void setStartTime(uint32_t time) {
    watchdog_->startTime_ =
        std::chrono::steady_clock::now() - std::chrono::seconds(time);
  }

  void updateSystemMetrics() {
    watchdog_->updateSystemMetrics();
  }

  Watchdog& getWatchdog() {
    return *watchdog_;
  }

  std::shared_ptr<const Config> testConfig_;
  std::unique_ptr<Watchdog> watchdog_;

  std::thread watchdogThread_;
};

// Basic test to ensure that we could run and stop watchdog
TEST_F(WatchdogTestFixture, BasicTest) {
  EXPECT_EQ(watchdog_->getModuleName(), "watchdog");
  // Validate that config is properly set and not nullptr
  EXPECT_NE(watchdog_->config_, nullptr);
  EXPECT_EQ(watchdog_->config_, testConfig_);
}

TEST(WatchdogTest, MonitorModuleTest) {
  Watchdog watchdog{nullptr};
  MonitoredModule module;
  auto& messages = subscribeToLogMessages("");

  {
    messages.clear();

    watchdog.monitorModule("test", module);

    EXPECT_EQ(&watchdog.monitoredModules_.at("test").get(), &module);
    EXPECT_EQ(0, messages.size());
  }

  // module is already monitored, the name does not matter
  {
    messages.clear();

    watchdog.monitorModule("test-other-module", module);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(),
        "Module test-other-module is already monitored.");

    // not changed
    EXPECT_EQ(&watchdog.monitoredModules_.at("test").get(), &module);
  }

  // already monitored
  {
    messages.clear();

    MonitoredModule otherModule;

    watchdog.monitorModule("test", otherModule);

    EXPECT_EQ(1, messages.size());
    EXPECT_EQ(
        messages[0].first.getMessage(), "Module test is already monitored.");

    // not changed
    EXPECT_EQ(&watchdog.monitoredModules_.at("test").get(), &module);
  }
}

/*
 * Test the function getQueueSizes with 3 scenarios:
 *   1. Query all monitored queues
 *   2. Query monitored queues under module1. When the parent module
 *      path and the children module path are both specified. All monitored
 *      queues under the parent module should be returned.
 *   3. Query a specfic monitored queue
 */
TEST(WatchdogTest, GetQueueSizesTest) {
  MonitoredModule module1;
  MonitoredQueue<std::deque<int>> queue11;
  MonitoredQueue<std::deque<int>> queue12;
  module1.monitorQueue("queue1", queue11, MonitorableQueueTrace::Direction::IN);
  module1.monitorQueue("queue2", queue12, MonitorableQueueTrace::Direction::IN);

  MonitoredModule module2;
  MonitoredQueue<std::deque<int>> queue21;
  module2.monitorQueue("queue1", queue21, MonitorableQueueTrace::Direction::IN);

  // sizes: queue11: 3, queue12: 1, queue21: 2
  queue11.push_back(1);
  queue11.push_back(1);
  queue11.push_back(1);

  queue12.push_back(1);

  queue21.push_back(1);
  queue21.push_back(1);

  Watchdog watchdog{nullptr};
  watchdog.monitorModule("module1", module1);
  watchdog.monitorModule("module2", module2);

  // Query all
  {
    std::vector<std::string> paths = {};

    auto queueSizes = watchdog.getQueueSizes(
        std::make_unique<std::vector<std::string>>(paths));

    QueueSizeMapT expectedResults = {
        {"module1.queue1", 3}, {"module1.queue2", 1}, {"module2.queue1", 2}};

    EXPECT_EQ(queueSizes, expectedResults);
  }

  // Query module1
  {
    std::vector<std::string> paths = {"module1.queue1", "module1"};

    auto queueSizes = watchdog.getQueueSizes(
        std::make_unique<std::vector<std::string>>(paths));

    QueueSizeMapT expectedResults = {
        {"module1.queue1", 3}, {"module1.queue2", 1}};

    EXPECT_EQ(queueSizes, expectedResults);
  }

  // Query module1.queue2
  {
    std::vector<std::string> paths = {"module1.queue2"};

    auto queueSizes = watchdog.getQueueSizes(
        std::make_unique<std::vector<std::string>>(paths));

    QueueSizeMapT expectedResults = {{"module1.queue2", 1}};

    EXPECT_EQ(queueSizes, expectedResults);
  }
}

/*
 * Test the function getQueueSizes with nested monitored modules
 */
TEST(WatchdogTest, GetQueueSizesNestedModuleTest) {
  MonitoredModule module1;
  MonitoredQueue<std::deque<int>> queue11;
  MonitoredQueue<std::deque<int>> queue12;
  module1.monitorQueue("queue1", queue11, MonitorableQueueTrace::Direction::IN);
  module1.monitorQueue("queue2", queue12, MonitorableQueueTrace::Direction::IN);

  MonitoredModule module2;
  MonitoredQueue<std::deque<int>> queue21;
  module2.monitorQueue("queue1", queue21, MonitorableQueueTrace::Direction::IN);

  // sizes: queue11: 3, queue12: 1, queue21: 2
  queue11.push_back(1);
  queue11.push_back(1);
  queue11.push_back(1);

  queue12.push_back(1);

  queue21.push_back(1);
  queue21.push_back(1);

  // watchdog -> module1 -> module2
  Watchdog watchdog{nullptr};
  watchdog.monitorModule("module1", module1);
  module1.monitorModule("module2", module2);

  // Query module1
  {
    std::vector<std::string> paths = {"module1"};

    auto queueSizes = watchdog.getQueueSizes(
        std::make_unique<std::vector<std::string>>(paths));

    QueueSizeMapT expectedResults = {
        {"module1.queue1", 3},
        {"module1.queue2", 1},
        {"module1.module2.queue1", 2}};

    EXPECT_EQ(queueSizes, expectedResults);
  }

  // Query module1.module2
  {
    std::vector<std::string> paths = {"module1.module2"};

    auto queueSizes = watchdog.getQueueSizes(
        std::make_unique<std::vector<std::string>>(paths));

    QueueSizeMapT expectedResults = {{"module1.module2.queue1", 2}};

    EXPECT_EQ(queueSizes, expectedResults);
  }
}

// Test to validate system metrics update functionality
TEST_F(WatchdogTestFixture, SystemMetricsUpdateBasicTest) {
  // Reset all relevant counters before test
  facebook::fb303::ThreadCachedServiceData::getShared()->setCounter(
      "bgpd.process.uptime.seconds", 0);
  facebook::fb303::ThreadCachedServiceData::getShared()->setCounter(
      "bgpd.process.memory.rss.bytes", 0);
  facebook::fb303::ThreadCachedServiceData::getShared()->setCounter(
      "bgpd.process.cpu.percent", 0);

  // Manually adjust start time to ensure uptime > 0 for testing
  setStartTime(2);

  // Call updateSystemMetrics - should not throw
  EXPECT_NO_THROW(updateSystemMetrics());

  // Verify uptime counter is set and greater than 0
  const int64_t uptimeSeconds =
      facebook::fb303::ThreadCachedServiceData::getShared()->getCounter(
          "bgpd.process.uptime.seconds");
  EXPECT_GT(uptimeSeconds, 0);

  /*
   * Memory and CPU counters depend on system availability
   * but at minimum should not cause crashes
   */
  const int64_t memoryBytes =
      facebook::fb303::ThreadCachedServiceData::getShared()->getCounter(
          "bgpd.process.memory.rss.bytes");
  EXPECT_GE(memoryBytes, 0);

  const int64_t cpuPercent =
      facebook::fb303::ThreadCachedServiceData::getShared()->getCounter(
          "bgpd.process.cpu.percent");
  EXPECT_GE(cpuPercent, 0);
}

/*
 * Verify getUptimeSeconds() reflects the configured start time and is
 * monotonically non-decreasing across samples.
 */
TEST_F(WatchdogTestFixture, GetUptimeSecondsTest) {
  /*
   * Pin the start time 5 seconds in the past so uptime is deterministically >
   * 0.
   */
  setStartTime(5);

  const int64_t uptime1 = getWatchdog().getUptimeSeconds();
  EXPECT_GE(uptime1, 5);

  // A second sample (taken later in wall-clock time) must not go backwards.
  const int64_t uptime2 = getWatchdog().getUptimeSeconds();
  EXPECT_GE(uptime2, uptime1);
}

// Test to make sure system metrics coroutine task runs periodically
TEST_F(WatchdogTestFixture, SystemMetricsLoopTest) {
  // Reset the uptime counter
  facebook::fb303::ThreadCachedServiceData::getShared()->setCounter(
      "bgpd.process.uptime.seconds", 0);

  auto now = std::chrono::steady_clock::now();
  folly::EventBase testEvb;

  testEvb.scheduleAt(
      [&]() noexcept {
        // Initially the counter should be 0 or very small
        const int64_t initialUptime =
            facebook::fb303::ThreadCachedServiceData::getShared()->getCounter(
                "bgpd.process.uptime.seconds");
        EXPECT_GE(initialUptime, 0);
      },
      now + std::chrono::milliseconds(100));

  testEvb.scheduleAt(
      [&]() noexcept {
        // After some time, verify the system metrics task is running
        const int64_t laterUptime =
            facebook::fb303::ThreadCachedServiceData::getShared()->getCounter(
                "bgpd.process.uptime.seconds");
        EXPECT_GT(laterUptime, 0);

        // shut down eventbase for testing
        testEvb.terminateLoopSoon();
      },
      now + kWatchdogSystemMetricsInterval + std::chrono::milliseconds(100));

  // test magic starts here
  testEvb.loop();
}

// Test fixture for Watchdog with memory profiling enabled
class WatchdogMemoryProfilingTestFixture : public WatchdogTestFixture {
 protected:
  // Override createConfig to provide memory profiling configuration
  std::shared_ptr<const Config> createConfig() override {
    thrift::BgpConfig bgpConfig;
    // Set minimal required fields for a valid config
    bgpConfig.local_as_4_byte() = 65000;
    bgpConfig.router_id() = "1.1.1.1";

    // Configure memory profiling
    thrift::BgpSettingConfig settingConfig;
    thrift::MemoryProfilingConfig memProfilingConfig;
    memProfilingConfig.enable_memory_profiling() = true;
    memProfilingConfig.heap_dump_interval_s() = kHeapDumpIntervalSeconds;
    settingConfig.memory_profiling_config() = memProfilingConfig;
    bgpConfig.bgp_setting_config() = settingConfig;

    return std::make_shared<const Config>(bgpConfig);
  }

  static constexpr int32_t kHeapDumpIntervalSeconds = 2;
};

// Test to verify memory profiling config is properly loaded
TEST_F(WatchdogMemoryProfilingTestFixture, MemoryProfilingEnabledTest) {
  // Validate that config is properly set
  EXPECT_NE(watchdog_->config_, nullptr);

  // Verify memory profiling config is available
  auto memConfig = watchdog_->config_->getMemoryProfilingConfig();
  ASSERT_NE(memConfig, nullptr);

  // Verify memory profiling is enabled
  EXPECT_TRUE(*memConfig->enable_memory_profiling());

  // Verify heap dump interval is correctly set
  EXPECT_EQ(*memConfig->heap_dump_interval_s(), kHeapDumpIntervalSeconds);
}

// Test to verify memory profiling is disabled by default
TEST_F(WatchdogTestFixture, MemoryProfilingDisabledTest) {
  // Validate that config is properly set
  EXPECT_NE(watchdog_->config_, nullptr);

  // Verify memory profiling config is not available (returns nullptr)
  auto memConfig = watchdog_->config_->getMemoryProfilingConfig();
  EXPECT_EQ(memConfig, nullptr);
}

/* ── HeartbeatSnapshot and SystemResourceLimits infrastructure ── */

TEST(WatchdogTest, HeartbeatSnapshots_EmptyInitially) {
  Watchdog watchdog(nullptr);
  auto snapshots = watchdog.getHeartbeatSnapshots();
  EXPECT_TRUE(snapshots.empty());
}

TEST(WatchdogTest, SystemResourceLimits_DefaultZero) {
  Watchdog watchdog(nullptr);
  EXPECT_EQ(watchdog.getSystemResourceLimits().rssLimitBytes, 0);
}

TEST(WatchdogTest, SystemResourceLimits_CustomValue) {
  SystemResourceLimits limits;
  limits.rssLimitBytes = 4096LL * 1024 * 1024;
  Watchdog watchdog(nullptr, std::move(limits));
  EXPECT_EQ(
      watchdog.getSystemResourceLimits().rssLimitBytes, 4096LL * 1024 * 1024);
}

} // namespace facebook::bgp
