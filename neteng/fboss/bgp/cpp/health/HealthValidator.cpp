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

#include "neteng/fboss/bgp/cpp/health/HealthValidator.h"

#include <chrono>
#include <cstdlib>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/File.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <thrift/lib/cpp/util/EnumUtils.h>

#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/EvbUtils.h"
#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/peer/PeerManagerBase.h"
#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"
#include "neteng/fboss/bgp/cpp/rib/RibBase.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"

namespace facebook::bgp {

using neteng::fboss::bgp::thrift::HealthCheckCategory;
using neteng::fboss::bgp::thrift::HealthCheckId;
using neteng::fboss::bgp::thrift::HealthCheckStatus;
using neteng::fboss::bgp::thrift::THealthCheckResult;
using neteng::fboss::bgp::thrift::THealthReport;
using neteng::fboss::bgp::thrift::TModuleHealthReport;

/* Thresholds are defined in HealthValidator.h for test visibility */

HealthValidator::HealthValidator(
    PeerManagerBase* peerMgr,
    RibBase* rib,
    Watchdog* watchdog,
    NexthopHandler* nexthopHandler,
    std::shared_ptr<ConfigManager> configManager)
    : peerMgr_(peerMgr),
      rib_(rib),
      watchdog_(watchdog),
      nexthopHandler_(nexthopHandler),
      configManager_(std::move(configManager)) {}

folly::coro::Task<THealthReport> HealthValidator::generateReport() {
  THealthReport report;
  auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
                 .count();
  report.timestampMs() = now;

  auto& modules = *report.modules();
  modules.emplace_back(checkGlobalSystem());
  modules.emplace_back(checkGlobalTaskThread());
  modules.emplace_back(checkGlobalConvergence());
  modules.emplace_back(checkSessionManager());
  modules.emplace_back(co_await checkPeerManager());
  modules.emplace_back(co_await checkRib());
  modules.emplace_back(checkNetlinkWrapper());
  modules.emplace_back(checkNexthopTracker());
  modules.emplace_back(checkFibAgent());
  modules.emplace_back(checkThriftEndpoint());

  uint32_t pass = 0, fail = 0, skip = 0, warn = 0;
  bool anyModuleNonSkipped = false;
  HealthCheckStatus worstStatus = HealthCheckStatus::PASS;

  for (const auto& mod : modules) {
    for (const auto& check : *mod.checks()) {
      switch (*check.status()) {
        case HealthCheckStatus::PASS:
          ++pass;
          break;
        case HealthCheckStatus::FAIL:
          ++fail;
          break;
        case HealthCheckStatus::WARN:
          ++warn;
          break;
        case HealthCheckStatus::SKIPPED:
          ++skip;
          break;
        case HealthCheckStatus::UNKNOWN:
          break;
      }
    }

    auto modStatus = *mod.overallStatus();
    if (modStatus != HealthCheckStatus::SKIPPED) {
      anyModuleNonSkipped = true;
    }

    if (modStatus == HealthCheckStatus::FAIL) {
      worstStatus = HealthCheckStatus::FAIL;
    } else if (
        modStatus == HealthCheckStatus::WARN &&
        worstStatus != HealthCheckStatus::FAIL) {
      worstStatus = HealthCheckStatus::WARN;
    }
  }

  /* If every module was SKIPPED, overall should be SKIPPED not PASS */
  if (!anyModuleNonSkipped) {
    worstStatus = HealthCheckStatus::SKIPPED;
  }

  report.overallStatus() = worstStatus;
  report.passCount() = static_cast<int32_t>(pass);
  report.failCount() = static_cast<int32_t>(fail);
  report.skipCount() = static_cast<int32_t>(skip);
  report.warnCount() = static_cast<int32_t>(warn);
  co_return report;
}

/* ────────────────────────────────────────────────────────────────
 * GLOBAL_SYSTEM checks (doc 1.1.x)
 * ──────────────────────────────────────────────────────────────── */
TModuleHealthReport HealthValidator::checkGlobalSystem() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::GLOBAL_SYSTEM;
  auto& checks = *report.checks();

  /* 1.1.1 Thrift endpoint reachable
   * Use bgpd.process.uptime.seconds (set by Watchdog) as the daemon
   * liveness signal. Port 179 is checked by 1.4.1, port 6909 by 2.2.1,
   * so no need to duplicate TCP connect checks here. */
  {
    auto uptime = getCounter("bgpd.process.uptime.seconds");
    bool alive = uptime.has_value() && *uptime > 0;

    checks.emplace_back(makeResult(
        HealthCheckId::GLOBAL_SYSTEM_THRIFT_REACHABLE,
        HealthCheckCategory::GLOBAL_SYSTEM,
        alive ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
        alive ? fmt::format("daemon alive, uptime = {}s", *uptime)
              : "Daemon not running or uptime counter missing"));
  }

  /* 1.1.2 RSS memory under control
   * Compare current RSS against the limit passed via SystemResourceLimits. */
  {
    auto rss = getCounter("bgpd.process.memory.rss.bytes");
    int64_t limitBytes = 0;
    if (watchdog_) {
      limitBytes = watchdog_->getSystemResourceLimits().rssLimitBytes;
    }

    if (!rss.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_SYSTEM_RSS_MEMORY,
          HealthCheckCategory::GLOBAL_SYSTEM,
          HealthCheckStatus::FAIL,
          "Counter bgpd.process.memory.rss.bytes not found"));
    } else if (limitBytes <= 0) {
      double rssMb = static_cast<double>(*rss) / (1024.0 * 1024.0);

      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_SYSTEM_RSS_MEMORY,
          HealthCheckCategory::GLOBAL_SYSTEM,
          HealthCheckStatus::SKIPPED,
          fmt::format("rss = {:.0f}MB (RSS limit not configured)", rssMb)));
    } else {
      double pct =
          static_cast<double>(*rss) / static_cast<double>(limitBytes) * 100.0;
      double rssMb = static_cast<double>(*rss) / (1024.0 * 1024.0);
      double limitMb = static_cast<double>(limitBytes) / (1024.0 * 1024.0);

      HealthCheckStatus status = HealthCheckStatus::PASS;
      if (pct >= kMemoryCriticalPct) {
        status = HealthCheckStatus::FAIL;
      } else if (pct >= kMemoryWarningPct) {
        status = HealthCheckStatus::WARN;
      }

      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_SYSTEM_RSS_MEMORY,
          HealthCheckCategory::GLOBAL_SYSTEM,
          status,
          fmt::format(
              "rss = {:.0f}MB / {:.0f}MB ({:.1f}%)", rssMb, limitMb, pct),
          pct,
          kMemoryWarningPct));
    }
  }

  /* 1.1.3 CPU under control */
  {
    auto cpu = getCounter("bgpd.process.cpu.percent");
    if (!cpu.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_SYSTEM_CPU_USAGE,
          HealthCheckCategory::GLOBAL_SYSTEM,
          HealthCheckStatus::FAIL,
          "Counter bgpd.process.cpu.percent not found"));
    } else {
      double cpuPct = static_cast<double>(*cpu);

      HealthCheckStatus status = HealthCheckStatus::PASS;
      if (cpuPct >= kCpuCriticalPct) {
        status = HealthCheckStatus::FAIL;
      } else if (cpuPct >= kCpuWarningPct) {
        status = HealthCheckStatus::WARN;
      }

      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_SYSTEM_CPU_USAGE,
          HealthCheckCategory::GLOBAL_SYSTEM,
          status,
          fmt::format("cpu = {}%", *cpu),
          cpuPct,
          kCpuWarningPct));
    }
  }

  /* 1.1.8 Attribute dedup efficiency */
  {
    if (!peerMgr_) {
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_SYSTEM_ATTR_DEDUP,
          HealthCheckCategory::GLOBAL_SYSTEM,
          HealthCheckStatus::FAIL,
          "PeerManagerBase not available"));
    } else {
      auto stats = peerMgr_->getAttributeStats();
      int64_t totalAttrs = *stats.total_num_of_attributes();
      int64_t uniqueAttrs = *stats.total_unique_attributes();
      double ratio =
          totalAttrs > 0 ? static_cast<double>(uniqueAttrs) / totalAttrs : 0.0;

      /* Skip dedup check when total attributes is too small —
       * on small-scale boxes, dedup ratio is naturally high. */
      constexpr int64_t kMinAttrsForDedupCheck = 100;
      if (totalAttrs < kMinAttrsForDedupCheck) {
        checks.emplace_back(makeResult(
            HealthCheckId::GLOBAL_SYSTEM_ATTR_DEDUP,
            HealthCheckCategory::GLOBAL_SYSTEM,
            HealthCheckStatus::PASS,
            fmt::format(
                "unique_attrs/total_attrs = {}/{} = {:.3f} "
                "(below {} threshold, skipping dedup evaluation)",
                uniqueAttrs,
                totalAttrs,
                ratio,
                kMinAttrsForDedupCheck)));
      } else {
        HealthCheckStatus status = HealthCheckStatus::PASS;
        if (ratio >= kAttrDedupCriticalThreshold) {
          status = HealthCheckStatus::FAIL;
        }

        checks.emplace_back(makeResult(
            HealthCheckId::GLOBAL_SYSTEM_ATTR_DEDUP,
            HealthCheckCategory::GLOBAL_SYSTEM,
            status,
            fmt::format(
                "unique_attrs/total_attrs = {}/{} = {:.3f}",
                uniqueAttrs,
                totalAttrs,
                ratio),
            ratio,
            kAttrDedupCriticalThreshold));
      }
    }
  }

  /* 1.1.9 Previous exit was planned (no crash) */
  checks.emplace_back(checkPlannedExit());

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

/* ────────────────────────────────────────────────────────────────
 * GLOBAL_TASK_THREAD checks (doc 1.2.x)
 * ──────────────────────────────────────────────────────────────── */
TModuleHealthReport HealthValidator::checkGlobalTaskThread() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::GLOBAL_TASK_THREAD;
  auto& checks = *report.checks();

  auto uptime = getCounter("bgpd.process.uptime.seconds");

  /* 1.2.1 All module heartbeats alive
   * Compare current heartbeat counter against Watchdog's stored snapshot.
   * If heartbeat didn't advance while time did, the thread is stuck. */
  {
    if (!watchdog_) {
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_TASK_HEARTBEATS,
          HealthCheckCategory::GLOBAL_TASK_THREAD,
          HealthCheckStatus::FAIL,
          "Watchdog not available"));
    } else {
      auto snapshots = watchdog_->getHeartbeatSnapshots();
      if (snapshots.empty()) {
        checks.emplace_back(makeResult(
            HealthCheckId::GLOBAL_TASK_HEARTBEATS,
            HealthCheckCategory::GLOBAL_TASK_THREAD,
            HealthCheckStatus::PASS,
            "No snapshots yet (Watchdog just started)"));
      } else {
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();

        int64_t worstDrift = 0;
        std::string worstModule;
        int modulesChecked = 0;

        for (const auto& [moduleName, deq] : snapshots) {
          if (deq.empty()) {
            continue;
          }

          auto currentHb =
              getCounter(fmt::format("bgpd.heartbeat.{}", moduleName));
          if (!currentHb.has_value()) {
            continue;
          }

          const auto& oldest = deq.front();
          int64_t timeDelta = now - oldest.timestampSeconds;
          int64_t hbDelta = *currentHb - oldest.heartbeatValue;
          int64_t drift = std::abs(timeDelta - hbDelta);
          ++modulesChecked;

          if (drift > worstDrift) {
            worstDrift = drift;
            worstModule = moduleName;
          }
        }

        if (modulesChecked == 0) {
          checks.emplace_back(makeResult(
              HealthCheckId::GLOBAL_TASK_HEARTBEATS,
              HealthCheckCategory::GLOBAL_TASK_THREAD,
              HealthCheckStatus::FAIL,
              "Snapshots exist but no heartbeat counters found"));
        } else {
          HealthCheckStatus status = HealthCheckStatus::PASS;
          if (worstDrift >= kHeartbeatCriticalStallSeconds) {
            status = HealthCheckStatus::FAIL;
          } else if (worstDrift >= kHeartbeatWarningStallSeconds) {
            status = HealthCheckStatus::WARN;
          }

          checks.emplace_back(makeResult(
              HealthCheckId::GLOBAL_TASK_HEARTBEATS,
              HealthCheckCategory::GLOBAL_TASK_THREAD,
              status,
              status == HealthCheckStatus::PASS
                  ? fmt::format(
                        "all {} modules healthy, max drift = {}s",
                        modulesChecked,
                        worstDrift)
                  : fmt::format(
                        "module {} stalled, drift = {}s",
                        worstModule,
                        worstDrift),
              static_cast<double>(worstDrift),
              static_cast<double>(kHeartbeatWarningStallSeconds)));
        }
      }
    }
  }

  /* 1.2.2 Process uptime reasonable */
  {
    if (!uptime.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_TASK_UPTIME,
          HealthCheckCategory::GLOBAL_TASK_THREAD,
          HealthCheckStatus::FAIL,
          "Counter bgpd.process.uptime.seconds not found"));
    } else {
      bool passed = *uptime > kUptimeMinSeconds;
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_TASK_UPTIME,
          HealthCheckCategory::GLOBAL_TASK_THREAD,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("uptime = {}s", *uptime),
          static_cast<double>(*uptime),
          static_cast<double>(kUptimeMinSeconds)));
    }
  }

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

/* ────────────────────────────────────────────────────────────────
 * GLOBAL_CONVERGENCE checks (doc 1.3.x)
 * ──────────────────────────────────────────────────────────────── */
TModuleHealthReport HealthValidator::checkGlobalConvergence() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::GLOBAL_CONVERGENCE;
  auto& checks = *report.checks();

  auto initDurationMs = getCounter(
      fmt::format(
          kInitEventCounterFormat,
          apache::thrift::util::enumNameSafe(
              neteng::fboss::bgp::thrift::BgpInitializationEvent::
                  INITIALIZED)));

  /* 1.3.1 Reached INITIALIZED state */
  {
    bool initialized = initDurationMs.has_value() && *initDurationMs > 0;
    checks.emplace_back(makeResult(
        HealthCheckId::GLOBAL_CONVERGENCE_INITIALIZED,
        HealthCheckCategory::GLOBAL_CONVERGENCE,
        initialized ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
        initialized ? fmt::format("initialized in {}ms", *initDurationMs)
                    : "Not yet initialized"));
  }

  /* 1.3.2 Initialization time reasonable
   * Uses INITIALIZED duration (not bgpd.convergenceTimeMs which is unreliable).
   * TODO: kMaxConvergenceMs (5min) may need per-scale tuning —
   * small-scale devices (rsw/fsw) initialize much faster than large EB routers.
   */
  {
    if (!initDurationMs.has_value() || *initDurationMs <= 0) {
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_CONVERGENCE_TIME,
          HealthCheckCategory::GLOBAL_CONVERGENCE,
          HealthCheckStatus::SKIPPED,
          "Not yet initialized"));
    } else {
      bool passed = *initDurationMs <= kMaxConvergenceMs;
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_CONVERGENCE_TIME,
          HealthCheckCategory::GLOBAL_CONVERGENCE,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("initializationTime = {}ms", *initDurationMs),
          static_cast<double>(*initDurationMs),
          static_cast<double>(kMaxConvergenceMs)));
    }
  }

  /* 1.3.3 No initialization timeout */
  {
    auto timeout = getCounter("bgpd.initialized.timeout");
    if (!timeout.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_CONVERGENCE_INIT_TIMEOUT,
          HealthCheckCategory::GLOBAL_CONVERGENCE,
          HealthCheckStatus::FAIL,
          "Counter bgpd.initialized.timeout not found"));
    } else {
      bool passed = (*timeout == 0);
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_CONVERGENCE_INIT_TIMEOUT,
          HealthCheckCategory::GLOBAL_CONVERGENCE,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          passed ? "Converged naturally"
                 : "Initialization timed out (forced convergence)",
          static_cast<double>(*timeout),
          0.0));
    }
  }

  /* 1.3.4 EoR received from all peers
   * TODO: Expose a PeerManagerBase API to list unconverged
   * peers (those that haven't sent EoR) for richer diagnostics. */
  {
    auto eorReceived = getCounter(
        fmt::format(
            kInitEventCounterFormat,
            apache::thrift::util::enumNameSafe(
                neteng::fboss::bgp::thrift::BgpInitializationEvent::
                    ALL_EOR_RECEIVED)));
    auto eorTimerExpired = getCounter(
        fmt::format(
            kInitEventCounterFormat,
            apache::thrift::util::enumNameSafe(
                neteng::fboss::bgp::thrift::BgpInitializationEvent::
                    EOR_TIMER_EXPIRED)));

    bool eorOk = eorReceived.has_value();
    bool timerExpired = eorTimerExpired.has_value();

    HealthCheckStatus status = HealthCheckStatus::PASS;
    std::string msg;
    if (eorOk && !timerExpired) {
      msg = "All EoR received naturally";
    } else if (eorOk && timerExpired) {
      msg = "EoR received but timer also expired";
    } else if (!eorOk && timerExpired) {
      status = HealthCheckStatus::WARN;
      msg = "EoR timer expired before all peers sent EoR";
    } else {
      /* Neither set — still initializing or very early */
      msg = "EoR phase not yet reached";
    }

    checks.emplace_back(makeResult(
        HealthCheckId::GLOBAL_CONVERGENCE_EOR_RECEIVED,
        HealthCheckCategory::GLOBAL_CONVERGENCE,
        status,
        msg));
  }

  /* 1.3.5 All init phases completed in canonical order.
   *
   * The full BgpInitializationEvent sequence is validated in enum order, which
   * is its intended chronological order. Absent events are skipped rather than
   * treated as end-of-sequence: some phases are legitimately absent -- e.g.
   * FSDB_SUBSCRIBED fires only on the DC agent, ALL_EOR_RECEIVED and
   * EOR_TIMER_EXPIRED are alternative convergence triggers (typically only one
   * occurs), and any tail event is missing while initialization is still in
   * progress. Only events that actually fired are compared pairwise, so ANY
   * out-of-order timestamp is flagged generically -- including an
   * ALL_EOR_RECEIVED that lands after the EoR timer forced convergence (and
   * after INITIALIZED).
   *
   * An inversion is FAIL, not WARN: it means the daemon marked INITIALIZED and
   * programmed FIB before a peer's routes were in, then that peer's EoR arrived
   * late -- a real convergence-correctness violation. A clean forced
   * convergence (EoR timer fired, no peer ever caught up) produces no inversion
   * and stays PASS here; that softer signal is surfaced as WARN by the
   * INIT_TIMEOUT / EOR_RECEIVED checks instead. */
  {
    using BgpInitEvent = neteng::fboss::bgp::thrift::BgpInitializationEvent;
    std::vector<BgpInitEvent> expectedOrder = {
        BgpInitEvent::INITIALIZING,
        BgpInitEvent::AGENT_CONFIGURED,
        BgpInitEvent::FSDB_SUBSCRIBED,
        BgpInitEvent::PEER_INFO_LOADED,
        BgpInitEvent::ALL_EOR_RECEIVED,
        BgpInitEvent::EOR_TIMER_EXPIRED,
        BgpInitEvent::RIB_COMPUTED,
        BgpInitEvent::FIB_SYNCED,
        BgpInitEvent::EOR_SENT,
        BgpInitEvent::INITIALIZED,
    };

    bool ordered = true;
    int64_t prevTime = -1;
    std::optional<BgpInitEvent> prevEvent;
    std::string detail;
    for (const auto& event : expectedOrder) {
      auto counter = getCounter(
          fmt::format(
              kInitEventCounterFormat,
              apache::thrift::util::enumNameSafe(event)));
      if (!counter.has_value()) {
        /* Event absent (skipped phase, alternate EoR trigger, or init still in
         * progress) -- skip it and keep validating the events that did fire. */
        continue;
      }
      if (*counter < prevTime) {
        ordered = false;
        detail = fmt::format(
            "out-of-order: {} ({}ms) occurred after {} ({}ms)",
            apache::thrift::util::enumNameSafe(*prevEvent),
            prevTime,
            apache::thrift::util::enumNameSafe(event),
            *counter);
        break;
      }
      prevTime = *counter;
      prevEvent = event;
    }

    std::string initPhasesMsg = ordered
        ? "All init phases monotonically increasing"
        : std::move(detail);
    checks.emplace_back(makeResult(
        HealthCheckId::GLOBAL_CONVERGENCE_INIT_PHASES,
        HealthCheckCategory::GLOBAL_CONVERGENCE,
        ordered ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
        initPhasesMsg));
  }

  /* 1.3.6 No safe mode active */
  {
    if (!peerMgr_) {
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_CONVERGENCE_SAFE_MODE,
          HealthCheckCategory::GLOBAL_CONVERGENCE,
          HealthCheckStatus::FAIL,
          "PeerManagerBase not available"));
    } else {
      bool safeModeOn = peerMgr_->getIsSafeModeOn();
      checks.emplace_back(makeResult(
          HealthCheckId::GLOBAL_CONVERGENCE_SAFE_MODE,
          HealthCheckCategory::GLOBAL_CONVERGENCE,
          safeModeOn ? HealthCheckStatus::WARN : HealthCheckStatus::PASS,
          safeModeOn ? "Safe mode is ON (accepting only golden prefixes)"
                     : "Safe mode off"));
    }
  }

  /* 1.3.8 No peers stuck in handshake (>30s)
   * Counter-based proxy (configured - running) is unreliable:
   * dynamic peers inflate runningSessions beyond configuredPeers,
   * and admin-down peers are configured but intentionally not running.
   * Accurate check requires per-peer state + duration from getBgpSessions().
   * Deferred to MEDIUM complexity with per-peer iteration. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::GLOBAL_CONVERGENCE_STUCK_HANDSHAKE,
        HealthCheckCategory::GLOBAL_CONVERGENCE,
        HealthCheckStatus::SKIPPED,
        "Requires per-peer state iteration (MEDIUM complexity)"));
  }

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

/* ────────────────────────────────────────────────────────────────
 * SESSION_MANAGER checks (doc 1.4.x)
 * ──────────────────────────────────────────────────────────────── */
TModuleHealthReport HealthValidator::checkSessionManager() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::SESSION_MANAGER;
  auto& checks = *report.checks();

  auto runningSessions = getCounter("bgpd.runningSessions");

  /* 1.4.1 Listening on port 179
   * Directly verify the BGP port is bound by attempting a TCP connect
   * to localhost:179 with a short timeout. */
  {
    bool listening = false;
    auto sock = folly::File(
        ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0), true /* ownsFd */);
    if (sock.fd() >= 0) {
      struct sockaddr_in6 addr{};
      addr.sin6_family = AF_INET6;
      addr.sin6_port = htons(nettools::bgplib::constants::kBgpPort);
      addr.sin6_addr = in6addr_loopback;

      int rc = ::connect(sock.fd(), (struct sockaddr*)&addr, sizeof(addr));
      if (rc == 0 || errno == EINPROGRESS) {
        listening = true;
      }
    }

    checks.emplace_back(makeResult(
        HealthCheckId::SESSION_PORT_179,
        HealthCheckCategory::SESSION_MANAGER,
        listening ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
        listening ? "Port 179 is listening" : "Port 179 is not reachable"));
  }

  /* 1.4.2 At least one peer ESTABLISHED */
  {
    if (!runningSessions.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::SESSION_ESTABLISHED,
          HealthCheckCategory::SESSION_MANAGER,
          HealthCheckStatus::FAIL,
          "Counter bgpd.runningSessions not found"));
    } else {
      bool passed = (*runningSessions > 0);
      checks.emplace_back(makeResult(
          HealthCheckId::SESSION_ESTABLISHED,
          HealthCheckCategory::SESSION_MANAGER,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
          fmt::format("runningSessions = {}", *runningSessions),
          static_cast<double>(*runningSessions),
          1.0));
    }
  }

  /* 1.4.4 No recent session flaps
   * bgpd.sessionStateChanges is a fb303::SUM timeseries.
   * Use .sum.600 (10min window). FAIL if flaps exceed
   * kSessionFlapThresholdPct of total running sessions. */
  {
    auto flaps = getCounter("bgpd.sessionStateChanges.sum.600");
    if (!flaps.has_value() || !runningSessions.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::SESSION_FLAPS,
          HealthCheckCategory::SESSION_MANAGER,
          HealthCheckStatus::FAIL,
          "Counter bgpd.sessionStateChanges.sum.600 or "
          "bgpd.runningSessions not found"));
    } else {
      double threshold =
          static_cast<double>(*runningSessions) * kSessionFlapThresholdPct;
      bool passed = static_cast<double>(*flaps) <= threshold;
      checks.emplace_back(makeResult(
          HealthCheckId::SESSION_FLAPS,
          HealthCheckCategory::SESSION_MANAGER,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format(
              "sessionFlaps(10min) = {} (threshold = {:.0f}, "
              "{:.0f}% of {} sessions)",
              *flaps,
              threshold,
              kSessionFlapThresholdPct * 100,
              *runningSessions),
          static_cast<double>(*flaps),
          threshold));
    }
  }

  /* TODO: 1.4.5 No hold timer expirations
   * Global peer.totalHoldTimerExpiry is too broad — need per-peer check.
   * Iterate getBgpSessions() to find which peer(s) experienced hold timer
   * expiry and report their names. Deferred to MEDIUM complexity. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::SESSION_HOLD_TIMER_EXPIRY,
        HealthCheckCategory::SESSION_MANAGER,
        HealthCheckStatus::SKIPPED,
        "Per-peer hold timer check deferred (needs session iteration)"));
  }

  /* 1.4.6 Queue sizes normal
   * Check both per-peer queues (under peer_manager) and shared queues
   * (ribInQ_, ribOutQ_ under rib). Use min(kMaxIngressQueueSize,
   * kMaxEgressQueueSize) from BgpStructs.h as threshold. */
  {
    if (!watchdog_) {
      checks.emplace_back(makeResult(
          HealthCheckId::SESSION_PEER_QUEUE,
          HealthCheckCategory::SESSION_MANAGER,
          HealthCheckStatus::FAIL,
          "Watchdog not available"));
    } else {
      auto queueThreshold = std::min(
          nettools::bgplib::kMaxIngressQueueSize,
          nettools::bgplib::kMaxEgressQueueSize);
      auto paths = std::make_unique<MonitoredPathT>(
          MonitoredPathT{kModulePeerManager, kModuleRib});
      auto queueSizes = watchdog_->getQueueSizes(paths);

      int overThreshold = 0;
      std::string worstQueue;
      int32_t worstSize = 0;
      for (const auto& [name, size] : queueSizes) {
        if (size > static_cast<int32_t>(queueThreshold)) {
          ++overThreshold;
          if (size > worstSize) {
            worstSize = size;
            worstQueue = name;
          }
        }
      }

      bool passed = (overThreshold == 0);
      checks.emplace_back(makeResult(
          HealthCheckId::SESSION_PEER_QUEUE,
          HealthCheckCategory::SESSION_MANAGER,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          passed ? fmt::format(
                       "all {} queues within threshold ({})",
                       queueSizes.size(),
                       queueThreshold)
                 : fmt::format(
                       "{} queues over threshold (worst: {} = {}, limit = {})",
                       overThreshold,
                       worstQueue,
                       worstSize,
                       queueThreshold),
          passed ? std::optional<double>(std::nullopt)
                 : std::optional<double>(static_cast<double>(worstSize)),
          static_cast<double>(queueThreshold)));
    }
  }

  /* 1.4.7 Collision events under threshold
   * No global collision counter exists; collisions are tracked per-peer
   * with platform-specific prefixes. Deferred to MEDIUM complexity. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::SESSION_COLLISIONS,
        HealthCheckCategory::SESSION_MANAGER,
        HealthCheckStatus::SKIPPED,
        "Per-peer collision check deferred (MEDIUM complexity)"));
  }

  /* TODO: 1.4.8 No notification messages sent/received
   * Global peer.messagesSent.notification is a lifetime counter that
   * accumulates across session flaps. A past notification from a
   * resolved session should not trigger a current health failure.
   * Need rate-based check (.rate.60) or per-session counter reset. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::SESSION_NOTIFICATIONS,
        HealthCheckCategory::SESSION_MANAGER,
        HealthCheckStatus::SKIPPED,
        "Lifetime counter not useful; needs rate-based check"));
  }

  /* 1.4.9 Socket bytes flowing
   * Socket byte counters are per-peer timeseries with platform-specific
   * prefixes. Accurate check requires per-peer iteration.
   * Deferred to MEDIUM complexity with per-peer analysis. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::SESSION_SOCKET_BYTES,
        HealthCheckCategory::SESSION_MANAGER,
        HealthCheckStatus::SKIPPED,
        "Per-peer socket byte check deferred (MEDIUM complexity)"));
  }

  /* TODO: 1.4.10 Keepalive messages flowing
   * Lifetime keepAlive count has no useful signal without a delta.
   * Need rate-based check (e.g. peer.messagesRecv.keepAlive.rate.60 > 0)
   * or per-peer windowed average. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::SESSION_KEEPALIVES,
        HealthCheckCategory::SESSION_MANAGER,
        HealthCheckStatus::SKIPPED,
        "Lifetime counter not useful; needs rate-based check"));
  }

  /* TODO: 1.4.11 No socket errors
   * Same issue: peer.messagesSent.socketFailure is a lifetime counter.
   * Historical socket errors from past sessions are not actionable.
   * Need rate-based check or per-session scoped counter. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::SESSION_SOCKET_ERRORS,
        HealthCheckCategory::SESSION_MANAGER,
        HealthCheckStatus::SKIPPED,
        "Lifetime counter not useful; needs rate-based check"));
  }

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

/* ────────────────────────────────────────────────────────────────
 * Stub implementations for categories completed in later diffs
 * ──────────────────────────────────────────────────────────────── */

/* ────────────────────────────────────────────────────────────────
 * PEER_MANAGER checks (doc 1.5.x)
 * ──────────────────────────────────────────────────────────────── */
THealthCheckResult HealthValidator::evaluatePrefixLimitDrops(
    const std::vector<neteng::fboss::bgp::thrift::TBgpSession>& sessions) {
  constexpr size_t kMaxListedPeers = 10;
  std::vector<std::string> affected;
  for (const auto& session : sessions) {
    const int64_t preDropped =
        session.prepolicy_rcvd_dropped_prefix_count().value_or(0);
    if (preDropped > 0) {
      affected.emplace_back(
          fmt::format("{} (PR dropped {})", *session.peer_addr(), preDropped));
    }
  }

  THealthCheckResult result;
  result.checkId() = HealthCheckId::PEER_PREFIX_LIMIT_DROPS;
  result.category() = HealthCheckCategory::PEER_MANAGER;
  result.observedValue() = static_cast<double>(affected.size());
  result.threshold() = 0.0;
  result.timestampMs() =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  if (affected.empty()) {
    result.status() = HealthCheckStatus::PASS;
    result.message() = "No peers dropping routes due to per-peer prefix limit";
    return result;
  }

  std::string listed;
  for (size_t i = 0; i < std::min(affected.size(), kMaxListedPeers); ++i) {
    if (i != 0) {
      listed += ", ";
    }
    listed += affected[i];
  }
  if (affected.size() > kMaxListedPeers) {
    listed += fmt::format(", +{} more", affected.size() - kMaxListedPeers);
  }
  result.status() = HealthCheckStatus::WARN;
  result.message() = fmt::format(
      "{} peer(s) dropping routes (per-peer prefix limit): {}",
      affected.size(),
      listed);
  return result;
}

folly::coro::Task<TModuleHealthReport> HealthValidator::checkPeerManager() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::PEER_MANAGER;
  auto& checks = *report.checks();

  /* 1.5.1 Number of peers with zero route exchange
   * Counts ESTABLISHED peers where both preInPrefixCount and
   * postOutPrefixCount are 0 (excludes dynamic/VIP peers). */
  {
    auto val = getCounter("peer.totalPeerWithNoRouteExchange");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_ZERO_ROUTES,
          HealthCheckCategory::PEER_MANAGER,
          HealthCheckStatus::FAIL,
          "Counter peer.totalPeerWithNoRouteExchange not found"));
    } else {
      bool passed = (*val == 0);
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_ZERO_ROUTES,
          HealthCheckCategory::PEER_MANAGER,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("peersWithNoRouteExchange = {}", *val),
          static_cast<double>(*val),
          0.0));
    }
  }

  /* 1.5.2 No slow peers (DETACHED_BLOCKED)
   * Use .sum.600 (10min window) — absolute count is lifetime and
   * not useful for current health. */
  {
    auto val = getCounter("bgpcpp.slow_peer_detection_count.sum.600");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_SLOW_DETACHED,
          HealthCheckCategory::PEER_MANAGER,
          HealthCheckStatus::FAIL,
          "Counter bgpcpp.slow_peer_detection_count.sum.600 not found"));
    } else {
      bool passed = (*val == 0);
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_SLOW_DETACHED,
          HealthCheckCategory::PEER_MANAGER,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("slowPeerDetections(10min) = {}", *val),
          static_cast<double>(*val),
          0.0));
    }
  }

  /* 1.5.3 No dropped prefixes (capacity/overload protection)
   * Counts prefixes dropped by per-switch overload protection
   * (max path/prefix limits), per-peer route caps, or golden prefix
   * policy purge. NOT normal policy rejections. The counter is
   * initialized to 0 and incremented on every such drop, so it is
   * always populated: 0 means no drops, >0 means actual drops. */
  {
    auto val = getCounter("peer.totalDroppedPrefixes");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_DROPPED_PREFIXES,
          HealthCheckCategory::PEER_MANAGER,
          HealthCheckStatus::FAIL,
          "Counter peer.totalDroppedPrefixes not found"));
    } else {
      bool passed = (*val <= 0);
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_DROPPED_PREFIXES,
          HealthCheckCategory::PEER_MANAGER,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("droppedPrefixes = {}", *val),
          static_cast<double>(std::max(0L, *val)),
          0.0));
    }
  }

  /* 1.5.4 No thrift rejects/suspends
   * thriftReject: request rejected by continueExecution() after timeout.
   * thriftSuspend: request suspended (100ms sleep) due to overload.
   * Both are lifetime flat counters from BgpServiceBase. */
  {
    auto rejects = getCounter("bgpd.thriftReject");
    auto suspends = getCounter("bgpd.thriftSuspend");
    int64_t total = rejects.value_or(0) + suspends.value_or(0);
    bool passed = (total == 0);
    checks.emplace_back(makeResult(
        HealthCheckId::PEER_THRIFT_REJECTS,
        HealthCheckCategory::PEER_MANAGER,
        passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
        fmt::format(
            "rejects={} suspends={}",
            rejects.value_or(0),
            suspends.value_or(0)),
        static_cast<double>(total),
        0.0));
  }

  /* 1.5.5 Per-peer received prefix count non-zero
   * Effectively covered by 1.5.1 (totalPeerWithNoRouteExchange):
   * that counter already identifies ESTABLISHED peers with both
   * preInPrefixCount and postOutPrefixCount == 0.
   *
   * TODO: A per-peer breakdown would require
   * getBgpSessions() via coroutine context. A follow-up diff can
   * transform check methods to coro-based for full per-peer data. */
  {
    auto noExchange = getCounter("peer.totalPeerWithNoRouteExchange");
    auto sessions = getCounter("bgpd.runningSessions");
    if (!noExchange.has_value() || !sessions.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_PREFIX_COUNT,
          HealthCheckCategory::PEER_MANAGER,
          HealthCheckStatus::FAIL,
          "Counters not available"));
    } else {
      bool passed = (*noExchange == 0 && *sessions > 0);
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_PREFIX_COUNT,
          HealthCheckCategory::PEER_MANAGER,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format(
              "runningSessions={} peersWithNoExchange={}",
              *sessions,
              *noExchange),
          static_cast<double>(*noExchange),
          0.0));
    }
  }

  /* 1.5.6 Policy cache functioning (hit rate >50%) */
  {
    auto hits = getCounter("bgpd.policy_cache.hit");
    auto misses = getCounter("bgpd.policy_cache.miss");
    if (!hits.has_value() && !misses.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_POLICY_CACHE,
          HealthCheckCategory::PEER_MANAGER,
          HealthCheckStatus::FAIL,
          "Policy cache counters not found"));
    } else {
      int64_t h = hits.value_or(0);
      int64_t m = misses.value_or(0);
      int64_t total = h + m;
      double hitRate = total > 0 ? static_cast<double>(h) / total : 1.0;
      bool passed = hitRate >= 0.5;
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_POLICY_CACHE,
          HealthCheckCategory::PEER_MANAGER,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("hit={} miss={} rate={:.2f}", h, m, hitRate),
          hitRate,
          0.5));
    }
  }

  /* TODO: 1.5.7 Per-peer ribVersion consistent with global
   * Each peer's rib_version (from TBgpSession) should be within
   * kRibVersionLagThreshold of bgpcpp.rib.tableVersion. A large delta
   * means the peer is falling behind on RIB updates.
   *
   * Deferred: getSessionInfos() requires coroutine context
   * (co_getAllPeerDisplayInfos from SessionManager). A follow-up diff
   * can transform HealthValidator check methods to coro-based,
   * enabling direct access to getBgpSessions() and per-peer data. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::PEER_RIB_VERSION,
        HealthCheckCategory::PEER_MANAGER,
        HealthCheckStatus::SKIPPED,
        fmt::format(
            "Per-peer ribVersion check deferred "
            "(threshold={}, needs coroutine context)",
            kRibVersionLagThreshold)));
  }

  /* 1.5.8 Per-peer prefix-limit drops
   * Reports which peers are actively dropping received (PR) routes because they
   * reached their configured per-peer prefix limit, with exact per-peer counts
   * (reused from TBgpSession). Peer enumeration runs on the SessionManager evb
   * and the session lookup on the PeerManagerBase evb; both are bounded by
   * co_runOnEvbWithTimeout so a stuck evb fails the check instead of hanging
   * generateReport(). */
  {
    if (!peerMgr_) {
      checks.emplace_back(makeResult(
          HealthCheckId::PEER_PREFIX_LIMIT_DROPS,
          HealthCheckCategory::PEER_MANAGER,
          HealthCheckStatus::SKIPPED,
          "PeerManagerBase not available"));
    } else {
      /* co_getAllPeerDisplayInfos() hops onto the SessionManager evb without a
       * timeout, so call the synchronous accessor under the same bounded wait
       * used for getSessionInfos below. */
      auto sessionMgr = peerMgr_->getSessionManager();
      auto peersResult = co_await co_runOnEvbWithTimeout(
          sessionMgr->getEventBase(),
          [sessionMgr]() { return sessionMgr->getAllPeerDisplayInfos(); },
          kHealthCheckModuleTimeout);
      if (peersResult.hasException()) {
        checks.emplace_back(makeResult(
            HealthCheckId::PEER_PREFIX_LIMIT_DROPS,
            HealthCheckCategory::PEER_MANAGER,
            HealthCheckStatus::FAIL,
            peersResult.exception().is_compatible_with<folly::FutureTimeout>()
                ? fmt::format(
                      "SessionManager evb unresponsive (timeout after {}s)",
                      kHealthCheckModuleTimeout.count())
                : fmt::format(
                      "getAllPeerDisplayInfos() failed: {}",
                      peersResult.exception().what())));
      } else {
        auto result = co_await co_runOnEvbWithTimeout(
            peerMgr_->getEventBase(),
            [this, peers = std::move(peersResult.value())]() {
              return peerMgr_->getSessionInfos(peers);
            },
            kHealthCheckModuleTimeout);
        if (result.hasValue()) {
          checks.emplace_back(evaluatePrefixLimitDrops(result.value()));
        } else if (result.exception()
                       .is_compatible_with<folly::FutureTimeout>()) {
          checks.emplace_back(makeResult(
              HealthCheckId::PEER_PREFIX_LIMIT_DROPS,
              HealthCheckCategory::PEER_MANAGER,
              HealthCheckStatus::FAIL,
              fmt::format(
                  "PeerManagerBase evb unresponsive (timeout after {}s)",
                  kHealthCheckModuleTimeout.count())));
        } else {
          checks.emplace_back(makeResult(
              HealthCheckId::PEER_PREFIX_LIMIT_DROPS,
              HealthCheckCategory::PEER_MANAGER,
              HealthCheckStatus::FAIL,
              fmt::format(
                  "getSessionInfos() failed: {}", result.exception().what())));
        }
      }
    }
  }

  report.overallStatus() = computeOverallStatus(checks);
  co_return report;
}

/* ────────────────────────────────────────────────────────────────
 * RIB checks (doc 1.6.x)
 * ──────────────────────────────────────────────────────────────── */
folly::coro::Task<TModuleHealthReport> HealthValidator::checkRib() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::RIB;
  auto& checks = *report.checks();

  /* 1.6.1 Local routes originated correctly
   * getOriginatedRoutes() uses runInEventBaseThreadAndWait which hangs
   * if Rib's evb is stuck. Use co_runOnEvbWithTimeout to cap wait time. */
  {
    if (!rib_) {
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_ORIGINATED_ROUTES,
          HealthCheckCategory::RIB,
          HealthCheckStatus::FAIL,
          "Rib not available"));
    } else {
      auto result = co_await co_runOnEvbWithTimeout(
          rib_->getEventBase(),
          [this]() { return rib_->getOriginatedRoutes(); },
          kHealthCheckModuleTimeout);

      if (result.hasValue()) {
        int64_t count = static_cast<int64_t>(result.value().size());
        bool passed = (count > 0);
        checks.emplace_back(makeResult(
            HealthCheckId::RIB_ORIGINATED_ROUTES,
            HealthCheckCategory::RIB,
            passed ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
            fmt::format("originatedRoutes = {}", count),
            static_cast<double>(count)));
      } else if (result.exception()
                     .is_compatible_with<folly::FutureTimeout>()) {
        checks.emplace_back(makeResult(
            HealthCheckId::RIB_MODULE_RESPONSIVENESS,
            HealthCheckCategory::RIB,
            HealthCheckStatus::FAIL,
            fmt::format(
                "Rib evb unresponsive (timeout after {}s)",
                kHealthCheckModuleTimeout.count())));
      } else {
        checks.emplace_back(makeResult(
            HealthCheckId::RIB_ORIGINATED_ROUTES,
            HealthCheckCategory::RIB,
            HealthCheckStatus::FAIL,
            fmt::format(
                "getOriginatedRoutes() failed: {}",
                result.exception().what())));
      }
    }
  }

  /* 1.6.2 Path selection duration < 5s
   * Uses the p99 quantile stat to catch tail latency spikes.
   * DEFINE_quantile_stat exports: .avg.60, .p50.60, .p99.60, etc. */
  {
    auto val = getCounter("bgpd.rib.fullSyncPathSelectionTimeMs.p99.60");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_PATH_SELECTION_DURATION,
          HealthCheckCategory::RIB,
          HealthCheckStatus::SKIPPED,
          "Counter bgpd.rib.fullSyncPathSelectionTimeMs.p99.60 not found "
          "(no path selection has run in the last 60s)"));
    } else {
      bool passed = (*val < kMaxPathSelectionMs);
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_PATH_SELECTION_DURATION,
          HealthCheckCategory::RIB,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("pathSelectionMs = {}", *val),
          static_cast<double>(*val),
          static_cast<double>(kMaxPathSelectionMs)));
    }
  }

  /* 1.6.3 Non-zero received routes */
  {
    auto totalPaths = getCounter("bgpd.rib.totalRibPaths");
    auto originated = getCounter("bgpd.rib.totalOriginatedRoutes");
    if (!totalPaths.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_RECEIVED_ROUTES,
          HealthCheckCategory::RIB,
          HealthCheckStatus::FAIL,
          "Counter bgpd.rib.totalRibPaths not found"));
    } else {
      int64_t received = *totalPaths - originated.value_or(0);
      bool passed = (received > 0);
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_RECEIVED_ROUTES,
          HealthCheckCategory::RIB,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
          fmt::format(
              "receivedRoutes = {} (total={} - originated={})",
              received,
              *totalPaths,
              originated.value_or(0)),
          static_cast<double>(received)));
    }
  }

  /* 1.6.4 RIB table version non-zero */
  {
    auto ver = getCounter("bgpcpp.rib.tableVersion");
    if (!ver.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_TABLE_VERSION,
          HealthCheckCategory::RIB,
          HealthCheckStatus::FAIL,
          "Counter bgpcpp.rib.tableVersion not found"));
    } else {
      bool passed = (*ver > 0);
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_TABLE_VERSION,
          HealthCheckCategory::RIB,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
          fmt::format("ribVersion = {}", *ver),
          static_cast<double>(*ver)));
    }
  }

  /* 1.6.6 Prefix/path usage within configured switch limits
   * Compare actual unique prefixes and total paths against the
   * limits from switch_limit_config. */
  {
    auto uniquePrefixes = getCounter("peer.totalUniquePrefixes");
    auto totalPaths = getCounter("peer.totalPaths");
    auto prefixLimit = getCounter("bgpd.unique_prefix_limit");
    auto pathLimit = getCounter("bgpd.total_path_limit");

    bool hasLimits = (prefixLimit.has_value() && *prefixLimit > 0) ||
        (pathLimit.has_value() && *pathLimit > 0);

    if (!hasLimits) {
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_OVERLOAD_MODE,
          HealthCheckCategory::RIB,
          HealthCheckStatus::SKIPPED,
          "No switch limits configured"));
    } else {
      double worstPct = 0.0;
      std::string worstDetail;

      if (prefixLimit.has_value() && *prefixLimit > 0 &&
          uniquePrefixes.has_value()) {
        double pct = static_cast<double>(*uniquePrefixes) /
            static_cast<double>(*prefixLimit) * 100.0;
        if (pct > worstPct) {
          worstPct = pct;
          worstDetail = fmt::format(
              "prefixes = {} / {} ({:.1f}%)",
              *uniquePrefixes,
              *prefixLimit,
              pct);
        }
      }

      if (pathLimit.has_value() && *pathLimit > 0 && totalPaths.has_value()) {
        double pct = static_cast<double>(*totalPaths) /
            static_cast<double>(*pathLimit) * 100.0;
        if (pct > worstPct) {
          worstPct = pct;
          worstDetail = fmt::format(
              "paths = {} / {} ({:.1f}%)", *totalPaths, *pathLimit, pct);
        }
      }

      HealthCheckStatus status = HealthCheckStatus::PASS;
      if (worstPct >= kSwitchLimitCriticalPct) {
        status = HealthCheckStatus::FAIL;
      } else if (worstPct >= kSwitchLimitWarningPct) {
        status = HealthCheckStatus::WARN;
      }

      if (worstDetail.empty()) {
        worstDetail = "Switch limit counters not found";
      }
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_OVERLOAD_MODE,
          HealthCheckCategory::RIB,
          status,
          worstDetail,
          worstPct,
          100.0));
    }
  }

  /* 1.6.7 FIB routes and ShadowRIB entries consistent
   * Compare fib.totalUcastRoutes (FIB route count) vs
   * bgpd.rib.totalShadowRibEntries (shadow RIB entries).
   * Both represent prefix/route counts, not per-peer paths. */
  {
    auto fibRoutes = getCounter("fib.totalUcastRoutes");
    auto shadowEntries = getCounter("bgpd.rib.totalShadowRibEntries");
    if (!fibRoutes.has_value() || !shadowEntries.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_SHADOW_CONSISTENT,
          HealthCheckCategory::RIB,
          HealthCheckStatus::FAIL,
          "FIB/ShadowRIB counters not found"));
    } else {
      int64_t maxVal = std::max(*fibRoutes, *shadowEntries);
      int64_t delta = std::abs(*fibRoutes - *shadowEntries);
      double divergencePct =
          maxVal > 0 ? static_cast<double>(delta) / maxVal : 0.0;
      bool passed = (divergencePct <= kRibShadowMaxDivergencePct);
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_SHADOW_CONSISTENT,
          HealthCheckCategory::RIB,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format(
              "ribRoutes={} shadowEntries={} divergence={:.1f}%",
              *fibRoutes,
              *shadowEntries,
              divergencePct * 100),
          divergencePct,
          kRibShadowMaxDivergencePct));
    }
  }

  /* 1.6.8 Best path + FIB programming pauses not excessive */
  {
    auto val =
        getCounter("bgpd.rib.bestPathAndFibProgrammingPauseTimeMs.p99.60");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_PAUSE_TIME,
          HealthCheckCategory::RIB,
          HealthCheckStatus::FAIL,
          "Quantile stat bestPathAndFibProgrammingPauseTimeMs not found"));
    } else {
      bool passed = (*val < kMaxBpFibPauseMs);
      checks.emplace_back(makeResult(
          HealthCheckId::RIB_PAUSE_TIME,
          HealthCheckCategory::RIB,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("p99PauseMs = {}", *val),
          static_cast<double>(*val),
          static_cast<double>(kMaxBpFibPauseMs)));
    }
  }

  report.overallStatus() = computeOverallStatus(checks);
  co_return report;
}

/* ────────────────────────────────────────────────────────────────
 * NETLINK_WRAPPER checks (doc 1.7.x, EBB only)
 *
 * Base implementation handles only the platform-agnostic branches.
 * The real NetlinkWrapper-backed check lives in HealthValidatorBB
 * so that DC/OSS binaries have no NetlinkWrapper compile-time dep.
 * ──────────────────────────────────────────────────────────────── */
TModuleHealthReport HealthValidator::checkNetlinkWrapper() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::NETLINK_WRAPPER;
  auto& checks = *report.checks();

  /* 1.7.1 Tracked interfaces > 0
   * Use config (enable_next_hop_tracking) as SoT for whether
   * NetlinkWrapper should be active. If NHT is configured on a
   * platform that does not provide a NetlinkWrapper (DC/OSS), that
   * is a configuration mistake. */
  bool nhtEnabled = false;
  if (configManager_) {
    auto config = configManager_->getConfig();
    nhtEnabled = config && config->getBgpGlobalConfig() &&
        config->getBgpGlobalConfig()->enableNextHopTracking;
  }

  if (!nhtEnabled) {
    checks.emplace_back(makeResult(
        HealthCheckId::NETLINK_TRACKED_INTERFACES,
        HealthCheckCategory::NETLINK_WRAPPER,
        HealthCheckStatus::SKIPPED,
        "NetlinkWrapper not enabled in config"));
  } else {
    checks.emplace_back(makeResult(
        HealthCheckId::NETLINK_TRACKED_INTERFACES,
        HealthCheckCategory::NETLINK_WRAPPER,
        HealthCheckStatus::FAIL,
        "Config enables NHT but NetlinkWrapper not available on this platform"));
  }

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

/* ────────────────────────────────────────────────────────────────
 * NEXTHOP_TRACKER checks (doc 1.8.x)
 * ──────────────────────────────────────────────────────────────── */
TModuleHealthReport HealthValidator::checkNexthopTracker() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::NEXTHOP_TRACKER;
  auto& checks = *report.checks();

  bool nhtEnabled = false;
  if (configManager_) {
    auto config = configManager_->getConfig();
    nhtEnabled = config && config->getBgpGlobalConfig() &&
        config->getBgpGlobalConfig()->enableNextHopTracking;
  }

  /* Number of nexthops the RIB currently tracks. RibStats keeps this counter
   * in lockstep with nexthopInfoMap_ (incremented on the route-install and
   * nexthop-update paths, decremented on removal), so a value > 0 means the
   * NHT pipeline is live and tracking at least one nexthop. */
  auto nexthopInfoCount = getCounter(RibStats::kNexthopInfoCount);
  bool runtimeActive = nexthopInfoCount.value_or(0) > 0;

  /* 1.8.1 NHT feature state consistent with config
   * Use config enable_next_hop_tracking as SoT. Runtime activity is derived
   * from the bgpcpp.rib.nexthop_info.count counter. */
  {
    bool consistent = (nhtEnabled == runtimeActive) || !nhtEnabled;
    checks.emplace_back(makeResult(
        HealthCheckId::NHT_CONFIG_CONSISTENT,
        HealthCheckCategory::NEXTHOP_TRACKER,
        consistent ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
        fmt::format(
            "config={} runtime={}",
            nhtEnabled ? "enabled" : "disabled",
            runtimeActive ? "active" : "inactive")));
  }

  /* 1.8.5 NexthopCache populated
   * Only meaningful if NHT is enabled in config. Uses the
   * bgpcpp.rib.nexthop_info.count counter to check that the RIB is tracking
   * at least one nexthop. */
  {
    if (!nhtEnabled) {
      checks.emplace_back(makeResult(
          HealthCheckId::NHT_CACHE_NOT_EMPTY,
          HealthCheckCategory::NEXTHOP_TRACKER,
          HealthCheckStatus::SKIPPED,
          "NHT not enabled in config"));
    } else {
      checks.emplace_back(makeResult(
          HealthCheckId::NHT_CACHE_NOT_EMPTY,
          HealthCheckCategory::NEXTHOP_TRACKER,
          runtimeActive ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format(
              "nexthop_info.count = {}", nexthopInfoCount.value_or(0))));
    }
  }

  /* TODO: 1.8.2 NexthopHandler stream connected
   * MEDIUM complexity — need to expose NexthopHandler's thrift stream
   * connection status. nexthopHandler_ pointer added for future use. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::NHT_STREAM_CONNECTED,
        HealthCheckCategory::NEXTHOP_TRACKER,
        HealthCheckStatus::SKIPPED,
        "NexthopHandler stream status not exposed yet (MEDIUM)"));
  }

  /* TODO: 1.8.3 No unreachable nexthops for best paths
   * MEDIUM complexity — need to cross-reference nexthop reachability
   * with best path usage in RIB. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::NHT_UNREACHABLE_NEXTHOPS,
        HealthCheckCategory::NEXTHOP_TRACKER,
        HealthCheckStatus::SKIPPED,
        "Nexthop reachability cross-reference not implemented (MEDIUM)"));
  }

  /* TODO: 1.8.4 FSDB subscriptions healthy (DC only)
   * MEDIUM complexity — need to expose FsdbFibWatcher and
   * FsdbNeighborWatcher subscription status. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::NHT_FSDB_SUBSCRIPTIONS,
        HealthCheckCategory::NEXTHOP_TRACKER,
        HealthCheckStatus::SKIPPED,
        "FSDB subscription status not exposed yet (MEDIUM)"));
  }

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

/* ────────────────────────────────────────────────────────────────
 * FIB_AGENT checks (doc 2.1.x)
 * ──────────────────────────────────────────────────────────────── */
TModuleHealthReport HealthValidator::checkFibAgent() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::FIB_AGENT;
  auto& checks = *report.checks();

  /* 2.1.1 FIB agent connected and programmable */
  {
    auto val = getCounter("fib.agentProgrammable");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::FIB_AGENT_CONNECTED,
          HealthCheckCategory::FIB_AGENT,
          HealthCheckStatus::FAIL,
          "Counter fib.agentProgrammable not found"));
    } else {
      bool passed = (*val == 1);
      checks.emplace_back(makeResult(
          HealthCheckId::FIB_AGENT_CONNECTED,
          HealthCheckCategory::FIB_AGENT,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
          fmt::format("agentProgrammable = {}", *val),
          static_cast<double>(*val),
          1.0));
    }
  }

  /* 2.1.2 RIB-FIB in sync */
  {
    auto val = getCounter("fib.synced");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::FIB_AGENT_SYNCED,
          HealthCheckCategory::FIB_AGENT,
          HealthCheckStatus::FAIL,
          "Counter fib.synced not found"));
    } else {
      bool passed = (*val == 1);
      checks.emplace_back(makeResult(
          HealthCheckId::FIB_AGENT_SYNCED,
          HealthCheckCategory::FIB_AGENT,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
          fmt::format("fibSynced = {}", *val),
          static_cast<double>(*val),
          1.0));
    }
  }

  /* TODO: 2.1.3 No FIB programming failures
   * fib.agentUpdateFailures is a flat lifetime counter (incrementCounter).
   * Historical failures from past sessions are not actionable.
   * Need rate-based counter or convert to timeseries with .sum.600. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::FIB_AGENT_UPDATE_FAILURES,
        HealthCheckCategory::FIB_AGENT,
        HealthCheckStatus::SKIPPED,
        "Lifetime counter not useful; needs rate-based check"));
  }

  /* TODO: 2.1.4 No FIB status check failures
   * fib.agentStatusFailures is a flat lifetime counter (incrementCounter).
   * Same issue as 2.1.3. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::FIB_AGENT_STATUS_FAILURES,
        HealthCheckCategory::FIB_AGENT,
        HealthCheckStatus::SKIPPED,
        "Lifetime counter not useful; needs rate-based check"));
  }

  /* 2.1.5 FIB unicast routes > 0 */
  {
    auto val = getCounter("fib.totalUcastRoutes");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::FIB_AGENT_ROUTES_EXIST,
          HealthCheckCategory::FIB_AGENT,
          HealthCheckStatus::FAIL,
          "Counter fib.totalUcastRoutes not found"));
    } else {
      bool passed = (*val > 0);
      checks.emplace_back(makeResult(
          HealthCheckId::FIB_AGENT_ROUTES_EXIST,
          HealthCheckCategory::FIB_AGENT,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
          fmt::format("totalUcastRoutes = {}", *val),
          static_cast<double>(*val),
          1.0));
    }
  }

  /* 2.1.6 removed — duplicate of 2.1.2 fib.synced */

  /* TODO: 2.1.7 No FIB holddown active
   * Holddown state (ProgrammingHistory backoff after repeated failures)
   * is not exposed as a counter. Need to add a counter or thrift API. */
  {
    checks.emplace_back(makeResult(
        HealthCheckId::FIB_AGENT_HOLDDOWN,
        HealthCheckCategory::FIB_AGENT,
        HealthCheckStatus::SKIPPED,
        "Holddown state not exposed as counter (MEDIUM)"));
  }

  /* 2.1.8 No FIB flush events
   * fib.flushed is a timeseries with fb303::SUM export.
   * Use .sum.600 (10min window) for recent flush detection. */
  {
    auto val = getCounter("fib.flushed.sum.600");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::FIB_AGENT_FLUSH_EVENTS,
          HealthCheckCategory::FIB_AGENT,
          HealthCheckStatus::FAIL,
          "Counter fib.flushed.sum.600 not found"));
    } else {
      bool passed = (*val == 0);
      checks.emplace_back(makeResult(
          HealthCheckId::FIB_AGENT_FLUSH_EVENTS,
          HealthCheckCategory::FIB_AGENT,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("fibFlushed(10min) = {}", *val),
          static_cast<double>(*val),
          0.0));
    }
  }

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

/* ────────────────────────────────────────────────────────────────
 * THRIFT_ENDPOINT checks (doc 2.2.x)
 * ──────────────────────────────────────────────────────────────── */
TModuleHealthReport HealthValidator::checkThriftEndpoint() {
  TModuleHealthReport report;
  report.category() = HealthCheckCategory::THRIFT_ENDPOINT;
  auto& checks = *report.checks();

  /* 2.2.1 Thrift service alive
   * Validate by attempting a TCP connect to [::1]:kBgpThriftPort.
   * Unlike 1.1.1 (aliveSince counter), this directly verifies the
   * thrift endpoint is reachable. */
  {
    bool listening = false;
    int sock = ::socket(AF_INET6, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock >= 0) {
      struct sockaddr_in6 addr{};
      addr.sin6_family = AF_INET6;
      addr.sin6_port = htons(kBgpThriftPort);
      addr.sin6_addr = in6addr_loopback;

      int rc = ::connect(sock, (struct sockaddr*)&addr, sizeof(addr));
      if (rc == 0 || errno == EINPROGRESS) {
        listening = true;
      }

      ::close(sock);
    }

    checks.emplace_back(makeResult(
        HealthCheckId::THRIFT_ALIVE,
        HealthCheckCategory::THRIFT_ENDPOINT,
        listening ? HealthCheckStatus::PASS : HealthCheckStatus::FAIL,
        listening
            ? fmt::format("Thrift port {} is listening", kBgpThriftPort)
            : fmt::format("Thrift port {} is not reachable", kBgpThriftPort)));
  }

  /* 2.2.2 No streaming session rejections */
  {
    auto val = getCounter("bgpd.streamingSessionsRejected");
    if (!val.has_value()) {
      checks.emplace_back(makeResult(
          HealthCheckId::THRIFT_STREAMING_REJECTS,
          HealthCheckCategory::THRIFT_ENDPOINT,
          HealthCheckStatus::FAIL,
          "Counter bgpd.streamingSessionsRejected not found"));
    } else {
      bool passed = (*val == 0);
      checks.emplace_back(makeResult(
          HealthCheckId::THRIFT_STREAMING_REJECTS,
          HealthCheckCategory::THRIFT_ENDPOINT,
          passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
          fmt::format("streamingSessionsRejected = {}", *val),
          static_cast<double>(*val),
          0.0));
    }
  }

  /* TODO: 2.2.3 Drain state as expected
   * Drain state itself is not a health issue — it is intentional.
   * The real check is: when drain_state != UNDRAINED, verify that
   * the correct drain policy (e.g. PROPAGATE_NOTHING or *_DRAIN)
   * is applied to each peer's egress_policy_name via getBgpSessions().
   * When UNDRAINED, verify no drain policy is active.
   *
   * Requires getBgpSessions() via coroutine context to iterate
   * per-peer egress_policy_name. A follow-up diff can implement
   * this when check methods are transformed to coro-based. */
  {
    if (!configManager_) {
      checks.emplace_back(makeResult(
          HealthCheckId::THRIFT_DRAIN_STATE,
          HealthCheckCategory::THRIFT_ENDPOINT,
          HealthCheckStatus::FAIL,
          "ConfigManager not available"));
    } else {
      auto config = configManager_->getConfig();
      if (!config) {
        checks.emplace_back(makeResult(
            HealthCheckId::THRIFT_DRAIN_STATE,
            HealthCheckCategory::THRIFT_ENDPOINT,
            HealthCheckStatus::FAIL,
            "Config not available"));
      } else {
        auto drainState = *config->getConfig().drain_state();
        auto drainName = apache::thrift::util::enumNameSafe(drainState);

        checks.emplace_back(makeResult(
            HealthCheckId::THRIFT_DRAIN_STATE,
            HealthCheckCategory::THRIFT_ENDPOINT,
            HealthCheckStatus::SKIPPED,
            fmt::format(
                "drainState = {}. Per-peer drain policy verification "
                "deferred (needs coroutine context)",
                drainName)));
      }
    }
  }

  report.overallStatus() = computeOverallStatus(checks);
  return report;
}

/* ────────────────────────────────────────────────────────────────
 * Virtual check methods (overridden in platform subclasses)
 * ──────────────────────────────────────────────────────────────── */
THealthCheckResult HealthValidator::checkPlannedExit() {
  auto val = getCounter("bgpd.plannedExit");
  if (!val.has_value()) {
    return makeResult(
        HealthCheckId::GLOBAL_SYSTEM_PLANNED_EXIT,
        HealthCheckCategory::GLOBAL_SYSTEM,
        HealthCheckStatus::FAIL,
        "Counter bgpd.plannedExit not found");
  }
  bool passed = (*val == 1);
  return makeResult(
      HealthCheckId::GLOBAL_SYSTEM_PLANNED_EXIT,
      HealthCheckCategory::GLOBAL_SYSTEM,
      passed ? HealthCheckStatus::PASS : HealthCheckStatus::WARN,
      fmt::format("plannedExit = {}", *val),
      static_cast<double>(*val),
      1.0);
}

/* ────────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────── */
THealthCheckResult HealthValidator::makeResult(
    HealthCheckId checkId,
    HealthCheckCategory category,
    HealthCheckStatus status,
    const std::string& message,
    std::optional<double> observedValue,
    std::optional<double> threshold) {
  THealthCheckResult result;
  result.checkId() = checkId;
  result.category() = category;
  result.status() = status;
  result.message() = message;
  if (observedValue.has_value()) {
    result.observedValue() = *observedValue;
  }
  if (threshold.has_value()) {
    result.threshold() = *threshold;
  }
  result.timestampMs() =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  return result;
}

std::optional<int64_t> HealthValidator::getCounter(const std::string& key) {
  try {
    return fb303::ThreadCachedServiceData::get()->getServiceData()->getCounter(
        key);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

HealthCheckStatus HealthValidator::computeOverallStatus(
    const std::vector<THealthCheckResult>& checks) {
  bool anyFail = false;
  bool anyPass = false;
  bool anyWarn = false;
  for (const auto& c : checks) {
    switch (*c.status()) {
      case HealthCheckStatus::FAIL:
        anyFail = true;
        break;
      case HealthCheckStatus::WARN:
        anyWarn = true;
        break;
      case HealthCheckStatus::PASS:
        anyPass = true;
        break;
      case HealthCheckStatus::SKIPPED:
      case HealthCheckStatus::UNKNOWN:
        break;
    }
  }

  if (anyFail) {
    return HealthCheckStatus::FAIL;
  }
  if (anyWarn) {
    return HealthCheckStatus::WARN;
  }
  if (anyPass) {
    return HealthCheckStatus::PASS;
  }
  return HealthCheckStatus::SKIPPED;
}

} // namespace facebook::bgp
