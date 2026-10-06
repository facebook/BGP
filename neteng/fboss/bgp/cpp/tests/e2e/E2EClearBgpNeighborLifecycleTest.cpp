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

#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>

#include "neteng/fboss/bgp/cpp/BgpServiceBase.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/TestSessionManager.h"

using namespace facebook::neteng::fboss::bgp::thrift;
using facebook::neteng::fboss::bgp_attr::TBgpAfi;

namespace facebook::bgp {

class E2EClearBgpNeighborRRTest : public E2ERibTestFixture {
 protected:
  void SetUp() override {
    auto spec3WithRR = kDefaultPeerSpec3;
    spec3WithRR.enableRouteRefresh = true;
    addPeer(spec3WithRR);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
    bgpService_ = std::make_unique<BgpServiceBase>(
        *peerManager_,
        configManager_,
        *rib_,
        getWatchdog(),
        /*enable_thrift_protection=*/false);
  }

  void TearDown() override {
    bgpService_.reset();
    E2ERibTestFixture::TearDown();
  }

  std::unique_ptr<BgpServiceBase> bgpService_;
};

TEST_F(E2EClearBgpNeighborRRTest, InSendsRouteRefreshAndPreservesRoutes) {
  bringUpAllPeersWithEor();

  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  const auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  drainPeerQueueCompletely(peerId3);
  ASSERT_EQ(getPeerQueueSize(peerId3), 0);

  auto adjRib = getAdjRibByAddr(kPeerAddr3);
  ASSERT_NE(adjRib, nullptr);
  ASSERT_TRUE(
      adjRib->isRouteRefreshNegotiated() ||
      adjRib->isEnhancedRouteRefreshNegotiated());

  folly::coro::blockingWait(bgpService_->co_clearBgpNeighborImpl(
      std::make_unique<std::string>(kPeerAddr3.str()),
      ClearBgpNeighborDirection::ROUTE_REFRESH_IN,
      TBgpAfi::AFI_ALL));

  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  auto rr1 = readOutboundRouteRefreshFromPeer(peerId3);
  ASSERT_TRUE(rr1.has_value());
  EXPECT_EQ(rr1->afi(), nettools::bgplib::BgpUpdateAfi::AFI_IPv4);
  EXPECT_EQ(
      rr1->msgSubType(),
      nettools::bgplib::BgpRouteRefreshMessageSubtype::ROUTE_REFRESH_REQUEST);

  auto rr2 = readOutboundRouteRefreshFromPeer(peerId3);
  ASSERT_TRUE(rr2.has_value());
  EXPECT_EQ(rr2->afi(), nettools::bgplib::BgpUpdateAfi::AFI_IPv6);
  EXPECT_EQ(
      rr2->msgSubType(),
      nettools::bgplib::BgpRouteRefreshMessageSubtype::ROUTE_REFRESH_REQUEST);
  EXPECT_EQ(getPeerQueueSize(peerId3), 0);
}

TEST_F(E2EClearBgpNeighborRRTest, InWithAfiFilterSendsOnlyRequestedAfi) {
  bringUpAllPeersWithEor();

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  const auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  drainPeerQueueCompletely(peerId3);
  ASSERT_EQ(getPeerQueueSize(peerId3), 0);

  folly::coro::blockingWait(bgpService_->co_clearBgpNeighborImpl(
      std::make_unique<std::string>(kPeerAddr3.str()),
      ClearBgpNeighborDirection::ROUTE_REFRESH_IN,
      TBgpAfi::AFI_IPV4));

  auto rr = readOutboundRouteRefreshFromPeer(peerId3);
  ASSERT_TRUE(rr.has_value());
  EXPECT_EQ(rr->afi(), nettools::bgplib::BgpUpdateAfi::AFI_IPv4);
  EXPECT_EQ(
      rr->msgSubType(),
      nettools::bgplib::BgpRouteRefreshMessageSubtype::ROUTE_REFRESH_REQUEST);
  EXPECT_EQ(getPeerQueueSize(peerId3), 0);
}

class E2EClearBgpNeighborSessionTest : public E2ESessionTestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
    bgpService_ = std::make_unique<BgpServiceBase>(
        *peerManager_,
        configManager_,
        *rib_,
        getWatchdog(),
        /*enable_thrift_protection=*/false);
  }

  void TearDown() override {
    bgpService_.reset();
    E2ESessionTestFixture::TearDown();
  }

  std::unique_ptr<BgpServiceBase> bgpService_;
};

TEST_F(
    E2EClearBgpNeighborSessionTest,
    HardResetTerminatesAndReEstablishesSession) {
  bringUpPeerAndWait(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  auto oldVersion = testSessionManager_->getPeerVersionNumber(peerId3);
  auto oldInputQueue = testSessionManager_->getPeerInputQueue(peerId3);
  ASSERT_NE(oldVersion, nullptr);
  ASSERT_NE(oldInputQueue, nullptr);
  const auto oldVersionValue = oldVersion->get();

  folly::coro::blockingWait(bgpService_->co_clearBgpNeighborImpl(
      std::make_unique<std::string>(kPeerAddr3.str()),
      ClearBgpNeighborDirection::HARD_RESET,
      TBgpAfi::AFI_ALL));

  ASSERT_TRUE(waitForSessionEstablished(kPeerAddr3));
  auto& peerState = testSessionManager_->getPeerStates().at(peerId3);
  EXPECT_GT(peerState.versionNumber->get(), oldVersionValue);
  EXPECT_NE(peerState.adjRibOutQ, oldInputQueue);
  EXPECT_TRUE(peerState.established);
}

} // namespace facebook::bgp
