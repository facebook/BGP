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

#ifndef NETENG_FBOSS_BGP_CPP_TESTS_E2E_TESTSESSIONMANAGER_H_
#define NETENG_FBOSS_BGP_CPP_TESTS_E2E_TESTSESSIONMANAGER_H_

#include <folly/container/F14Map.h>
#include <folly/futures/Future.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/VersionNumber.h"
#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"

namespace facebook::bgp {

/*
 * E2ETestSessionManager: SessionManager subclass for E2E tests that pushes
 * session events through the real notifyCoroQueue pipeline.
 *
 * Unlike MockSessionManager which returns fixed shared queues,
 * E2ETestSessionManager creates per-peer queues and manages per-peer
 * VersionNumbers — matching production behavior.
 *
 * Session events flow through:
 *   simulateSessionEstablished() → getNotifyCoroQueue().push()
 *     → notifyCoroQueue_ → PeerManagerBase::processPeerEventLoop()
 *     → sessionEstablished() / sessionTerminated()
 */
class E2ETestSessionManager : public SessionManager {
 public:
  explicit E2ETestSessionManager(const bgp::BgpGlobalConfig& config);

  void run() noexcept override;
  void stop() noexcept override;

  bool isPeerVersionValid(
      const nettools::bgplib::BgpPeerId& peerId,
      const uint64_t versionNumber) const noexcept override;

  std::shared_ptr<AdjRib::AdjRibInQueueT> getPeerOutputQueue(
      const nettools::bgplib::BgpPeerId& peerId) noexcept override;
  std::shared_ptr<AdjRib::AdjRibOutQueueT> getPeerInputQueue(
      const nettools::bgplib::BgpPeerId& peerId) noexcept override;
  std::shared_ptr<AdjRib::BoundedAdjRibOutQueueT> getBoundedPeerInputQueue(
      const nettools::bgplib::BgpPeerId& peerId) noexcept override;

  /* ========== Test Simulation API ========== */

  /*
   * Simulate a session becoming ESTABLISHED.
   * Creates per-peer queues, VersionNumber, ObservableSessionInfo.
   * Pushes ObservableStateT through notifyCoroQueue so
   * PeerManagerBase::processPeerEventLoop() processes it.
   *
   * Returns the version number assigned to this session incarnation.
   */
  uint64_t simulateSessionEstablished(
      const nettools::bgplib::BgpPeerId& peerId,
      const nettools::bgplib::BgpPeerDisplayInfo& displayInfo,
      int queueCapacity = 8,
      int queueHighWm = 6,
      int queueLowWm = 2);

  /*
   * Simulate session termination.
   * Bumps VersionNumber and pushes IDLE ObservableStateT through
   * notifyCoroQueue.
   */
  void simulateSessionTerminated(const nettools::bgplib::BgpPeerId& peerId);

  /*
   * Get the current VersionNumber for a peer.
   * Used for version-aware test assertions.
   */
  std::shared_ptr<nettools::bgplib::VersionNumber> getPeerVersionNumber(
      const nettools::bgplib::BgpPeerId& peerId) const;

  struct PeerSessionState {
    std::shared_ptr<AdjRib::AdjRibInQueueT> adjRibInQ;
    std::shared_ptr<AdjRib::AdjRibOutQueueT> adjRibOutQ;
    std::shared_ptr<AdjRib::BoundedAdjRibOutQueueT> boundedAdjRibOutQ;
    std::shared_ptr<nettools::bgplib::VersionNumber> versionNumber;
    nettools::bgplib::BgpPeerDisplayInfo displayInfo;
    bool established{false};
  };

  const folly::F14NodeMap<nettools::bgplib::BgpPeerId, PeerSessionState>&
  getPeerStates() const {
    return peerStates_;
  }

 private:
  folly::F14NodeMap<nettools::bgplib::BgpPeerId, PeerSessionState> peerStates_;
  folly::Future<folly::Unit> mainFiber_;
};

} // namespace facebook::bgp

#endif // NETENG_FBOSS_BGP_CPP_TESTS_E2E_TESTSESSIONMANAGER_H_
