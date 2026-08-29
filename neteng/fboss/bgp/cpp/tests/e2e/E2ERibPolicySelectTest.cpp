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
 * E2E tests for RIB policy evaluation - Part 2 (Selection).
 *
 * These tests verify path selection policies and UCMP through the
 * complete BGP pipeline.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib, RibPolicy
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

class E2ERibPolicySelectTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    createRib();
    createPeerManager(/*enableUpdateGroup=*/false,
                      /*enableEgressBackpressure=*/true);
  }

  void bringUpAllPeersWithEor() {
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);
    BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
    BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
    sendEoRToPeer(peerId3);
    sendEoRToPeer(peerId4);
    sendEoRToPeer(peerId5);
    EXPECT_TRUE(waitForEoR(peerId3));
    EXPECT_TRUE(waitForEoR(peerId4));
    EXPECT_TRUE(waitForEoR(peerId5));
  }
};

/*
 * Test: Default path selection (no policy)
 *
 * Verifies that when multiple paths exist with identical cost attributes
 * (same local_pref, MED, AS path length), they are treated as ECMP and
 * both nexthops are programmed to FIB.
 */
TEST_F(E2ERibPolicySelectTest, DefaultPathSelection) {
  bringUpAllPeersWithEor();

  /*
   * Two paths with identical cost attributes:
   * - Same local_pref (100)
   * - Same MED (0)
   * - Same AS path length (1)
   * These are treated as equal-cost paths for ECMP.
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Verify route is advertised to peer5 */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  /*
   * Verify both nexthops are programmed (ECMP behavior).
   * With default path selection, equal-cost paths result in multipath.
   */
  auto weightedNexthops = getFibWeightedNexthops("10.0.0.0/8");
  ASSERT_NE(weightedNexthops, nullptr)
      << "Weighted nexthops should be programmed to FIB";
  EXPECT_EQ(weightedNexthops->size(), 2)
      << "Default selection with equal-cost paths should result in ECMP";

  auto nexthop1 = folly::IPAddress("11.0.0.1");
  auto nexthop2 = folly::IPAddress("11.0.0.2");
  EXPECT_TRUE(weightedNexthops->count(nexthop1) > 0)
      << "ECMP should include nexthop from peer3: " << nexthop1.str();
  EXPECT_TRUE(weightedNexthops->count(nexthop2) > 0)
      << "ECMP should include nexthop from peer4: " << nexthop2.str();
}

/*
 * Test: Path selection with community criteria
 *
 * Verifies that when routes with and without communities compete,
 * the community attribute is preserved in the advertised route.
 */
TEST_F(E2ERibPolicySelectTest, PathSelectionWithCommunity) {
  bringUpAllPeersWithEor();

  /* Route with community from peer3 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0);

  /* Route without community from peer4 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /*
   * Verify the route is advertised with community preserved.
   * With equal-cost paths, the route from peer3 (with community) should
   * have its community attribute included in the advertisement.
   */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr5,
      "127.5.0.4",
      "4200000001 65001",
      "100:1"))
      << "Route should be advertised with community 100:1 from peer3";
}

/*
 * Test: ECMP path selection
 *
 * Verifies that two equal-cost paths (same AS path, local_pref, MED) are both
 * programmed to FIB as ECMP nexthops.
 */
TEST_F(E2ERibPolicySelectTest, EcmpPathSelection) {
  bringUpAllPeersWithEor();

  /*
   * Two equal-cost paths with identical attributes:
   * - Same AS path length (65001)
   * - Same local_pref (100)
   * - Same MED (0)
   * This should result in ECMP with both nexthops programmed to FIB.
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Verify route is advertised to peer5 */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  /*
   * Verify ECMP: both nexthops should be present in FIB.
   * The FIB receives the BGP nexthops from the UPDATE messages (11.0.0.1 and
   * 11.0.0.2), not the peer's configured interface nexthops.
   */
  auto weightedNexthops = getFibWeightedNexthops("10.0.0.0/8");
  ASSERT_NE(weightedNexthops, nullptr)
      << "Weighted nexthops should be programmed to FIB";
  EXPECT_EQ(weightedNexthops->size(), 2)
      << "ECMP should have 2 nexthops for equal-cost paths";

  auto nexthop1 = folly::IPAddress("11.0.0.1");
  auto nexthop2 = folly::IPAddress("11.0.0.2");
  EXPECT_TRUE(weightedNexthops->count(nexthop1) > 0)
      << "ECMP should include nexthop from peer3: " << nexthop1.str();
  EXPECT_TRUE(weightedNexthops->count(nexthop2) > 0)
      << "ECMP should include nexthop from peer4: " << nexthop2.str();
}

/*
 * Test: Override path selection on attribute change
 *
 * This test verifies that when a better path arrives, the bestpath changes
 * and the new bestpath is advertised to peers.
 *
 * Note: When the bestpath changes for an existing prefix, only an implicit
 * withdrawal (new announcement with updated attributes) is sent - not a
 * separate withdrawal followed by announcement. The receiving peer should
 * treat the new announcement as replacing the old one.
 */
TEST_F(E2ERibPolicySelectTest, OverridePathSelectionOnChange) {
  bringUpAllPeersWithEor();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");

  /*
   * Step 1: peer4 sends route with LP=200 (becomes bestpath)
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002", "", 0, 200);
  ASSERT_TRUE(waitForRouteInShadowRib(prefix, kPeerAddr4));
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65002"));

  /*
   * Step 2: peer3 sends route with LP=300 (becomes new bestpath)
   * Wait specifically for the bestpath to change to peer3
   */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 300);
  ASSERT_TRUE(waitForRouteInShadowRib(prefix, kPeerAddr3));
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "4200000001 65001"));
}

/*
 * Test: IPv6 policy processing
 */
TEST_F(E2ERibPolicySelectTest, IPv6PolicyProcessing) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));
}

/*
 * Test: Path withdrawal triggers reselection
 */
TEST_F(E2ERibPolicySelectTest, WithdrawalTriggersReselection) {
  bringUpAllPeersWithEor();

  // Primary path (higher local_pref)
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 200, 0);

  // Backup path
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  // Withdraw primary
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  // Backup should be selected
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

} // namespace facebook::bgp
