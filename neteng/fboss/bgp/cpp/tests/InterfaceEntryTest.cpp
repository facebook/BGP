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
  auto ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 0);

  // add a new Address (/32 expands to 1 IP)
  auto isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, true);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 1);

  // Check the IP is present
  auto ip = folly::IPAddress("9.0.0.1");
  EXPECT_TRUE(ipReachability.contains(ip));
  EXPECT_FALSE(ipReachability.at(ip)); // Default reachability is false

  // try to add the same address
  isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, true);

  EXPECT_FALSE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 1);

  // Add a /31 prefix (2 IPs: 9.0.0.2 and 9.0.0.3)
  isUpdated = ifEntry.updateAddr(kV4Prefix8_2Slash31, true);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 3); // 1 from /32 + 2 from /31

  auto ip2 = folly::IPAddress("9.0.0.2");
  auto ip3 = folly::IPAddress("9.0.0.3");
  EXPECT_TRUE(ipReachability.contains(ip2));
  EXPECT_TRUE(ipReachability.contains(ip3));

  // try to remove an address not present
  isUpdated = ifEntry.updateAddr(kV4Prefix8_4Slash32, false);

  EXPECT_FALSE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
  EXPECT_EQ(ipReachability.size(), 3);

  // remove the /32 address that is present
  isUpdated = ifEntry.updateAddr(kV4Prefix8_1Slash32, false);

  EXPECT_TRUE(isUpdated);
  ipReachability = ifEntry.getIpReachabilityMap();
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
 * Legacy path: updateReachabilityForAllIPs flips every seeded IP.
 */
TEST(InterfaceEntryTest, UpdateReachabilityForAllIPsTest) {
  auto ifName = "eth0";
  InterfaceEntry ifEntry{ifName};

  // Add a /31 prefix (2 IPs)
  ifEntry.updateAddr(kV4Prefix2Slash31, true);

  auto ip1 = folly::IPAddress("9.0.0.0");
  auto ip2 = folly::IPAddress("9.0.0.1");

  // Initially both should be unreachable
  EXPECT_FALSE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2));

  // Update all IPs to reachable
  auto isUpdate = ifEntry.updateReachabilityForAllIPs(true);
  EXPECT_TRUE(isUpdate);
  EXPECT_TRUE(ifEntry.isReachable(ip1));
  EXPECT_TRUE(ifEntry.isReachable(ip2));

  // Update all again with same value - should return false
  isUpdate = ifEntry.updateReachabilityForAllIPs(true);
  EXPECT_FALSE(isUpdate);

  // Update all IPs to unreachable
  isUpdate = ifEntry.updateReachabilityForAllIPs(false);
  EXPECT_TRUE(isUpdate);
  EXPECT_FALSE(ifEntry.isReachable(ip1));
  EXPECT_FALSE(ifEntry.isReachable(ip2));
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

} // namespace facebook::bgp
