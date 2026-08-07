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
 * E2E tests for legacy IPv4-unicast NLRI encoding toward capability-less peers.
 *
 * The behavior is gated by the enable_legacy_v4_nlri_encoding thrift config
 * knob (BgpSettingConfig, default off), plumbed Config -> BgpGlobalConfig ->
 * AdjRib. A capability-less peer advertised no MP-EXT capability
 * (BgpPeerSpec::mpExtCapable = false):
 *   - config true        -> classic v4 NLRI + NEXT_HOP (attr 3, RFC 4271):
 *                           v4Announced2() populated, mpAnnounced() empty
 *   - config unset/false -> MP_REACH_NLRI (attr 14), same as an MP peer:
 *                           mpAnnounced() populated with afi=AFI_IPv4
 * An MP-capable peer always receives MP_REACH regardless of the config, and a
 * v4 withdrawal is always classic (the gate is announcement-only).
 *
 * Wire encoding is asserted on the raw BgpUpdate2 drained off each peer's
 * egress queue, because verifyRouteAdd() accepts either encoding and cannot
 * prove which one was used.
 */

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"

using namespace facebook::bgp;

namespace facebook::bgp {

namespace {
using nettools::bgplib::BgpUpdateAfi;
} // namespace

/*
 * Fixture: peer3 is capability-less (no MP-EXT), peer4 is a normal MP peer, and
 * peer5 is an MP source used to originate the prefix that is then advertised to
 * both peer3 and peer4. Observing the SAME prefix on both receivers isolates
 * the per-peer encoding decision.
 *
 * SetUp() only registers peers; each test calls buildRib() to construct the
 * RIB/peer-manager, choosing the thrift config gate state per test.
 */
class E2ELegacyV4NlriEncodingTest : public E2ESessionTestFixture {
 protected:
  void SetUp() override {
    BgpPeerSpec legacyPeer = kDefaultPeerSpec3;
    legacyPeer.mpExtCapable = false; /* capability-less receiver */
    addPeer(legacyPeer);

    addPeer(kDefaultPeerSpec4); /* MP receiver */
    addPeer(kDefaultPeerSpec5); /* MP source */
  }

  /*
   * Build the RIB and peer manager with the legacy v4 NLRI encoding thrift
   * config gate on or off. When legacyEncoding is false the config field is
   * left unset, exercising the default-off ("not present") path that mirrors
   * the removed gflag's default.
   */
  void buildRib(bool legacyEncoding) {
    if (legacyEncoding) {
      enableLegacyV4NlriEncoding(true);
    }
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false,
        /*enableEgressBackpressure=*/false);
  }

  void bringUpAllPeersWithEoR() {
    bringUpPeerAndWait(kPeerAddr3);
    bringUpPeerAndWait(kPeerAddr4);
    bringUpPeerAndWait(kPeerAddr5);
    sendEoRToPeer(peerId3_);
    sendEoRToPeer(peerId4_);
    sendEoRToPeer(peerId5_);
    /* EoR is a precondition for the wire-encoding assertions below. */
    ASSERT_TRUE(waitForEoR(peerId3_));
    ASSERT_TRUE(waitForEoR(peerId4_));
    ASSERT_TRUE(waitForEoR(peerId5_));
  }

  const BgpPeerId peerId3_{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  const BgpPeerId peerId4_{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  const BgpPeerId peerId5_{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
};

/*
 * Feature enabled (config true): the capability-less peer (peer3) receives an
 * IPv4 announcement as classic NLRI + NEXT_HOP, while the MP peer (peer4)
 * receives MP_REACH_NLRI.
 */
TEST_F(
    E2ELegacyV4NlriEncodingTest,
    CapabilityLessPeerClassicNlriMpPeerMpReach) {
  buildRib(/*legacyEncoding=*/true);

  bringUpAllPeersWithEoR();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr5, "11.0.0.1", "65005");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* peer3 (capability-less): classic v4 NLRI + NEXT_HOP, no MP_REACH. */
  auto toLegacy = waitForOutboundUpdate(peerId3_);
  ASSERT_TRUE(toLegacy.has_value());
  const auto& legacyUpdate = **toLegacy;
  EXPECT_FALSE(legacyUpdate.v4Announced2()->empty())
      << "capability-less peer must receive classic v4 NLRI";
  EXPECT_TRUE(legacyUpdate.mpAnnounced()->prefixes()->empty())
      << "capability-less peer must NOT receive MP_REACH";
  EXPECT_FALSE(legacyUpdate.attrs()->nexthop()->empty())
      << "classic NLRI must carry a NEXT_HOP attribute";

  /* peer4 (MP): MP_REACH_NLRI with afi=AFI_IPv4, no classic NLRI. */
  auto toMp = waitForOutboundUpdate(peerId4_);
  ASSERT_TRUE(toMp.has_value());
  const auto& mpUpdate = **toMp;
  EXPECT_FALSE(mpUpdate.mpAnnounced()->prefixes()->empty())
      << "MP peer must receive MP_REACH";
  EXPECT_EQ(BgpUpdateAfi::AFI_IPv4, *mpUpdate.mpAnnounced()->afi());
  EXPECT_TRUE(mpUpdate.v4Announced2()->empty())
      << "MP peer must NOT receive classic v4 NLRI";
}

/*
 * Feature disabled (config unset/false): the gate is off, so the
 * capability-less peer (peer3) receives MP_REACH_NLRI just like an MP peer --
 * exact pre-feature behavior, matching the removed gflag's default-off state.
 */
TEST_F(E2ELegacyV4NlriEncodingTest, CapabilityLessPeerMpReachWhenDisabled) {
  buildRib(/*legacyEncoding=*/false);

  bringUpAllPeersWithEoR();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr5, "11.0.0.1", "65005");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* peer3 (capability-less) with the feature off: MP_REACH, no classic NLRI. */
  auto toLegacy = waitForOutboundUpdate(peerId3_);
  ASSERT_TRUE(toLegacy.has_value());
  const auto& legacyUpdate = **toLegacy;
  EXPECT_FALSE(legacyUpdate.mpAnnounced()->prefixes()->empty())
      << "feature off: capability-less peer must receive MP_REACH";
  EXPECT_EQ(BgpUpdateAfi::AFI_IPv4, *legacyUpdate.mpAnnounced()->afi());
  EXPECT_TRUE(legacyUpdate.v4Announced2()->empty())
      << "feature off: capability-less peer must NOT receive classic v4 NLRI";
}

/*
 * Regression guard for the "gap is announcement-only" invariant with the
 * feature enabled: a v4 withdrawal to the capability-less peer is always
 * classic (v4Withdrawn2), never MP_UNREACH.
 */
TEST_F(E2ELegacyV4NlriEncodingTest, WithdrawalToCapabilityLessPeerIsClassic) {
  buildRib(/*legacyEncoding=*/true);

  bringUpAllPeersWithEoR();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr5, "11.0.0.1", "65005");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Drain the classic announcement to peer3 first. */
  auto announce = waitForOutboundUpdate(peerId3_);
  ASSERT_TRUE(announce.has_value());
  EXPECT_FALSE((*announce)->v4Announced2()->empty())
      << "announcement to capability-less peer must be classic v4 NLRI";

  /* Now withdraw and confirm the withdrawal is classic v4, not MP_UNREACH. */
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr5);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.0.0.0/8"));

  auto withdraw = waitForOutboundUpdate(peerId3_);
  ASSERT_TRUE(withdraw.has_value());
  EXPECT_FALSE((*withdraw)->v4Withdrawn2()->empty())
      << "v4 withdrawal to capability-less peer must be classic NLRI";
  EXPECT_TRUE((*withdraw)->mpWithdrawn()->prefixes()->empty())
      << "v4 withdrawal must NOT use MP_UNREACH";
}

} // namespace facebook::bgp
