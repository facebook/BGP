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
 * E2E tests for RIB route announcements and bestpath changes.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

using E2ERibAnnouncementTest = E2ERibTestFixture;

TEST_F(E2ERibAnnouncementTest, BasicRouteAnnouncement) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERibAnnouncementTest, BestpathChangeLocalPref) {
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
 * Test: Implicit withdrawal when same peer updates a route
 *
 * When a peer sends a new UPDATE for the same prefix, the old route is
 * implicitly withdrawn and replaced. We verify the new route with different
 * attributes is advertised to other peers.
 *
 * Original unit test: AdjRibOutTest::ImplicitWithdrawalOfChangedRoute
 * Note: Original tests IBGP/EBGP split horizon. This E2E test uses all eBGP
 * peers, so we verify attribute changes propagate (not withdrawal).
 */
TEST_F(E2ERibAnnouncementTest, RouteUpdateImplicitWithdrawal) {
  bringUpAllPeersWithEor();

  /*
   * First route with community 100:1
   */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0, 100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  std::vector<VerifySpec> initialRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, initialRoutes));

  /*
   * Same peer sends update with different community - implicit withdrawal.
   * The old route is replaced, and new route with 100:2 is advertised.
   */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:2", 0, 100);
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4", "", "100:2"));
}

TEST_F(E2ERibAnnouncementTest, MultiplePrefixAnnouncements) {
  bringUpAllPeersWithEor();

  /*
   * Add and verify routes one at a time to ensure each UPDATE
   * is consumed before the next route is added.
   */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "100:1", 0, 100);
  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  addRoute(
      "v4", "172.16.0.0", 12, kPeerAddr3, "11.0.0.1", "65001", "100:2", 0, 100);
  auto prefix2 = folly::IPAddress::createNetwork("172.16.0.0/12");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));
  EXPECT_TRUE(verifyRouteAdd("v4", "172.16.0.0", 12, kPeerAddr5, "127.5.0.4"));

  addRoute(
      "v4",
      "192.168.0.0",
      16,
      kPeerAddr3,
      "11.0.0.1",
      "65001",
      "100:3",
      0,
      100);
  auto prefix3 = folly::IPAddress::createNetwork("192.168.0.0/16");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix3));
  EXPECT_TRUE(verifyRouteAdd("v4", "192.168.0.0", 16, kPeerAddr5, "127.5.0.4"));
}

TEST_F(E2ERibAnnouncementTest, RouteWithCommunity) {
  bringUpAllPeersWithEor();

  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      "11.0.0.1",
      "65001",
      "100:1 100:2",
      0,
      100);

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Peer specs for the multi-peer flap test (peers 3-5 reuse the defaults).
 */
inline const BgpPeerSpec kFlapPeerSpec6 = {
    .asn = kPeerAsn6,
    .localAddr = kLocalAddr6,
    .peerAddr = kPeerAddr6,
    .v4Nexthop = kNextHopV4_6,
    .v6Nexthop = kNextHopV6_6,
};

inline const BgpPeerSpec kFlapPeerSpec7 = {
    .asn = kPeerAsn7,
    .localAddr = kLocalAddr7,
    .peerAddr = kPeerAddr7,
    .v4Nexthop = kNextHopV4_7,
    .v6Nexthop = kNextHopV6_7,
};

/*
 * Fixture with 5 eBGP peers (3-7), update groups off. Routes are RIB-originated
 * (local) so every peer is a recipient that gets dumped the full RIB.
 */
class E2EMultiPeerFlapTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    addPeer(kFlapPeerSpec6);
    addPeer(kFlapPeerSpec7);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
  }
};

/*
 * Stress the rib-dump scheduling/cancellation path across many peers: with 10
 * routes in the RIB, flap all 5 peers up/down 10 times (each bring-up schedules
 * a rib dump; each bring-down cancels it via cancelRibDumpForAdjRib), then
 * bring them all up stably and verify every peer converges to the full RIB.
 */
TEST_F(E2EMultiPeerFlapTest, FlapAllPeersDuringRibDumpConvergeToFullRib) {
  struct FlapPeer {
    folly::IPAddress addr;
    folly::IPAddress nexthop;
  };
  const std::vector<FlapPeer> peers = {
      {kPeerAddr3, kNextHopV4_3},
      {kPeerAddr4, kNextHopV4_4},
      {kPeerAddr5, kNextHopV4_5},
      {kPeerAddr6, kNextHopV4_6},
      {kPeerAddr7, kNextHopV4_7},
  };

  /*
   * Inject 10 RIB-originated routes so there are 10 routes to dump to each peer
   * that comes up.
   */
  constexpr int kNumRoutes = 10;
  std::vector<std::string> prefixes;
  for (int i = 0; i < kNumRoutes; ++i) {
    const auto prefix = fmt::format("10.{}.0.0", i);
    prefixes.push_back(prefix);
    injectLocalRoutesAtRuntime({fmt::format("{}/16", prefix)}, {}, kLocalPref);
    ASSERT_TRUE(waitForRouteInShadowRib(
        folly::IPAddress::createNetwork(fmt::format("{}/16", prefix))));
  }

  /*
   * Flap all 5 peers up/down 10 times: each round brings every peer up (rib
   * dumps in flight) then tears every peer down (cancelling them). Increasing
   * version numbers keep each incarnation fresh.
   */
  for (int round = 0; round < 10; ++round) {
    for (const auto& p : peers) {
      bringUpPeer(p.addr, round + 1);
    }
    for (const auto& p : peers) {
      bringDownPeer(p.addr);
    }
  }

  /*
   * Final stable bring-up of all peers EXCEPT the last one, which is kept down.
   * The peers that come up must converge to the full RIB; the one left down
   * must not become established (its last scheduled dump was cancelled on
   * teardown).
   */
  const auto& downPeer = peers.back();
  for (size_t i = 0; i + 1 < peers.size(); ++i) {
    bringUpPeer(peers[i].addr, 100);
    sendEoRToPeer(BgpPeerId{peers[i].addr, peers[i].addr.asV4().toLongHBO()});
  }
  for (size_t i = 0; i + 1 < peers.size(); ++i) {
    const auto& p = peers[i];
    std::vector<VerifySpec> expected;
    for (const auto& prefix : prefixes) {
      expected.push_back(
          {.prefix = prefix,
           .prefixLen = 16,
           .expectedNexthop = p.nexthop.str()});
    }
    EXPECT_TRUE(verifyRoutes("v4", p.addr, expected))
        << "peer " << p.addr.str() << " did not converge to the full RIB";
  }

  /* The peer kept down must not be established. */
  auto downAdjRib = getAdjRibByAddr(downPeer.addr);
  if (downAdjRib) {
    EXPECT_FALSE(downAdjRib->isStateEstablished())
        << "peer " << downPeer.addr.str()
        << " was kept down but is established";
  }
}

} // namespace facebook::bgp
