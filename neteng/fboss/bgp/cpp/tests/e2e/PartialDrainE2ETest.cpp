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
 * PartialDrainE2ETest.cpp
 *
 * E2E tests for MNH partial drain functionality.
 * Tests cover:
 *   - Live-to-partial-drain-to-recover transitions
 *   - Strict MNH unchanged behavior (withdrawals when partial drain disabled)
 *   - Config rollback (disabling partial drain reverts to strict MNH)
 *   - Multi-prefix partial drain scenarios
 *
 * Mocked: FIB (TestFib), SessionManager (MockSessionManager)
 * Real: RIB, PeerManager, AdjRib
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/rib/RibDC.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;
using namespace rib_policy;

namespace facebook {
namespace bgp {

/*
 * E2E test fixture for partial drain tests.
 *
 * Uses 4 EBGP peers (peer3..peer6) to send routes, plus peer7 as an
 * IBGP/EBGP receiver for bestpath advertisements.  The fixture wires up
 * the full RIB + PeerManager pipeline with a TestFib so we can inject
 * PathSelectionPolicy (MNH + partial drain knobs) at runtime and observe
 * the end-to-end behavior.
 */
class E2EPartialDrainTest : public E2ETestFixture {
 protected:
  /* Peer specs for the 4 route-sending EBGP peers */
  static constexpr auto kMnhThreshold = 3;

  /*
   * Partial drain is a DC-only feature, so this suite runs against a RibDC
   * (its Buck target links :e2e_test_fixture_dc). rib_ is typed as the
   * platform-neutral RibBase; downcast to RibDC to reach the DC-only
   * path-selection / partial-drain APIs.
   */
  RibDC* dcRib() {
    return static_cast<RibDC*>(rib_.get());
  }

  BgpPeerSpec peerSpec3_ = {
      .asn = kPeerAsn3,
      .localAddr = kLocalAddr1,
      .peerAddr = kPeerAddr3,
      .v4Nexthop = kNextHopV4_3,
      .v6Nexthop = kNextHopV6_3,
      .description = "EBGP Peer 3",
  };

  BgpPeerSpec peerSpec4_ = {
      .asn = kPeerAsn4,
      .localAddr = kLocalAddr1,
      .peerAddr = kPeerAddr4,
      .v4Nexthop = kNextHopV4_4,
      .v6Nexthop = kNextHopV6_4,
      .description = "EBGP Peer 4",
  };

  BgpPeerSpec peerSpec5_ = {
      .asn = kPeerAsn5,
      .localAddr = kLocalAddr5,
      .peerAddr = kPeerAddr5,
      .v4Nexthop = kNextHopV4_5,
      .v6Nexthop = kNextHopV6_5,
      .description = "EBGP Peer 5",
  };

  BgpPeerSpec peerSpec6_ = {
      .asn = kPeerAsn6,
      .localAddr = kLocalAddr6,
      .peerAddr = kPeerAddr6,
      .v4Nexthop = kNextHopV4_6,
      .v6Nexthop = kNextHopV6_6,
      .description = "EBGP Peer 6",
  };

  /* Receiver peer for bestpath advertisements */
  BgpPeerSpec peerSpec7_ = {
      .asn = kPeerAsn7,
      .localAddr = kLocalAddr7,
      .peerAddr = kPeerAddr7,
      .v4Nexthop = kNextHopV4_7,
      .v6Nexthop = kNextHopV6_7,
      .description = "EBGP Peer 7 (receiver)",
  };

  void setupComponents() {
    addPeer(peerSpec3_);
    addPeer(peerSpec4_);
    addPeer(peerSpec5_);
    addPeer(peerSpec6_);
    addPeer(peerSpec7_);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
  }

  void bringUpAllPeersWithEor() {
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);
    bringUpPeer(kPeerAddr6);
    bringUpPeer(kPeerAddr7);

    BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
    BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
    BgpPeerId peerId6{kPeerAddr6, kPeerAddr6.asV4().toLongHBO()};
    BgpPeerId peerId7{kPeerAddr7, kPeerAddr7.asV4().toLongHBO()};

    sendEoRToPeer(peerId3);
    sendEoRToPeer(peerId4);
    sendEoRToPeer(peerId5);
    sendEoRToPeer(peerId6);
    sendEoRToPeer(peerId7);

    EXPECT_TRUE(waitForEoR(peerId7));
  }

  /*
   * Inject a PathSelectionPolicy with MNH threshold and partial drain flag.
   * Applies to the given list of prefixes.
   *
   * setPathSelectionPolicy() only enqueues; the RIB installs the policy on its
   * own event base. Block until the RIB reports it back, so whatever the
   * caller does next runs against the injected policy. A policy the RIB
   * declines to install fails here and names the cause, instead of surfacing
   * later as a bestpath or drain status that never converges.
   */
  void injectPathSelectionPolicy(
      const std::vector<folly::CIDRNetwork>& prefixes,
      int32_t mnhThreshold,
      bool enablePartialDrain) {
    TPathSelector tPathSelector;
    tPathSelector.bgp_native_path_selection_min_nexthop() = mnhThreshold;
    tPathSelector.drain_on_min_nexthop_violation() = enablePartialDrain;
    auto tPolicy =
        createTPathSelectionPolicyWithPathSelector(prefixes, tPathSelector);
    const auto expected = PathSelectionPolicy{tPolicy}.toThrift();
    dcRib()->setPathSelectionPolicy(
        std::make_unique<TPathSelectionPolicy>(tPolicy));
    WITH_RETRIES_N_TIMED(100, std::chrono::milliseconds(20), {
      EXPECT_EVENTUALLY_TRUE(dcRib()->getPathSelectionPolicy() == expected)
          << "RIB never installed the injected PathSelectionPolicy";
    });
  }

  /*
   * Clear the PathSelectionPolicy by sending one with no MNH constraint.
   */
  void clearPathSelectionPolicy(
      const std::vector<folly::CIDRNetwork>& prefixes) {
    TPathSelector tPathSelector;
    // No MNH threshold set - clears the constraint
    dcRib()->setPathSelectionPolicy(
        std::make_unique<TPathSelectionPolicy>(
            createTPathSelectionPolicyWithPathSelector(
                prefixes, tPathSelector)));
  }

  /*
   * Query partial drain status from RIB (thread-safe wrapper).
   */
  neteng::fboss::bgp::thrift::TPartialDrainStatus getPartialDrainStatus() {
    neteng::fboss::bgp::thrift::TPartialDrainStatus status;
    rib_->getEventBase().runInEventBaseThreadAndWait(
        [&]() { status = dcRib()->getPartialDrainStatus(); });
    return status;
  }

  /*
   * Drain all pending outbound updates from a peer's queue. Used between
   * phases when subsequent reads must see only updates triggered by the
   * next event.
   */
  void drainPeerQueue(const folly::IPAddress& peerAddr) {
    BgpPeerId peerId{peerAddr, peerAddr.asV4().toLongHBO()};
    while (waitForOutboundUpdate(peerId, /*maxRetries=*/5).has_value()) {
      /* discard */
    }
  }

  /*
   * Read up to maxAttempts outbound updates from a peer's queue and return
   * the first one whose announcements include the expected prefix. Used
   * when the RIB may emit several intermediate advertisements (e.g.
   * bestpath churn from cascading peer-down events) before the one we
   * actually want to verify.
   */
  std::optional<std::shared_ptr<const nettools::bgplib::BgpUpdate2>>
  readUpdateForPrefix(
      const BgpPeerId& peerId,
      const folly::CIDRNetwork& prefix,
      int maxAttempts = 20) {
    for (int i = 0; i < maxAttempts; ++i) {
      auto updateOpt = waitForOutboundUpdate(peerId, /*maxRetries=*/10);
      if (!updateOpt.has_value()) {
        continue;
      }
      bool cidrFound = false;
      for (const auto& nlri : *(*updateOpt)->v4Announced2()) {
        if (network::toCIDRNetwork(*nlri.prefix()) == prefix) {
          cidrFound = true;
          break;
        }
      }
      if (!cidrFound) {
        for (const auto& nlri : *(*updateOpt)->mpAnnounced()->prefixes()) {
          if (network::toCIDRNetwork(*nlri.prefix()) == prefix) {
            cidrFound = true;
            break;
          }
        }
      }
      if (cidrFound) {
        return updateOpt;
      }
    }
    return std::nullopt;
  }

  static bool updateHasCommunity(
      const nettools::bgplib::BgpUpdate2& update,
      const nettools::bgplib::BgpAttrCommunityC& community) {
    for (const auto& comm : *update.attrs()->communities()) {
      if (*comm.asn() == community.asn && *comm.value() == community.value) {
        return true;
      }
    }
    return false;
  }
};

/*
 * Test 1: LiveToPartialDrainToRecover
 *
 * Phase 1: 4 peers announce a prefix. MNH=3 with partial drain enabled.
 *          MNH is satisfied -> no partial drain.
 * Phase 2: 2 peers go down. Only 2 paths remain (< MNH=3).
 *          Partial drain activates: bestpath retained, drain community
 * attached. Phase 3: 1 peer comes back. 3 paths (>= MNH=3). Partial drain
 * deactivates: normal operation resumes.
 */
TEST_F(E2EPartialDrainTest, LiveToPartialDrainToRecover) {
  XLOG(INFO, "=== Starting LiveToPartialDrainToRecover ===");

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.1.0.0/24");

  /* Phase 1: All 4 peers announce the prefix */
  XLOG(INFO, "Phase 1: All 4 peers announce prefix");
  addRoute("v4", "10.1.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.1.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  addRoute("v4", "10.1.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  addRoute("v4", "10.1.0.0", 24, kPeerAddr6, kNextHopV4_6.str(), "64543");

  /* Wait for all 4 paths to arrive in RIB */
  ASSERT_TRUE(waitForPathCountInRib("10.1.0.0/24", 4));

  /* Inject MNH=3 policy with partial drain enabled only after all 4 paths are
   * present, so MNH=3 is satisfied on the first path-selection pass. Injecting
   * before the routes arrive would expose a transient sub-threshold window
   * (paths arriving 1->2->3->4) in which partial drain briefly activates,
   * racing the point-in-time check below. */
  injectPathSelectionPolicy(
      {prefix}, kMnhThreshold, /*enablePartialDrain=*/true);

  /* Verify bestpath exists and MNH is satisfied (no partial drain) */
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  auto status1 = getPartialDrainStatus();
  EXPECT_FALSE(*status1.is_partially_drained())
      << "Phase 1: MNH satisfied, partial drain should be inactive";

  /* Phase 2: Bring down 2 peers -> only 2 paths remain (< MNH=3) */
  XLOG(INFO, "Phase 2: Bring down peer5 and peer6");
  bringDownPeer(kPeerAddr5);
  bringDownPeer(kPeerAddr6);

  /* Wait for partial drain to activate (path selection converges asynchronously
   * after peers go down: AdjRibIn marks routes stale, RIB removes paths, then
   * path selection re-runs and applies MNH policy). */
  WITH_RETRIES_N(50, {
    auto status2 = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status2.is_partially_drained())
        << "Phase 2: MNH violated, partial drain should be active";
    EXPECT_EVENTUALLY_EQ(*status2.num_affected_prefixes(), 1);
  });

  /* Bestpath should still exist (not withdrawn) */
  auto bestpath = getBestPath(prefix);
  EXPECT_NE(bestpath, nullptr)
      << "Phase 2: Bestpath should be retained under partial drain";

  /* Phase 3: Bring back peer5 -> 3 paths (>= MNH=3), partial drain clears */
  XLOG(INFO, "Phase 3: Bring up peer5");
  bringUpPeer(kPeerAddr5);
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId5);

  /* Re-announce route from peer5 */
  addRoute("v4", "10.1.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");

  /* Wait for 3 paths */
  ASSERT_TRUE(waitForPathCountInRib("10.1.0.0/24", 3));

  /* Wait for partial drain to clear (path selection re-runs after peer rejoin
   * + EoR + new path arrival, then MNH-satisfied state propagates). */
  WITH_RETRIES_N(50, {
    auto status3 = getPartialDrainStatus();
    EXPECT_EVENTUALLY_FALSE(*status3.is_partially_drained())
        << "Phase 3: MNH recovered, partial drain should be inactive";
  });
}

/*
 * Test 2: StrictMnhStillWithdraws
 *
 * With drain_on_min_nexthop_violation=false (default), MNH violation should
 * cause the prefix to be withdrawn entirely (no partial drain).
 */
TEST_F(E2EPartialDrainTest, StrictMnhStillWithdraws) {
  XLOG(INFO, "=== Starting StrictMnhStillWithdraws ===");

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.2.0.0/24");

  /* Phase 1: All 4 peers announce the prefix, MNH satisfied */
  addRoute("v4", "10.2.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.2.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  addRoute("v4", "10.2.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  addRoute("v4", "10.2.0.0", 24, kPeerAddr6, kNextHopV4_6.str(), "64543");
  ASSERT_TRUE(waitForPathCountInRib("10.2.0.0/24", 4));
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  /* Inject MNH=3 WITHOUT partial drain */
  injectPathSelectionPolicy(
      {prefix}, kMnhThreshold, /*enablePartialDrain=*/false);

  /* Phase 2: Bring down 2 peers -> violate MNH */
  bringDownPeer(kPeerAddr5);
  bringDownPeer(kPeerAddr6);

  /* Under strict MNH, wait for bestpath to be withdrawn (path selection
   * re-runs asynchronously after peers go down). Withdrawal and the
   * partial-drain status update can land in separate path-selection passes, so
   * assert both invariants inside the same retry window to avoid evaluating the
   * drain check between passes. */
  WITH_RETRIES_N(50, {
    auto bestpath = getBestPath(prefix);
    EXPECT_EVENTUALLY_EQ(bestpath, nullptr)
        << "Strict MNH: bestpath should be nullptr (withdrawn)";
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_FALSE(*status.is_partially_drained())
        << "Strict MNH: partial drain should NOT be active";
  });
}

/*
 * Test 3: RollbackPartialDrainConfig
 *
 * Phase 1: drain_on_min_nexthop_violation=true, MNH violated -> partial drain
 * active. Phase 2: Send new policy without drain_on_min_nexthop_violation ->
 * reverts to strict MNH behavior (withdrawal).
 */
TEST_F(E2EPartialDrainTest, RollbackPartialDrainConfig) {
  XLOG(INFO, "=== Starting RollbackPartialDrainConfig ===");

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.3.0.0/24");

  /* Announce routes from only 2 peers (will violate MNH=3) */
  addRoute("v4", "10.3.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.3.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  ASSERT_TRUE(waitForPathCountInRib("10.3.0.0/24", 2));

  /* Phase 1: Inject MNH=3 with partial drain enabled */
  XLOG(INFO, "Phase 1: Enable partial drain with MNH=3");
  injectPathSelectionPolicy(
      {prefix}, kMnhThreshold, /*enablePartialDrain=*/true);

  /* Wait for partial drain to activate (policy update is processed
   * asynchronously by RIB and triggers path selection). */
  WITH_RETRIES_N(50, {
    auto status1 = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status1.is_partially_drained())
        << "Phase 1: Partial drain should be active";
  });

  /* Bestpath retained under partial drain */
  auto bestpath1 = getBestPath(prefix);
  EXPECT_NE(bestpath1, nullptr)
      << "Phase 1: Bestpath should be retained under partial drain";

  /* Phase 2: Rollback - disable partial drain */
  XLOG(INFO, "Phase 2: Rollback partial drain config");
  injectPathSelectionPolicy(
      {prefix}, kMnhThreshold, /*enablePartialDrain=*/false);

  /* Wait for rollback: bestpath should be withdrawn AND partial drain
   * should clear (both happen asynchronously after path selection re-runs). */
  WITH_RETRIES_N(50, {
    auto bestpath2 = getBestPath(prefix);
    EXPECT_EVENTUALLY_EQ(bestpath2, nullptr)
        << "Phase 2: After rollback, bestpath should be withdrawn (strict MNH)";
    auto status2 = getPartialDrainStatus();
    EXPECT_EVENTUALLY_FALSE(*status2.is_partially_drained())
        << "Phase 2: Partial drain should be inactive after rollback";
  });
}

/*
 * Test 4: PartialDrainWithMultiplePrefixes
 *
 * 3 prefixes with MNH=3, drain_on_min_nexthop_violation=true.
 * - Prefix A: 4 paths (MNH satisfied)
 * - Prefix B: 2 paths (MNH violated -> partial drain)
 * - Prefix C: 2 paths (MNH violated -> partial drain)
 *
 * Verify: getPartialDrainStatus has num_affected_prefixes=2.
 */
TEST_F(E2EPartialDrainTest, PartialDrainWithMultiplePrefixes) {
  XLOG(INFO, "=== Starting PartialDrainWithMultiplePrefixes ===");

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefixA = folly::IPAddress::createNetwork("10.10.0.0/24");
  const auto prefixB = folly::IPAddress::createNetwork("10.20.0.0/24");
  const auto prefixC = folly::IPAddress::createNetwork("10.30.0.0/24");

  /* Prefix A: 4 paths (MNH satisfied) */
  addRoute("v4", "10.10.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.10.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  addRoute("v4", "10.10.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  addRoute("v4", "10.10.0.0", 24, kPeerAddr6, kNextHopV4_6.str(), "64543");
  ASSERT_TRUE(waitForPathCountInRib("10.10.0.0/24", 4));

  /* Prefix B: only 2 paths (violates MNH) */
  addRoute("v4", "10.20.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.20.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  ASSERT_TRUE(waitForPathCountInRib("10.20.0.0/24", 2));

  /* Prefix C: only 2 paths (violates MNH) */
  addRoute("v4", "10.30.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.30.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  ASSERT_TRUE(waitForPathCountInRib("10.30.0.0/24", 2));

  /*
   * Inject the MNH=3 partial-drain policy only after all paths are present, so
   * the policy change drives a single deterministic recompute over the final
   * path counts (A=4 satisfied; B=2 and C=2 violating). Injecting before the
   * routes arrive races the async setPathSelectionPolicy against per-route path
   * selection -- a route selected before the policy lands is never re-marked,
   * leaving partial drain stuck at 0 affected -- the same ordering hazard
   * called out in LiveToPartialDrainToRecover.
   */
  injectPathSelectionPolicy(
      {prefixA, prefixB, prefixC}, kMnhThreshold, /*enablePartialDrain=*/true);

  /*
   * Partial-drain status is updated asynchronously by the policy-change
   * recompute, so retry until it converges — consistent with the other tests
   * in this file.
   */
  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status.is_partially_drained())
        << "Partial drain should be active (prefixes B and C violate MNH)";
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 2)
        << "Exactly 2 prefixes should be in partial drain (B and C)";
  });

  /* Prefix A should NOT be partially drained (MNH satisfied) */
  auto bestpathA = getBestPath(prefixA);
  EXPECT_NE(bestpathA, nullptr)
      << "Prefix A: bestpath should exist (MNH satisfied)";

  /* Prefixes B and C should have bestpath retained under partial drain */
  auto bestpathB = getBestPath(prefixB);
  EXPECT_NE(bestpathB, nullptr)
      << "Prefix B: bestpath retained under partial drain";

  auto bestpathC = getBestPath(prefixC);
  EXPECT_NE(bestpathC, nullptr)
      << "Prefix C: bestpath retained under partial drain";
}

/*
 * T1a: DrainCommunityAttachedOnPartialDrain
 *
 * Wire-level guarantee that the drain community (65446:10) appears on the
 * outbound advertisement to peer7 once a prefix enters partial drain.
 * Existing tests only assert RIB-internal state (getPartialDrainStatus and
 * getBestPath) — peer7's queue is never read. This test exercises the full
 * pipeline ending in applyPartialDrainCommunities() in AdjRibCommon.cpp,
 * which is the entire feature's external observable.
 *
 * Phase 1: 4 paths -> MNH=3 satisfied -> live advertisement (no drain).
 * Phase 2: drop 2 peers -> 2 paths < MNH -> drain re-advertisement carries
 *          65446:10.
 */
TEST_F(E2EPartialDrainTest, DrainCommunityAttachedOnPartialDrain) {
  XLOG(INFO, "=== Starting DrainCommunityAttachedOnPartialDrain ===");

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.4.0.0/24");
  injectPathSelectionPolicy(
      {prefix}, kMnhThreshold, /*enablePartialDrain=*/true);

  /* Phase 1: 4 paths -> MNH satisfied */
  addRoute("v4", "10.4.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.4.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  addRoute("v4", "10.4.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  addRoute("v4", "10.4.0.0", 24, kPeerAddr6, kNextHopV4_6.str(), "64543");
  ASSERT_TRUE(waitForPathCountInRib("10.4.0.0/24", 4));

  /* Drain peer7's queue of all initial bestpath churn so subsequent reads
   * see only updates triggered by the upcoming drain transition. */
  drainPeerQueue(kPeerAddr7);

  /* Phase 2: drop 2 peers -> drain activates */
  bringDownPeer(kPeerAddr5);
  bringDownPeer(kPeerAddr6);

  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status.is_partially_drained());
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 1);
  });

  /* Find the drain re-advertisement to peer7. Bestpath churn from the
   * peer-down events may emit one or more intermediate updates without the
   * drain bit; the drain re-advertisement is the one we care about. */
  BgpPeerId peerId7{kPeerAddr7, kPeerAddr7.asV4().toLongHBO()};
  bool foundDrainCommunity = false;
  for (int attempt = 0; attempt < 30; ++attempt) {
    auto updateOpt = readUpdateForPrefix(peerId7, prefix, 5);
    if (!updateOpt.has_value()) {
      continue;
    }
    if (updateHasCommunity(**updateOpt, kDrainCommunity)) {
      foundDrainCommunity = true;
      break;
    }
  }
  EXPECT_TRUE(foundDrainCommunity)
      << "peer7 never received an update for 10.4.0.0/24 carrying drain "
         "community 65446:10";
}

/*
 * T1b: DrainCommunityRemovedOnRecover
 *
 * Wire-level guarantee that the drain community is REMOVED from outbound
 * advertisements once partial drain clears. Today only the RIB-internal
 * boolean flip is asserted — peer7 might keep receiving 65446:10 on the
 * wire forever and existing tests would still pass.
 *
 * Phase 1: drop 2 peers -> drain -> peer7 sees 65446:10
 * Phase 2: bring peer5 back -> MNH recovered -> peer7 receives an update
 *          for the same prefix WITHOUT 65446:10.
 */
TEST_F(E2EPartialDrainTest, DrainCommunityRemovedOnRecover) {
  XLOG(INFO, "=== Starting DrainCommunityRemovedOnRecover ===");

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.5.0.0/24");
  injectPathSelectionPolicy(
      {prefix}, kMnhThreshold, /*enablePartialDrain=*/true);

  addRoute("v4", "10.5.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.5.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  addRoute("v4", "10.5.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  addRoute("v4", "10.5.0.0", 24, kPeerAddr6, kNextHopV4_6.str(), "64543");
  ASSERT_TRUE(waitForPathCountInRib("10.5.0.0/24", 4));

  drainPeerQueue(kPeerAddr7);

  /* Phase 1: trigger drain. */
  bringDownPeer(kPeerAddr5);
  bringDownPeer(kPeerAddr6);

  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status.is_partially_drained());
  });

  BgpPeerId peerId7{kPeerAddr7, kPeerAddr7.asV4().toLongHBO()};
  bool sawDrainCommunity = false;
  for (int attempt = 0; attempt < 30; ++attempt) {
    auto updateOpt = readUpdateForPrefix(peerId7, prefix, 5);
    if (updateOpt.has_value() &&
        updateHasCommunity(**updateOpt, kDrainCommunity)) {
      sawDrainCommunity = true;
      break;
    }
  }
  ASSERT_TRUE(sawDrainCommunity)
      << "Pre-recover assertion failed: peer7 never saw drain community";

  /* Drain churn before triggering recovery so we read only post-recover
   * updates. */
  drainPeerQueue(kPeerAddr7);

  /* Phase 2: bring peer5 back -> MNH recovered. */
  bringUpPeer(kPeerAddr5);
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId5);
  addRoute("v4", "10.5.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  ASSERT_TRUE(waitForPathCountInRib("10.5.0.0/24", 3));

  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_FALSE(*status.is_partially_drained());
  });

  /* The recovery re-advertisement to peer7 must NOT carry 65446:10. We
   * accept any update that announces the prefix; what matters is that drain
   * community is absent on each one. */
  bool foundRecoveryUpdate = false;
  for (int attempt = 0; attempt < 30; ++attempt) {
    auto updateOpt = readUpdateForPrefix(peerId7, prefix, 5);
    if (!updateOpt.has_value()) {
      continue;
    }
    EXPECT_FALSE(updateHasCommunity(**updateOpt, kDrainCommunity))
        << "Recovery update to peer7 still carried drain community";
    foundRecoveryUpdate = true;
    break;
  }
  EXPECT_TRUE(foundRecoveryUpdate)
      << "peer7 never received a post-recovery update for 10.5.0.0/24";
}

/*
 * T1d: DrainCommunityWithCommunityBlindEgressPolicy
 *
 * Regression guard for the egress policy-cache collision fixed by keying the
 * cache on isPartialDrain (MNH 6/N). When the receiver peer has an egress
 * policy that does NOT match or set communities, PolicyAttributesMask::
 * communities is false, so the policy-cache key excludes communities. The
 * live (non-drain) announce primes the cache with the live post-policy
 * result; the later drain announce for the same prefix would, without
 * isPartialDrain in the cache key, get a stale live cache hit and never carry
 * the drain community on the wire.
 *
 * The other DrainCommunity* E2E tests do not configure an egress policy on
 * peer7, so egressPolicyConfigured() is false and the cache is bypassed --
 * they pass even with the collision present. This test attaches a community-
 * blind egress policy to peer7 and exercises the full pipeline (RIB ->
 * PeerManager -> AdjRib -> egress policy + cache) to peer7's wire, asserting
 * the drain community DOES appear after a live->drain transition. It fails
 * without the isPartialDrain cache key and passes with it.
 */
TEST_F(E2EPartialDrainTest, DrainCommunityWithCommunityBlindEgressPolicy) {
  XLOG(INFO, "=== Starting DrainCommunityWithCommunityBlindEgressPolicy ===");

  /* Community-blind egress policy on peer7 -> cache key excludes communities */
  addAcceptAllEgressPolicy("accept-all-egress");
  peerSpec7_.egressPolicyName = "accept-all-egress";

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.11.0.0/24");
  injectPathSelectionPolicy(
      {prefix}, kMnhThreshold, /*enablePartialDrain=*/true);

  /* Phase 1: 4 paths -> MNH satisfied -> live advertisement primes the cache */
  addRoute("v4", "10.11.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.11.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  addRoute("v4", "10.11.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  addRoute("v4", "10.11.0.0", 24, kPeerAddr6, kNextHopV4_6.str(), "64543");
  ASSERT_TRUE(waitForPathCountInRib("10.11.0.0/24", 4));

  /* Ensure the live advertisement is emitted: this stores the live (no-drain)
   * post-policy result under the prefix+policy cache key. */
  BgpPeerId peerId7{kPeerAddr7, kPeerAddr7.asV4().toLongHBO()};
  ASSERT_TRUE(readUpdateForPrefix(peerId7, prefix).has_value())
      << "peer7 never received the initial live advertisement";
  drainPeerQueue(kPeerAddr7);

  /* Phase 2: drop 2 peers -> drain activates */
  bringDownPeer(kPeerAddr5);
  bringDownPeer(kPeerAddr6);
  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status.is_partially_drained());
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 1);
  });

  /* The drain re-advertisement must carry 65446:10 even though the egress
   * policy is community-blind. Without isPartialDrain in the cache key this
   * would be a stale live cache hit with no drain community. */
  bool foundDrainCommunity = false;
  for (int attempt = 0; attempt < 30; ++attempt) {
    auto updateOpt = readUpdateForPrefix(peerId7, prefix, 5);
    if (updateOpt.has_value() &&
        updateHasCommunity(**updateOpt, kDrainCommunity)) {
      foundDrainCommunity = true;
      break;
    }
  }
  EXPECT_TRUE(foundDrainCommunity)
      << "peer7 (community-blind egress policy) never received 10.11.0.0/24 "
         "carrying drain community 65446:10 -- egress policy cache collision";
}

/*
 * Separate fixture: peer7 has add-path BOTH so the local router sends
 * multiple paths to peer7 with distinct path IDs. Mirrors the structure of
 * E2EPartialDrainTest but overrides peer7 setup to enable add-path SEND.
 */
class E2EPartialDrainAddPathTest : public E2EPartialDrainTest {
 protected:
  void setupComponentsAddPathPeer7() {
    /* peer7 receives multipaths via add-path SEND */
    peerSpec7_.addPathCapability = nettools::bgplib::BgpAddPathSendRec::BOTH;
    addPeer(peerSpec3_);
    addPeer(peerSpec4_);
    addPeer(peerSpec5_);
    addPeer(peerSpec6_);
    addPeer(peerSpec7_);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/false, /*enableEgressBackpressure=*/true);
  }
};

/*
 * T1c: AddPathSendDrainPropagation
 *
 * Add-path SEND has its own copy of the drain-bit propagation logic in
 * PeerManager.cpp:971-977 and AdjRibGroup.cpp:798/843, neither of which is
 * exercised by any existing E2E test (no peer in the partial-drain fixture
 * has addPathCapability set). This test wires peer7 with add-path BOTH so
 * the local router emits per-path advertisements; verifies that updates to
 * peer7 carry the drain community when partial drain is active.
 */
TEST_F(E2EPartialDrainAddPathTest, AddPathSendDrainPropagation) {
  XLOG(INFO, "=== Starting AddPathSendDrainPropagation ===");

  setupComponentsAddPathPeer7();
  bringUpAllPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.6.0.0/24");
  /* MNH=5 with 4 paths -> always violated -> drain immediately on first
   * path-selection pass after the routes arrive. */
  injectPathSelectionPolicy(
      {prefix}, /*mnhThreshold=*/5, /*enablePartialDrain=*/true);

  addRoute("v4", "10.6.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.6.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  addRoute("v4", "10.6.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  addRoute("v4", "10.6.0.0", 24, kPeerAddr6, kNextHopV4_6.str(), "64543");
  ASSERT_TRUE(waitForPathCountInRib("10.6.0.0/24", 4));

  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status.is_partially_drained());
  });

  /* peer7 should receive at least one add-path entry for the prefix carrying
   * the drain community. The exact count depends on encoder behavior — what
   * matters is that the add-path SEND code path attaches drain. */
  BgpPeerId peerId7{kPeerAddr7, kPeerAddr7.asV4().toLongHBO()};
  bool foundDrainOnAddPath = false;
  for (int attempt = 0; attempt < 30; ++attempt) {
    auto updateOpt = readUpdateForPrefix(peerId7, prefix, 5);
    if (!updateOpt.has_value()) {
      continue;
    }
    if (updateHasCommunity(**updateOpt, kDrainCommunity)) {
      foundDrainOnAddPath = true;
      break;
    }
  }
  EXPECT_TRUE(foundDrainOnAddPath)
      << "Add-path peer7 never received an update for 10.6.0.0/24 carrying "
         "drain community 65446:10";
}

/*
 * T2a: PartialDrainWithdrawAffectedPeerMidDrain
 *
 * Cascading peer losses while drain is already active. Today's tests only
 * exercise a clean drop-2 / restore-1 lifecycle; this one keeps dropping
 * peers while drain is active and verifies:
 *   - num_affected_prefixes stays 1 across drops that don't add prefixes
 *   - transition_count does NOT bump on path-count changes within a drained
 *     prefix (only on device-level 0<->non-zero flips)
 *   - dropping the LAST path clears drain (entry has no routes) and the
 *     transition counter bumps to reflect the device flipping out of drain.
 */
TEST_F(E2EPartialDrainTest, PartialDrainWithdrawAffectedPeerMidDrain) {
  XLOG(INFO, "=== Starting PartialDrainWithdrawAffectedPeerMidDrain ===");

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefix = folly::IPAddress::createNetwork("10.7.0.0/24");
  injectPathSelectionPolicy(
      {prefix}, kMnhThreshold, /*enablePartialDrain=*/true);

  addRoute("v4", "10.7.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  addRoute("v4", "10.7.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  addRoute("v4", "10.7.0.0", 24, kPeerAddr5, kNextHopV4_5.str(), "64542");
  addRoute("v4", "10.7.0.0", 24, kPeerAddr6, kNextHopV4_6.str(), "64543");
  ASSERT_TRUE(waitForPathCountInRib("10.7.0.0/24", 4));

  /* Drop 2 peers -> drain activates (4 -> 2 paths). */
  bringDownPeer(kPeerAddr5);
  bringDownPeer(kPeerAddr6);

  int64_t transitionCountAfterFirstDrain = 0;
  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status.is_partially_drained());
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 1);
    transitionCountAfterFirstDrain = *status.partial_drain_transition_count();
  });
  EXPECT_GE(transitionCountAfterFirstDrain, 1)
      << "First device-level drain should have bumped transition_count";

  /* Drop one more peer mid-drain (2 -> 1 path). Still drained, still 1
   * affected prefix, transition count unchanged because device-level
   * is_partially_drained doesn't flip. */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPathCountInRib("10.7.0.0/24", 1));

  /* The 2->1 path drop never empties the entry, so the device stays drained
   * throughout with exactly 1 affected prefix and an unchanged transition
   * count. Assert these invariants hold across a settled window rather than via
   * eventual-equality, which would pass on the first matching sample and miss a
   * delayed bump. */
  CHECK_HOLDS_FOR_DURATION(std::chrono::seconds(2), [&]() {
    auto status = getPartialDrainStatus();
    return *status.is_partially_drained() &&
        *status.num_affected_prefixes() == 1 &&
        *status.partial_drain_transition_count() ==
        transitionCountAfterFirstDrain;
  });
  /* Bestpath still retained. */
  EXPECT_NE(getBestPath(prefix), nullptr);

  /* Drop the last path -> entry has no routes -> drain clears, count bumps
   * to reflect the device flipping out of drain. */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.7.0.0/24"));

  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_FALSE(*status.is_partially_drained());
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 0);
    EXPECT_EVENTUALLY_EQ(
        *status.partial_drain_transition_count(),
        transitionCountAfterFirstDrain + 1)
        << "Final drop should bump device-level transition counter";
  });
}

/*
 * T3a: TransitionCountOnlyOnDeviceFlip
 *
 * E2E mirror of RibTest::PartialDrainTransitionCountOnlyOnDeviceFlip. Locks
 * in the IDL contract on partial_drain_transition_count: the counter must
 * bump only when the device-level is_partially_drained flips (count crosses
 * zero), NOT on every per-prefix transition. Without this guard, a future
 * refactor could double-count or under-count, silently breaking Centralium's
 * device-level drain accounting.
 *
 * Sequence on transition_count, starting at 0:
 *   1. Prefix A enters drain: 0 -> 1 device flip -> count = 1
 *   2. Prefix B enters drain: device already drained -> count stays at 1
 *   3. Prefix A recovers: device still drained (B still in drain) -> count
 *      stays at 1
 *   4. Prefix B recovers: 1 -> 0 device flip -> count = 2
 */
TEST_F(E2EPartialDrainTest, TransitionCountOnlyOnDeviceFlip) {
  XLOG(INFO, "=== Starting TransitionCountOnlyOnDeviceFlip ===");

  setupComponents();
  bringUpAllPeersWithEor();

  const auto prefixA = folly::IPAddress::createNetwork("10.8.0.0/24");
  const auto prefixB = folly::IPAddress::createNetwork("10.9.0.0/24");
  injectPathSelectionPolicy(
      {prefixA, prefixB}, kMnhThreshold, /*enablePartialDrain=*/true);

  /* Phase 1: prefixA gets 1 path -> drain. Device flips 0 -> 1, count = 1. */
  addRoute("v4", "10.8.0.0", 24, kPeerAddr3, kNextHopV4_3.str(), "65010");
  ASSERT_TRUE(waitForPathCountInRib("10.8.0.0/24", 1));

  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_TRUE(*status.is_partially_drained());
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 1);
    EXPECT_EVENTUALLY_EQ(*status.partial_drain_transition_count(), 1);
  });

  /* Phase 2: prefixB also gets 1 path -> per-prefix drain transition, but
   * device is already drained. count must stay at 1. */
  addRoute("v4", "10.9.0.0", 24, kPeerAddr4, kNextHopV4_4.str(), "64541");
  ASSERT_TRUE(waitForPathCountInRib("10.9.0.0/24", 1));

  /* Wait for prefixB's drain to register (num_affected_prefixes 1 -> 2). */
  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 2);
  });

  /* The device is already drained, so the second prefix entering drain must NOT
   * bump the device-level transition_count. Assert it stays at 1 across a
   * settled window rather than via eventual-equality, which would pass on the
   * first matching sample and miss a delayed bump. */
  CHECK_HOLDS_FOR_DURATION(std::chrono::seconds(2), [&]() {
    auto status = getPartialDrainStatus();
    return *status.partial_drain_transition_count() == 1;
  });

  /* Phase 3: drop peer3 -> prefixA loses its only path -> entry empties ->
   * prefixA exits drain. Device-level is_partially_drained still true (B is
   * still drained). count must stay at 1. */
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.8.0.0/24"));

  /* Wait for prefixA to exit drain (num_affected_prefixes 2 -> 1). */
  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 1);
  });

  /* prefixA exited drain but the device stays drained (prefixB still in drain),
   * so a per-prefix recovery must NOT bump transition_count. Assert the device
   * stays drained and the count holds at 1 across a settled window rather than
   * via eventual-equality, which would pass on the first matching sample and
   * miss a delayed bump. */
  CHECK_HOLDS_FOR_DURATION(std::chrono::seconds(2), [&]() {
    auto status = getPartialDrainStatus();
    return *status.is_partially_drained() &&
        *status.partial_drain_transition_count() == 1;
  });

  /* Phase 4: drop peer4 -> prefixB exits drain too -> device flips 1 -> 0.
   * count must bump to 2. */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForRouteWithdrawnFromRib("10.9.0.0/24"));

  WITH_RETRIES_N(50, {
    auto status = getPartialDrainStatus();
    EXPECT_EVENTUALLY_FALSE(*status.is_partially_drained());
    EXPECT_EVENTUALLY_EQ(*status.num_affected_prefixes(), 0);
    EXPECT_EVENTUALLY_EQ(*status.partial_drain_transition_count(), 2)
        << "Last prefix exiting drain (device flips) must bump count";
  });
}

} // namespace bgp
} // namespace facebook
