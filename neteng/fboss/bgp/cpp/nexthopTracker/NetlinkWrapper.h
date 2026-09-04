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

#include <atomic>
#include <chrono>
#include <optional>

#include <folly/CancellationToken.h>
#include <folly/Synchronized.h>
#include <folly/fibers/TimedMutex.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/memory/not_null.h>
#include <gflags/gflags.h>
#include <openr/messaging/ReplicateQueue.h>
#include <openr/nl/NetlinkProtocolSocket.h>
#include <re2/re2.h>
#include "neteng/fboss/bgp/cpp/lib/fibers/Queue.h"

#include "neteng/fboss/bgp/cpp/common/BgpModuleBase.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/common/Types.h"
#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/InterfaceEntry.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/InterfacePrefixTable.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopCache.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"
#include "openr/if/gen-cpp2/FibService.h"
#include "openr/if/gen-cpp2/FibServiceAsyncClient.h"

DECLARE_int32(bgp_netlink_link_up_hold_initial_ms);
DECLARE_int32(bgp_netlink_link_up_hold_max_ms);

namespace facebook::bgp {
/**
 * NetlinkWrapper will be run on its own thread where it will query and react to
 * netlink events pertaining to directly connected next-hops reachability. It
 * will send this reachability information to the Nexthop Cache, where updates
 * in nexthops' reachability will be used to determine best path calculation in
 * the RIB.
 */
class NetlinkWrapper : public BgpModuleBase {
 public:
  NetlinkWrapper(
      std::shared_ptr<NexthopCache> nexthopCache,
      nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
      openr::messaging::ReplicateQueue<openr::fbnl::NetlinkEvent>&
          netlinkEventsQueue,
      const std::vector<std::string>& includeInterfaceRegexes,
      const EnableNetlinkDampening enableNetlinkDampening =
          EnableNetlinkDampening{false},
      std::optional<std::chrono::milliseconds> syncInterval = std::nullopt,
      const int32_t openrFibAgentPort = kDefaultFibAgentPort);
  ~NetlinkWrapper() override = default;

  void run() noexcept override;
  void stop() noexcept override;
  void updateEntryStats(
      neteng::fboss::bgp::thrift::TEntryStats& stats) const noexcept;

  // The length of the first hold, and the maximum hold length.
  struct HoldTimes {
    std::chrono::milliseconds initial;
    std::chrono::milliseconds max;
  };

  /**
   * Set the link-up hold times. Any thread can call this.
   *
   * The netlink fiber reads the new values at the next link-down, so a change
   * takes effect then. A hold that already started keeps its length, and the
   * longest that can last is the previous max.
   *
   * The caller must check the range. The values must be more than 0, and max
   * must be at least initial.
   */
  void setLinkUpHoldTimes(
      std::chrono::milliseconds initial,
      std::chrono::milliseconds max) noexcept;

  /**
   * True when the link-up hold is on, from
   * BgpSettingConfig.enable_netlink_dampening. Any thread can call this,
   * because the value is fixed at construction.
   */
  bool isNetlinkDampeningEnabled() const noexcept {
    return enableNetlinkDampening_;
  }

  /** The link-up hold times that the netlink fiber uses now. */
  HoldTimes getLinkUpHoldTimes() const noexcept;

 private:
  /**
   * Fiber task for syncing interfaces with retries and timeout handling.
   */
  void syncInterfaceTask();

  /**
   * Used on initialization in order to get a current snapshot of all interfaces
   * and their reachability status. This calls getAllInterfaces(). If
   * interfaces are returned, we will update internal datastructures interfaces_
   * and ifIndexToName_, and then call addOrUpdateInterfaces() to update the
   * NexthopCache. If sync is unsuccessful, we will log a warning, then retry.
   */
  bool syncInterfaces();

  /**
   * Queries netlink for all interfaces and their reachability status.
   */
  folly::F14NodeMap<int, InterfaceEntry> getAllInterfaces();

  /**
   * Checks if ifName matches the regex set by the thrift config.
   */
  bool isIfNameRegexMatch(std::string const& ifName) const;
  /**
   * Helper function used in ctor to compile regex set.
   */
  std::shared_ptr<re2::RE2::Set> compileRegexSet(
      const std::vector<std::string>& includeInterfaceRegexes);

  /**
   * [Netlink Event]
   * We will process 3 different types of netlink events
   * - Link Events
   * - Address Events
   * - Neighbor Events
   */
  void processLinkEvent(openr::fbnl::Link&& link);
  void processIfAddressEvent(openr::fbnl::IfAddress&& addr);
  void processNeighborEvent(openr::fbnl::Neighbor&& nbr);

  /**
   * Helper function to update NexthopCache with interface reachability status.
   */
  void addOrUpdateNextHopStatus(
      const InterfaceEntry& interface,
      std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now());

  /**
   * @brief Send one batch to nexthopCache_, ribInQ_ and the OpenR FIB agent.
   *
   * @details Makes two calls. updateCacheAndNotifyRib applies vec to
   * nexthopCache_, and the statuses the cache reports as changed go on to
   * ribInQ_ as one RibInNexthopUpdate, so a batch that changes nothing sends no
   * RIB message. enqueueConnectedNextHopStatus then puts the whole
   * thriftStatuses batch on pendingFibOpenrUpdates_, which the OpenR update
   * fiber reads before calling the FIB agent. An empty vec returns at once and
   * no consumer gets a message.
   *
   * Both queues are bounded -- pendingFibOpenrUpdates_ holds
   * kFibOpenrMaxPendingUpdates (1000) requests -- so a full queue suspends the
   * fiber until a consumer frees a slot. That is why the name says fiber
   * context: on a plain thread the same waits block the EventBase instead.
   *
   * The caller must hold publishLock_. This function does not take the lock,
   * because folly::fibers::TimedMutex is not recursive.
   *
   * @param vec Nexthop statuses for the cache and the RIB
   * @param thriftStatuses The same statuses in the shape the FIB agent takes
   */
  void publishStatusesWithFiberContext(
      std::vector<NexthopStatus>&& vec,
      std::vector<openr::thrift::ConnectedNextHopStatus>&& thriftStatuses);

  /*
   * The state of every hold at one time point. getLinkHoldState fills it in
   * one walk of interfaces_, and releaseEndedHolds acts on it.
   */
  struct LinkHoldState {
    // Interfaces whose hold end time passed.
    std::vector<std::string> ifNamesToRelease;
    // The earliest hold that did not end. Empty when no interface holds.
    std::optional<std::chrono::steady_clock::time_point> nextEndTime;
    // Interfaces that have a hold, for interfaceHoldCount_.
    int64_t heldCount{0};
  };

  /**
   * One walk of interfaces_ at time now. The caller must hold publishLock_.
   */
  LinkHoldState getLinkHoldState(
      std::chrono::steady_clock::time_point now) const;

  /**
   * Release each hold that ended, and publish those interfaces in one batch.
   * Return the earliest hold end time that did not pass, so that the caller
   * can start a timer for it.
   */
  std::optional<std::chrono::steady_clock::time_point> releaseEndedHolds();

  /**
   * @brief Release the holds that ended, then arm holdReleaseTimer_ for the
   *        earliest one still pending.
   *
   * @details Both callers reach this: processLinkEvent when a hold starts, and
   * holdReleaseTimer_ itself when it fires. Arming for the earliest pending
   * hold on every call is what stops a hold that ends sooner from waiting
   * behind one that ends later. With no hold left, releaseEndedHolds returns
   * an empty time and the timer stays disarmed.
   */
  void releaseAndReschedule() noexcept;

  /**
   * Pull resolution of a single nexthop, used only on the interface-state path
   * (bgp_resolve_nexthops_from_interface_state).
   *
   * If the nexthop is directly connected (one best match against the global
   * interface-prefix table), pushes its current reachability into the cache so
   * the RIB gets an answer -- reachable if the covering interface is up,
   * unreachable+connected otherwise (which hides the route until the covering
   * interface comes up) -- and returns true. If it is not directly connected,
   * this leaves the entry untouched so the recursive/FIB path can own it, and
   * returns false.
   *
   * Reachability is answered purely from interface link state (isInterfaceUp);
   * the kernel neighbor (ARP/ND) table is not consulted.
   */
  bool evaluateNexthop(const folly::IPAddress& nexthopIp);

  /**
   * True if the given interface index maps to a tracked interface that is
   * currently up. Drives interface-state reachability in evaluateNexthop.
   */
  bool isInterfaceUp(int ifIndex) const;

  /**
   * @brief Read the kernel neighbor table and apply one interface's entries.
   *
   * @details Netlink reports only a change of neighbor state. updateAddr adds
   * the host IPs of a new prefix as unreachable. The kernel has no change to
   * report for a neighbor it resolved earlier, so no event follows and this
   * function reads that state from the kernel directly. getAllNeighbors()
   * returns every interface's entries, so the result is filtered to the
   * interface's ifIndex and the rest discarded.
   *
   * Only the netlink event fiber calls this function. It suspends that fiber
   * until the kernel answers, while evb_ continues to run the other fibers.
   * The caller then publishes through addOrUpdateNextHopStatus.
   *
   * @param interfaceEntry Tracked interface whose neighbor entries are applied
   */
  void readNeighborsForInterface(InterfaceEntry& interfaceEntry);

  /**
   * Helper function to get or create an interface entry
   */
  InterfaceEntry& getOrCreateInterfaceEntry(const std::string& ifName);

  /**
   * Method to create a thrift client and connect to OpenR FIB agent
   */
  void connectOpenrFibAgent();

  /**
   * Method to disconnect OpenR FIB agent
   */
  void disconnectOpenrFibAgent();

  /**
   * Update OpenR FIB agent with connected next hop status.
   * This function sends the request to the OpenR FIB agent and blocks the fiber
   * (not the thread) while waiting for a response.
   */
  void updateOpenrConnectedNextHopStatus(
      const openr::thrift::ConnectedNextHopStatusRequest& request);

  /**
   * Update nexthop cache and push changed statuses to ribInQ.
   */
  void updateCacheAndNotifyRib(const std::vector<NexthopStatus>& updates);

  /**
   * Helper function to enqueue OpenR FIB agent updates
   */
  void enqueueConnectedNextHopStatus(
      openr::thrift::ConnectedNextHopStatusRequest&& request);

  // Fiber manager
  folly::fibers::FiberManager& fm_;

  // Vector to store all fiber tasks
  std::vector<folly::Future<folly::Unit>> workers_{};

  // Shared pointer to the NexthopCache to update
  folly::not_null_shared_ptr<NexthopCache> nexthopCache_;

  // Reference to the RibInMessage queue to send nexthop updates
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ_;

  // Queue to listen to LINK/ADDRESS/NEIGHBOR events from netlink
  openr::messaging::RQueue<openr::fbnl::NetlinkEvent> netlinkEventsRQueue_;

  // Interface index to name. Used to resolve ifIndex on address and neighbor
  // events
  folly::F14NodeMap<int64_t, std::string> ifIndexToName_;

  // Interface index to Interface Entry. Used to store tracked interfaces.
  folly::F14NodeMap<std::string, InterfaceEntry> interfaces_;

  // Global table of local interface prefixes across all tracked interfaces.
  // Classifies whether a nexthop is directly connected in a single
  // longest-prefix (best) match. Only populated when
  // bgp_resolve_nexthops_from_interface_state is on.
  InterfacePrefixTable prefixes_;

  // Pointer to interface with NetlinkProtocolSocket
  folly::not_null_shared_ptr<openr::fbnl::NetlinkProtocolSocket> nlSock_;

  // Cancellation source for the sync interface task (used for testing)
  folly::CancellationSource syncInterfaceTaskCanceler_;

  /**
   * @brief Whether the link-up hold is on.
   *
   * @details Set from BgpSettingConfig.enable_netlink_dampening, and forced
   * false on the bgp_resolve_nexthops_from_interface_state path where the hold
   * has no effect. Read where a hold starts and where a hold ends, but never in
   * canUseLink, so a hold that has already started can always end.
   */
  const EnableNetlinkDampening enableNetlinkDampening_;

  /**
   * @brief Fires at the earliest hold end time to release the holds that ended.
   *
   * @details Created in run() only when enableNetlinkDampening_ is set, and
   * armed by releaseAndReschedule. The callback hands the work to a fiber
   * rather than doing it inline, because the release reaches
   * ribInQ_.fiberPush, which suspends when the queue is full.
   */
  std::unique_ptr<folly::AsyncTimeout> holdReleaseTimer_;

  /**
   * @brief The length of the first hold, and the ceiling the ladder doubles to.
   *
   * @details Seeded from the gflags at construction. Both are read at each
   * link-down, so a new value takes effect at the next flap and a hold that has
   * already started keeps its length. Atomic because any thread may write them.
   *
   * One lock covers both values. setLinkUpHoldTimes checks that max is at
   * least initial, so a reader that took one new value beside one old value
   * could see a pair the setter never accepted.
   */
  folly::Synchronized<HoldTimes> holdTimes_{HoldTimes{
      std::chrono::milliseconds(FLAGS_bgp_netlink_link_up_hold_initial_ms),
      std::chrono::milliseconds(FLAGS_bgp_netlink_link_up_hold_max_ms)}};

  /**
   * @brief The number of interfaces that currently have a hold.
   *
   * @details Written only by releaseEndedHolds, which counts the holds it walks
   * past. The thrift worker and the health thread read it without hopping to
   * this EventBase, so it is atomic and they never walk interfaces_ instead.
   */
  std::atomic<int64_t> interfaceHoldCount_{0};

  /*
   * One publish makes three changes, and this lock keeps them together:
   *
   * 1. the nexthopCache_ write
   * 2. the ribInQ_ push
   * 3. the pendingFibOpenrUpdates_ enqueue
   *
   * Two paths publish. addOrUpdateNextHopStatus publishes for one netlink
   * event. releaseEndedHolds publishes for the holds that ended, and the hold
   * timer runs it on its own fiber.
   *
   * Steps 2 and 3 can suspend the fiber. Without this lock, a publish that
   * stops between the steps lets a later publish finish first, and leaves the
   * older value in the consumers.
   */
  folly::fibers::TimedMutex publishLock_;

  // RE2 patterns for interface name matching in nexthop tracking
  std::shared_ptr<re2::RE2::Set> includeInterfaceRegexSet_;

  // Time interval between syncs and timeout
  std::chrono::milliseconds syncReadTimeout_;

  // Port number to connect to OpenR FIB agent
  const int32_t openrFibAgentPort_;

  // OpenR FIB agent client
  std::unique_ptr<apache::thrift::Client<openr::thrift::FibService>> client_;

  // Queue for pending OpenR FIB agent updates (fiber-aware bounded queue)
  // Uses folly::fibers::Baton for fiber-level blocking when queue is full/empty
  nettools::bgplib::RWQueue<openr::thrift::ConnectedNextHopStatusRequest>
      pendingFibOpenrUpdates_;

// Per class placeholder for test code injection
// only need to be setup once here
#ifdef NetlinkWrapper_TEST_FRIENDS
  NetlinkWrapper_TEST_FRIENDS
#endif
};

} // namespace facebook::bgp
