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

#include "neteng/fboss/bgp/cpp/tests/e2e/TestSessionManager.h"

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeer.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeerManager.h"

using facebook::nettools::bgplib::BgpPeerDisplayInfo;
using facebook::nettools::bgplib::BgpPeerId;
using facebook::nettools::bgplib::BgpSessionState;
using facebook::nettools::bgplib::FiberBgpPeer;
using facebook::nettools::bgplib::ObservableEventT;
using facebook::nettools::bgplib::VersionNumber;

namespace facebook::bgp {

E2ETestSessionManager::E2ETestSessionManager(const bgp::BgpGlobalConfig& config)
    : SessionManager(
          config,
          /* enableMessagesOverNotifyQueue */ true,
          /* enableCoroNotifyQueue */ true) {}

void E2ETestSessionManager::run() noexcept {
  mainFiber_ = fm_.addTaskFuture(
      [this] { facebook::nettools::bgplib::FiberBgpPeerManager::run(); });
  evb_.loopForever();
}

void E2ETestSessionManager::stop() noexcept {
  if (!alreadyShutdown_) {
    facebook::nettools::bgplib::FiberBgpPeerManager::shutdownWithGR(false);
  }
  if (mainFiber_.valid()) {
    std::move(mainFiber_).get();
  }
  evb_.terminateLoopSoon();
}

bool E2ETestSessionManager::isPeerVersionValid(
    const BgpPeerId& peerId,
    const uint64_t versionNumber) const noexcept {
  auto it = peerStates_.find(peerId);
  if (it == peerStates_.end() || !it->second.versionNumber) {
    return false;
  }
  return it->second.versionNumber->getWithoutLock() == versionNumber;
}

std::shared_ptr<AdjRib::AdjRibInQueueT>
E2ETestSessionManager::getPeerOutputQueue(const BgpPeerId& peerId) noexcept {
  auto it = peerStates_.find(peerId);
  if (it != peerStates_.end()) {
    return it->second.adjRibInQ;
  }
  return nullptr;
}

std::shared_ptr<AdjRib::AdjRibOutQueueT>
E2ETestSessionManager::getPeerInputQueue(const BgpPeerId& peerId) noexcept {
  auto it = peerStates_.find(peerId);
  if (it != peerStates_.end()) {
    return it->second.adjRibOutQ;
  }
  return nullptr;
}

std::shared_ptr<AdjRib::BoundedAdjRibOutQueueT>
E2ETestSessionManager::getBoundedPeerInputQueue(
    const BgpPeerId& peerId) noexcept {
  auto it = peerStates_.find(peerId);
  if (it != peerStates_.end()) {
    return it->second.boundedAdjRibOutQ;
  }
  return nullptr;
}

uint64_t E2ETestSessionManager::simulateSessionEstablished(
    const BgpPeerId& peerId,
    const BgpPeerDisplayInfo& displayInfo,
    int queueCapacity,
    int queueHighWm,
    int queueLowWm) {
  XLOGF(
      INFO,
      "[E2ETestSessionManager] simulateSessionEstablished for peer: {}",
      peerId.peerAddr.str());

  auto adjRibInQ = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto adjRibOutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();
  auto boundedAdjRibOutQ = std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
      queueCapacity, queueHighWm, queueLowWm);

  auto& state = peerStates_[peerId];
  if (!state.versionNumber) {
    state.versionNumber = std::make_shared<VersionNumber>(1);
  } else {
    state.versionNumber->bumpUp();
  }
  state.adjRibInQ = adjRibInQ;
  state.adjRibOutQ = adjRibOutQ;
  state.boundedAdjRibOutQ = boundedAdjRibOutQ;
  state.displayInfo = displayInfo;
  state.established = true;

  uint64_t version = state.versionNumber->getWithoutLock();

  auto sessionInfo = FiberBgpPeer::getObservableSessionInfo(
      displayInfo,
      adjRibOutQ,
      boundedAdjRibOutQ,
      adjRibInQ,
      state.versionNumber);

  FiberBgpPeer::ObservableStateT stateEvt{
      .peerId = peerId,
      .state = BgpSessionState::ESTABLISHED,
      .versionNumber = version,
      .sessionInfo = std::move(sessionInfo)};

  ObservableEventT obsEvent = std::move(stateEvt);
  getNotifyCoroQueue().push(std::move(obsEvent));

  XLOGF(
      INFO,
      "[E2ETestSessionManager] Pushed ESTABLISHED event, version={}",
      version);
  return version;
}

void E2ETestSessionManager::simulateSessionTerminated(const BgpPeerId& peerId) {
  auto it = peerStates_.find(peerId);
  if (it == peerStates_.end()) {
    XLOGF(
        WARN,
        "[E2ETestSessionManager] simulateSessionTerminated: peer not found: {}",
        peerId.peerAddr.str());
    return;
  }

  XLOGF(
      INFO,
      "[E2ETestSessionManager] simulateSessionTerminated for peer: {}",
      peerId.peerAddr.str());

  auto& state = it->second;

  if (state.boundedAdjRibOutQ) {
    state.boundedAdjRibOutQ->close();
  }

  if (state.adjRibInQ) {
    state.adjRibInQ->fiberPush(FiberBgpPeer::BgpSessionStop{});
  }

  state.versionNumber->bumpUp();
  state.established = false;

  FiberBgpPeer::ObservableStateT stateEvt{
      .peerId = peerId,
      .state = BgpSessionState::IDLE,
      .versionNumber = 0,
      .sessionInfo = nullptr};

  ObservableEventT obsEvent = std::move(stateEvt);
  getNotifyCoroQueue().push(std::move(obsEvent));

  XLOGF(
      INFO,
      "[E2ETestSessionManager] Pushed IDLE event for peer: {}",
      peerId.peerAddr.str());
}

std::shared_ptr<VersionNumber> E2ETestSessionManager::getPeerVersionNumber(
    const BgpPeerId& peerId) const {
  auto it = peerStates_.find(peerId);
  if (it != peerStates_.end()) {
    return it->second.versionNumber;
  }
  return nullptr;
}

} // namespace facebook::bgp
