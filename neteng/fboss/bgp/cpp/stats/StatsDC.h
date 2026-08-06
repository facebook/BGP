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

#include <cstdint>

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

// Device-level partial drain state: 1 when at least one prefix is partially
// drained, 0 otherwise. Updated on each 0<->1 transition so a true<->false
// flip is observable on ODS independent of the FSDB publish path.
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

void initCounters();

} // namespace FsdbStatsDC

void initStatsDC();

} // namespace facebook::bgp
