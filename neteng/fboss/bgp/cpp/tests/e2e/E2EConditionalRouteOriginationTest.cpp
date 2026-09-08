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
 * E2E tests for conditional route origination (require_nexthop_resolution).
 *
 * A conditional local route is originated and advertised only while its
 * configured nexthop is resolved (i.e. an NDP/ARP neighbor is present for it).
 * When that neighbor is lost -- e.g. the egress port goes down and
 * FsdbNeighborWatcher reports the nexthop as unresolved -- the route must be
 * withdrawn from the RIB and un-advertised to peers promptly.
 *
 * These tests drive the full RibDC + PeerManagerBase path with only the FIB
 * mocked. Conditional origination is a RibDC-only feature, so this target links
 * :e2e_test_fixture_dc. Resolution changes are delivered the way production
 * delivers them: a NexthopResolutionUpdate on the RIB-in queue, exactly as
 * FsdbNeighborWatcher pushes it from FSDB neighbor-table diffs.
 *
 * Regression coverage for S661800 / T270671727, where a conditional VIP stayed
 * in the RIB and advertised for ~5 minutes after its nexthop's NDP entry was
 * removed on port-down.
 */

#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

namespace {
// TEST-NET-3 documentation prefix (RFC 5737); non-routable and collision-free.
constexpr auto kConditionalPrefixStr = "192.0.2.0/24";
constexpr auto kConditionalPrefixAddr = "192.0.2.0";
constexpr uint8_t kConditionalPrefixLen = 24;
} // namespace

class E2EConditionalRouteOriginationTest : public E2ETestFixture {
 protected:
  folly::CIDRNetwork conditionalPrefix() const {
    return folly::IPAddress::createNetwork(kConditionalPrefixStr);
  }

  /*
   * The nexthop whose NDP/ARP resolution gates origination (the neighbor
   * reachable out of the egress port). The same address is carried in the
   * NexthopResolutionUpdate delivered below.
   */
  folly::IPAddress conditionalNexthop() const {
    return folly::IPAddress("192.0.2.254");
  }

  /*
   * Register the conditional local route (require_nexthop_resolution=true)
   * before the RIB is built, then start the RIB and PeerManagerBase. peer5 is
   * the receiver whose advertisements the tests assert on.
   */
  void setUpConditionalRoute() {
    thrift::BgpNetwork network;
    network.prefix() = kConditionalPrefixStr;
    network.nexthop() = conditionalNexthop().str();
    network.require_nexthop_resolution() = true;
    localRoutes_[conditionalPrefix()] = std::move(network);

    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
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

  /*
   * Deliver a nexthop resolution change to the RIB, mirroring what
   * FsdbNeighborWatcher pushes when NDP/ARP neighbor state changes.
   * blockingWait is safe here: this runs on the test thread, and the RIB drains
   * ribInQ_ on its own event base.
   */
  void sendNexthopResolution(
      std::vector<folly::IPAddress> resolved,
      std::vector<folly::IPAddress> unresolved) {
    folly::coro::blockingWait(ribInQ_.push(
        NexthopResolutionUpdate(std::move(resolved), std::move(unresolved))));
  }
};

/*
 * With no nexthop resolution received, a conditional route must NOT originate:
 * it is absent from the RIB and never advertised.
 */
TEST_F(E2EConditionalRouteOriginationTest, NotOriginatedUntilNexthopResolved) {
  setUpConditionalRoute();
  bringUpAllPeersWithEor();

  EXPECT_TRUE(verifyRouteNotInShadowRib(conditionalPrefix()));
}

/*
 * Core S661800 scenario. Resolve the nexthop (neighbor up on the port) so the
 * route originates and is advertised, then mark it unresolved (port down / NDP
 * entry gone) and verify the route is withdrawn from the RIB and a withdrawal
 * is advertised to the peer -- instead of lingering as it did in the SEV.
 */
TEST_F(E2EConditionalRouteOriginationTest, WithdrawnWhenNexthopUnresolved) {
  setUpConditionalRoute();
  bringUpAllPeersWithEor();

  // Neighbor resolves: route originates and is advertised (next-hop-self).
  sendNexthopResolution({conditionalNexthop()}, {});
  ASSERT_TRUE(waitForRouteInShadowRib(conditionalPrefix()));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      kConditionalPrefixAddr,
      kConditionalPrefixLen,
      kPeerAddr5,
      kNextHopV4_5.str()));

  /*
   * Port down: neighbor lost. Route must leave the RIB and be withdrawn to
   * peer.
   */
  sendNexthopResolution({}, {conditionalNexthop()});
  ASSERT_TRUE(waitForRouteWithdrawnFromRib(kConditionalPrefixStr));
  EXPECT_TRUE(verifyRouteWithdraw(
      "v4", kConditionalPrefixAddr, kConditionalPrefixLen, kPeerAddr5));
  EXPECT_TRUE(verifyRouteNotInShadowRib(conditionalPrefix()));
}

/*
 * After a withdraw-on-unresolved, re-resolving the nexthop (port back up) must
 * re-originate and re-advertise the route.
 */
TEST_F(E2EConditionalRouteOriginationTest, ReoriginatedAfterReresolution) {
  setUpConditionalRoute();
  bringUpAllPeersWithEor();

  sendNexthopResolution({conditionalNexthop()}, {});
  ASSERT_TRUE(waitForRouteInShadowRib(conditionalPrefix()));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      kConditionalPrefixAddr,
      kConditionalPrefixLen,
      kPeerAddr5,
      kNextHopV4_5.str()));

  sendNexthopResolution({}, {conditionalNexthop()});
  ASSERT_TRUE(waitForRouteWithdrawnFromRib(kConditionalPrefixStr));
  EXPECT_TRUE(verifyRouteWithdraw(
      "v4", kConditionalPrefixAddr, kConditionalPrefixLen, kPeerAddr5));

  sendNexthopResolution({conditionalNexthop()}, {});
  ASSERT_TRUE(waitForRouteInShadowRib(conditionalPrefix()));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      kConditionalPrefixAddr,
      kConditionalPrefixLen,
      kPeerAddr5,
      kNextHopV4_5.str()));
}

} // namespace facebook::bgp
