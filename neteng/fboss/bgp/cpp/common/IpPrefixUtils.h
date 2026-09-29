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

#include "configerator/structs/neteng/fboss/bgp/if/gen-cpp2/bgp_attr_types.h"

namespace facebook::bgp {

/** Build a Thrift IP prefix from an address using its full bit width. */
neteng::fboss::bgp_attr::TIpPrefix createTIpPrefix(
    const folly::IPAddress& address);

/** Build a Thrift IP prefix from a CIDR network. */
neteng::fboss::bgp_attr::TIpPrefix createTIpPrefix(
    const folly::CIDRNetwork& prefix);

} // namespace facebook::bgp
