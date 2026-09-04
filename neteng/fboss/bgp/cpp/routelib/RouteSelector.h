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
 * BGP route selection logic.
 */

#pragma once

#include <boost/noncopyable.hpp>
#include <folly/Optional.h>
#include <functional>
#include <map>
#include "neteng/fboss/bgp/cpp/routelib/RouteBase.h"
#include "neteng/fboss/bgp/cpp/routelib/RouteFilter.h"

namespace facebook {
namespace nettools {
namespace edge {

std::vector<RouteFilterConfig> getRouteFilterConfigs(
    const std::vector<std::pair<RouteMetric, bool>>& metricAndHighests);

RouteFilterConfig getMembershipFilterConfig(
    const RouteMetric& metric,
    const MembershipRouteFilterAction& action,
    const std::unordered_set<int64_t>& filterValues);

RouteFilterConfig getHighestLowestRouteFilterConfig(
    const RouteMetric& metric,
    const HighestLowestRouteFilterAction& action);

RouteFilterConfig getRecoverEquivalentRouteFilterConfig(
    const RouteMetric& metric,
    const std::vector<RouteFilterConfig>& tiebreakerConfigs);

class RouteSelector : boost::noncopyable {
 public:
  explicit RouteSelector(
      const std::vector<RouteFilterConfig>& routeFilterConfigs);
  RouteSelector() = default;
  virtual ~RouteSelector() = default;

  /*
   * Compare all routes for a single prefix and mark with the
   * appropriate PREFERRED/WITHDRAWN flag.
   * Return true if there are any routes that were marked.
   */
  bool markPreferredRoutes(
      const std::vector<std::shared_ptr<RouteBase>>& routes);

  /*
   * Selects from provided routes. If filters match multiple, it will return
   * all of them. If filters match none, it will return an empty vector.
   */
  std::vector<std::shared_ptr<RouteBase>> selectRoutes(
      const std::vector<std::shared_ptr<RouteBase>>& routes) const;

  /*
   * In addition to the behavior of selectRoutes, it marks the filter by
   * which a path was rejected.
   */
  std::vector<std::shared_ptr<RouteBase>> selectRoutesAndSetRejectionFilter(
      const std::vector<std::shared_ptr<RouteBase>>& routes) const;

  /*
   * Compares two routes using configured filters that support comparison
   * Return true if ri2 is preferred, false if equal or if ri1 is preferred
   */
  bool compareRoutes(
      const std::shared_ptr<const RouteBase>& ri1,
      const std::shared_ptr<const RouteBase>& ri2) const;

 protected:
  AcceptedAndRejectedRoutes applyFilters(
      const std::vector<std::shared_ptr<RouteBase>>& routes,
      folly::Optional<std::map<
          std::shared_ptr<RouteBase>,
          nettools::edge::RouteFilterConfig>*> filterMap = folly::none) const;

  std::vector<std::unique_ptr<RouteFilter>> filters_;
};

} // namespace edge
} // namespace nettools
} // namespace facebook
