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

/*
 * RouteFilter wrapper for RouteInfo objects
 */

#pragma once

#include "neteng/fboss/bgp/cpp/common/RouteInfo.h"
#include "neteng/fboss/bgp/cpp/routelib/RouteFilter.h"

namespace facebook::bgp {

class RouteInfoFilter : nettools::edge::RouteFilter {
 public:
  /*
   * Factory method for constructing RouteFilter that filters on the specified
   * metric.
   */
  static std::unique_ptr<nettools::edge::RouteFilter> fromRouteFilterConfig(
      const nettools::edge::RouteFilterConfig& routeFilterConfig);

 private:
  // Returns 1 if route is locally originated, 0 otherwise.
  static int64_t getIsRouteLocal(
      const std::shared_ptr<const nettools::edge::RouteBase> ri);
};

} // namespace facebook::bgp
