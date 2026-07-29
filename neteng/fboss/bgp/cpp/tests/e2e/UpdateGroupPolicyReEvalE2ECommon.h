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
 * Common fixture for Update Group policy re-evaluation E2E tests.
 *
 * Combines the SlowPeerTestBase group-state/backpressure helpers with runtime
 * egress-policy changes driven through the real BgpServiceBB thrift APIs:
 *   - co_setPeerGroupsPolicy: peer-group-level egress policy change. With no
 *     per-peer override this takes the in-place group rekey + group RIB re-walk
 *     path (group-only re-evaluation).
 *   - co_setPeersPolicy / co_unsetPeersPolicy: per-peer egress policy override,
 *     which on re-evaluation splits/moves the peer to a different group.
 *
 * BgpServiceBB shares the fixture's single ConfigManager with the PeerManager
 * so config version tracking stays consistent across policy updates.
 *
 * Ordering constraint: peers must reach JOINED_RUNNING and consume EoR (so the
 * PeerManager reports initialized) before any policy RPC — otherwise the API
 * rejects with INPUT_ERROR.
 */

#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/facebook/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"

namespace facebook {
namespace bgp {

/* Policy names registered by setupPolicies(). */
inline constexpr auto kAcceptPolicyName = "ACCEPT_POLICY";
inline constexpr auto kDenyPolicyName = "DENY_POLICY";
inline constexpr auto kTagPolicyName = "TAG_POLICY";
inline constexpr auto kTagCommunity = "12345:1";

/*
 * Community-matching egress policies mirroring the UT
 * (UpdateGroupPolicyReEvalUTCommon.h). Routes are tagged on ingress with these
 * communities and the egress policies match on them:
 *   - kMatchNoAdvtDenyPolicyName: DENY routes carrying kCommNoAdvt, permit
 * rest.
 *   - kMatchModifyAppendPolicyName: append kCommAppend to routes carrying
 *     kCommModify, permit all.
 *   - kPermitAllPolicyName: permit all, unmodified.
 */
inline constexpr auto kCommNoAdvt = "65535:65282";
inline constexpr auto kCommModify = "65500:100";
inline constexpr auto kCommAppend = "65500:200";
inline constexpr auto kMatchNoAdvtDenyPolicyName =
    "match-no-advt-community-deny-continue";
inline constexpr auto kMatchModifyAppendPolicyName =
    "match-modify-community-append-tag-continue";
inline constexpr auto kPermitAllPolicyName = "permit-all-continue";

/* Shared peer-group name used to form a single update group. */
inline constexpr auto kReEvalPeerGroupName = "reeval-peer-group";

/*
 * Per-group peer-group name for the multi-group tests. Since peerGroupName is
 * part of the update-group key, distinct names form distinct update groups.
 */
inline std::string reEvalPeerGroupName(int groupIdx) {
  return fmt::format("reeval-peer-group-{}", groupIdx);
}

/* Default scale for the group-level re-evaluation scale test. */
inline constexpr int kNumPeers = 10;
inline constexpr int kNumRoutes = 100;

/* Multi-group scale: 4 peer groups (= 4 update groups), 10 peers each. */
inline constexpr int kNumGroups = 4;
inline constexpr int kPeersPerGroup = 10;

/*
 * Base fixture: SlowPeerTestBase (group-state + backpressure helpers,
 * parameterized over serialization mode) plus a BgpServiceBB for runtime
 * policy changes.
 */
class UpdateGroupPolicyReEvalE2EBase : public SlowPeerTestBase {
 protected:
  /*
   * Deny routes carrying kCommNoAdvt, permit everything else. Mirrors the UT
   * buildMatchNoAdvtDenyPolicy.
   */
  static bgp_policy::BgpPolicyStatement buildMatchNoAdvtDenyPolicy() {
    auto match = createBgpPolicyAtomicMatch(
        bgp_policy::BgpPolicyAtomicMatchType::COMMUNITY_LIST, {kCommNoAdvt});
    auto denyTerm = createBgpPolicyTerm(
        "deny-no-advt",
        "",
        {std::move(match)},
        {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::DENY)},
        bgp_policy::FlowControlAction::NEXT_TERM);
    auto acceptTerm = createBgpPolicyTerm(
        "accept-all",
        "",
        {},
        {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
        bgp_policy::FlowControlAction::NEXT_TERM);
    return createBgpPolicyStatement(
        kMatchNoAdvtDenyPolicyName,
        {std::move(denyTerm), std::move(acceptTerm)});
  }

  /*
   * Append kCommAppend to routes carrying kCommModify, permit all. Mirrors the
   * UT buildMatchModifyAppendPolicy.
   */
  static bgp_policy::BgpPolicyStatement buildMatchModifyAppendPolicy() {
    auto match = createBgpPolicyAtomicMatch(
        bgp_policy::BgpPolicyAtomicMatchType::COMMUNITY_LIST, {kCommModify});
    auto tagTerm = createBgpPolicyTerm(
        "append-tag",
        "",
        {std::move(match)},
        {createBgpPolicyCommunityAction(
             bgp_policy::CommunityActionType::ADD, {kCommAppend}),
         createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
        bgp_policy::FlowControlAction::NEXT_TERM);
    auto acceptTerm = createBgpPolicyTerm(
        "accept-all",
        "",
        {},
        {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
        bgp_policy::FlowControlAction::NEXT_TERM);
    return createBgpPolicyStatement(
        kMatchModifyAppendPolicyName,
        {std::move(tagTerm), std::move(acceptTerm)});
  }

  /* Permit-all, unmodified. Mirrors the UT buildPermitAllPolicy. */
  static bgp_policy::BgpPolicyStatement buildPermitAllPolicy() {
    auto acceptTerm = createBgpPolicyTerm(
        "accept-all",
        "",
        {},
        {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
        bgp_policy::FlowControlAction::NEXT_TERM);
    return createBgpPolicyStatement(
        kPermitAllPolicyName, {std::move(acceptTerm)});
  }

  /*
   * Register accept-all (PERMIT), deny-all (DENY), and tag (PERMIT + add
   * community) egress policies, plus the community-matching policies used by
   * the scale re-eval tests. Call before setupComponentsWithBgpService().
   */
  void setupPolicies() {
    auto acceptStatement = createBgpPolicyStatement(
        kAcceptPolicyName,
        {createBgpPolicyTerm(
            "accept-all",
            "Accept all routes",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    auto denyStatement = createBgpPolicyStatement(
        kDenyPolicyName,
        {createBgpPolicyTerm(
            "deny-all",
            "Deny all routes",
            {},
            {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::DENY)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    auto tagStatement = createBgpPolicyStatement(
        kTagPolicyName,
        {createBgpPolicyTerm(
            "tag-all",
            "Accept and tag with community",
            {},
            {createBgpPolicyCommunityAction(
                 bgp_policy::CommunityActionType::ADD, {kTagCommunity}),
             createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(acceptStatement);
    policies.bgp_policy_statements()->emplace_back(denyStatement);
    policies.bgp_policy_statements()->emplace_back(tagStatement);
    policies.bgp_policy_statements()->emplace_back(
        buildMatchNoAdvtDenyPolicy());
    policies.bgp_policy_statements()->emplace_back(
        buildMatchModifyAppendPolicy());
    policies.bgp_policy_statements()->emplace_back(buildPermitAllPolicy());
    setPolicyConfig(policies);
  }

  /*
   * Build RIB + PeerManager (update group + egress backpressure enabled) and a
   * BgpServiceBB sharing the same ConfigManager. Mirrors
   * setupSlowPeerComponents but adds the service layer for runtime policy RPCs.
   */
  void setupComponentsWithBgpService(
      int queueCapacity = 8,
      int queueHighWm = 6,
      int queueLowWm = 2,
      bool enableUpdateGroup = true) {
    setDefaultQueueSizes(queueCapacity, queueHighWm, queueLowWm);
    createRib();
    createPeerManager(
        enableUpdateGroup,
        /*enableEgressBackpressure=*/true,
        GetParam().enableSerializeGroupPdu);

    watchdog_ = std::make_unique<Watchdog>(config_);
    bgpService_ = std::make_unique<BgpServiceBB>(
        *peerManager_,
        configManager_,
        *rib_,
        *watchdog_,
        /*nlWrapper=*/nullptr,
        false);
  }

  /*
   * Add peers 3/4/5 into a shared peer group with NO per-peer egress policy
   * (so they share one update group and take the group-level re-eval path),
   * bring them up, consume dual-stack EoRs, and wait for JOINED_RUNNING.
   */
  PeerIds setupThreePeersInGroupJoined(
      int queueCapacity = 8,
      int queueHighWm = 6,
      int queueLowWm = 2,
      bool enableUpdateGroup = true) {
    auto spec3 = kDefaultPeerSpec3;
    auto spec4 = kDefaultPeerSpec4;
    auto spec5 = kDefaultPeerSpec5;
    spec3.peerGroupName = kReEvalPeerGroupName;
    spec4.peerGroupName = kReEvalPeerGroupName;
    spec5.peerGroupName = kReEvalPeerGroupName;
    addPeer(spec3);
    addPeer(spec4);
    addPeer(spec5);

    setupComponentsWithBgpService(
        queueCapacity, queueHighWm, queueLowWm, enableUpdateGroup);

    BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
    BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);
    sendEoRToPeer(peerId3);
    sendEoRToPeer(peerId4);
    sendEoRToPeer(peerId5);

    /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer). */
    EXPECT_TRUE(waitForEoR(peerId3));
    EXPECT_TRUE(waitForEoR(peerId3));
    EXPECT_TRUE(waitForEoR(peerId4));
    EXPECT_TRUE(waitForEoR(peerId4));
    EXPECT_TRUE(waitForEoR(peerId5));
    EXPECT_TRUE(waitForEoR(peerId5));

    EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
    EXPECT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
    EXPECT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

    return {peerId3, peerId4, peerId5};
  }

  /*
   * The pool of peer specs (kPeerAddr3..kPeerAddr12) shared by the single-group
   * and multi-group setup helpers. v6 nexthops only exist up to peer 9, so
   * peers 10-12 reuse kNextHopV6_3 (unused unless v6 routes are advertised).
   */
  static std::vector<BgpPeerSpec> allPeerSpecs() {
    return {
        kDefaultPeerSpec3,
        kDefaultPeerSpec4,
        kDefaultPeerSpec5,
        {kPeerAsn6, kLocalAddr6, kPeerAddr6, kNextHopV4_6, kNextHopV6_3},
        {kPeerAsn7, kLocalAddr7, kPeerAddr7, kNextHopV4_7, kNextHopV6_3},
        {kPeerAsn8, kLocalAddr8, kPeerAddr8, kNextHopV4_8, kNextHopV6_3},
        {kPeerAsn9, kLocalAddr9, kPeerAddr9, kNextHopV4_9, kNextHopV6_3},
        {kPeerAsn10, kLocalAddr10, kPeerAddr10, kNextHopV4_10, kNextHopV6_3},
        {kPeerAsn11, kLocalAddr11, kPeerAddr11, kNextHopV4_11, kNextHopV6_3},
        {kPeerAsn12, kLocalAddr12, kPeerAddr12, kNextHopV4_12, kNextHopV6_3},
    };
  }

  /*
   * Generate a distinct EBGP peer spec for index i (0-based). Peer i is
   * 127.(3+i).0.1, so peer 0 == kPeerAddr3 (the address the block/detach tests
   * target). Addresses/ASNs are synthesized rather than taken from the fixed
   * pool so the multi-group tests can scale past 10 peers. localAddr is shared
   * (unused for grouping). Peers are v4-only so each group negotiates a single
   * AFI and sends exactly one EoR.
   */
  static BgpPeerSpec makePeerSpec(int i) {
    BgpPeerSpec spec{};
    spec.asn = 64541 + i;
    spec.localAddr = kLocalAddr1;
    spec.peerAddr = folly::IPAddress(fmt::format("127.{}.0.1", 3 + i));
    spec.v4Nexthop = folly::IPAddress(fmt::format("127.5.0.{}", 1 + i));
    spec.v6Nexthop = kEmptyV6Nexthop;
    spec.disableIpv6Afi = true;
    return spec;
  }

  /*
   * Add numGroups peer groups of peersPerGroup peers each (specs synthesized
   * via makePeerSpec). Peers in group g are assigned peerGroupName
   * reEvalPeerGroupName(g); since peerGroupName is part of the update-group
   * key, each peer group forms its own update group. Brings all peers up,
   * consumes dual-stack EoRs, waits for JOINED_RUNNING, and returns the
   * BgpPeerIds grouped by update group (result[g] = peers of group g).
   */
  std::vector<std::vector<BgpPeerId>> setupPeersInGroups(
      int numGroups,
      int peersPerGroup,
      int queueCapacity = 8,
      int queueHighWm = 6,
      int queueLowWm = 2,
      bool enableUpdateGroup = true) {
    const int total = numGroups * peersPerGroup;
    /* peerAddr third octet is 3 + i, which must stay within a single byte. */
    EXPECT_LE(total, 250) << "setupPeersInGroups supports at most 250 peers";

    std::vector<folly::IPAddress> peerAddrs;
    std::vector<std::vector<BgpPeerId>> groups(numGroups);
    for (int i = 0; i < total; ++i) {
      auto spec = makePeerSpec(i);
      const int groupIdx = i / peersPerGroup;
      spec.peerGroupName = reEvalPeerGroupName(groupIdx);
      addPeer(spec);
      peerAddrs.push_back(spec.peerAddr);
      groups[groupIdx].push_back(
          BgpPeerId{spec.peerAddr, spec.peerAddr.asV4().toLongHBO()});
    }

    setupComponentsWithBgpService(
        queueCapacity, queueHighWm, queueLowWm, enableUpdateGroup);

    for (const auto& peerAddr : peerAddrs) {
      bringUpPeer(peerAddr);
    }
    for (const auto& group : groups) {
      for (const auto& peerId : group) {
        sendEoRToPeer(peerId);
      }
    }
    /* Peers are v4-only, so each sends exactly one EoR. */
    for (const auto& group : groups) {
      for (const auto& peerId : group) {
        EXPECT_TRUE(waitForEoR(peerId));
      }
    }
    for (const auto& peerAddr : peerAddrs) {
      EXPECT_TRUE(waitForPeerState(peerAddr, PeerUpdateState::JOINED_RUNNING));
    }
    return groups;
  }

  /*
   * Add numPeers (up to 10, kPeerAddr3..kPeerAddr12) into a shared peer group
   * with NO per-peer egress policy (so they form one update group and take the
   * group-level re-eval path), bring them up, consume dual-stack EoRs, and wait
   * for JOINED_RUNNING. Returns the peers' BgpPeerIds.
   *
   * v6 nexthops only exist up to peer 9, so peers 10-12 reuse kNextHopV6_3 (the
   * v6 nexthop is unused unless v6 routes are advertised).
   */
  std::vector<BgpPeerId> setupNPeersInGroupJoined(
      int numPeers,
      int queueCapacity = 8,
      int queueHighWm = 6,
      int queueLowWm = 2,
      bool enableUpdateGroup = true,
      bool waitForJoinedRunning = true) {
    const auto allSpecs = allPeerSpecs();
    EXPECT_LE(numPeers, static_cast<int>(allSpecs.size()))
        << "setupNPeersInGroupJoined supports at most " << allSpecs.size()
        << " peers";

    std::vector<folly::IPAddress> peerAddrs;
    std::vector<BgpPeerId> peerIds;
    for (int i = 0; i < numPeers; ++i) {
      auto spec = allSpecs[i];
      spec.peerGroupName = kReEvalPeerGroupName;
      addPeer(spec);
      peerAddrs.push_back(spec.peerAddr);
      peerIds.push_back(
          BgpPeerId{spec.peerAddr, spec.peerAddr.asV4().toLongHBO()});
    }

    setupComponentsWithBgpService(
        queueCapacity, queueHighWm, queueLowWm, enableUpdateGroup);

    for (const auto& peerAddr : peerAddrs) {
      bringUpPeer(peerAddr);
    }
    for (const auto& peerId : peerIds) {
      sendEoRToPeer(peerId);
    }
    /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer). */
    for (const auto& peerId : peerIds) {
      EXPECT_TRUE(waitForEoR(peerId));
      EXPECT_TRUE(waitForEoR(peerId));
    }
    /*
     * With the update group disabled, peers stay in the default DOWN state (the
     * JOINED_RUNNING state machine is update-group only), so callers on the
     * flag-off path pass waitForJoinedRunning=false.
     */
    if (waitForJoinedRunning) {
      for (const auto& peerAddr : peerAddrs) {
        EXPECT_TRUE(
            waitForPeerState(peerAddr, PeerUpdateState::JOINED_RUNNING));
      }
    }
    return peerIds;
  }

  /*
   * Bring a queue-blocked peer (JOINED_BLOCKED or DETACHED_BLOCKED) back to
   * JOINED_RUNNING: unblock and drain its queue in a loop while yielding to the
   * evb, with a final drain to clear any leftover change-list messages. Mirrors
   * the DSP rejoin drain-while-waiting pattern (P2320590232 Pattern 1).
   */
  void unblockAndDrainToJoined(
      const folly::IPAddress& peerAddr,
      const BgpPeerId& peerId) {
    /* Unblock without draining so recordDrainedRoutes captures every message.
     */
    unblockPeer(peerAddr, /*maxRetries=*/0);
    for (int i = 0; i < 20; ++i) {
      if (getPeerState(peerAddr) == PeerUpdateState::JOINED_RUNNING) {
        break;
      }
      recordDrainedRoutes(peerId, 1, 100);
      peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
      folly::futures::sleep(std::chrono::milliseconds(100)).get();
    }
    /* Final drain clears leftover CL messages before any post-rejoin verify. */
    recordDrainedRoutes(peerId, 1, 100);
    ASSERT_TRUE(waitForPeerState(peerAddr, PeerUpdateState::JOINED_RUNNING));
  }

  /*
   * Peer-group-level egress policy change (group-only re-evaluation path).
   */
  neteng::fboss::bgp::thrift::BgpPolicyChangeResult setPeerGroupPolicy(
      const std::string& peerGroupName,
      const std::string& policyName,
      bgp_policy::DIRECTION direction = bgp_policy::DIRECTION::OUT) {
    std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
        peerGroupsPolicy;
    peerGroupsPolicy[peerGroupName][direction] = policyName;

    return folly::coro::blockingWait(bgpService_->co_setPeerGroupsPolicy(
        std::make_unique<decltype(peerGroupsPolicy)>(
            std::move(peerGroupsPolicy))));
  }

  /*
   * Apply the same egress policy to MULTIPLE peer groups in a single
   * co_setPeerGroupsPolicy RPC -- re-evaluates each named update group, leaving
   * the others untouched.
   */
  neteng::fboss::bgp::thrift::BgpPolicyChangeResult setPeerGroupsPolicy(
      const std::vector<std::string>& peerGroupNames,
      const std::string& policyName,
      bgp_policy::DIRECTION direction = bgp_policy::DIRECTION::OUT) {
    std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
        peerGroupsPolicy;
    for (const auto& peerGroupName : peerGroupNames) {
      peerGroupsPolicy[peerGroupName][direction] = policyName;
    }
    return folly::coro::blockingWait(bgpService_->co_setPeerGroupsPolicy(
        std::make_unique<decltype(peerGroupsPolicy)>(
            std::move(peerGroupsPolicy))));
  }

  /*
   * Per-peer egress policy override (peer split/move path).
   */
  neteng::fboss::bgp::thrift::BgpPolicyChangeResult setPeerPolicy(
      const std::string& peerAddr,
      const std::string& policyName,
      bgp_policy::DIRECTION direction = bgp_policy::DIRECTION::OUT) {
    std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
        peersPolicy;
    peersPolicy[peerAddr][direction] = policyName;

    return folly::coro::blockingWait(bgpService_->co_setPeersPolicy(
        std::make_unique<decltype(peersPolicy)>(std::move(peersPolicy))));
  }

  /*
   * Remove a per-peer egress policy override.
   */
  neteng::fboss::bgp::thrift::BgpPolicyChangeResult unsetPeerPolicy(
      const std::string& peerAddr,
      bgp_policy::DIRECTION direction = bgp_policy::DIRECTION::OUT) {
    std::map<std::string, std::set<bgp_policy::DIRECTION>> peersToUnset;
    peersToUnset[peerAddr].insert(direction);

    return folly::coro::blockingWait(bgpService_->co_unsetPeersPolicy(
        std::make_unique<decltype(peersToUnset)>(std::move(peersToUnset))));
  }

  /*
   * Inject `count` /24 routes (36.0.i.0/24, i in 0..count-1) with per-route
   * ingress communities mirroring the UT: odd i carry kCommNoAdvt, i % 3 == 0
   * carry kCommModify (a route can carry both). The route index i is encoded in
   * the 3rd octet so verifyRibOutEntries' routeIndexPredicate sees it. Waits
   * for each route to reach the shadow RIB. When `injectedOut` is non-null, it
   * receives the injected prefixes (index order) so callers can assert exactly
   * which routes should have been advertised without re-deriving the format.
   */
  void injectCommunityTaggedRoutes(
      int count,
      std::vector<folly::CIDRNetwork>* injectedOut = nullptr) {
    /*
     * Group prefixes by their (kCommNoAdvt?, kCommModify?) community combo and
     * inject each combo as a single batch. injectLocalRoutesAtRuntime applies
     * one community set to the whole batch, so this is at most 4 RIB calls --
     * per-route injection is far too slow at this scale.
     */
    std::map<std::vector<std::string>, std::vector<std::string>>
        comboToPrefixes;
    for (int i = 0; i < count; ++i) {
      std::vector<std::string> communities;
      if (i % 2 == 1) {
        communities.push_back(kCommNoAdvt);
      }
      if (i % 3 == 0) {
        communities.push_back(kCommModify);
      }
      const auto prefix = fmt::format("36.0.{}.0/24", i);
      comboToPrefixes[communities].push_back(prefix);
      if (injectedOut) {
        injectedOut->push_back(folly::IPAddress::createNetwork(prefix));
      }
    }
    for (const auto& [communities, prefixes] : comboToPrefixes) {
      injectLocalRoutesAtRuntime(prefixes, communities, 150);
    }
    /* Barrier: wait for the highest-index route to reach the shadow RIB. */
    ASSERT_TRUE(waitForRouteInShadowRib(
        folly::IPAddress::createNetwork(
            fmt::format("36.0.{}.0/24", count - 1))))
        << "community-tagged routes did not reach shadow RIB";
  }

  /*
   * Poll until the peer's RIB-OUT advertises exactly `expectedCount` of the
   * community-tagged routes, letting the async group re-eval settle before a
   * deterministic per-route assertion.
   */
  void waitForRibOutAdvertisedCount(
      const folly::IPAddress& peerAddr,
      size_t expectedCount) {
    auto isAdvertised = [](const AdjRibEntry& entry,
                           const folly::CIDRNetwork&) {
      return entry.getPostAttr() != nullptr;
    };
    WITH_RETRIES_N(60, {
      EXPECT_EVENTUALLY_EQ(
          verifyRibOutEntries(
              peerAddr, [](int) { return true; }, isAdvertised),
          expectedCount);
    });
  }

  /*
   * Assert a peer's RIB-OUT matches what the egress policy should advertise
   * over the community-tagged routes (odd i carry kCommNoAdvt, i % 3 == 0 carry
   * kCommModify). Mirrors the UT expectRibOutForPolicy:
   *   - kMatchNoAdvtDenyPolicyName: even routes advertised, odd routes denied.
   *   - kMatchModifyAppendPolicyName: all advertised; i % 3 == 0 carry the
   *     appended kCommAppend.
   *   - kPermitAllPolicyName: all advertised, unmodified.
   * `count` is the number of injected routes (indices 0..count-1).
   */
  void expectRibOutForPolicy(
      const folly::IPAddress& peerAddr,
      const std::string& policyName,
      int count) {
    auto isEven = [](int i) { return i % 2 == 0; };
    auto isOdd = [](int i) { return i % 2 == 1; };
    auto isModify = [](int i) { return i % 3 == 0; };
    auto all = [](int) { return true; };
    size_t evens = 0, odds = 0, modifies = 0;
    for (int i = 0; i < count; ++i) {
      evens += isEven(i) ? 1 : 0;
      odds += isOdd(i) ? 1 : 0;
      modifies += isModify(i) ? 1 : 0;
    }

    if (policyName == kMatchNoAdvtDenyPolicyName) {
      EXPECT_EQ(
          verifyRibOutEntries(peerAddr, isEven, verifyAdvertised()), evens);
      EXPECT_EQ(
          verifyRibOutEntries(peerAddr, isOdd, verifyNotAdvertised()), odds);
    } else if (policyName == kMatchModifyAppendPolicyName) {
      EXPECT_EQ(
          verifyRibOutEntries(peerAddr, all, verifyAdvertised()),
          static_cast<size_t>(count));
      EXPECT_EQ(
          verifyRibOutEntries(
              peerAddr, isModify, verifyCommOnAdvertisedRoute(kCommAppend)),
          modifies);
    } else if (policyName == kPermitAllPolicyName) {
      EXPECT_EQ(
          verifyRibOutEntries(peerAddr, all, verifyAdvertised()),
          static_cast<size_t>(count));
    } else {
      FAIL() << "expectRibOutForPolicy: unknown policy " << policyName;
    }
  }

  /*
   * True if the deserialized path attributes carry the given "asn:value"
   * community.
   */
  static bool attrsHaveCommunity(
      const nettools::bgplib::BgpAttributes& attrs,
      const std::string& community) {
    const auto colon = community.find(':');
    const auto asn = folly::to<uint32_t>(community.substr(0, colon));
    const auto value = folly::to<uint32_t>(community.substr(colon + 1));
    for (const auto& comm : attrs.communities().value()) {
      if (static_cast<uint32_t>(comm.asn().value()) == asn &&
          static_cast<uint32_t>(comm.value().value()) == value) {
        return true;
      }
    }
    return false;
  }

  /* Route index i encoded in the 3rd octet of 36.0.i.0/24. */
  static int routeIndexFromPrefix(const folly::CIDRNetwork& prefix) {
    return (prefix.first.asV4().toLongHBO() >> 8) & 0xFF;
  }

  /*
   * Assert the routes a peer actually received (recorded via
   * recordDrainedRoutes) match what the egress policy should advertise over the
   * community-tagged routes -- the wire-level counterpart of
   * expectRibOutForPolicy:
   *   - kMatchNoAdvtDenyPolicyName: even prefixes present, odd prefixes absent
   *     (withdrawn).
   *   - kMatchModifyAppendPolicyName: all present; i % 3 == 0 carry
   * kCommAppend.
   *   - kPermitAllPolicyName: all present.
   */
  void expectReceivedRoutesForPolicy(
      const BgpPeerId& peerId,
      const std::string& policyName,
      const std::vector<folly::CIDRNetwork>& injectedPrefixes) {
    const auto& received = receivedRoutes(peerId);
    for (const auto& prefix : injectedPrefixes) {
      const int i = routeIndexFromPrefix(prefix);
      const auto it = received.find(prefix);
      const bool advertised = it != received.end();
      const auto prefixStr = folly::IPAddress::networkToString(prefix);
      if (policyName == kMatchNoAdvtDenyPolicyName) {
        if (i % 2 == 0) {
          EXPECT_TRUE(advertised)
              << "peer " << peerId.peerAddr.str() << " missing " << prefixStr;
        } else {
          EXPECT_FALSE(advertised) << "peer " << peerId.peerAddr.str()
                                   << " should have withdrawn " << prefixStr;
        }
      } else if (policyName == kMatchModifyAppendPolicyName) {
        EXPECT_TRUE(advertised)
            << "peer " << peerId.peerAddr.str() << " missing " << prefixStr;
        if (advertised && i % 3 == 0) {
          EXPECT_TRUE(attrsHaveCommunity(it->second, kCommAppend))
              << "peer " << peerId.peerAddr.str() << " " << prefixStr
              << " missing appended community " << kCommAppend;
        }
      } else if (policyName == kPermitAllPolicyName) {
        EXPECT_TRUE(advertised)
            << "peer " << peerId.peerAddr.str() << " missing " << prefixStr;
      } else {
        FAIL() << "expectReceivedRoutesForPolicy: unknown policy "
               << policyName;
      }
    }
  }

  /*
   * Withdraw the given prefixes at runtime. The withdrawals flow through the
   * shadow RIB onto the group's changelist, so the group's changelist consumer
   * processes them (unlike the initial dump, which sees only announcements).
   */
  void withdrawRoutes(const std::vector<folly::CIDRNetwork>& prefixes) {
    std::vector<std::string> prefixStrs;
    prefixStrs.reserve(prefixes.size());
    for (const auto& prefix : prefixes) {
      prefixStrs.push_back(folly::IPAddress::networkToString(prefix));
    }
    withdrawLocalRoutesAtRuntime(prefixStrs);
  }

  std::unique_ptr<Watchdog> watchdog_;
  std::unique_ptr<BgpServiceBB> bgpService_;
};

/* Fixture aliases per test group. */
class ConsumerOnChangelistWithOnlyAnnouncements
    : public UpdateGroupPolicyReEvalE2EBase {};

/*
 * Cases where the group's changelist carries announcements AND withdrawals:
 * some routes are withdrawn (and the group is confirmed to have processed at
 * least one changelist withdrawal) before the peer is blocked/detached, so the
 * re-evaluation runs against a changelist that has already seen withdrawals.
 */
class ConsumerOnChangelistWithAnnouncementsWithdrawals
    : public UpdateGroupPolicyReEvalE2EBase {};

/*
 * Multi-group cases: peers are spread across 4 peer groups (= 4 update groups)
 * and a single setPeerGroupsPolicy call operates on 2 of the 4 groups; the
 * other 2 must stay on their original (permit-all) policy.
 */
class MultiGroupPolicyReEval : public UpdateGroupPolicyReEvalE2EBase {};

} // namespace bgp
} // namespace facebook
