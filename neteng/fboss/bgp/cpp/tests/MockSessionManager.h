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
#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"

namespace facebook::bgp {

using AdjRibInRWQueueT =
    bgp::MonitoredRWQueue<nettools::bgplib::FiberBgpPeer::OutputMessageT>;

class MockSessionManager : public SessionManager {
 public:
  explicit MockSessionManager(
      const bgp::BgpGlobalConfig& config,
      bool enableMessagesOverNotifyQueue = true,
      std::shared_ptr<AdjRib::AdjRibOutQueueT> boundedIqueue = nullptr,
      std::shared_ptr<AdjRib::AdjRibInQueueT> oQueue = nullptr,
      bool enableCoroNotifyQueue = true)
      : SessionManager(
            config,
            enableMessagesOverNotifyQueue,
            enableCoroNotifyQueue) {
    boundedIqueue_ = boundedIqueue;
    oQueue_ = oQueue;
  }
  MOCK_METHOD(
      (folly::Expected<folly::Unit, SessionManager::ErrorCode>),
      stopDynamicPeerWithGracefulRestart,
      (const folly::CIDRNetwork& peerPrefix));

  MOCK_METHOD(
      (folly::Expected<folly::Unit, SessionManager::ErrorCode>),
      startDynamicPeer,
      (const folly::CIDRNetwork& peerPrefix));

  MOCK_METHOD(
      (folly::Expected<folly::Unit, SessionManager::ErrorCode>),
      shutdownDynamicPeer,
      (const folly::CIDRNetwork& peerPrefix));

  MOCK_METHOD(
      (folly::Expected<folly::Unit, SessionManager::ErrorCode>),
      shutdownPeer,
      (const folly::IPAddress& peerAddr, bool peerDelete));

  MOCK_METHOD(
      (folly::Expected<folly::Unit, SessionManager::ErrorCode>),
      startPeer,
      (const folly::IPAddress& peerAddr));

  MOCK_METHOD(
      (folly::Expected<folly::Unit, SessionManager::ErrorCode>),
      addPeer_,
      (const folly::IPAddress& peerAddr,
       const bgp::PeeringParams& params,
       const nettools::bgplib::ConnTimeParams& connTimeParams));

  MOCK_METHOD(
      (folly::Expected<folly::Unit, SessionManager::ErrorCode>),
      addDynamicPeer_,
      (const folly::CIDRNetwork& peerPrefix, const bgp::PeeringParams& params));

  MOCK_METHOD(
      (folly::Expected<folly::Unit, SessionManager::ErrorCode>),
      stopPeer,
      (const folly::IPAddress& peerAddr, bool withGR));

  MOCK_METHOD(
      std::optional<nettools::bgplib::BgpPeerDisplayInfo>,
      getEstablishedPeerDisplayInfo,
      (const nettools::bgplib::BgpPeerId& peerId));

  MOCK_METHOD(
      bool,
      isPeerVersionValid_,
      (const nettools::bgplib::BgpPeerId& peerId, const uint64_t versionNumber),
      (const));

  // This override method calls mocked method
  bool isPeerVersionValid(
      const nettools::bgplib::BgpPeerId& peerId,
      const uint64_t versionNumber) const noexcept override {
    return isPeerVersionValid_(peerId, versionNumber);
  }

  folly::Expected<folly::Unit, SessionManager::ErrorCode> addPeer(
      const folly::IPAddress& peerAddr,
      const bgp::PeeringParams& params,
      const nettools::bgplib::ConnTimeParams& connTimeParams) override {
    addPeer_(peerAddr, params, connTimeParams);
    return SessionManager::addPeer(peerAddr, params, connTimeParams);
  }

  folly::Expected<folly::Unit, SessionManager::ErrorCode> addDynamicPeer(
      const folly::CIDRNetwork& peerPrefix,
      const bgp::PeeringParams& params) override {
    addDynamicPeer_(peerPrefix, params);
    return SessionManager::addDynamicPeer(peerPrefix, params);
  }

  // Override method for SessionManager
  std::shared_ptr<AdjRib::AdjRibInQueueT> getPeerOutputQueue(
      const nettools::bgplib::BgpPeerId& /* peerId */) noexcept override {
    return oQueue_;
  }

  // Override method for SessionManager
  std::shared_ptr<nettools::bgplib::FiberBgpPeer::InputQueueT>
  getPeerInputQueue(
      const nettools::bgplib::BgpPeerId& /* peerId */) noexcept override {
    return nullptr;
  }

  /* Override method for SessionManager */
  std::shared_ptr<AdjRib::AdjRibOutQueueT> getBoundedPeerInputQueue(
      const nettools::bgplib::BgpPeerId& /* peerId */) noexcept override {
    return boundedIqueue_;
  }

  std::shared_ptr<AdjRib::AdjRibOutQueueT> boundedIqueue_;
  std::shared_ptr<AdjRib::AdjRibInQueueT> oQueue_;
};

} // namespace facebook::bgp
