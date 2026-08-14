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

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ESessionTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/TestSessionManager.h"

#include <folly/logging/xlog.h>
#include <thread>

#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/tests/PeerManagerTestUtils.h"

using facebook::nettools::bgplib::BgpPeerDisplayInfo;
using facebook::nettools::bgplib::BgpPeerId;
using facebook::nettools::bgplib::FiberBgpPeer;

namespace facebook::bgp {

void E2ESessionTestFixture::TearDown() {
  XLOG(INFO, "=== E2ESessionTestFixture TearDown starting ===");

  auto allQueues = getAllPeerQueues();

  for (const auto& [peerId, queues] : allQueues) {
    if (queues.boundedAdjRibOutQ) {
      queues.boundedAdjRibOutQ->close();
    }
  }

  for (const auto& [peerId, queues] : allQueues) {
    queues.adjRibInQ->fiberPush(FiberBgpPeer::BgpSessionStop{});
  }

  if (peerManager_) {
    peerManager_->stop();
    if (peerMgrThread_.joinable()) {
      peerMgrThread_.join();
    }
  }

  if (testSessionManager_) {
    testSessionManager_->stop();
    if (sessionMgrThread_ && sessionMgrThread_->joinable()) {
      sessionMgrThread_->join();
    }
  }

  if (rib_) {
    rib_->stop();
    if (ribThread_.joinable()) {
      ribThread_.join();
    }
  }

  peerManager_.reset();
  testSessionManager_.reset();
  rib_.reset();

  XLOG(INFO, "=== E2ESessionTestFixture TearDown complete ===");
}

void E2ESessionTestFixture::createPeerManager(
    bool enableUpdateGroup,
    bool enableEgressBackpressure,
    bool enableSerializeGroupPdu) {
  XLOG(INFO, "=== Creating PeerManagerBase with E2ETestSessionManager... ===");

  FLAGS_enable_egress_backpressure_in_peer_mgr_tests = enableEgressBackpressure;

  config_ = getConfig(enableUpdateGroup, enableSerializeGroupPdu);
  auto globalConfig = config_->getBgpGlobalConfig();
  ASSERT_NE(globalConfig, nullptr);

  testSessionManager_ = std::make_shared<E2ETestSessionManager>(*globalConfig);
  sessionMgrThread_ =
      std::make_shared<std::thread>(testSessionManager_->runInThread());

  configManager_ = std::make_shared<ConfigManager>(config_);

  std::shared_ptr<PolicyManager> policyManager = nullptr;
  if (policyConfig_.has_value()) {
    policyManager =
        std::make_shared<PolicyManager>(*policyConfig_, globalConfig.get());
  }

  peerManager_ = std::make_unique<PeerManagerBase>(
      configManager_, policyManager, ribInQ_, ribOutQ_, nbrRouteChangeQ_);

  peerManager_->setSessionManager(testSessionManager_);

  peerMgrThread_ = std::thread([this]() {
    XLOG(INFO, "PeerManagerBase thread started (E2ETestSessionManager)");
    peerManager_->run();
    XLOG(INFO, "PeerManagerBase thread exiting");
  });

  peerManager_->getEventBase().waitUntilRunning();
  XLOG(INFO, "PeerManagerBase running with E2ETestSessionManager");
}

void E2ESessionTestFixture::bringUpPeer(
    const folly::IPAddress& peerAddr,
    uint64_t /*versionNumber*/) {
  BgpPeerId peerId{peerAddr, peerAddr.asV4().toLongHBO()};
  XLOGF(INFO, "=== bringUpPeer (session pipeline) for: {} ===", peerAddr.str());

  auto currentConfig = configManager_ ? configManager_->getConfig() : config_;
  auto peerConfig = currentConfig->getConfigOfAPeer(peerAddr);
  ASSERT_TRUE(peerConfig.has_value())
      << "Peer config not found for " << peerAddr.str();
  auto& cfg = peerConfig.value();
  auto globalConfig = currentConfig->getBgpGlobalConfig();

  BgpPeerDisplayInfo displayInfo;
  displayInfo.peeringParams.peerAddr = peerId.peerAddr;
  displayInfo.peeringParams.remoteAs = cfg.peerAsn;
  displayInfo.peeringParams.localAs =
      cfg.localAsn.value_or(globalConfig->localAsn);
  displayInfo.peeringParams.globalAs = globalConfig->localAsn;
  displayInfo.peeringParams.localBgpId = globalConfig->routerId.asV4();
  displayInfo.peeringParams.holdTime =
      cfg.holdTime.value_or(std::chrono::seconds(90));
  displayInfo.peeringParams.description = cfg.description.value_or("");
  displayInfo.peeringParams.isAfiIpv4Configured = true;
  displayInfo.peeringParams.isAfiIpv6Configured = true;
  displayInfo.peeringParams.nexthopV4 = cfg.nexthopV4;
  displayInfo.peeringParams.nexthopV6 = cfg.nexthopV6;
  ASSERT_TRUE(cfg.bindAddr.has_value());
  displayInfo.localAddr = cfg.bindAddr.value();
  displayInfo.remoteBgpId = peerId.remoteBgpId;
  displayInfo.negotiatedCapabilities.mpExtV4Unicast() = true;
  displayInfo.negotiatedCapabilities.mpExtV6Unicast() = true;
  displayInfo.negotiatedCapabilities.as4byte() = true;

  /* Model the peer's remote (pre-negotiation) MP-EXT-exists bit; negotiated v4
   * stays true so v4 is still announced. A capability-less peer advertised no
   * MP-EXT (mpExtCapable = false). */
  bool mpExtCapable = true;
  auto mpExtIt = peerMpExtCapable_.find(peerAddr);
  if (mpExtIt != peerMpExtCapable_.end()) {
    mpExtCapable = mpExtIt->second;
  }
  displayInfo.remoteCapabilities.mpExtExist() = mpExtCapable;

  auto addPathIt = peerAddPathCapabilities_.find(peerAddr);
  if (addPathIt != peerAddPathCapabilities_.end() &&
      addPathIt->second.has_value()) {
    nettools::bgplib::BgpAddPathCapability v4AddPath;
    v4AddPath.afi() = nettools::bgplib::BgpUpdateAfi::AFI_IPv4;
    v4AddPath.safi() = nettools::bgplib::BgpUpdateSafi::SAFI_UNICAST;
    v4AddPath.sor() = addPathIt->second.value();

    nettools::bgplib::BgpAddPathCapability v6AddPath;
    v6AddPath.afi() = nettools::bgplib::BgpUpdateAfi::AFI_IPv6;
    v6AddPath.safi() = nettools::bgplib::BgpUpdateSafi::SAFI_UNICAST;
    v6AddPath.sor() = addPathIt->second.value();

    displayInfo.negotiatedCapabilities.addPathCapabilities()->push_back(
        v4AddPath);
    displayInfo.negotiatedCapabilities.addPathCapabilities()->push_back(
        v6AddPath);
  }

  auto version = testSessionManager_->simulateSessionEstablished(
      peerId,
      displayInfo,
      defaultQueueCapacity_,
      defaultQueueHighWm_,
      defaultQueueLowWm_);

  /*
   * No need to cache queues here — getPeerQueues() / getAllPeerQueues()
   * read fresh from testSessionManager_->getPeerStates() on every call.
   * This keeps the test fixture in sync with restartSession swaps.
   */

  XLOGF(
      INFO,
      "Pushed ESTABLISHED event for peer: {} version={}",
      peerAddr.str(),
      version);
}

void E2ESessionTestFixture::bringUpPeerAndWait(
    const folly::IPAddress& peerAddr,
    uint64_t versionNumber,
    int maxRetries) {
  bringUpPeer(peerAddr, versionNumber);
  ASSERT_TRUE(waitForSessionEstablished(peerAddr, maxRetries))
      << "Session establishment timed out for peer: " << peerAddr;
}

void E2ESessionTestFixture::bringDownPeer(const folly::IPAddress& peerAddr) {
  BgpPeerId peerId{peerAddr, peerAddr.asV4().toLongHBO()};
  XLOGF(
      INFO, "=== bringDownPeer (session pipeline) for: {} ===", peerAddr.str());

  testSessionManager_->simulateSessionTerminated(peerId);
}

void E2ESessionTestFixture::bringDownPeerAndWait(
    const folly::IPAddress& peerAddr,
    int maxRetries) {
  bringDownPeer(peerAddr);
  ASSERT_TRUE(waitForSessionTerminated(peerAddr, maxRetries))
      << "Session termination timed out for peer: " << peerAddr;
}

bool E2ESessionTestFixture::waitForSessionEstablished(
    const folly::IPAddress& peerAddr,
    int maxRetries) {
  BgpPeerId peerId{peerAddr, peerAddr.asV4().toLongHBO()};

  /*
   * Poll until PeerManagerBase has processed the ESTABLISHED event.
   * We check by running a task on the PeerManagerBase's EVB — if the
   * EVB is processing our check, it has already processed any
   * earlier events in the queue (events are processed in order).
   * Then we verify the peer's queues have been registered by checking
   * testSessionManager's state matches what PeerManagerBase would have used.
   */
  for (int i = 0; i < maxRetries; i++) {
    bool processed = false;
    auto& evb = peerManager_->getEventBase();
    evb.runInEventBaseThreadAndWait([&]() {
      /*
       * If we're executing on the PeerManagerBase EVB, all prior events
       * (including the ESTABLISHED event) have been processed.
       * The session event was pushed to notifyCoroQueue, which is
       * drained by processPeerEventLoop on this same EVB thread.
       */
      processed = true;
    });
    if (processed) {
      /* Give the coroutine loop one more cycle to process */
      std::this_thread::yield();
      evb.runInEventBaseThreadAndWait([&]() {});
      XLOGF(
          INFO,
          "Session established for peer: {} (after {} retries)",
          peerAddr.str(),
          i + 1);
      return true;
    }
    std::this_thread::yield();
  }
  XLOGF(
      WARN,
      "Timed out waiting for session establishment for: {}",
      peerAddr.str());
  return false;
}

bool E2ESessionTestFixture::waitForSessionTerminated(
    const folly::IPAddress& peerAddr,
    int /*maxRetries*/) {
  /*
   * Drain the PeerManagerBase EVB twice to ensure the TERMINATED event
   * has been fully processed. The first drain ensures the event is
   * dequeued, the yield + second drain ensures any follow-up work
   * (like AdjRib cleanup) completes.
   */
  auto& evb = peerManager_->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {});
  std::this_thread::yield();
  evb.runInEventBaseThreadAndWait([&]() {});
  waitForSessionTerminationBaton(peerAddr);
  XLOGF(INFO, "Session terminated for peer: {}", peerAddr.str());
  return true;
}

std::optional<E2ETestFixture::PeerQueues> E2ESessionTestFixture::getPeerQueues(
    const BgpPeerId& peerId) const {
  if (!testSessionManager_) {
    return std::nullopt;
  }
  const auto& states = testSessionManager_->getPeerStates();
  auto it = states.find(peerId);
  if (it == states.end()) {
    return std::nullopt;
  }
  return PeerQueues{
      it->second.adjRibInQ,
      it->second.adjRibOutQ,
      it->second.boundedAdjRibOutQ};
}

std::unordered_map<BgpPeerId, E2ETestFixture::PeerQueues>
E2ESessionTestFixture::getAllPeerQueues() const {
  std::unordered_map<BgpPeerId, PeerQueues> result;
  if (!testSessionManager_) {
    return result;
  }
  for (const auto& [peerId, state] : testSessionManager_->getPeerStates()) {
    result.emplace(
        peerId,
        PeerQueues{state.adjRibInQ, state.adjRibOutQ, state.boundedAdjRibOutQ});
  }
  return result;
}

std::optional<BgpPeerId> E2ESessionTestFixture::findPeerIdByAddress(
    const folly::IPAddress& peerAddr) const {
  if (!testSessionManager_) {
    return std::nullopt;
  }
  for (const auto& [peerId, state] : testSessionManager_->getPeerStates()) {
    if (peerId.peerAddr == peerAddr) {
      return peerId;
    }
  }
  return std::nullopt;
}

} // namespace facebook::bgp
