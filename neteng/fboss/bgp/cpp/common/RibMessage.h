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

#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/RouteInfo.h"
#include "neteng/fboss/bgp/cpp/common/Structs.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopStatus.h"

namespace facebook::bgp {

using PrefixPathId = std::tuple<folly::CIDRNetwork, uint32_t>;
using PrefixPathIds = std::vector<PrefixPathId>;

struct RibInAnnouncement {
  const TinyPeerInfo peer;
  const PrefixPathIds pfxPathIds; // v4 or v6 prefixes
  const std::shared_ptr<const BgpPath> attrs;

  RibInAnnouncement(
      const TinyPeerInfo& peer,
      PrefixPathIds pfxPathIds,
      const std::shared_ptr<const BgpPath> attrs)
      : peer(peer), pfxPathIds(std::move(pfxPathIds)), attrs(attrs) {}
};

struct RibInWithdrawal {
  const TinyPeerInfo peer;
  const PrefixPathIds pfxPathIds; // v4 or v6 prefixes

  RibInWithdrawal(const TinyPeerInfo& peer, const PrefixPathIds& pfxPathIds)
      : peer(peer), pfxPathIds(pfxPathIds) {}
};

/*
 * Signal from PeerMgr to Rib to start best-path computation.
 *
 * nexthopResolutionTimeout is how long Rib may wait for all registered nexthops
 * to resolve before running the initial full-sync, so the sync does not wipe
 * FIB routes whose nexthops FSDB has not resolved yet (e.g. a coordinated
 * FSDB+bgpd restart). It is the time left in PeerMgr's EoR budget (eor_time_s)
 * when all peer EoRs were received. Zero means compute immediately without
 * waiting: nexthop tracking is not in effect, or PeerMgr's EoR timer already
 * expired (budget exhausted) and is forcing computation.
 */
struct RibInInitialPathComputation {
  std::chrono::milliseconds nexthopResolutionTimeout{0};

  RibInInitialPathComputation() = default;
  explicit RibInInitialPathComputation(
      std::chrono::milliseconds nexthopResolutionTimeout)
      : nexthopResolutionTimeout(nexthopResolutionTimeout) {}
};

// Get full dump from Rib for newly established peers from PeerMgr to Rib
struct RibDumpReq {
  const nettools::bgplib::BgpPeerId peerId;
  const bool sendAddPath;
  RibDumpReq(
      const nettools::bgplib::BgpPeerId& peerId,
      bool sendAddPath = false)
      : peerId(peerId), sendAddPath(sendAddPath) {}
};

/**
 * @brief: Message to pause the RIB operations
 * @details: This message is sent to Rib to pause the following operations:
 * 1. Best path computation
 * 2. FIB programming
 * 3. Cancels scheduled FibBatchTimer_
 * @param taskName: Name of the task that is pausing the RIB operations
 */
struct PauseBestPathAndFibProgramming {
  RibPauseResumeCause taskName;
  explicit PauseBestPathAndFibProgramming(RibPauseResumeCause taskName)
      : taskName(taskName) {}
};

/**
 * @brief: Message to resume the RIB operations
 * @details: This message is sent to Rib to resume the following operations:
 * 1. Best path computation
 * 2. FIB programming
 * 3. Schedule FibBatchTimer_
 * @param taskName: Name of the task that is resuming the RIB operations
 */
struct ResumeBestPathAndFibProgramming {
  RibPauseResumeCause taskName;
  explicit ResumeBestPathAndFibProgramming(RibPauseResumeCause taskName)
      : taskName(taskName) {}
};

/**
 * @brief Message to convey changes or additions to directly connected nexthops
 * or remote nexthops
 * @details This message is sent to Rib from the nexthopHandler thread
 * @param nexthopStatuses: Vector of NexthopStatus objects containing nexthop IP
 * addresses and their status
 */
struct RibInNexthopUpdate {
  const std::vector<NexthopStatus> nexthopStatuses;

  explicit RibInNexthopUpdate(std::vector<NexthopStatus> nexthopStatuses)
      : nexthopStatuses(std::move(nexthopStatuses)) {}
};

/**
 * @brief Message to convey newly resolved or newly unresolved nexthops
 * @details This message is sent to Rib from NeighborWatcher based on FSDB state
 * updates
 * @param resolved: Vector of resolved nexthop IP addresses
 * @param unresolved: Vector of unresolved nexthop IP addresses
 */
struct NexthopResolutionUpdate {
  explicit NexthopResolutionUpdate(
      std::vector<folly::IPAddress> resolved,
      std::vector<folly::IPAddress> unresolved)
      : resolved(std::move(resolved)), unresolved(std::move(unresolved)) {}

  const std::vector<folly::IPAddress> resolved;
  const std::vector<folly::IPAddress> unresolved;
};

using RibInMessage = std::variant<
    RibInAnnouncement,
    RibInWithdrawal,
    RibInInitialPathComputation,
    PauseBestPathAndFibProgramming,
    ResumeBestPathAndFibProgramming,
    RibInNexthopUpdate,
    NexthopResolutionUpdate>;

struct RibOutAnnouncementEntry {
  // v4 or v6 prefix
  const folly::CIDRNetwork prefix;
  const uint32_t pathIdToSend;

  // Peer & BgpPath of the best entry
  const TinyPeerInfo peer;
  std::shared_ptr<const BgpPath> attrs;

  const std::optional<size_t> switchId{std::nullopt};
  const std::optional<size_t> multiPathSize{std::nullopt};

  // Aggregated UCMP weight of multipath entries
  const std::optional<float> aggregateReceivedUcmpWeight{std::nullopt};
  const std::optional<float> aggregateLocalUcmpWeight{std::nullopt};

  // Rib policy ucmp weight override
  const std::optional<float> ribPolicyUcmpWeight{std::nullopt};

  // A prefix is subjected to out-delay if it is the first time we installed it
  // in FIB.
  bool newlyInstalledInLocalRib{false};

  // Rib entry's installation time stamp
  std::chrono::time_point<std::chrono::system_clock> installTimeStamp;

  /*
   * RIB version when this entry was last modified. Used for tracking
   * how caught up each peer is with RIB state (backpressure visibility).
   */
  uint64_t ribVersion{0};

  bool isPartialDrain{false};

  RibOutAnnouncementEntry(
      const folly::CIDRNetwork& prefix,
      const uint32_t pathIdToSend,
      const TinyPeerInfo& peer,
      std::shared_ptr<const BgpPath> attrs,
      const std::optional<size_t> switchId = std::nullopt,
      const std::optional<size_t> multiPathSize = std::nullopt,
      const std::optional<float>& aggregateReceivedUcmpWeight = std::nullopt,
      const std::optional<float>& aggregateLocalUcmpWeight = std::nullopt,
      const std::optional<float>& ribPolicyUcmpWeight = std::nullopt,
      bool newlyInstalledInLocalRib = false,
      std::chrono::time_point<std::chrono::system_clock> installTimeStamp =
          std::chrono::system_clock::now(),
      uint64_t ribVersion = 0,
      bool isPartialDrain = false)
      : prefix(prefix),
        pathIdToSend(pathIdToSend),
        peer(peer),
        attrs(std::move(attrs)),
        switchId(switchId),
        multiPathSize(multiPathSize),
        aggregateReceivedUcmpWeight(aggregateReceivedUcmpWeight),
        aggregateLocalUcmpWeight(aggregateLocalUcmpWeight),
        ribPolicyUcmpWeight(ribPolicyUcmpWeight),
        newlyInstalledInLocalRib(newlyInstalledInLocalRib),
        installTimeStamp(installTimeStamp),
        ribVersion(ribVersion),
        isPartialDrain(isPartialDrain) {}
};

struct RibOutWithdrawalEntry {
  const folly::CIDRNetwork prefix; // v4 or v6 prefix
  const uint32_t pathIdToSend;
  std::optional<folly::IPAddress> nh; // TODO: deprecate this field once
                                      // ADD-PATH changes are completed
  /*
   * RIB version when this withdrawal was generated. Used for tracking
   * how caught up each peer is with RIB state (backpressure visibility).
   */
  uint64_t ribVersion{0};

  explicit RibOutWithdrawalEntry(
      const folly::CIDRNetwork& prefix,
      const uint32_t pathIdToSend,
      std::optional<folly::IPAddress> nh = std::nullopt,
      uint64_t ribVersion = 0)
      : prefix(prefix),
        pathIdToSend(pathIdToSend),
        nh(nh),
        ribVersion(ribVersion) {}
};

struct RibOutAnnouncement {
  std::vector<RibOutAnnouncementEntry> entries;
  bool sendWithEoR{false};
  bool initialDump{false}; // Indicates initial dump request's response
  std::vector<RibOutAnnouncementEntry> addPathEntries;
};

struct RibOutWithdrawal {
  std::vector<RibOutWithdrawalEntry> entries;
  std::vector<RibOutWithdrawalEntry> addPathEntries;
};

struct ShadowRibRouteInfo {
  const TinyPeerInfo peer;
  const std::shared_ptr<const BgpPath> attrs;
  uint8_t flags{0};
  const uint32_t pathIdToSend;
  const bool isPartialDrain{false};

  ShadowRibRouteInfo(
      const TinyPeerInfo& peer,
      std::shared_ptr<const BgpPath> attrs,
      const uint32_t pathIdToSend,
      bool isPartialDrain = false)
      : peer(peer),
        attrs(std::move(attrs)),
        pathIdToSend(pathIdToSend),
        isPartialDrain(isPartialDrain) {}
};

/*
 * SR routeInfos are keyed by nexthop: RibOutWithdrawal messages carry the
 * nexthop, so keying by it avoids a search or a separate lookup table. The
 * uint32_t alternative of PathId is a remnant of the removed rib-allocated
 * path-id path (getPathId now always returns the folly::IPAddress alternative);
 * collapsing PathId to just folly::IPAddress is a separate follow-up cleanup.
 */
using PathId = std::variant<uint32_t, /* pathIdToSend */ folly::IPAddress>;
using ShadowRibRouteInfos =
    std::unordered_map<PathId, std::shared_ptr<ShadowRibRouteInfo>>;

/*
 * @brief  Following set represents bit position for each
 *         flag representing state of a specific routeinfo
 *         (path) in a ShadowRibEntry
 */
/*
 * This path is updated which yet to be consumed by all the
 * consumers. The flag will be reset as soon that update is
 * consumed by all active consumers
 */
#define SHADOWRIBROUTE_IN_UPDATE 0x1
/*
 * This path is withdrawn which yet to be consumed by all the
 * consumers. The path will be removed from ShadowRibEntry as
 * soon withdrawal is consumed by all active consumers
 */
#define SHADOWRIBROUTE_IN_WITHDRAW 0x2

/*
 * @brief  set passed bitmap values in the srRoute
 *
 * @param  srRoute  a specific path of srEntry to be updated
 * @param  flag     set of bitmaps to be updated (it does not
 *                  reset any existing set bits)
 *
 * @return void
 */
inline void setShadowRibRouteState(
    std::shared_ptr<ShadowRibRouteInfo>& srRoute,
    uint8_t flag) {
  srRoute->flags |= flag;
}

/*
 * @brief  reset passed bitmap values in the srRoute
 *
 * @param  srRoute  a specific path of srEntry to be updated
 * @param  flag     set of bitmaps to be reset (it does not
 *                  reset any other set bits)
 *
 * @return void
 */
inline void resetShadowRibRouteState(
    std::shared_ptr<ShadowRibRouteInfo>& srRoute,
    uint8_t flag) {
  srRoute->flags &= ~flag;
}

/*
 * Is in update bit flag set
 */
inline bool isShadowRibRouteInUpdate(uint8_t flags) {
  return (flags & SHADOWRIBROUTE_IN_UPDATE);
}

/*
 * Is in withdraw bit flag set
 */
inline bool isShadowRibRouteInWithdraw(uint8_t flags) {
  return (flags & SHADOWRIBROUTE_IN_WITHDRAW);
}

/*
 * ShadowRibOutAnnouncementEntry, aka, ShadowRibEntry will contain all
 * information needed for handling a RibDumpReq and RouteRefresh request.
 *
 * One ShadowRibEntry will contain: bestpath, ECMP(for add-path).
 */
struct ShadowRibOutAnnouncementEntry {
  folly::CIDRNetwork prefix; // v4 or v6 prefix with mask

  /*
   * RouteInfo struct contains all required information(TinyPeerInfo, BgpPath,
   * etc.)
   *
   * TODO: multi-path will be keyed by nexthop for now while the sendPathId is
   * under development. Will switch to use pathId to differentiate paths.
   */
  std::shared_ptr<ShadowRibRouteInfo> bestpath;
  ShadowRibRouteInfos multipaths;

  /*
   * Shared information for the same ShadowRibEntry regardless of path.
   */
  std::optional<size_t> switchId{std::nullopt};
  std::optional<size_t> multiPathSize{std::nullopt};

  // Aggregated UCMP weight of multipath entries
  std::optional<float> aggregateReceivedUcmpWeight{std::nullopt};
  std::optional<float> aggregateLocalUcmpWeight{std::nullopt};

  // Rib policy ucmp weight override
  std::optional<float> ribPolicyUcmpWeight{std::nullopt};

  // A prefix is subjected to out-delay if it is the first time we installed it
  // in FIB.
  bool newlyInstalledInLocalRib{false};

  // Rib entry's installation time stamp
  std::chrono::time_point<std::chrono::system_clock> installTimeStamp;

  // Number of times entry has been published to changeListTracker
  uint32_t publishCount{0};

  /*
   * RIB version when this entry was last modified. Used for CLI display
   * and tracking how caught up each peer is with RIB state.
   */
  uint64_t ribVersion{0};

  // Constructor
  ShadowRibOutAnnouncementEntry() {}
  ShadowRibOutAnnouncementEntry(
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<ShadowRibRouteInfo> bestpath,
      ShadowRibRouteInfos multipaths,
      const std::optional<size_t> switchId = std::nullopt,
      const std::optional<size_t> multiPathSize = std::nullopt,
      const std::optional<float>& aggregateReceivedUcmpWeight = std::nullopt,
      const std::optional<float>& aggregateLocalUcmpWeight = std::nullopt,
      const std::optional<float>& ribPolicyUcmpWeight = std::nullopt,
      bool newlyInstalledInLocalRib = false,
      std::chrono::time_point<std::chrono::system_clock> installTimeStamp =
          std::chrono::system_clock::now(),
      uint64_t ribVersion = 0)
      : prefix(prefix),
        bestpath(std::move(bestpath)),
        multipaths(std::move(multipaths)),
        switchId(switchId),
        multiPathSize(multiPathSize),
        aggregateReceivedUcmpWeight(aggregateReceivedUcmpWeight),
        aggregateLocalUcmpWeight(aggregateLocalUcmpWeight),
        ribPolicyUcmpWeight(ribPolicyUcmpWeight),
        newlyInstalledInLocalRib(newlyInstalledInLocalRib),
        installTimeStamp(installTimeStamp),
        ribVersion(ribVersion) {}
};

struct ShadowRibOutAnnouncement {
  std::vector<ShadowRibOutAnnouncementEntry> entries;
  bool sendWithEoR{false};
  bool initialDump{false};
};

/*
 * ShadowRibOutWithdrawalEntry will contain all information needed to send a
 * withdrawal for different nexthops.
 */
struct ShadowRibOutWithdrawalEntry {
  folly::CIDRNetwork prefix; // v4 or v6 prefix with mask
  std::vector<folly::IPAddress> nexthops; // bestpath and multipaths nexthops
};

struct ShadowRibOutWithdrawal {
  std::vector<ShadowRibOutWithdrawalEntry> entries;
};

using ShadowRibEntry = ShadowRibOutAnnouncementEntry;

/*
 * EoR marker message which is consumed only by PeerManagerBase during
 * BGP initialization.
 * This message is expected to come after the
 * last announcement enqueued to ribOutQ_ of the Fib programmed
 * prefixes during initialization, also referred to as the initial dump.
 */
struct RibInitialAnnouncementStart {};

using RibOutMessage = std::variant<
    RibOutAnnouncement,
    RibOutWithdrawal,
    ShadowRibOutAnnouncement,
    ShadowRibOutWithdrawal,
    RibInitialAnnouncementStart>;

} // namespace facebook::bgp
