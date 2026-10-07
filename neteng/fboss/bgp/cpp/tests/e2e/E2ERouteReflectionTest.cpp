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

#include "neteng/fboss/bgp/cpp/tests/AdjRibInUtils.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

namespace facebook::bgp {

class E2ERouteReflectionTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    setLocalClusterId(kLocalClusterAddr1.asV4());

    auto sourcePeer = kDefaultPeerSpec3;
    sourcePeer.asn = kAsn1;
    sourcePeer.isRrClient = true;
    addPeer(sourcePeer);

    auto receiverPeer = kDefaultPeerSpec4;
    receiverPeer.asn = kAsn1;
    receiverPeer.isRrClient = true;
    addPeer(receiverPeer);

    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
  }

  void bringUpPeersWithEor() {
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    sendEoRToPeer(kPeerId3);
    sendEoRToPeer(kPeerId4);
    ASSERT_TRUE(waitForEoR(kPeerId3));
    ASSERT_TRUE(waitForEoR(kPeerId4));
  }

  void addReflectedRoute(
      const folly::CIDRNetwork& prefix,
      const folly::IPAddressV4& originatorId,
      const std::vector<folly::IPAddressV4>& clusterList) {
    auto update = createBgpUpdateAnnouncement(
        /*isV4=*/true,
        prefix,
        "11.0.0.1",
        {65001},
        /*communities=*/{});
    update->attrs()->originatorId() = originatorId.toLong();
    for (const auto& clusterId : clusterList) {
      update->attrs()->clusterList()->push_back(clusterId.toLong());
    }

    const auto queues = getPeerQueues(kPeerId3);
    ASSERT_TRUE(queues.has_value());
    folly::coro::blockingWait(queues->adjRibInQ->push(std::move(update)));
  }
};

TEST_F(E2ERouteReflectionTest, ClusterLoopUsesClusterIdInsteadOfRouterId) {
  bringUpPeersWithEor();

  const auto rejectedPrefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  addReflectedRoute(
      rejectedPrefix, kPeerAddr3.asV4(), {kLocalClusterAddr1.asV4()});

  const auto acceptedPrefix = folly::IPAddress::createNetwork("20.0.0.0/8");
  addReflectedRoute(acceptedPrefix, kPeerAddr3.asV4(), {kLocalAddr1.asV4()});

  ASSERT_TRUE(waitForRouteInShadowRib(acceptedPrefix));
  EXPECT_TRUE(verifyRouteNotInShadowRib(rejectedPrefix));
}

TEST_F(E2ERouteReflectionTest, ReflectedAttributesUseDistinctIdentifiers) {
  bringUpPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  const auto update = waitForOutboundUpdate(kPeerId4);
  ASSERT_TRUE(update.has_value());
  ASSERT_TRUE(findPrefixInAnnouncements(**update, /*isV4=*/true, prefix));
  ASSERT_TRUE((*update)->attrs()->originatorId().has_value());
  EXPECT_EQ(
      kPeerAddr3.asV4().toLong(), (*update)->attrs()->originatorId().value());
  const std::vector<int64_t> expectedClusterList{
      kLocalClusterAddr1.asV4().toLong()};
  EXPECT_EQ(expectedClusterList, (*update)->attrs()->clusterList().value());
}

} // namespace facebook::bgp
