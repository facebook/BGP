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

/**
 * E2E tests for nexthop-self policy override behavior.
 *
 * Validates that when an egress policy explicitly sets the nexthop via
 * SetNexthop action, the policy-set nexthop takes precedence over
 * nexthop-self (which normally rewrites the nexthop for eBGP peers or
 * when next_hop_self is configured).
 *
 * Test scenarios:
 *   1. eBGP peer with SetNexthop egress policy → policy nexthop preserved
 *   2. eBGP peer without SetNexthop policy → nexthop-self applied (baseline)
 *   3. iBGP peer with nextHopSelf + SetNexthop policy → policy nexthop
 * preserved
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib, PolicyManager
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

static const std::string kNhOverridePolicyName = "nexthop-override-policy";
static const std::string kTestPrefix = "10.1.0.0";
static constexpr uint8_t kTestPrefixLen = 24;
static const std::string kPolicyNexthop = "99.99.99.99";
static const std::string kTestAsPath = "65001";

class E2ENexthopSelfPolicyOverrideTest : public E2ETestFixture {
 protected:
  /**
   * Create an egress policy with a SetNexthop action that sets
   * the nexthop to kPolicyNexthop for all routes.
   */
  void setupSetNexthopPolicy() {
    auto nexthopAction =
        createBgpPolicyNexthopAction(folly::IPAddress(kPolicyNexthop));

    auto term = createBgpPolicyTerm(
        "set-nexthop-term",
        "Set nexthop to policy value",
        {} /* matches all */,
        {nexthopAction},
        bgp_policy::FlowControlAction::NEXT_TERM);

    auto statement = createBgpPolicyStatement(kNhOverridePolicyName, {term});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(statement);
    setPolicyConfig(policies);
  }

  /**
   * Create an accept-all egress policy (no SetNexthop action).
   */
  void setupAcceptAllPolicy() {
    auto term = createBgpPolicyTerm(
        "accept-all",
        "Accept all routes without modifying nexthop",
        {} /* matches all */,
        {} /* no actions */,
        bgp_policy::FlowControlAction::NEXT_TERM);

    auto statement = createBgpPolicyStatement(kNhOverridePolicyName, {term});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(statement);
    setPolicyConfig(policies);
  }

  /**
   * Setup peers, create RIB + PeerManagerBase, bring up peers, complete EoR.
   */
  void setupAndBringUpPeers(
      const BgpPeerSpec& sourceSpec,
      const BgpPeerSpec& destSpec) {
    addPeer(sourceSpec);
    addPeer(destSpec);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
    bringUpPeer(sourceSpec.peerAddr);
    bringUpPeer(destSpec.peerAddr);

    BgpPeerId srcId{
        sourceSpec.peerAddr, sourceSpec.peerAddr.asV4().toLongHBO()};
    BgpPeerId dstId{destSpec.peerAddr, destSpec.peerAddr.asV4().toLongHBO()};
    sendEoRToPeer(srcId);
    sendEoRToPeer(dstId);
    ASSERT_TRUE(waitForEoR(srcId));
    ASSERT_TRUE(waitForEoR(dstId));
  }

  /**
   * Inject a test route from the source peer.
   */
  void injectRoute(const folly::IPAddress& sourcePeerAddr) {
    addRoute(
        "v4",
        kTestPrefix,
        kTestPrefixLen,
        sourcePeerAddr,
        "11.0.0.1" /* original nexthop from source peer */,
        kTestAsPath);

    auto prefix = folly::IPAddress::createNetwork(
        fmt::format("{}/{}", kTestPrefix, kTestPrefixLen));
    ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  }

  /* eBGP source peer (different ASN, injects routes) */
  BgpPeerSpec makeEbgpSource() {
    return BgpPeerSpec{
        .asn = kPeerAsn3,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr3,
        .v4Nexthop = kNextHopV4_3,
        .v6Nexthop = kNextHopV6_3,
    };
  }

  /* eBGP dest peer with egress SetNexthop policy */
  BgpPeerSpec makeEbgpDestWithPolicy() {
    return BgpPeerSpec{
        .asn = kPeerAsn5,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
        .egressPolicyName = kNhOverridePolicyName,
    };
  }

  /* eBGP dest peer with accept-all policy (no SetNexthop) */
  BgpPeerSpec makeEbgpDestWithAcceptAll() {
    return BgpPeerSpec{
        .asn = kPeerAsn5,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
        .egressPolicyName = kNhOverridePolicyName,
    };
  }

  /* iBGP source peer (same ASN as local) */
  BgpPeerSpec makeIbgpSource() {
    return BgpPeerSpec{
        .asn = kAsn1,
        .localAddr = kLocalAddr1,
        .peerAddr = kPeerAddr3,
        .v4Nexthop = kNextHopV4_3,
        .v6Nexthop = kNextHopV6_3,
    };
  }

  /* iBGP dest peer with egress SetNexthop policy */
  BgpPeerSpec makeIbgpDestWithPolicy() {
    return BgpPeerSpec{
        .asn = kAsn1,
        .localAddr = kLocalAddr5,
        .peerAddr = kPeerAddr5,
        .v4Nexthop = kNextHopV4_5,
        .v6Nexthop = kNextHopV6_5,
        .egressPolicyName = kNhOverridePolicyName,
    };
  }
};

/**
 * eBGP peer with SetNexthop egress policy.
 *
 * Without the fix: nexthop-self would override the policy-set nexthop
 * to the local peering address (kNextHopV4_5).
 *
 * With the fix: the policy-set nexthop (kPolicyNexthop) is preserved.
 */
TEST_F(
    E2ENexthopSelfPolicyOverrideTest,
    eBgpPeer_SetNexthopPolicy_PreservesPolicy) {
  setupSetNexthopPolicy();
  setupAndBringUpPeers(makeEbgpSource(), makeEbgpDestWithPolicy());
  injectRoute(kPeerAddr3);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      kTestPrefix,
      kTestPrefixLen,
      kPeerAddr5,
      kPolicyNexthop /* expect policy nexthop, NOT nexthop-self */,
      "" /* asPath */,
      "" /* community */));
}

/**
 * eBGP peer with accept-all egress policy (no SetNexthop).
 *
 * Without SetNexthop action, nexthop-self should be applied normally.
 * The nexthop should be the local peering address (kNextHopV4_5).
 */
TEST_F(
    E2ENexthopSelfPolicyOverrideTest,
    eBgpPeer_AcceptAllPolicy_NexthopSelfApplied) {
  setupAcceptAllPolicy();
  setupAndBringUpPeers(makeEbgpSource(), makeEbgpDestWithAcceptAll());
  injectRoute(kPeerAddr3);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      kTestPrefix,
      kTestPrefixLen,
      kPeerAddr5,
      kNextHopV4_5.str() /* expect nexthop-self (local address) */,
      "" /* asPath */,
      "" /* community */));
}

} // namespace facebook::bgp
