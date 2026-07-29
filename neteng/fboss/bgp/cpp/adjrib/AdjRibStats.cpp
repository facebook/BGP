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

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStats.h"

#include <gflags/gflags.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"

DEFINE_bool(
    enable_peer_status_logging,
    false,
    "Publish fb303 counter if BGP session is up or down");

namespace facebook::bgp {

// Static inits for received prefix tracking (used by AdjRibIn stats)
// Note: These are extern declared in AdjRibStats.h and defined here
uint32_t totalRcvdPrefixCount = 0;
uint32_t maxPeerRcvdPrefixCount = 0;
uint32_t totalAcceptedPrefixCount = 0;
uint32_t totalDroppedPrefixCount = 0;

// Static inits for sent prefix tracking (used by AdjRibOut stats)
uint32_t totalSentPrefixCount = 0;
uint32_t totalVipPrefixesCount = 0;

/*******************************************************************************
 *             AdjRibStats methods
 *******************************************************************************/

void AdjRibStats::clear() {
  sentUpdateMsgs = 0;
  sentEndOfRibMsgs = 0;
  recvUpdateMsgs = 0;
  recvEndOfRibMsgs = 0;
  sentAnnouncementsIpv4 = 0;
  sentAnnouncementsIpv6 = 0;
  recvAnnouncementsIpv4 = 0;
  recvAnnouncementsIpv6 = 0;
  sentWithdrawals = 0;
  recvWithdrawals = 0;

  egressQueueBlocks = 0;
  egressQueueTotalBlockDurationMs = 0;
  lastEgressQueueBlockTimeMs = 0;
  transientRouteUpdatesSuppressed = 0;
  numTimesDetachedByBlocking = 0;
  numTimesDetachedByPolicy = 0;
  numTimesRejoined = 0;
  totalIngressRouteFilterDenied = 0;
  preFilterDroppedRouteCount = 0;
  BgpStats::initEgressBackpressureStats();
  BgpStats::initWellKnownCommunityStats();
}

void AdjRibStats::incrementPreFilterDroppedRouteCount() {
  preFilterDroppedRouteCount++;
}

/*******************************************************************************
 *             AdjRibIn stats functionality
 *******************************************************************************/

void AdjRibStats::incrementPostInPrefixCount(bool isIpv4) {
  postInPrefixCount++;
  if (isIpv4) {
    postInPrefixCountIpv4++;
  } else {
    postInPrefixCountIpv6++;
  }
  totalAcceptedPrefixCount++;
  PeerStats::setPeerPostInPrefixes(peerIdOdsStr, postInPrefixCount);
  PeerStats::setTotalAcceptedPrefixes(totalAcceptedPrefixCount);
}

void AdjRibStats::decrementPostInPrefixCount(bool isIpv4) {
  postInPrefixCount--;
  if (isIpv4) {
    postInPrefixCountIpv4--;
  } else {
    postInPrefixCountIpv6--;
  }
  totalAcceptedPrefixCount--;
  PeerStats::setPeerPostInPrefixes(peerIdOdsStr, postInPrefixCount);
  PeerStats::setTotalAcceptedPrefixes(totalAcceptedPrefixCount);
}

void AdjRibStats::incrementRecvUpdateMsgs() {
  recvUpdateMsgs++;
  PeerStats::addPeerMessagesRecvUpdate(peerIdOdsStr);
}

void AdjRibStats::incrementRecvEndOfRibMsgs() {
  recvEndOfRibMsgs++;
}

void AdjRibStats::incrementRecvAnnouncementsIpv4() {
  recvAnnouncementsIpv4++;
  PeerStats::incrMessageRecvAnnouncedIpv4(peerIdOdsStr);
}

void AdjRibStats::incrementRecvAnnouncementsIpv6() {
  recvAnnouncementsIpv6++;
  PeerStats::incrMessageRecvAnnouncedIpv6(peerIdOdsStr);
}

void AdjRibStats::incrementRecvWithdrawals() {
  recvWithdrawals++;
  PeerStats::incrMessageRecvWithdraw(peerIdOdsStr);
}

void AdjRibStats::incrementEnforceFirstAsRejects() {
  totalEnforceFirstAsRejects++;
}

void AdjRibStats::incrementIngressRouteFilterDenied() {
  totalIngressRouteFilterDenied++;
  PeerStats::addPeerIngressRouteFilterDenied(peerIdOdsStr);
}

/*******************************************************************************
 *             AdjRibOut stats functionality
 *******************************************************************************/

void AdjRibStats::incrementSentUpdateMsgs(uint64_t bgpMessageCnt) {
  sentUpdateMsgs += bgpMessageCnt;
  PeerStats::addPeerMessagesSentUpdate(peerIdOdsStr, bgpMessageCnt);
}

void AdjRibStats::incrementSentEndOfRibMsgs(uint64_t bgpMessageCnt) {
  sentEndOfRibMsgs += bgpMessageCnt;
}

void AdjRibStats::incrementSentAnnouncementsIpv4() {
  sentAnnouncementsIpv4++;
  PeerStats::incrMessageSentAnnouncedIpv4(peerIdOdsStr);
}

void AdjRibStats::incrementSentAnnouncementsIpv6() {
  sentAnnouncementsIpv6++;
  PeerStats::incrMessageSentAnnouncedIpv6(peerIdOdsStr);
}

void AdjRibStats::incrementSentWithdrawals() {
  sentWithdrawals++;
  PeerStats::incrMessageSentWithdraw(peerIdOdsStr);
}

void AdjRibStats::incrementPreOutPrefixCount(bool isIpv4) {
  preOutPrefixCount++;
  if (isIpv4) {
    preOutPrefixCountIpv4++;
  } else {
    preOutPrefixCountIpv6++;
  }
  PeerStats::setPeerPreOutPrefixes(peerIdOdsStr, preOutPrefixCount);
}

void AdjRibStats::decrementPreOutPrefixCount(bool isIpv4) {
  if (preOutPrefixCount == 0) {
    /*
     * Underflow guard: clamp instead of wrapping the unsigned counter, matching
     * subtractFromTotalSentPrefixCount. Indicates an increment/decrement
     * accounting bug upstream. Rate-limited so a stuck call site can't flood
     * the log. The per-family counters are already 0 when the aggregate is 0,
     * so leave them untouched.
     */
    XLOGF_EVERY_MS(
        ERR,
        200000,
        "preOutPrefixCount underflow: decrementing below 0 for peer {}",
        peerIdOdsStr);
    return;
  }
  preOutPrefixCount--;
  if (isIpv4) {
    preOutPrefixCountIpv4--;
  } else {
    preOutPrefixCountIpv6--;
  }
  PeerStats::setPeerPreOutPrefixes(peerIdOdsStr, preOutPrefixCount);
}

void AdjRibStats::incrementPostOutPrefixCount(bool isIpv4, uint32_t numPeers) {
  postOutPrefixCount++;
  if (isIpv4) {
    postOutPrefixCountIpv4++;
  } else {
    postOutPrefixCountIpv6++;
  }
  if (totalSentPrefixCount == 0) {
    BgpStats::resetNoPrefixSent();
  }
  totalSentPrefixCount += numPeers;
  PeerStats::setPeerPostOutPrefixes(peerIdOdsStr, postOutPrefixCount);
  PeerStats::setTotalSentPrefixes(totalSentPrefixCount);
  PeerStats::setTotalPaths(totalRcvdPrefixCount + totalSentPrefixCount);
}

void AdjRibStats::decrementPostOutPrefixCount(bool isIpv4, uint32_t numPeers) {
  if (postOutPrefixCount == 0) {
    /*
     * Underflow guard: clamp instead of wrapping the unsigned counter, matching
     * subtractFromTotalSentPrefixCount and decrementPreOutPrefixCount. Return
     * early so this container's counter and the global totalSentPrefixCount
     * cannot diverge silently: with nothing left in this container there is
     * nothing to remove from the global on its behalf. Rate-limited so a stuck
     * call site can't flood the log. The per-family counters are already 0 when
     * the aggregate is 0, so leave them untouched.
     */
    XLOGF_EVERY_MS(
        ERR,
        200000,
        "postOutPrefixCount underflow: decrementing below 0 for peer {}",
        peerIdOdsStr);
    return;
  }
  postOutPrefixCount--;
  if (isIpv4) {
    postOutPrefixCountIpv4--;
  } else {
    postOutPrefixCountIpv6--;
  }
  if (numPeers > totalSentPrefixCount) {
    /* Underflow guard: clamp the global to 0 rather than wrapping. */
    XLOGF_EVERY_MS(
        ERR,
        200000,
        "totalSentPrefixCount underflow: subtracting {} from {} for peer {}",
        numPeers,
        totalSentPrefixCount,
        peerIdOdsStr);
    numPeers = totalSentPrefixCount;
  }
  totalSentPrefixCount -= numPeers;
  PeerStats::setPeerPostOutPrefixes(peerIdOdsStr, postOutPrefixCount);
  PeerStats::setTotalSentPrefixes(totalSentPrefixCount);
  PeerStats::setTotalPaths(totalRcvdPrefixCount + totalSentPrefixCount);
}

void AdjRibStats::copyEgressPrefixCountsFrom(const AdjRibStats& other) {
  postOutPrefixCount = other.postOutPrefixCount;
  postOutPrefixCountIpv4 = other.postOutPrefixCountIpv4;
  postOutPrefixCountIpv6 = other.postOutPrefixCountIpv6;
  preOutPrefixCount = other.preOutPrefixCount;
  preOutPrefixCountIpv4 = other.preOutPrefixCountIpv4;
  preOutPrefixCountIpv6 = other.preOutPrefixCountIpv6;
  PeerStats::setPeerPostOutPrefixes(peerIdOdsStr, postOutPrefixCount);
  PeerStats::setPeerPreOutPrefixes(peerIdOdsStr, preOutPrefixCount);
}

void AdjRibStats::clearEgressPrefixCounts() {
  preOutPrefixCount = 0;
  preOutPrefixCountIpv4 = 0;
  preOutPrefixCountIpv6 = 0;
  postOutPrefixCount = 0;
  postOutPrefixCountIpv4 = 0;
  postOutPrefixCountIpv6 = 0;
  PeerStats::setPeerPostOutPrefixes(peerIdOdsStr, postOutPrefixCount);
  PeerStats::setPeerPreOutPrefixes(peerIdOdsStr, preOutPrefixCount);
}

void AdjRibStats::subtractFromTotalSentPrefixCount(uint32_t count) {
  if (count > totalSentPrefixCount) {
    /*
     * Underflow guard: we are removing more than the global total holds, which
     * indicates an increment/decrement accounting bug upstream. Clamp to avoid
     * wrapping the unsigned counter and log for investigation.
     */
    XLOGF_EVERY_MS(
        ERR,
        200000,
        "totalSentPrefixCount underflow: attempted to subtract {} from {}",
        count,
        totalSentPrefixCount);
    count = totalSentPrefixCount;
  }
  totalSentPrefixCount -= count;
  PeerStats::setTotalSentPrefixes(totalSentPrefixCount);
  PeerStats::setTotalPaths(totalRcvdPrefixCount + totalSentPrefixCount);
}

/*******************************************************************************
 *             Backpressure stats functionality
 *******************************************************************************/

void AdjRibStats::incrementEgressQueueBackpressuredEvents() {
  ++egressQueueBlocks;
}

void AdjRibStats::addEgressQueueBlockDuration(uint64_t duration) {
  egressQueueTotalBlockDurationMs += duration;
}

void AdjRibStats::incrementTransientRouteUpdatesSuppressed() {
  ++transientRouteUpdatesSuppressed;
}

void AdjRibStats::incrementTimesDetachedByBlocking() {
  ++numTimesDetachedByBlocking;
}

void AdjRibStats::incrementTimesDetachedByPolicy() {
  ++numTimesDetachedByPolicy;
}

void AdjRibStats::incrementTimesRejoined() {
  ++numTimesRejoined;
}

void AdjRibStats::setLastEgressQueueBlockTime(uint64_t lastBlockTimeMs) {
  lastEgressQueueBlockTimeMs = lastBlockTimeMs;
}

/*******************************************************************************
 *             Session and attribute stats functionality
 *******************************************************************************/

void AdjRibStats::addPeerSessionStateChanges() {
  PeerStats::addPeerSessionStateChanges(peerIdOdsStr);
}

void AdjRibStats::exportPeerStatus(bool isUp) {
  if (FLAGS_enable_peer_status_logging) {
    PeerStats::setPeerStatus(peerIdOdsStr, isUp ? 1 : 0);
  }
}

void AdjRibStats::setPeerTableVersion(uint64_t version) {
  PeerStats::setPeerTableVersion(peerIdOdsStr, version);
}

void AdjRibStats::updateAttributeSizes(
    const std::shared_ptr<const BgpPath>& attrs) {
  if (attrs) {
    auto asPathSize = attrs->getAsPath().get().size();
    auto communitySize = attrs->getCommunities().get().size();
    auto extendedCommunitySize = attrs->getExtCommunities().get().size();
    auto clusterListSize = attrs->getClusterList().get().size();
    auto topologyInfoSize =
        attrs->getTopologyInfo() ? attrs->getTopologyInfo()->size() : 0;

    PeerStats::addAsPathSizeToAvg(asPathSize);
    PeerStats::addCommunitySizeToAvg(communitySize);
    PeerStats::addExtendedCommunitySizeToAvg(extendedCommunitySize);
    PeerStats::addClusterListSizeToAvg(clusterListSize);
    PeerStats::addTopologyInfoSizeToAvg(topologyInfoSize);
    totalAttributeUpdates++;
  }
}

} // namespace facebook::bgp
