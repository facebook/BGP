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
#include <optional>
#include "openr/if/gen-cpp2/Network_types.h"

namespace facebook::bgp {

/**
 * @class NexthopInfoBase
 * @brief Base interface for NexthopInfo.
 *
 * This interface that allows RouteInfo to access nexthop cost data
 * without creating circular dependencies.
 */
class NexthopInfoBase {
 public:
  // Add default constructor
  NexthopInfoBase() = default;
  virtual ~NexthopInfoBase() = default;

  /*
   * Delete copy constructor and assignment operator to be consistent with
   * derived class
   */
  NexthopInfoBase(const NexthopInfoBase&) = delete;
  NexthopInfoBase& operator=(const NexthopInfoBase&) = delete;

  // Allow move operations
  NexthopInfoBase(NexthopInfoBase&&) = default;
  NexthopInfoBase& operator=(NexthopInfoBase&&) = default;

  /**
   * @brief Get the IGP cost to reach the nexthop
   * @return Optional IGP cost, nullopt if the nexthop is unreachable
   */
  virtual std::optional<uint32_t> getIgpCost() const = 0;

  virtual std::optional<bool> isConnected() const = 0;

  /**
   * @brief Whether the nexthop is resolved for best-path selection.
   * @return true if the nexthop is eligible for best-path selection
   */
  virtual bool isResolvedForSelection() const = 0;
};

} // namespace facebook::bgp
