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

#include <folly/coro/BlockingWait.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/async/EventBase.h>

#include "neteng/fboss/bgp/cpp/lib/tests/FiberBgpPeerManagerTestUtils.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"

namespace facebook::nettools::bgplib {

using bgp::TBgpSessionConnectMode;
using folly::IPAddress;
using folly::SocketAddress;
using std::make_shared;

enum class ReplacementConnectionInitiator { LOCAL, REMOTE, BOTH };

class E2ESessionRouterIdReplacementTest
    : public ::testing::TestWithParam<ReplacementConnectionInitiator> {
 protected:
  E2ESessionRouterIdReplacementTest()
      : fmWrapper_(
            folly::fibers::getFiberManager(evb_, getFiberManagerOptions(256))) {
  }

  template <typename T>
  T runCoroOnEventBase(folly::coro::Task<T>&& task) {
    return folly::fibers::await([this, task = std::move(task)](
                                    folly::fibers::Promise<T> promise) mutable {
      folly::coro::co_withExecutor(&evb_, std::move(task))
          .start(
              [promise = std::move(promise)](folly::Try<T>&& result) mutable {
                promise.setValue(std::move(result));
              });
    });
  }

  folly::EventBase evb_;
  std::reference_wrapper<folly::fibers::FiberManager> fmWrapper_;
};

TEST_P(E2ESessionRouterIdReplacementTest, ReplacementUsesNewRouterId) {
  auto& fm = fmWrapper_.get();
  const auto peerAddr = kR2Lo1;
  const auto localAddr = kR1Lo1;
  const auto oldRouterId = IPAddress("163.77.193.38");
  const auto newRouterId = IPAddress("129.134.86.221");
  const BgpPeerId oldPeerId{peerAddr, oldRouterId.asV4().toLongHBO()};
  const BgpPeerId newPeerId{peerAddr, newRouterId.asV4().toLongHBO()};

  auto localConfig = makeBgpGlobalConfig(localAddr, localAddr);
  auto oldRemoteConfig = makeBgpGlobalConfig(oldRouterId, oldRouterId);
  auto newRemoteConfig = makeBgpGlobalConfig(newRouterId, newRouterId);
  auto localPeerMgr = make_shared<TestFiberBgpPeerManager>(
      localConfig,
      nullptr,
      fm,
      evb_,
      false /* enableMessagesOverNotifyQueue */,
      true /* enableCoroNotifyQueue */);
  auto oldRemotePeerMgr = make_shared<TestFiberBgpPeerManager>(
      oldRemoteConfig,
      nullptr,
      fm,
      evb_,
      false /* enableMessagesOverNotifyQueue */,
      true /* enableCoroNotifyQueue */);
  auto newRemotePeerMgr = make_shared<TestFiberBgpPeerManager>(
      newRemoteConfig,
      nullptr,
      fm,
      evb_,
      false /* enableMessagesOverNotifyQueue */,
      true /* enableCoroNotifyQueue */);
  fm.addTask([localPeerMgr] { localPeerMgr->run(); });
  fm.addTask([oldRemotePeerMgr] { oldRemotePeerMgr->run(); });
  fm.addTask([newRemotePeerMgr] { newRemotePeerMgr->run(); });

  fm.addTask([&, localPeerMgr, oldRemotePeerMgr, newRemotePeerMgr] {
    const auto localPort = localPeerMgr->getListenAddress()->getPort();
    const auto oldRemotePort = oldRemotePeerMgr->getListenAddress()->getPort();
    ASSERT_TRUE(oldRemotePeerMgr
                    ->addPeer(
                        localAddr,
                        100,
                        100,
                        SocketAddress(peerAddr, 0),
                        localPort,
                        ConnTimeParams{},
                        TBgpSessionConnectMode::PASSIVE_ONLY)
                    .hasValue());
    ASSERT_TRUE(localPeerMgr
                    ->addPeer(
                        peerAddr,
                        100,
                        100,
                        SocketAddress(localAddr, 0),
                        oldRemotePort,
                        ConnTimeParams{},
                        TBgpSessionConnectMode::ACTIVE_ONLY)
                    .hasValue());

    auto oldEstablished = runCoroOnEventBase(
        facebook::bgp::test::boundedPop(
            localPeerMgr->getNotifyCoroQueue(),
            "old router-ID session establishment"));
    ASSERT_TRUE(
        std::holds_alternative<FiberBgpPeer::ObservableStateT>(oldEstablished));
    const auto& oldEstablishedState =
        std::get<FiberBgpPeer::ObservableStateT>(oldEstablished);
    EXPECT_EQ(BgpSessionState::ESTABLISHED, oldEstablishedState.state);
    EXPECT_EQ(oldPeerId, oldEstablishedState.peerId);

    oldRemotePeerMgr->shutdownWithGR(false);
    auto oldTerminated = runCoroOnEventBase(
        facebook::bgp::test::boundedPop(
            localPeerMgr->getNotifyCoroQueue(),
            "old router-ID session teardown"));
    ASSERT_TRUE(
        std::holds_alternative<FiberBgpPeer::ObservableStateT>(oldTerminated));
    const auto& oldTerminatedState =
        std::get<FiberBgpPeer::ObservableStateT>(oldTerminated);
    EXPECT_NE(BgpSessionState::ESTABLISHED, oldTerminatedState.state);
    EXPECT_EQ(oldPeerId, oldTerminatedState.peerId);

    const auto newRemotePort = newRemotePeerMgr->getListenAddress()->getPort();
    const auto addLocalPeer = [&](const TBgpSessionConnectMode connectMode) {
      return localPeerMgr->addPeer(
          peerAddr,
          100,
          100,
          SocketAddress(localAddr, 0),
          newRemotePort,
          ConnTimeParams{},
          connectMode);
    };
    const auto addRemotePeer = [&](const TBgpSessionConnectMode connectMode) {
      return newRemotePeerMgr->addPeer(
          localAddr,
          100,
          100,
          SocketAddress(peerAddr, 0),
          localPort,
          ConnTimeParams{},
          connectMode);
    };
    if (GetParam() == ReplacementConnectionInitiator::LOCAL) {
      ASSERT_TRUE(
          addRemotePeer(TBgpSessionConnectMode::PASSIVE_ONLY).hasValue());
      ASSERT_TRUE(addLocalPeer(TBgpSessionConnectMode::ACTIVE_ONLY).hasValue());
    } else if (GetParam() == ReplacementConnectionInitiator::REMOTE) {
      ASSERT_TRUE(
          addLocalPeer(TBgpSessionConnectMode::PASSIVE_ONLY).hasValue());
      ASSERT_TRUE(
          addRemotePeer(TBgpSessionConnectMode::ACTIVE_ONLY).hasValue());
    } else {
      ASSERT_TRUE(
          addLocalPeer(TBgpSessionConnectMode::PASSIVE_ACTIVE).hasValue());
      ASSERT_TRUE(
          addRemotePeer(TBgpSessionConnectMode::PASSIVE_ACTIVE).hasValue());
    }

    auto newEstablished = runCoroOnEventBase(
        facebook::bgp::test::boundedPop(
            localPeerMgr->getNotifyCoroQueue(),
            "replacement router-ID session establishment"));
    ASSERT_TRUE(
        std::holds_alternative<FiberBgpPeer::ObservableStateT>(newEstablished));
    const auto& newEstablishedState =
        std::get<FiberBgpPeer::ObservableStateT>(newEstablished);
    EXPECT_EQ(BgpSessionState::ESTABLISHED, newEstablishedState.state);
    EXPECT_EQ(newPeerId, newEstablishedState.peerId);
    EXPECT_FALSE(localPeerMgr->isPeerUp(oldPeerId));
    EXPECT_TRUE(localPeerMgr->isPeerUp(newPeerId));
    EXPECT_TRUE(localPeerMgr->isPeerUp(peerAddr))
        << "stale state for the old router ID makes activeConnect() retry";

    auto displayInfo = localPeerMgr->getPeerDisplayInfo(peerAddr);
    ASSERT_TRUE(displayInfo.has_value());
    ASSERT_EQ(1, displayInfo->size());
    EXPECT_EQ(newPeerId.remoteBgpId, displayInfo->front().remoteBgpId);
    EXPECT_EQ(BgpSessionState::ESTABLISHED, displayInfo->front().state);

    localPeerMgr->shutdownWithGR(false);
    newRemotePeerMgr->shutdownWithGR(false);
  });

  evb_.loop();
}

INSTANTIATE_TEST_SUITE_P(
    ActiveConnectionDirection,
    E2ESessionRouterIdReplacementTest,
    ::testing::Values(
        ReplacementConnectionInitiator::LOCAL,
        ReplacementConnectionInitiator::REMOTE,
        ReplacementConnectionInitiator::BOTH),
    [](const ::testing::TestParamInfo<ReplacementConnectionInitiator>& info) {
      if (info.param == ReplacementConnectionInitiator::LOCAL) {
        return "LocalInitiates";
      }
      return info.param == ReplacementConnectionInitiator::REMOTE
          ? "RemoteInitiates"
          : "BothInitiate";
    });

} // namespace facebook::nettools::bgplib
