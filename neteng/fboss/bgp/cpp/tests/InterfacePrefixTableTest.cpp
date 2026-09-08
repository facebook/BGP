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

#include <folly/Conv.h>
#include <folly/IPAddress.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "neteng/fboss/bgp/cpp/nexthopTracker/InterfacePrefixTable.h"

namespace facebook::bgp {

/**
 * Adding/removing a prefix records or drops the directly-connected
 * classification and keeps address families isolated.
 */
TEST(InterfacePrefixTableTest, AddRemoveClassifies) {
  InterfacePrefixTable table;

  // Adding a wide /16 records the prefix (coverage appears).
  auto wide = folly::IPAddress::createNetwork("10.10.0.1/16");
  EXPECT_TRUE(table.addPrefix(wide, 1));

  // The prefix classifies addresses within it as directly connected.
  EXPECT_TRUE(table.isDirectlyConnected(folly::IPAddress("10.10.0.2")));
  EXPECT_TRUE(table.isDirectlyConnected(folly::IPAddress("10.10.255.254")));
  EXPECT_FALSE(table.isDirectlyConnected(folly::IPAddress("10.11.0.1")));
  // Family isolation: a v6 address is never covered by a v4 prefix.
  EXPECT_FALSE(table.isDirectlyConnected(folly::IPAddress("2401:db00::1")));

  // Adding the same prefix again is a no-op (no new coverage).
  EXPECT_FALSE(table.addPrefix(wide, 1));

  // Removing the prefix drops it (coverage disappears).
  EXPECT_TRUE(table.removePrefix(wide));
  EXPECT_FALSE(table.isDirectlyConnected(folly::IPAddress("10.10.0.2")));

  // Removing a prefix that is not tracked is a no-op.
  EXPECT_FALSE(table.removePrefix(wide));
}

/**
 * Two interface addresses in the same subnet (e.g. 10.0.0.1/16 and 10.0.0.2/16)
 * normalize to a single radix prefix node. addPrefix reports a coverage change
 * only when that node is created, and removePrefix only when it is destroyed:
 * adding a sibling into an already-covered subnet returns false, and removing
 * one sibling keeps the subnet directly connected as long as another address
 * still contributes it. This is exactly what prevents the over-drop where
 * removing one of two same-subnet addresses wrongly declassified peers still
 * reachable via the other. Because each node tracks the set of contributing
 * addresses (not a bare refcount), duplicate address events are idempotent.
 */
TEST(InterfacePrefixTableTest, SameSubnetSiblingsRefcount) {
  InterfacePrefixTable table;

  /*
   * Build the prefixes the way the kernel/netlink path does: host address +
   * prefix length, NOT masked to the network. createNetwork() masks by default,
   * which would collapse two same-subnet addresses into one identical entry.
   */
  folly::CIDRNetwork addr1{folly::IPAddress("10.0.0.1"), 16};
  folly::CIDRNetwork addr2{folly::IPAddress("10.0.0.2"), 16}; // same /16
  auto peer = folly::IPAddress("10.0.5.7"); // covered by the shared /16

  // First address creates the prefix node (on interface 1).
  EXPECT_TRUE(table.addPrefix(addr1, 1));
  EXPECT_TRUE(table.isDirectlyConnected(peer));

  // A sibling in the same subnet (on interface 2) does not change coverage.
  EXPECT_FALSE(table.addPrefix(addr2, 2));
  EXPECT_TRUE(table.isDirectlyConnected(peer));

  /*
   * A duplicate event for an address already recorded is also a no-op: the map
   * of contributors dedups it rather than inflating a count.
   */
  EXPECT_FALSE(table.addPrefix(addr1, 1));

  // Removing one sibling keeps the subnet directly connected via the other.
  EXPECT_FALSE(table.removePrefix(addr1));
  EXPECT_TRUE(table.isDirectlyConnected(peer));

  /*
   * Removing addr1 again is a no-op (already gone) and, crucially, does NOT
   * drop the node while addr2 still contributes it.
   */
  EXPECT_FALSE(table.removePrefix(addr1));
  EXPECT_TRUE(table.isDirectlyConnected(peer));

  // Removing the last contributor destroys the node (coverage disappears).
  EXPECT_TRUE(table.removePrefix(addr2));
  EXPECT_FALSE(table.isDirectlyConnected(peer));
}

/**
 * Nested prefixes coexist as distinct radix nodes and resolve by best (longest)
 * match. Removing a less-specific prefix must not be kept alive by a host of a
 * more-specific nested prefix: a contributor is attributed to its exact subnet,
 * so removing the /16 declassifies addresses covered only by the /16 while ones
 * still covered by the /24 stay directly connected. Families stay isolated.
 */
TEST(InterfacePrefixTableTest, NestedPrefixRemovalKeepsMoreSpecific) {
  InterfacePrefixTable table;

  auto v4Wide = folly::IPAddress::createNetwork("10.10.0.1/16");
  auto v4Specific = folly::IPAddress::createNetwork("10.10.0.1/24");
  auto v6 = folly::IPAddress::createNetwork("2401:db00:10::1/64");
  EXPECT_TRUE(table.addPrefix(v4Wide, 1));
  EXPECT_TRUE(table.addPrefix(v4Specific, 1));
  EXPECT_TRUE(table.addPrefix(v6, 2));

  auto peerSpecific = folly::IPAddress("10.10.0.2"); // in /24 and /16
  auto peerWide = folly::IPAddress("10.10.5.7"); // in /16 only
  auto peerV6 = folly::IPAddress("2401:db00:10::2"); // in 2401:db00:10::/64

  EXPECT_TRUE(table.isDirectlyConnected(peerSpecific));
  EXPECT_TRUE(table.isDirectlyConnected(peerWide));
  EXPECT_TRUE(table.isDirectlyConnected(peerV6));

  /*
   * Remove the wide /16: peerSpecific stays covered by the /24, but peerWide
   * (only in /16) is no longer directly connected. The /24's host must not keep
   * the /16 alive.
   */
  EXPECT_TRUE(table.removePrefix(v4Wide));
  EXPECT_TRUE(table.isDirectlyConnected(peerSpecific)); // still covered by /24
  EXPECT_FALSE(table.isDirectlyConnected(peerWide)); // /16 gone
  EXPECT_TRUE(table.isDirectlyConnected(peerV6));

  // Removing a prefix that is not tracked is a no-op.
  EXPECT_FALSE(
      table.removePrefix(folly::IPAddress::createNetwork("172.16.0.1/16")));

  // Removing the v6 prefix declassifies only the v6 peer (family isolation).
  EXPECT_TRUE(table.removePrefix(v6));
  EXPECT_TRUE(table.isDirectlyConnected(peerSpecific));
  EXPECT_FALSE(table.isDirectlyConnected(peerV6));
}

/**
 * The table is a radix tree, so directly-connected classification is an
 * O(address-bits) longest-prefix (best) match that scales to thousands of
 * secondary interface addresses rather than degrading linearly. Add many
 * prefixes, verify coverage via best match for covered and uncovered addresses,
 * then remove them all and verify declassification.
 */
TEST(InterfacePrefixTableTest, BestMatchScalesToManyPrefixes) {
  InterfacePrefixTable table;

  constexpr int kNumPrefixes = 4000;
  std::vector<folly::CIDRNetwork> prefixes;
  prefixes.reserve(kNumPrefixes);
  for (int i = 0; i < kNumPrefixes; ++i) {
    // Distinct /24s: 10.<hi>.<lo>.0/24 (hi in 0..15, lo in 0..255).
    prefixes.push_back(
        folly::IPAddress::createNetwork(
            folly::to<std::string>("10.", i / 256, ".", i % 256, ".0/24")));
    // Each /24 lives on a distinct interface (ifindex i).
    EXPECT_TRUE(table.addPrefix(prefixes.back(), i));
  }

  // Every host inside an added /24 is directly connected (best match finds it).
  for (int i = 0; i < kNumPrefixes; ++i) {
    EXPECT_TRUE(table.isDirectlyConnected(
        folly::IPAddress(
            folly::to<std::string>("10.", i / 256, ".", i % 256, ".42"))));
  }
  // Addresses outside every prefix are not directly connected.
  EXPECT_FALSE(table.isDirectlyConnected(folly::IPAddress("172.16.0.1")));
  EXPECT_FALSE(table.isDirectlyConnected(folly::IPAddress("2401:db00::1")));

  // Removing every prefix declassifies all of them.
  for (const auto& prefix : prefixes) {
    EXPECT_TRUE(table.removePrefix(prefix));
  }
  EXPECT_FALSE(table.isDirectlyConnected(folly::IPAddress("10.0.0.42")));
}

/**
 * coveringIfIndex returns the interface of the best-matching prefix so the
 * interface-state path can check the covering interface's link state, and
 * returns nullopt when the address is not directly connected (including across
 * address families).
 */
TEST(InterfacePrefixTableTest, CoveringIfIndexTargetsInterface) {
  InterfacePrefixTable table;
  table.addPrefix(folly::IPAddress::createNetwork("10.10.0.1/16"), 7);

  // A covered host resolves to its contributing interface.
  auto ifIndex = table.coveringIfIndex(folly::IPAddress("10.10.5.7"));
  ASSERT_TRUE(ifIndex.has_value());
  EXPECT_EQ(7, *ifIndex);

  // An address outside every prefix yields no interface.
  EXPECT_FALSE(
      table.coveringIfIndex(folly::IPAddress("172.16.0.1")).has_value());
  // Family isolation: a v6 address is not covered by a v4 prefix.
  EXPECT_FALSE(
      table.coveringIfIndex(folly::IPAddress("2401:db00::1")).has_value());
}

/**
 * On a routed backbone a subnet lives on a single L3 interface, possibly via
 * several addresses (a primary and one or more secondaries). They all
 * contribute the one interface index, so a covered host resolves to that
 * interface, and dropping one address keeps the resolution intact while another
 * still contributes the subnet.
 */
TEST(InterfacePrefixTableTest, CoveringIfIndexSameInterfaceSecondaries) {
  InterfacePrefixTable table;
  /*
   * Primary and secondary addresses of interface 3 in the same /16 (host
   * addresses kept unmasked so both are recorded as distinct contributors).
   */
  folly::CIDRNetwork primary{folly::IPAddress("10.0.0.1"), 16};
  folly::CIDRNetwork secondary{folly::IPAddress("10.0.0.5"), 16};
  table.addPrefix(primary, 3);
  table.addPrefix(secondary, 3);

  auto peer = folly::IPAddress("10.0.9.9"); // covered by the shared /16
  auto ifIndex = table.coveringIfIndex(peer);
  ASSERT_TRUE(ifIndex.has_value());
  EXPECT_EQ(3, *ifIndex);

  /*
   * Dropping the primary leaves the subnet covered by the secondary, still
   * resolving to interface 3.
   */
  EXPECT_FALSE(table.removePrefix(primary));
  ifIndex = table.coveringIfIndex(peer);
  ASSERT_TRUE(ifIndex.has_value());
  EXPECT_EQ(3, *ifIndex);
}

/**
 * Resolution is by best (longest) match: a host inside a nested prefix resolves
 * to the more-specific prefix's interface, not the covering one.
 */
TEST(InterfacePrefixTableTest, CoveringIfIndexUsesBestMatch) {
  InterfacePrefixTable table;
  // A wide /16 on interface 1 and a nested /24 on interface 2.
  table.addPrefix(folly::IPAddress::createNetwork("10.10.0.1/16"), 1);
  table.addPrefix(folly::IPAddress::createNetwork("10.10.0.1/24"), 2);

  // A host inside the /24 resolves to the more-specific prefix's interface.
  auto specific = table.coveringIfIndex(folly::IPAddress("10.10.0.42"));
  ASSERT_TRUE(specific.has_value());
  EXPECT_EQ(2, *specific);
  // A host covered only by the /16 resolves to interface 1.
  auto wide = table.coveringIfIndex(folly::IPAddress("10.10.5.7"));
  ASSERT_TRUE(wide.has_value());
  EXPECT_EQ(1, *wide);
}

} // namespace facebook::bgp
