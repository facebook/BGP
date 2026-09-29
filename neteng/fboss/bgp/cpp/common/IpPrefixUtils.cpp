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

#include "neteng/fboss/bgp/cpp/common/IpPrefixUtils.h"

#include <utility>

#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"

namespace facebook::bgp {

neteng::fboss::bgp_attr::TIpPrefix createTIpPrefix(
    const folly::CIDRNetwork& prefix) {
  const auto& [address, prefixLength] = prefix;

  neteng::fboss::bgp_attr::TIpPrefix thriftPrefix;
  thriftPrefix.afi() = address.isV4()
      ? neteng::fboss::bgp_attr::TBgpAfi::AFI_IPV4
      : neteng::fboss::bgp_attr::TBgpAfi::AFI_IPV6;
  thriftPrefix.num_bits() = prefixLength;

  /* FBOSS CLI uses num_bits to distinguish local routes. */
  if (address == kLocalRouteV4Nexthop || address == kLocalRouteV6Nexthop) {
    thriftPrefix.num_bits() = 0;
  }

  thriftPrefix.prefix_bin() =
      facebook::network::toBinaryAddress(address).addr()->toStdString();
  return thriftPrefix;
}

neteng::fboss::bgp_attr::TIpPrefix createTIpPrefix(
    const folly::IPAddress& address) {
  return createTIpPrefix(
      std::make_pair(address, address.isV4() ? uint8_t{32} : uint8_t{128}));
}

} // namespace facebook::bgp
