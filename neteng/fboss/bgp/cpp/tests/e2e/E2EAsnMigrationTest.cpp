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

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <folly/io/async/AsyncTimeout.h>
#include <folly/synchronization/Baton.h>

#include "fb303/ThreadCachedServiceData.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/TestSessionManager.h"

namespace facebook::bgp {
namespace {

constexpr uint32_t kConfedParentAsn = 65000;
constexpr auto kTestPrefix = "10.10.0.0";
constexpr uint8_t kTestPrefixLength = 16;
constexpr auto kTestNexthop = "11.0.0.1";
constexpr auto kInputAsPath = "65001";

struct RuntimePeerState {
  uint32_t remoteAs{0};
  BgpSessionType sessionType{BgpSessionType::EBGP};
  std::shared_ptr<AdjRibOutGroup> updateGroup;
};

class E2EAsnMigrationTest : public E2ESessionTestFixture {
 protected:
  static constexpr auto kCleanupPollInterval = std::chrono::milliseconds(10);
  static constexpr size_t kCleanupPollAttempts = 500;

  static BgpPeerSpec makeV4Peer(
      BgpPeerSpec spec,
      uint32_t configuredRemoteAs,
      std::optional<uint32_t> additionalRemoteAs) {
    spec.asn = configuredRemoteAs;
    spec.additionalRemoteAs = additionalRemoteAs;
    spec.disableIpv6Afi = true;
    spec.v6Nexthop = kEmptyV6Nexthop;
    return spec;
  }

  void setupPeers(
      const std::vector<BgpPeerSpec>& peers,
      bool enableUpdateGroup = false) {
    for (const auto& peer : peers) {
      addPeer(peer);
    }
    createRib();
    createPeerManager(enableUpdateGroup, /*enableEgressBackpressure=*/false);
  }

  void establishV4Peer(const folly::IPAddress& peerAddr, uint32_t remoteAs) {
    bringUpPeerWithRemoteAs(peerAddr, remoteAs);
    ASSERT_TRUE(waitForSessionEstablished(peerAddr));
    BgpPeerId peerId{peerAddr, peerAddr.asV4().toLongHBO()};
    sendEoRToPeer(peerId);
    ASSERT_TRUE(waitForEoR(peerId));
  }

  void establishV4Peers(
      const std::vector<std::pair<folly::IPAddress, uint32_t>>& peers) {
    for (const auto& [peerAddr, remoteAs] : peers) {
      bringUpPeerWithRemoteAs(peerAddr, remoteAs);
    }
    for (const auto& peer : peers) {
      const auto& peerAddr = peer.first;
      ASSERT_TRUE(waitForSessionEstablished(peerAddr));
      BgpPeerId peerId{peerAddr, peerAddr.asV4().toLongHBO()};
      sendEoRToPeer(peerId);
    }
  }

  std::optional<RuntimePeerState> getRuntimePeerState(
      const folly::IPAddress& peerAddr) {
    return folly::via(
               &peerManager_->getEventBase(),
               [this, peerAddr]() -> std::optional<RuntimePeerState> {
                 auto adjRib = getAdjRibByAddr(peerAddr);
                 if (!adjRib) {
                   return std::nullopt;
                 }
                 return RuntimePeerState{
                     .remoteAs = adjRib->getRemoteAs(),
                     .sessionType = adjRib->getUpdateGroupKey().sessionType,
                     .updateGroup = adjRib->getUpdateGroup()};
               })
        .get();
  }

  std::optional<uint32_t> getReportedRemoteAs(
      const folly::IPAddress& peerAddr) {
    BgpPeerId peerId{peerAddr, peerAddr.asV4().toLongHBO()};
    const auto& peerStates = testSessionManager_->getPeerStates();
    auto peerState = peerStates.find(peerId);
    if (peerState == peerStates.end()) {
      return std::nullopt;
    }

    std::unordered_multimap<
        folly::IPAddress,
        std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>>
        peerInfoMap;
    peerInfoMap.emplace(
        peerAddr,
        std::make_shared<nettools::bgplib::BgpPeerDisplayInfo>(
            peerState->second.displayInfo));
    const auto sessions = peerManager_->getDetailSessionInfos(peerInfoMap);
    if (sessions.size() != 1 || !sessions.front().peer().has_value()) {
      return std::nullopt;
    }
    return *sessions.front().peer()->remote_as_4_byte();
  }

  std::optional<bool> getRibOutAdvertised(
      const folly::IPAddress& peerAddr,
      const folly::CIDRNetwork& prefix) {
    return folly::via(
               &peerManager_->getEventBase(),
               [this, peerAddr, prefix]() -> std::optional<bool> {
                 auto adjRib = getAdjRibByAddr(peerAddr);
                 if (!adjRib) {
                   return std::nullopt;
                 }
                 auto* entry = adjRib->getRibEntry(
                     /*ingress=*/false, prefix);
                 if (!entry) {
                   return false;
                 }
                 return entry->getPostAttr() != nullptr;
               })
        .get();
  }

  void drainRouteProcessing() {
    rib_->getEventBase().runInEventBaseThreadAndWait([]() {});
    peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
    peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
  }

  bool waitForAdjRibRemoval(const folly::IPAddress& peerAddr) {
    struct WaitState {
      folly::Baton<> done;
      std::unique_ptr<folly::AsyncTimeout> timer;
      std::atomic<bool> removed{false};
      size_t remainingAttempts{kCleanupPollAttempts};
    };

    auto state = std::make_shared<WaitState>();
    auto& eventBase = peerManager_->getEventBase();
    eventBase.runInEventBaseThreadAndWait(
        [this, state, &eventBase, peerAddr]() {
          state->timer = folly::AsyncTimeout::make(
              eventBase, [this, state, peerAddr]() noexcept {
                if (getAdjRibByAddr(peerAddr) == nullptr) {
                  state->removed.store(true, std::memory_order_release);
                  state->done.post();
                  return;
                }
                if (--state->remainingAttempts == 0) {
                  state->done.post();
                  return;
                }
                state->timer->scheduleTimeout(kCleanupPollInterval);
              });
          state->timer->scheduleTimeout(std::chrono::milliseconds(0));
        });

    try {
      test::boundedBatonWait(
          state->done,
          "ASN migration dynamic peer cleanup",
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
    return state->removed.load(std::memory_order_acquire);
  }
};

TEST_F(E2EAsnMigrationTest, ReportsAcceptedAdditionalAsWithoutMutatingConfig) {
  auto peer = makeV4Peer(kDefaultPeerSpec3, kPeerAsn3, kAsn1);
  setupPeers({peer});

  establishV4Peer(peer.peerAddr, kAsn1);

  const auto config = config_->getConfigOfAPeer(peer.peerAddr);
  ASSERT_TRUE(config.has_value());
  EXPECT_EQ(kPeerAsn3, config->peerAsn);
  EXPECT_EQ(kAsn1, config->additionalRemoteAs);

  const auto runtime = getRuntimePeerState(peer.peerAddr);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(kAsn1, runtime->remoteAs);
  EXPECT_EQ(BgpSessionType::IBGP, runtime->sessionType);
  EXPECT_EQ(kAsn1, getReportedRemoteAs(peer.peerAddr));
}

TEST_F(E2EAsnMigrationTest, SessionFlapReclassifiesUsingNewlyAcceptedAsn) {
  auto peer = makeV4Peer(kDefaultPeerSpec3, kPeerAsn3, kAsn1);
  setupPeers({peer});

  establishV4Peer(peer.peerAddr, kAsn1);
  auto runtime = getRuntimePeerState(peer.peerAddr);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(kAsn1, runtime->remoteAs);
  EXPECT_EQ(BgpSessionType::IBGP, runtime->sessionType);

  bringDownPeerAndWait(peer.peerAddr);
  establishV4Peer(peer.peerAddr, kPeerAsn3);
  runtime = getRuntimePeerState(peer.peerAddr);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(kPeerAsn3, runtime->remoteAs);
  EXPECT_EQ(BgpSessionType::EBGP, runtime->sessionType);
  EXPECT_EQ(kPeerAsn3, getReportedRemoteAs(peer.peerAddr));

  bringDownPeerAndWait(peer.peerAddr);
  establishV4Peer(peer.peerAddr, kAsn1);
  runtime = getRuntimePeerState(peer.peerAddr);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(kAsn1, runtime->remoteAs);
  EXPECT_EQ(BgpSessionType::IBGP, runtime->sessionType);
}

TEST_F(E2EAsnMigrationTest, EffectiveIbgpPreservesOutboundRouteAttributes) {
  auto source = makeV4Peer(kDefaultPeerSpec3, kAsn1, std::nullopt);
  source.isRrClient = true;
  auto destination = makeV4Peer(kDefaultPeerSpec4, kPeerAsn4, kAsn1);
  setupPeers({source, destination});
  establishV4Peers({{source.peerAddr, kAsn1}, {destination.peerAddr, kAsn1}});

  addRoute(
      "v4",
      kTestPrefix,
      kTestPrefixLength,
      source.peerAddr,
      kTestNexthop,
      kInputAsPath,
      "",
      /*addPathId=*/0,
      /*localPref=*/200,
      /*med=*/50);
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  BgpPeerId destinationId{
      destination.peerAddr, destination.peerAddr.asV4().toLongHBO()};
  auto update = waitForOutboundUpdate(destinationId);
  ASSERT_TRUE(update.has_value());
  ASSERT_NE(nullptr, *update);
  EXPECT_TRUE(findPrefixInAnnouncements(
      **update, /*isV4=*/true, prefix, /*addPathId=*/0));
  EXPECT_TRUE(verifyRouteAttributes(**update, kTestNexthop, kInputAsPath, ""));
  ASSERT_TRUE((*update)->attrs()->localPref().has_value());
  EXPECT_EQ(200, *(*update)->attrs()->localPref());
}

TEST_F(E2EAsnMigrationTest, EffectiveEbgpRewritesOutboundRouteAttributes) {
  auto source = makeV4Peer(kDefaultPeerSpec3, kAsn1, std::nullopt);
  auto destination = makeV4Peer(kDefaultPeerSpec4, kAsn1, kPeerAsn4);
  setupPeers({source, destination});
  establishV4Peers(
      {{source.peerAddr, kAsn1}, {destination.peerAddr, kPeerAsn4}});

  addRoute(
      "v4",
      kTestPrefix,
      kTestPrefixLength,
      source.peerAddr,
      kTestNexthop,
      kInputAsPath,
      "",
      /*addPathId=*/0,
      /*localPref=*/200,
      /*med=*/50);
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  BgpPeerId destinationId{
      destination.peerAddr, destination.peerAddr.asV4().toLongHBO()};
  auto update = waitForOutboundUpdate(destinationId);
  ASSERT_TRUE(update.has_value());
  ASSERT_NE(nullptr, *update);
  EXPECT_TRUE(findPrefixInAnnouncements(
      **update, /*isV4=*/true, prefix, /*addPathId=*/0));
  EXPECT_TRUE(verifyRouteAttributes(
      **update,
      destination.v4Nexthop.str(),
      std::to_string(kAsn1) + " " + kInputAsPath,
      ""));
  EXPECT_FALSE((*update)->attrs()->localPref().has_value());
}

TEST_F(E2EAsnMigrationTest, AcceptedRemoteAsSuppressesLoopedAdvertisement) {
  auto source = makeV4Peer(kDefaultPeerSpec3, kPeerAsn3, std::nullopt);
  auto destination = makeV4Peer(kDefaultPeerSpec4, kPeerAsn4, kPeerAsn5);
  setupPeers({source, destination});
  establishV4Peers(
      {{source.peerAddr, kPeerAsn3}, {destination.peerAddr, kPeerAsn5}});

  addRoute(
      "v4",
      kTestPrefix,
      kTestPrefixLength,
      source.peerAddr,
      kTestNexthop,
      std::to_string(kPeerAsn5));
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));
  drainRouteProcessing();

  EXPECT_EQ(false, getRibOutAdvertised(destination.peerAddr, prefix));
}

TEST_F(
    E2EAsnMigrationTest,
    ConfiguredRemoteAsDoesNotSuppressAdditionalAsSession) {
  auto source = makeV4Peer(kDefaultPeerSpec3, kPeerAsn3, std::nullopt);
  auto destination = makeV4Peer(kDefaultPeerSpec4, kPeerAsn4, kPeerAsn5);
  setupPeers({source, destination});
  establishV4Peers(
      {{source.peerAddr, kPeerAsn3}, {destination.peerAddr, kPeerAsn5}});

  addRoute(
      "v4",
      kTestPrefix,
      kTestPrefixLength,
      source.peerAddr,
      kTestNexthop,
      std::to_string(kPeerAsn4));
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  ASSERT_TRUE(waitForRouteInShadowRib(prefix));

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      kTestPrefix,
      kTestPrefixLength,
      destination.peerAddr,
      destination.v4Nexthop.str(),
      std::to_string(kAsn1) + " " + std::to_string(kPeerAsn4)));
}

TEST_F(
    E2EAsnMigrationTest,
    UpdateGroupsSeparatePeersWithDifferentEffectiveSessionTypes) {
  auto first = makeV4Peer(kDefaultPeerSpec3, kPeerAsn3, kAsn1);
  auto second = first;
  second.peerAddr = kPeerAddr4;
  setupPeers({first, second}, /*enableUpdateGroup=*/true);

  establishV4Peers({{first.peerAddr, kPeerAsn3}, {second.peerAddr, kAsn1}});

  const auto firstState = getRuntimePeerState(first.peerAddr);
  const auto secondState = getRuntimePeerState(second.peerAddr);
  ASSERT_TRUE(firstState.has_value());
  ASSERT_TRUE(secondState.has_value());
  ASSERT_NE(nullptr, firstState->updateGroup);
  ASSERT_NE(nullptr, secondState->updateGroup);
  EXPECT_EQ(BgpSessionType::EBGP, firstState->sessionType);
  EXPECT_EQ(BgpSessionType::IBGP, secondState->sessionType);
  EXPECT_NE(firstState->updateGroup, secondState->updateGroup);
}

TEST_F(
    E2EAsnMigrationTest,
    UpdateGroupsMergePeersWithSameEffectiveSessionType) {
  auto first = makeV4Peer(kDefaultPeerSpec3, kAsn1, kPeerAsn5);
  auto second = first;
  second.peerAddr = kPeerAddr4;
  second.asn = kPeerAsn4;
  setupPeers({first, second}, /*enableUpdateGroup=*/true);

  establishV4Peers({{first.peerAddr, kPeerAsn5}, {second.peerAddr, kPeerAsn5}});

  const auto firstState = getRuntimePeerState(first.peerAddr);
  const auto secondState = getRuntimePeerState(second.peerAddr);
  ASSERT_TRUE(firstState.has_value());
  ASSERT_TRUE(secondState.has_value());
  ASSERT_NE(nullptr, firstState->updateGroup);
  ASSERT_NE(nullptr, secondState->updateGroup);
  EXPECT_EQ(BgpSessionType::EBGP, firstState->sessionType);
  EXPECT_EQ(BgpSessionType::EBGP, secondState->sessionType);
  EXPECT_EQ(firstState->updateGroup, secondState->updateGroup);
}

TEST_F(E2EAsnMigrationTest, EffectiveAsDrivesConfedSessionType) {
  auto peer = makeV4Peer(kDefaultPeerSpec3, kAsn1, kPeerAsn4);
  peer.isConfedPeer = true;
  peer.localConfedAsn = kConfedParentAsn;
  setupPeers({peer});

  establishV4Peer(peer.peerAddr, kPeerAsn4);

  const auto runtime = getRuntimePeerState(peer.peerAddr);
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(kPeerAsn4, runtime->remoteAs);
  EXPECT_EQ(BgpSessionType::ConfedEBGP, runtime->sessionType);
}

TEST_F(E2EAsnMigrationTest, EffectiveVipAsDrivesDynamicPeerCleanup) {
  BgpStats::setRunningVipSessions(0);
  auto peer = createBgpPeer(
      kPeerAsn3,
      kLocalAddr2,
      kPeerPrefix4,
      kNextHopV4_2,
      kNextHopV6_2,
      true,
      kPeerTypeShiv);
  peer.additional_remote_as_4_byte() = kVipAsn;
  peers_.push_back(std::move(peer));
  createRib();
  createPeerManager(
      /*enableUpdateGroup=*/false,
      /*enableEgressBackpressure=*/false);

  bringUpPeerWithRemoteAs(kDynamicPeerAddr4, kVipAsn);
  ASSERT_TRUE(waitForSessionEstablished(kDynamicPeerAddr4));
  auto* stats = facebook::fb303::ThreadCachedServiceData::get();
  stats->publishStats();
  EXPECT_EQ(1, stats->getCounter(BgpStats::kRunningVipSessions));

  bringDownPeerAndWait(kDynamicPeerAddr4);
  ASSERT_TRUE(waitForAdjRibRemoval(kDynamicPeerAddr4));
  stats->publishStats();
  EXPECT_EQ(0, stats->getCounter(BgpStats::kRunningVipSessions));
}

TEST_F(
    E2EAsnMigrationTest,
    ConfiguredVipDoesNotDriveDynamicCleanupForNonVipSession) {
  BgpStats::setRunningVipSessions(0);
  auto peer = createBgpPeer(
      kVipAsn,
      kLocalAddr2,
      kPeerPrefix4,
      kNextHopV4_2,
      kNextHopV6_2,
      true,
      kPeerTypeShiv);
  peer.additional_remote_as_4_byte() = kPeerAsn3;
  peers_.push_back(std::move(peer));
  createRib();
  createPeerManager(
      /*enableUpdateGroup=*/false,
      /*enableEgressBackpressure=*/false);

  bringUpPeerWithRemoteAs(kDynamicPeerAddr4, kPeerAsn3);
  ASSERT_TRUE(waitForSessionEstablished(kDynamicPeerAddr4));
  auto* stats = facebook::fb303::ThreadCachedServiceData::get();
  stats->publishStats();
  EXPECT_EQ(0, stats->getCounter(BgpStats::kRunningVipSessions));

  bringDownPeerAndWait(kDynamicPeerAddr4);
  EXPECT_NE(nullptr, getAdjRibByAddr(kDynamicPeerAddr4));
}

} // namespace
} // namespace facebook::bgp
