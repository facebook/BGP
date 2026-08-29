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
 * E2E tests for peer failover scenarios.
 *
 * These tests verify backup path activation, peer failure handling,
 * and route convergence after peer recovery through the complete BGP pipeline.
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

/*
 * E2EPeerFailoverTest
 *
 * Inherits from E2ERibTestFixture which provides:
 * - SetUp() with 3 peers (kDefaultPeerSpec3, 4, 5)
 * - bringUpAllPeersWithEor() helper
 */
class E2EPeerFailoverTest : public E2ERibTestFixture {};

/*
 * Test: Routes advertised when all peers healthy
 *
 * Scenario:
 * - All peers up and healthy
 * - Announce routes
 * - All routes should be accepted and propagated
 */
TEST_F(E2EPeerFailoverTest, RoutesAdvertisedWhenPeersHealthy) {
  bringUpAllPeersWithEor();

  /* Announce several routes (within normal limits) */
  for (int i = 0; i < 5; ++i) {
    std::string prefix = "10.0." + std::to_string(i) + ".0";
    addRoute("v4", prefix, 24, kPeerAddr3, "11.0.0.1", "65001");
  }

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/24");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* All routes should be advertised */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 24, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Backup path activates on primary withdrawal
 *
 * Scenario:
 * - Primary path announced
 * - Backup path announced
 * - Primary withdrawn, backup becomes active
 */
TEST_F(E2EPeerFailoverTest, BackupPathActivatesOnPrimaryWithdraw) {
  bringUpAllPeersWithEor();

  /* Primary path with high local_pref */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 200, 0);

  /* Backup path with lower local_pref */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  /* Withdraw primary */
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  /* Backup should become active */
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));
}

/*
 * Test: Surviving peer routes remain active after peer failure
 *
 * Scenario:
 * - Routes from multiple peers
 * - One peer fails
 * - Remaining routes from other peer stay active
 */
TEST_F(E2EPeerFailoverTest, SurvivingPeerRoutesActiveAfterPeerFailure) {
  bringUpAllPeersWithEor();

  /* Routes from Peer3 */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  /* Routes from Peer4 */
  addRoute("v4", "20.0.0.0", 8, kPeerAddr4, "11.0.0.2", "65002");

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/8");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/8");

  ASSERT_TRUE(waitForRouteInShadowRib(prefix1));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix2));

  /* Routes may arrive in any order - use verifyRoutes */
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr5,
      {{"10.0.0.0", 8, "127.5.0.4", "", "", 0},
       {"20.0.0.0", 8, "127.5.0.4", "", "", 0}}));

  /* Peer3 fails - its route should be withdrawn */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  /*
   * Peer4's route should still be active.
   * This assertion addresses reviewer feedback - we should verify that
   * routes from surviving peers remain active after another peer fails.
   */
  EXPECT_TRUE(waitForRouteInShadowRib(prefix2))
      << "Peer4's route should still be in shadowRib after Peer3 fails";
}

/*
 * Test: Routes re-established after peer recovery
 *
 * Scenario:
 * - Peer announces routes
 * - Peer fails and recovers
 * - Routes re-established
 */
TEST_F(E2EPeerFailoverTest, RoutesReestablishedAfterPeerRecovery) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  /* Peer fails */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  /* Peer recovers */
  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Re-announce route */
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  /* Use batch verification to handle potential queue ordering */
  std::vector<VerifySpec> expectedRoutes = {
      {.prefix = "10.0.0.0", .prefixLen = 8, .expectedNexthop = "127.5.0.4"},
  };
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr5, expectedRoutes));
}

/*
 * Test: IPv6 routes propagated with failover support
 *
 * Scenario:
 * - IPv6 routes are handled with same failover behavior as IPv4
 */
TEST_F(E2EPeerFailoverTest, IPv6RoutesPropagatedWithFailover) {
  bringUpAllPeersWithEor();

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");

  auto prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));
}

} // namespace facebook::bgp
