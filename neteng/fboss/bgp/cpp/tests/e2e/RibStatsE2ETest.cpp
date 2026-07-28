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
 * E2E tests for the `show bgp summary` global stats: total loc-RIB prefix
 * count and RIB version.
 *
 * Tests verify:
 * 1. Injecting routes increases both the total prefix count and the RIB
 *    version.
 * 2. Withdrawing routes decreases the total prefix count.
 *
 * Process uptime is a pure function of the Watchdog start time; its
 * deterministic coverage lives in WatchdogTest.GetUptimeSecondsTest and
 * BgpServiceBaseTest.GetProcessUptimeSecondsTest. An E2E sample cannot assert a
 * positive uptime without a controlled start time or a sleep, so it is not
 * duplicated here.
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

class RibStatsE2ETest : public E2ETestFixture {
 protected:
  void setupComponents() {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    addPeer(kDefaultPeerSpec5);

    createRib();
    createPeerManager(
        true /* enableUpdateGroup */, true /* enableEgressBackpressure */);
  }

  /*
   * Thin pass-throughs: RibBase::getNumPrefixes() and getRibVersion() both
   * dispatch to the RIB event base internally (getRibVersion reads the
   * RIB-evb-confined ribVersion_ via that hop), so both are race-free to call
   * directly from the test thread -- no outer evb hop.
   */
  uint64_t getNumPrefixes() {
    return rib_->getNumPrefixes();
  }

  uint64_t getRibVersion() {
    return rib_->getRibVersion();
  }
};

/*
 * Injecting routes increases the total prefix count and the RIB version;
 * each additional distinct prefix bumps both.
 */
TEST_F(RibStatsE2ETest, PrefixCountAndVersionIncreaseOnRouteAdd) {
  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  /* No routes yet: empty RIB, version 0. */
  EXPECT_EQ(getNumPrefixes(), 0);
  EXPECT_EQ(getRibVersion(), 0);

  /* Inject the first prefix from peer3. */
  addRoute("v4", "10.0.1.0", 24, kPeerAddr3, kNextHopV4_3.str());
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.1.0", 24, kPeerAddr5, kNextHopV4_5.str()));

  facebook::fboss::checkWithRetry(
      [&]() { return getNumPrefixes() == 1; },
      20 /* retries */,
      std::chrono::milliseconds(100));
  EXPECT_EQ(getNumPrefixes(), 1);
  const uint64_t versionAfterFirst = getRibVersion();
  EXPECT_GT(versionAfterFirst, 0);

  /* Inject a second, distinct prefix from peer3. */
  addRoute("v4", "10.0.2.0", 24, kPeerAddr3, kNextHopV4_3.str());
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.2.0", 24, kPeerAddr5, kNextHopV4_5.str()));

  facebook::fboss::checkWithRetry(
      [&]() { return getNumPrefixes() == 2; },
      20 /* retries */,
      std::chrono::milliseconds(100));
  EXPECT_EQ(getNumPrefixes(), 2);
  EXPECT_GT(getRibVersion(), versionAfterFirst);
}

/*
 * Withdrawing a route decreases the total prefix count back to zero.
 */
TEST_F(RibStatsE2ETest, PrefixCountDecreasesOnWithdraw) {
  setupComponents();

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr5);

  addRoute("v4", "10.0.1.0", 24, kPeerAddr3, kNextHopV4_3.str());
  EXPECT_TRUE(
      verifyRouteAdd("v4", "10.0.1.0", 24, kPeerAddr5, kNextHopV4_5.str()));

  facebook::fboss::checkWithRetry(
      [&]() { return getNumPrefixes() == 1; },
      20 /* retries */,
      std::chrono::milliseconds(100));
  EXPECT_EQ(getNumPrefixes(), 1);

  /* Withdraw the route; the loc-RIB entry is removed. */
  deleteRoute("v4", "10.0.1.0", 24, kPeerAddr3);
  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.1.0", 24, kPeerAddr5));

  facebook::fboss::checkWithRetry(
      [&]() { return getNumPrefixes() == 0; },
      20 /* retries */,
      std::chrono::milliseconds(100));
  EXPECT_EQ(getNumPrefixes(), 0);
}

} // namespace bgp
} // namespace facebook
