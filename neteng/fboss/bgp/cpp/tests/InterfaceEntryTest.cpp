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

#include <folly/IPAddress.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/InterfaceEntry.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace ::testing;
namespace facebook::bgp {

/**
 * Verify updating the ifindex correctly changes the entry
 * and its return isUpdated is returned correctly
 */
TEST(InterfaceEntryTest, UpdateIfIndexTest) {
  auto ifName = "eth0";
  InterfaceEntry ifEntry{ifName};

  EXPECT_EQ(ifEntry.getIfName(), ifName);

  // update ifIndex
  auto isUpdate = ifEntry.updateIfIndex(10);
  EXPECT_EQ(ifEntry.getIfIndex(), 10);
  EXPECT_TRUE(isUpdate);

  // no update
  isUpdate = ifEntry.updateIfIndex(10);
  EXPECT_EQ(ifEntry.getIfIndex(), 10);
  EXPECT_FALSE(isUpdate);
}

/**
 * Legacy (subnet-enumeration) path: adding an address enumerates the host IPs
 * in the prefix (bounded by kDefaultMaxIPsInCIDR) and seeds them; removing it
 * clears them.
 */
TEST(InterfaceEntryTest, UpdateAddrTest) {
  auto ifName = "eth0";
  InterfaceEntry ifEntry{ifName};

  EXPECT_EQ(ifEntry.getIfName(), ifName);

  // Initially no IPs
  auto ipReachability = ifEntry.getNeighborStateMap();
  EXPECT_EQ(ipReachability.size(), 0);

  // add a new Address (/32 expands to 1 IP)
  auto isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, true);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getNeighborStateMap();
  EXPECT_EQ(ipReachability.size(), 1);

  // Check the IP is present
  auto ip = folly::IPAddress("9.0.0.1");
  EXPECT_TRUE(ipReachability.contains(ip));
  EXPECT_FALSE(ipReachability.at(ip)); // Default reachability is false

  // try to add the same address
  isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, true);

  EXPECT_FALSE(isUpdated);
  ipReachability = ifEntry.getNeighborStateMap();
  EXPECT_EQ(ipReachability.size(), 1);

  // Add a /31 prefix (2 IPs: 9.0.0.2 and 9.0.0.3)
  isUpdated = ifEntry.updateAddr(kV4Prefix8_2Slash31, true);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getNeighborStateMap();
  EXPECT_EQ(ipReachability.size(), 3); // 1 from /32 + 2 from /31

  auto ip2 = folly::IPAddress("9.0.0.2");
  auto ip3 = folly::IPAddress("9.0.0.3");
  EXPECT_TRUE(ipReachability.contains(ip2));
  EXPECT_TRUE(ipReachability.contains(ip3));

  // try to remove an address not present
  isUpdated = ifEntry.updateAddr(kV4Prefix8_4Slash32, false);

  EXPECT_FALSE(isUpdated);
  ipReachability = ifEntry.getNeighborStateMap();
  EXPECT_EQ(ipReachability.size(), 3);

  // remove the /32 address that is present
  isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, false);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getNeighborStateMap();
  EXPECT_EQ(ipReachability.size(), 2); // Only /31 IPs remain
  EXPECT_FALSE(ipReachability.contains(ip));
  EXPECT_TRUE(ipReachability.contains(ip2));
  EXPECT_TRUE(ipReachability.contains(ip3));
}

/**
 * Legacy path: reachability can only be set for IPs already seeded by
 * updateAddr; a neighbor event for an untracked IP is dropped.
 */
TEST(InterfaceEntryTest, UpdateReachabilityTest) {
  auto ifName = "eth0";
  InterfaceEntry ifEntry{ifName};
  // Published reachability ANDs the link half, so bring the link up first.
  ifEntry.setUp(true);

  EXPECT_EQ(ifEntry.getIfName(), ifName);

  // Add a /31 prefix (2 IPs: 9.0.0.0 and 9.0.0.1)
  auto isUpdate = ifEntry.updateAddr(kV4Prefix2Slash31, true);
  EXPECT_TRUE(isUpdate);

  auto ip1 = folly::IPAddress("9.0.0.0");
  auto ip2 = folly::IPAddress("9.0.0.1");

  // Both IPs should exist with default reachability = false
  EXPECT_FALSE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2));

  // Update reachability for first IP
  isUpdate = ifEntry.updateReachability(ip1, true);
  EXPECT_TRUE(isUpdate);
  EXPECT_TRUE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2)); // Second IP unchanged

  // Update same reachability again - should return false (no change)
  isUpdate = ifEntry.updateReachability(ip1, true);
  EXPECT_FALSE(isUpdate);
  EXPECT_TRUE(ifEntry.isReachable(ip1));

  // Update reachability for IP not in the interface - should return false
  auto unknownIp = folly::IPAddress("10.0.0.1");
  isUpdate = ifEntry.updateReachability(unknownIp, true);
  EXPECT_FALSE(isUpdate);
  EXPECT_FALSE(ifEntry.isReachable(unknownIp));
}

/**
 * Legacy path: published reachability is the kernel's neighbor state ANDed with
 * link state, and a link-down must not destroy the neighbor half. This is the
 * invariant S695537 turned on: the kernel keeps its neighbor entry across a
 * carrier bounce and emits nothing afterwards, so if bgpd cleared the map on
 * link-down there would be nothing left to restore on link-up.
 */
TEST(InterfaceEntryTest, PublishedReachabilityAndsLinkState) {
  InterfaceEntry ifEntry{"eth0"};
  ifEntry.updateAddr(kV4Prefix2Slash31, true);

  auto ip1 = folly::IPAddress("9.0.0.0");
  auto ip2 = folly::IPAddress("9.0.0.1");

  // Link up, no neighbor state yet.
  ifEntry.setUp(true);
  EXPECT_FALSE(ifEntry.hasReachableNeighbor());
  EXPECT_FALSE(ifEntry.isReachable(ip1));

  // Kernel resolves ip1.
  EXPECT_TRUE(ifEntry.updateReachability(ip1, true));
  EXPECT_TRUE(ifEntry.hasReachableNeighbor());
  EXPECT_TRUE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2));

  // Link goes down: published value drops, stored neighbor state survives.
  EXPECT_TRUE(ifEntry.setUp(false));
  EXPECT_FALSE(ifEntry.isReachable(ip1));
  EXPECT_TRUE(ifEntry.getNeighborStateMap().at(ip1));
  EXPECT_TRUE(ifEntry.hasReachableNeighbor());

  // Link returns: reachable again, with no neighbor event in between.
  EXPECT_TRUE(ifEntry.setUp(true));
  EXPECT_TRUE(ifEntry.isReachable(ip1));

  // forEachPublishedReachability reports the same ANDed values.
  ifEntry.setUp(false);
  folly::F14NodeMap<folly::IPAddress, bool> published;
  ifEntry.forEachPublishedReachability(
      [&](const folly::IPAddress& ip, bool reachable) {
        published.insert({ip, reachable});
      });
  const folly::F14NodeMap<folly::IPAddress, bool> expected{
      {ip1, false}, {ip2, false}};
  EXPECT_EQ(expected, published);
}

/**
 * Removing an address must take its reachable host IPs out of the count that
 * hasReachableNeighbor reads. Otherwise the interface reports a neighbor it no
 * longer tracks, and every later link flap republishes an empty interface.
 */
TEST(InterfaceEntryTest, AddressRemovalClearsReachableNeighbor) {
  InterfaceEntry ifEntry{"eth0"};
  ifEntry.updateAddr(kV4Prefix2Slash31, true);
  ifEntry.setUp(true);
  EXPECT_TRUE(ifEntry.updateReachability(folly::IPAddress("9.0.0.0"), true));
  EXPECT_TRUE(ifEntry.hasReachableNeighbor());

  EXPECT_TRUE(ifEntry.updateAddr(kV4Prefix2Slash31, false));

  EXPECT_TRUE(ifEntry.getNeighborStateMap().empty());
  EXPECT_FALSE(ifEntry.hasReachableNeighbor());
}

/**
 * One interface carries a v4 /31 and a v6 /127, so two peers resolve at the
 * same time. The answer stays true until the last one goes, which is why the
 * entry counts reachable neighbors instead of holding a single flag.
 */
TEST(InterfaceEntryTest, LastReachableNeighborDecidesTheAnswer) {
  InterfaceEntry ifEntry{"eth0"};
  ifEntry.updateAddr(kV4Prefix2Slash31, true);
  ifEntry.updateAddr(kV6Prefix2Slash127, true);
  ifEntry.setUp(true);

  auto v4Peer = folly::IPAddress("9.0.0.0");
  auto v6Peer = folly::IPAddress("2002::");
  EXPECT_TRUE(ifEntry.updateReachability(v4Peer, true));
  EXPECT_TRUE(ifEntry.updateReachability(v6Peer, true));

  EXPECT_TRUE(ifEntry.updateReachability(v4Peer, false));
  EXPECT_TRUE(ifEntry.hasReachableNeighbor());

  EXPECT_TRUE(ifEntry.updateReachability(v6Peer, false));
  EXPECT_FALSE(ifEntry.hasReachableNeighbor());
}

// --- Interface-state path: link state + per-interface prefix reverse index ---

/**
 * Interface link (operational) state: defaults to down, setUp reports whether
 * the state changed, and isUp reflects the current value. Drives
 * directly-connected reachability on the interface-state path.
 */
TEST(InterfaceEntryTest, LinkStateTest) {
  InterfaceEntry ifEntry{"eth0"};

  // Defaults to down.
  EXPECT_FALSE(ifEntry.isUp());

  // Bringing it up is a change.
  EXPECT_TRUE(ifEntry.setUp(true));
  EXPECT_TRUE(ifEntry.isUp());

  // Setting the same state again is a no-op.
  EXPECT_FALSE(ifEntry.setUp(true));
  EXPECT_TRUE(ifEntry.isUp());

  // Bringing it down is a change.
  EXPECT_TRUE(ifEntry.setUp(false));
  EXPECT_FALSE(ifEntry.isUp());
}

/**
 * Per-interface prefix set (the reverse index of InterfacePrefixTable):
 * addPrefix/removePrefix report whether the set changed and getPrefixes
 * reflects the current contents. Used on the interface-state path to find which
 * subnets an interface covers when a link event arrives.
 */
TEST(InterfaceEntryTest, PrefixTrackingTest) {
  InterfaceEntry ifEntry{"eth0"};

  // Empty to start.
  EXPECT_TRUE(ifEntry.getPrefixes().empty());

  folly::CIDRNetwork v4{folly::IPAddress("10.0.0.1"), 16};
  folly::CIDRNetwork v6{folly::IPAddress("2401:db00:10::1"), 64};

  // Adding a prefix is a change; re-adding the same one is not.
  EXPECT_TRUE(ifEntry.addPrefix(v4));
  EXPECT_FALSE(ifEntry.addPrefix(v4));
  EXPECT_EQ(ifEntry.getPrefixes().size(), 1);
  EXPECT_TRUE(ifEntry.getPrefixes().contains(v4));

  // A second, distinct prefix (different family) is also tracked.
  EXPECT_TRUE(ifEntry.addPrefix(v6));
  EXPECT_EQ(ifEntry.getPrefixes().size(), 2);
  EXPECT_TRUE(ifEntry.getPrefixes().contains(v6));

  // Removing a tracked prefix is a change; removing it again is not.
  EXPECT_TRUE(ifEntry.removePrefix(v4));
  EXPECT_FALSE(ifEntry.removePrefix(v4));
  EXPECT_FALSE(ifEntry.getPrefixes().contains(v4));
  EXPECT_EQ(ifEntry.getPrefixes().size(), 1);

  // Removing a prefix that was never added is a no-op.
  EXPECT_FALSE(ifEntry.removePrefix(
      folly::CIDRNetwork{folly::IPAddress("172.16.0.1"), 16}));
}

// --- Link-up hold (link-flap dampening): state and ladder ---

namespace {
/*
 * A fixed clock origin. Each hold test owns its own time, so no test sleeps
 * and no test needs an injected clock. The origin is one hour after the epoch,
 * so a test can subtract time without going below the epoch. The epoch is the
 * value that means "no hold".
 */
const auto kT0 =
    std::chrono::steady_clock::time_point{} + std::chrono::hours(1);
constexpr auto kInitial = kInitialLinkUpHoldDownTime;
constexpr auto kMax = kMaxLinkUpHoldDownTime;

// An entry with the link up and one resolved kernel neighbor.
InterfaceEntry makeResolvedEntry(const folly::IPAddress& peer) {
  InterfaceEntry ifEntry{"po245"};
  ifEntry.setUp(true);
  ifEntry.updateAddr(kV4Prefix2Slash31, true);
  ifEntry.updateReachability(peer, true);
  return ifEntry;
}
} // namespace

/**
 * The first flap starts a hold. Open/R penalizes the first link-down as well
 * (ExponentialBackoff.cpp:51-52), and this design matches it.
 */
TEST(InterfaceEntryTest, FirstFlapStartsHold) {
  InterfaceEntry ifEntry{"po245"};
  ifEntry.setUp(true);

  ifEntry.recordLinkDown(kT0, kInitial, kMax);
  EXPECT_TRUE(ifEntry.startLinkUpHold(kT0));
  EXPECT_EQ(kT0 + kInitial, ifEntry.getHoldEndTime());
}

/*
 * A long outage serves the whole hold while the link is down. A planned drain
 * or a cable repair therefore returns with no delay.
 */
TEST(InterfaceEntryTest, NoHoldAfterLongOutage) {
  InterfaceEntry ifEntry{"po245"};
  ifEntry.setUp(true);

  ifEntry.recordLinkDown(kT0, kInitial, kMax);
  const auto upTime = kT0 + std::chrono::hours(1);
  EXPECT_FALSE(ifEntry.startLinkUpHold(upTime));
  EXPECT_FALSE(ifEntry.getHoldEndTime().has_value());
  EXPECT_TRUE(ifEntry.canUseLink(upTime));
}

/*
 * An outage shorter than the hold serves part of it. The link owes only the
 * rest, and the hold still ends at one hold length after the link-down.
 */
TEST(InterfaceEntryTest, ShortOutageOwesOnlyTheRemainingHold) {
  InterfaceEntry ifEntry{"po245"};
  ifEntry.setUp(true);

  ifEntry.recordLinkDown(kT0, kInitial, kMax);
  const auto upTime = kT0 + std::chrono::milliseconds(120);
  EXPECT_TRUE(ifEntry.startLinkUpHold(upTime));
  EXPECT_EQ(kT0 + kInitial, ifEntry.getHoldEndTime());

  // 80ms of the 200ms hold are left, so the link is not usable yet.
  EXPECT_FALSE(ifEntry.canUseLink(upTime));
  EXPECT_TRUE(ifEntry.canUseLink(kT0 + kInitial));
}

/**
 * A new entry has no hold on its first link-up. holdTime_ is zero until the
 * first link-down, which is the same as Open/R currentBackoff_ == 0.
 */
TEST(InterfaceEntryTest, NoHoldBeforeFirstLinkDown) {
  InterfaceEntry ifEntry{"po245"};
  ifEntry.setUp(true);

  EXPECT_FALSE(ifEntry.startLinkUpHold(kT0));
  EXPECT_FALSE(ifEntry.getHoldEndTime().has_value());
  EXPECT_TRUE(ifEntry.canUseLink(kT0));
}

/**
 * The ladder doubles on each repeat flap and stops at max.
 */
TEST(InterfaceEntryTest, LadderDoublesOnRepeatFlapAndStopsAtMax) {
  InterfaceEntry ifEntry{"po245"};
  ifEntry.setUp(true);

  // Each link-down is 1ms after the last one, so none of them decays.
  const std::vector<int64_t> expectedMs{
      200, 400, 800, 1600, 3200, 6400, 8000, 8000};
  std::vector<int64_t> actualMs;
  auto now = kT0;
  for (size_t i = 0; i < expectedMs.size(); ++i) {
    ifEntry.recordLinkDown(now, kInitial, kMax);
    EXPECT_TRUE(ifEntry.startLinkUpHold(now));
    actualMs.push_back(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            *ifEntry.getHoldEndTime() - now)
            .count());
    now += std::chrono::milliseconds(1);
  }
  EXPECT_EQ(expectedMs, actualMs);
}

/**
 * A link that stays quiet for the whole max hold returns to the initial hold.
 */
TEST(InterfaceEntryTest, LadderReturnsToInitialAfterQuietPeriod) {
  InterfaceEntry ifEntry{"po245"};
  ifEntry.setUp(true);

  // Two flaps close together take the ladder to 400ms.
  ifEntry.recordLinkDown(kT0, kInitial, kMax);
  ifEntry.recordLinkDown(kT0 + std::chrono::milliseconds(1), kInitial, kMax);
  ifEntry.startLinkUpHold(kT0 + std::chrono::milliseconds(1));
  EXPECT_EQ(
      std::chrono::milliseconds(400),
      *ifEntry.getHoldEndTime() - (kT0 + std::chrono::milliseconds(1)));

  // A link-down max after the last one wipes the history.
  const auto quietDown = kT0 + std::chrono::milliseconds(1) + kMax;
  ifEntry.recordLinkDown(quietDown, kInitial, kMax);
  ifEntry.startLinkUpHold(quietDown);
  EXPECT_EQ(kInitial, *ifEntry.getHoldEndTime() - quietDown);
}

/**
 * RED test. A hold stops the published reachability until the clock passes the
 * hold end time. Remove "now >= holdEndTime_" from canUseLink and only this
 * test fails.
 */
TEST(InterfaceEntryTest, HoldStopsPublishedReachabilityUntilItEnds) {
  auto peer = folly::IPAddress("9.0.0.1");
  auto ifEntry = makeResolvedEntry(peer);
  ASSERT_TRUE(ifEntry.isReachable(peer, kT0));

  // Two transitions: the link-down moves the ladder, the link-up starts a hold.
  ifEntry.setUp(false);
  ifEntry.recordLinkDown(kT0, kInitial, kMax);
  ifEntry.setUp(true);
  ASSERT_TRUE(ifEntry.startLinkUpHold(kT0));

  // The kernel neighbor state survives, but the hold stops the publish.
  EXPECT_TRUE(ifEntry.getNeighborStateMap().at(peer));
  EXPECT_FALSE(
      ifEntry.isReachable(peer, kT0 + kInitial - std::chrono::milliseconds(1)));

  // The hold ends by arithmetic. No event is necessary.
  EXPECT_TRUE(ifEntry.isReachable(peer, kT0 + kInitial));
}

/**
 * A hold never stops a link-down. The published value is false at every time,
 * so the RIB withdraws the routes with no delay.
 */
TEST(InterfaceEntryTest, HoldNeverStopsLinkDown) {
  auto peer = folly::IPAddress("9.0.0.1");
  auto ifEntry = makeResolvedEntry(peer);

  ifEntry.recordLinkDown(kT0, kInitial, kMax);
  ifEntry.setUp(true);
  ASSERT_TRUE(ifEntry.startLinkUpHold(kT0));

  // The link goes down again while the hold runs.
  ifEntry.setUp(false);
  EXPECT_FALSE(ifEntry.isReachable(peer, kT0));
  EXPECT_FALSE(ifEntry.isReachable(peer, kT0 + kInitial));
  EXPECT_FALSE(ifEntry.isReachable(peer, kT0 + std::chrono::hours(1)));
}

/**
 * A hold that ended does not make a flushed neighbor reachable. This is the
 * companion to NetlinkWrapperTest TestNeighborFlushClearsStateAcrossLinkDownUp:
 * the hold changes only the link half, never the neighbor half.
 */
TEST(InterfaceEntryTest, HoldDoesNotMakeFlushedNeighborReachable) {
  auto peer = folly::IPAddress("9.0.0.1");
  auto ifEntry = makeResolvedEntry(peer);

  // The kernel removes the neighbor, then the link flaps.
  ASSERT_TRUE(ifEntry.updateReachability(peer, false));
  ifEntry.setUp(false);
  ifEntry.recordLinkDown(kT0, kInitial, kMax);
  ifEntry.setUp(true);
  ASSERT_TRUE(ifEntry.startLinkUpHold(kT0));

  // The hold ends, but the neighbor half is still false.
  EXPECT_TRUE(ifEntry.canUseLink(kT0 + kInitial));
  EXPECT_FALSE(ifEntry.isReachable(peer, kT0 + kInitial));
}

/**
 * Both public accessors read the same predicate, so they cannot disagree at
 * the same time.
 */
TEST(InterfaceEntryTest, IsReachableAndForEachGiveTheSameValue) {
  auto peer = folly::IPAddress("9.0.0.1");
  auto other = folly::IPAddress("9.0.0.0");
  auto ifEntry = makeResolvedEntry(peer);

  ifEntry.recordLinkDown(kT0, kInitial, kMax);
  ifEntry.setUp(true);
  ASSERT_TRUE(ifEntry.startLinkUpHold(kT0));

  for (const auto& now : {kT0, kT0 + kInitial}) {
    folly::F14NodeMap<folly::IPAddress, bool> published;
    ifEntry.forEachPublishedReachability(
        [&](const folly::IPAddress& ip, bool reachable) {
          published.insert({ip, reachable});
        },
        now);
    const folly::F14NodeMap<folly::IPAddress, bool> expected{
        {peer, ifEntry.isReachable(peer, now)},
        {other, ifEntry.isReachable(other, now)}};
    EXPECT_EQ(expected, published);
  }
}

} // namespace facebook::bgp
