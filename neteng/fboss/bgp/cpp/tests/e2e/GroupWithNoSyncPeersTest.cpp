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
 * E2E tests: update group left with NO IN-SYNC PEERS, and the peer PROMOTION
 * that recovers it.
 *
 * unregisterPeer -> recoverIfNoSyncPeers() -> handleNoSyncPeers() clears the
 * packing list and cancels the consume timer (the group is "frozen"), then
 * tries to promote a detached member: DFP accepted directly, DSP via
 * tryAcceptPeersToGroup collapse; else stay frozen while a peer that detached
 * AFTER joining still shares the group's entries; else
 * promoteDetachedPeerToSync() on the first DEP-A, so its change-list position
 * and RIB-OUT become the group's truth; else stay frozen until a peer
 * self-promotes at the end of its own drain.
 *
 * Only the END STATE is asserted: every peer ends with every RIB route in its
 * RIB-OUT and on the wire, carrying the latest attributes. Each publish is a
 * numbered "round" that bumps LOCAL_PREF by 100 and stamps a matching community
 * (65501:100, ...); assertions key off the community because these peers are
 * eBGP, where LOCAL_PREF is not propagated. The decision tree is already unit
 * tested; what these add is end-to-end proof that a promoted peer becomes the
 * group's sync peer and every survivor converges. Prefix range 36.0.x.0/24.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupPolicyReEvalE2ECommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

class GroupWithNoSyncPeersE2ETest : public UpdateGroupPolicyReEvalE2EBase {
 protected:
  /*
   * Publish the whole route set again as the next "round", returning the round
   * community to verify against. LOCAL_PREF alone is invisible to these eBGP
   * peers, so bumping it produces no re-advertisement at all; the round
   * community both drives the re-advertisement and marks the round.
   */
  std::string publishNextRound(int count = kNumRoutes, int numAttrBuckets = 0) {
    nextLocalPref_ += 100;
    const auto roundComm = fmt::format("{}:{}", kRoundCommAsn, nextLocalPref_);
    /*
     * Per-route community combos, as in injectCommunityTaggedRoutes: they pack
     * the round into several attribute groups. A single uniform UPDATE would
     * never fill a tiny peer queue, so no peer would block or detach.
     */
    std::map<std::vector<std::string>, std::vector<std::string>>
        comboToPrefixes;
    injectedPrefixes_.clear();
    for (int i = 0; i < count; ++i) {
      std::vector<std::string> communities{roundComm};
      if (numAttrBuckets > 0) {
        /*
         * Spread the round over numAttrBuckets attribute groups instead of ~4,
         * so a test can block a peer PART WAY through a round.
         */
        communities.push_back(
            fmt::format("{}:{}", kBucketCommAsn, i % numAttrBuckets));
      } else {
        if (i % 2 == 1) {
          communities.push_back(kCommNoAdvt);
        }
        if (i % 3 == 0) {
          communities.push_back(kCommModify);
        }
      }
      const auto prefix = fmt::format("36.0.{}.0/24", i);
      comboToPrefixes[communities].push_back(prefix);
      injectedPrefixes_.push_back(folly::IPAddress::createNetwork(prefix));
    }
    for (const auto& [communities, prefixes] : comboToPrefixes) {
      injectLocalRoutesAtRuntime(prefixes, communities, nextLocalPref_);
    }
    EXPECT_TRUE(waitForRouteInShadowRib(injectedPrefixes_.back()))
        << "published routes did not reach the shadow RIB";
    return roundComm;
  }

  /*
   * RIB-OUT predicate: entry advertised and carrying the given community. Not
   * verifyCommOnAdvertisedRoute, which EXPECT_TRUEs on every non-matching entry
   * and so records a failure per prefix per retry inside a polling loop.
   */
  static std::function<bool(const AdjRibEntry&, const folly::CIDRNetwork&)>
  advertisedWithCommunity(const std::string& community) {
    const auto expected =
        *nettools::bgplib::BgpAttrCommunityC::createBgpAttrCommunity(community);
    return [expected](
               const AdjRibEntry& entry, const folly::CIDRNetwork&) -> bool {
      const auto postAttr = entry.getPostAttr();
      if (!postAttr) {
        return false;
      }
      for (const auto& comm : postAttr->getCommunities().get()) {
        if (comm == expected) {
          return true;
        }
      }
      return false;
    };
  }

  /*
   * Poll until the peer's RIB-OUT advertises every injected prefix on the given
   * round. Promotion and rejoin are async, so the end state is awaited.
   */
  void expectRibOutConverged(
      const folly::IPAddress& peerAddr,
      const std::string& roundComm) {
    const auto expectedCount = injectedPrefixes_.size();
    /* Short gaps: WITH_RETRIES_N's 1s default would blow the test timeout at
     * this peer count whenever a peer fails to converge. */
    WITH_RETRIES_N_TIMED(40, std::chrono::milliseconds(100), {
      EXPECT_EVENTUALLY_EQ(
          verifyRibOutEntries(
              peerAddr,
              [](int) { return true; },
              advertisedWithCommunity(roundComm)),
          expectedCount);
    });
  }

  /* Assert the peer received every injected prefix on the wire, on the given
   * round (i.e. it is not left on a stale one). */
  void expectReceivedRoutesConverged(
      const BgpPeerId& peerId,
      const std::string& roundComm) {
    const auto& received = receivedRoutes(peerId);
    for (const auto& prefix : injectedPrefixes_) {
      const auto prefixStr = folly::IPAddress::networkToString(prefix);
      const auto it = received.find(prefix);
      if (it == received.end()) {
        ADD_FAILURE() << "peer " << peerId.peerAddr.str() << " never received "
                      << prefixStr;
        continue;
      }
      EXPECT_TRUE(attrsHaveCommunity(it->second, roundComm))
          << "peer " << peerId.peerAddr.str() << " " << prefixStr
          << " is on a stale round (missing community " << roundComm << ")";
    }
  }

  /* Non-asserting RIB-OUT check, for polling a convergence loop. */
  bool ribOutMatchesRound(
      const folly::IPAddress& peerAddr,
      const std::string& roundComm) {
    return verifyRibOutEntries(
               peerAddr,
               [](int) { return true; },
               advertisedWithCommunity(roundComm)) == injectedPrefixes_.size();
  }

  /*
   * Drain each peer repeatedly while waiting for JOINED_RUNNING, then assert
   * RIB-OUT and wire convergence. A catching-up peer must drain before it can
   * rejoin, and a DEP-A is served by its OWN dump, so deliveries land at any
   * point during recovery and must be recorded rather than discarded.
   */
  void expectAllPeersConverged(
      const std::vector<BgpPeerId>& peerIds,
      const std::string& roundComm) {
    drainUntil(
        peerIds,
        [&]() {
          for (const auto& peerId : peerIds) {
            if (getPeerState(peerId.peerAddr) !=
                    PeerUpdateState::JOINED_RUNNING ||
                !ribOutMatchesRound(peerId.peerAddr, roundComm)) {
              return false;
            }
          }
          return true;
        },
        "peers never converged on the expected round",
        60);
    /* Final drain so late deliveries are recorded before verifying. */
    for (const auto& peerId : peerIds) {
      recordDrainedRoutes(peerId);
    }
    for (const auto& peerId : peerIds) {
      EXPECT_TRUE(
          waitForPeerState(peerId.peerAddr, PeerUpdateState::JOINED_RUNNING))
          << "peer " << peerId.peerAddr.str()
          << " did not reach JOINED_RUNNING";
      EXPECT_TRUE(isPeerInSync(peerId.peerAddr))
          << "peer " << peerId.peerAddr.str() << " is not in sync";
      expectRibOutConverged(peerId.peerAddr, roundComm);
      expectReceivedRoutesConverged(peerId, roundComm);
    }
  }

  /* In-sync peer count of the update group the peer belongs to. */
  size_t getNumInSyncPeers(const folly::IPAddress& peerAddr) {
    auto group = getUpdateGroupForPeer(peerAddr);
    return group ? group->getNumInSyncPeers() : 0;
  }

  /* The group's cached change-list position. */
  uint64_t getGroupRibVersion(const folly::IPAddress& peerAddr) {
    auto group = getUpdateGroupForPeer(peerAddr);
    return group ? group->getLastSeenRibVersion() : 0;
  }

  /* Bring a set of peers down, one at a time. */
  void bringDownPeers(const std::vector<BgpPeerId>& peerIds) {
    for (const auto& peerId : peerIds) {
      bringDownPeer(peerId.peerAddr);
    }
  }

  /*
   * Detach a blocked peer via the production slow-peer path without waiting for
   * the group-wide threshold. detachSlowPeer is the same call the detection
   * makes; this just lets the test choose WHICH blocked peer detaches.
   */
  void detachBlockedPeerDirectly(const folly::IPAddress& peerAddr) {
    auto group = getUpdateGroupForPeer(peerAddr);
    ASSERT_NE(group, nullptr);
    auto adjRib = getAdjRibByAddr(peerAddr);
    ASSERT_NE(adjRib, nullptr);
    peerManager_->getEventBase().runInEventBaseThreadAndWait(
        [&group, &adjRib]() { group->detachSlowPeer(adjRib); });
  }

  /* True when every listed peer is in the given state. */
  bool allPeersInState(
      const std::vector<BgpPeerId>& peerIds,
      PeerUpdateState state) {
    for (const auto& peerId : peerIds) {
      if (getPeerState(peerId.peerAddr) != state) {
        return false;
      }
    }
    return true;
  }

  /* True if the peer is flagged IS_DETACHED_FAST_PEER: it finished draining
   * before the group moved, so it can rejoin without collapse. */
  bool isDetachedFastPeer(const folly::IPAddress& peerAddr) {
    auto& evb = peerManager_->getEventBase();
    return folly::via(
               &evb,
               [this, peerAddr]() -> bool {
                 auto adjRib = getAdjRibByAddr(peerAddr);
                 return adjRib &&
                     adjRib->isAdjRibFlagSet(AdjRib::IS_DETACHED_FAST_PEER);
               })
        .get();
  }

  /*
   * True if the peer registered into an ALREADY INITIALIZED group.
   * AdjRibGroup::registerPeer only sets DETACHED_ON_REGISTRATION when the group
   * is not UNINITIALIZED; that plus a zero detach version makes a peer a DEP-A.
   */
  bool isDetachedOnRegistration(const folly::IPAddress& peerAddr) {
    auto& evb = peerManager_->getEventBase();
    return folly::via(
               &evb,
               [this, peerAddr]() -> bool {
                 auto adjRib = getAdjRibByAddr(peerAddr);
                 return adjRib &&
                     adjRib->isAdjRibFlagSet(AdjRib::DETACHED_ON_REGISTRATION);
               })
        .get();
  }

  /* The peer's frozen detach version; 0 for a peer that never detached. */
  uint64_t getPeerDetachedRibVersion(const folly::IPAddress& peerAddr) {
    auto& evb = peerManager_->getEventBase();
    return folly::via(
               &evb,
               [this, peerAddr]() -> uint64_t {
                 auto adjRib = getAdjRibByAddr(peerAddr);
                 return adjRib ? adjRib->getDetachedRibVersion() : 0;
               })
        .get();
  }

  /* The peer's own change-list position (the group's, if it is in sync). */
  uint64_t getPeerLastSeenRibVersion(const folly::IPAddress& peerAddr) {
    auto& evb = peerManager_->getEventBase();
    return folly::via(
               &evb,
               [this, peerAddr]() -> uint64_t {
                 auto adjRib = getAdjRibByAddr(peerAddr);
                 return adjRib ? adjRib->getLastSeenRibVersion() : 0;
               })
        .get();
  }

  /*
   * Assert every listed peer sits strictly AHEAD of the group on the change
   * list. transitionPeerUpdateState parks an ahead-of-group peer and a
   * caught-up DSP in the SAME state, and adopting a peer's position in
   * promoteDetachedPeerToSync is vacuous if that peer is merely level.
   */
  void expectAllAheadOfGroup(const std::vector<BgpPeerId>& peerIds) {
    const auto groupVersion = getGroupRibVersion(peerIds[0].peerAddr);
    for (const auto& peerId : peerIds) {
      EXPECT_GT(getPeerLastSeenRibVersion(peerId.peerAddr), groupVersion)
          << "peer " << peerId.peerAddr.str()
          << " is not ahead of the group (group at " << groupVersion
          << "), so it took the DSP branch rather than the DEP-A one";
    }
  }

  /* The group's count of members that detached AFTER joining (DEP-B). */
  size_t getNumPeersDetachedAfterJoin(const folly::IPAddress& peerAddr) {
    auto group = getUpdateGroupForPeer(peerAddr);
    return group ? group->getNumPeersDetachedAfterJoin() : 0;
  }

  /*
   * Assert every listed peer really is a DEP-A. State cannot answer this -- a
   * DEP-A and a DSP both park in DETACHED_READY_TO_JOIN -- so check the
   * registration flag and zero detach version. numPeersDetachedAfterJoin_ must
   * be 0 too: handleNoSyncPeers checks it before promoting at all.
   */
  void expectAllDepA(const std::vector<BgpPeerId>& peerIds) {
    for (const auto& peerId : peerIds) {
      EXPECT_TRUE(isDetachedOnRegistration(peerId.peerAddr))
          << "peer " << peerId.peerAddr.str()
          << " is not DETACHED_ON_REGISTRATION, so it is not a DEP-A";
      EXPECT_EQ(getPeerDetachedRibVersion(peerId.peerAddr), 0u)
          << "peer " << peerId.peerAddr.str()
          << " has a non-zero detach version, so it detached after joining";
    }
    EXPECT_EQ(getNumPeersDetachedAfterJoin(peerIds[0].peerAddr), 0u)
        << "a member detached after joining, so handleNoSyncPeers will stay "
           "frozen instead of taking the DEP-A promotion branch";
  }

  /*
   * Leave the group with one JOINED_BLOCKED member and every other member
   * re-registered as a DEP-A.
   *
   * Bringing a peer up "late" is not enough: where its own dump lands decides
   * the branch transitionPeerUpdateState takes -- level with the group makes it
   * a DSP, past the group a DEP-A. Flapping EXISTING members of a stalled group
   * removes that ambiguity. All members are blocked first so the group's send
   * (hence its change-list consumption) is suspended; the peers to convert are
   * dropped, extra rounds run the RIB past the group, and they come back as
   * DEP-A. stallPeer stays blocked as the group's only sync peer.
   */
  void flapPeersIntoDepA(
      const BgpPeerId& stallPeer,
      const std::vector<BgpPeerId>& toFlap,
      const std::vector<BgpPeerId>& returnBlocked,
      int extraRounds,
      std::string* roundCommOut,
      const std::vector<BgpPeerId>& drainDuring = {},
      bool singleEoR = false) {
    std::vector<BgpPeerId> allMembers{stallPeer};
    allMembers.insert(allMembers.end(), toFlap.begin(), toFlap.end());

    /*
     * Slow-peer detection off for every member: one that auto-detaches while
     * blocked detaches AFTER joining, making it a DEP-B and leaving
     * numPeersDetachedAfterJoin_ non-zero, which blocks all promotion.
     */
    for (const auto& peerId : allMembers) {
      setSlowPeerThresholds(
          peerId.peerAddr,
          std::chrono::milliseconds(600000),
          1000000,
          std::chrono::milliseconds(600000));
      blockPeer(peerId.peerAddr);
    }

    /* One round is enough to fill every (tiny) queue and suspend the send. */
    *roundCommOut = publishNextRound();
    for (const auto& peerId : allMembers) {
      ASSERT_TRUE(waitForPeerQueueBlocked(peerId))
          << "member " << peerId.peerAddr.str() << " never filled its queue";
    }
    ASSERT_TRUE(
        waitForPeerState(stallPeer.peerAddr, PeerUpdateState::JOINED_BLOCKED));
    const auto stalledVersion = getGroupRibVersion(stallPeer.peerAddr);

    /* Drop the members that are to come back as DEP-A. */
    for (const auto& peerId : toFlap) {
      bringDownPeer(peerId.peerAddr);
    }

    /* Run the RIB past the group: it cannot consume these rounds while its
     * send is suspended on stallPeer. */
    for (int i = 0; i < extraRounds; ++i) {
      *roundCommOut = publishNextRound();
      drainAllFast(drainDuring);
    }
    drainAll(drainDuring);
    peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
    /*
     * Barrier: publishNextRound only waits for the last prefix to reach the
     * shadow RIB, and the RIB VERSION bump can still be in flight behind it. A
     * peer dumping before it lands snapshots the version the group is stalled
     * at, and comes back level with the group instead of ahead of it.
     */
    WITH_RETRIES_N(10, {
      EXPECT_EVENTUALLY_TRUE(rib_->getRibVersion() > stalledVersion);
    });
    ASSERT_EQ(getGroupRibVersion(stallPeer.peerAddr), stalledVersion)
        << "group kept consuming the change list; the peers coming back will "
           "not be ahead of it and will not be DEP-A";

    /* Bring them back: each re-registers into the running, stalled group. */
    auto comesBackBlocked = [&returnBlocked](const folly::IPAddress& addr) {
      for (const auto& peerId : returnBlocked) {
        if (peerId.peerAddr == addr) {
          return true;
        }
      }
      return false;
    };
    for (const auto& peerId : toFlap) {
      if (comesBackBlocked(peerId.peerAddr)) {
        bringUpPeerBlockedLate(peerId);
        continue;
      }
      /* Clear the block left over from the stall before it dumps again. */
      unblockPeer(peerId.peerAddr, /*maxRetries=*/0);
      if (singleEoR) {
        bringUpLatePeerSingleEoR(peerId);
      } else {
        bringUpLatePeer(peerId);
      }
    }
  }

  /*
   * Drain the given peers repeatedly until `done` holds, failing if it never
   * does. A catching-up peer must drain before it can change state, and its
   * deliveries must be recorded rather than discarded.
   */
  void drainUntil(
      const std::vector<BgpPeerId>& peersToDrain,
      const std::function<bool()>& done,
      const std::string& what,
      int maxRetries = 60) {
    WITH_RETRIES_N_TIMED(maxRetries, std::chrono::milliseconds(100), {
      drainAllFast(peersToDrain);
      peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
      EXPECT_EVENTUALLY_TRUE(done()) << what;
    });
  }

  /* Drain every peer once, recording deliveries. */
  void drainAll(const std::vector<BgpPeerId>& peerIds) {
    for (const auto& peerId : peerIds) {
      recordDrainedRoutes(peerId);
    }
  }

  /*
   * Drain-and-record with a minimal idle budget, for use INSIDE polling loops.
   * recordDrainedRoutes' default 20 idle retries at 50ms cost a second per
   * already-empty peer. The loop retries anyway, so one is enough; drainAll is
   * still used for the final drain before assertions.
   */
  void drainAllFast(const std::vector<BgpPeerId>& peerIds) {
    for (const auto& peerId : peerIds) {
      recordDrainedRoutes(peerId, /*idleRetries=*/1, /*maxMessages=*/200);
    }
  }

  /*
   * Detach `victims` via frequency-based slow-peer detection (block-count
   * threshold 1). They detach AFTER joining, so they are DEP-B (counted in
   * numPeersDetachedAfterJoin_) and the group will not promote past them.
   */
  void detachPeersBlocked(
      const std::vector<BgpPeerId>& victims,
      const std::vector<BgpPeerId>& others,
      std::string* roundCommOut,
      int numAttrBuckets = 0) {
    for (const auto& victim : victims) {
      setSlowPeerThresholds(
          victim.peerAddr,
          std::chrono::milliseconds(600000),
          1,
          std::chrono::milliseconds(60000));
      blockPeer(victim.peerAddr);
    }
    *roundCommOut = publishNextRound(kNumRoutes, numAttrBuckets);
    drainAll(others);
    for (const auto& victim : victims) {
      EXPECT_TRUE(waitForPeerQueueBlocked(victim));
      ASSERT_TRUE(
          waitForPeerState(victim.peerAddr, PeerUpdateState::DETACHED_BLOCKED));
      ASSERT_TRUE(isPeerDetached(victim.peerAddr));
    }
  }

  /*
   * Late-join a peer with its queue blocked from the start, so its own initial
   * dump cannot drain and it stays detached (never becomes promotable).
   */
  void bringUpPeerBlockedLate(const BgpPeerId& peerId) {
    bringUpPeerBlocked(peerId.peerAddr);
    sendEoRToPeer(peerId);
    /*
     * Deliberately no waitForEoR here: it reads from the peer's queue, which
     * would drain the very initial dump this peer is meant to be stalled on and
     * let it rejoin instead of staying detached.
     */
    peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
  }

  /*
   * Late-join a v4-only peer (makePeerSpec, used by the multi-group cases):
   * a single AFI is negotiated, so exactly one EoR is expected.
   */
  void bringUpLatePeerSingleEoR(const BgpPeerId& peerId) {
    bringUpPeer(peerId.peerAddr);
    sendEoRToPeer(peerId);
    EXPECT_TRUE(waitForEoR(peerId));
  }

  /* Bring up a peer after the group is initialized. */
  void bringUpLatePeer(const BgpPeerId& peerId) {
    bringUpPeer(peerId.peerAddr);
    sendEoRToPeer(peerId);
    EXPECT_TRUE(waitForEoR(peerId));
    EXPECT_TRUE(waitForEoR(peerId));
  }

  /* Unblock without discarding: deliveries are recorded, not dropped. */
  void unblockPeersRecording(const std::vector<BgpPeerId>& peerIds) {
    for (const auto& peerId : peerIds) {
      unblockPeer(peerId.peerAddr, /*maxRetries=*/0);
    }
  }

  /* ASN used for the per-round marker community (65501:<localPref>). */
  static constexpr int kRoundCommAsn = 65501;
  /* ASN used to split a round across extra attribute groups (see
   * publishNextRound's numAttrBuckets). */
  static constexpr int kBucketCommAsn = 65502;

  uint32_t nextLocalPref_{0};
  std::vector<folly::CIDRNetwork> injectedPrefixes_;
};

/*
 * A3: last sync peers down with only DEP-B (slow-peer detached) members left.
 *
 * 5 of 10 peers are slow-peer detached and the other 5 brought DOWN. Because
 * the detached ones detached AFTER joining (numPeersDetachedAfterJoin_ > 0),
 * handleNoSyncPeers deliberately promotes nobody and the group freezes; once
 * they are unblocked and drain, exactly one is promoted and the rest rejoin.
 */
TEST_P(GroupWithNoSyncPeersE2ETest, LastSyncPeersDownDetachedBlockedPromotion) {
  XLOGF(INFO, "=== TEST: LastSyncPeersDownDetachedBlockedPromotion ===");

  setupPolicies();
  /* Tiny queues on the future detached peers so they block under load. */
  const std::vector<int> victimIdx = {0, 1, 2, 3, 4};
  const auto allSpecs = allPeerSpecs();
  for (const auto idx : victimIdx) {
    setQueueSizeForPeer(
        allSpecs[idx].peerAddr, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  }
  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers, /*queueCapacity=*/10, /*queueHighWm=*/8, /*queueLowWm=*/2);
  ASSERT_EQ(peerIds.size(), static_cast<size_t>(kNumPeers));

  std::vector<BgpPeerId> detachedPeers(peerIds.begin(), peerIds.begin() + 5);
  std::vector<BgpPeerId> syncPeers(peerIds.begin() + 5, peerIds.end());

  auto group = getUpdateGroupForPeer(peerIds[0].peerAddr);
  ASSERT_NE(group, nullptr);
  EXPECT_EQ(getGroupMemberCount(peerIds[0].peerAddr), kNumPeers);

  /* Baseline route round; everyone is in sync and receives it. */
  publishNextRound();
  drainAll(peerIds);

  /*
   * Detach the 5 victims (DEP-B, DETACHED_BLOCKED). This publishes the round
   * that fills their queues, so it is the latest round the group has seen.
   */
  std::string detachRoundComm;
  detachPeersBlocked(detachedPeers, syncPeers, &detachRoundComm);
  EXPECT_EQ(getNumInSyncPeers(peerIds[0].peerAddr), syncPeers.size());

  /*
   * Take the 5 in-sync peers down so the group freezes. Nothing is published
   * while frozen: the group cannot advance its marker, so a round now would
   * park the detached peers "ahead of group" -- a different failure mode.
   */
  drainAll(syncPeers);
  bringDownPeers(syncPeers);

  EXPECT_EQ(getNumInSyncPeers(detachedPeers[0].peerAddr), 0u);
  EXPECT_EQ(
      getGroupMemberCount(detachedPeers[0].peerAddr), detachedPeers.size());

  /*
   * Unblock the detached peers: they drain, catch up to the frozen group's
   * marker, and exactly one is promoted to SYNC while the rest rejoin through
   * the normal collapse path.
   */
  unblockPeersRecording(detachedPeers);
  expectAllPeersConverged(detachedPeers, detachRoundComm);

  EXPECT_EQ(getNumInSyncPeers(detachedPeers[0].peerAddr), detachedPeers.size());

  /*
   * The group is serving again: a fresh round published after recovery must
   * reach every surviving peer.
   */
  const auto postRecoveryRoundComm = publishNextRound();
  expectAllPeersConverged(detachedPeers, postRecoveryRoundComm);

  XLOGF(INFO, "=== TEST PASSED: LastSyncPeersDownDetachedBlockedPromotion ===");
}

/*
 * A1: two DETACHED FAST PEERS are accepted straight back into a group that has
 * no in-sync peers left, with no collapse needed.
 *
 * isDFP() requires the peer's packing list drained, its detachedRibVersion ==
 * its own == the GROUP's lastSeenRibVersion, and the group's packing list
 * NON-empty. A third peer is therefore held at JOINED_BLOCKED throughout: that
 * keeps the group's send suspended, which both keeps the packing list non-empty
 * and pins the group's marker while the candidates drain.
 */
TEST_P(GroupWithNoSyncPeersE2ETest, DfpPeersAcceptedIntoGroupWithNoSyncPeers) {
  XLOGF(INFO, "=== TEST: DfpPeersAcceptedIntoGroupWithNoSyncPeers ===");

  setupPolicies();
  const auto allSpecs = allPeerSpecs();
  /* Peers 0,1 are the DFP candidates. */
  for (int idx = 0; idx < 2; ++idx) {
    setQueueSizeForPeer(
        allSpecs[idx].peerAddr, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  }
  /*
   * Peer 2 is the packing-list holder. Its queue is deliberately DEEPER than
   * the candidates': they must block in the first messages of the round, while
   * the holder crosses its watermark later in the SAME round.
   */
  setQueueSizeForPeer(
      allSpecs[2].peerAddr, /*capacity=*/20, /*highWm=*/15, /*lowWm=*/0);
  /*
   * Bystanders get a deep default queue: the detach round below is split
   * across 20 attribute groups, and 20 UPDATEs overflow the usual cap-10 queue,
   * which would detach the bystanders along with the candidates.
   */
  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers, /*queueCapacity=*/100, /*queueHighWm=*/80, /*queueLowWm=*/2);
  std::vector<BgpPeerId> dfpPeers(peerIds.begin(), peerIds.begin() + 2);
  const auto& holder = peerIds[2];
  std::vector<BgpPeerId> otherSyncPeers(peerIds.begin() + 2, peerIds.end());

  publishNextRound();
  drainAll(peerIds);

  std::vector<BgpPeerId> drainablePeers(peerIds.begin() + 3, peerIds.end());

  /*
   * Slow-peer auto-detach stays disabled throughout: the block-count threshold
   * lives on the GROUP, so any value low enough to detach the candidates also
   * detaches the holder. The candidates are detached explicitly instead.
   */
  setSlowPeerThresholds(
      holder.peerAddr,
      std::chrono::milliseconds(600000),
      1000000,
      std::chrono::milliseconds(600000));

  /*
   * Block the candidates and the holder, then publish ONE round split across
   * 20 attribute groups: the candidates block early, the holder later in the
   * same round, leaving the packing list non-empty and the marker pinned --
   * both isDFP() preconditions.
   */
  blockPeer(holder.peerAddr);
  for (const auto& peerId : dfpPeers) {
    blockPeer(peerId.peerAddr);
  }
  const auto roundComm = publishNextRound(kNumRoutes, /*numAttrBuckets=*/20);

  /*
   * The send suspends ON the candidates, so the holder cannot fill until they
   * leave the in-sync set: wait for them, detach them, then wait for the
   * holder. Wait on the QUEUE, not the state, which flips only on a deferral.
   */
  for (const auto& peerId : dfpPeers) {
    ASSERT_TRUE(waitForPeerQueueBlocked(peerId))
        << "DFP candidate " << peerId.peerAddr.str()
        << " never filled its queue";
  }

  /* Detach only the candidates; this also un-suspends the group's send. */
  for (const auto& peerId : dfpPeers) {
    detachBlockedPeerDirectly(peerId.peerAddr);
  }
  for (const auto& peerId : dfpPeers) {
    ASSERT_TRUE(
        waitForPeerState(peerId.peerAddr, PeerUpdateState::DETACHED_BLOCKED));
  }

  /* With the candidates gone the group distributes the REST of the same round,
   * which is what finally blocks the holder. */
  drainUntil(
      drainablePeers,
      [&]() {
        return getPeerState(holder.peerAddr) == PeerUpdateState::JOINED_BLOCKED;
      },
      "holder never blocked on the remainder of the detach round",
      40);
  ASSERT_FALSE(isPeerDetached(holder.peerAddr))
      << "the holder must stay in sync so it keeps the group's packing list "
         "non-empty";

  /*
   * NOTHING IS PUBLISHED from here on: consuming a new round would move the
   * candidates' lastSeenRibVersion past their detachedRibVersion, failing
   * isDFP() and silently sending them down the DSP collapse path.
   */

  /*
   * DRJ acceptance is deliberately NOT deferred here: DFPs are merged by
   * checkAndAcceptReadyToJoinPeers off the send guard, so deferring would make
   * that pass skip both peers. The group is still suspended on the holder, so
   * neither marker moves while the candidates drain into DFPs.
   */
  unblockPeersRecording(dfpPeers);
  drainUntil(
      dfpPeers,
      [&]() {
        return allPeersInState(
            dfpPeers, PeerUpdateState::DETACHED_READY_TO_JOIN);
      },
      "peers never parked at DETACHED_READY_TO_JOIN",
      40);
  for (const auto& peerId : dfpPeers) {
    ASSERT_TRUE(waitForPeerState(
        peerId.peerAddr, PeerUpdateState::DETACHED_READY_TO_JOIN));
    /*
     * The whole point of this case: these must be DFPs, not DSPs. If the group
     * or the peers moved after the detach they would silently take the DSP
     * collapse path instead and this test would duplicate the DSP one.
     */
    ASSERT_TRUE(isDetachedFastPeer(peerId.peerAddr))
        << "peer " << peerId.peerAddr.str()
        << " parked at DRJ as a DSP, not a DFP -- the group or the peer moved "
           "after the detach";
  }

  /*
   * Take every in-sync peer down, holder LAST and still blocked. Tearing it
   * down fails its deferred push and lets the suspended send finish, and the
   * exit guard then runs checkAndAcceptReadyToJoinPeers and merges the DFPs --
   * handleNoSyncPeers does not merge DFPs.
   */
  drainAll(drainablePeers);
  bringDownPeers(drainablePeers);
  ASSERT_TRUE(
      allPeersInState(dfpPeers, PeerUpdateState::DETACHED_READY_TO_JOIN))
      << "the DFPs must still be detached when the last sync peer goes down";
  bringDownPeer(holder.peerAddr);
  EXPECT_EQ(getGroupMemberCount(dfpPeers[0].peerAddr), dfpPeers.size());

  /*
   * Both DFPs are back in sync, accepted DIRECTLY with no collapse, from
   * checkAndAcceptReadyToJoinPeers as the group's send completes.
   */
  EXPECT_EQ(getNumInSyncPeers(dfpPeers[0].peerAddr), dfpPeers.size())
      << "the DFPs were not accepted back when the last sync peer went down";
  for (const auto& peerId : dfpPeers) {
    EXPECT_TRUE(
        waitForPeerState(peerId.peerAddr, PeerUpdateState::JOINED_RUNNING))
        << "peer " << peerId.peerAddr.str() << " was not accepted back";
  }

  /* The recovered group serves a fresh round to both. */
  const auto postRecoveryRoundComm = publishNextRound();
  expectAllPeersConverged(dfpPeers, postRecoveryRoundComm);

  XLOGF(INFO, "=== TEST PASSED: DfpPeersAcceptedIntoGroupWithNoSyncPeers ===");
}

/*
 * A4 shared shape: leave the group with 9 DEP-A members and no sync peer.
 *
 * All 10 peers join, then all are blocked so the group's send -- and its
 * change-list consumption -- is suspended. 9 are FLAPPED while extra rounds run
 * the RIB past the group's frozen marker, so each re-registers as a DEP-A ahead
 * of the group. Peer 0 stays JOINED_BLOCKED as the last sync peer; bringing it
 * DOWN leaves zero in-sync peers with numPeersDetachedAfterJoin_ == 0, the one
 * situation where handleNoSyncPeers promotes a DEP-A. See flapPeersIntoDepA for
 * why the flap is what makes that composition deterministic.
 */
TEST_P(GroupWithNoSyncPeersE2ETest, DepAPromotedWhenLastSyncPeerDown) {
  XLOGF(INFO, "=== TEST: DepAPromotedWhenLastSyncPeerDown ===");

  setupPolicies();
  const auto allSpecs = allPeerSpecs();
  /* Every member must be able to fill its queue and sit at JOINED_BLOCKED. */
  for (int i = 0; i < kNumPeers; ++i) {
    setQueueSizeForPeer(
        allSpecs[i].peerAddr, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  }

  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers,
      /*queueCapacity=*/10,
      /*queueHighWm=*/8,
      /*queueLowWm=*/2);
  const auto& blockedSyncPeer = peerIds[0];

  publishNextRound();
  drainAll(peerIds);

  /* Flap the other 9 into DEP-A. Peer 9 comes back blocked and non-promotable,
   * so the promotion must pick a READY peer; it rejoins later. */
  std::vector<BgpPeerId> lateJoiners(peerIds.begin() + 1, peerIds.end());
  std::vector<BgpPeerId> readyLateJoiners(
      peerIds.begin() + 1, peerIds.begin() + 9);
  std::string roundComm;
  flapPeersIntoDepA(
      blockedSyncPeer,
      lateJoiners,
      /*returnBlocked=*/{peerIds[9]},
      /*extraRounds=*/2,
      &roundComm);
  expectAllDepA(lateJoiners);

  /* Let the 8 unblocked joiners finish their own dumps and park at DRJ. */
  drainUntil(
      readyLateJoiners,
      [&]() {
        return allPeersInState(
            readyLateJoiners, PeerUpdateState::DETACHED_READY_TO_JOIN);
      },
      "peers never parked at DETACHED_READY_TO_JOIN",
      40);
  expectAllAheadOfGroup(readyLateJoiners);

  /*
   * Hold the ordinary rejoin path. Bringing the blocked peer down unwinds the
   * group's suspended send, whose exit guard runs
   * checkAndAcceptReadyToJoinPeers and would accept these peers before
   * recoverIfNoSyncPeers ever sees zero sync peers. That pass skips peers with
   * DRJ acceptance deferred; handleNoSyncPeers' DEP-A branch does not.
   */
  for (const auto& peerId : readyLateJoiners) {
    testOnlyDeferDrjAcceptance(peerId.peerAddr, true);
  }

  /* Last sync peer down: one ready DEP-A is promoted in place. */
  bringDownPeer(blockedSyncPeer.peerAddr);
  EXPECT_EQ(getGroupMemberCount(peerIds[1].peerAddr), lateJoiners.size());
  EXPECT_EQ(getNumInSyncPeers(peerIds[1].peerAddr), 1u)
      << "exactly one DEP-A should have been promoted to carry the group";

  /* Release the hold and unblock the 9th: everyone rejoins. */
  for (const auto& peerId : readyLateJoiners) {
    testOnlyDeferDrjAcceptance(peerId.peerAddr, false);
  }
  unblockPeersRecording({peerIds[9]});

  /* The recovered group serves a fresh round to every surviving peer. */
  const auto postRecoveryRoundComm = publishNextRound();
  expectAllPeersConverged(lateJoiners, postRecoveryRoundComm);
  EXPECT_EQ(getNumInSyncPeers(peerIds[1].peerAddr), lateJoiners.size());

  XLOGF(INFO, "=== TEST PASSED: DepAPromotedWhenLastSyncPeerDown ===");
}

/*
 * A4b: the same DEP-A shape, but EVERY flapped member comes back blocked, so
 * nothing is promotable when the last sync peer goes down and the group
 * freezes. On unblock the first to finish draining promotes ITSELF from
 * transitionPeerUpdateState and un-freezes the group; the rest then rejoin.
 */
TEST_P(GroupWithNoSyncPeersE2ETest, DepASelfPromotesAfterFrozenGroupUnblock) {
  XLOGF(INFO, "=== TEST: DepASelfPromotesAfterFrozenGroupUnblock ===");

  setupPolicies();
  const auto allSpecs = allPeerSpecs();
  for (int i = 0; i < kNumPeers; ++i) {
    setQueueSizeForPeer(
        allSpecs[i].peerAddr, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  }

  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers,
      /*queueCapacity=*/10,
      /*queueHighWm=*/8,
      /*queueLowWm=*/2);
  const auto& blockedSyncPeer = peerIds[0];

  publishNextRound();
  drainAll(peerIds);

  /* Every flapped member returns blocked, so none of them can drain. */
  std::vector<BgpPeerId> lateJoiners(peerIds.begin() + 1, peerIds.end());
  std::string roundComm;
  flapPeersIntoDepA(
      blockedSyncPeer,
      lateJoiners,
      /*returnBlocked=*/lateJoiners,
      /*extraRounds=*/2,
      &roundComm);
  expectAllDepA(lateJoiners);

  /*
   * All 9 must be blocked on their OWN init dump BEFORE the last sync peer
   * goes down: if even one had drained, handleNoSyncPeers would promote it and
   * the self-promotion path would never run.
   */
  for (const auto& peerId : lateJoiners) {
    ASSERT_TRUE(waitForPeerStateAny(
        peerId.peerAddr,
        {PeerUpdateState::DETACHED_INIT_DUMP,
         PeerUpdateState::DETACHED_BLOCKED}))
        << "peer " << peerId.peerAddr.str()
        << " never entered its own init dump after the flap";
    ASSERT_TRUE(waitForPeerQueueBlocked(peerId))
        << "peer " << peerId.peerAddr.str()
        << " never filled its queue on its own init dump";
    ASSERT_TRUE(
        waitForPeerState(peerId.peerAddr, PeerUpdateState::DETACHED_BLOCKED))
        << "peer " << peerId.peerAddr.str()
        << " did not stall at DETACHED_BLOCKED, so it may be promotable";
  }

  /* Last sync peer down: nothing is promotable, so the group freezes. */
  bringDownPeer(blockedSyncPeer.peerAddr);
  EXPECT_EQ(getNumInSyncPeers(peerIds[1].peerAddr), 0u)
      << "no blocked DEP-A should have been promotable";
  EXPECT_EQ(getGroupMemberCount(peerIds[1].peerAddr), lateJoiners.size());

  /*
   * Unblock them all: the first to finish draining promotes itself and the
   * group resumes, after which the rest rejoin.
   */
  unblockPeersRecording(lateJoiners);
  drainUntil(
      lateJoiners,
      [&]() { return getNumInSyncPeers(peerIds[1].peerAddr) > 0; },
      "no DEP-A promoted itself to un-freeze the group",
      60);
  EXPECT_GE(getNumInSyncPeers(peerIds[1].peerAddr), 1u)
      << "no DEP-A promoted itself to un-freeze the group";

  const auto postRecoveryRoundComm = publishNextRound();
  expectAllPeersConverged(lateJoiners, postRecoveryRoundComm);
  EXPECT_EQ(getNumInSyncPeers(peerIds[1].peerAddr), lateJoiners.size());

  XLOGF(INFO, "=== TEST PASSED: DepASelfPromotesAfterFrozenGroupUnblock ===");
}

/*
 * A blocking DSP goes DOWN before it can rejoin: the group must not stay stuck.
 *
 * handleNoSyncPeers refuses to promote while numPeersDetachedAfterJoin_ > 0: a
 * peer that detached AFTER joining still shares the group's entries, and
 * promoting a diverged DEP-A ahead of it would delete group-only entries and
 * corrupt that peer's pending rejoin. One sharing DSP is therefore enough to
 * keep a group with no sync peers frozen even with 8 promotable DEP-A members
 * waiting in DETACHED_READY_TO_JOIN.
 *
 * Removing the blocker must resolve it: bringing the DSP down before it ever
 * rejoins decrements the counter in removePeer(), and unregisterPeer() then
 * calls recoverIfNoSyncPeers(), so that very pass must promote a waiting peer.
 * If the bookkeeping ever leaked the group would stay frozen forever with
 * promotable peers available, which is the failure this guards against.
 */
TEST_P(GroupWithNoSyncPeersE2ETest, BlockingDspDownLetsDrjPeerBePromoted) {
  XLOGF(INFO, "=== TEST: BlockingDspDownLetsDrjPeerBePromoted ===");

  setupPolicies();
  const auto allSpecs = allPeerSpecs();
  for (int i = 0; i < kNumPeers; ++i) {
    setQueueSizeForPeer(
        allSpecs[i].peerAddr, /*capacity=*/3, /*highWm=*/2, /*lowWm=*/0);
  }

  auto peerIds = setupNPeersInGroupJoined(
      kNumPeers,
      /*queueCapacity=*/10,
      /*queueHighWm=*/8,
      /*queueLowWm=*/2);
  const auto& stallPeer = peerIds[0];
  const auto& dspPeer = peerIds[1];
  std::vector<BgpPeerId> drjPeers(peerIds.begin() + 2, peerIds.end());

  publishNextRound();
  drainAll(peerIds);

  /*
   * Slow-peer detach peer 1 FIRST, while the block-count threshold is still
   * low: it detaches AFTER joining, so it carries a non-zero
   * detachedRibVersion and is the sharing DSP that will block promotion. The
   * flap below raises the threshold group-wide, so nothing else can detach.
   */
  std::string detachRoundComm;
  std::vector<BgpPeerId> others(peerIds.begin() + 2, peerIds.end());
  others.push_back(stallPeer);
  detachPeersBlocked({dspPeer}, others, &detachRoundComm);
  ASSERT_TRUE(isPeerDetached(dspPeer.peerAddr));

  /* Flap the other 8 into DEP-A behind the stalled group. */
  std::string roundComm;
  flapPeersIntoDepA(
      stallPeer,
      drjPeers,
      /*returnBlocked=*/{},
      /*extraRounds=*/2,
      &roundComm);
  drainUntil(
      drjPeers,
      [&]() {
        return allPeersInState(
            drjPeers, PeerUpdateState::DETACHED_READY_TO_JOIN);
      },
      "peers never parked at DETACHED_READY_TO_JOIN",
      40);
  /*
   * The standoff under test is "promotable DEP-A peers held back by ONE sharing
   * DSP", so the sharing count must come from dspPeer alone.
   */
  for (const auto& peerId : drjPeers) {
    EXPECT_TRUE(isDetachedOnRegistration(peerId.peerAddr))
        << "peer " << peerId.peerAddr.str() << " is not a DEP-A";
    EXPECT_EQ(getPeerDetachedRibVersion(peerId.peerAddr), 0u)
        << "peer " << peerId.peerAddr.str() << " detached after joining";
  }
  EXPECT_EQ(getNumPeersDetachedAfterJoin(drjPeers[0].peerAddr), 1u)
      << "only the sharing DSP should count as detached after joining";
  expectAllAheadOfGroup(drjPeers);

  /* Hold the ordinary rejoin path so handleNoSyncPeers decides what happens. */
  for (const auto& peerId : drjPeers) {
    testOnlyDeferDrjAcceptance(peerId.peerAddr, true);
  }

  /* Last sync peer down: the sharing DSP keeps the group frozen. */
  bringDownPeer(stallPeer.peerAddr);
  EXPECT_EQ(getNumInSyncPeers(drjPeers[0].peerAddr), 0u)
      << "the sharing DSP should have blocked promotion, leaving it frozen";
  EXPECT_TRUE(isPeerDetached(dspPeer.peerAddr))
      << "the DSP must still be detached -- it must not have rejoined";

  /* Remove the blocker before it ever rejoins. */
  bringDownPeer(dspPeer.peerAddr);
  EXPECT_EQ(getNumInSyncPeers(drjPeers[0].peerAddr), 1u)
      << "group is stuck: no DRJ peer was promoted after the blocking DSP left";
  EXPECT_EQ(getGroupMemberCount(drjPeers[0].peerAddr), drjPeers.size());

  /* Release the hold: the rest rejoin and the group serves a fresh round. */
  for (const auto& peerId : drjPeers) {
    testOnlyDeferDrjAcceptance(peerId.peerAddr, false);
  }
  const auto postRecoveryRoundComm = publishNextRound();
  expectAllPeersConverged(drjPeers, postRecoveryRoundComm);
  EXPECT_EQ(getNumInSyncPeers(drjPeers[0].peerAddr), drjPeers.size());

  XLOGF(INFO, "=== TEST PASSED: BlockingDspDownLetsDrjPeerBePromoted ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    GroupWithNoSyncPeersE2ETest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
