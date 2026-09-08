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

#include "neteng/fboss/bgp/cpp/lib/coro/CoroQueue.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberServerSocket.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberSocket.h"

#include <folly/io/coro/ServerSocket.h>
#include <folly/io/coro/Transport.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GTest.h>

#include <folly/Random.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/io/async/test/SocketPair.h>

#include <fbzmq/zmq/Socket.h>

using namespace folly;
using namespace folly::coro;
using namespace folly::fibers;
using namespace facebook::nettools::bgplib;
using namespace std::chrono_literals;

namespace {
std::string genRandomStr(const int len) {
  std::string s;
  s.resize(len);

  static const std::string alphanum =
      "0123456789"
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
      "abcdefghijklmnopqrstuvwxyz";

  for (int i = 0; i < len; ++i) {
    s[i] = alphanum.at(Random::rand32() % alphanum.size());
  }

  return s;
}
} // namespace

TEST(CoroSocket, IOAndMessagingExample) {
  EventBase evb;
  constexpr int kSize = 128;

  ServerSocket css(AsyncServerSocket::newSocket(&evb), std::nullopt, 16);
  auto serverAddr = css.getAsyncServerSocket()->getAddress();

  /*
   * client reads from server socket and uses queue to merge in additional
   * timeout event
   */
  auto client = [](EventBase& evb,
                   SocketAddress const& serverAddr) -> Task<bool> {
    XLOGF(DBG1, "Connecting to {}", serverAddr.describe());

    auto cs = co_await folly::coro::Transport::newConnectedSocket(
        &evb, serverAddr, 0ms);

    // This combines output from socket reader and timeout event generator
    CoroQueue<std::variant<bool, size_t>> queue;

    auto sockReader = [&]() -> coro::Task<void> {
      auto ex = co_await coro::co_current_executor;
      std::array<uint8_t, kSize> buf;
      co_await futures::sleep(10ms).via(ex);
      auto len =
          co_await cs.read(MutableByteRange(buf.begin(), buf.end()), 0ms);
      co_await queue.push(len);
    };

    auto timeoutWaiter = [&]() -> coro::Task<void> {
      auto ex = co_await coro::co_current_executor;
      co_await futures::sleep(5ms).via(ex);
      co_await queue.push(true);
    };
    co_withExecutor(
        &evb,

        sockReader())
        .start();
    co_withExecutor(&evb, timeoutWaiter()).start();

    // the queue should have two values once the co-routines above complete
    {
      auto val = co_await queue.wait_and_pop();
      bool item1;
      EXPECT_NO_THROW(item1 = std::get<bool>(val.value()));
      EXPECT_TRUE(item1);
    }

    size_t len{0};
    {
      auto val = co_await queue.wait_and_pop();
      EXPECT_NO_THROW(len = std::get<size_t>(val.value()));
    }

    cs.close();
    co_return len == kSize;
  };

  auto server = [](ServerSocket& css) -> Task<void> {
    auto sock = co_await css.accept();
    std::array<uint8_t, kSize> buf;
    memset(buf.data(), 'a', kSize);
    co_await sock->write(ByteRange(buf.begin(), buf.end()));
    css.close();
  };

  auto futClient = co_withExecutor(&evb, client(evb, serverAddr)).start();
  auto futServer = co_withExecutor(&evb, server(css)).start();

  evb.loop();

  EXPECT_TRUE(futClient.isReady());
  EXPECT_TRUE(futServer.isReady());

  EXPECT_TRUE(futClient.value());
}

/*
 * Fibers + coroutines in one event loop
 */

/*
 * The fixture provides fiber manager and evb for the tests
 */
class FiberSocketFixture : public ::testing::Test {
 public:
  FiberSocketFixture() = default;
  ~FiberSocketFixture() override = default;

  void SetUp() override {
    manager = std::make_unique<FiberManager>(
        std::make_unique<EventBaseLoopController>());

    static_cast<EventBaseLoopController&>(manager->loopController())
        .attachEventBase(evb);
  }

  void TearDown() override {}

  std::unique_ptr<FiberManager> manager;
  EventBase evb;
};

TEST_F(FiberSocketFixture, ReadWritePair) {
  // grab the socket pair
  SocketPair sp;
  auto fd0 = sp.extractFD0();
  auto fd1 = sp.extractFD1();
  constexpr auto kBlockSize = 1024;

  // fiber world

  auto as0 =
      to_shared_ptr(AsyncSocket::newSocket(&evb, NetworkSocket::fromFd(fd0)));
  auto fs0 = FiberSocket(as0, &evb);
  manager->addTask([fs0 = std::move(fs0), kBlockSize]() mutable {
    std::array<uint8_t, kBlockSize> data{{'a'}};
    uint32_t offset{0};
    while (offset < kBlockSize) {
      auto result = fs0.write(
          IOBuf::wrapBuffer(data.begin() + offset, data.size() - offset));
      ASSERT_TRUE(result.hasValue());
      EXPECT_EQ(kBlockSize, result.value());
      offset += *result;
    }
    fs0.close();
  });

  auto as1 =
      to_shared_ptr(AsyncSocket::newSocket(&evb, NetworkSocket::fromFd(fd1)));
  manager->addTask([as1 = std::move(as1)]() mutable {
    auto fs1 = FiberSocket(as1);
    uint64_t bytesRead = 0;
    // we are going to append returned IOBufs to this chain
    IOBuf buf;
    while (bytesRead < kBlockSize) {
      // result is folly expected
      auto result = fs1.read(kBlockSize).then([&buf, &bytesRead](auto bufRead) {
        bytesRead += bufRead->length();
        buf.appendChain(std::move(bufRead));
      });
      ASSERT_TRUE(result.hasValue());
    }
    // assemble all buffers in contiguous block
    buf.coalesce();
    std::array<uint8_t, kBlockSize> expected{{'a'}};
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), buf.data()));
    fs1.close();
  });

  // coro world

  ServerSocket css(AsyncServerSocket::newSocket(&evb), std::nullopt, 16);
  auto serverAddr = css.getAsyncServerSocket()->getAddress();

  auto client = [](EventBase& evb,
                   SocketAddress const& serverAddr) -> Task<bool> {
    auto cs = co_await folly::coro::Transport::newConnectedSocket(
        &evb, serverAddr, 0ms);
    std::array<uint8_t, kBlockSize> buf;
    auto len = co_await cs.read(MutableByteRange(buf.begin(), buf.end()), 0ms);
    cs.close();
    co_return len == buf.size();
  };

  auto server = [](ServerSocket& css) -> Task<void> {
    auto sock = co_await css.accept();
    std::array<uint8_t, kBlockSize> buf;
    memset(buf.data(), 'a', kBlockSize);
    co_await sock->write(ByteRange(buf.begin(), buf.end()));
    css.close();
  };

  auto futClient = co_withExecutor(&evb, client(evb, serverAddr)).start();
  auto futServer = co_withExecutor(&evb, server(css)).start();

  evb.loop();

  EXPECT_TRUE(futClient.isReady());
  EXPECT_TRUE(futServer.isReady());

  EXPECT_TRUE(futClient.value());
}

/*
 * TCP client connects to server; the server writes random string.
 * the string is passed from TCP client to ZMQ req/rep socket pair.
 * we read the string from rep socket and compare to original.
 */
TEST(CoroSocket, ZmqAndCoroPipeline) {
  EventBase evb;
  fbzmq::Context ctx;
  fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT> req(
      ctx, none, none, fbzmq::NonblockingFlag{true}, &evb);
  fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER> rep(
      ctx, none, none, fbzmq::NonblockingFlag{true}, &evb);
  constexpr int kBlockSize = 1024;

  // connect ZMQ socket pair
  rep.bind(fbzmq::SocketUrl{"inproc://test"}).value();

  // server socket, bind before anything
  ServerSocket css(AsyncServerSocket::newSocket(&evb), std::nullopt, 16);
  // so we can connect to this server
  auto serverAddr = css.getAsyncServerSocket()->getAddress();

  // string we're passing throug the pipeline of socket
  const auto str = genRandomStr(kBlockSize);

  // connect to TCP server, read block of data, write it to ZMQ
  auto tcpZmqClient =
      [](EventBase& evb,
         SocketAddress const& serverAddr,
         fbzmq::Socket<ZMQ_REQ, fbzmq::ZMQ_CLIENT>& req) -> Task<bool> {
    auto cs = co_await folly::coro::Transport::newConnectedSocket(
        &evb, serverAddr, 0ms);
    std::array<uint8_t, kBlockSize> buf;
    memset(buf.data(), 0, kBlockSize);
    auto len = co_await cs.read(MutableByteRange(buf.begin(), buf.end()), 0ms);
    cs.close();
    if (len != kBlockSize) {
      co_return false;
    }
    // now pass-on to ZMQ side
    req.connect(fbzmq::SocketUrl{"inproc://test"}).value();
    auto msg = fbzmq::Message::from(std::string(std::begin(buf), std::end(buf)))
                   .value();
    co_await req.sendOneCoro(msg);
    co_return true;
  };

  // server simply writes random string to connected client
  auto tcpServer = [](ServerSocket& css, const std::string& str) -> Task<void> {
    auto sock = co_await css.accept();
    co_await sock->write(folly::ByteRange(folly::StringPiece(str)));
    css.close();
  };

  // zmq receives string and compares to expected
  auto zmqServer = [](EventBase& evb,
                      fbzmq::Socket<ZMQ_REP, fbzmq::ZMQ_SERVER>& rep,
                      const std::string& str) -> Task<bool> {
    auto rcvd = co_await rep.recvOneCoro();
    co_return str == rcvd.value().read<std::string>().value();
  };

  // start all three tasks

  auto futTcpZmqClient =
      co_withExecutor(&evb, tcpZmqClient(evb, serverAddr, req)).start();
  auto futTcpServer = co_withExecutor(&evb, tcpServer(css, str)).start();
  auto futZmqServer = co_withExecutor(&evb, zmqServer(evb, rep, str)).start();

  evb.loop();

  EXPECT_TRUE(futTcpZmqClient.isReady());
  EXPECT_TRUE(futTcpServer.isReady());
  EXPECT_TRUE(futZmqServer.isReady());

  EXPECT_TRUE(futTcpZmqClient.value());
  EXPECT_TRUE(futZmqServer.value());
}
