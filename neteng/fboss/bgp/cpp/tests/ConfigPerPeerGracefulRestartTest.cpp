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
#include <memory>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "folly/IPAddress.h"
#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"

using namespace facebook::bgp;
using namespace facebook::bgp::thrift;

class PerPeerGracefulRestartTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a basic BGP config
    config_.router_id() = "10.0.0.1";
    config_.local_as_4_byte() = 65001;
    config_.hold_time() = 30;
    config_.listen_addr() = "::";
    config_.listen_port() = 179;

    // Set global graceful restart time to 120 seconds
    config_.graceful_restart_convergence_seconds() = 120;
  }

  BgpConfig config_;
};

TEST_F(PerPeerGracefulRestartTest, TestGlobalGracefulRestartOnly) {
  // Add a peer without specific graceful restart configuration
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";

  config_.peers() = std::vector<BgpPeer>{peer};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Get peering params for the peer
  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  // Should use global graceful restart time (120 seconds)
  ASSERT_TRUE(peeringParams.grRestartTime.has_value());
  EXPECT_EQ(peeringParams.grRestartTime->count(), 120);
}

TEST_F(PerPeerGracefulRestartTest, TestPerPeerGracefulRestartOverride) {
  // Add a peer with specific graceful restart configuration
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";

  // Set per-peer graceful restart time to 60 seconds
  BgpPeerTimers timers;
  timers.hold_time_seconds() = 30;
  timers.keep_alive_seconds() = 10;
  timers.out_delay_seconds() = 0;
  timers.graceful_restart_seconds() = 60; // Per-peer override
  peer.bgp_peer_timers() = timers;

  config_.peers() = std::vector<BgpPeer>{peer};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Get peering params for the peer
  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  // Should use per-peer graceful restart time (60 seconds)
  ASSERT_TRUE(peeringParams.grRestartTime.has_value());
  EXPECT_EQ(peeringParams.grRestartTime->count(), 60);
}

TEST_F(PerPeerGracefulRestartTest, TestPerGroupGracefulRestartOverride) {
  // Create a peer group with graceful restart configuration
  PeerGroup peerGroup;
  peerGroup.name() = "test_group";

  BgpPeerTimers groupTimers;
  groupTimers.hold_time_seconds() = 30;
  groupTimers.keep_alive_seconds() = 10;
  groupTimers.out_delay_seconds() = 0;
  groupTimers.graceful_restart_seconds() = 90; // Per-group override
  peerGroup.bgp_peer_timers() = groupTimers;

  config_.peer_groups() = std::vector<PeerGroup>{peerGroup};

  // Add a peer that uses the peer group
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";
  peer.peer_group_name() = "test_group";

  config_.peers() = std::vector<BgpPeer>{peer};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Get peering params for the peer
  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  // Should use per-group graceful restart time (90 seconds)
  ASSERT_TRUE(peeringParams.grRestartTime.has_value());
  EXPECT_EQ(peeringParams.grRestartTime->count(), 90);
}

TEST_F(PerPeerGracefulRestartTest, TestPerPeerOverridesPerGroup) {
  // Create a peer group with graceful restart configuration
  PeerGroup peerGroup;
  peerGroup.name() = "test_group";

  BgpPeerTimers groupTimers;
  groupTimers.hold_time_seconds() = 30;
  groupTimers.keep_alive_seconds() = 10;
  groupTimers.out_delay_seconds() = 0;
  groupTimers.graceful_restart_seconds() = 90; // Per-group setting
  peerGroup.bgp_peer_timers() = groupTimers;

  config_.peer_groups() = std::vector<PeerGroup>{peerGroup};

  // Add a peer that uses the peer group but overrides the graceful restart
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";
  peer.peer_group_name() = "test_group";

  // Override per-peer graceful restart time to 30 seconds
  BgpPeerTimers peerTimers;
  peerTimers.hold_time_seconds() = 30;
  peerTimers.keep_alive_seconds() = 10;
  peerTimers.out_delay_seconds() = 0;
  peerTimers.graceful_restart_seconds() = 30; // Per-peer override
  peer.bgp_peer_timers() = peerTimers;

  config_.peers() = std::vector<BgpPeer>{peer};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Get peering params for the peer
  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  /*
   * Should use per-peer graceful restart time (30 seconds), overriding
   * per-group
   */
  ASSERT_TRUE(peeringParams.grRestartTime.has_value());
  EXPECT_EQ(peeringParams.grRestartTime->count(), 30);
}

TEST_F(PerPeerGracefulRestartTest, TestDisableGracefulRestartPerPeer) {
  // Add a peer with graceful restart disabled (0 seconds)
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";

  // Set per-peer graceful restart time to 0 (disabled)
  BgpPeerTimers timers;
  timers.hold_time_seconds() = 30;
  timers.keep_alive_seconds() = 10;
  timers.out_delay_seconds() = 0;
  timers.graceful_restart_seconds() = 0; // Disable graceful restart
  peer.bgp_peer_timers() = timers;

  config_.peers() = std::vector<BgpPeer>{peer};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Get peering params for the peer
  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  // Should use per-peer graceful restart time (0 seconds - disabled)
  ASSERT_TRUE(peeringParams.grRestartTime.has_value());
  EXPECT_EQ(peeringParams.grRestartTime->count(), 0);
}

TEST_F(
    PerPeerGracefulRestartTest,
    TestBackwardCompatibilityNoGracefulRestartField) {
  /*
   * Test backward compatibility: peer with no graceful_restart_seconds field at
   * all
   */
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";

  // Create BGP timers WITHOUT graceful_restart_seconds field
  BgpPeerTimers timers;
  timers.hold_time_seconds() = 30;
  timers.keep_alive_seconds() = 10;
  timers.out_delay_seconds() = 0;
  /*
   * Note: NOT setting graceful_restart_seconds - this tests backward
   * compatibility
   */
  peer.bgp_peer_timers() = timers;

  config_.peers() = std::vector<BgpPeer>{peer};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Get peering params for the peer
  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  // Should fall back to global graceful restart time (120 seconds)
  ASSERT_TRUE(peeringParams.grRestartTime.has_value());
  EXPECT_EQ(peeringParams.grRestartTime->count(), 120);
}

TEST_F(PerPeerGracefulRestartTest, TestPeerWithoutTimersConfig) {
  // Test null safety: peer with no bgp_peer_timers at all
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";
  // Note: NOT setting bgp_peer_timers at all

  config_.peers() = std::vector<BgpPeer>{peer};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Get peering params for the peer
  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  // Should fall back to global graceful restart time (120 seconds)
  ASSERT_TRUE(peeringParams.grRestartTime.has_value());
  EXPECT_EQ(peeringParams.grRestartTime->count(), 120);
}

TEST_F(PerPeerGracefulRestartTest, TestInvalidPeerGroupReference) {
  // Test error handling: peer references non-existent peer group
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";
  peer.peer_group_name() = "non_existent_group"; // Reference invalid group

  config_.peers() = std::vector<BgpPeer>{peer};

  // Creating Config object should throw an exception for invalid peer group
  EXPECT_THROW(
      { auto bgpConfig = std::make_shared<Config>(std::move(config_)); },
      facebook::bgp::BgpError);
}

TEST_F(PerPeerGracefulRestartTest, TestMixedConfiguration) {
  // Test mixed configuration: some peers with graceful restart, some without

  // Create peer group with graceful restart
  PeerGroup peerGroup;
  peerGroup.name() = "mixed_group";
  BgpPeerTimers groupTimers;
  groupTimers.hold_time_seconds() = 30;
  groupTimers.keep_alive_seconds() = 10;
  groupTimers.out_delay_seconds() = 0;
  groupTimers.graceful_restart_seconds() = 90;
  peerGroup.bgp_peer_timers() = groupTimers;
  config_.peer_groups() = std::vector<PeerGroup>{peerGroup};

  // Peer 1: Uses peer group (90 seconds)
  BgpPeer peer1;
  peer1.peer_addr() = "10.0.0.2";
  peer1.remote_as_4_byte() = 65002;
  peer1.local_addr() = "";
  peer1.next_hop4() = "10.0.0.1";
  peer1.next_hop6() = "::1";
  peer1.peer_group_name() = "mixed_group";

  // Peer 2: Per-peer override (60 seconds)
  BgpPeer peer2;
  peer2.peer_addr() = "10.0.0.3";
  peer2.remote_as_4_byte() = 65002;
  peer2.local_addr() = "";
  peer2.next_hop4() = "10.0.0.1";
  peer2.next_hop6() = "::1";
  BgpPeerTimers peer2Timers;
  peer2Timers.hold_time_seconds() = 30;
  peer2Timers.keep_alive_seconds() = 10;
  peer2Timers.out_delay_seconds() = 0;
  peer2Timers.graceful_restart_seconds() = 60;
  peer2.bgp_peer_timers() = peer2Timers;

  // Peer 3: No graceful restart config (falls back to global 120 seconds)
  BgpPeer peer3;
  peer3.peer_addr() = "10.0.0.4";
  peer3.remote_as_4_byte() = 65002;
  peer3.local_addr() = "";
  peer3.next_hop4() = "10.0.0.1";
  peer3.next_hop6() = "::1";

  config_.peers() = std::vector<BgpPeer>{peer1, peer2, peer3};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Verify peer 1 (peer group)
  auto peer1Addr = folly::IPAddress("10.0.0.2");
  auto peer1Config = bgpConfig->getPeerToConfig().find(peer1Addr);
  ASSERT_NE(peer1Config, bgpConfig->getPeerToConfig().end());
  auto peer1Params = bgpConfig->getPeeringParamsForPeer(*peer1Config->second);
  EXPECT_EQ(peer1Params.grRestartTime->count(), 90);

  // Verify peer 2 (per-peer override)
  auto peer2Addr = folly::IPAddress("10.0.0.3");
  auto peer2Config = bgpConfig->getPeerToConfig().find(peer2Addr);
  ASSERT_NE(peer2Config, bgpConfig->getPeerToConfig().end());
  auto peer2Params = bgpConfig->getPeeringParamsForPeer(*peer2Config->second);
  EXPECT_EQ(peer2Params.grRestartTime->count(), 60);

  // Verify peer 3 (global fallback)
  auto peer3Addr = folly::IPAddress("10.0.0.4");
  auto peer3Config = bgpConfig->getPeerToConfig().find(peer3Addr);
  ASSERT_NE(peer3Config, bgpConfig->getPeerToConfig().end());
  auto peer3Params = bgpConfig->getPeeringParamsForPeer(*peer3Config->second);
  EXPECT_EQ(peer3Params.grRestartTime->count(), 120);
}

TEST_F(PerPeerGracefulRestartTest, TestGlobalDisabledWithPerPeerEnabled) {
  /*
   * Test scenario: Global graceful restart disabled, but specific peers enable
   * it
   */
  config_.graceful_restart_convergence_seconds() = 0; // Disable globally

  // Peer with graceful restart enabled despite global disable
  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";

  BgpPeerTimers timers;
  timers.hold_time_seconds() = 30;
  timers.keep_alive_seconds() = 10;
  timers.out_delay_seconds() = 0;
  timers.graceful_restart_seconds() = 60; // Enable for this peer
  peer.bgp_peer_timers() = timers;

  config_.peers() = std::vector<BgpPeer>{peer};

  // Create Config object
  auto bgpConfig = std::make_shared<Config>(std::move(config_));

  // Get peering params for the peer
  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  // Should use per-peer graceful restart time (60 seconds), not global (0)
  ASSERT_TRUE(peeringParams.grRestartTime.has_value());
  EXPECT_EQ(peeringParams.grRestartTime->count(), 60);
}

TEST_F(PerPeerGracefulRestartTest, TestNoGRHelperWhenGlobalUnconfigured) {
  /*
   * When graceful_restart_convergence_seconds is not set globally and no
   * per-peer override exists, grRestartTime should be nullopt so that GR
   * capability is not advertised (no GR helper behavior).
   */
  BgpConfig config;
  config.router_id() = "10.0.0.1";
  config.local_as_4_byte() = 65001;
  config.hold_time() = 30;
  config.listen_addr() = "::";
  config.listen_port() = 179;
  // NOTE: graceful_restart_convergence_seconds is NOT set

  BgpPeer peer;
  peer.peer_addr() = "10.0.0.2";
  peer.remote_as_4_byte() = 65002;
  peer.local_addr() = "";
  peer.next_hop4() = "10.0.0.1";
  peer.next_hop6() = "::1";

  config.peers() = std::vector<BgpPeer>{peer};

  auto bgpConfig = std::make_shared<Config>(std::move(config));

  auto peerAddr = folly::IPAddress("10.0.0.2");
  auto peerConfigIter = bgpConfig->getPeerToConfig().find(peerAddr);
  ASSERT_NE(peerConfigIter, bgpConfig->getPeerToConfig().end());

  auto peeringParams =
      bgpConfig->getPeeringParamsForPeer(*peerConfigIter->second);

  // grRestartTime should be nullopt - no GR capability will be advertised
  EXPECT_FALSE(peeringParams.grRestartTime.has_value());
}
