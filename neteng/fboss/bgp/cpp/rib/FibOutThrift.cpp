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

#include "neteng/fboss/bgp/cpp/rib/FibOutThrift.h"

#include <stdexcept>

#include "neteng/fboss/bgp/cpp/common/IpPrefixUtils.h"

namespace facebook::bgp {
namespace {

using neteng::fboss::bgp::thrift::TFibNexthopSet;
using neteng::fboss::bgp::thrift::TFibOutNextHop;
using neteng::fboss::bgp::thrift::TFibOutNextHopRole;
using neteng::fboss::bgp::thrift::TFibOutOperation;
using neteng::fboss::bgp::thrift::TFibOutRoute;

/** Convert the native FIB-out operation to its wire enum. */
TFibOutOperation toThriftOperation(FibOutOperation operation) {
  switch (operation) {
    case FibOutOperation::NONE:
      return TFibOutOperation::NONE;
    case FibOutOperation::PROGRAM:
      return TFibOutOperation::PROGRAM;
  }
  throw std::logic_error("invalid FIB-out operation");
}

/** Convert the native nexthop role to its wire enum. */
TFibOutNextHopRole toThriftRole(FibOutNexthopRole role) {
  switch (role) {
    case FibOutNexthopRole::PRIMARY:
      return TFibOutNextHopRole::PRIMARY;
    case FibOutNexthopRole::BACKUP:
      return TFibOutNextHopRole::BACKUP;
  }
  throw std::logic_error("invalid FIB-out nexthop role");
}

/** Convert one platform-normalized nexthop to its wire representation. */
TFibOutNextHop toThriftNexthop(
    const FibOutNexthop& nexthop,
    const FibOutTopologyInfoMap* topologyInfo) {
  TFibOutNextHop thriftNexthop;
  thriftNexthop.next_hop() = createTIpPrefix(nexthop.address);
  thriftNexthop.weight() = nexthop.weight;
  thriftNexthop.role() = toThriftRole(nexthop.role);
  if (nexthop.connected) {
    thriftNexthop.is_connected() = *nexthop.connected;
  }
  if (nexthop.interfaceName) {
    thriftNexthop.interface_name() = *nexthop.interfaceName;
  }
  if (topologyInfo && nexthop.role == FibOutNexthopRole::PRIMARY) {
    const auto topology = topologyInfo->find(nexthop.address);
    if (topology != topologyInfo->end()) {
      thriftNexthop.topology_info() = topology->second;
    }
  }
  return thriftNexthop;
}

} // namespace

TFibOutRoute toThriftFibOutRoute(const FibOutState& state) {
  if (!state.nexthops) {
    throw std::logic_error("FIB-out state has no nexthop set");
  }
  TFibOutRoute route;
  route.operation() = toThriftOperation(state.metadata.operation());
  if (const auto adminDistance = state.metadata.adminDistance()) {
    route.admin_distance() = *adminDistance;
  }
  if (const auto classId = state.metadata.classId()) {
    route.class_id() = *classId;
  }

  route.next_hops()->reserve(state.nexthops->size());
  for (const auto& nexthop : *state.nexthops) {
    route.next_hops()->push_back(
        toThriftNexthop(nexthop, state.topologyInfo.get()));
  }
  return route;
}

TFibNexthopSet toThriftFibNexthopSet(
    const FibNexthopSet& nexthops,
    int64_t routeRefCount) {
  TFibNexthopSet thriftSet;
  thriftSet.next_hops()->reserve(nexthops.size());
  for (const auto& nexthop : nexthops) {
    thriftSet.next_hops()->push_back(toThriftNexthop(nexthop, nullptr));
  }
  thriftSet.ref_count() = routeRefCount;
  return thriftSet;
}

} // namespace facebook::bgp
