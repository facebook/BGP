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

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/RouteInfo.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopAssociationList.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfoBase.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopStatus.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"

namespace facebook::bgp {

class NexthopInfo : public NexthopInfoBase {
 public:
  explicit NexthopInfo(const NexthopStatus& status) : status_(status) {
    // Stamp the initial resolution state so "never resolved" (unset) is
    // distinguishable from "resolved at least once".
    const auto now = std::chrono::steady_clock::now();
    if (status_.isReachable()) {
      lastReachabilityChangeTs_ = now;
    }
    if (status_.getIgpCost().has_value()) {
      lastIgpCostChangeTs_ = now;
    }
  }

  // Delete copy constructor and assignment operator since
  // NexthopAssociationList is not copyable
  NexthopInfo(const NexthopInfo&) = delete;
  NexthopInfo& operator=(const NexthopInfo&) = delete;

  // Allow move operations
  NexthopInfo(NexthopInfo&&) noexcept = default;
  NexthopInfo& operator=(NexthopInfo&&) = delete;

  // Virtual destructor since this class is inherited from NexthopInfoBase
  ~NexthopInfo() override = default;

  /*
   * [Accessor Methods]
   */
  const folly::IPAddress& getNextHop() const {
    return status_.getNexthop();
  }

  bool isReachable() const {
    return status_.isReachable();
  }

  std::optional<uint32_t> getIgpCost() const override {
    return status_.getIgpCost();
  }

  std::optional<bool> isConnected() const override {
    return status_.isConnected();
  }

  bool isResolvedForSelection() const override {
    return status_.isResolvedForSelection();
  }

  /**
   * @brief Update the status of this nexthop
   * @param status The new status for this nexthop
   */
  void updateStatus(const NexthopStatus& status) {
    const auto now = std::chrono::steady_clock::now();
    if (status_.isReachable() != status.isReachable()) {
      lastReachabilityChangeTs_ = now;
    }
    if (status_.getIgpCost() != status.getIgpCost()) {
      lastIgpCostChangeTs_ = now;
    }
    status_ = status;
  }

  /**
   * @brief Time of the last reachability transition for this nexthop.
   * @return steady_clock time_point of the last change; nullopt if the nexthop
   *         has never been reachable (i.e. never resolved).
   */
  std::optional<std::chrono::steady_clock::time_point>
  getLastReachabilityChangeTs() const {
    return lastReachabilityChangeTs_;
  }

  /**
   * @brief Time of the last IGP-cost change for this nexthop.
   * @return steady_clock time_point of the last change; nullopt if a cost has
   *         never been received (i.e. never resolved).
   */
  std::optional<std::chrono::steady_clock::time_point> getLastIgpCostChangeTs()
      const {
    return lastIgpCostChangeTs_;
  }

  void linkRouteInfo(RouteInfo& routeInfo) {
    routeInfo.setNexthopInfo(this);
    nexthopAssociationList_.link(routeInfo);
    if (!isReachable()) {
      RibStats::incrInactivePathCount(1);
    }
  }

  void unlinkRouteInfo(RouteInfo& routeInfo) {
    routeInfo.setNexthopInfo(nullptr);
    nexthopAssociationList_.unlink(routeInfo);
    if (!isReachable()) {
      RibStats::decrInactivePathCount(1);
    }
  }

  uint32_t getRouteInfoListSize() const {
    return nexthopAssociationList_.size();
  }

  /**
   * @brief Get an iterator to the beginning of the RouteInfo list
   * @return Iterator to the beginning of the RouteInfo list
   */
  auto begin() const {
    return nexthopAssociationList_.begin();
  }

  /**
   * @brief Get an iterator to the end of the RouteInfo list
   * @return Iterator to the end of the RouteInfo list
   */
  auto end() const {
    return nexthopAssociationList_.end();
  }

 private:
  // Nexthop status containing IP address and IGP cost
  NexthopStatus status_;
  // List of routes associated with the nexthop
  NexthopAssociationList nexthopAssociationList_;
  // Timestamps of the last reachability / IGP-cost change. nullopt means the
  // corresponding value has never been resolved since this entry was created.
  std::optional<std::chrono::steady_clock::time_point>
      lastReachabilityChangeTs_;
  std::optional<std::chrono::steady_clock::time_point> lastIgpCostChangeTs_;
};

} // namespace facebook::bgp
