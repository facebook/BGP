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

#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalRibEncoder.h"

#include <utility>

namespace facebook::bgp {
namespace {
constexpr auto kPoolSweepInterval = std::chrono::minutes(3);
} // namespace

bgp_thrift::TRibEntryCanonical CanonicalRibEncoder::buildEntry(
    const folly::CIDRNetwork& prefix,
    int64_t ribVersion,
    const std::vector<CanonicalPathInput>& paths,
    bool exportMultipaths) {
  auto result = encoding_.buildEntryReportingChanges(
      prefix,
      ribVersion,
      paths,
      /*includeBestPath=*/true,
      /*includePaths=*/exportMultipaths);
  poolDirty_ |= result.poolsChanged;
  return std::move(result.entry);
}

void CanonicalRibEncoder::markReclamationPending() {
  reclamationPending_ = true;
}

bool CanonicalRibEncoder::consumeDirtyAndSweep(TimePoint now) {
  bool reclaimed = false;
  if (reclamationPending_ && now - lastSweepTime_ >= kPoolSweepInterval) {
    reclaimed = encoding_.sweep();
    lastSweepTime_ = now;
    reclamationPending_ = false;
  }
  const bool full = poolDirty_ || reclaimed;
  poolDirty_ = false;
  return full;
}

bgp_thrift::TBgpAttrDict CanonicalRibEncoder::dictSnapshot() const {
  return encoding_.dictSnapshot();
}

folly::F14FastMap<int64_t, bgp_thrift::TBgpDedupedPath>
CanonicalRibEncoder::pathAttrsSnapshot() const {
  return encoding_.pathAttrsSnapshot();
}

folly::F14FastMap<int64_t, bgp_thrift::TCanonicalPeer>
CanonicalRibEncoder::peersSnapshot() const {
  return encoding_.peersSnapshot();
}

CanonicalRibEncoder::PoolStatsSnapshot CanonicalRibEncoder::poolStats() const {
  return PoolStatsSnapshot{
      .wholePath = encoding_.wholePathStats(),
      .asPath = encoding_.asPathStats(),
      .communities = encoding_.communitiesStats(),
      .extCommunities = encoding_.extCommunitiesStats(),
      .clusterList = encoding_.clusterListStats(),
  };
}

} // namespace facebook::bgp
