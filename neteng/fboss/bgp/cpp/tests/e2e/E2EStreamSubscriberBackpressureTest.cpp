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
 * E2E coverage for MP-BGP monitor (thrift stream subscriber) egress
 * backpressure.
 *
 * These tests use a real in-process thrift server
 * (apache::thrift::ScopedServerInterfaceThread over BgpServiceStream) and a
 * real client (apache::thrift::Client<TBgpServiceStream>). The test sets the
 * rate at which the client reads. Therefore real thrift stream credit causes
 * the block transition and the unblock transition.
 *
 * The production incident was an unbounded queue that grew behind a slow
 * monitor. Therefore each test asserts a numeric ceiling on the number of
 * queued messages.
 */

#include <algorithm>
#include <atomic>

#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/CancellationToken.h>
#include <folly/Synchronized.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/fibers/Semaphore.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/util/ScopedServerInterfaceThread.h>

#include "neteng/fboss/bgp/cpp/BgpServiceStream.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"
#include "neteng/fboss/bgp/if/gen-cpp2/TBgpServiceStream.h"

using facebook::neteng::fboss::bgp::thrift::TBgpRouteDelta;
using facebook::neteng::fboss::bgp::thrift::TBgpServiceStream;

namespace facebook::bgp {

namespace {

/*
 * A stream subscriber uses the same egress queue sizes as a peer. There is no
 * separate knob for a subscriber. Each test that waits for a block first
 * injects more routes than the high watermark of the queue.
 */
constexpr size_t kSubscriberQueueCapacity =
    nettools::bgplib::kMaxEgressQueueSize;

/*
 * The client buffers one chunk. With a larger buffer the client would accept
 * a prefetch window of messages before it stops to give stream credit. With
 * one chunk a monitor that stops to read stops the credit immediately.
 */
constexpr int32_t kClientChunkBufferSize = 1;

constexpr auto kMonitorName = "test-mp-bgp-monitor";
constexpr auto kSecondMonitorName = "test-mp-bgp-monitor-2";

folly::CIDRNetwork toCidr(const std::string& prefix) {
  return folly::IPAddress::createNetwork(prefix);
}

// "<asn>:<value>" for every community on an UPDATE, sorted.
std::vector<std::string> communitiesOf(
    const nettools::bgplib::BgpUpdate2& update) {
  std::vector<std::string> communities;
  for (const auto& comm : update.attrs()->communities().value()) {
    communities.push_back(
        fmt::format("{}:{}", comm.asn().value(), comm.value().value()));
  }
  std::sort(communities.begin(), communities.end());
  return communities;
}

/*
 * A "pretend MP-BGP monitor": a real thrift stream client whose consumption
 * rate the test drives explicitly.
 *
 * A credit controls each read. The consumer coroutine waits for a credit
 * before it reads an item. Thus a monitor with no credit is a receiver that
 * stopped to read, grant(n) is a receiver that reads n more messages, and
 * resume() is a receiver that keeps up. This class uses no sleep and makes no
 * assumption about the wall clock.
 */
class PretendMonitor {
 public:
  struct ReceivedMessage {
    bool isEoR{false};
    std::optional<nettools::bgplib::BgpUpdate2> update;
  };

  PretendMonitor(
      apache::thrift::ScopedServerInterfaceThread& server,
      std::string name)
      : name_(std::move(name)),
        client_(server.newClient<apache::thrift::Client<TBgpServiceStream>>()) {
  }

  ~PretendMonitor() {
    stop();
  }

  /* Subscribe and start the consumer thread. Starts with zero credits. */
  void start() {
    apache::thrift::RpcOptions options;
    options.setChunkBufferSize(kClientChunkBufferSize);
    auto stream = client_->semifuture_subscribe(options, name_).get();
    thread_ = std::thread([this, s = std::move(stream)]() mutable {
      folly::coro::blockingWait(
          folly::coro::co_withCancellation(
              cancelSource_.getToken(), consumeLoop(std::move(s))));
    });
  }

  /*
   * Let the monitor keep up with the server. This function sets a flag. It
   * does not give a large number of credits. With a large number of credits,
   * stall() must take back each semaphore token in a separate atomic
   * operation. That loop is slow, and it makes the test time out under TSAN
   * on a busy host.
   */
  void resume() {
    ungated_.store(true);
    /* Wake a consumer that already waits for a credit. */
    credits_.signal();
  }

  /*
   * Take back each unused credit. Then the monitor stops at the next message.
   * Without this call a monitor that ran during the setup keeps reading, and
   * the queue never reaches its high watermark.
   */
  void stall() {
    ungated_.store(false);
    while (credits_.try_wait()) {
    }
  }

  void stop() {
    if (stopped_.exchange(true)) {
      return;
    }
    stopping_.store(true);
    /* Wake the consumer. It can wait for a credit or wait in gen.next(). */
    credits_.signal();
    cancelSource_.requestCancellation();
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  size_t eorCount() const {
    return messages_.withRLock([](const auto& msgs) {
      return static_cast<size_t>(std::count_if(
          msgs.begin(), msgs.end(), [](const auto& m) { return m.isEoR; }));
    });
  }

  /*
   * Returns each announced prefix in the order of arrival. It keeps a repeat
   * of a prefix. A test uses this list to prove that a block and an unblock
   * lose no route and repeat no route.
   */
  std::vector<folly::CIDRNetwork> announcedPrefixesInOrder() const {
    std::vector<folly::CIDRNetwork> prefixes;
    messages_.withRLock([&](const auto& msgs) {
      for (const auto& msg : msgs) {
        if (msg.isEoR || !msg.update.has_value()) {
          continue;
        }
        for (const auto& prefix : getAnnouncedPrefixes(*msg.update)) {
          prefixes.push_back(prefix);
        }
      }
    });
    return prefixes;
  }

  std::set<folly::CIDRNetwork> announcedPrefixes() const {
    auto ordered = announcedPrefixesInOrder();
    return std::set<folly::CIDRNetwork>(ordered.begin(), ordered.end());
  }

  /* The last UPDATE that announced `prefix`, or nullopt if never seen. */
  std::optional<nettools::bgplib::BgpUpdate2> updateForPrefix(
      const folly::CIDRNetwork& prefix) const {
    std::optional<nettools::bgplib::BgpUpdate2> found;
    messages_.withRLock([&](const auto& msgs) {
      for (const auto& msg : msgs) {
        if (msg.isEoR || !msg.update.has_value()) {
          continue;
        }
        auto announced = getAnnouncedPrefixes(*msg.update);
        if (std::find(announced.begin(), announced.end(), prefix) !=
            announced.end()) {
          found = msg.update;
        }
      }
    });
    return found;
  }

  bool waitForEoR(int maxRetries = 100) {
    bool got = false;
    WITH_RETRIES_N(maxRetries, {
      got = eorCount() > 0;
      EXPECT_EVENTUALLY_TRUE(got);
    });
    return got;
  }

  bool waitForAnnouncedPrefixes(
      const std::set<folly::CIDRNetwork>& expected,
      int maxRetries = 200) {
    bool got = false;
    WITH_RETRIES_N(maxRetries, {
      got = announcedPrefixes() == expected;
      EXPECT_EVENTUALLY_TRUE(got);
    });
    return got;
  }

 private:
  folly::coro::Task<void> consumeLoop(
      apache::thrift::ClientBufferedStream<TBgpRouteDelta> stream) {
    auto gen = std::move(stream).toAsyncGenerator();
    while (true) {
      co_await folly::coro::co_safe_point;
      /*
       * Gate BEFORE pulling: an ungranted monitor never calls next(), so the
       * client buffer stays full, no RequestN goes out, and the server-side
       * generator stays suspended. That is the condition under test.
       */
      if (!ungated_.load()) {
        auto credit = co_await folly::coro::co_awaitTry(credits_.co_wait());
        if (credit.hasException()) {
          co_return;
        }
      }
      if (stopping_.load()) {
        co_return;
      }
      auto item = co_await folly::coro::co_awaitTry(gen.next());
      if (item.hasException() || !item->has_value()) {
        co_return;
      }
      record(**item);
    }
  }

  void record(const TBgpRouteDelta& delta) {
    ReceivedMessage msg;
    const auto& wrapper = delta.update2OrEor().value();
    if (wrapper.update2().has_value()) {
      msg.update = wrapper.update2().value();
    } else if (wrapper.eor().has_value()) {
      msg.isEoR = true;
    }
    messages_.wlock()->push_back(std::move(msg));
  }

  const std::string name_;
  std::unique_ptr<apache::thrift::Client<TBgpServiceStream>> client_;
  std::thread thread_;
  folly::fibers::Semaphore credits_{0};
  folly::CancellationSource cancelSource_;
  std::atomic<bool> ungated_{false};
  std::atomic<bool> stopping_{false};
  std::atomic<bool> stopped_{false};
  folly::Synchronized<std::vector<ReceivedMessage>> messages_;
};

} // namespace

class StreamSubscriberBackpressureTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    /*
     * FLAGS_enable_stream_subscriber_backpressure is a process-global gflag.
     * The saver restores it on destruction, so a suite that changes the gflag
     * cannot change the behavior of another fixture in this binary.
     */
    flagSaver_ = std::make_unique<gflags::FlagSaver>();
    FLAGS_enable_stream_subscriber_backpressure = gflagBackpressure();
    setEnableStreamSubscriberBackpressure(configBackpressure());
    addPeer(kDefaultPeerSpec3);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/updateGroupEnabled(),
        /*enableEgressBackpressure=*/true);
    bringUpPeer(kPeerAddr3);
    peerId3_ = BgpPeerId{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    sendEoRToPeer(peerId3_);
    ASSERT_TRUE(waitForEoR(peerId3_));

    auto handler = std::make_shared<BgpServiceStream>(peerManager_.get());
    server_ =
        std::make_unique<apache::thrift::ScopedServerInterfaceThread>(handler);
  }

  void TearDown() override {
    /*
     * Monitors first: each cancels its stream, which tears the subscriber
     * down on the peer-manager event base. Then the thrift server, then the
     * fixture's own components.
     */
    monitors_.clear();
    server_.reset();
    E2ETestFixture::TearDown();
    flagSaver_.reset();
  }

  /*
   * The value of the BgpConfig field. std::nullopt leaves the field out of
   * the config. The base suite runs the production default: the config says
   * nothing and the gflag decides.
   */
  virtual std::optional<bool> configBackpressure() const {
    return std::nullopt;
  }

  /* The value of the gflag. It is true in production. */
  virtual bool gflagBackpressure() const {
    return true;
  }

  /* Overridden by the update-groups-on suite. */
  virtual bool updateGroupEnabled() const {
    return false;
  }

  PretendMonitor& makeMonitor(const std::string& name) {
    monitors_.push_back(std::make_unique<PretendMonitor>(*server_, name));
    return *monitors_.back();
  }

  /*
   * Subscribe `monitor` and let it read the initial dump and the EoR of the
   * session. Then take back each unused credit. The test starts from a known
   * state in which the monitor reads nothing more.
   */
  void startAndQuiesce(PretendMonitor& monitor, const std::string& name) {
    monitor.start();
    ASSERT_TRUE(waitForSubscriberEstablished(name, /*established=*/true));
    monitor.resume();
    ASSERT_TRUE(monitor.waitForEoR());
    ASSERT_TRUE(waitForSubscriberQueueSizeAtMost(name, 0));
    monitor.stall();
  }

  /*
   * Inject `count` routes, each with its own community so each becomes its
   * own UPDATE and therefore its own egress queue slot.
   */
  std::set<folly::CIDRNetwork> injectDistinctRoutes(
      size_t count,
      int firstOctet) {
    std::set<folly::CIDRNetwork> prefixes;
    for (size_t i = 0; i < count; ++i) {
      auto prefix = fmt::format("{}.{}.0.0/16", firstOctet, i);
      injectLocalRoutesAtRuntime({prefix}, {fmt::format("64512:{}", i)});
      prefixes.insert(toCidr(prefix));
    }
    return prefixes;
  }

  /*
   * Collect the prefixes that bgpd sent to peer3 up to now. This function
   * empties the egress queue of peer3. Call it more than one time with the
   * same accumulator.
   */
  void accumulatePeerAnnouncedPrefixes(
      std::set<folly::CIDRNetwork>& accumulator) {
    auto messages = drainAllOutboundMessagesToOrderedVec(
        peerId3_,
        /*idleRetries=*/5,
        /*maxMessages=*/0,
        /*sleepMsBetweenRetries=*/0);
    for (const auto& msg : messages) {
      if (msg.isEoR || !msg.update) {
        continue;
      }
      for (const auto& prefix : getAnnouncedPrefixes(*msg.update)) {
        accumulator.insert(prefix);
      }
    }
  }

  BgpPeerId peerId3_;
  std::unique_ptr<gflags::FlagSaver> flagSaver_;
  std::unique_ptr<apache::thrift::ScopedServerInterfaceThread> server_;
  std::vector<std::unique_ptr<PretendMonitor>> monitors_;
};

/*
 * The gflag is false and the config says nothing. The subscriber must use the
 * unbounded egress path. Its queue must never block, and the subscriber must
 * still receive every route.
 */
class StreamSubscriberNoBackpressureTest
    : public StreamSubscriberBackpressureTest {
 protected:
  bool gflagBackpressure() const override {
    return false;
  }
};

/*
 * This is the main test. A receiver that stops to read must keep the queue
 * at its bound and must stop the change-list consumer. After the receiver
 * reads again, it must get every route.
 */
TEST_F(StreamSubscriberBackpressureTest, BlockAndUnblockOnSlowConsumer) {
  auto& monitor = makeMonitor(kMonitorName);
  ASSERT_NO_FATAL_FAILURE(startAndQuiesce(monitor, kMonitorName));

  /* Must exceed kSubscriberQueueCapacity, or the queue never blocks. */
  constexpr size_t kNumRoutes = 20;
  const auto expected = injectDistinctRoutes(kNumRoutes, 11);

  EXPECT_TRUE(waitForSubscriberQueueBlocked(kMonitorName));

  /* The number of queued messages must stay at or below the capacity. */
  EXPECT_THAT(
      getSubscriberQueueSize(kMonitorName),
      ::testing::Optional(::testing::Le(kSubscriberQueueCapacity)));

  /*
   * The backpressure must reach the producer. The AdjRib must cancel its
   * change-list consume timer. Then it does not pack an update that it cannot
   * put in the queue.
   */
  EXPECT_TRUE(
      waitForSubscriberChangeListTimer(kMonitorName, /*scheduled=*/false));

  /* Recovery must be automatic: resuming the reader is the only input. */
  monitor.resume();

  EXPECT_TRUE(waitForSubscriberQueueUnblocked(kMonitorName));
  EXPECT_TRUE(
      waitForSubscriberChangeListTimer(kMonitorName, /*scheduled=*/true));
  EXPECT_TRUE(waitForSubscriberQueueSizeAtMost(kMonitorName, 0));

  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(expected));

  const auto ordered = monitor.announcedPrefixesInOrder();
  EXPECT_EQ(expected.size(), ordered.size())
      << "a block/unblock cycle must not duplicate or drop announcements";

  /* Attributes must survive the cycle intact, per prefix. */
  for (size_t i = 0; i < kNumRoutes; ++i) {
    const auto prefix = toCidr(fmt::format("11.{}.0.0/16", i));
    const auto update = monitor.updateForPrefix(prefix);
    ASSERT_TRUE(update.has_value())
        << "missing announcement for "
        << folly::IPAddress::networkToString(prefix);
    const std::vector<std::string> expectedCommunities{
        fmt::format("64512:{}", i)};
    EXPECT_EQ(expectedCommunities, communitiesOf(*update));
  }
}

/*
 * This test guards against the production incident. While the receiver does
 * not read, the queue depth must stay at its bound. The number of routes that
 * arrive must not change that.
 */
TEST_F(StreamSubscriberBackpressureTest, QueueDepthBoundedUnderSustainedStall) {
  auto& monitor = makeMonitor(kMonitorName);
  ASSERT_NO_FATAL_FAILURE(startAndQuiesce(monitor, kMonitorName));

  const auto expected = injectDistinctRoutes(60, 12);
  EXPECT_TRUE(waitForSubscriberQueueBlocked(kMonitorName));

  /*
   * Sample repeatedly while the RIB keeps churning. Under the old unbounded
   * path this count climbed with every injected route.
   */
  for (int round = 0; round < 20; ++round) {
    EXPECT_THAT(
        getSubscriberQueueSize(kMonitorName),
        ::testing::Optional(::testing::Le(kSubscriberQueueCapacity)))
        << "subscriber egress queue exceeded its bound at round " << round;
  }

  monitor.resume();
  EXPECT_TRUE(waitForSubscriberQueueUnblocked(kMonitorName));
  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(expected));
}

/* A monitor that stops to read must not stop the BGP peers of the same RIB. */
TEST_F(StreamSubscriberBackpressureTest, SlowSubscriberDoesNotStallBgpPeer) {
  auto& monitor = makeMonitor(kMonitorName);
  ASSERT_NO_FATAL_FAILURE(startAndQuiesce(monitor, kMonitorName));

  /* Discard whatever peer3 was sent during setup. */
  std::set<folly::CIDRNetwork> peerPrefixes;
  accumulatePeerAnnouncedPrefixes(peerPrefixes);
  peerPrefixes.clear();

  const auto expected = injectDistinctRoutes(20, 13);
  EXPECT_TRUE(waitForSubscriberQueueBlocked(kMonitorName));

  /* With the monitor blocked, the real peer must still receive every route. */
  bool peerConverged = false;
  WITH_RETRIES_N(200, {
    accumulatePeerAnnouncedPrefixes(peerPrefixes);
    peerConverged = peerPrefixes == expected;
    EXPECT_EVENTUALLY_TRUE(peerConverged);
  });
  EXPECT_TRUE(peerConverged);

  /* And the monitor is still blocked -- the peer drained on its own path. */
  EXPECT_THAT(
      isSubscriberQueueBlocked(kMonitorName), ::testing::Optional(true));

  monitor.resume();
  EXPECT_TRUE(waitForSubscriberQueueUnblocked(kMonitorName));
  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(expected));
}

/* A monitor that stops to read must not stop a second monitor. */
TEST_F(StreamSubscriberBackpressureTest, SlowSubscriberDoesNotStallFastOne) {
  auto& slow = makeMonitor(kMonitorName);
  auto& fast = makeMonitor(kSecondMonitorName);
  ASSERT_NO_FATAL_FAILURE(startAndQuiesce(slow, kMonitorName));
  ASSERT_NO_FATAL_FAILURE(startAndQuiesce(fast, kSecondMonitorName));

  /* Only the fast monitor keeps consuming. */
  fast.resume();

  const auto expected = injectDistinctRoutes(20, 14);

  EXPECT_TRUE(waitForSubscriberQueueBlocked(kMonitorName));
  EXPECT_TRUE(fast.waitForAnnouncedPrefixes(expected));
  EXPECT_TRUE(waitForSubscriberQueueSizeAtMost(kSecondMonitorName, 0));

  /* The slow one is still blocked and still bounded. */
  EXPECT_THAT(
      isSubscriberQueueBlocked(kMonitorName), ::testing::Optional(true));
  EXPECT_THAT(
      getSubscriberQueueSize(kMonitorName),
      ::testing::Optional(::testing::Le(kSubscriberQueueCapacity)));

  slow.resume();
  EXPECT_TRUE(waitForSubscriberQueueUnblocked(kMonitorName));
  EXPECT_TRUE(slow.waitForAnnouncedPrefixes(expected));
}

/*
 * Disconnecting while blocked must tear the subscriber down cleanly -- the
 * stream generator is parked on a full queue at that moment -- and a
 * re-subscribe must come back with fresh queues, since the previous session
 * close()d them.
 */
TEST_F(
    StreamSubscriberBackpressureTest,
    DisconnectWhileBlockedThenResubscribe) {
  auto& monitor = makeMonitor(kMonitorName);
  ASSERT_NO_FATAL_FAILURE(startAndQuiesce(monitor, kMonitorName));

  const auto firstBatch = injectDistinctRoutes(20, 15);
  EXPECT_TRUE(waitForSubscriberQueueBlocked(kMonitorName));

  /* Drop the client while the server side is backpressured. */
  monitor.stop();
  EXPECT_TRUE(
      waitForSubscriberEstablished(kMonitorName, /*established=*/false));

  /*
   * Subscribe again with the same name. The new session must get new queues.
   * A push into the previous queue returns false, because that queue is
   * closed. Then this monitor would never get the routes.
   */
  auto& monitor2 = makeMonitor(kMonitorName);
  monitor2.start();
  ASSERT_TRUE(waitForSubscriberEstablished(kMonitorName, /*established=*/true));
  monitor2.resume();

  ASSERT_TRUE(monitor2.waitForEoR());
  EXPECT_TRUE(monitor2.waitForAnnouncedPrefixes(firstBatch));

  /* The fresh session must still deliver subsequent updates. */
  const auto secondBatch = injectDistinctRoutes(10, 16);
  auto combined = firstBatch;
  combined.insert(secondBatch.begin(), secondBatch.end());
  EXPECT_TRUE(monitor2.waitForAnnouncedPrefixes(combined));
  EXPECT_TRUE(waitForSubscriberQueueSizeAtMost(kMonitorName, 0));
}

/*
 * A monitor that stalls part-way through its initial RIB dump must block
 * there, and converge on the full RIB once it resumes.
 */
TEST_F(StreamSubscriberBackpressureTest, BlockDuringInitialRibDump) {
  /*
   * Populate the RIB before anyone subscribes, so subscribe() triggers a dump
   * large enough to overrun the subscriber's queue.
   */
  const auto expected = injectDistinctRoutes(30, 17);
  ASSERT_TRUE(waitForRouteInShadowRib(toCidr("17.29.0.0/16")));

  auto& monitor = makeMonitor(kMonitorName);
  monitor.start();
  ASSERT_TRUE(waitForSubscriberEstablished(kMonitorName, /*established=*/true));

  /* Consume nothing: the dump has to stall against the bounded queue. */
  EXPECT_TRUE(waitForSubscriberQueueBlocked(kMonitorName));
  EXPECT_THAT(
      getSubscriberQueueSize(kMonitorName),
      ::testing::Optional(::testing::Le(kSubscriberQueueCapacity)));
  EXPECT_TRUE(
      waitForSubscriberChangeListTimer(kMonitorName, /*scheduled=*/false));

  monitor.resume();

  EXPECT_TRUE(waitForSubscriberQueueUnblocked(kMonitorName));
  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(expected));
  EXPECT_TRUE(monitor.waitForEoR());
}

/*
 * This test covers the ordinary disconnect. A monitor that has read all the
 * messages drops its stream. At that moment the generator on the server waits
 * in pop() on an empty queue and the client still has credit.
 *
 * This case fails if the generator passes only the token of the server to
 * co_withCancellation. That call keeps the first token and ignores the token
 * of thrift. Then the semaphore wait inside pop() never sees the disconnect
 * of the client, the generator never ends, and the subscriber stays
 * ESTABLISHED. Such subscribers use up streamSubscriberLimit.
 *
 * DisconnectWhileBlockedThenResubscribe cannot find this defect. A blocked
 * generator waits at co_yield, and thrift ends a generator at that point in
 * both designs.
 */
TEST_F(
    StreamSubscriberBackpressureTest,
    DisconnectWhileIdleTearsDownSubscriber) {
  auto& monitor = makeMonitor(kMonitorName);
  ASSERT_NO_FATAL_FAILURE(startAndQuiesce(monitor, kMonitorName));

  /* Read every message. Then the generator waits on an empty queue. */
  monitor.resume();
  const auto expected = injectDistinctRoutes(5, 19);
  ASSERT_TRUE(monitor.waitForAnnouncedPrefixes(expected));
  ASSERT_TRUE(waitForSubscriberQueueSizeAtMost(kMonitorName, 0));
  ASSERT_THAT(
      isSubscriberQueueBlocked(kMonitorName), ::testing::Optional(false));

  monitor.stop();
  EXPECT_TRUE(
      waitForSubscriberEstablished(kMonitorName, /*established=*/false));

  /* A new subscription must succeed. That proves bgpd freed the slot. */
  auto& monitor2 = makeMonitor(kMonitorName);
  monitor2.start();
  ASSERT_TRUE(waitForSubscriberEstablished(kMonitorName, /*established=*/true));
  monitor2.resume();
  EXPECT_TRUE(monitor2.waitForEoR());
  EXPECT_TRUE(monitor2.waitForAnnouncedPrefixes(expected));
}

/*
 * This suite runs with the update groups on and with the backpressure on. A
 * stream subscriber is never registered with UpdateGroupManager, so it uses
 * its own change-list consume timer. With the backpressure on, that timer is
 * the only code that moves a packed update into the egress queue. If
 * processRibDumpReq() does not call activateChangeListConsumer() for the
 * subscriber, the monitor keeps an open stream and gets no route and no
 * EoR.
 */
class StreamSubscriberBackpressureUpdateGroupTest
    : public StreamSubscriberBackpressureTest {
 protected:
  bool updateGroupEnabled() const override {
    return true;
  }
};

TEST_F(
    StreamSubscriberBackpressureUpdateGroupTest,
    ConvergesWithUpdateGroupsEnabled) {
  auto& monitor = makeMonitor(kMonitorName);
  monitor.start();
  ASSERT_TRUE(waitForSubscriberEstablished(kMonitorName, /*established=*/true));
  monitor.resume();

  /* Initial dump must complete, EoR included. */
  ASSERT_TRUE(monitor.waitForEoR());

  const auto expected = injectDistinctRoutes(20, 20);
  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(expected));
  EXPECT_TRUE(waitForSubscriberQueueSizeAtMost(kMonitorName, 0));

  /* Backpressure must still engage and release under update groups. */
  monitor.stall();
  const auto more = injectDistinctRoutes(20, 21);
  EXPECT_TRUE(waitForSubscriberQueueBlocked(kMonitorName));

  monitor.resume();
  EXPECT_TRUE(waitForSubscriberQueueUnblocked(kMonitorName));

  auto combined = expected;
  combined.insert(more.begin(), more.end());
  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(combined));
}

/*
 * The config disables the feature while the gflag enables it. This is the way
 * to disable the bounded path on a device with a config push. The config must
 * win, and the subscriber must use the unbounded path.
 */
class StreamSubscriberConfigDisableTest
    : public StreamSubscriberBackpressureTest {
 protected:
  std::optional<bool> configBackpressure() const override {
    return false;
  }
  bool gflagBackpressure() const override {
    return true;
  }
};

TEST_F(StreamSubscriberConfigDisableTest, ConfigFalseOverridesGflagTrue) {
  auto& monitor = makeMonitor(kMonitorName);
  monitor.start();
  ASSERT_TRUE(waitForSubscriberEstablished(kMonitorName, /*established=*/true));

  peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
    auto* subscriber = peerManager_->getStreamSubscriber(kMonitorName);
    ASSERT_NE(subscriber, nullptr);
    EXPECT_FALSE(subscriber->boundedEgress);
    /* The unbounded path serves the stream through a publisher. */
    EXPECT_NE(subscriber->publisher, nullptr);
  });

  const auto expected = injectDistinctRoutes(20, 22);

  /* The bounded queue stays unused, so it cannot block. */
  EXPECT_THAT(
      isSubscriberQueueBlocked(kMonitorName), ::testing::Optional(false));

  monitor.resume();
  EXPECT_TRUE(monitor.waitForEoR());
  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(expected));
}

/*
 * The config enables the feature while the gflag disables it. The config must
 * win, and the subscriber must use the bounded path.
 */
class StreamSubscriberConfigEnableTest
    : public StreamSubscriberBackpressureTest {
 protected:
  std::optional<bool> configBackpressure() const override {
    return true;
  }
  bool gflagBackpressure() const override {
    return false;
  }
};

TEST_F(StreamSubscriberConfigEnableTest, ConfigTrueOverridesGflagFalse) {
  auto& monitor = makeMonitor(kMonitorName);
  ASSERT_NO_FATAL_FAILURE(startAndQuiesce(monitor, kMonitorName));

  peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
    auto* subscriber = peerManager_->getStreamSubscriber(kMonitorName);
    ASSERT_NE(subscriber, nullptr);
    EXPECT_TRUE(subscriber->boundedEgress);
    /* The bounded path serves the stream from a generator. */
    EXPECT_EQ(subscriber->publisher, nullptr);
  });

  const auto expected = injectDistinctRoutes(20, 23);
  EXPECT_TRUE(waitForSubscriberQueueBlocked(kMonitorName));
  EXPECT_THAT(
      getSubscriberQueueSize(kMonitorName),
      ::testing::Optional(::testing::Le(kSubscriberQueueCapacity)));

  monitor.resume();
  EXPECT_TRUE(waitForSubscriberQueueUnblocked(kMonitorName));
  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(expected));
}

/*
 * With the feature disabled the unbounded path must not change. bgpd must not
 * mark the subscriber as bounded, the bounded queue must stay unused and
 * unblocked, and every route must reach a monitor that gives no stream credit
 * until the end of the test.
 */
TEST_F(StreamSubscriberNoBackpressureTest, LegacyUnboundedPathUnchanged) {
  auto& monitor = makeMonitor(kMonitorName);
  monitor.start();
  ASSERT_TRUE(waitForSubscriberEstablished(kMonitorName, /*established=*/true));

  peerManager_->getEventBase().runInEventBaseThreadAndWait([&]() {
    auto* subscriber = peerManager_->getStreamSubscriber(kMonitorName);
    ASSERT_NE(subscriber, nullptr);
    EXPECT_FALSE(subscriber->boundedEgress);
    /* The unbounded path serves the stream through a publisher. */
    EXPECT_NE(subscriber->publisher, nullptr);
  });

  const auto expected = injectDistinctRoutes(20, 18);

  /*
   * The unbounded path does not use the bounded queue. Therefore that queue
   * cannot block.
   */
  EXPECT_THAT(
      isSubscriberQueueBlocked(kMonitorName), ::testing::Optional(false));
  EXPECT_THAT(getSubscriberQueueSize(kMonitorName), ::testing::Optional(0u));

  monitor.resume();
  EXPECT_TRUE(monitor.waitForEoR());
  EXPECT_TRUE(monitor.waitForAnnouncedPrefixes(expected));
}

} // namespace facebook::bgp
