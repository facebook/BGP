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

#include <deque>

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <openr/monitor/SystemMetrics.h>

#include "neteng/fboss/bgp/cpp/common/BgpModuleBase.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredModule.h"

namespace facebook::bgp {

using MonitoredPathT = std::vector<std::string>;
using QueueSizeMapT = folly::F14FastMap<std::string, int32_t>;

/**
 * System resource limits derived from gflags in Main.cpp.
 * Passed to Watchdog at construction, accessible to HealthValidator.
 */
struct SystemResourceLimits {
  int64_t rssLimitBytes{0};
};

struct HeartbeatSnapshot {
  int64_t timestampSeconds{0};
  int64_t heartbeatValue{0};
};

class Watchdog : public BgpModuleBase {
 public:
  explicit Watchdog(
      std::shared_ptr<const Config> config,
      SystemResourceLimits resourceLimits = {});

  const SystemResourceLimits& getSystemResourceLimits() const {
    return resourceLimits_;
  }

  folly::F14FastMap<std::string, std::deque<HeartbeatSnapshot>>
  getHeartbeatSnapshots() const;

  virtual ~Watchdog() override {}

  void run() noexcept override;
  void stop() noexcept override;

  void monitorModule(
      const std::string& moduleName,
      MonitoredModule& module) noexcept;

  /**
   * Get the queue sizes for the modules by querying the watchdog.
   *
   * @param paths The list of query paths to query. The path can contain
   * the module name or module name followed by queue name, e.g.
   * ["module1", "module2.queue1", "module3.queue2.queue3"]
   * @return The map of queue sizes where the key is the queue path and
   * the value is the size of the queue.
   */
  QueueSizeMapT getQueueSizes(
      const std::unique_ptr<MonitoredPathT>& paths) noexcept;

  /**
   * Get the BGP++ process uptime in seconds, i.e. the elapsed time since this
   * Watchdog (created once at process startup) recorded startTime_. startTime_
   * is set once in the constructor and never mutated, so this is safe to call
   * from any thread. Also used to publish the bgpd.process.uptime.seconds ODS
   * counter.
   */
  int64_t getUptimeSeconds() const;

 private:
  folly::coro::Task<void> monitorSystemMetricsLoop();
  folly::coro::Task<void> snapshotHeartbeatsLoop();
  folly::coro::Task<void> dumpHeapProfileLoop(const int32_t intervalInSecond);
  void updateSystemMetrics();

  // shared_ptr to the BGP++ config
  std::shared_ptr<const Config> config_;

  // System resource limits derived from gflags
  SystemResourceLimits resourceLimits_;

  folly::F14FastMap<std::string, std::reference_wrapper<MonitoredModule>>
      monitoredModules_;

  // System metrics collector
  openr::SystemMetrics systemMetrics_;
  std::chrono::steady_clock::time_point startTime_;

  /* Written by Watchdog's evb thread, read by BgpService's thrift thread via
   * getHeartbeatSnapshots(). Keeps up to kMaxHeartbeatSnapshots per module
   * for stable oldest-baseline drift detection. */
  folly::Synchronized<
      folly::F14FastMap<std::string, std::deque<HeartbeatSnapshot>>>
      heartbeatSnapshots_;

/*
 * per class placeholder for test code injection
 * only need to be setup once here
 */
#ifdef Watchdog_TEST_FRIENDS
  Watchdog_TEST_FRIENDS
#endif
      friend class WatchdogTestFixture;
};

} // namespace facebook::bgp
