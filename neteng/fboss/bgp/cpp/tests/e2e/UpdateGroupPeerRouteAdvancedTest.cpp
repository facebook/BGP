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
 * Parameterized E2E tests for BGP Update Group with PEER ROUTES
 * Phase 2: Routes received from peers and redistributed to other peers
 * Coverage: eBGP routes, redistribution, blocking/unblocking scenarios
 *
 * Part 2: Advanced peer route operations (AttrChange, AddWithdrawReAdd)
 *
 * Parameterized by:
 *   - IP version (v4/v6)
 *   - Serialization mode (enableSerializeGroupPdu true/false)
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test parameters for IP version and serialization mode.
 */
struct ProtocolParams {
  std::string protocol; /* "v4" or "v6" */
  std::string name; /* For test naming: e.g., "IPv4_NoSerialization" */
  bool useV6; /* Whether to use IPv6 routes and peers */
  bool enableSerializeGroupPdu;
};

/* IPv4 without serialization */
static const ProtocolParams kIPv4Params = {
    .protocol = "v4",
    .name = "IPv4",
    .useV6 = false,
    .enableSerializeGroupPdu = false,
};

/* IPv4 with serialization */
static const ProtocolParams kIPv4SerializedParams = {
    .protocol = "v4",
    .name = "IPv4_Serialized",
    .useV6 = false,
    .enableSerializeGroupPdu = true,
};

/* IPv6 without serialization */
static const ProtocolParams kIPv6Params = {
    .protocol = "v6",
    .name = "IPv6",
    .useV6 = true,
    .enableSerializeGroupPdu = false,
};

/* IPv6 with serialization */
static const ProtocolParams kIPv6SerializedParams = {
    .protocol = "v6",
    .name = "IPv6_Serialized",
    .useV6 = true,
    .enableSerializeGroupPdu = true,
};

class UpdateGroupPeerRouteTest
    : public E2ETestFixture,
      public ::testing::WithParamInterface<ProtocolParams> {
 protected:
  void setupComponents() {
    createRib();
    createPeerManager(
        true /* enableUpdateGroup */,
        true /* enableEgressBackpressure */,
        GetParam().enableSerializeGroupPdu);
  }

  /* Access test parameters */
  const ProtocolParams& params() const {
    return GetParam();
  }

  /* Get protocol string for addRoute/verifyRoutes */
  std::string protocol() const {
    return params().useV6 ? "ipv6" : "ipv4";
  }

  /* Get test prefix based on protocol */
  std::string testPrefix() const {
    return params().useV6 ? "2001:db8::" : "10.0.0.0";
  }

  int testPrefixLen() const {
    return params().useV6 ? 32 : 8;
  }

  uint8_t testPrefixLenU8() const {
    return static_cast<uint8_t>(params().useV6 ? 32 : 8);
  }

  /* Get nexthop for route advertisement */
  std::string routeNexthop() const {
    return params().useV6 ? "2001:db8:1::1" : "192.168.1.1";
  }

  /* Get expected nexthops for verification */
  std::string expectedNexthopPeer4() const {
    return params().useV6 ? "2401:db00:e011:411:1000::2b" : "127.5.0.3";
  }

  std::string expectedNexthopPeer5() const {
    return params().useV6 ? "2401:db00:e011:411:1000::2d" : "127.5.0.4";
  }
};

/*
 * =================================================================
 * STATIC PEER SPECS - eBGP peers for same update group testing
 * =================================================================
 */

/* Peer 1: Route sender (eBGP peer that originates routes) */
static const BgpPeerSpec kEbgpPeerSpec1 = {
    .asn = kPeerAsn3, /* ASN 4200000010 */
    .localAddr = kLocalAddr1,
    .peerAddr = kPeerAddr3, /* 127.3.0.1 */
    .v4Nexthop = kNextHopV4_3,
    .v6Nexthop = kNextHopV6_3,
    .description = kDescription1,
};

/* Peer 2: Route receiver (eBGP peer in same update group as Peer 1) */
static const BgpPeerSpec kEbgpPeerSpec2 = {
    .asn = kPeerAsn4, /* ASN 64541 (different from Peer 1 = eBGP) */
    .localAddr = kLocalAddr1,
    .peerAddr = kPeerAddr4, /* 127.4.0.1 */
    .v4Nexthop = kNextHopV4_4,
    .v6Nexthop = kNextHopV6_4,
};

/* Peer 3: Another route receiver (same update group) */
static const BgpPeerSpec kEbgpPeerSpec3 = {
    .asn = kPeerAsn5, /* ASN 64542 (different from Peer 1 = eBGP) */
    .localAddr = kLocalAddr1,
    .peerAddr = kPeerAddr5, /* 127.5.0.1 */
    .v4Nexthop = kNextHopV4_5,
    .v6Nexthop = kNextHopV6_5,
};

/*
 * =================================================================
 * TEST: SimplePeerRouteAttrChange
 * =================================================================
 * Goal: Verify peer route attribute change propagation
 * Parameterized: v4/v6 x serialization
 */
TEST_P(UpdateGroupPeerRouteTest, SimplePeerRouteAttrChange) {
  XLOGF(INFO, "=== TEST: SimplePeerRouteAttrChange ({}) ===", params().name);

  addPeer(kEbgpPeerSpec1);
  addPeer(kEbgpPeerSpec2);
  addPeer(kEbgpPeerSpec3);

  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));

  /* Build expected routes for initial verification */
  std::vector<E2ETestFixture::VerifySpec> expectedOriginalPeer4 = {
      {
          .prefix = testPrefix(),
          .prefixLen = testPrefixLenU8(),
          .expectedNexthop = expectedNexthopPeer4(),
          .expectedAsPath = "4200000001 65001",
          .expectedCommunity = "100:1",
      },
  };

  /* Add route with community 100:1 */
  XLOG(INFO, "Peer1 advertising route with community 100:1");
  addRoute(
      protocol(),
      testPrefix(),
      testPrefixLen(),
      kPeerAddr3,
      routeNexthop(),
      "65001",
      "100:1");

  EXPECT_TRUE(verifyRoutes(protocol(), kPeerAddr4, expectedOriginalPeer4));

  /* Change community to 100:2 */
  XLOG(INFO, "Peer1 updating route with community 100:2");
  addRoute(
      protocol(),
      testPrefix(),
      testPrefixLen(),
      kPeerAddr3,
      routeNexthop(),
      "65001",
      "100:2");

  /* Verify attribute change propagated */
  std::vector<E2ETestFixture::VerifySpec> expectedNewCommunityPeer4 = {
      {
          .prefix = testPrefix(),
          .prefixLen = testPrefixLenU8(),
          .expectedNexthop = expectedNexthopPeer4(),
          .expectedAsPath = "4200000001 65001",
          .expectedCommunity = "100:2",
      },
  };
  std::vector<E2ETestFixture::VerifySpec> expectedNewCommunityPeer5 = {
      {
          .prefix = testPrefix(),
          .prefixLen = testPrefixLenU8(),
          .expectedNexthop = expectedNexthopPeer5(),
          .expectedAsPath = "4200000001 65001",
          .expectedCommunity = "100:2",
      },
  };

  EXPECT_TRUE(verifyRoutes(protocol(), kPeerAddr4, expectedNewCommunityPeer4));
  EXPECT_TRUE(verifyRoutes(protocol(), kPeerAddr5, expectedNewCommunityPeer5));

  XLOG(INFO, "=== TEST PASSED: SimplePeerRouteAttrChange ===");
}

/*
 * =================================================================
 * TEST: SimplePeerRouteAddWithdrawReAdd
 * =================================================================
 * Goal: Verify complete route lifecycle - add, withdraw, re-add
 * Parameterized: v4/v6 x serialization
 */
TEST_P(UpdateGroupPeerRouteTest, SimplePeerRouteAddWithdrawReAdd) {
  XLOGF(
      INFO,
      "=== TEST: SimplePeerRouteAddWithdrawReAdd ({}) ===",
      params().name);

  addPeer(kEbgpPeerSpec1);
  addPeer(kEbgpPeerSpec2);
  addPeer(kEbgpPeerSpec3);

  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId5));

  /* Build expected routes for verification */
  std::vector<E2ETestFixture::VerifySpec> expectedPeer4 = {
      {
          .prefix = testPrefix(),
          .prefixLen = testPrefixLenU8(),
          .expectedNexthop = expectedNexthopPeer4(),
          .expectedAsPath = "4200000001 65001",
          .expectedCommunity = "100:1",
      },
  };
  std::vector<E2ETestFixture::VerifySpec> expectedPeer5 = {
      {
          .prefix = testPrefix(),
          .prefixLen = testPrefixLenU8(),
          .expectedNexthop = expectedNexthopPeer5(),
          .expectedAsPath = "4200000001 65001",
          .expectedCommunity = "100:1",
      },
  };

  /* Phase 1: Add route */
  XLOGF(
      INFO,
      "Phase 1: Peer1 advertising route {}/{}",
      testPrefix(),
      testPrefixLen());
  addRoute(
      protocol(),
      testPrefix(),
      testPrefixLen(),
      kPeerAddr3,
      routeNexthop(),
      "65001",
      "100:1");

  EXPECT_TRUE(verifyRoutes(protocol(), kPeerAddr4, expectedPeer4));
  EXPECT_TRUE(verifyRoutes(protocol(), kPeerAddr5, expectedPeer5));

  /* Phase 2: Withdraw route */
  XLOGF(
      INFO,
      "Phase 2: Peer1 withdrawing route {}/{}",
      testPrefix(),
      testPrefixLen());
  deleteRoute(protocol(), testPrefix(), testPrefixLen(), kPeerAddr3);

  std::vector<E2ETestFixture::WithdrawSpec> expectedWithdraws = {
      {.prefix = testPrefix(), .prefixLen = testPrefixLenU8()},
  };

  EXPECT_TRUE(verifyRouteWithdraws(protocol(), kPeerAddr4, expectedWithdraws));
  EXPECT_TRUE(verifyRouteWithdraws(protocol(), kPeerAddr5, expectedWithdraws));

  /* Phase 3: Re-add the same route */
  XLOGF(
      INFO,
      "Phase 3: Peer1 re-advertising route {}/{}",
      testPrefix(),
      testPrefixLen());
  addRoute(
      protocol(),
      testPrefix(),
      testPrefixLen(),
      kPeerAddr3,
      routeNexthop(),
      "65001",
      "100:1");

  EXPECT_TRUE(verifyRoutes(protocol(), kPeerAddr4, expectedPeer4));
  EXPECT_TRUE(verifyRoutes(protocol(), kPeerAddr5, expectedPeer5));

  XLOG(INFO, "=== TEST PASSED: SimplePeerRouteAddWithdrawReAdd ===");
}

/*
 * Instantiate tests for all protocol and serialization combinations.
 */
INSTANTIATE_TEST_SUITE_P(
    ProtocolModes,
    UpdateGroupPeerRouteTest,
    ::testing::Values(
        kIPv4Params,
        kIPv4SerializedParams,
        kIPv6Params,
        kIPv6SerializedParams),
    [](const ::testing::TestParamInfo<ProtocolParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
