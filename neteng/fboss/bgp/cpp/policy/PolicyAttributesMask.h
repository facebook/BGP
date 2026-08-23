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

/**
 * This struct stores flags to mask path attributes not used during policy
 * evaluation. Path attributes include all BgpPath attributes, and
 * prefix.
 *
 *   PolicyAttributesMask{.origin = true, .med = false}
 *
 * would indicate that med should be masked away, and that origin is used.
 *
 * One mask is created per policy statement. All masks are stored by
 * policy name on the PolicyManager.
 *
 */
namespace facebook::bgp {

struct PolicyAttributesMask {
  // BgpPath : nettools::bgplib::BgpAttributesC
  bool origin = false;
  bool asPath = false;
  bool nexthop = false;
  bool med = false;
  bool localPref = false;
  bool atomicAggregate = false;
  bool aggregator = false;
  bool communities = false;
  bool originatorId = false;
  bool clusterList = false;
  bool extCommunities = false;
  bool weight = false;
  bool backupAddr = false;

  // ------------ External attributes ---------------
  bool prefix = false;
  bool customizedLbwEnabled = false;

  bool operator==(const PolicyAttributesMask& other) const = default;
};

} // namespace facebook::bgp
