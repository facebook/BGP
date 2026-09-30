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
#include "neteng/fboss/bgp/cpp/routelib/RouteSelector.h"

#include <folly/container/F14Set.h>
#include <folly/logging/xlog.h>

namespace facebook {
namespace nettools {
namespace edge {

using std::pair;
using std::shared_ptr;
using std::vector;

vector<RouteFilterConfig> getRouteFilterConfigs(
    const vector<pair<RouteMetric, bool>>& metricAndHighests) {
  vector<RouteFilterConfig> rules;
  RouteFilterConfig ignoreDeleted;
  ignoreDeleted.metric = RouteMetric::DELETED_ROUTE;
  MembershipRouteFilterOpts membershipFilterOpts{
      MembershipRouteFilterAction::CHOOSE_MEMBER, {0}};
  ignoreDeleted.opts = membershipFilterOpts;
  rules.push_back(ignoreDeleted);

  for (const auto& metricAndHighest : metricAndHighests) {
    RouteFilterConfig rule;
    rule.metric = metricAndHighest.first;
    HighestLowestRouteFilterOpts highestLowestFilterOpts;
    highestLowestFilterOpts.action = metricAndHighest.second
        ? HighestLowestRouteFilterAction::CHOOSE_HIGHEST
        : HighestLowestRouteFilterAction::CHOOSE_LOWEST;
    rule.opts = highestLowestFilterOpts;
    rules.push_back(rule);
  }
  return rules;
}

RouteFilterConfig getMembershipFilterConfig(
    const RouteMetric& metric,
    const MembershipRouteFilterAction& action,
    const std::unordered_set<int64_t>& filterValues) {
  RouteFilterConfig rule;
  rule.metric = metric;
  MembershipRouteFilterOpts membershipFilterOpts{action, filterValues};
  rule.opts = membershipFilterOpts;
  return rule;
}

RouteFilterConfig getHighestLowestRouteFilterConfig(
    const RouteMetric& metric,
    const HighestLowestRouteFilterAction& action) {
  RouteFilterConfig rule;
  rule.metric = metric;
  HighestLowestRouteFilterOpts highestLowestFilterOpts{action};
  rule.opts = highestLowestFilterOpts;
  return rule;
}

RouteFilterConfig getRecoverEquivalentRouteFilterConfig(
    const RouteMetric& metric,
    const std::vector<RouteFilterConfig>& tiebreakerConfigs) {
  RouteFilterConfig rule;
  rule.metric = metric;
  RecoverEquivalentRouteFilterOpts recoverEquivalentFilterOpts;
  recoverEquivalentFilterOpts.tiebreakerConfigs = tiebreakerConfigs;
  rule.opts = recoverEquivalentFilterOpts;
  return rule;
}

RouteSelector::RouteSelector(
    const vector<RouteFilterConfig>& routeFilterConfigs) {
  for (const auto& routeFilterConfig : routeFilterConfigs) {
    filters_.push_back(RouteFilter::fromRouteFilterConfig(routeFilterConfig));
  }
}

bool RouteSelector::markPreferredRoutes(
    const vector<shared_ptr<RouteBase>>& routes) {
  // Must not use ALREADY_PREFERRED_ROUTE filter with this function.
  for (const auto& filter : filters_) {
    if (filter->getRouteMetric() == RouteMetric::ALREADY_PREFERRED_ROUTE) {
      XLOGF(
          FATAL,
          "ALREADY_PREFERRED_ROUTE filter is used in markPreferredRoutes()");
    }
  }

  AcceptedAndRejectedRoutes filteredRoutes = applyFilters(routes);

  // Mark changes.
  bool preferredRoutesChanged = false;
  for (auto& routeInfo : filteredRoutes.first) {
    if (!routeInfo->getIsRoutePreferred()) {
      preferredRoutesChanged = true;
    }
    routeInfo->setRoutePreferred();
  }
  for (auto& routeInfo : filteredRoutes.second) {
    if (routeInfo->getIsRoutePreferred()) {
      preferredRoutesChanged = true;
    }
    routeInfo->clearRoutePreferred();
  }

  return preferredRoutesChanged;
}

vector<shared_ptr<RouteBase>> RouteSelector::selectRoutes(
    const vector<shared_ptr<RouteBase>>& routes) const {
  auto result = applyFilters(routes);
  return result.first;
}

vector<shared_ptr<RouteBase>> RouteSelector::selectRoutesAndSetRejectionFilter(
    const std::vector<shared_ptr<RouteBase>>& routes) const {
  std::map<shared_ptr<RouteBase>, nettools::edge::RouteFilterConfig> filterMap;

  for (auto& rInfo : routes) {
    rInfo->clearBestPathFilterCriteria();
  }
  auto result = applyFilters(routes, &filterMap);
  // Set the filter based on which a path was rejected.
  for (auto& rInfo : routes) {
    if (filterMap.find(rInfo) != filterMap.end()) {
      rInfo->setBestPathFilterCriteria(filterMap[rInfo]);
    }
  }
  return result.first;
}

bool RouteSelector::compareRoutes(
    const shared_ptr<const RouteBase>& ri1,
    const shared_ptr<const RouteBase>& ri2) const {
  for (const auto& filter : filters_) {
    if (!filter->comparable()) {
      continue;
    } else if (filter->compare(ri1, ri2)) {
      return true;
    } else if (filter->compare(ri2, ri1)) {
      return false;
    }
  }
  return false;
}

AcceptedAndRejectedRoutes RouteSelector::applyFilters(
    const std::vector<std::shared_ptr<RouteBase>>& routes,
    folly::Optional<std::map<
        std::shared_ptr<RouteBase>,
        nettools::edge::RouteFilterConfig>*> filterMap) const {
  /*
   * We pass the routes acccepted and rejected by the previous filter to the
   * immediate successor, this enables the successor to recover rejected routes
   */

  // Initially, all routes are "accepted" and none are "rejected"
  AcceptedAndRejectedRoutes filteredRoutes{routes, {}};

  for (const auto& filter : filters_) {
    if (routes.empty()) {
      break;
    }

    filteredRoutes = filter->filter(filteredRoutes, filterMap);
  }

  /*
   * A later filter can recover an earlier rejection. Derive final rejections
   * from the selected set so every input route is accepted or rejected once,
   * including routes rejected by nested tiebreakers.
   */
  const folly::F14FastSet<shared_ptr<RouteBase>> acceptedRoutes(
      filteredRoutes.first.begin(), filteredRoutes.first.end());
  vector<shared_ptr<RouteBase>> rejectedRoutes;
  rejectedRoutes.reserve(routes.size());
  for (const auto& route : routes) {
    if (!acceptedRoutes.contains(route)) {
      rejectedRoutes.push_back(route);
    }
  }
  return {std::move(filteredRoutes.first), std::move(rejectedRoutes)};
}
} // namespace edge
} // namespace nettools
} // namespace facebook
