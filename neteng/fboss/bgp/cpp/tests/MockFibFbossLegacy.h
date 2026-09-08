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

/*
 * TODO: This entire file should be deleted, since it's based on mocking unused
 * future_* style apis. All tests shoudl be migrated to use the new mock infra
 * for BGP and agent interaction in FibFbossMock.h Please note this migration
 * work is a pending followup from coro migration done in D52582069
 */

#pragma once

#include <folly/io/async/AsyncSocket.h>
#include <gmock/gmock.h>
#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include "fboss/agent/if/gen-cpp2/FbossCtrl.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/rib/FibFboss.h"

namespace facebook::bgp {
class RibBase;
} // namespace facebook::bgp

class MockFbossCtrlAsyncClient
    : public apache::thrift::Client<facebook::fboss::FbossCtrl> {
 public:
  using apache::thrift::Client<facebook::fboss::FbossCtrl>::Client;

  MOCK_METHOD(
      folly::Future<folly::Unit>,
      future_syncFib,
      (apache::thrift::RpcOptions&,
       int16_t,
       const std::vector<facebook::fboss::UnicastRoute>&));

  MOCK_METHOD(
      folly::Future<folly::Unit>,
      future_addUnicastRoutes,
      (apache::thrift::RpcOptions&,
       int16_t,
       const std::vector<facebook::fboss::UnicastRoute>&));

  MOCK_METHOD(
      folly::Future<folly::Unit>,
      future_deleteUnicastRoutes,
      (apache::thrift::RpcOptions&,
       int16_t,
       const std::vector<facebook::fboss::IpPrefix>&));

  MOCK_METHOD(folly::Future<int64_t>, future_aliveSince, ());

  MOCK_METHOD(
      folly::Future<facebook::fb303::cpp2::fb_status>,
      future_getStatus,
      ());
};

class MockFibFboss : public facebook::bgp::FibFboss {
 public:
  static std::unique_ptr<MockFibFboss> createMockFibFboss(
      folly::EventBase* evb,
      folly::coro::CancellableAsyncScope& asyncScope,
      Fib::FibMessageQueue& toRibQ) {
    auto fib = std::unique_ptr<MockFibFboss>(
        new MockFibFboss(evb, asyncScope, toRibQ));
    fib->connectAgent();
    return fib;
  }

  void connectAgent() override {
    // create a mocked version of FbossCtrlAsyncClient
    client_ = createFibFbossClientHelper();

    // create a batch
    batch_ = std::make_unique<Batch>();
  }

  std::unique_ptr<MockFbossCtrlAsyncClient> createFibFbossClientHelper() {
    auto addr = folly::SocketAddress(facebook::bgp::kLoopBackAddressV6, 5909);
    auto sock = folly::AsyncSocket::newSocket(evb_, addr, 20);
    sock->setSendTimeout(20);
    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(sock));
    auto client =
        std::make_unique<MockFbossCtrlAsyncClient>(std::move(channel));

    // ATTN: this is important to set mockClient_ for MOCK method validation
    mockClient_ = client.get();

    return client;
  }

  // Mocked version of FbossCtrlAsyncClient for MOCK_METHOD validation usage
  MockFbossCtrlAsyncClient* mockClient_;

 protected:
  // ATTN: reuse FibFboss's constructor
  using facebook::bgp::FibFboss::FibFboss;

/*
 * per class placeholder for test code injection
 * only need to be setup once here
 */
#ifdef MockFibFboss_TEST_FRIENDS
  MockFibFboss_TEST_FRIENDS
#endif
};
