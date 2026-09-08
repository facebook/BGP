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

#include "neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeerManager.h"

namespace facebook::bgp {

/**
 * SessionManager is the standalone version of FiberBgpPeerManager
 */
class SessionManager : public nettools::bgplib::FiberBgpPeerManager {
 public:
  explicit SessionManager(
      const bgp::BgpGlobalConfig& config,
      bool enableMessagesOverNotifyQueue = true,
      bool enableCoroNotifyQueue = false)
      : FiberBgpPeerManager(
            config,
            enableMessagesOverNotifyQueue,
            enableCoroNotifyQueue) {}

  void run() noexcept override;

  // The public stop interface called in Main.cpp
  void stop() noexcept override;

 private:
  friend class PeerManagerBase;

  // called by PeerManagerBase
  void shutdownWithGR(bool gracefulRestart) noexcept {
    FiberBgpPeerManager::shutdownWithGR(gracefulRestart);
  }

  folly::Future<folly::Unit> mainFiber_;

/*
 * per class placeholder for test code injection
 * only need to be setup once here
 */
#ifdef SessionManager_TEST_FRIENDS
  SessionManager_TEST_FRIENDS
#endif
};

} // namespace facebook::bgp
