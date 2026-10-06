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

#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"

namespace facebook::bgp {

void SessionManager::run() noexcept {
  mainFiber_ = fm_.addTaskFuture([this] { FiberBgpPeerManager::run(); });

  evb_.loopForever();
}

// The public stop interface called in Main.cpp
void SessionManager::stop() noexcept {
  if (!alreadyShutdown_) {
    shutdownWithGR(GracefulRestartFlag{false});
  }
  if (mainFiber_.valid()) {
    std::move(mainFiber_).get();
  }
  evb_.terminateLoopSoon();
}

void SessionManager::restartSession(const folly::IPAddress& peerAddr) noexcept {
  FiberBgpPeerManager::restartSession(peerAddr);
}

} // namespace facebook::bgp
