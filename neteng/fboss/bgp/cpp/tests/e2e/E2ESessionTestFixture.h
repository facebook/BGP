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

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

namespace facebook::bgp {

class E2ETestSessionManager;

/*
 * E2ESessionTestFixture: E2E test fixture that routes session events
 * through the real notifyCoroQueue pipeline via E2ETestSessionManager.
 *
 * Key difference from E2ETestFixture: bringUpPeer/bringDownPeer push
 * events through the coro queue, so PeerManagerBase::processPeerEventLoop()
 * processes them asynchronously. Tests must call waitForSessionEstablished()
 * or use bringUpPeerAndWait() to ensure processing completes.
 */
class E2ESessionTestFixture : public E2ETestFixture {
 protected:
  void TearDown() override;

  void createPeerManager(
      bool enableUpdateGroup = true,
      bool enableEgressBackpressure = true,
      bool enableSerializeGroupPdu = false);

  void bringUpPeer(
      const folly::IPAddress& peerAddr,
      uint64_t versionNumber = 0);

  void bringUpPeerAndWait(
      const folly::IPAddress& peerAddr,
      uint64_t versionNumber = 0,
      int maxRetries = 50);

  void bringDownPeer(const folly::IPAddress& peerAddr);

  void bringDownPeerAndWait(
      const folly::IPAddress& peerAddr,
      int maxRetries = 50);

  bool waitForSessionEstablished(
      const folly::IPAddress& peerAddr,
      int maxRetries = 50);

  bool waitForSessionTerminated(
      const folly::IPAddress& peerAddr,
      int maxRetries = 50);

  std::optional<PeeringParams> getAdjRibPeeringParams(
      const folly::IPAddress& peerAddr);

  /*
   * Override base accessors to read fresh from
   * testSessionManager_->getPeerStates() on every call. After
   * E2ETestSessionManager::restartSession swaps queues, callers must see
   * the new shared_ptrs immediately — never the closed pre-restart queues.
   * T270956942.
   */
  std::optional<PeerQueues> getPeerQueues(
      const BgpPeerId& peerId) const override;

  std::unordered_map<BgpPeerId, PeerQueues> getAllPeerQueues() const override;

  std::optional<BgpPeerId> findPeerIdByAddress(
      const folly::IPAddress& peerAddr) const override;

  std::shared_ptr<E2ETestSessionManager> testSessionManager_;
};

} // namespace facebook::bgp
