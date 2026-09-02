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

#include "neteng/fboss/bgp/cpp/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"
#include "neteng/fboss/bgp/if/gen-cpp2/TBgpService.h"

namespace facebook::bgp {
namespace {

constexpr auto kInitialDumpProfile =
    "AdjRibOutGroup::buildAndSendGroupBgpMessages";

const std::vector<std::string> kInitialRoutes = {
    "198.51.100.0/24",
    "198.51.101.0/24",
    "2001:db8:100::/48",
    "2001:db8:101::/48",
};

class UpdateGroupInitialDumpCoalescingE2ETest : public E2ESessionTestFixture {
 protected:
  using BgpClient =
      apache::thrift::Client<neteng::fboss::bgp::thrift::TBgpService>;

  static constexpr auto kConditionPollInterval = std::chrono::milliseconds(10);
  static constexpr auto kConditionWaitTimeout = std::chrono::seconds(5);

  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
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
    folly::coro::blockingWait(bgpClient_->co_startProfiler(false));
    folly::coro::blockingWait(bgpClient_->co_setProfilerFilter(""));
    folly::coro::blockingWait(bgpClient_->co_clearProfilerStats());
  }

  void TearDown() override {
    if (bgpClient_) {
      EXPECT_NO_THROW(
          folly::coro::blockingWait(bgpClient_->co_startProfiler(false)));
      EXPECT_NO_THROW(
          folly::coro::blockingWait(bgpClient_->co_clearProfilerStats()));
      EXPECT_NO_THROW(
          folly::coro::blockingWait(bgpClient_->co_setProfilerFilter("")));
    }
    bgpClient_.reset();
    bgpService_.reset();
    watchdog_.reset();
    E2ESessionTestFixture::TearDown();
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
    state->deadline = std::chrono::steady_clock::now() + kConditionWaitTimeout;
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
                state->timer->scheduleTimeout(kConditionPollInterval);
              });
          state->timer->scheduleTimeout(std::chrono::milliseconds(0));
        });

    try {
      test::boundedBatonWait(
          state->done,
          "initial-dump coalescing PeerManager condition",
          kConditionWaitTimeout * 2);
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

    const auto result =
        folly::coro::blockingWait(bgpClient_->co_addPeers(peers));
    ASSERT_EQ(
        neteng::fboss::bgp::thrift::BgpConfigChangeResult::CONFIG_APPLIED,
        result);
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

  std::unique_ptr<Watchdog> watchdog_;
  std::shared_ptr<BgpServiceBB> bgpService_;
  std::unique_ptr<BgpClient> bgpClient_;
};

TEST_F(
    UpdateGroupInitialDumpCoalescingE2ETest,
    SameKeyPeerEventsQueuedTogetherScheduleOneInitialDump) {
  const BgpPeerId bootstrapPeer{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  const BgpPeerId peer4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  const BgpPeerId peer5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeerAndWait(kPeerAddr3);
  sendEoRToPeer(bootstrapPeer);
  ASSERT_TRUE(waitForEoR(bootstrapPeer));
  ASSERT_TRUE(waitForEoR(bootstrapPeer));
  ASSERT_TRUE(waitForPeerManagerCondition(
      [&]() { return peerManager_->getIsInitialized(); }));

  std::weak_ptr<AdjRibOutGroup> bootstrapGroup;
  auto& eventBase = peerManager_->getEventBase();
  eventBase.runInEventBaseThreadAndWait([&]() {
    auto adjRib = getAdjRibByAddr(kPeerAddr3);
    if (adjRib) {
      bootstrapGroup = adjRib->getUpdateGroup();
    }
  });
  ASSERT_FALSE(bootstrapGroup.expired());
  bringDownPeerAndWait(kPeerAddr3);
  ASSERT_TRUE(
      waitForPeerManagerCondition([&]() { return bootstrapGroup.expired(); }));

  injectLocalRoutesAtRuntime(kInitialRoutes);
  for (const auto& route : kInitialRoutes) {
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(route)));
  }

  addTargetPeersThroughService();

  folly::coro::blockingWait(bgpClient_->co_setProfilerFilter(
      "^AdjRibOutGroup::buildAndSendGroupBgpMessages$"));
  folly::coro::blockingWait(bgpClient_->co_clearProfilerStats());
  folly::coro::blockingWait(bgpClient_->co_startProfiler(true));

  struct EventBaseBarrier {
    folly::Baton<> blocked;
    folly::Baton<> release;
    std::atomic<bool> released{false};
  };
  auto barrier = std::make_shared<EventBaseBarrier>();
  eventBase.runInEventBaseThread([barrier] {
    barrier->blocked.post();
    barrier->released.store(
        barrier->release.try_wait_for(kConditionWaitTimeout),
        std::memory_order_release);
  });
  test::boundedBatonWait(
      barrier->blocked, "PeerManager EventBase test barrier");
  auto releaseGuard = folly::makeGuard([barrier] { barrier->release.post(); });
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  barrier->release.post();
  releaseGuard.dismiss();
  eventBase.runInEventBaseThreadAndWait([] {});
  ASSERT_TRUE(barrier->released.load(std::memory_order_acquire));

  ASSERT_TRUE(waitForPeerManagerCondition([&]() {
    auto adjRib4 = getAdjRibByAddr(kPeerAddr4);
    auto adjRib5 = getAdjRibByAddr(kPeerAddr5);
    auto group = adjRib4 ? adjRib4->getUpdateGroup() : nullptr;
    return adjRib4 && adjRib5 && group && group == adjRib5->getUpdateGroup() &&
        group->getMemberCount() == 2 &&
        group->getState() == UpdateGroupState::IDLE &&
        adjRib4->getPeerState() == PeerUpdateState::JOINED_RUNNING &&
        adjRib5->getPeerState() == PeerUpdateState::JOINED_RUNNING &&
        !adjRib4->egressEoRsPending() && !adjRib5->egressEoRsPending();
  }));

  /* Flush work enqueued by either initial-dump task before stopping the
   * profiler; a queued duplicate builder is the regression being measured. */
  eventBase.runInEventBaseThreadAndWait([]() {});
  eventBase.runInEventBaseThreadAndWait([]() {});
  folly::coro::blockingWait(bgpClient_->co_startProfiler(false));

  auto stats = folly::coro::blockingWait(bgpClient_->co_getProfilerStats());
  auto profile = std::find_if(stats.begin(), stats.end(), [](const auto& stat) {
    return *stat.name() == kInitialDumpProfile;
  });
  ASSERT_NE(stats.end(), profile);
  EXPECT_EQ(1, *profile->count())
      << "one already-initialized same-key peer event must not enqueue a "
         "second group initial-dump builder";

  expectCompleteInitialDump(peer4);
  expectCompleteInitialDump(peer5);
}

} // namespace
} // namespace facebook::bgp
