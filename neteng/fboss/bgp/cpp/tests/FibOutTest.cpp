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

#include "neteng/fboss/bgp/cpp/rib/FibOut.h"
#include "neteng/fboss/bgp/cpp/rib/FibOutThrift.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include <gtest/gtest.h>

namespace facebook::bgp {
namespace {

/** Construct one primary IPv4 FIB-out nexthop for registry tests. */
FibOutNexthop makeNexthop(uint32_t address, uint32_t weight = 1) {
  return FibOutNexthop{
      .address = folly::IPAddress::fromLongHBO(address),
      .weight = weight,
      .role = FibOutNexthopRole::PRIMARY,
  };
}

TEST(FibOutTest, ReturnsSamePointerForEqualNormalizedSets) {
  const auto nexthop1 = makeNexthop(0x01000001, 10);
  const auto nexthop2 = makeNexthop(0x01000002, 20);

  FibNexthopSets nexthopSets;
  auto set1 = nexthopSets.getOrCreate({nexthop2, nexthop1, nexthop1});
  auto set2 = nexthopSets.getOrCreate({nexthop1, nexthop2});

  EXPECT_EQ(set1, set2);
  EXPECT_EQ(*set1, (FibNexthopSet{nexthop1, nexthop2}));
  EXPECT_EQ(nexthopSets.size(), 1);

  set1.reset();
  EXPECT_EQ(nexthopSets.size(), 1);
  set2.reset();
  EXPECT_EQ(nexthopSets.size(), 0);
}

TEST(FibOutTest, Shares128WaySet) {
  FibNexthopSet nexthops;
  nexthops.reserve(128);
  for (uint32_t index = 0; index < 128; ++index) {
    nexthops.push_back(makeNexthop(0x01000001 + index, index + 1));
  }

  FibNexthopSets nexthopSets;
  const auto set1 = nexthopSets.getOrCreate(nexthops);
  std::reverse(nexthops.begin(), nexthops.end());
  const auto set2 = nexthopSets.getOrCreate(nexthops);

  EXPECT_EQ(set1, set2);
  EXPECT_EQ(set1->size(), 128);
}

TEST(FibOutTest, KeepsDifferentSetsDistinct) {
  const auto nexthop1 = makeNexthop(0x01000001);
  const auto nexthop2 = makeNexthop(0x01000002);
  const auto nexthop3 = makeNexthop(0x01000003);
  FibNexthopSets nexthopSets;
  const auto set1 = nexthopSets.getOrCreate({nexthop1, nexthop2});
  const auto set2 = nexthopSets.getOrCreate({nexthop2, nexthop3});

  EXPECT_NE(set1, set2);
}

TEST(FibOutTest, ReleasesSetsAfterLastSharedPointer) {
  FibNexthopSets nexthopSets;
  std::weak_ptr<const FibNexthopSet> weakSet;
  EXPECT_EQ(nexthopSets.size(), 0);
  {
    const auto set = nexthopSets.getOrCreate({makeNexthop(0x01000001)});
    weakSet = set;
    EXPECT_EQ(nexthopSets.size(), 1);
  }

  EXPECT_TRUE(weakSet.expired());
  EXPECT_EQ(nexthopSets.size(), 0);
}

TEST(FibOutTest, SetCanOutliveRegistry) {
  std::shared_ptr<const FibNexthopSet> set;
  {
    FibNexthopSets nexthopSets;
    set = nexthopSets.getOrCreate({makeNexthop(0x01000001)});
    EXPECT_EQ(nexthopSets.size(), 1);
  }

  EXPECT_EQ(set->size(), 1);
  set.reset();
}

TEST(FibOutTest, ReleasingClearedSetDoesNotRemoveReplacement) {
  FibNexthopSets nexthopSets;
  auto oldSet = nexthopSets.getOrCreate({makeNexthop(0x01000001)});
  nexthopSets.clear();
  ASSERT_EQ(nexthopSets.size(), 0);

  auto replacement = nexthopSets.getOrCreate({makeNexthop(0x01000001)});
  ASSERT_NE(oldSet, replacement);
  ASSERT_EQ(nexthopSets.size(), 1);

  oldSet.reset();
  EXPECT_EQ(nexthopSets.size(), 1);
  replacement.reset();
  EXPECT_EQ(nexthopSets.size(), 0);
}

TEST(FibOutTest, DistinguishesMissingOptionalFields) {
  auto withMissingFields = makeNexthop(0x01000001);
  auto withValues = withMissingFields;
  withValues.interfaceName = "";
  withValues.connected = false;

  FibNexthopSets nexthopSets;
  const auto missing = nexthopSets.getOrCreate({withMissingFields});
  const auto present = nexthopSets.getOrCreate({withValues});

  EXPECT_NE(missing, present);
}

TEST(FibOutTest, SetIdentityIncludesEveryNormalizedNexthopField) {
  const FibOutNexthop base{
      .address = folly::IPAddress("fe80::1"),
      .interfaceName = "eth1/1",
      .weight = 1,
      .role = FibOutNexthopRole::PRIMARY,
      .connected = true,
  };
  auto otherInterface = base;
  otherInterface.interfaceName = "eth1/2";
  auto otherWeight = base;
  otherWeight.weight = 2;
  auto otherRole = base;
  otherRole.role = FibOutNexthopRole::BACKUP;
  auto otherConnected = base;
  otherConnected.connected = false;

  FibNexthopSets nexthopSets;
  const auto baseSet = nexthopSets.getOrCreate({base});
  EXPECT_NE(baseSet, nexthopSets.getOrCreate({otherInterface}));
  EXPECT_NE(baseSet, nexthopSets.getOrCreate({otherWeight}));
  EXPECT_NE(baseSet, nexthopSets.getOrCreate({otherRole}));
  EXPECT_NE(baseSet, nexthopSets.getOrCreate({otherConnected}));
}

TEST(FibOutTest, TraversalReportsExternalOwnerCounts) {
  const auto nexthop1 = makeNexthop(0x01000001);
  const auto nexthop2 = makeNexthop(0x01000002);
  FibNexthopSets nexthopSets;
  const auto first = nexthopSets.getOrCreate({nexthop1});
  const auto firstAlias = nexthopSets.getOrCreate({nexthop1});
  const auto second = nexthopSets.getOrCreate({nexthop2});
  ASSERT_EQ(first, firstAlias);
  ASSERT_NE(first, second);
  size_t visited{0};

  nexthopSets.forEach(
      [&](const FibNexthopSet& nexthops, size_t externalOwnerCount) {
        ++visited;
        if (nexthops == FibNexthopSet{nexthop1}) {
          EXPECT_EQ(externalOwnerCount, 2);
        } else if (nexthops == FibNexthopSet{nexthop2}) {
          EXPECT_EQ(externalOwnerCount, 1);
        } else {
          ADD_FAILURE() << "visited an unknown canonical nexthop set";
        }
      });

  EXPECT_EQ(visited, 2);
}

TEST(FibOutTest, TraversalIncludesEmptyCanonicalSet) {
  FibNexthopSets nexthopSets;
  const auto empty = nexthopSets.getOrCreate({});
  ASSERT_TRUE(empty->empty());
  size_t visited{0};

  nexthopSets.forEach(
      [&](const FibNexthopSet& nexthops, size_t externalOwnerCount) {
        ++visited;
        EXPECT_TRUE(nexthops.empty());
        EXPECT_EQ(externalOwnerCount, 1);
      });

  EXPECT_EQ(visited, 1);
}

TEST(FibOutTest, ReplacingLastExternalOwnerRemovesPreviousSet) {
  FibNexthopSets nexthopSets;
  auto routeState = std::make_shared<const FibOutState>(FibOutState{
      .nexthops = nexthopSets.getOrCreate({makeNexthop(0x01000001)}),
      .metadata = FibOutRouteMetadata::program(),
  });
  nexthopSets.forEach([](const FibNexthopSet&, size_t ownerCount) {
    EXPECT_EQ(ownerCount, 1);
  });

  routeState = std::make_shared<const FibOutState>(FibOutState{
      .nexthops = nexthopSets.getOrCreate({makeNexthop(0x01000002)}),
      .metadata = FibOutRouteMetadata::program(),
  });

  EXPECT_EQ(nexthopSets.size(), 1);
  nexthopSets.forEach([&](const FibNexthopSet& nexthops, size_t ownerCount) {
    EXPECT_EQ(nexthops, FibNexthopSet{makeNexthop(0x01000002)});
    EXPECT_EQ(ownerCount, 1);
  });
}

TEST(FibOutTest, MaterializesThriftOnlyForTheRpcBoundary) {
  FibNexthopSets nexthopSets;
  const auto nexthops = nexthopSets.getOrCreate({FibOutNexthop{
      .address = folly::IPAddress::fromLongHBO(0x01000001),
      .interfaceName = "Ethernet1",
      .weight = 10,
      .role = FibOutNexthopRole::BACKUP,
      .connected = true,
  }});

  const auto thriftRoute = toThriftFibOutRoute(
      FibOutState{
          .nexthops = nexthops,
          .metadata = FibOutRouteMetadata::program(20, 7),
      });
  neteng::fboss::bgp::thrift::TFibOutNextHop expectedNexthop;
  expectedNexthop.next_hop()->afi() =
      neteng::fboss::bgp_attr::TBgpAfi::AFI_IPV4;
  expectedNexthop.next_hop()->num_bits() = 32;
  expectedNexthop.next_hop()->prefix_bin() = std::string("\x01\x00\x00\x01", 4);
  expectedNexthop.weight() = 10;
  expectedNexthop.role() =
      neteng::fboss::bgp::thrift::TFibOutNextHopRole::BACKUP;
  expectedNexthop.is_connected() = true;
  expectedNexthop.interface_name() = "Ethernet1";
  neteng::fboss::bgp::thrift::TFibOutRoute expectedRoute;
  expectedRoute.operation() =
      neteng::fboss::bgp::thrift::TFibOutOperation::PROGRAM;
  expectedRoute.next_hops() = {std::move(expectedNexthop)};
  expectedRoute.admin_distance() = 20;
  expectedRoute.class_id() = 7;

  EXPECT_EQ(thriftRoute, expectedRoute);
}

TEST(FibOutTest, ConvertsMinimalProgramWithPrimaryAndBackupNexthops) {
  FibNexthopSets nexthopSets;
  const auto nexthops = nexthopSets.getOrCreate({
      FibOutNexthop{
          .address = folly::IPAddress::fromLongHBO(0x02000001),
          .weight = 10,
          .role = FibOutNexthopRole::PRIMARY,
      },
      FibOutNexthop{
          .address = folly::IPAddress::fromLongHBO(0x02000002),
          .weight = 0,
          .role = FibOutNexthopRole::BACKUP,
      },
  });

  const auto topologyInfo = std::make_shared<FibOutTopologyInfoMap>();
  topologyInfo->emplace(
      folly::IPAddress::fromLongHBO(0x02000001),
      std::unordered_map<std::string, int64_t>{{"rack_id", 3}});
  topologyInfo->emplace(
      folly::IPAddress::fromLongHBO(0x02000002),
      std::unordered_map<std::string, int64_t>{{"rack_id", 4}});
  const auto thriftRoute = toThriftFibOutRoute(
      FibOutState{
          .nexthops = nexthops,
          .metadata = FibOutRouteMetadata::program(),
          .topologyInfo = topologyInfo,
      });
  neteng::fboss::bgp::thrift::TFibOutNextHop primary;
  primary.next_hop()->afi() = neteng::fboss::bgp_attr::TBgpAfi::AFI_IPV4;
  primary.next_hop()->num_bits() = 32;
  primary.next_hop()->prefix_bin() = std::string("\x02\x00\x00\x01", 4);
  primary.weight() = 10;
  primary.role() = neteng::fboss::bgp::thrift::TFibOutNextHopRole::PRIMARY;
  primary.topology_info() = {{"rack_id", 3}};
  neteng::fboss::bgp::thrift::TFibOutNextHop backup;
  backup.next_hop()->afi() = neteng::fboss::bgp_attr::TBgpAfi::AFI_IPV4;
  backup.next_hop()->num_bits() = 32;
  backup.next_hop()->prefix_bin() = std::string("\x02\x00\x00\x02", 4);
  backup.weight() = 0;
  backup.role() = neteng::fboss::bgp::thrift::TFibOutNextHopRole::BACKUP;
  neteng::fboss::bgp::thrift::TFibOutRoute expected;
  expected.operation() = neteng::fboss::bgp::thrift::TFibOutOperation::PROGRAM;
  expected.next_hops() = {std::move(primary), std::move(backup)};
  EXPECT_EQ(thriftRoute, expected);
}

TEST(FibOutTest, ConvertsEmptyFibOutState) {
  FibNexthopSets nexthopSets;
  const auto emptyNexthops = nexthopSets.getOrCreate({});
  const auto none = toThriftFibOutRoute(
      FibOutState{
          .nexthops = emptyNexthops,
          .metadata = FibOutRouteMetadata{},
      });
  neteng::fboss::bgp::thrift::TFibOutRoute expected;
  expected.operation() = neteng::fboss::bgp::thrift::TFibOutOperation::NONE;
  EXPECT_EQ(none, expected);
}

TEST(FibOutTest, ConvertsCanonicalNexthopSetWithRouteReferenceCount) {
  const FibNexthopSet nexthops{FibOutNexthop{
      .address = folly::IPAddress::fromLongHBO(0x01000001),
      .interfaceName = "Ethernet1",
      .weight = 10,
      .role = FibOutNexthopRole::BACKUP,
      .connected = true,
  }};
  neteng::fboss::bgp::thrift::TFibOutNextHop expectedNexthop;
  expectedNexthop.next_hop()->afi() =
      neteng::fboss::bgp_attr::TBgpAfi::AFI_IPV4;
  expectedNexthop.next_hop()->num_bits() = 32;
  expectedNexthop.next_hop()->prefix_bin() = std::string("\x01\x00\x00\x01", 4);
  expectedNexthop.weight() = 10;
  expectedNexthop.role() =
      neteng::fboss::bgp::thrift::TFibOutNextHopRole::BACKUP;
  expectedNexthop.is_connected() = true;
  expectedNexthop.interface_name() = "Ethernet1";
  neteng::fboss::bgp::thrift::TFibNexthopSet expected;
  expected.next_hops() = {std::move(expectedNexthop)};
  expected.ref_count() = 7;

  EXPECT_EQ(toThriftFibNexthopSet(nexthops, 7), expected);
}

} // namespace
} // namespace facebook::bgp
