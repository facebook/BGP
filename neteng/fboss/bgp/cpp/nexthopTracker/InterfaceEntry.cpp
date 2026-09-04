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

#include "neteng/fboss/bgp/cpp/nexthopTracker/InterfaceEntry.h"

#include <algorithm>

#include <gflags/gflags.h>

#include "neteng/fboss/bgp/cpp/common/Utils.h"

DEFINE_bool(
    bgp_resolve_nexthops_from_interface_state,
    false,
    "When true, NetlinkWrapper resolves a directly-connected nexthop's "
    "reachability purely from interface link state: the nexthop is reachable if "
    "a covering interface prefix is up, unreachable if it is down, and not "
    "directly connected (left to the FIB/recursive path) if no interface prefix "
    "covers it. Host routes / ARP / ND (kernel neighbor table) are ignored on "
    "this path. This lets peers on subnets wider than a /31 (v4) or /127 (v6) "
    "be resolved. Default false preserves the legacy subnet-enumeration "
    "behavior. Intended for testing. "
    "Backbone (BB/EBB) only: this flag is DEFINEd in the interface_entry "
    "library, which is only linked into the backbone binary (bgpd_cpp_bb). It "
    "is not present in the DC binary (bgpd_cpp), so it cannot be enabled there "
    "and the DC nexthop-registration path (FsdbFibWatcher/FsdbNeighborWatcher "
    "feeding NexthopCache) is unaffected.");

namespace facebook::bgp {
InterfaceEntry::InterfaceEntry(std::string const& ifName) : ifName_(ifName) {}

bool InterfaceEntry::updateAddr(folly::CIDRNetwork const& addr, bool isValid) {
  /*
   * Legacy (default) path: seed/clear reachability by enumerating every host IP
   * in the interface prefix. Bounded by kDefaultMaxIPsInCIDR, so any prefix
   * wider than a /31 (v4) or /127 (v6) seeds nothing. When
   * bgp_resolve_nexthops_from_interface_state is on, reachability is driven
   * purely by interface link state and interface prefixes are tracked globally
   * by NetlinkWrapper (InterfacePrefixTable), so this is not called.
   */
  bool isUpdated = false;
  auto ips = listAllIPsInCIDR(addr);
  if (isValid) {
    // Add all IPs from the CIDR
    for (const auto& ip : ips) {
      auto result = ipReachabilityMap_.insert({ip, false});
      isUpdated |= result.second;
    }
  } else {
    // Remove all IPs from the CIDR
    for (const auto& ip : ips) {
      auto it = ipReachabilityMap_.find(ip);
      if (it == ipReachabilityMap_.end()) {
        continue;
      }
      if (it->second) {
        --reachableNeighborCount_;
      }
      ipReachabilityMap_.erase(it);
      isUpdated = true;
    }
  }
  return isUpdated;
}

bool InterfaceEntry::updateIfIndex(int ifIndex) {
  bool isUpdated = false;
  if (ifIndex_ != ifIndex) {
    isUpdated = true;
    ifIndex_ = ifIndex;
  }

  return isUpdated;
}

bool InterfaceEntry::updateReachability(
    const folly::IPAddress& ip,
    bool reachability) {
  auto it = ipReachabilityMap_.find(ip);
  if (it == ipReachabilityMap_.end()) {
    /*
     * Legacy behavior: only IPs seeded by updateAddr are tracked; an update for
     * an untracked IP is dropped.
     */
    return false;
  }

  if (it->second != reachability) {
    it->second = reachability;
    reachability ? ++reachableNeighborCount_ : --reachableNeighborCount_;
    return true;
  }

  return false;
}

bool InterfaceEntry::isReachable(
    const folly::IPAddress& ip,
    std::chrono::steady_clock::time_point now) const {
  auto it = ipReachabilityMap_.find(ip);
  if (it == ipReachabilityMap_.end()) {
    return false;
  }
  const bool neighborReachable = it->second;
  return neighborReachable && canUseLink(now);
}

bool InterfaceEntry::hasReachableNeighbor() const {
  return reachableNeighborCount_ > 0;
}

std::string InterfaceEntry::getIfName() const {
  return ifName_;
}

int InterfaceEntry::getIfIndex() const {
  return ifIndex_;
}

bool InterfaceEntry::setUp(bool isUp) {
  if (isUp_ != isUp) {
    isUp_ = isUp;
    return true;
  }
  return false;
}

bool InterfaceEntry::isUp() const {
  return isUp_;
}

void InterfaceEntry::recordLinkDown(
    std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds initialHoldDownTime,
    std::chrono::milliseconds maxHoldDownTime) {
  holdEndTime_.reset();
  /*
   * holdTime_ is set to initialHoldDownTime on the first link-down, and on any
   * later link-down that comes maxHoldDownTime or more after the previous one.
   * Otherwise holdTime_ doubles, up to maxHoldDownTime.
   *
   * The window is maxHoldDownTime, the same length as the longest hold. A flap
   * that is farther apart than the longest hold cannot be absorbed by a hold.
   * To add a penalty for it gives only more downtime. Open/R uses the same
   * rule (openr/link-monitor/InterfaceEntry.cpp:70-74).
   */
  const bool isRepeatFlap =
      lastDownTime_.has_value() && (now - *lastDownTime_) < maxHoldDownTime;
  holdTime_ = isRepeatFlap ? std::min(holdTime_ * 2, maxHoldDownTime)
                           : initialHoldDownTime;
  lastDownTime_ = now;
}

bool InterfaceEntry::startLinkUpHold(
    std::chrono::steady_clock::time_point now) {
  /*
   * There is no hold before the first link-down of the process. It is the
   * same state as Open/R currentBackoff_ == 0, where canTryNow returns true.
   */
  if (!lastDownTime_.has_value()) {
    return false;
  }
  /*
   * The hold runs from the link-down time. A long outage therefore serves the
   * whole hold while the link is down, and the link returns with no delay.
   */
  const auto endTime = *lastDownTime_ + holdTime_;
  if (now >= endTime) {
    return false;
  }
  holdEndTime_ = endTime;
  return true;
}

bool InterfaceEntry::canUseLink(
    std::chrono::steady_clock::time_point now) const {
  return isUp_ && (!holdEndTime_.has_value() || now >= *holdEndTime_);
}

bool InterfaceEntry::isHoldEnded(
    std::chrono::steady_clock::time_point now) const {
  return holdEndTime_.has_value() && now >= *holdEndTime_;
}

void InterfaceEntry::removeHold() {
  holdEndTime_.reset();
}

std::optional<std::chrono::steady_clock::time_point>
InterfaceEntry::getHoldEndTime() const {
  return holdEndTime_;
}

bool InterfaceEntry::addPrefix(const folly::CIDRNetwork& prefix) {
  return prefixes_.insert(prefix).second;
}

bool InterfaceEntry::removePrefix(const folly::CIDRNetwork& prefix) {
  return prefixes_.erase(prefix) == 1;
}

const folly::F14FastSet<folly::CIDRNetwork>& InterfaceEntry::getPrefixes()
    const {
  return prefixes_;
}

const folly::F14NodeMap<folly::IPAddress, bool>&
InterfaceEntry::getNeighborStateMap() const {
  return ipReachabilityMap_;
}

} // namespace facebook::bgp
