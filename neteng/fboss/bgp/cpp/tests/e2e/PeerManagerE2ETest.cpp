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
 * E2E test for PeerManagerBase: Tests PeerManagerBase BGP flow end-to-end
 */

#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Timeout.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <thread>

#include "neteng/fboss/bgp/cpp/tests/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

class PeerManagerE2ETest : public E2ETestFixture {
 protected:
  void setupComponents(
      bool enableUpdateGroup = true,
      bool enableEgressBackpressure = true) {
    /* Add default peers to the configuration */
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);

    createPeerManager(enableUpdateGroup, enableEgressBackpressure);
  }
};

/*
 * Test flapping of a peer under ingress back pressure
 *
 * This test verifies that peers can flap correctly when the ingress queue
 * (ribInQ_) is under back pressure due to a shallow queue capacity (size 1).
 *
 * Test setup:
 * - Creates a shallow ribInQ_ with capacity 1 to create back pressure
 * - Runs two coroutines on separate threads:
 *   1. Producer (peerFlapper): Flaps peers 10 times (bring up, add routes,
 * bring down)
 *   2. Consumer (queueDrainer): Slowly pops items from ribInQ_ with 10ms delay
 *
 * The test uses:
 * - folly::coro::timeout() to detect if the producer gets stuck (5 second
 * timeout)
 * - folly::coro::co_awaitTry() to handle timeout/cancellation
 *   - timeout would cause the test to crash and fail
 *   - cancellation is issued when flapping is properly completed
 * - folly::CancellationSource to signal the consumer to stop after producer
 * completes
 *
 * Expected behavior:
 * - Producer should complete all 10 flaps without getting stuck
 * - Consumer drains the queue, preventing indefinite blocking
 * - If producer blocks for more than 5 seconds, test fails with timeout error
 */
TEST_F(PeerManagerE2ETest, PeerFlappingUnderIngressBackPressureTest) {
  // Create a shallow rib in queue to create back pressure
  ribInQ_ = nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>(1);
  setupComponents();

  // Cancellation source to signal when flapping is complete
  folly::CancellationSource cancellationSource;

  // Producer coroutine: flaps the peer 10 times
  auto peerFlapper = [this]() -> folly::coro::Task<void> {
    XLOG(INFO, "=== Flapping start ===");
    for (int i = 0; i < 10; ++i) {
      XLOGF(
          INFO,
          "=== [{}] Turn up peers, ribInQ_ size = {} ===",
          i,
          ribInQ_.size());
      bringUpPeer(kPeerAddr3);
      bringUpPeer(kPeerAddr4);
      // Add some routes so that we would block the queue later
      XLOGF(
          INFO,
          "=== [{}] Add routes, ribInQ_ size = {} ===",
          i,
          ribInQ_.size());
      addRoute(
          "v4" /* protocol */,
          "10.0.1.0" /* prefix */,
          24 /* prefixLen */,
          kPeerAddr3 /* peer */,
          kPeerAddr3.str() /* nexthop - same as peer */,
          "" /* asPath */,
          "" /* community */);
      addRoute(
          "v6" /* protocol */,
          "2401:db8::" /* prefix */,
          64 /* prefixLen */,
          kPeerAddr4 /* peer */,
          kPeerAddr4.str() /* nexthop - same as peer */,
          "" /* asPath */,
          "" /* community */);

      XLOGF(
          INFO,
          "=== [{}] Flap peers, ribInQ_ size = {} ===",
          i,
          ribInQ_.size());
      bringDownPeer(kPeerAddr3);
      bringDownPeer(kPeerAddr4);
      XLOGF(
          INFO,
          "=== [{}] Peers are down, ribInQ_ size = {} ===",
          i,
          ribInQ_.size());
    }
    XLOG(INFO, "=== Flapping complete ===");
    co_return;
  };

  /*
   * Consumer coroutine: pops items from ribInQ_
   * Uses co_awaitTry with timeout to gracefully handle cancellation or timeout
   */
  auto queueDrainer = [this]() -> folly::coro::Task<void> {
    XLOG(INFO, "=== Queue drainer start ===");
    while (true) {
      auto result = co_await folly::coro::co_awaitTry(
          folly::coro::timeout(ribInQ_.pop(), std::chrono::seconds(5)));
      if (result.hasException()) {
        if (result.exception()
                .template is_compatible_with<folly::FutureTimeout>()) {
          // fail with error message
          co_yield folly::coro::co_error(
              std::runtime_error(
                  "Peer flapping is stuck for 5 seconds, ribInQ_ size = " +
                  std::to_string(ribInQ_.size())));
        } else {
          XLOG(INFO, "=== Queue drainer cancelled, exiting gracefully ===");
        }
        co_return;
      }
      XLOGF(
          INFO,
          "=== Popped message from ribInQ_, size = {} ===",
          ribInQ_.size());
      /*
       * wait for 10 ms to simulate the consumer being slow
       * no cancellation should be triggered here
       */
      co_await folly::coro::co_withCancellation(
          folly::CancellationToken{},
          folly::coro::sleep(std::chrono::milliseconds(10)));
    }
  };

  // Start the queue drainer FIRST so it's ready to consume
  std::thread drainerThread([&queueDrainer, &cancellationSource]() {
    folly::coro::blockingWait(
        folly::coro::co_withCancellation(
            cancellationSource.getToken(), queueDrainer()));
  });

  // Start the peer flapper on its own thread
  std::thread flapperThread(
      [&peerFlapper]() { folly::coro::blockingWait(peerFlapper()); });

  // Wait for flapper to complete, then cancel drainer
  flapperThread.join();
  cancellationSource.requestCancellation();
  drainerThread.join();
}

} // namespace bgp
} // namespace facebook
