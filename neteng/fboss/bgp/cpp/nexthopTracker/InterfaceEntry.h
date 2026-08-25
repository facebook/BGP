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
#include <optional>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/io/async/AsyncTimeout.h>
#include <gflags/gflags.h>

/*
 * Gate for the interface-link-state directly-connected nexthop resolution path.
 * Defined in InterfaceEntry.cpp, which lives in the interface_entry library
 * that is linked only into the BB/EBB daemon (bgpd_cpp_bb), so the flag cannot
 * be enabled on the DC binary. Declared here (rather than in an implementation
 * file) so every consumer in the BB-only netlink path picks it up from one
 * place.
 */
DECLARE_bool(bgp_resolve_nexthops_from_interface_state);

namespace facebook::bgp {

/**
 * Holds interface attributes, and detect changes in interface on every update
 */
class InterfaceEntry final {
 public:
  explicit InterfaceEntry(std::string const& ifName);

  /**
   * Add or remove an interface address. Returns true if there was a change.
   *
   * Enumerates the host IPs in the prefix (bounded by kDefaultMaxIPsInCIDR) and
   * seeds (isValid) or clears them in the reachability map. This is the legacy
   * (default) path. When bgp_resolve_nexthops_from_interface_state is on,
   * reachability is driven purely by interface link state and interface
   * prefixes are tracked globally by NetlinkWrapper (InterfacePrefixTable) and
   * per-interface via addPrefix/removePrefix below, so this is not called in
   * that mode.
   */
  bool updateAddr(folly::CIDRNetwork const& addr, bool isValid);
  bool updateIfIndex(int ifIndex);

  /**
   * Record the kernel's neighbor (ARP/ND) state for an IP already seeded by
   * updateAddr. Legacy (bgp_resolve_nexthops_from_interface_state off) path
   * only: only IPs seeded by updateAddr are tracked, so an update for an
   * untracked IP is dropped (returns false). Returns true if the tracked state
   * changed.
   */
  bool updateReachability(const folly::IPAddress& ip, bool reachability);

  /**
   * The published reachability of one IP. This is the kernel neighbor state
   * AND the link state AND the hold state. It is false for an IP that is not
   * tracked.
   *
   * The now parameter has a default value, so a caller that does not use the
   * hold does not have to supply it.
   */
  bool isReachable(
      const folly::IPAddress& ip,
      std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now()) const;

  /**
   * True if the kernel reports at least one seeded host IP on this interface as
   * reachable. A link state change can only change a published value when this
   * holds, so callers use it to suppress no-op republishes.
   */
  bool hasReachableNeighbor() const;

  /**
   * Give fn(ip, reachable) for each seeded host IP. The reachable value is the
   * kernel neighbor state AND the link state AND the hold state.
   *
   * The two halves are combined here at read time rather than stored combined.
   * A link event must not write the neighbor half. After a debounce netlink
   * sends no neighbor event, so nothing would restore it.
   *
   * The now parameter has a default value, so a caller that does not use the
   * hold does not have to supply it.
   */
  template <typename F>
  void forEachPublishedReachability(
      F&& fn,
      std::chrono::steady_clock::time_point now =
          std::chrono::steady_clock::now()) const {
    const bool linkUsable = canUseLink(now);
    for (const auto& [ip, neighborReachable] : ipReachabilityMap_) {
      fn(ip, neighborReachable && linkUsable);
    }
  }

  std::string getIfName() const;
  int getIfIndex() const;

  /**
   * Interface link (operational) state, maintained on both paths. On the legacy
   * path it is the link half of published reachability (see
   * forEachPublishedReachability). On the
   * bgp_resolve_nexthops_from_interface_state path a directly-connected nexthop
   * is reachable iff a covering interface is up. setUp returns true if the
   * state changed.
   */
  bool setUp(bool isUp);
  bool isUp() const;

  /**
   * Tell the entry that the link went down. Call this on each link-down
   * transition.
   *
   * A link-down never has a hold, so this function removes the current hold.
   * The function then moves the ladder. If the link was quiet for more than
   * maxHoldDownTime, the ladder returns to initialHoldDownTime. If the link
   * was not quiet, the hold time doubles, to a maximum of maxHoldDownTime.
   *
   * This function does the same as openr::ExponentialBackoff::reportError
   * (ExponentialBackoff.cpp:45-56). It also includes the decay from
   * reportSuccess (:37-41), which openr does in isActive (:70-74).
   */
  void recordLinkDown(
      std::chrono::steady_clock::time_point now,
      std::chrono::milliseconds initialHoldDownTime,
      std::chrono::milliseconds maxHoldDownTime);

  /**
   * Tell the entry that the link came up. Call this on each link-up
   * transition. The function returns true if it started a hold.
   *
   * The hold runs from the link-down time, as Open/R does
   * (ExponentialBackoff.cpp:67-70). A link that stayed down for longer than
   * the hold has already served it, so it returns with no delay. Only a link
   * that returns quickly still owes time.
   */
  bool startLinkUpHold(std::chrono::steady_clock::time_point now);

  /**
   * Return true if the code can use the link at this time. This is the link
   * half of the published reachability. The kernel neighbor state is the
   * other half.
   */
  bool canUseLink(std::chrono::steady_clock::time_point now) const;

  /**
   * Return true if a hold is present and the clock passed its end time. The
   * hold release loop uses this function.
   *
   * This is not the same question as canUseLink. canUseLink also reads the
   * link state.
   */
  bool isHoldEnded(std::chrono::steady_clock::time_point now) const;

  /**
   * Remove the hold. This is bookkeeping only, because canUseLink is already
   * true when the clock passed the end time.
   */
  void removeHold();

  /**
   * The time when the link-up hold ends. Empty when no hold is present.
   */
  std::optional<std::chrono::steady_clock::time_point> getHoldEndTime() const;

  /**
   * Per-interface set of contributed prefixes (the reverse index of
   * InterfacePrefixTable). Used by the interface-state path to find, on a link
   * event, which subnets this interface covers so the registered nexthops in
   * them can be re-evaluated. Bounded by the interface's address count, not RIB
   * scale. addPrefix/removePrefix return true if the set changed.
   */
  bool addPrefix(const folly::CIDRNetwork& prefix);
  bool removePrefix(const folly::CIDRNetwork& prefix);
  const folly::F14FastSet<folly::CIDRNetwork>& getPrefixes() const;

  /**
   * The kernel's neighbor (ARP/ND) state per seeded host IP. This is only the
   * neighbor half of reachability; the link half lives in isUp_. Read the
   * published value via forEachPublishedReachability or isReachable.
   */
  const folly::F14NodeMap<folly::IPAddress, bool>& getNeighborStateMap() const;

 private:
  // Interface name
  std::string ifName_;
  // Interface Index
  int ifIndex_{-1};
  /*
   * Kernel neighbor (ARP/ND) state per seeded host IP. Written only by neighbor
   * events and the startup neighbor dump -- never by a link event.
   */
  folly::F14NodeMap<folly::IPAddress, bool> ipReachabilityMap_{};
  /*
   * The number of entries in ipReachabilityMap_ whose value is true.
   * updateAddr and updateReachability are the only writers of the map, so they
   * are the only two places that move this count.
   */
  size_t reachableNeighborCount_{0};
  // Interface operational (link) state.
  bool isUp_{false};
  /*
   * The time when the link-up hold ends. It is empty when no hold is present.
   *
   * This is a time, not a flag. A hold ends because the clock passes this
   * time. No event is necessary. If the release is late, the published value
   * is still correct.
   */
  std::optional<std::chrono::steady_clock::time_point> holdEndTime_;
  /*
   * The time of the last link-down transition. It is empty until the first
   * link-down. The ladder uses this value to decide if a flap is new or if it
   * repeats.
   */
  std::optional<std::chrono::steady_clock::time_point> lastDownTime_;
  /*
   * The length of the next link-up hold. It is zero before the first
   * link-down.
   */
  std::chrono::milliseconds holdTime_{0};
  // Prefixes this interface contributes to the global InterfacePrefixTable.
  // Only populated on the bgp_resolve_nexthops_from_interface_state path.
  folly::F14FastSet<folly::CIDRNetwork> prefixes_{};
};

} // namespace facebook::bgp
