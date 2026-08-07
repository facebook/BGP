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
 * A peer that advertised no MP-EXT capability (BgpPeerSpec::mpExtCapable=false)
 * receives IPv4 announcements as classic NLRI + NEXT_HOP (attr 3, RFC 4271),
 * while an MP-capable peer keeps receiving MP_REACH_NLRI (attr 14). The two
 * encodings never share an update group.
 *
 * Wire encoding is asserted on the raw BgpUpdate2 drained off each peer's
 * egress queue, because verifyRouteAdd() accepts either encoding and cannot
 * prove which one was used:
 *   - classic v4 NLRI  -> v4Announced2() populated, mpAnnounced() empty
 *   - MP_REACH_NLRI    -> mpAnnounced() populated with afi=AFI_IPv4
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
 */
class E2ELegacyV4NlriEncodingTest : public E2ESessionTestFixture {
 protected:
  void SetUp() override {
    BgpPeerSpec legacyPeer = kDefaultPeerSpec3;
    legacyPeer.mpExtCapable = false; /* capability-less receiver */
    addPeer(legacyPeer);

    addPeer(kDefaultPeerSpec4); /* MP receiver */
    addPeer(kDefaultPeerSpec5); /* MP source */

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
 * The capability-less peer (peer3) receives an IPv4 announcement as classic
 * NLRI + NEXT_HOP, while the MP peer (peer4) receives MP_REACH_NLRI.
 */
TEST_F(E2ELegacyV4NlriEncodingTest, LegacyPeerClassicNlriMpPeerMpReach) {
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
 * Regression guard for the "gap is announcement-only" invariant: v4 withdrawals
 * to the capability-less peer are classic (v4Withdrawn2). Verify a withdrawal
 * uses v4Withdrawn2 and never MP_UNREACH.
 */
TEST_F(E2ELegacyV4NlriEncodingTest, WithdrawalToLegacyPeerIsClassic) {
  bringUpAllPeersWithEoR();

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  addRoute("v4", "10.0.0.0", 8, kPeerAddr5, "11.0.0.1", "65005");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Drain the announcement to peer3 first. */
  auto announce = waitForOutboundUpdate(peerId3_);
  ASSERT_TRUE(announce.has_value());
  EXPECT_FALSE((*announce)->v4Announced2()->empty());

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
