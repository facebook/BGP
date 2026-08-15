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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <folly/ScopeGuard.h>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/synchronization/Baton.h>

#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include "neteng/fboss/bgp/cpp/facebook/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"
#include "neteng/fboss/bgp/if/gen-cpp2/TBgpService.h"

namespace facebook::bgp {
namespace {

constexpr auto kAcceptPolicy = "SPLIT_DUMP_ACCEPT";
constexpr auto kDummyPolicy = "SPLIT_DUMP_DUMMY";
constexpr auto kTagPolicy = "SPLIT_DUMP_TAG";
constexpr auto kTagCommunity = "64512:314";
constexpr auto kBootstrapPeerGroup = "split-dump-bootstrap";

/* Injected only after the split, to model RIB churn on a stranded group. */
constexpr auto kPostSplitRoute = "203.0.150.0/24";

const std::vector<std::string> kInitialRoutes = {
    "203.0.113.0/24",
    "203.0.114.0/24",
    "2001:db8:314::/48",
    "2001:db8:315::/48",
};

class UpdateGroupSplitInitialDumpE2ETest : public E2ESessionTestFixture {
 protected:
  using BgpClient =
      apache::thrift::Client<neteng::fboss::bgp::thrift::TBgpService>;

  static constexpr auto kPollInterval = std::chrono::milliseconds(10);
  static constexpr auto kWaitTimeout = std::chrono::seconds(5);

  void SetUp() override {
    setupPolicies();

    auto bootstrap = kDefaultPeerSpec3_AddPath;
    bootstrap.peerGroupName = kBootstrapPeerGroup;
    bootstrap.egressPolicyName = kAcceptPolicy;
    addPeer(bootstrap);

    setDefaultQueueSizes(
        /*capacity=*/64, /*highWm=*/48, /*lowWm=*/0);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/true,
        /*enableEgressBackpressure=*/true,
        /*enableSerializeGroupPdu=*/true);

    watchdog_ = std::make_unique<Watchdog>(config_);
    bgpService_ = std::make_shared<BgpServiceBB>(
        *peerManager_,
        configManager_,
        *rib_,
        *watchdog_,
        /*nlWrapper=*/nullptr,
        /*enable_thrift_protection=*/false);
    bgpClient_ = apache::thrift::makeTestClient<BgpClient>(bgpService_);
  }

  void TearDown() override {
    bgpClient_.reset();
    bgpService_.reset();
    watchdog_.reset();
    E2ESessionTestFixture::TearDown();
  }

  void setupPolicies() {
    auto accept = [](const std::string& name) {
      return createBgpPolicyStatement(
          name,
          {createBgpPolicyTerm(
              "accept-all",
              "Accept all routes",
              {},
              {createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
              bgp_policy::FlowControlAction::NEXT_TERM)});
    };
    auto tag = createBgpPolicyStatement(
        kTagPolicy,
        {createBgpPolicyTerm(
            "tag-all",
            "Accept and tag all routes",
            {},
            {createBgpPolicyCommunityAction(
                 bgp_policy::CommunityActionType::ADD, {kTagCommunity}),
             createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT)},
            bgp_policy::FlowControlAction::NEXT_TERM)});

    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(accept(kAcceptPolicy));
    policies.bgp_policy_statements()->emplace_back(accept(kDummyPolicy));
    policies.bgp_policy_statements()->emplace_back(std::move(tag));
    setPolicyConfig(policies);
  }

  neteng::fboss::bgp::thrift::BgpPolicyChangeResult setPeerPolicy(
      const folly::IPAddress& peerAddr,
      const std::string& policyName) {
    std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>
        peersPolicy;
    peersPolicy[peerAddr.str()][bgp_policy::DIRECTION::OUT] = policyName;
    return folly::coro::blockingWait(
        bgpClient_->co_setPeersPolicy(peersPolicy));
  }

  thrift::BgpPeer makeDynamicPeer(
      const folly::IPAddress& peerAddr,
      uint32_t remoteAs,
      const folly::IPAddress& nexthopV4,
      const folly::IPAddress& nexthopV6) {
    thrift::BgpPeer peer;
    peer.peer_addr() = peerAddr.str();
    peer.local_addr() = kLocalAddr1.str();
    peer.remote_as_4_byte() = remoteAs;
    peer.next_hop4() = nexthopV4.str();
    peer.next_hop6() = nexthopV6.str();
    return peer;
  }

  void addTargetPeersThroughService() {
    std::vector<thrift::BgpPeer> peers;
    peers.emplace_back(
        makeDynamicPeer(kPeerAddr4, kPeerAsn4, kNextHopV4_4, kNextHopV6_4));
    peers.emplace_back(
        makeDynamicPeer(kPeerAddr5, kPeerAsn5, kNextHopV4_5, kNextHopV6_5));
    ASSERT_EQ(
        neteng::fboss::bgp::thrift::BgpConfigChangeResult::CONFIG_APPLIED,
        folly::coro::blockingWait(bgpClient_->co_addPeers(peers)));
  }

  template <typename Predicate>
  bool waitForPeerManagerCondition(Predicate predicate) {
    struct WaitState {
      folly::Baton<> done;
      std::unique_ptr<folly::AsyncTimeout> timer;
      std::atomic<bool> matched{false};
      std::chrono::steady_clock::time_point deadline;
    };

    auto state = std::make_shared<WaitState>();
    state->deadline = std::chrono::steady_clock::now() + kWaitTimeout;
    auto& eventBase = peerManager_->getEventBase();
    eventBase.runInEventBaseThreadAndWait(
        [state, &eventBase, predicate = std::move(predicate)]() mutable {
          state->timer = folly::AsyncTimeout::make(
              eventBase,
              [state, predicate = std::move(predicate)]() mutable noexcept {
                if (predicate()) {
                  state->matched.store(true, std::memory_order_release);
                  state->done.post();
                  return;
                }
                if (std::chrono::steady_clock::now() >= state->deadline) {
                  state->done.post();
                  return;
                }
                state->timer->scheduleTimeout(kPollInterval);
              });
          state->timer->scheduleTimeout(std::chrono::milliseconds(0));
        });

    try {
      test::boundedBatonWait(
          state->done,
          "split initial-dump PeerManager condition",
          kWaitTimeout * 2);
    } catch (...) {
      eventBase.runInEventBaseThread([state] {
        state->timer->cancelTimeout();
        state->timer.reset();
      });
      throw;
    }
    eventBase.runInEventBaseThreadAndWait([state] {
      state->timer->cancelTimeout();
      state->timer.reset();
    });
    return state->matched.load(std::memory_order_acquire);
  }

  void expectCompleteInitialDump(const BgpPeerId& peerId) {
    const auto messages = drainAllOutboundMessagesToOrderedVec(
        peerId,
        /*idleRetries=*/1,
        /*maxMessages=*/32,
        /*sleepMsBetweenRetries=*/0);

    size_t eorCount{0};
    std::map<folly::CIDRNetwork, size_t> announced;
    std::vector<folly::CIDRNetwork> withdrawn;
    for (const auto& message : messages) {
      if (message.isEoR) {
        ++eorCount;
        continue;
      }
      ASSERT_NE(nullptr, message.update);
      for (const auto& prefix : getAnnouncedPrefixes(*message.update)) {
        ++announced[prefix];
      }
      auto updateWithdrawals = getWithdrawnPrefixes(*message.update);
      withdrawn.insert(
          withdrawn.end(), updateWithdrawals.begin(), updateWithdrawals.end());
    }

    EXPECT_EQ(2, eorCount);
    EXPECT_TRUE(withdrawn.empty());
    EXPECT_EQ(kInitialRoutes.size(), announced.size());
    for (const auto& route : kInitialRoutes) {
      const auto network = folly::IPAddress::createNetwork(route);
      ASSERT_TRUE(announced.contains(network))
          << "initial dump missed " << route << " for " << peerId.str();
      EXPECT_EQ(1, announced.at(network))
          << "initial dump duplicated " << route << " for " << peerId.str();
    }
  }

  /*
   * Drive a partial egress-policy split whose source group has not run its
   * initial dump, so the target is created UNINITIALIZED with an empty packing
   * list and its peer still in INIT. Returns once the split is observed and
   * the source's held dump has been released.
   */
  void driveUninitializedPolicySplit() {
    const BgpPeerId bootstrapPeer{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    bringUpPeerAndWait(kPeerAddr3);
    sendEoRToPeer(bootstrapPeer);
    ASSERT_TRUE(waitForEoR(bootstrapPeer));
    ASSERT_TRUE(waitForEoR(bootstrapPeer));
    ASSERT_TRUE(waitForPeerManagerCondition(
        [&] { return peerManager_->getIsInitialized(); }));

    ASSERT_NO_FATAL_FAILURE(addTargetPeersThroughService());

    injectLocalRoutesAtRuntime(kInitialRoutes);
    for (const auto& route : kInitialRoutes) {
      ASSERT_TRUE(
          waitForRouteInShadowRib(folly::IPAddress::createNetwork(route)));
    }

    struct EventBaseBarrier {
      folly::Baton<> entered;
      folly::Baton<> release;
      folly::Baton<> exited;
      std::atomic<bool> released{false};
    };
    struct RaceState {
      bool preconditionObserved{false};
    };
    auto race = std::make_shared<RaceState>();
    auto& eventBase = peerManager_->getEventBase();

    auto dummyGate = std::make_shared<EventBaseBarrier>();
    auto sessionGate = std::make_shared<EventBaseBarrier>();
    auto targetGate = std::make_shared<EventBaseBarrier>();
    auto dummyGateGuard =
        folly::makeGuard([dummyGate] { dummyGate->release.post(); });
    auto sessionGateGuard =
        folly::makeGuard([sessionGate] { sessionGate->release.post(); });
    auto targetGateGuard =
        folly::makeGuard([targetGate] { targetGate->release.post(); });

    /*
     * The nesting fixes the production work order without touching production
     * state. The dummy policy apply queues a fleet reconciliation. The session
     * coroutine then drains both already-pushed ESTABLISHED events and queues
     * one source-group dump. The innermost gate runs after both registrations
     * but before that reconciliation and dump. The generated target-policy RPC
     * commits the config and queues the live AdjRib apply; the prequeued
     * reconciliation then observes that apply before the source dump runs.
     */
    eventBase.runInEventBaseThread(
        [this, &eventBase, dummyGate, sessionGate, targetGate, race] {
          eventBase.runInLoop(
              [this, &eventBase, dummyGate, sessionGate, targetGate, race] {
                eventBase.runInLoop([sessionGate] {
                  sessionGate->entered.post();
                  sessionGate->released.store(
                      sessionGate->release.try_wait_for(kWaitTimeout),
                      std::memory_order_release);
                  sessionGate->exited.post();
                });
                eventBase.runInLoop([this, &eventBase, targetGate, race] {
                  eventBase.runInLoop([this, targetGate, race] {
                    auto first = getAdjRibByAddr(kPeerAddr4);
                    auto second = getAdjRibByAddr(kPeerAddr5);
                    auto source = first ? first->getUpdateGroup() : nullptr;
                    race->preconditionObserved = first && second && source &&
                        source == second->getUpdateGroup() &&
                        source->getState() == UpdateGroupState::UNINITIALIZED &&
                        source->getMemberCount() == 2 &&
                        source->getNumInSyncPeers() == 2 &&
                        first->getPeerState() == PeerUpdateState::INIT &&
                        second->getPeerState() == PeerUpdateState::INIT &&
                        source->getGroupKey().egressPolicyName.value_or("") ==
                            "" &&
                        !source->getGroupKey().peerOverride &&
                        !source->getChangeListConsumer();
                    /*
                     * Hold the source's queued dump here. It is ahead of the
                     * target-policy RPC in the queue, so without this it walks
                     * first and the split target inherits READY instead of
                     * UNINITIALIZED -- the case under test never forms.
                     */
                    if (source) {
                      source->testOnlyDeferInitialDump = true;
                    }
                    targetGate->entered.post();
                    targetGate->released.store(
                        targetGate->release.try_wait_for(kWaitTimeout),
                        std::memory_order_release);
                    targetGate->exited.post();
                  });
                });
                dummyGate->entered.post();
                dummyGate->released.store(
                    dummyGate->release.try_wait_for(kWaitTimeout),
                    std::memory_order_release);
                dummyGate->exited.post();
              });
        });

    test::boundedBatonWait(
        dummyGate->entered, "split initial-dump dummy-policy gate");
    const auto dummyResult = setPeerPolicy(kPeerAddr3, kDummyPolicy);
    ASSERT_EQ(
        neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED,
        dummyResult);
    dummyGate->release.post();
    dummyGateGuard.dismiss();
    test::boundedBatonWait(
        dummyGate->exited, "split initial-dump dummy-policy gate exit");
    ASSERT_TRUE(dummyGate->released.load(std::memory_order_acquire));

    test::boundedBatonWait(
        sessionGate->entered, "split initial-dump session gate");
    bringUpPeer(kPeerAddr4);
    bringUpPeer(kPeerAddr5);
    sessionGate->release.post();
    sessionGateGuard.dismiss();
    test::boundedBatonWait(
        sessionGate->exited, "split initial-dump session gate exit");
    ASSERT_TRUE(sessionGate->released.load(std::memory_order_acquire));

    test::boundedBatonWait(
        targetGate->entered, "split initial-dump target-policy gate");
    const auto targetResult = setPeerPolicy(kPeerAddr4, kTagPolicy);
    targetGate->release.post();
    targetGateGuard.dismiss();
    test::boundedBatonWait(
        targetGate->exited, "split initial-dump target-policy gate exit");
    ASSERT_TRUE(targetGate->released.load(std::memory_order_acquire));
    ASSERT_EQ(
        neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED,
        targetResult);
    ASSERT_TRUE(race->preconditionObserved)
        << "both session events must register in one queued-dump source group "
           "before the target policy RPC";

    /*
     * Wait for the split itself, which is what proves the target was carved out
     * of a source that had not dumped: the target inherits UNINITIALIZED, an
     * empty packing list and peers in INIT, and the inline egress-policy
     * re-evaluation is the only thing that can serve it. Only then release the
     * source's dump, so it cannot have raced ahead of the split.
     */
    ASSERT_TRUE(waitForPeerManagerCondition([&] {
      auto first = getAdjRibByAddr(kPeerAddr4);
      auto second = getAdjRibByAddr(kPeerAddr5);
      auto firstGroup = first ? first->getUpdateGroup() : nullptr;
      auto secondGroup = second ? second->getUpdateGroup() : nullptr;
      return firstGroup && secondGroup && firstGroup != secondGroup;
    })) << "target peer was never split into its own group";

    eventBase.runInEventBaseThreadAndWait([this] {
      auto second = getAdjRibByAddr(kPeerAddr5);
      if (auto sourceGroup = second ? second->getUpdateGroup() : nullptr) {
        sourceGroup->testOnlyDeferInitialDump = false;
      }
    });
  }

  std::unique_ptr<Watchdog> watchdog_;
  std::shared_ptr<BgpServiceBB> bgpService_;
  std::unique_ptr<BgpClient> bgpClient_;
};

TEST_F(
    UpdateGroupSplitInitialDumpE2ETest,
    PartialPolicySplitSchedulesTargetInitialDump) {
  const BgpPeerId firstTarget{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  const BgpPeerId secondTarget{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  ASSERT_NO_FATAL_FAILURE(driveUninitializedPolicySplit());

  const bool completed = waitForPeerManagerCondition([&] {
    auto first = getAdjRibByAddr(kPeerAddr4);
    auto second = getAdjRibByAddr(kPeerAddr5);
    auto firstGroup = first ? first->getUpdateGroup() : nullptr;
    auto secondGroup = second ? second->getUpdateGroup() : nullptr;
    return first && second && firstGroup && secondGroup &&
        firstGroup != secondGroup && firstGroup->getMemberCount() == 1 &&
        secondGroup->getMemberCount() == 1 &&
        firstGroup->getGroupKey().egressPolicyName.value_or("") == kTagPolicy &&
        secondGroup->getGroupKey().egressPolicyName.value_or("") == "" &&
        firstGroup->getState() == UpdateGroupState::IDLE &&
        secondGroup->getState() == UpdateGroupState::IDLE &&
        first->getPeerState() == PeerUpdateState::JOINED_RUNNING &&
        second->getPeerState() == PeerUpdateState::JOINED_RUNNING &&
        !firstGroup->egressEoRsPending() && !secondGroup->egressEoRsPending();
  });

  PeerUpdateState firstState{PeerUpdateState::DOWN};
  UpdateGroupState firstGroupState{UpdateGroupState::UNINITIALIZED};
  size_t firstMembers{0};
  peerManager_->getEventBase().runInEventBaseThreadAndWait([&] {
    auto first = getAdjRibByAddr(kPeerAddr4);
    auto group = first ? first->getUpdateGroup() : nullptr;
    if (first) {
      firstState = first->getPeerState();
    }
    if (group) {
      firstGroupState = group->getState();
      firstMembers = group->getMemberCount();
    }
  });
  ASSERT_TRUE(completed) << "split target peer state="
                         << static_cast<int>(firstState)
                         << " group state=" << static_cast<int>(firstGroupState)
                         << " members=" << firstMembers;

  expectCompleteInitialDump(firstTarget);
  expectCompleteInitialDump(secondTarget);
}

/*
 * A split target that never gets its initial dump is not merely silent: once
 * the RIB churns, the change list carries new prefixes to it while the table
 * that predates the split, and the EoR that would mark it complete, never
 * arrive. The peer then holds a permanently partial table it cannot know is
 * partial -- and the session looks healthy, because updates keep flowing.
 *
 * This pins the whole set: pre-split routes, both EoRs, and the post-split
 * route all reach the split target.
 */
TEST_F(
    UpdateGroupSplitInitialDumpE2ETest,
    SplitTargetGetsFullTableNotOnlyPostSplitChurn) {
  const BgpPeerId firstTarget{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  ASSERT_NO_FATAL_FAILURE(driveUninitializedPolicySplit());

  const auto postSplitNetwork =
      folly::IPAddress::createNetwork(kPostSplitRoute);
  injectLocalRoutesAtRuntime({kPostSplitRoute});
  ASSERT_TRUE(waitForRouteInShadowRib(postSplitNetwork));

  /*
   * Accumulate across drains: the dump and the churn land in separate batches,
   * and each drain consumes what it reads.
   */
  size_t eorCount{0};
  std::map<folly::CIDRNetwork, size_t> announced;
  auto drainOnce = [&]() {
    for (const auto& message : drainAllOutboundMessagesToOrderedVec(
             firstTarget,
             /*idleRetries=*/1,
             /*maxMessages=*/64,
             /*sleepMsBetweenRetries=*/0)) {
      if (message.isEoR) {
        ++eorCount;
        continue;
      }
      if (message.update) {
        for (const auto& prefix : getAnnouncedPrefixes(*message.update)) {
          ++announced[prefix];
        }
      }
    }
  };

  WITH_RETRIES_N(60, {
    drainOnce();
    EXPECT_EVENTUALLY_TRUE(
        announced.contains(postSplitNetwork) && eorCount >= 2 &&
        announced.size() > kInitialRoutes.size());
  });

  EXPECT_TRUE(announced.contains(postSplitNetwork))
      << "post-split churn never reached the split target";
  for (const auto& route : kInitialRoutes) {
    const auto network = folly::IPAddress::createNetwork(route);
    EXPECT_TRUE(announced.contains(network))
        << "split target received post-split churn but never the pre-split "
        << "route " << route;
  }
  EXPECT_EQ(2, eorCount)
      << "split target received routes but never an EoR, so it cannot tell "
         "its table is incomplete";
}

} // namespace
} // namespace facebook::bgp
