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
#include <memory>
#include <string>

#include <folly/IPAddress.h>
#include <folly/container/F14Set.h>

#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"

namespace facebook::bgp {

class BgpPath;

// Adjacency Rib stats - global counters shared across all peers
extern uint32_t totalRcvdPrefixCount;
extern uint32_t totalSentPrefixCount;
extern uint32_t totalVipPrefixesCount;
extern uint32_t totalAcceptedPrefixCount;
extern uint32_t totalDroppedPrefixCount;
extern uint32_t maxPeerRcvdPrefixCount;

class AdjRibStats {
 public:
  explicit AdjRibStats(const std::string& peerIdOdsStr)
      : peerIdOdsStr(peerIdOdsStr) {
    PeerStats::initPeerCounters(peerIdOdsStr);
  }

  /*******************************************************************************
   *             Start   -    AdjRibIn stats functionality
   *******************************************************************************/
  void incrementPreInPrefixCount(
      const folly::CIDRNetwork& prefix,
      const bool isVipPrefix,
      const bool isGoldenVip);
  void decrementPreInPrefixCount(const folly::CIDRNetwork& prefix);
  uint32_t getPreInPrefixCount() const {
    return preInPrefixCount;
  }
  uint32_t getPreInPrefixCountIpv4() const {
    return preInPrefixCountIpv4;
  }
  uint32_t getPreInPrefixCountIpv6() const {
    return preInPrefixCountIpv6;
  }
  void incrementPostInPrefixCount(bool isIpv4);
  void decrementPostInPrefixCount(bool isIpv4);
  uint32_t getPostInPrefixCount() const {
    return postInPrefixCount;
  }
  uint32_t getPostInPrefixCountIpv4() const {
    return postInPrefixCountIpv4;
  }
  uint32_t getPostInPrefixCountIpv6() const {
    return postInPrefixCountIpv6;
  }
  uint64_t getRecvUpdateMsgs() const {
    return recvUpdateMsgs;
  }
  void incrementRecvUpdateMsgs();

  // Control-plane count of EoR PDUs received (a distinct PDU from UPDATE);
  // converges with the socket-layer socket_rx_eor_msgs.
  uint64_t getRecvEndOfRibMsgs() const {
    return recvEndOfRibMsgs;
  }
  void incrementRecvEndOfRibMsgs();

  uint64_t getRecvAnnouncementsIpv4() const {
    return recvAnnouncementsIpv4;
  }
  uint64_t getRecvAnnouncementsIpv6() const {
    return recvAnnouncementsIpv6;
  }
  uint64_t getRecvWithdrawals() const {
    return recvWithdrawals;
  }
  uint64_t getEnforceFirstAsRejects() const {
    return totalEnforceFirstAsRejects;
  }
  void incrementRecvAnnouncementsIpv4();
  void incrementRecvAnnouncementsIpv6();
  void incrementRecvWithdrawals();
  void incrementEnforceFirstAsRejects();
  void incrementIngressRouteFilterDenied();
  uint64_t getTotalIngressRouteFilterDenied() const {
    return totalIngressRouteFilterDenied;
  }

  /*
   * Per-peer count of received routes dropped because the peer reached its
   * configured pre-filter max prefix limit (RouteLimit.max_routes). This is
   * reset on session down via clear(). A non-zero value means the peer is
   * actively shedding received (PR) routes, which is a more reliable signal
   * than comparing the live prefix count against the limit (the count can churn
   * back below the limit while drops continue).
   */
  void incrementPreFilterDroppedRouteCount();
  uint32_t getPreFilterDroppedRouteCount() const {
    return preFilterDroppedRouteCount;
  }
  /*******************************************************************************
   *             End   -    AdjRibIn stats functionality
   *******************************************************************************/

  void updateAttributeSizes(const std::shared_ptr<const BgpPath>& attrs);
  uint64_t getTotalAttributeUpdates() const {
    return totalAttributeUpdates;
  }

  uint64_t getSentUpdateMsgs() const {
    return sentUpdateMsgs;
  }
  void incrementSentUpdateMsgs(uint64_t bgpMessageCnt);

  // EoR is a distinct BGP PDU from UPDATE; tracked separately so the
  // control-plane count converges with the socket-layer txMsgs.endOfRib and
  // stays out of sentUpdateMsgs (which must converge with txMsgs.update).
  uint64_t getSentEndOfRibMsgs() const {
    return sentEndOfRibMsgs;
  }
  void incrementSentEndOfRibMsgs(uint64_t bgpMessageCnt);

  // Reset cumulative egress (sent) message counts (update/EoR/announcements/
  // withdrawals). Does NOT touch the live prefix gauges (postOut...), which
  // reflect current advertised state rather than deltas.
  void clearEgressMessageCounts();

  // Reset cumulative ingress (recv) message counts (update/EoR/announcements/
  // withdrawals). Does NOT touch the live prefix gauges.
  void clearIngressMessageCounts();

  uint64_t getSentAnnouncementsIpv4() const {
    return sentAnnouncementsIpv4;
  }
  uint64_t getSentAnnouncementsIpv6() const {
    return sentAnnouncementsIpv6;
  }
  uint64_t getSentWithdrawals() const {
    return sentWithdrawals;
  }
  void incrementSentAnnouncementsIpv4();
  void incrementSentAnnouncementsIpv6();
  void incrementSentWithdrawals();

  /*
   * AdjRibStats is held by both AdjRibGroup and AdjRib. Some fields are
   * global aggregates across all peers (e.g. totalSentPrefixCount), while
   * others are per-container (e.g. postOutPrefixCount tracks prefixes for
   * each in-sync peer individually). numPeers controls how much to adjust
   * totalSentPrefixCount: an AdjRibGroup passes the number of in-sync
   * peers since a group prefix is sent to all of them, while an AdjRib
   * passes 1 (default).
   */
  void incrementPostOutPrefixCount(bool isIpv4, uint32_t numPeers = 1);
  void decrementPostOutPrefixCount(bool isIpv4, uint32_t numPeers = 1);
  uint32_t getPostOutPrefixCount() const {
    return postOutPrefixCount;
  }
  uint32_t getPostOutPrefixCountIpv4() const {
    return postOutPrefixCountIpv4;
  }
  uint32_t getPostOutPrefixCountIpv6() const {
    return postOutPrefixCountIpv6;
  }

  void incrementPreOutPrefixCount(bool isIpv4);
  void decrementPreOutPrefixCount(bool isIpv4);
  uint32_t getPreOutPrefixCount() const {
    return preOutPrefixCount;
  }
  uint32_t getPreOutPrefixCountIpv4() const {
    return preOutPrefixCountIpv4;
  }
  uint32_t getPreOutPrefixCountIpv6() const {
    return preOutPrefixCountIpv6;
  }

  void copyEgressPrefixCountsFrom(const AdjRibStats& other);

  /*
   * Reset this container's egress (out) prefix counts to zero without touching
   * the global totalSentPrefixCount. Used when a detached peer rejoins its
   * group: the peer stops counting independently and folds back into the
   * group's shared accounting, so its snapshot counts are cleared while the
   * global total (which already reflects this peer's advertisements, now
   * tracked via the group) is left as-is.
   */
  void clearEgressPrefixCounts();

  /*
   * Subtract a departed peer's advertised-prefix contribution from the global
   * totalSentPrefixCount, without altering any per-container
   * postOutPrefixCount. Used on peer-down under update groups: an in-sync peer
   * removes the group's postOutPrefixCount (it shared the group's RIB-OUT), a
   * detached peer removes its own. The per-container counts are intentionally
   * left untouched -- the group keeps advertising to its remaining in-sync
   * peers, and a detached peer's snapshot is discarded together with the peer.
   */
  void subtractFromTotalSentPrefixCount(uint32_t count);

  void incrementEgressQueueBackpressuredEvents();

  uint32_t getEgressQueueBackpressuredEvents() const {
    return egressQueueBlocks;
  }

  void addEgressQueueBlockDuration(uint64_t duration);

  uint64_t getEgressQueueTotalBlockDuration() const {
    return egressQueueTotalBlockDurationMs;
  }

  void incrementTransientRouteUpdatesSuppressed();

  uint64_t getTransientRouteUpdatesSuppressed() const {
    return transientRouteUpdatesSuppressed;
  }

  /*
   * Cumulative count of how many times this peer was detached from its group
   * (broken down by reason) and how many times it rejoined. Unlike the group's
   * numPeersDetachedAfterJoin_ gauge (which rises on detach and falls on
   * rejoin/down), these monotonically increase over the session.
   */
  void incrementTimesDetachedByBlocking();
  void incrementTimesDetachedByPolicy();
  void incrementTimesRejoined();
  uint64_t getNumTimesDetachedByBlocking() const {
    return numTimesDetachedByBlocking;
  }
  uint64_t getNumTimesDetachedByPolicy() const {
    return numTimesDetachedByPolicy;
  }
  uint64_t getNumTimesRejoined() const {
    return numTimesRejoined;
  }

  void setLastEgressQueueBlockTime(uint64_t lastBlockTimeMs);

  uint64_t getLastEgressQueueBlockTime() const {
    return lastEgressQueueBlockTimeMs;
  }

  void addPeerSessionStateChanges();

  void exportPeerStatus(bool isUp);

  void setPeerTableVersion(uint64_t version);

  /* Reset per-session stats to initial state. */
  void clear();

  const std::string& getPeerIdOdsStr() const {
    return peerIdOdsStr;
  }

 private:
  folly::F14FastSet<folly::CIDRNetwork> vipPrefixes;
  uint32_t preInPrefixCount{0}; // Number of prefixes received pre-policy
  uint32_t postInPrefixCount{0}; // Number of prefixes received post-policy
  uint32_t preOutPrefixCount{0}; // Number of prefixes advertised pre-policy
  uint32_t postOutPrefixCount{0}; // Number of prefixes advertised post-policy
  uint32_t preInPrefixCountIpv4{0};
  uint32_t preInPrefixCountIpv6{0};
  uint32_t postInPrefixCountIpv4{0};
  uint32_t postInPrefixCountIpv6{0};
  uint32_t preOutPrefixCountIpv4{0};
  uint32_t preOutPrefixCountIpv6{0};
  uint32_t postOutPrefixCountIpv4{0};
  uint32_t postOutPrefixCountIpv6{0};
  uint64_t sentUpdateMsgs{0}; // Number of update messages sent
  uint64_t sentEndOfRibMsgs{0}; // Number of EoR messages sent (distinct PDU)
  uint64_t recvUpdateMsgs{0}; // Number of update messages recv
  uint64_t recvEndOfRibMsgs{0}; // Number of EoR messages recv (distinct PDU)
  uint64_t sentAnnouncementsIpv4{0}; // Number of update annoucements sent ipv4
  uint64_t sentAnnouncementsIpv6{0}; // Number of update annoucements sent ipv6
  uint64_t recvAnnouncementsIpv4{0}; // Number of update annoucements recv ipv4
  uint64_t recvAnnouncementsIpv6{0}; // Number of update annoucements recv ipv6
  uint64_t sentWithdrawals{0}; // Number of update withdrawals sent
  uint64_t recvWithdrawals{0}; // Number of update withdrawals recv
  uint64_t totalAttributeUpdates{0}; // Number of attr updates, for testing
  uint64_t totalEnforceFirstAsRejects{0}; // Number of time the validation fails
  uint64_t totalIngressRouteFilterDenied{
      0}; // Number of ingress routes denied by CRF
  uint32_t preFilterDroppedRouteCount{
      0}; // Routes dropped by pre_filter max_routes cap

  uint32_t egressQueueBlocks{0};
  uint64_t egressQueueTotalBlockDurationMs{0};
  uint64_t lastEgressQueueBlockTimeMs{0};
  uint64_t transientRouteUpdatesSuppressed{0};
  uint64_t numTimesDetachedByBlocking{0}; // Cumulative blocking detachments
  uint64_t numTimesDetachedByPolicy{0}; // Cumulative policy detachments
  uint64_t numTimesRejoined{0}; // Cumulative rejoins into the group

  const std::string peerIdOdsStr;

#ifdef AdjRibStats_TEST_FRIENDS
  AdjRibStats_TEST_FRIENDS
#endif
};

} // namespace facebook::bgp
