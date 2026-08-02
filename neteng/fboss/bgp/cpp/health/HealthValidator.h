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

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <folly/coro/Task.h>

#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

namespace facebook::bgp {

class ConfigManager;
class NexthopHandler;
class PeerManagerBase;
class RibBase;
class Watchdog;

/**
 * Stateless health check evaluator for BGP++.
 *
 * Created once in BgpService and reused across getHealthReport() calls.
 * Reads fb303 counters (thread-safe) and calls existing module APIs.
 * Module pointers may be null for platform-conditional checks or testing.
 * Config is used as source of truth for platform-conditional checks.
 *
 * Platform-specific overrides (e.g. NetlinkWrapper-aware checks) live in
 * subclasses (HealthValidatorBB) so the base has no NetlinkWrapper
 * compile-time dependency. DC/OSS link against this base directly.
 */
class HealthValidator {
 public:
  HealthValidator(
      PeerManagerBase* peerMgr,
      RibBase* rib,
      Watchdog* watchdog,
      NexthopHandler* nexthopHandler = nullptr,
      std::shared_ptr<ConfigManager> configManager = nullptr);

  virtual ~HealthValidator() = default;

  /**
   * Run all health checks and return the full report.
   * Returns a coro task to enable future async module calls
   * (e.g. co_getBgpSessions() for per-peer checks).
   *
   * IMPORTANT: Must be called from a thrift server thread, NOT from
   * PeerManagerBase or Rib event base threads. Some checks call module
   * methods that use runInEventBaseThreadAndWait() which would
   * deadlock if called from the owning event base.
   */
  folly::coro::Task<neteng::fboss::bgp::thrift::THealthReport> generateReport();

  /*
   * Build the per-peer prefix-limit drop health result from a set of sessions.
   * WARN (listing each affected peer with its exact PR dropped count) when any
   * peer has dropped received routes due to its configured per-peer prefix
   * limit, else PASS. Pure and static so it can be unit tested without a live
   * PeerManagerBase.
   */
  static neteng::fboss::bgp::thrift::THealthCheckResult
  evaluatePrefixLimitDrops(
      const std::vector<neteng::fboss::bgp::thrift::TBgpSession>& sessions);

  /*
   * Classify RIB_ORIGINATED_ROUTES by comparing how many routes the RIB
   * actually originated against how many the config declares (networks4 +
   * networks6, the source of truth). The count itself is irrelevant -- zero is
   * valid (e.g. a backbone router with no network statements); only config-vs-
   * RIB agreement matters. A shortfall means a configured origination was not
   * realized in the RIB (invalid origin value, or a network's policy rejecting
   * it) and is a FAIL. `configured` == nullopt (config unavailable) -> SKIPPED.
   * Pure and static so it can be unit tested without a live RIB or config.
   */
  static neteng::fboss::bgp::thrift::HealthCheckStatus classifyOriginatedRoutes(
      int64_t originated,
      std::optional<int64_t> configured);

 protected:
  using THealthCheckResult = neteng::fboss::bgp::thrift::THealthCheckResult;
  using TModuleHealthReport = neteng::fboss::bgp::thrift::TModuleHealthReport;
  using THealthReport = neteng::fboss::bgp::thrift::THealthReport;
  using HealthCheckId = neteng::fboss::bgp::thrift::HealthCheckId;
  using HealthCheckCategory = neteng::fboss::bgp::thrift::HealthCheckCategory;
  using HealthCheckStatus = neteng::fboss::bgp::thrift::HealthCheckStatus;

  /* One method per category */
  TModuleHealthReport checkGlobalSystem();
  TModuleHealthReport checkGlobalTaskThread();
  TModuleHealthReport checkGlobalConvergence();
  TModuleHealthReport checkSessionManager();
  folly::coro::Task<TModuleHealthReport> checkPeerManager();
  folly::coro::Task<TModuleHealthReport> checkRib();
  /* Virtual: BB override consults its NetlinkWrapper. Base returns
   * SKIPPED when NHT is disabled in config, FAIL when NHT is enabled
   * but the platform does not provide a NetlinkWrapper. */
  virtual TModuleHealthReport checkNetlinkWrapper();

  /* Virtual: BB override returns SKIPPED (planned exit tracking
   * is not meaningful in EBB context). */
  virtual THealthCheckResult checkPlannedExit();
  TModuleHealthReport checkNexthopTracker();
  TModuleHealthReport checkFibAgent();
  TModuleHealthReport checkThriftEndpoint();

  /* Helpers */
  THealthCheckResult makeResult(
      HealthCheckId checkId,
      HealthCheckCategory category,
      HealthCheckStatus status,
      const std::string& message,
      std::optional<double> observedValue = std::nullopt,
      std::optional<double> threshold = std::nullopt);

  std::optional<int64_t> getCounter(const std::string& key);

  static HealthCheckStatus computeOverallStatus(
      const std::vector<THealthCheckResult>& checks);

  PeerManagerBase* peerMgr_;

  /*
   * Holds the base Rib pointer. For platform-specific health checks,
   * use dynamic_cast to access the concrete type:
   *   if (auto* ribDC = dynamic_cast<RibDC*>(rib_)) { ... }
   *   else if (auto* ribBB = dynamic_cast<RibBB*>(rib_)) { ... }
   * This avoids splitting HealthValidator per platform.
   */
  RibBase* rib_;
  Watchdog* watchdog_;
  [[maybe_unused]] NexthopHandler* nexthopHandler_;
  std::shared_ptr<ConfigManager> configManager_;
};

/* ── Health check thresholds ────────────────────────────────────
 * Defined here so they are visible to both the implementation
 * and unit tests for threshold-based assertions.
 * ──────────────────────────────────────────────────────────────── */

/* Minimum process uptime before heartbeat checks are meaningful */
inline constexpr int64_t kUptimeMinSeconds = 30;

/* Resource utilization thresholds (percentage-based for portability)
 * TODO(T266104366): Define platform-specific thresholds.
 * Current values may not be role/network agnostic (e.g., DC 5GB vs EB 20GB). */
inline constexpr double kCpuWarningPct = 50.0;
inline constexpr double kCpuCriticalPct = 80.0;
inline constexpr double kMemoryWarningPct = 20.0;
inline constexpr double kMemoryCriticalPct = 50.0;

/* Maximum acceptable convergence time.
 * TODO(T266104555): Define separate values for DC vs EBB.
 * DC max observed ~3min (https://fburl.com/ods/bgpd_convergence_dc).
 * EBB convergence characteristics may differ significantly. */
inline constexpr int64_t kMaxConvergenceMs = 300000; /* 5 min */

/* Session flap threshold: FAIL if flaps in 10min window exceed
 * this fraction of total running sessions */
inline constexpr double kSessionFlapThresholdPct = 0.5;

/* Per-peer RIB version lag threshold: FAIL if any peer is more
 * than this many versions behind the global rib.tableVersion.
 * TODO(T266578118): Fine-tune threshold based on
 * scale — peer flaps and best-path recalculation can cause
 * transient lag that varies by deployment size. */
inline constexpr int64_t kRibVersionLagThreshold = 1000;

/* Attribute deduplication efficiency thresholds.
 * WARNING (0.5-0.8): treated as PASS — normal on small-scale boxes.
 * CRITICAL (>0.8): treated as FAIL — poor dedup at scale. */
inline constexpr double kAttrDedupWarningThreshold = 0.5;
inline constexpr double kAttrDedupCriticalThreshold = 0.8;

/* Maximum acceptable path selection duration */
inline constexpr int64_t kMaxPathSelectionMs = 5000;

/* fib.totalUcastRoutes vs bgpd.rib.totalShadowRibEntries divergence
 * threshold. Both count route/prefix-level entries but may differ
 * transiently during in-flight updates. */
inline constexpr double kRibShadowMaxDivergencePct = 0.1; /* 10% */

/* Switch limit usage thresholds (percentage of configured limit).
 * WARN at 80% to give operators time to act before overload triggers.
 * FAIL at 100% — overload protection mode will activate. */
inline constexpr double kSwitchLimitWarningPct = 80.0;
inline constexpr double kSwitchLimitCriticalPct = 100.0;

/* Maximum acceptable best-path + FIB programming pause time (p99, ms) */
inline constexpr int64_t kMaxBpFibPauseMs = 5000;

/* Heartbeat stall detection thresholds (seconds of drift).
 * Drift = abs(timeDelta - heartbeatDelta) where ideally both ~1/s. */
inline constexpr int64_t kHeartbeatWarningStallSeconds = 5;
inline constexpr int64_t kHeartbeatCriticalStallSeconds = 15;

/* Timeout for cross-module evb calls (e.g., Rib getOriginatedRoutes).
 * Uses co_runOnEvbWithTimeout backed by global Timekeeper —
 * fires even if the target module's evb is stuck. */
inline constexpr std::chrono::seconds kHealthCheckModuleTimeout{5};

} // namespace facebook::bgp
