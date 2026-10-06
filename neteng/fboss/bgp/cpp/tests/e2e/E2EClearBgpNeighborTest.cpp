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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include <thrift/lib/cpp/TApplicationException.h>

#include "neteng/fboss/bgp/cpp/BgpServiceBase.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/TestSessionManager.h"

using namespace facebook::neteng::fboss::bgp::thrift;
using facebook::neteng::fboss::bgp_attr::TBgpAfi;

namespace facebook::bgp {

/*
 * Tests that exercise OUT clear and HARD_RESET. These do NOT require Route
 * Refresh capability negotiation — operator-initiated re-dump (OUT) and
 * session restart (HARD_RESET) are local actions that bypass RFC 2918/7313
 * peer-protocol gates. The fixture leaves both Route Refresh capabilities
 * disabled on every peer to verify that independence.
 */
class E2EClearBgpNeighborTest : public E2ERibTestFixture {
 protected:
  std::unique_ptr<BgpServiceBase> bgpService_;

  void SetUp() override {
    E2ERibTestFixture::SetUp();
    bgpService_ = std::make_unique<BgpServiceBase>(
        *peerManager_,
        configManager_,
        *rib_,
        getWatchdog(),
        /*enable_thrift_protection=*/false);
  }
};

struct RequestedAfiTestCase {
  const char* name;
  TBgpAfi afi;
  const char* family;
  const char* prefix;
  int prefixLength;
  const char* expectedNexthop;
};

class E2EClearBgpNeighborAfiTest
    : public E2EClearBgpNeighborTest,
      public ::testing::WithParamInterface<RequestedAfiTestCase> {};

TEST_F(E2EClearBgpNeighborTest, OutReAnnouncesRoutes) {
  bringUpAllPeersWithEor();

  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100, 0);
  auto v4prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(v4prefix));
  EXPECT_TRUE(verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, "127.5.0.4"));

  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");
  auto v6prefix = folly::IPAddress::createNetwork("2001:db8::/32");
  ASSERT_TRUE(waitForRouteInShadowRib(v6prefix));
  EXPECT_TRUE(verifyRouteAdd(
      "v6", "2001:db8::", 32, kPeerAddr5, "2401:db00:e011:411:1000::2d"));

  drainPeerQueueCompletely(peerId5);

  folly::coro::blockingWait(bgpService_->co_clearBgpNeighborImpl(
      std::make_unique<std::string>(kPeerAddr5.str()),
      ClearBgpNeighborDirection::ROUTE_REFRESH_OUT,
      TBgpAfi::AFI_ALL));

  /*
   * AdjRibOut may emit the per-AFI announcements in either order during a
   * full re-dump. Collect both updates and check prefix presence
   * order-agnostically.
   */
  auto upd1 = readOutboundUpdateToPeer(peerId5);
  auto upd2 = readOutboundUpdateToPeer(peerId5);
  ASSERT_TRUE(upd1.has_value() && upd2.has_value())
      << "Expected 2 re-announce updates after OUT clear";

  auto matchPrefixWithNexthop = [](const nettools::bgplib::BgpUpdate2& update,
                                   bool isV4,
                                   const folly::CIDRNetwork& cidr,
                                   const std::string& expectedNexthop) {
    return findPrefixInAnnouncements(update, isV4, cidr, 0) &&
        verifyRouteAttributes(update, expectedNexthop);
  };

  bool v4Found = matchPrefixWithNexthop(**upd1, true, v4prefix, "127.5.0.4") ||
      matchPrefixWithNexthop(**upd2, true, v4prefix, "127.5.0.4");
  bool v6Found = matchPrefixWithNexthop(
                     **upd1, false, v6prefix, "2401:db00:e011:411:1000::2d") ||
      matchPrefixWithNexthop(
                     **upd2, false, v6prefix, "2401:db00:e011:411:1000::2d");

  EXPECT_TRUE(v4Found)
      << "OUT clear should re-announce v4 route with expected nexthop";
  EXPECT_TRUE(v6Found)
      << "OUT clear should re-announce v6 route with expected nexthop";
  EXPECT_TRUE(waitForEoR(peerId5)) << "Expected first AFI EoR";
  EXPECT_TRUE(waitForEoR(peerId5)) << "Expected second AFI EoR";
}

TEST_P(E2EClearBgpNeighborAfiTest, OutReAnnouncesOnlyRequestedAfi) {
  bringUpAllPeersWithEor();

  const BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  const auto& testCase = GetParam();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001", "", 0, 100, 0);
  addRoute("v6", "2001:db8::", 32, kPeerAddr3, "2001:db8::1", "65001");

  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("10.0.0.0/8")));
  ASSERT_TRUE(waitForRouteInShadowRib(
      folly::IPAddress::createNetwork("2001:db8::/32")));

  drainPeerQueueCompletely(peerId5);

  folly::coro::blockingWait(bgpService_->co_clearBgpNeighborImpl(
      std::make_unique<std::string>(kPeerAddr5.str()),
      ClearBgpNeighborDirection::ROUTE_REFRESH_OUT,
      testCase.afi));

  EXPECT_TRUE(verifyRouteAdd(
      testCase.family,
      testCase.prefix,
      testCase.prefixLength,
      kPeerAddr5,
      testCase.expectedNexthop,
      "",
      "",
      0,
      50));

  const auto drained = drainPeerQueueCompletely(peerId5, 3, 10);
  EXPECT_EQ(drained, 1)
      << "Expected only the requested-AFI EoR and no other-family route";
}

INSTANTIATE_TEST_SUITE_P(
    RequestedAfi,
    E2EClearBgpNeighborAfiTest,
    ::testing::Values(
        RequestedAfiTestCase{
            "IPv4",
            TBgpAfi::AFI_IPV4,
            "v4",
            "10.0.0.0",
            8,
            "127.5.0.4"},
        RequestedAfiTestCase{
            "IPv6",
            TBgpAfi::AFI_IPV6,
            "v6",
            "2001:db8::",
            32,
            "2401:db00:e011:411:1000::2d"}),
    [](const ::testing::TestParamInfo<RequestedAfiTestCase>& info) {
      return info.param.name;
    });

TEST_F(E2EClearBgpNeighborTest, InvalidPeerNoSideEffects) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_THROW(
      folly::coro::blockingWait(bgpService_->co_clearBgpNeighborImpl(
          std::make_unique<std::string>("192.168.99.99"),
          ClearBgpNeighborDirection::HARD_RESET,
          TBgpAfi::AFI_ALL)),
      apache::thrift::TApplicationException);

  EXPECT_TRUE(waitForRouteInShadowRib(prefix))
      << "Existing routes should be unaffected by invalid peer clear";
}

TEST_F(E2EClearBgpNeighborTest, InClearThrowsWhenRouteRefreshNotNegotiated) {
  /*
   * IN clear sends a Route Refresh request to the peer, so either standard or
   * Enhanced Route Refresh must be negotiated. This fixture enables neither,
   * and the call must fail rather than silently succeeding.
   */
  bringUpAllPeersWithEor();

  EXPECT_THROW(
      folly::coro::blockingWait(bgpService_->co_clearBgpNeighborImpl(
          std::make_unique<std::string>(kPeerAddr3.str()),
          ClearBgpNeighborDirection::ROUTE_REFRESH_IN,
          TBgpAfi::AFI_ALL)),
      apache::thrift::TApplicationException);
}

} // namespace facebook::bgp
