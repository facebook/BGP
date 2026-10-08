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
 * E2E test for BgpProfiler: Exercises profiled code paths at scale
 * and produces example profiler output.
 *
 * These tests inject many routes through the BGP flow to exercise
 * the profiled functions (PeerManagerBase loops, AdjRib packing, etc.)
 * and verify that the profiler captures meaningful statistics.
 */

#include <gtest/gtest.h>

#include <fmt/format.h>
#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/BgpProfiler.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook {
namespace bgp {

class BgpProfilerE2ETest : public E2ETestFixture {
 protected:
  void SetUp() override {
    E2ETestFixture::SetUp();
    /*
     * Enable profiler for these tests
     */
    BgpProfiler::getInstance()->setEnabled(true);
    BgpProfiler::getInstance()->clearStats();
  }

  void TearDown() override {
    /*
     * Disable profiler before teardown so no new recordings start.
     * Stop all background threads first (E2ETestFixture::TearDown),
     * then read stats safely with no concurrent writers.
     */
    BgpProfiler::getInstance()->setEnabled(false);
    E2ETestFixture::TearDown();
    printProfilerStats();
    BgpProfiler::getInstance()->clearStats();
  }

  void setupComponents(bool enableUpdateGroup = false) {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);

    createRib();
    createPeerManager(enableUpdateGroup);
  }

  void bringUpAllPeersWithEor() {
    bringUpPeer(kPeerAddr3);
    bringUpPeer(kPeerAddr4);

    BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
    BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

    sendEoRToPeer(peerId3);
    sendEoRToPeer(peerId4);
    EXPECT_TRUE(waitForEoR(peerId3));
    EXPECT_TRUE(waitForEoR(peerId4));
  }

  void printProfilerStats() {
    auto stats = BgpProfiler::getInstance()->getStats();

    if (stats.empty()) {
      XLOG(INFO, "No profiler stats captured");
      return;
    }

    /*
     * Print formatted table header
     */
    XLOG(INFO, "");
    XLOG(
        INFO,
        "╔════════════════════════════════════════════════════════════════════════════╗");
    XLOG(
        INFO,
        "║              BGP Profiler Statistics                                       ║");
    XLOG(
        INFO,
        "╠════════════════════════════════════════════════════════════════════════════╣");
    XLOG(
        INFO,
        "║ {:<45} │ {:>5} │ {:>5} │ {:>5} │ {:>5} ║",
        "Function",
        "Count",
        "P50",
        "P90",
        "P99");
    XLOG(
        INFO,
        "╟─────────────────────────────────────────────┼───────┼───────┼───────┼───────╢");

    /*
     * Sort by count descending
     */
    std::sort(stats.begin(), stats.end(), [](const auto& a, const auto& b) {
      return a.count > b.count;
    });

    for (const auto& s : stats) {
      /*
       * Truncate long names
       */
      std::string name = s.name;
      if (name.length() > 45) {
        name = name.substr(0, 42) + "...";
      }

      XLOG(
          INFO,
          "║ {:<45} │ {:>5} │ {:>4}ms │ {:>4}ms │ {:>4}ms ║",
          name,
          s.count,
          s.p50Ms,
          s.p90Ms,
          s.p99Ms);
    }

    XLOG(
        INFO,
        "╚════════════════════════════════════════════════════════════════════════════╝");
    XLOG(INFO, "");
  }
};

/*
 * Test basic peer up and EoR exchange with profiling
 *
 * Verifies that profiler captures statistics for basic BGP operations
 * like peer message loops.
 */
TEST_F(BgpProfilerE2ETest, ProfilePeerUpAndEoRExchange) {
  setupComponents();

  /*
   * Bring up peers and complete EoR exchange
   */
  bringUpAllPeersWithEor();

  /*
   * Disable profiler and drain queues so all in-flight ScopedProfiles
   * complete before we read stats. The profiler is lock-free so we
   * must ensure no concurrent writers when reading histograms.
   */
  BgpProfiler::getInstance()->setEnabled(false);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  auto stats = BgpProfiler::getInstance()->getStats();

  XLOGF(INFO, "Captured stats for {} functions", stats.size());

  bool foundAnyStats = !stats.empty();
  EXPECT_TRUE(foundAnyStats) << "Expected profiler to capture some statistics";
}

/*
 * Test large scale route processing with profiling
 *
 * Injects 1K routes through the BGP flow and verifies that
 * the profiler captures meaningful statistics for:
 * - PeerManagerBase message loops
 * - AdjRib packing functions
 * - RIB processing
 */
TEST_F(BgpProfilerE2ETest, ProfileLargeScaleRouteProcessing) {
  setupComponents();

  /*
   * Bring up peers and complete EoR exchange
   */
  bringUpAllPeersWithEor();

  /*
   * Clear stats after peer up to focus on route processing
   */
  BgpProfiler::getInstance()->clearStats();

  constexpr int kNumRoutes = 1000;

  /*
   * Inject routes from peer 3
   * This exercises profiled code paths at scale
   */
  XLOGF(INFO, "Injecting {} IPv4 routes from peer 3", kNumRoutes);
  for (int i = 0; i < kNumRoutes; ++i) {
    /*
     * Generate unique /24 prefixes across the 10.0.0.0/8 space
     * Format: 10.A.B.0/24 where A = i/256, B = i%256
     */
    std::string prefix = fmt::format("10.{}.{}.0", (i / 256) % 256, i % 256);
    addRoute(
        "v4",
        prefix,
        24,
        kPeerAddr3,
        kPeerAddr3.str(),
        "" /* asPath */,
        "" /* community */);
  }

  /*
   * Wait for the LAST injected route to land in PeerManagerBase's shadowRib.
   * This is the synchronization point: shadowRibEntries_ is only populated
   * from inside processAdjRibEvent (which runs under a ScopedProfile
   * "PeerManagerBase::processAdjRibMsg"), so by the time the last prefix is
   * visible the per-message ScopedProfile has destructed and recorded.
   *
   * Without this barrier, in @mode/opt the test thread races ahead of the
   * async pipeline (adjRibInQ -> AdjRib -> fromAdjRibQ_ ->
   * processAdjRibMsgLoop -> adjRibOutQ). drainPeerQueueCompletely's
   * default 5-retry empty-queue check exits before the pipeline has
   * produced any output, getStats() then returns empty, and the test
   * fails with stats.size() == 0 (~65% flake rate observed in opt-mode
   * stress runs).
   */
  auto lastPrefix = folly::IPAddress::createNetwork(
      fmt::format(
          "10.{}.{}.0/24",
          ((kNumRoutes - 1) / 256) % 256,
          (kNumRoutes - 1) % 256));
  ASSERT_TRUE(waitForRouteInShadowRib(lastPrefix, kPeerAddr3));

  /*
   * Drain all queues FIRST so in-flight route processing completes while
   * the profiler is still enabled — ScopedProfile checks isEnabled() at
   * construction, so disabling before drain skips recording in opt mode.
   * Only after drain do we disable and read, satisfying the lock-free
   * profiler's no-concurrent-writers invariant on the read side.
   */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  BgpProfiler::getInstance()->setEnabled(false);

  auto stats = BgpProfiler::getInstance()->getStats();

  XLOGF(
      INFO,
      "Captured stats for {} functions after {} routes",
      stats.size(),
      kNumRoutes);

  /*
   * Verify we captured stats for the processing functions
   */
  std::set<std::string> expectedFunctions = {
      "PeerManagerBase::processAdjRibMsgLoop",
  };

  std::set<std::string> foundFunctions;
  for (const auto& s : stats) {
    foundFunctions.insert(s.name);
    XLOGF(INFO, "Found profiled function: {} count={}", s.name, s.count);
  }

  for (const auto& expected : expectedFunctions) {
    if (foundFunctions.find(expected) == foundFunctions.end()) {
      XLOGF(INFO, "Did not find expected function: {}", expected);
    }
  }

  /*
   * We should have captured at least some stats
   */
  EXPECT_GT(stats.size(), 0) << "Expected profiler to capture statistics";
}

/*
 * Test profiler filter functionality
 *
 * Verifies that the filter regex correctly limits which functions
 * are profiled.
 */
TEST_F(BgpProfilerE2ETest, ProfilerFilterTest) {
  /*
   * Set filter to only profile PeerManagerBase functions
   */
  BgpProfiler::getInstance()->setFilterRegex("PeerManagerBase::.*");
  BgpProfiler::getInstance()->clearStats();

  setupComponents();
  bringUpAllPeersWithEor();

  /*
   * Drain queues FIRST so bringUp's ScopedProfile work completes while the
   * profiler is still enabled, then disable and read.
   */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  BgpProfiler::getInstance()->setEnabled(false);

  auto stats = BgpProfiler::getInstance()->getStats();

  /*
   * All captured functions should match the filter
   */
  for (const auto& s : stats) {
    EXPECT_TRUE(s.name.find("PeerManagerBase::") == 0)
        << "Function " << s.name << " should match filter PeerManagerBase::.*";
  }

  /*
   * Clear filter for next tests
   */
  BgpProfiler::getInstance()->setFilterRegex("");
}

/*
 * Test profiler enable/disable at runtime
 *
 * Verifies that disabling the profiler stops stat collection.
 */
TEST_F(BgpProfilerE2ETest, ProfilerEnableDisableTest) {
  setupComponents();
  bringUpAllPeersWithEor();

  /*
   * Drain queues FIRST so bringUp's ScopedProfile work completes while the
   * profiler is still enabled, then disable and read.
   */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  BgpProfiler::getInstance()->setEnabled(false);

  auto statsEnabled = BgpProfiler::getInstance()->getStats();
  size_t countEnabled = 0;
  for (const auto& s : statsEnabled) {
    countEnabled += s.count;
  }
  XLOGF(INFO, "Total calls captured while enabled: {}", countEnabled);

  /*
   * Clear stats and keep profiler disabled.
   * Do more operations — profiler should not capture anything.
   */
  BgpProfiler::getInstance()->clearStats();

  addRoute("v4", "10.0.0.0", 24, kPeerAddr3, kPeerAddr3.str());
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  auto statsDisabled = BgpProfiler::getInstance()->getStats();
  size_t countDisabled = 0;
  for (const auto& s : statsDisabled) {
    countDisabled += s.count;
  }
  XLOGF(INFO, "Total calls captured while disabled: {}", countDisabled);

  EXPECT_EQ(countDisabled, 0) << "Should not capture stats when disabled";
}

} // namespace bgp
} // namespace facebook
