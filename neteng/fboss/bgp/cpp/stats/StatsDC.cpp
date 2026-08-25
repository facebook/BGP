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

#include "neteng/fboss/bgp/cpp/stats/StatsDC.h"

namespace facebook::bgp {

namespace BgpStatsDC {

void initCounters() {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kRunningVipServiceSessions, -1);
  fb303::ThreadCachedServiceData::get()->setCounter(kVipServiceEnabled, -1);
  fb303::ThreadCachedServiceData::get()->setCounter(kUcmpAlbwEnabled, -1);
  fb303::ThreadCachedServiceData::get()->setCounter(kUcmpAlbwInitialized, -1);

  // [CRF File Mode]
  fb303::ThreadCachedServiceData::get()->setCounter(kCrfFileModeEnabled, 0);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCrfArtifactReadSuccess, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCrfArtifactReadFailure, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCrfPolicyAppliedSuccess, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCrfPolicyAppliedFailure, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCrfThriftRpcRejected, fb303::SUM);

  // [CPS File Mode]
  fb303::ThreadCachedServiceData::get()->setCounter(kCpsFileModeEnabled, 0);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCpsArtifactReadSuccess, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCpsArtifactReadFailure, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCpsPolicyAppliedSuccess, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCpsPolicyAppliedFailure, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCpsThriftRpcRejected, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kCpsForceUpdateBypass, fb303::SUM);
}

void setRunningVipServiceSessions(uint32_t val) {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kRunningVipServiceSessions, val);
}

void setVipServiceEnabled(bool vipSvcEnabled) {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kVipServiceEnabled, vipSvcEnabled ? 1 : 0);
}

void setUcmpAlbwEnabled(bool enabled) {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kUcmpAlbwEnabled, enabled ? 1 : 0);
}

void setUcmpAlbwInitialized(bool initialized) {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kUcmpAlbwInitialized, initialized ? 1 : 0);
}

void setCrfFileModeEnabled(bool enabled) {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kCrfFileModeEnabled, enabled ? 1 : 0);
}

void incrCrfArtifactReadSuccess() {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      kCrfArtifactReadSuccess, 1);
}

void incrCrfArtifactReadFailure() {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      kCrfArtifactReadFailure, 1);
}

void incrCrfPolicyAppliedSuccess() {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      kCrfPolicyAppliedSuccess, 1);
}

void incrCrfPolicyAppliedFailure() {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      kCrfPolicyAppliedFailure, 1);
}

void incrCrfThriftRpcRejected() {
  fb303::ThreadCachedServiceData::get()->addStatValue(kCrfThriftRpcRejected, 1);
}

void setCpsFileModeEnabled(bool enabled) {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kCpsFileModeEnabled, enabled ? 1 : 0);
}

void incrCpsArtifactReadSuccess() {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      kCpsArtifactReadSuccess, 1);
}

void incrCpsArtifactReadFailure() {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      kCpsArtifactReadFailure, 1);
}

void incrCpsPolicyAppliedSuccess() {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      kCpsPolicyAppliedSuccess, 1);
}

void incrCpsPolicyAppliedFailure() {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      kCpsPolicyAppliedFailure, 1);
}

void incrCpsThriftRpcRejected() {
  fb303::ThreadCachedServiceData::get()->addStatValue(kCpsThriftRpcRejected, 1);
}

void incrCpsForceUpdateBypass() {
  fb303::ThreadCachedServiceData::get()->addStatValue(kCpsForceUpdateBypass, 1);
}

} // namespace BgpStatsDC

namespace RibStatsDC {

void initCounters() {
  fb303::ThreadCachedServiceData::get()->setCounter(kRibIsPartialDrain, 0);
}

DEFINE_timeseries(psPolicyRcvd, kPsPolicyRcvd, fb303::COUNT);
DEFINE_timeseries(psPolicyUpdate, kPsPolicyUpdate, fb303::COUNT);
DEFINE_timeseries(raPolicyRcvd, kRaPolicyRcvd, fb303::COUNT);
DEFINE_timeseries(raPolicyUpdate, kRaPolicyUpdate, fb303::COUNT);

// time for rib to overwrite route attributes per route
DEFINE_quantile_stat(
    ribRouteAttributeOverwriteTimeMs,
    kRibRouteAttributeOverwriteTimeMs,
    fb303::ExportTypeConsts::kAvg,
    fb303::QuantileConsts::kP50_P95_P99,
    fb303::SlidingWindowPeriodConsts::kOneMinTenMin);
// time for rib to overwrite route attributes in a full sync
DEFINE_quantile_stat(
    ribFullSyncRouteAttributeOverwriteTimeMs,
    ribFullSyncRouteAttributeOverwriteTimeMs,
    fb303::ExportTypeConsts::kAvg,
    fb303::QuantileConsts::kP50_P95_P99,
    fb303::SlidingWindowPeriodConsts::kOneMinTenMin);

DEFINE_timeseries(
    canonicalRibExportUpsert,
    kCanonicalRibExportUpsert,
    fb303::SUM);
DEFINE_timeseries(
    canonicalRibExportDelete,
    kCanonicalRibExportDelete,
    fb303::SUM);
DEFINE_timeseries(
    canonicalRibExportIncrementalBatchUpdate,
    kCanonicalRibExportIncrementalBatchUpdate,
    fb303::SUM);
DEFINE_timeseries(
    canonicalRibExportFullSnapshotUpdate,
    kCanonicalRibExportFullSnapshotUpdate,
    fb303::SUM);
DEFINE_timeseries(
    canonicalRibExportReconnectRebuildRequest,
    kCanonicalRibExportReconnectRebuildRequest,
    fb303::SUM);
DEFINE_timeseries(
    canonicalRibExportReconnectRebuildStart,
    kCanonicalRibExportReconnectRebuildStart,
    fb303::SUM);
DEFINE_quantile_stat(
    canonicalRibExportBuildTimeMs,
    kCanonicalRibExportBuildTimeMs,
    fb303::ExportTypeConsts::kAvg,
    fb303::QuantileConsts::kP50_P95_P99,
    fb303::SlidingWindowPeriodConsts::kOneMinTenMin);
DEFINE_quantile_stat(
    canonicalRibExportPublishTimeMs,
    kCanonicalRibExportPublishTimeMs,
    fb303::ExportTypeConsts::kAvg,
    fb303::QuantileConsts::kP50_P95_P99,
    fb303::SlidingWindowPeriodConsts::kOneMinTenMin);
DEFINE_quantile_stat(
    canonicalRibExportRibThreadTimeMs,
    kCanonicalRibExportRibThreadTimeMs,
    fb303::ExportTypeConsts::kAvg,
    fb303::QuantileConsts::kP50_P95_P99,
    fb303::SlidingWindowPeriodConsts::kOneMinTenMin);

void setCanonicalRibPoolStats(
    std::string_view pool,
    size_t live,
    size_t highWater) {
  auto* stats = fb303::ThreadCachedServiceData::get();
  const auto prefix = fmt::format("bgpcpp.rib.canonicalExporter.pool.{}", pool);
  /* Pool IDs are signed int64, so these cardinalities cannot exceed INT64_MAX.
   */
  stats->setCounter(fmt::format("{}.live", prefix), static_cast<int64_t>(live));
  stats->setCounter(
      fmt::format("{}.highWater", prefix), static_cast<int64_t>(highWater));
}

DEFINE_timeseries(
    raPolicyCacheMigrationIdentical,
    kRaPolicyCacheMigrationIdentical,
    fb303::COUNT);
DEFINE_timeseries(
    raPolicyCacheMigrationExpirationOnly,
    kRaPolicyCacheMigrationExpirationOnly,
    fb303::COUNT);
DEFINE_timeseries(
    raPolicyCacheMigrationSelective,
    kRaPolicyCacheMigrationSelective,
    fb303::COUNT);
DEFINE_timeseries(
    raPolicyCachePreserved,
    kRaPolicyCachePreserved,
    fb303::COUNT);
DEFINE_timeseries(
    raPolicyCacheInvalidated,
    kRaPolicyCacheInvalidated,
    fb303::COUNT);
DEFINE_timeseries(
    raPolicyReEvalPrefixes,
    kRaPolicyReEvalPrefixes,
    fb303::COUNT);
DEFINE_quantile_stat(
    raPolicyCacheMigrationTimeMs,
    kRaPolicyCacheMigrationTimeMs,
    fb303::ExportTypeConsts::kAvg,
    fb303::QuantileConsts::kP50_P95_P99,
    fb303::SlidingWindowPeriodConsts::kOneMinTenMin);

void setIsPartialDrain(bool isPartiallyDrained) {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kRibIsPartialDrain, isPartiallyDrained ? 1 : 0);
}

} // namespace RibStatsDC

//------------------------ FsdbStatsDC ------------------------//

namespace FsdbStatsDC {

DEFINE_quantile_stat(
    fsdbSyncerPublishStateEnqueueTimeMs,
    kFsdbSyncerPublishStateEnqueueTimeMs,
    fb303::ExportTypeConsts::kAvg,
    fb303::QuantileConsts::kP50_P95_P99,
    fb303::SlidingWindowPeriodConsts::kOneMinTenMin);

DEFINE_timeseries(
    fsdbNhtNexthopReachable,
    kFsdbNhtNexthopReachable,
    fb303::COUNT);
DEFINE_timeseries(
    fsdbNhtNexthopUnreachable,
    kFsdbNhtNexthopUnreachable,
    fb303::COUNT);
DEFINE_timeseries(fsdbNhtDisconnects, kFsdbNhtDisconnects, fb303::COUNT);

void initCounters() {
  fb303::ThreadCachedServiceData::get()->setCounter(
      kFsdbNhtNexthopReachable + ".count", 0);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kFsdbNhtNexthopReachable + ".count.60", 0);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kFsdbNhtNexthopUnreachable + ".count", 0);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kFsdbNhtNexthopUnreachable + ".count.60", 0);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kFsdbNhtDisconnects + ".count", 0);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kFsdbNhtDisconnects + ".count.60", 0);
  fb303::ThreadCachedServiceData::get()->setCounter(kFsdbNhtConnected, -1);
}

void incrFsdbNhtNexthopReachable() {
  STATS_fsdbNhtNexthopReachable.add(1);
}

void incrFsdbNhtNexthopUnreachable() {
  STATS_fsdbNhtNexthopUnreachable.add(1);
}

void incrFsdbNhtDisconnects() {
  STATS_fsdbNhtDisconnects.add(1);
}

void setFsdbNhtConnected(int64_t val) {
  fb303::ThreadCachedServiceData::get()->setCounter(kFsdbNhtConnected, val);
}

void addFsdbSyncerSubtreeEvent(
    std::string_view subtree,
    std::string_view event) {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      fmt::format("bgpcpp.fsdbSyncer.{}.{}", subtree, event), 1, fb303::SUM);
}

void addFsdbSyncerLifecycleEvent(std::string_view event) {
  fb303::ThreadCachedServiceData::get()->addStatValue(
      fmt::format("bgpcpp.fsdbSyncer.{}", event), 1, fb303::SUM);
}

} // namespace FsdbStatsDC

void initStatsDC() {
  BgpStatsDC::initCounters();
  RibStatsDC::initCounters();
  FsdbStatsDC::initCounters();
}

} // namespace facebook::bgp
