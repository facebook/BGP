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

#pragma once

/*
 * Shared utilities for Update Group parameterized E2E tests.
 * Provides:
 *   - ProtocolParams struct for test parameterization
 *   - Pre-defined IPv4/IPv6 test parameters (with/without serialization)
 *   - UpdateGroupTestBase fixture class with common helper methods
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

namespace facebook {
namespace bgp {

/*
 * Test parameters for Update Group tests.
 * Parameterized by IP version and serialization mode.
 */
struct ProtocolParams {
  std::string protocol; /* "v4" or "v6" */
  std::string name; /* For test naming: e.g., "IPv4_NoSerialization" */

  /* Base prefixes for route generation - index corresponds to route number */
  std::vector<std::string> prefixes;

  /* Whether to use v6-capable peer spec */
  bool useV6Peer;

  /* Enable group PDU serialization for zero-copy distribution */
  bool enableSerializeGroupPdu;
};

/* IPv4 without serialization */
inline const ProtocolParams kIPv4Params = {
    .protocol = "v4",
    .name = "IPv4",
    .prefixes =
        {"10.0.0.0/8",
         "20.0.0.0/8",
         "30.0.0.0/8",
         "40.0.0.0/8",
         "50.0.0.0/8",
         "60.0.0.0/8",
         "70.0.0.0/8",
         "80.0.0.0/8",
         "90.0.0.0/8",
         "100.0.0.0/8"},
    .useV6Peer = false,
    .enableSerializeGroupPdu = false,
};

/* IPv4 with serialization */
inline const ProtocolParams kIPv4SerializedParams = {
    .protocol = "v4",
    .name = "IPv4_Serialized",
    .prefixes =
        {"10.0.0.0/8",
         "20.0.0.0/8",
         "30.0.0.0/8",
         "40.0.0.0/8",
         "50.0.0.0/8",
         "60.0.0.0/8",
         "70.0.0.0/8",
         "80.0.0.0/8",
         "90.0.0.0/8",
         "100.0.0.0/8"},
    .useV6Peer = false,
    .enableSerializeGroupPdu = true,
};

/* IPv6 without serialization */
inline const ProtocolParams kIPv6Params = {
    .protocol = "v6",
    .name = "IPv6",
    .prefixes =
        {"2001:db8:1::/48",
         "2001:db8:2::/48",
         "2001:db8:3::/48",
         "2001:db8:4::/48",
         "2001:db8:5::/48",
         "2001:db8:6::/48",
         "2001:db8:7::/48",
         "2001:db8:8::/48",
         "2001:db8:9::/48",
         "2001:db8:10::/48"},
    .useV6Peer = true,
    .enableSerializeGroupPdu = false,
};

/* IPv6 with serialization */
inline const ProtocolParams kIPv6SerializedParams = {
    .protocol = "v6",
    .name = "IPv6_Serialized",
    .prefixes =
        {"2001:db8:1::/48",
         "2001:db8:2::/48",
         "2001:db8:3::/48",
         "2001:db8:4::/48",
         "2001:db8:5::/48",
         "2001:db8:6::/48",
         "2001:db8:7::/48",
         "2001:db8:8::/48",
         "2001:db8:9::/48",
         "2001:db8:10::/48"},
    .useV6Peer = true,
    .enableSerializeGroupPdu = true,
};

/*
 * Base parameterized test fixture for Update Group tests.
 * Provides common setup and helper methods for route verification.
 */
class UpdateGroupTestBase
    : public E2ETestFixture,
      public ::testing::WithParamInterface<ProtocolParams> {
 protected:
  void setupComponents() {
    createRib();
    createPeerManager(
        true /* enableUpdateGroup */, params().enableSerializeGroupPdu);
  }

  /* Access test parameters */
  const ProtocolParams& params() const {
    return GetParam();
  }

  /* Get peer spec based on protocol */
  BgpPeerSpec getPeerSpec3() const {
    return params().useV6Peer ? kDefaultPeerSpec3_v6 : kDefaultPeerSpec3;
  }

  BgpPeerSpec getPeerSpec4() const {
    return params().useV6Peer ? kDefaultPeerSpec4_v6 : kDefaultPeerSpec4;
  }

  /* Get prefix by index (0-based) */
  std::string prefix(size_t idx) const {
    return params().prefixes.at(idx);
  }

  /* Get prefixes as a subset */
  std::vector<std::string> prefixes(size_t start, size_t count) const {
    std::vector<std::string> result;
    for (size_t i = start; i < start + count && i < params().prefixes.size();
         ++i) {
      result.push_back(params().prefixes[i]);
    }
    return result;
  }

  /*
   * Generate VerifySpec from prefix string.
   * Extracts prefix and prefix length from "10.0.0.0/8" format.
   */
  static std::pair<std::string, uint8_t> parsePrefixStr(
      const std::string& prefixStr) {
    auto slashPos = prefixStr.find('/');
    std::string prefix = prefixStr.substr(0, slashPos);
    uint8_t prefixLen =
        static_cast<uint8_t>(std::stoi(prefixStr.substr(slashPos + 1)));
    return {prefix, prefixLen};
  }

  /*
   * Generate route verify specs from prefixes and community base.
   * Communities are generated as "base:1", "base:2", etc.
   */
  std::vector<VerifySpec> makeRouteSpecs(
      const std::vector<std::string>& prefixStrs,
      int communityBase) const {
    std::vector<VerifySpec> specs;
    int idx = 1;
    for (const auto& prefixStr : prefixStrs) {
      auto [prefix, prefixLen] = parsePrefixStr(prefixStr);
      specs.push_back(
          VerifySpec{
              .prefix = prefix,
              .prefixLen = prefixLen,
              .expectedNexthop = "", /* Filled by verifyRoutesWithFix */
              .expectedAsPath = "4200000001",
              .expectedCommunity =
                  std::to_string(communityBase) + ":" + std::to_string(idx++),
          });
    }
    return specs;
  }

  /*
   * Generate route verify spec with custom community suffix.
   */
  std::vector<VerifySpec> makeRouteSpec(
      const std::string& prefixStr,
      const std::string& community) const {
    auto [prefix, prefixLen] = parsePrefixStr(prefixStr);
    return {VerifySpec{
        .prefix = prefix,
        .prefixLen = prefixLen,
        .expectedNexthop = "",
        .expectedAsPath = "4200000001",
        .expectedCommunity = community,
    }};
  }

  /*
   * Generate withdraw specs from prefixes.
   */
  std::vector<WithdrawSpec> makeWithdrawSpecs(
      const std::vector<std::string>& prefixStrs) const {
    std::vector<WithdrawSpec> specs;
    for (const auto& prefixStr : prefixStrs) {
      auto [prefix, prefixLen] = parsePrefixStr(prefixStr);
      specs.push_back(
          WithdrawSpec{
              .prefix = prefix,
              .prefixLen = prefixLen,
          });
    }
    return specs;
  }

  /*
   * Helper to find expected nexthop for peer based on protocol.
   */
  std::string getExpectedNexthop(const folly::IPAddress& peerAddr) const {
    for (const auto& peer : peers_) {
      if (*peer.peer_addr() == peerAddr.str()) {
        if (params().protocol == "v6") {
          return *peer.next_hop6();
        } else {
          return *peer.next_hop4();
        }
      }
    }
    XLOGF(ERR, "getExpectedNexthop: Peer {} not found", peerAddr.str());
    return "127.1.0.1";
  }

  /*
   * Wrapper to automatically inject correct nexthop based on protocol.
   */
  bool verifyRoutesWithFix(
      const folly::IPAddress& peer,
      const std::vector<VerifySpec>& routes) {
    auto routesCopy = routes;
    std::string expectedNh = getExpectedNexthop(peer);
    for (auto& route : routesCopy) {
      route.expectedNexthop = expectedNh;
    }
    return E2ETestFixture::verifyRoutes(params().protocol, peer, routesCopy);
  }

  /*
   * Inject routes at runtime with communities based on communityBase.
   */
  void injectRoutes(
      const std::vector<std::string>& prefixStrs,
      int communityBase,
      int localPref = 150) {
    int idx = 1;
    for (const auto& prefixStr : prefixStrs) {
      std::string community =
          std::to_string(communityBase) + ":" + std::to_string(idx++);
      injectLocalRoutesAtRuntime({prefixStr}, {community}, localPref);
      ASSERT_TRUE(
          waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefixStr)));
    }
  }

  /*
   * Withdraw routes at runtime.
   */
  void withdrawRoutes(const std::vector<std::string>& prefixStrs) {
    withdrawLocalRoutesAtRuntime(prefixStrs);
  }

  /*
   * Verify route withdrawals.
   */
  bool verifyWithdraws(
      const folly::IPAddress& peer,
      const std::vector<std::string>& prefixStrs) {
    return verifyRouteWithdraws(
        params().protocol, peer, makeWithdrawSpecs(prefixStrs));
  }
};

/*
 * Helper macro to instantiate tests for IPv4/IPv6 with and without
 * group PDU serialization. Creates 4 test variants:
 *   - IPv4: v4 without serialization
 *   - IPv4_Serialized: v4 with enableSerializeGroupPdu=true
 *   - IPv6: v6 without serialization
 *   - IPv6_Serialized: v6 with enableSerializeGroupPdu=true
 */
#define INSTANTIATE_UPDATE_GROUP_TESTS(TestClass)                \
  INSTANTIATE_TEST_SUITE_P(                                      \
      IpVersions,                                                \
      TestClass,                                                 \
      ::testing::Values(                                         \
          kIPv4Params,                                           \
          kIPv4SerializedParams,                                 \
          kIPv6Params,                                           \
          kIPv6SerializedParams),                                \
      [](const ::testing::TestParamInfo<ProtocolParams>& info) { \
        return info.param.name;                                  \
      })

} // namespace bgp
} // namespace facebook
