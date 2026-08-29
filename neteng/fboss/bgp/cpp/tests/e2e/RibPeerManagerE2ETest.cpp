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
 * E2E test for RIB + PeerManagerBase: Tests complete BGP flow end-to-end
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManagerBase, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

class RibPeerManagerE2ETest : public E2ETestFixture {
 protected:
  void setupComponents(
      bool enableUpdateGroup = false,
      bool enableEgressBackpressure = true) {
    /* Add default peers to the configuration */
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);

    createRib();
    createPeerManager(enableUpdateGroup, enableEgressBackpressure);
  }

  void setupPeers(const std::vector<folly::IPAddress>& peers) {
    for (const auto& peer : peers) {
      bringUpPeer(peer);
    }
  }

  void sendEoRToAll(const std::vector<folly::IPAddress>& peers) {
    for (const auto& peer : peers) {
      BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
      sendEoRToPeer(peerId);
    }
  }
};

/*
 * Simple BGP test - no update groups, just basic initial dump with local route
 */
TEST_F(RibPeerManagerE2ETest, SimpleBgpInitialDumpNoUpdateGroup) {
  /* Add local route before creating components */
  addLocalRoute("10.0.0.0/8", {"100:1", "100:2"}, 100);

  /* Setup without update groups */
  setupComponents(
      false /* enableUpdateGroup */, true /* enableEgressBackpressure */);

  /* Bring up both peers (both are configured in setupComponents) */
  setupPeers({kPeerAddr3, kPeerAddr4});
  sendEoRToAll({kPeerAddr3, kPeerAddr4});

  /* Peer4 should receive the local route from initial dump */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      "127.5.0.3",
      "4200000001",
      "100:1 100:2"));
}

} // namespace bgp
} // namespace facebook
