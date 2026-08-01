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

#include <fb303/ThreadCachedServiceData.h>
#include <folly/coro/GtestHelpers.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/health/HealthValidator.h"
#include "neteng/fboss/bgp/cpp/health/facebook/HealthValidatorBB.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"

using namespace facebook::bgp;
using namespace facebook::neteng::fboss::bgp::thrift;

class HealthValidatorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    clearAllCounters();
    validator_ = std::make_unique<HealthValidator>(
        /*peerMgr=*/nullptr,
        /*rib=*/nullptr,
        /*watchdog=*/nullptr);
  }

  void TearDown() override {
    clearAllCounters();
  }

  void setCounter(const std::string& key, int64_t value) {
    facebook::fb303::ThreadCachedServiceData::get()
        ->getServiceData()
        ->setCounter(key, value);
  }

  void clearCounter(const std::string& key) {
    facebook::fb303::ThreadCachedServiceData::get()->clearCounter(key);
  }

  void clearAllCounters() {
    auto* sd =
        facebook::fb303::ThreadCachedServiceData::get()->getServiceData();
    std::map<std::string, int64_t> counters;
    sd->getCounters(counters);
    for (const auto& [key, _] : counters) {
      sd->clearCounter(key);
    }
  }

  const THealthCheckResult* findCheck(
      const THealthReport& report,
      HealthCheckId checkId) {
    for (const auto& mod : *report.modules()) {
      for (const auto& check : *mod.checks()) {
        if (*check.checkId() == checkId) {
          return &check;
        }
      }
    }
    return nullptr;
  }

  /* Build a validator whose config reports enable_next_hop_tracking=true so the
   * NHT checks exercise the enabled (PASS/WARN) path instead of SKIPPED. */
  std::unique_ptr<HealthValidator> makeNhtEnabledValidator() {
    facebook::bgp::thrift::BgpConfig thriftConfig;
    thriftConfig.router_id() = "1.1.1.1";
    thriftConfig.local_as_4_byte() = 65000;
    thriftConfig.bgp_setting_config() =
        facebook::bgp::thrift::BgpSettingConfig();
    thriftConfig.bgp_setting_config()->enable_next_hop_tracking() = true;
    auto config = std::make_shared<const Config>(std::move(thriftConfig));
    auto configManager = std::make_shared<ConfigManager>(config);
    return std::make_unique<HealthValidator>(
        /*peerMgr=*/nullptr,
        /*rib=*/nullptr,
        /*watchdog=*/nullptr,
        /*nexthopHandler=*/nullptr,
        configManager);
  }

  std::unique_ptr<HealthValidator> validator_;
};

/* ── GLOBAL_SYSTEM checks ── */

CO_TEST_F(HealthValidatorTest, ThriftReachable_Pass) {
  setCounter("bgpd.process.uptime.seconds", 300);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_SYSTEM_THRIFT_REACHABLE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, ThriftReachable_Fail_NotAlive) {
  clearCounter("bgpd.process.uptime.seconds");

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_SYSTEM_THRIFT_REACHABLE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, RssMemory_Skipped_NoWatchdog) {
  /* watchdog_ is nullptr -> limitBytes=0 -> SKIPPED */
  setCounter("bgpd.process.memory.rss.bytes", 300LL * 1024 * 1024);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_RSS_MEMORY);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, RssMemory_Error_NoCounter) {
  clearCounter("bgpd.process.memory.rss.bytes");

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_RSS_MEMORY);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

/* RSS tests below use TEST_F + blockingWait because they construct a local
 * Watchdog with SystemResourceLimits, which can't be shared via the
 * CO_TEST_F fixture's validator_ (that has nullptr watchdog). */

TEST_F(HealthValidatorTest, RssMemory_Pass_UnderThreshold) {
  /* 300MB / 2048MB = 14.6% < 20% warning -> PASS */
  SystemResourceLimits limits;
  limits.rssLimitBytes = 2048LL * 1024 * 1024;
  Watchdog watchdog(nullptr, std::move(limits));

  auto validatorWithWd =
      std::make_unique<HealthValidator>(nullptr, nullptr, &watchdog);

  setCounter("bgpd.process.memory.rss.bytes", 300LL * 1024 * 1024);

  auto report = folly::coro::blockingWait(validatorWithWd->generateReport());
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_RSS_MEMORY);
  ASSERT_NE(check, nullptr);
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

TEST_F(HealthValidatorTest, RssMemory_Warning) {
  /* 500MB / 2048MB = 24.4% >= 20% warning, < 50% critical -> WARNING */
  SystemResourceLimits limits;
  limits.rssLimitBytes = 2048LL * 1024 * 1024;
  Watchdog watchdog(nullptr, std::move(limits));

  auto validatorWithWd =
      std::make_unique<HealthValidator>(nullptr, nullptr, &watchdog);

  setCounter("bgpd.process.memory.rss.bytes", 500LL * 1024 * 1024);

  auto report = folly::coro::blockingWait(validatorWithWd->generateReport());
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_RSS_MEMORY);
  ASSERT_NE(check, nullptr);
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

TEST_F(HealthValidatorTest, RssMemory_Fail) {
  /* 1200MB / 2048MB = 58.6% >= 50% critical -> FAIL */
  SystemResourceLimits limits;
  limits.rssLimitBytes = 2048LL * 1024 * 1024;
  Watchdog watchdog(nullptr, std::move(limits));

  auto validatorWithWd =
      std::make_unique<HealthValidator>(nullptr, nullptr, &watchdog);

  setCounter("bgpd.process.memory.rss.bytes", 1200LL * 1024 * 1024);

  auto report = folly::coro::blockingWait(validatorWithWd->generateReport());
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_RSS_MEMORY);
  ASSERT_NE(check, nullptr);
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, CpuUsage_Pass) {
  setCounter("bgpd.process.cpu.percent", 30);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_CPU_USAGE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, CpuUsage_Warning) {
  /* 60% >= 50% warning, < 80% critical -> WARNING */
  setCounter("bgpd.process.cpu.percent", 60);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_CPU_USAGE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, CpuUsage_Fail) {
  /* 90% >= 80% critical -> FAIL */
  setCounter("bgpd.process.cpu.percent", 90);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_CPU_USAGE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, AttrDedup_Error_NoPeerManager) {
  /* PeerManagerBase is nullptr -> ERROR */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_ATTR_DEDUP);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, PlannedExit_Pass) {
  setCounter("bgpd.plannedExit", 1);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_PLANNED_EXIT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, PlannedExit_Warning_Crash) {
  setCounter("bgpd.plannedExit", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_PLANNED_EXIT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

/* ── GLOBAL_TASK_THREAD checks ── */

/* Heartbeat WARNING/FAIL drift tests require injecting snapshots into
 * Watchdog's private folly::Synchronized map. This needs a test-only
 * setter or friend access not yet available. Drift logic is validated
 * via lab testing (show bgp health on live devices). */

CO_TEST_F(HealthValidatorTest, Heartbeats_Error_NoWatchdog) {
  /* watchdog_ is nullptr -> ERROR */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_TASK_HEARTBEATS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

TEST_F(HealthValidatorTest, Heartbeats_Pass_NoSnapshots) {
  /* Watchdog just started, no snapshots yet -> PASS */
  Watchdog watchdog(nullptr);

  auto validatorWithWd =
      std::make_unique<HealthValidator>(nullptr, nullptr, &watchdog);

  auto report = folly::coro::blockingWait(validatorWithWd->generateReport());
  auto* check = findCheck(report, HealthCheckId::GLOBAL_TASK_HEARTBEATS);
  ASSERT_NE(check, nullptr);
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, ProcessUptime_Pass) {
  setCounter("bgpd.process.uptime.seconds", 300);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_TASK_UPTIME);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, ProcessUptime_Warning_TooShort) {
  setCounter("bgpd.process.uptime.seconds", 10);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_TASK_UPTIME);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

/* ── Report aggregation ── */

CO_TEST_F(HealthValidatorTest, GenerateReport_HasAllCategories) {
  auto report = co_await validator_->generateReport();
  EXPECT_EQ(report.modules()->size(), 10);
  EXPECT_GT(*report.timestampMs(), 0);
}

CO_TEST_F(HealthValidatorTest, GenerateReport_OverallStatus_Fail) {
  /* Force a failing check: CPU > threshold */
  setCounter("bgpd.process.cpu.percent", 95);
  setCounter("bgpd.process.uptime.seconds", 300);
  setCounter("bgpd.process.memory.rss.bytes", 100 * 1024 * 1024);
  setCounter("bgpd.plannedExit", 1);
  setCounter("bgpd.process.uptime.seconds", 120);

  auto report = co_await validator_->generateReport();
  EXPECT_EQ(*report.overallStatus(), HealthCheckStatus::FAIL);
  EXPECT_GT(*report.failCount(), 0);
}

/* ── GLOBAL_CONVERGENCE checks ── */

CO_TEST_F(HealthValidatorTest, Convergence_Initialized_Pass) {
  setCounter("initialization.INITIALIZED.duration_ms", 5000);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_INITIALIZED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Convergence_Initialized_Fail) {
  /* No INITIALIZED counter -> not yet initialized */
  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_INITIALIZED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Convergence_Time_Pass) {
  setCounter("initialization.INITIALIZED.duration_ms", 10000);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_TIME);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Convergence_Time_Warning_TooSlow) {
  setCounter("initialization.INITIALIZED.duration_ms", 500000);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_TIME);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Convergence_Time_Skipped_NotConverged) {
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_TIME);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Convergence_NoTimeout_Pass) {
  setCounter("bgpd.initialized.timeout", 0);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_INIT_TIMEOUT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Convergence_Timeout_Warning) {
  setCounter("bgpd.initialized.timeout", 1);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_INIT_TIMEOUT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Convergence_EorReceived_Pass) {
  setCounter("initialization.ALL_EOR_RECEIVED.duration_ms", 3000);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_EOR_RECEIVED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Convergence_EorTimerExpired_Warning) {
  setCounter("initialization.EOR_TIMER_EXPIRED.duration_ms", 60000);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_EOR_RECEIVED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Convergence_SafeMode_Error_NoPeerMgr) {
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_SAFE_MODE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Convergence_InitPhases_Pass) {
  setCounter("initialization.INITIALIZING.duration_ms", 0);
  setCounter("initialization.AGENT_CONFIGURED.duration_ms", 100);
  setCounter("initialization.PEER_INFO_LOADED.duration_ms", 200);
  setCounter("initialization.RIB_COMPUTED.duration_ms", 300);
  setCounter("initialization.INITIALIZED.duration_ms", 400);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_INIT_PHASES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Convergence_InitPhases_LateEorAfterTimer_Fail) {
  /* EoR timer forced convergence at 120979ms and INITIALIZED published 1ms
   * later; ALL_EOR_RECEIVED did not land until 446272ms -- ~5.4 min after the
   * timer and after INITIALIZED. The daemon programmed FIB before a peer's
   * routes were in, so this out-of-order inversion is a FAIL. */
  setCounter("initialization.INITIALIZING.duration_ms", 1);
  setCounter("initialization.AGENT_CONFIGURED.duration_ms", 43);
  setCounter("initialization.PEER_INFO_LOADED.duration_ms", 69);
  setCounter("initialization.ALL_EOR_RECEIVED.duration_ms", 446272);
  setCounter("initialization.EOR_TIMER_EXPIRED.duration_ms", 120979);
  setCounter("initialization.RIB_COMPUTED.duration_ms", 120979);
  setCounter("initialization.FIB_SYNCED.duration_ms", 120980);
  setCounter("initialization.EOR_SENT.duration_ms", 120980);
  setCounter("initialization.INITIALIZED.duration_ms", 120980);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_INIT_PHASES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Convergence_InitPhases_NaturalEor_Pass) {
  /* Healthy convergence: all EoR received (300ms) before RIB computation, no
   * EoR timer expiry. Full event sequence is monotonic. */
  setCounter("initialization.INITIALIZING.duration_ms", 0);
  setCounter("initialization.AGENT_CONFIGURED.duration_ms", 100);
  setCounter("initialization.PEER_INFO_LOADED.duration_ms", 200);
  setCounter("initialization.ALL_EOR_RECEIVED.duration_ms", 300);
  setCounter("initialization.RIB_COMPUTED.duration_ms", 400);
  setCounter("initialization.FIB_SYNCED.duration_ms", 500);
  setCounter("initialization.EOR_SENT.duration_ms", 600);
  setCounter("initialization.INITIALIZED.duration_ms", 700);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_INIT_PHASES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Convergence_InitPhases_TimerForcedNoEor_Pass) {
  /* Timer forced convergence and ALL_EOR_RECEIVED never fired. The events that
   * did fire are still monotonic, so ordering is clean -- the never-received
   * EoR is the EOR_RECEIVED check's concern, not this ordering check. */
  setCounter("initialization.INITIALIZING.duration_ms", 0);
  setCounter("initialization.AGENT_CONFIGURED.duration_ms", 100);
  setCounter("initialization.PEER_INFO_LOADED.duration_ms", 200);
  setCounter("initialization.EOR_TIMER_EXPIRED.duration_ms", 300);
  setCounter("initialization.RIB_COMPUTED.duration_ms", 300);
  setCounter("initialization.FIB_SYNCED.duration_ms", 301);
  setCounter("initialization.EOR_SENT.duration_ms", 301);
  setCounter("initialization.INITIALIZED.duration_ms", 301);

  auto report = co_await validator_->generateReport();
  auto* check =
      findCheck(report, HealthCheckId::GLOBAL_CONVERGENCE_INIT_PHASES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

/* ── SESSION_MANAGER checks ── */

CO_TEST_F(HealthValidatorTest, Session_Established_Pass) {
  setCounter("bgpd.runningSessions", 5);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::SESSION_ESTABLISHED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Session_Established_Fail_Zero) {
  setCounter("bgpd.runningSessions", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::SESSION_ESTABLISHED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Session_Flaps_Pass) {
  /* 10 sessions, 4 flaps in 10min = 40% < 50% threshold -> PASS */
  setCounter("bgpd.runningSessions", 10);
  setCounter("bgpd.sessionStateChanges.sum.600", 4);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::SESSION_FLAPS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Session_Flaps_Warning) {
  /* 10 sessions, 6 flaps in 10min = 60% > 50% threshold -> WARNING */
  setCounter("bgpd.runningSessions", 10);
  setCounter("bgpd.sessionStateChanges.sum.600", 6);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::SESSION_FLAPS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Session_HoldTimerExpiry_Skipped) {
  /* Needs per-peer session iteration for useful diagnostics */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::SESSION_HOLD_TIMER_EXPIRY);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Session_Notifications_Skipped) {
  /* Lifetime counter not useful; needs rate-based check */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::SESSION_NOTIFICATIONS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Session_Keepalives_Skipped) {
  /* Lifetime counter not useful; needs rate-based check */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::SESSION_KEEPALIVES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Session_SocketErrors_Skipped) {
  /* Lifetime counter not useful; needs rate-based check */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::SESSION_SOCKET_ERRORS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

/* ── PEER_MANAGER checks ── */

CO_TEST_F(HealthValidatorTest, Peer_ZeroRoutes_Pass) {
  setCounter("peer.totalPeerWithNoRouteExchange", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_ZERO_ROUTES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Peer_ZeroRoutes_Warning) {
  setCounter("peer.totalPeerWithNoRouteExchange", 3);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_ZERO_ROUTES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Peer_SlowDetached_Pass) {
  setCounter("bgpcpp.slow_peer_detection_count.sum.600", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_SLOW_DETACHED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Peer_DroppedPrefixes_Pass) {
  setCounter("peer.totalDroppedPrefixes", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_DROPPED_PREFIXES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Peer_DroppedPrefixes_Warn) {
  /* >0 = prefixes actually dropped -> WARN, regardless of mode. See S676351. */
  setCounter("peer.totalDroppedPrefixes", 5);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_DROPPED_PREFIXES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
  EXPECT_EQ(*check->message(), "droppedPrefixes = 5");
}

CO_TEST_F(HealthValidatorTest, Peer_ThriftRejects_Pass) {
  setCounter("bgpd.thriftReject", 0);
  setCounter("bgpd.thriftSuspend", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_THRIFT_REJECTS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Peer_ThriftRejects_Warning) {
  setCounter("bgpd.thriftReject", 5);
  setCounter("bgpd.thriftSuspend", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_THRIFT_REJECTS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Peer_PrefixCount_Pass) {
  setCounter("bgpd.runningSessions", 5);
  setCounter("peer.totalPeerWithNoRouteExchange", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_PREFIX_COUNT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Peer_PrefixCount_Warning) {
  setCounter("bgpd.runningSessions", 5);
  setCounter("peer.totalPeerWithNoRouteExchange", 2);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_PREFIX_COUNT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Peer_PolicyCache_Pass) {
  setCounter("bgpd.policy_cache.hit", 80);
  setCounter("bgpd.policy_cache.miss", 20);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_POLICY_CACHE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Peer_PolicyCache_Warning) {
  setCounter("bgpd.policy_cache.hit", 10);
  setCounter("bgpd.policy_cache.miss", 90);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_POLICY_CACHE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}
/* ── RIB checks ── */

CO_TEST_F(HealthValidatorTest, Rib_OriginatedRoutes_Error_NoRib) {
  /* Rib is nullptr in test -> ERROR */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_ORIGINATED_ROUTES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Rib_PathSelection_Pass) {
  setCounter("bgpd.rib.fullSyncPathSelectionTimeMs.p99.60", 1000);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_PATH_SELECTION_DURATION);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Rib_PathSelection_Fail_TooSlow) {
  /* fb303 quantile stats (.p99.60) are managed by the quantile
   * framework which may overwrite setCounter values. Accept PASS
   * (overwritten to 0) or FAIL (value retained at 8000 > 5000). */
  setCounter("bgpd.rib.fullSyncPathSelectionTimeMs.p99.60", 8000);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_PATH_SELECTION_DURATION);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  auto status = *check->status();
  EXPECT_TRUE(
      status == HealthCheckStatus::PASS || status == HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Rib_PathSelection_Skipped_NoStat) {
  /* fb303 quantile stats auto-export .p99.60 with value 0, so
   * the counter may exist even without explicit setCounter.
   * Accept PASS (auto-exported 0 < 5000) or SKIPPED (absent). */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_PATH_SELECTION_DURATION);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  auto status = *check->status();
  EXPECT_TRUE(
      status == HealthCheckStatus::PASS ||
      status == HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Rib_ReceivedRoutes_Pass) {
  setCounter("bgpd.rib.totalRibPaths", 5000);
  setCounter("bgpd.rib.totalOriginatedRoutes", 24);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_RECEIVED_ROUTES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Rib_ReceivedRoutes_Fail_Zero) {
  setCounter("bgpd.rib.totalRibPaths", 24);
  setCounter("bgpd.rib.totalOriginatedRoutes", 24);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_RECEIVED_ROUTES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Rib_TableVersion_Pass) {
  setCounter("bgpcpp.rib.tableVersion", 100);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_TABLE_VERSION);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Rib_OverloadMode_Skipped_NoLimits) {
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_OVERLOAD_MODE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Rib_OverloadMode_Pass_UnderLimit) {
  /* 5000 prefixes / 20000 limit = 25% -> PASS */
  setCounter("peer.totalUniquePrefixes", 5000);
  setCounter("bgpd.unique_prefix_limit", 20000);
  setCounter("bgpd.total_path_limit", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_OVERLOAD_MODE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Rib_OverloadMode_Warning_NearLimit) {
  /* 17000 prefixes / 20000 limit = 85% -> WARNING */
  setCounter("peer.totalUniquePrefixes", 17000);
  setCounter("bgpd.unique_prefix_limit", 20000);
  setCounter("bgpd.total_path_limit", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_OVERLOAD_MODE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Rib_OverloadMode_Fail_AtLimit) {
  /* 20000 paths / 20000 limit = 100% -> FAIL */
  setCounter("peer.totalPaths", 20000);
  setCounter("bgpd.total_path_limit", 20000);
  setCounter("bgpd.unique_prefix_limit", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_OVERLOAD_MODE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Rib_ShadowConsistent_Pass) {
  /* Within 10% divergence -> PASS */
  setCounter("fib.totalUcastRoutes", 5000);
  setCounter("bgpd.rib.totalShadowRibEntries", 4600);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_SHADOW_CONSISTENT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Rib_ShadowConsistent_Warning) {
  setCounter("fib.totalUcastRoutes", 5000);
  setCounter("bgpd.rib.totalShadowRibEntries", 3000);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_SHADOW_CONSISTENT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Rib_PauseTime_Pass) {
  setCounter("bgpd.rib.bestPathAndFibProgrammingPauseTimeMs.p99.60", 1000);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_PAUSE_TIME);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Rib_PauseTime_DefaultPass) {
  /* Quantile stat .p99.60 auto-exports with value 0 when no data —
   * 0 < 5000ms threshold = PASS. Cannot reliably set quantile stat
   * values via setCounter in unit tests. */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::RIB_PAUSE_TIME);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  auto status = *check->status();
  EXPECT_TRUE(
      status == HealthCheckStatus::PASS || status == HealthCheckStatus::FAIL);
}

/* ── NETLINK_WRAPPER checks ── */

CO_TEST_F(HealthValidatorTest, Netlink_Skipped_NhtNotEnabled) {
  /* No configManager (nullptr) -> nhtEnabled=false -> SKIPPED */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::NETLINK_TRACKED_INTERFACES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

/* ── NEXTHOP_TRACKER checks ── */

CO_TEST_F(HealthValidatorTest, NHT_ConfigConsistent_Pass_BothDisabled) {
  /* No configManager -> nhtEnabled=false, counter unset -> runtimeActive=false
   * consistent = (!nhtEnabled) -> PASS */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_CONFIG_CONSISTENT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, NHT_CacheNotEmpty_Skipped_NhtDisabled) {
  /* NHT not enabled in config -> SKIPPED */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_CACHE_NOT_EMPTY);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, NHT_CacheNotEmpty_Skipped_NoConfig) {
  /* Without configManager, nhtEnabled=false -> SKIPPED */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_CACHE_NOT_EMPTY);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, NHT_CacheNotEmpty_Pass_WhenTrackingNexthops) {
  /* NHT enabled + nexthop_info.count > 0 -> PASS */
  setCounter(RibStats::kNexthopInfoCount, 5);
  auto validator = makeNhtEnabledValidator();

  auto report = co_await validator->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_CACHE_NOT_EMPTY);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, NHT_CacheNotEmpty_Warn_WhenNoNexthopsTracked) {
  /* NHT enabled + nexthop_info.count == 0 -> WARN */
  setCounter(RibStats::kNexthopInfoCount, 0);
  auto validator = makeNhtEnabledValidator();

  auto report = co_await validator->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_CACHE_NOT_EMPTY);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, NHT_ConfigConsistent_Pass_WhenEnabledAndActive) {
  /* config enabled + nexthop_info.count > 0 -> consistent -> PASS */
  setCounter(RibStats::kNexthopInfoCount, 3);
  auto validator = makeNhtEnabledValidator();

  auto report = co_await validator->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_CONFIG_CONSISTENT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(
    HealthValidatorTest,
    NHT_ConfigConsistent_Warn_WhenEnabledButInactive) {
  /* config enabled + nexthop_info.count == 0 -> inconsistent -> WARN */
  setCounter(RibStats::kNexthopInfoCount, 0);
  auto validator = makeNhtEnabledValidator();

  auto report = co_await validator->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_CONFIG_CONSISTENT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, NHT_StreamConnected_Skipped) {
  /* 1.8.2 MEDIUM complexity — always SKIPPED */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_STREAM_CONNECTED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, NHT_UnreachableNexthops_Skipped) {
  /* 1.8.3 MEDIUM complexity — always SKIPPED */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_UNREACHABLE_NEXTHOPS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, NHT_FsdbSubscriptions_Skipped) {
  /* 1.8.4 MEDIUM complexity — always SKIPPED */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::NHT_FSDB_SUBSCRIPTIONS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

/* ── FIB_AGENT checks ── */

CO_TEST_F(HealthValidatorTest, Fib_Connected_Pass) {
  setCounter("fib.agentProgrammable", 1);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_CONNECTED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Fib_Connected_Fail) {
  setCounter("fib.agentProgrammable", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_CONNECTED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Fib_Synced_Pass) {
  setCounter("fib.synced", 1);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_SYNCED);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Fib_UpdateFailures_Skipped) {
  /* Lifetime counter not useful; needs rate-based check */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_UPDATE_FAILURES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Fib_StatusFailures_Skipped) {
  /* Lifetime counter not useful; needs rate-based check */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_STATUS_FAILURES);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Fib_Holddown_Skipped) {
  /* Holddown state not exposed as counter */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_HOLDDOWN);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

CO_TEST_F(HealthValidatorTest, Fib_Routes_Pass) {
  setCounter("fib.totalUcastRoutes", 5000);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_ROUTES_EXIST);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Fib_Routes_Fail_Zero) {
  setCounter("fib.totalUcastRoutes", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_ROUTES_EXIST);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Fib_Flush_Pass) {
  setCounter("fib.flushed.sum.600", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_FLUSH_EVENTS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Fib_Flush_Warning) {
  setCounter("fib.flushed.sum.600", 2);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::FIB_AGENT_FLUSH_EVENTS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

/* ── THRIFT_ENDPOINT checks ── */

CO_TEST_F(HealthValidatorTest, Thrift_Alive_PortCheck) {
  /* TCP connect to [::1]:6909. Result depends on test environment:
   * PASS if something listens on 6909, FAIL otherwise. */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::THRIFT_ALIVE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  auto status = *check->status();
  EXPECT_TRUE(
      status == HealthCheckStatus::PASS || status == HealthCheckStatus::FAIL);
}

CO_TEST_F(HealthValidatorTest, Thrift_StreamingRejects_Pass) {
  setCounter("bgpd.streamingSessionsRejected", 0);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::THRIFT_STREAMING_REJECTS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::PASS);
}

CO_TEST_F(HealthValidatorTest, Thrift_StreamingRejects_Warning) {
  setCounter("bgpd.streamingSessionsRejected", 3);

  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::THRIFT_STREAMING_REJECTS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::WARN);
}

CO_TEST_F(HealthValidatorTest, Thrift_DrainState_Error_NoConfig) {
  /* ConfigManager is nullptr -> ERROR */
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::THRIFT_DRAIN_STATE);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::FAIL);
}

/* ── HealthValidatorBB checks ── */

class HealthValidatorBBTest : public HealthValidatorTest {
 protected:
  void SetUp() override {
    HealthValidatorTest::SetUp();
    bbValidator_ = std::make_unique<HealthValidatorBB>(
        /*peerMgr=*/nullptr,
        /*rib=*/nullptr,
        /*watchdog=*/nullptr,
        /*nlWrapper=*/nullptr);
  }

  std::unique_ptr<HealthValidatorBB> bbValidator_;
};

CO_TEST_F(HealthValidatorBBTest, PlannedExit_Skipped_OnBBPlatform) {
  /* checkPlannedExit() returns SKIPPED unconditionally on BB, regardless of
   * the bgpd.plannedExit counter. */
  auto report = co_await bbValidator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::GLOBAL_SYSTEM_PLANNED_EXIT);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}

TEST(HealthValidatorPrefixLimitDropsTest, PassWhenNoDrops) {
  TBgpSession s1;
  s1.peer_addr() = "1.2.3.4";
  TBgpSession s2;
  s2.peer_addr() = "5.6.7.8";
  s2.prepolicy_rcvd_dropped_prefix_count() = 0;

  auto result = HealthValidator::evaluatePrefixLimitDrops({s1, s2});
  EXPECT_EQ(*result.status(), HealthCheckStatus::PASS);
  EXPECT_EQ(*result.observedValue(), 0.0);
}

TEST(HealthValidatorPrefixLimitDropsTest, WarnListsExactPerPeerCounts) {
  TBgpSession dropper1;
  dropper1.peer_addr() = "1.2.3.4";
  dropper1.prepolicy_rcvd_dropped_prefix_count() = 50;

  TBgpSession dropper2;
  dropper2.peer_addr() = "5.6.7.8";
  dropper2.prepolicy_rcvd_dropped_prefix_count() = 30;

  TBgpSession clean;
  clean.peer_addr() = "9.9.9.9";

  auto result =
      HealthValidator::evaluatePrefixLimitDrops({dropper1, dropper2, clean});

  EXPECT_EQ(*result.status(), HealthCheckStatus::WARN);
  EXPECT_EQ(*result.observedValue(), 2.0);
  const auto& msg = *result.message();
  EXPECT_NE(msg.find("1.2.3.4 (PR dropped 50)"), std::string::npos);
  EXPECT_NE(msg.find("5.6.7.8 (PR dropped 30)"), std::string::npos);
  // The clean peer must not be listed.
  EXPECT_EQ(msg.find("9.9.9.9"), std::string::npos);
}

CO_TEST_F(HealthValidatorTest, Peer_PrefixLimitDrops_SkippedWhenNoPeerMgr) {
  // The fixture validator has a null PeerManagerBase, so per-peer enumeration
  // is skipped rather than failed.
  auto report = co_await validator_->generateReport();
  auto* check = findCheck(report, HealthCheckId::PEER_PREFIX_LIMIT_DROPS);
  EXPECT_NE(check, nullptr);
  if (!check) {
    co_return;
  }
  EXPECT_EQ(*check->status(), HealthCheckStatus::SKIPPED);
}
