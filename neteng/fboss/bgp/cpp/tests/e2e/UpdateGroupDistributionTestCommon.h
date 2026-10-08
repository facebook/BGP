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
 * Common definitions for Update Group Distribution tests.
 * Shared by InitialDump, RuntimeRoute, PeerLifecycle, and Blocking tests.
 */

#include <gtest/gtest.h>

#include <unordered_map>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

namespace facebook {
namespace bgp {

/*
 * Additional peer constants for tests needing 10+ peers
 */
inline const auto kLocalAddr10 = folly::IPAddress("127.1.0.9");
inline const auto kPeerAddr10 = folly::IPAddress("127.10.0.1");
inline const uint32_t kPeerAsn10 = 64547;
inline const auto kNextHopV4_10 = folly::IPAddress("127.5.0.9");

inline const auto kLocalAddr11 = folly::IPAddress("127.1.0.10");
inline const auto kPeerAddr11 = folly::IPAddress("127.11.0.1");
inline const uint32_t kPeerAsn11 = 64548;
inline const auto kNextHopV4_11 = folly::IPAddress("127.5.0.10");

inline const auto kLocalAddr12 = folly::IPAddress("127.1.0.11");
inline const auto kPeerAddr12 = folly::IPAddress("127.12.0.1");
inline const uint32_t kPeerAsn12 = 64549;
inline const auto kNextHopV4_12 = folly::IPAddress("127.5.0.11");

/*
 * eBGP peer constants for testing different update groups
 */
inline const uint32_t kEbgpAsn1 = 65001;

/*
 * Test parameters for serialization mode
 */
struct SerializationParams {
  std::string name;
  bool enableSerializeGroupPdu;
};

inline const SerializationParams kNoSerialization = {
    "NoSerialization", // name
    false // enableSerializeGroupPdu
};

inline const SerializationParams kWithSerialization = {
    "Serialized", // name
    true // enableSerializeGroupPdu
};

/*
 * Helper function to get the expected nexthop for a given peer address
 */
inline std::string getExpectedNexthop(const folly::IPAddress& peerAddr) {
  static const std::unordered_map<folly::IPAddress, folly::IPAddress>
      kPeerToNexthopV4 = {
          {kPeerAddr3, folly::IPAddress("127.5.0.1")},
          {kPeerAddr4, folly::IPAddress("127.5.0.3")},
          {kPeerAddr5, folly::IPAddress("127.5.0.4")},
          {kPeerAddr6, folly::IPAddress("127.5.0.5")},
          {kPeerAddr7, folly::IPAddress("127.5.0.6")},
          {kPeerAddr8, folly::IPAddress("127.5.0.7")},
          {kPeerAddr9, folly::IPAddress("127.5.0.8")},
          {kPeerAddr10, folly::IPAddress("127.5.0.9")},
          {kPeerAddr11, folly::IPAddress("127.5.0.10")},
          {kPeerAddr12, folly::IPAddress("127.5.0.11")},
      };

  auto it = kPeerToNexthopV4.find(peerAddr);
  if (it != kPeerToNexthopV4.end()) {
    return it->second.str();
  }
  XLOGF(
      ERR, "getExpectedNexthop: No nexthop found for peer {}", peerAddr.str());
  return "";
}

/*
 * Base test fixture for Update Group Distribution tests.
 * Parameterized by serialization mode.
 */
class UpdateGroupDistributionTestBase
    : public E2ETestFixture,
      public ::testing::WithParamInterface<SerializationParams> {
 protected:
  void setupComponents() {
    createRib();
    createPeerManager(
        true /* enableUpdateGroup */, GetParam().enableSerializeGroupPdu);
  }
};

/*
 * Test fixture for Update Group blocking tests.
 */
class UpdateGroupBlockingTest : public UpdateGroupDistributionTestBase {};

/*
 * Test fixture for Update Group initial dump tests.
 */
class UpdateGroupInitialDumpTest : public UpdateGroupDistributionTestBase {};

/*
 * Test fixture for Update Group peer lifecycle tests.
 */
class UpdateGroupPeerLifecycleTest : public UpdateGroupDistributionTestBase {};

/*
 * Test fixture for Update Group runtime route tests.
 */
class UpdateGroupRuntimeRouteTest : public UpdateGroupDistributionTestBase {};

/*
 * Test fixture for Update Group peer join synchronization tests.
 */
class UpdateGroupPeerJoinSyncTest : public UpdateGroupDistributionTestBase {};

} // namespace bgp
} // namespace facebook
