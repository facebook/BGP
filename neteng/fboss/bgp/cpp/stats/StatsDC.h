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

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <folly/FixedString.h>

#include <fb303/ThreadCachedServiceData.h>
#include <fb303/detail/QuantileStatWrappers.h>
#include "neteng/fboss/bgp/cpp/common/Consts.h"

namespace facebook::bgp {
using folly::string_literals::operator""_fs;

namespace BgpStatsDC {

// VipService is enabled and running
constexpr auto kVipServiceEnabled = "bgpd.vipServiceEnabled"_fs;
// Total thrift-based vip injectors
constexpr auto kRunningVipServiceSessions = "bgpd.runningVipServiceSessions"_fs;
// whether ucmp auto link bandwidth feature is enabled
constexpr auto kUcmpAlbwEnabled = "bgpd.ucmp.auto_link_bandwidth.enabled"_fs;
// whether ucmp auto link bandwidth initialization was successful
constexpr auto kUcmpAlbwInitialized =
    "bgpd.ucmp.auto_link_bandwidth.initialized"_fs;

void initCounters();
// Set number of thrift-based vip injectors
void setRunningVipServiceSessions(uint32_t val);
void setVipServiceEnabled(bool vipSvcEnabled);
void setUcmpAlbwEnabled(bool enabled);
void setUcmpAlbwInitialized(bool initialized);

// [CRF File Mode]
constexpr auto kCrfFileModeEnabled = "bgpd.crf.file_mode_enabled"_fs;
constexpr auto kCrfArtifactReadSuccess = "bgpd.crf.artifact_read.success"_fs;
constexpr auto kCrfArtifactReadFailure = "bgpd.crf.artifact_read.failure"_fs;
constexpr auto kCrfPolicyAppliedSuccess = "bgpd.crf.policy_applied.success"_fs;
constexpr auto kCrfPolicyAppliedFailure = "bgpd.crf.policy_applied.failure"_fs;
constexpr auto kCrfThriftRpcRejected = "bgpd.crf.thrift_rpc_rejected"_fs;
void setCrfFileModeEnabled(bool enabled);
void incrCrfArtifactReadSuccess();
void incrCrfArtifactReadFailure();
void incrCrfPolicyAppliedSuccess();
void incrCrfPolicyAppliedFailure();
void incrCrfThriftRpcRejected();

// [CPS File Mode]
constexpr auto kCpsFileModeEnabled = "bgpd.cps.file_mode_enabled"_fs;
constexpr auto kCpsArtifactReadSuccess = "bgpd.cps.artifact_read.success"_fs;
constexpr auto kCpsArtifactReadFailure = "bgpd.cps.artifact_read.failure"_fs;
constexpr auto kCpsPolicyAppliedSuccess = "bgpd.cps.policy_applied.success"_fs;
constexpr auto kCpsPolicyAppliedFailure = "bgpd.cps.policy_applied.failure"_fs;
constexpr auto kCpsThriftRpcRejected = "bgpd.cps.thrift_rpc_rejected"_fs;
constexpr auto kCpsForceUpdateBypass = "bgpd.cps.force_update_bypass"_fs;
void setCpsFileModeEnabled(bool enabled);
void incrCpsArtifactReadSuccess();
void incrCpsArtifactReadFailure();
void incrCpsPolicyAppliedSuccess();
void incrCpsPolicyAppliedFailure();
void incrCpsThriftRpcRejected();
void incrCpsForceUpdateBypass();

} // namespace BgpStatsDC

namespace RibStatsDC {

void initCounters();

/*
 * Device-level partial drain state: 1 when at least one prefix is partially
 * drained, 0 otherwise. Updated on each 0<->1 transition so a true<->false
 * flip is observable on ODS independent of the FSDB publish path.
 */
inline const auto kRibIsPartialDrain =
    fmt::format("{}.rib.is_partial_drain", kBgpcppTag);
void setIsPartialDrain(bool isPartiallyDrained);

// total number of received path selection policy
inline constexpr auto kPsPolicyRcvd = "bgpd.ribPolicy.numRcvdPsPolicy";
DECLARE_timeseries(psPolicyRcvd);

// total number of path selection policy updates
inline constexpr auto kPsPolicyUpdate = "bgpd.ribPolicy.numUpdatedPsPolicy";
DECLARE_timeseries(psPolicyUpdate);

// total number of received route attribute policy
inline constexpr auto kRaPolicyRcvd = "bgpd.ribPolicy.numRcvdRaPolicy";
DECLARE_timeseries(raPolicyRcvd);

// total number of route attribute policy updates
inline constexpr auto kRaPolicyUpdate = "bgpd.ribPolicy.numUpdatedRaPolicy";
DECLARE_timeseries(raPolicyUpdate);

// time for rib to overwrite route attributes per route
inline constexpr auto kRibRouteAttributeOverwriteTimeMs =
    "bgpd.rib.routeAttributeOverwriteTimeMs";
DECLARE_quantile_stat(ribRouteAttributeOverwriteTimeMs);

// time for rib to overwrite route attributes in a full sync
inline constexpr auto ribFullSyncRouteAttributeOverwriteTimeMs =
    "bgpd.rib.fullSyncRouteAttributeOverwriteTimeMs";
DECLARE_quantile_stat(ribFullSyncRouteAttributeOverwriteTimeMs);

// Prefix changes encoded as present canonical entries.
inline constexpr auto kCanonicalRibExportUpsert =
    "bgpcpp.rib.canonicalExporter.numPrefixUpsert";
DECLARE_timeseries(canonicalRibExportUpsert);
// Prefix changes encoded as canonical entry deletions.
inline constexpr auto kCanonicalRibExportDelete =
    "bgpcpp.rib.canonicalExporter.numPrefixDelete";
DECLARE_timeseries(canonicalRibExportDelete);
// Incremental FSDB transactions containing one coalesced prefix batch.
inline constexpr auto kCanonicalRibExportIncrementalBatchUpdate =
    "bgpcpp.rib.canonicalExporter.numIncrementalBatchUpdate";
DECLARE_timeseries(canonicalRibExportIncrementalBatchUpdate);
// Complete canonical snapshots built for initial sync or reconnect.
inline constexpr auto kCanonicalRibExportFullSnapshotUpdate =
    "bgpcpp.rib.canonicalExporter.numFullSnapshotUpdate";
DECLARE_timeseries(canonicalRibExportFullSnapshotUpdate);
// FSDB reconnect generations received by the canonical exporter.
inline constexpr auto kCanonicalRibExportReconnectRebuildRequest =
    "bgpcpp.rib.canonicalExporter.numReconnectRebuildRequest";
DECLARE_timeseries(canonicalRibExportReconnectRebuildRequest);
// RIB-sized reconnect walks actually started after all scheduling gates.
inline constexpr auto kCanonicalRibExportReconnectRebuildStart =
    "bgpcpp.rib.canonicalExporter.numReconnectRebuildStart";
DECLARE_timeseries(canonicalRibExportReconnectRebuildStart);
// Time spent encoding canonical entries before each publication boundary.
inline constexpr auto kCanonicalRibExportBuildTimeMs =
    "bgpcpp.rib.canonicalExporter.buildTimeMs";
DECLARE_quantile_stat(canonicalRibExportBuildTimeMs);
// End-to-end time spent preparing and handing one update to FsdbSyncer.
inline constexpr auto kCanonicalRibExportPublishTimeMs =
    "bgpcpp.rib.canonicalExporter.publishTimeMs";
DECLARE_quantile_stat(canonicalRibExportPublishTimeMs);
// Total time canonical export occupies the RIB EventBase for one batch.
inline constexpr auto kCanonicalRibExportRibThreadTimeMs =
    "bgpcpp.rib.canonicalExporter.ribThreadTimeMs";
DECLARE_quantile_stat(canonicalRibExportRibThreadTimeMs);

/**
 * Set point-in-time fb303 counters describing one canonical interning pool.
 *
 * @param pool Stable pool label used in dynamic counter names.
 * @param live Slots currently tracked by the pool.
 * @param highWater Number of monotonic IDs allocated in the encoder epoch;
 *     equivalently, the next ID that would be assigned.
 */
void setCanonicalRibPoolStats(
    std::string_view pool,
    size_t live,
    size_t highWater);

// Cache migration outcome types
inline constexpr auto kRaPolicyCacheMigrationIdentical =
    "bgpd.ribPolicy.routeAttributePolicyCache.migration.identical";
DECLARE_timeseries(raPolicyCacheMigrationIdentical);
inline constexpr auto kRaPolicyCacheMigrationExpirationOnly =
    "bgpd.ribPolicy.routeAttributePolicyCache.migration.expirationOnly";
DECLARE_timeseries(raPolicyCacheMigrationExpirationOnly);
inline constexpr auto kRaPolicyCacheMigrationSelective =
    "bgpd.ribPolicy.routeAttributePolicyCache.migration.selective";
DECLARE_timeseries(raPolicyCacheMigrationSelective);

// Number of cache entries preserved/invalidated during selective migration
inline constexpr auto kRaPolicyCachePreserved =
    "bgpd.ribPolicy.routeAttributePolicyCache.num_preserved";
DECLARE_timeseries(raPolicyCachePreserved);
inline constexpr auto kRaPolicyCacheInvalidated =
    "bgpd.ribPolicy.routeAttributePolicyCache.num_invalidated";
DECLARE_timeseries(raPolicyCacheInvalidated);

// Number of prefixes re-evaluated during policy update
inline constexpr auto kRaPolicyReEvalPrefixes =
    "bgpd.ribPolicy.routeAttributePolicyCache.num_prefix_reeval";
DECLARE_timeseries(raPolicyReEvalPrefixes);

// Cache migration latency
inline constexpr auto kRaPolicyCacheMigrationTimeMs =
    "bgpd.ribPolicy.routeAttributePolicyCache.migration.process_time_ms";
DECLARE_quantile_stat(raPolicyCacheMigrationTimeMs);

} // namespace RibStatsDC

//------------------------ FsdbStatsDC ------------------------//

namespace FsdbStatsDC {

constexpr auto kNbrDownPrefix = "bgpd.fsdb."_fs;

// Time waiting for the publication barrier and enqueueing one FSDB patch.
inline constexpr auto kFsdbSyncerPublishStateEnqueueTimeMs =
    "bgpcpp.fsdbSyncer.publishStateEnqueueTimeMs";
DECLARE_quantile_stat(fsdbSyncerPublishStateEnqueueTimeMs);

// NHT FSDB reachability transition counters
inline const auto kFsdbNhtNexthopReachable =
    fmt::format("{}.nht.fsdb.nexthop_reachable", kBgpcppTag);
DECLARE_timeseries(fsdbNhtNexthopReachable);
void incrFsdbNhtNexthopReachable();

inline const auto kFsdbNhtNexthopUnreachable =
    fmt::format("{}.nht.fsdb.nexthop_unreachable", kBgpcppTag);
DECLARE_timeseries(fsdbNhtNexthopUnreachable);
void incrFsdbNhtNexthopUnreachable();

inline const auto kFsdbNhtDisconnects =
    fmt::format("{}.nht.fsdb.disconnects", kBgpcppTag);
DECLARE_timeseries(fsdbNhtDisconnects);
void incrFsdbNhtDisconnects();

inline const auto kFsdbNhtConnected =
    fmt::format("{}.nht.fsdb.connected", kBgpcppTag);
void setFsdbNhtConnected(int64_t val);

/**
 * Increment a dynamic per-subtree FSDB syncer counter.
 *
 * Counters use `bgpcpp.fsdbSyncer.<subtree>.<event>`. `numUpdate` counts every
 * update received, `numClear` explicit clears, and `numIncrementalPublish`
 * successful incremental patches containing the subtree.
 *
 * @param subtree Stable BgpData subtree label.
 * @param event Counter suffix such as numUpdate or numIncrementalPublish.
 */
void addFsdbSyncerSubtreeEvent(
    std::string_view subtree,
    std::string_view event);

/**
 * Increment a dynamic FSDB syncer lifecycle counter.
 *
 * @param event Counter suffix such as numConnect or numSnapshotPublish.
 */
void addFsdbSyncerLifecycleEvent(std::string_view event);

void initCounters();

} // namespace FsdbStatsDC

void initStatsDC();

} // namespace facebook::bgp
