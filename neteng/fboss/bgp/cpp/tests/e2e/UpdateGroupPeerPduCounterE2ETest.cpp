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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

#include <gtest/gtest.h>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/synchronization/Baton.h>

#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/TestSessionManager.h"

namespace facebook::bgp {
namespace {

constexpr auto kRoute = "60.0.0.0/8";

class UpdateGroupPeerPduCounterE2ETest : public E2ESessionTestFixture {
 protected:
  struct PduCounts {
    uint64_t updates{0};
    uint64_t announcementsV4{0};
    uint64_t withdrawals{0};
  };

  struct ApiPduCounts {
    size_t sessionCount{0};
    bool detailsPresent{false};
    int64_t sentUpdates{0};
    int64_t adjRibSentUpdates{0};
    int64_t announcementsV4{0};
    int64_t withdrawals{0};
  };

  static constexpr auto kConditionPollInterval = std::chrono::milliseconds(10);
  static constexpr size_t kConditionPollAttempts = 500;

  void SetUp() override {
    setEorTimeSeconds(1);
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);

    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/true,
        /*enableEgressBackpressure=*/true,
        /*enableSerializeGroupPdu=*/true);
  }

  template <typename Condition>
  bool waitForPeerManagerCondition(Condition condition) {
    struct WaitState {
      folly::Baton<> done;
      std::unique_ptr<folly::AsyncTimeout> timer;
      std::atomic<bool> matched{false};
      size_t remainingAttempts{kConditionPollAttempts};
    };

    auto state = std::make_shared<WaitState>();
    auto& eventBase = peerManager_->getEventBase();
    eventBase.runInEventBaseThreadAndWait(
        [state, &eventBase, condition = std::move(condition)]() mutable {
          state->timer = folly::AsyncTimeout::make(
              eventBase,
              [state, condition = std::move(condition)]() mutable noexcept {
                if (condition()) {
                  state->matched.store(true, std::memory_order_release);
                  state->done.post();
                  return;
                }
                if (--state->remainingAttempts == 0) {
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
          "peer PDU counter PeerManager condition",
          std::chrono::seconds(10));
    } catch (...) {
      eventBase.runInEventBaseThread([state]() {
        state->timer->cancelTimeout();
        state->timer.reset();
      });
      throw;
    }
    eventBase.runInEventBaseThreadAndWait([state]() {
      state->timer->cancelTimeout();
      state->timer.reset();
    });
    return state->matched.load(std::memory_order_acquire);
  }
};

TEST_F(
    UpdateGroupPeerPduCounterE2ETest,
    LatePeerDoesNotInheritGroupLifetimePduCountersAfterRejoin) {
  BgpPeerId peer3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peer4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeerAndWait(kPeerAddr3);
  sendEoRToPeer(peer3);
  ASSERT_TRUE(waitForEoR(peer3));
  ASSERT_TRUE(waitForEoR(peer3));

  injectLocalRoutesAtRuntime({kRoute});
  ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(kRoute)));
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "60.0.0.0", 8, kPeerAddr3, kNextHopV4_3.str()));

  withdrawLocalRoutesAtRuntime({kRoute});
  ASSERT_TRUE(verifyRouteWithdraws(
      "v4", kPeerAddr3, {{.prefix = "60.0.0.0", .prefixLen = 8}}));
  ASSERT_TRUE(
      verifyRouteNotInShadowRib(folly::IPAddress::createNetwork(kRoute)));

  injectLocalRoutesAtRuntime({kRoute});
  ASSERT_TRUE(waitForRouteInShadowRib(folly::IPAddress::createNetwork(kRoute)));
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "60.0.0.0", 8, kPeerAddr3, kNextHopV4_3.str()));

  bringUpPeerAndWait(kPeerAddr4);
  sendEoRToPeer(peer4);
  ASSERT_TRUE(drainAndFindRouteAdvertised(
      "v4", "60.0.0.0", 8, kPeerAddr4, kNextHopV4_4.str()));
  ASSERT_TRUE(waitForEoR(peer4));
  ASSERT_TRUE(waitForEoR(peer4));

  ASSERT_TRUE(waitForPeerManagerCondition([&]() {
    auto source = getAdjRibByAddr(kPeerAddr3);
    auto latePeer = getAdjRibByAddr(kPeerAddr4);
    return source && latePeer && source->getUpdateGroup() &&
        source->getUpdateGroup() == latePeer->getUpdateGroup() &&
        latePeer->getPeerState() == PeerUpdateState::JOINED_RUNNING;
  }));

  PduCounts peerCounts;
  PduCounts groupCounts;
  bool countsRead{false};
  peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
    auto latePeer = getAdjRibByAddr(kPeerAddr4);
    if (!latePeer) {
      return;
    }
    auto group = latePeer->getUpdateGroup();
    if (!group) {
      return;
    }

    const auto peerStats = latePeer->getStats();
    peerCounts = {
        .updates = peerStats.getSentUpdateMsgs(),
        .announcementsV4 = peerStats.getSentAnnouncementsIpv4(),
        .withdrawals = peerStats.getSentWithdrawals(),
    };

    const auto& groupStats = group->getStats();
    groupCounts = {
        .updates = groupStats.getSentUpdateMsgs(),
        .announcementsV4 = groupStats.getSentAnnouncementsIpv4(),
        .withdrawals = groupStats.getSentWithdrawals(),
    };
    countsRead = true;
  });

  ASSERT_TRUE(countsRead);
  ASSERT_LT(peerCounts.updates, groupCounts.updates);
  ASSERT_LT(peerCounts.announcementsV4, groupCounts.announcementsV4);
  ASSERT_LT(peerCounts.withdrawals, groupCounts.withdrawals);

  const auto peerState = testSessionManager_->getPeerStates().find(peer4);
  ASSERT_NE(testSessionManager_->getPeerStates().end(), peerState);
  std::unordered_multimap<
      folly::IPAddress,
      std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>>
      peerInfoMap;
  peerInfoMap.emplace(
      kPeerAddr4,
      std::make_shared<nettools::bgplib::BgpPeerDisplayInfo>(
          peerState->second.displayInfo));

  ApiPduCounts apiCounts;
  peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
    const auto sessions = peerManager_->getDetailSessionInfos(peerInfoMap);
    apiCounts.sessionCount = sessions.size();
    if (sessions.size() != 1 || !sessions.front().details().has_value()) {
      return;
    }
    const auto& session = sessions.front();
    const auto& details = session.details().value();
    apiCounts.detailsPresent = true;
    apiCounts.sentUpdates = session.sent_update_msgs().value();
    apiCounts.adjRibSentUpdates = details.adjrib_sent_update_msgs().value();
    apiCounts.announcementsV4 =
        details.sent_update_announcements_ipv4().value();
    apiCounts.withdrawals = details.sent_update_withdrawals().value();
  });

  ASSERT_EQ(1, apiCounts.sessionCount);
  ASSERT_TRUE(apiCounts.detailsPresent);
  EXPECT_EQ(peerCounts.updates, apiCounts.sentUpdates);
  EXPECT_EQ(peerCounts.updates, apiCounts.adjRibSentUpdates);
  EXPECT_EQ(peerCounts.announcementsV4, apiCounts.announcementsV4);
  EXPECT_EQ(peerCounts.withdrawals, apiCounts.withdrawals);
}

} // namespace
} // namespace facebook::bgp
