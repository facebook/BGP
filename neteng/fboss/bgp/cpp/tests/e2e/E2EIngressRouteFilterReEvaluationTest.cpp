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
 * E2E tests for CRF ingress route filter changes on routes that are already
 * learned. Platform-neutral: the same suite runs against RibBB and RibDC
 * through sibling Buck targets.
 *
 * The CRF policy is set at runtime on the peer manager after routes are
 * learned, and the effect is checked in the RIB and on the wire towards
 * another peer.
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/rib/RibPolicy.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

namespace facebook::bgp {

namespace {

/* Matches the description of kDefaultPeerSpec3 only. */
constexpr auto kPeer3StatementRegex = R"(rsw001\.p001\.f01\.bgp1)";

const auto kPrefix10 = folly::IPAddress::createNetwork("10.0.0.0/8");
const auto kPrefix20 = folly::IPAddress::createNetwork("20.0.0.0/8");
const auto kPrefix30 = folly::IPAddress::createNetwork("30.0.0.0/8");
const auto kPrefix40 = folly::IPAddress::createNetwork("40.0.0.0/8");

/* A blocking ingress filter on peer3 that allows only allowedPrefixes. */
rib_policy::TRouteFilterPolicy makePeer3IngressPolicy(
    const std::vector<folly::CIDRNetwork>& allowedPrefixes,
    int64_t version) {
  rib_policy::TRouteFilterPolicy policy;
  policy.statements()->emplace(
      kPeer3StatementRegex,
      createTRouteFilterStatement(
          allowedPrefixes, /*permissive=*/false, /*egress=*/false));
  policy.version() = version;
  return policy;
}

} // namespace

class E2EIngressRouteFilterReEvaluationTest : public E2ERibTestFixture {
 protected:
  void SetUp() override {}

  void createTopology(bool dynamicPolicyEvaluation) {
    enableDynamicPolicyEvaluation(dynamicPolicyEvaluation);
    E2ERibTestFixture::SetUp();
    bringUpAllPeersWithEor();
  }

  /* Announce prefixes from peer3 and wait until peer5 is sent each one. */
  void learnFromPeer3(const std::vector<folly::CIDRNetwork>& prefixes) {
    for (const auto& prefix : prefixes) {
      addRoute(
          "v4",
          prefix.first.str(),
          prefix.second,
          kPeerAddr3,
          "11.0.0.1",
          "65001");
      ASSERT_TRUE(waitForRouteInShadowRib(prefix));
      ASSERT_TRUE(verifyRouteAdd(
          "v4",
          prefix.first.str(),
          prefix.second,
          kPeerAddr5,
          kNextHopV4_5.str()));
    }
  }

  /*
   * setRouteFilterPolicy only schedules the apply on the peer manager thread,
   * so wait for that thread before checking the effect.
   */
  void setRouteFilterPolicy(const rib_policy::TRouteFilterPolicy& policy) {
    peerManager_->setRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(policy));
    peerManager_->getEventBase().runInEventBaseThreadAndWait([] {});
  }
};

/*
 * With dynamic policy evaluation on, a new ingress filter withdraws a route
 * that was already learned and is no longer allowed. An allowed route stays.
 */
TEST_F(
    E2EIngressRouteFilterReEvaluationTest,
    IngressFilterWithdrawsLearnedRouteWhenEnabled) {
  createTopology(/*dynamicPolicyEvaluation=*/true);
  learnFromPeer3({kPrefix10, kPrefix20});

  setRouteFilterPolicy(makePeer3IngressPolicy({kPrefix20}, /*version=*/1));

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));
  EXPECT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));
  EXPECT_TRUE(waitForRouteInShadowRib(kPrefix20));
}

/*
 * With dynamic policy evaluation on, removing the ingress filter brings back a
 * route that the filter had denied, without the peer re-sending it.
 */
TEST_F(
    E2EIngressRouteFilterReEvaluationTest,
    RemovingIngressFilterRestoresDeniedRouteWhenEnabled) {
  createTopology(/*dynamicPolicyEvaluation=*/true);
  learnFromPeer3({kPrefix10, kPrefix20});
  setRouteFilterPolicy(makePeer3IngressPolicy({kPrefix20}, /*version=*/1));
  ASSERT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr5));

  rib_policy::TRouteFilterPolicy noFilterPolicy;
  noFilterPolicy.version() = 2;
  setRouteFilterPolicy(noFilterPolicy);

  EXPECT_TRUE(waitForRouteInShadowRib(kPrefix10));
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.0.0", 8, kPeerAddr5, kNextHopV4_5.str()));
}

/*
 * With dynamic policy evaluation off, a new ingress filter applies only to
 * routes received after the change. A route learned before the change stays.
 *
 * 30/8 (denied) is sent before 40/8 (allowed) on the same session, so once
 * 40/8 is in the shadow RIB, 30/8 has been processed too.
 */
TEST_F(
    E2EIngressRouteFilterReEvaluationTest,
    IngressFilterKeepsLearnedRouteWhenDisabled) {
  createTopology(/*dynamicPolicyEvaluation=*/false);
  learnFromPeer3({kPrefix10, kPrefix20});

  setRouteFilterPolicy(
      makePeer3IngressPolicy({kPrefix20, kPrefix40}, /*version=*/1));
  addRoute("v4", "30.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  addRoute("v4", "40.0.0.0", 8, kPeerAddr3, "11.0.0.1", "65001");
  ASSERT_TRUE(waitForRouteInShadowRib(kPrefix40));

  EXPECT_TRUE(verifyRouteNotInShadowRib(kPrefix30));
  EXPECT_TRUE(waitForRouteInShadowRib(kPrefix10));
}

} // namespace facebook::bgp
