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
 * Filter configs used by EF, BmpCollector, and APM.
 */

#pragma once

#include "neteng/fboss/bgp/cpp/routelib/RouteSelector.h"

namespace facebook {
namespace nettools {
namespace edge {

inline std::vector<RouteFilterConfig> concatRouteFilterConfigs(
    const std::vector<RouteFilterConfig>& firstConfigVec,
    const std::vector<RouteFilterConfig>& secondConfigVec) {
  std::vector<RouteFilterConfig> mergedConfigVec;
  mergedConfigVec.reserve(firstConfigVec.size() + secondConfigVec.size());
  mergedConfigVec.insert(
      mergedConfigVec.end(), firstConfigVec.begin(), firstConfigVec.end());
  mergedConfigVec.insert(
      mergedConfigVec.end(), secondConfigVec.begin(), secondConfigVec.end());
  return mergedConfigVec;
}

/*
 * CONTROLLER_COMMUNITY prioritizes routes with EF's router-mode or metro-mode
 * controller community, which is necessary to identify a individual router's
 * preferred route(s) when we're considering EF routes.
 */
const std::vector<RouteFilterConfig> kRouterFilterConfigsBase = {
    getMembershipFilterConfig(
        RouteMetric::DELETED_ROUTE,
        MembershipRouteFilterAction::CHOOSE_MEMBER,
        {0}),
    getHighestLowestRouteFilterConfig(
        RouteMetric::PREFIX_LENGTH,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::CONTROLLER_COMMUNITY,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_LOCAL_PREFERENCE,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_AS_PATH_LEN,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_ORIGIN_CODE,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_MED_VALUE,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST),
    // Router will prefer external routes over internal (iBGP) learned routes
    getHighestLowestRouteFilterConfig(
        RouteMetric::EXTERNAL_ROUTE,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST),
};

// Rules for selecting the best route at a single PR without multipath.
const std::vector<RouteFilterConfig> kRouterFilterConfigsNoMultipath =
    concatRouteFilterConfigs(
        kRouterFilterConfigsBase,
        {getHighestLowestRouteFilterConfig(
             RouteMetric::BGP_ROUTER_ID,
             HighestLowestRouteFilterAction::CHOOSE_LOWEST),
         getHighestLowestRouteFilterConfig(
             RouteMetric::BGP_PEER_IP,
             HighestLowestRouteFilterAction::CHOOSE_LOWEST)});

/*
 * Rules for selecting the best route(s) at a single PR.
 *
 * These filters are designed for Juniper's BGP multipath behavior.
 * The filter set should be an extension of kRouterFilterConfigsNoMultipath,
 * adding any additional logic required for multipath computation.
 */
const std::vector<RouteFilterConfig> kRouterFilterConfigs =
    concatRouteFilterConfigs(
        kRouterFilterConfigsBase,
        {getRecoverEquivalentRouteFilterConfig(
            RouteMetric::BGP_PEER_ASN,
            // Tiebreakers before recovering equivalent routes
            {getHighestLowestRouteFilterConfig(
                 RouteMetric::BGP_ROUTER_ID,
                 HighestLowestRouteFilterAction::CHOOSE_LOWEST),
             getHighestLowestRouteFilterConfig(
                 RouteMetric::BGP_PEER_IP,
                 HighestLowestRouteFilterAction::CHOOSE_LOWEST)})});

/*
 * Rules for selecting the best route(s) at ASWs / PSWs from routes from PRs.
 *
 * These filters are designed for Arista's BGP multipath behavior.
 *
 * CONTROLLER_COMMUNITY_METRO prioritizes EF's metro-mode controller community,
 * which is necessary to identify the metro-level preferred routes from all of
 * the router-level preferred routes. If EF router-mode is in use, it will not
 * impact the distribution of traffic across PRs in a metro.
 *
 * We don't use RouteMetric::EXTERNAL_ROUTE in these filters, as all routes will
 * either be redistributed via iBGP (in clusers with ASWs) or eBGP (in all other
 * cluster, which have PSWs). In addition, the Route objects are generated from
 * the perspective of the PRs, so calling Route::getIsRouteExternal returns if
 * the routes are external or internal from the perspective of the PRs, not
 * the perspective of the ASW / PSWs.
 */
const std::vector<RouteFilterConfig> kMetroFilterConfigs = {
    getMembershipFilterConfig(
        RouteMetric::DELETED_ROUTE,
        MembershipRouteFilterAction::CHOOSE_MEMBER,
        {0}),
    getHighestLowestRouteFilterConfig(
        RouteMetric::PREFIX_LENGTH,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::CONTROLLER_COMMUNITY_METRO,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_LOCAL_PREFERENCE,
        HighestLowestRouteFilterAction::CHOOSE_HIGHEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_AS_PATH_LEN,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_ORIGIN_CODE,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST),
    getHighestLowestRouteFilterConfig(
        RouteMetric::BGP_MED_VALUE,
        HighestLowestRouteFilterAction::CHOOSE_LOWEST)};

} // namespace edge
} // namespace nettools
} // namespace facebook
