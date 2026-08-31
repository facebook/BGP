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

#include <gmock/gmock.h>

#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/test/TestLogHandler.h>

#include <neteng/fboss/bgp/cpp/config/ConfigStructs.h>
#include <neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeerManager.h>
#include <neteng/fboss/bgp/cpp/lib/fibers/Utils.h>

namespace {
const folly::IPAddress kR1Lo1 = folly::IPAddress("127.1.0.1");
const folly::IPAddress kR1Lo2 = folly::IPAddress("127.1.0.2");
const folly::IPAddress kR1Lo3 = folly::IPAddress("127.1.0.3");
const folly::IPAddress kR1Lo4 = folly::IPAddress("127.1.0.4");

const folly::IPAddress kR2Lo1 = folly::IPAddress("127.2.0.1");
const folly::IPAddress kR2Lo2 = folly::IPAddress("127.2.0.2");
const folly::IPAddress kR2Lo3 = folly::IPAddress("127.2.0.3");
const folly::IPAddress kR2Lo4 = folly::IPAddress("127.2.0.4");

const folly::CIDRNetwork kR1PfxSlash30 =
    folly::IPAddress::createNetwork("127.1.0.0/30");
const folly::CIDRNetwork kR2PfxSlash30 =
    folly::IPAddress::createNetwork("127.2.0.0/30");

const uint32_t kFbAsn = 32934;
} // namespace

using facebook::nettools::bgplib::BgpPeerId;
using facebook::nettools::bgplib::FiberServerSocket;

// Helper to account the number of times each callback routine is called.
class TestFiberBgpPeerCallback
    : public facebook::nettools::bgplib::FiberBgpPeerCallback {
 public:
  void sessionEstablished(const BgpPeerId& peerId) noexcept override {
    establishedCallbackCount[peerId]++;
    if (!isSessionUp(peerId)) {
      establishedPeers.insert(peerId);
    }
  }

  void sessionTerminated(const BgpPeerId& peerId) noexcept override {
    terminatedCallbackCount[peerId]++;
    if (isSessionUp(peerId)) {
      establishedPeers.erase(peerId);
    }
  }

  void bgpUpdatesReceived(
      const BgpPeerId& peerId,
      const facebook::nettools::bgplib::BgpUpdate2& update) noexcept override {
    ASSERT_TRUE(isSessionUp(peerId));
    bgpUpdatesReceivedCallbackData[peerId].push_back(update);
  }

  void bgpEndOfRibReceived(
      const BgpPeerId& peerId,
      const facebook::nettools::bgplib::BgpEndOfRib& eor) noexcept override {
    ASSERT_TRUE(isSessionUp(peerId));
    bgpEndOfRibReceivedCallbackData[peerId].push_back(eor);
  }

  void bgpRouteRefreshReceived(
      const BgpPeerId& peerId,
      const facebook::nettools::bgplib::BgpRouteRefresh& routeRefresh) noexcept
      override {
    ASSERT_TRUE(isSessionUp(peerId));
    bgpRouteRefreshReceivedCallbackData[peerId].push_back(routeRefresh);
  }

  bool isSessionUp(const BgpPeerId& peerId) {
    if (establishedPeers.find(peerId) != establishedPeers.end()) {
      return true;
    }
    return false;
  }

  uint32_t getEstablishedCallbackCount(const BgpPeerId& peerId) {
    auto it = establishedCallbackCount.find(peerId);
    if (it == establishedCallbackCount.end()) {
      return 0;
    }
    return it->second;
  }

  uint32_t getTerminatedCallbackCount(const BgpPeerId& peerId) {
    auto it = terminatedCallbackCount.find(peerId);
    if (it == terminatedCallbackCount.end()) {
      return 0;
    }
    return it->second;
  }

  std::vector<facebook::nettools::bgplib::BgpUpdate2>
  getBgpUpdatesReceivedCallbackData(const BgpPeerId& peerId) {
    auto it = bgpUpdatesReceivedCallbackData.find(peerId);
    if (it == bgpUpdatesReceivedCallbackData.end()) {
      return {};
    }
    return it->second;
  }

  std::vector<facebook::nettools::bgplib::BgpEndOfRib>
  getBgpEndOfRibReceivedCallbackData(const BgpPeerId& peerId) {
    auto it = bgpEndOfRibReceivedCallbackData.find(peerId);
    if (it == bgpEndOfRibReceivedCallbackData.end()) {
      return {};
    }
    return it->second;
  }

  std::vector<facebook::nettools::bgplib::BgpRouteRefresh>
  getBgpRouteRefreshReceivedCallbackData(const BgpPeerId& peerId) {
    auto it = bgpRouteRefreshReceivedCallbackData.find(peerId);
    if (it == bgpRouteRefreshReceivedCallbackData.end()) {
      return {};
    }
    return it->second;
  }

 private:
  std::unordered_set<BgpPeerId> establishedPeers;
  std::unordered_map<BgpPeerId, uint32_t> establishedCallbackCount;
  std::unordered_map<BgpPeerId, uint32_t> terminatedCallbackCount;
  std::unordered_map<
      BgpPeerId,
      std::vector<facebook::nettools::bgplib::BgpUpdate2>>
      bgpUpdatesReceivedCallbackData;
  std::unordered_map<
      BgpPeerId,
      std::vector<facebook::nettools::bgplib::BgpEndOfRib>>
      bgpEndOfRibReceivedCallbackData;
  std::unordered_map<
      BgpPeerId,
      std::vector<facebook::nettools::bgplib::BgpRouteRefresh>>
      bgpRouteRefreshReceivedCallbackData;
};

class TestFiberBgpPeerManager
    : public facebook::nettools::bgplib::FiberBgpPeerManager {
 public:
  TestFiberBgpPeerManager(
      const facebook::bgp::BgpGlobalConfig& config,
      TestFiberBgpPeerCallback* callback,
      folly::fibers::FiberManager& fm,
      folly::EventBase& evb,
      bool enableMessagesOverNotifyQueue = true,
      bool enableCoroNotifyQueue = false)
      : facebook::nettools::bgplib::FiberBgpPeerManager(
            config,
            fm,
            evb,
            enableMessagesOverNotifyQueue,
            enableCoroNotifyQueue),
        callback_(callback) {
    if (enableCoroNotifyQueue) {
      // skip processing non-coro callback for now
      return;
    }

    // fiber which converts notifications to callback
    fm.addTask([this, callback]() mutable {
      facebook::nettools::bgplib::BgpPeerManagerEventObserver visitor{callback};
      auto reader = getNotifyQueue();
      while (true) {
        auto msg = reader.get();
        if (!msg) {
          break;
        }
        std::visit(visitor, *msg);
      }
    });
  }

  // for testing
  auto getMonitoredItem() {
    return monitoredItems_;
  }

 private:
  // TestFiberBgpPeerCallback for listening to events about FiberBgpPeers
  TestFiberBgpPeerCallback* callback_;
};

class MockFiberBgpPeerManager : public TestFiberBgpPeerManager {
 public:
  MockFiberBgpPeerManager(
      const facebook::bgp::BgpGlobalConfig& config,
      TestFiberBgpPeerCallback* callback,
      folly::fibers::FiberManager& fm,
      folly::EventBase& evb,
      bool enableMessagesOverNotifyQueue = true,
      bool enableCoroNotifyQueue = false)
      : TestFiberBgpPeerManager(
            config,
            callback,
            fm,
            evb,
            enableMessagesOverNotifyQueue,
            enableCoroNotifyQueue) {}

  MOCK_METHOD(
      std::unique_ptr<FiberServerSocket>,
      makeServerSocket,
      (const std::optional<folly::SocketAddress>&),
      (override, const, noexcept));
};

void waitTillSessionsComeUp(
    folly::fibers::FiberManager& fm,
    std::shared_ptr<TestFiberBgpPeerManager> peerMgr,
    const std::unordered_set<BgpPeerId>& peerSet,
    const std::chrono::seconds& timeout = std::chrono::seconds(5));

void waitTillSessionsGoDown(
    folly::fibers::FiberManager& fm,
    std::shared_ptr<TestFiberBgpPeerManager> peerMgr,
    const std::unordered_set<BgpPeerId>& peerSet,
    const std::chrono::seconds& timeout = std::chrono::seconds(5));

inline facebook::bgp::BgpGlobalConfig makeBgpGlobalConfig(
    folly::IPAddress routerId,
    folly::IPAddress clusterId,
    const std::optional<folly::SocketAddress>& listenAddr = std::nullopt,
    bool enableEgressQueueBackpressure = false) {
  return facebook::bgp::BgpGlobalConfig{
      100, /* localAsn */
      routerId, /* routerId */
      clusterId, /* clusterId */
      std::chrono::seconds(180), /* holdTime */
      listenAddr, /* listenAddr */
      std::chrono::seconds(600), /* grRestartTime */
      std::unordered_map<
          folly::CIDRNetwork,
          facebook::bgp::thrift::BgpNetwork>(), /* networksV4 */
      std::unordered_map<
          folly::CIDRNetwork,
          facebook::bgp::thrift::BgpNetwork>(), /* networksV6 */
      std::nullopt, /* localConfedAsn */
      facebook::bgp::ComputeUcmpFromLbwComm{false}, /* computeUcmpFromLbwComm */
      0, /* ucmpWidth */
      std::nullopt, /* ucmpQuantizer */
      facebook::bgp::ValidateRemoteAs{true}, /* validateRemoteAs */
      facebook::bgp::SupportStatefulGr{true}, /* supportStatefulGr */
      facebook::bgp::EnableServerSocket{true}, /* enableServerSocket */
      facebook::bgp::AllowLoopbackReflection{
          false}, /* allowLoopbackReflection */
      facebook::bgp::CountConfedsInAsPathLen{
          false}, /* countConfedsInAsPathLen */
      std::nullopt, /* deviceName */
      std::nullopt, /* switchLimitConfig */
      std::nullopt, /* dynamicPeerLimit */
      std::nullopt, /* streamSubscriberLimit */
      facebook::bgp::EnableNexthopTracking{false}, /* enableNextHopTracking */
      std::vector<std::string>{}, /* includeInterfaceRegexes */
      facebook::bgp::EnableDynamicPolicyEvaluation{
          false}, /* enableDynamicPolicyEvaluation */
      std::nullopt, /* thriftServerConfig */
      enableEgressQueueBackpressure, /* enableEgressQueueBackpressure */
  };
}

inline std::vector<std::pair<folly::LogMessage, const folly::LogCategory*>>&
subscribeToLogMessages(
    const std::string& category,
    folly::LogLevel logLevel = folly::LogLevel::DBG1) {
  auto handler = std::make_shared<folly::TestLogHandler>();
  folly::LoggerDB::get().getCategory(category)->addHandler(handler);
  folly::LoggerDB::get().setLevel(category, logLevel);
  return handler->getMessages();
}
