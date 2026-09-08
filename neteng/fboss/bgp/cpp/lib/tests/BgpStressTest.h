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

#include <folly/IPAddress.h>
#include <gtest/gtest.h>
#include <vector>
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace {

struct BgpEntry {
  folly::CIDRNetwork prefix; // v4 or v6 prefix
  folly::IPAddress nexthop; // v4 or v6 nexthop
  facebook::nettools::bgplib::BgpUpdateAfi afi;
  facebook::nettools::bgplib::BgpUpdateSafi safi;
  std::shared_ptr<facebook::nettools::bgplib::BgpUpdate2> update;
};

/*
 * BgpUpdate2 has many fields like withdrawn routes, both v4 and v6 related
 * fields which may not exist for every entry etc. Here we are trying to see
 * if we store high order fields in a new struct with a shared pointer to
 * Bgp attributes how much memory will it reduce
 */
struct BgpEntryOpt {
  folly::CIDRNetwork prefix; // v4 or v6 prefix
  folly::IPAddress nexthop; // v4 or v6 nexthop
  facebook::nettools::bgplib::BgpUpdateAfi afi;
  facebook::nettools::bgplib::BgpUpdateSafi safi;
  std::shared_ptr<facebook::nettools::bgplib::BgpAttributes> attrs;
};

/*
 * Optimized structure, which uses all cpp structures instead of thrift
 * NOTE: This does not have attrs before policy, after policy (in/out) etc
 */
struct BgpEntryOpt2 {
  folly::CIDRNetwork prefix; // v4 or v6 prefix
  folly::IPAddress nexthop; // v4 or v6 nexthop
  facebook::nettools::bgplib::BgpUpdateAfi afi;
  facebook::nettools::bgplib::BgpUpdateSafi safi;
  std::shared_ptr<facebook::nettools::bgplib::BgpAttributesC> attrs;
};
} // namespace
