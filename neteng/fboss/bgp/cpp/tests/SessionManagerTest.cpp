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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

/*
 * Test RunInThread (calls run) and stop
 */
TEST(SessionManagerTest, RunInThreadTest) {
  bgp::BgpGlobalConfig globalConfig{
      kAsn1, // localAsn
      kLocalAddr1, // routerId
      kPeerAddr3, // clusterId
      kHoldTime, // holdTime
      std::nullopt, // listenAddr
      kGrRestartTime, // grRestartTime
      {}, // networksV4
      {}, // networksV6
      std::nullopt, // localConfedAsn
      facebook::bgp::ComputeUcmpFromLbwComm{false},
      0, // ucmp-width
      std::nullopt, // ucmp-quantizer
      facebook::bgp::ValidateRemoteAs{false},
      facebook::bgp::SupportStatefulGr{true},
      facebook::bgp::EnableServerSocket{false}};

  /*
   * We MUST create a shared pointer or shared_from_this would fail in
   * FiberBgpPeerManager
   */
  auto sessionMgr = std::make_shared<SessionManager>(globalConfig);
  auto thread = sessionMgr->runInThread();
  sessionMgr->stop();

  thread.join();
}

} // namespace facebook::bgp
