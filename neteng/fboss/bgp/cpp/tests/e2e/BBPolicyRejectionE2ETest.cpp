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
 * BBPolicyRejectionE2ETest.cpp
 *
 * BB-only E2E test. This target links the RibBB fixture (:e2e_test_fixture_bb),
 * so rib_ is a RibBB. It exercises the one piece of behavior that is unique to
 * RibBB and not shared with RibDC: RibBB::processRibPolicyMsgLoop rejects the
 * DC-only policy messages (CPS / CTE) — logging an error and incrementing the
 * numUnsupportedPolicyMsg counter — instead of acting on them. No e2e or unit
 * test covered this contract before; it is the safety guarantee that DC-only
 * features (partial drain, CPS, CTE) never take effect on the EBB platform.
 *
 * See E2ETestFixture.h ("Where does my test go?") for the BB/DC split.
 */

#include <gtest/gtest.h>

#include <folly/logging/xlog.h>

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBB.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

using namespace facebook::bgp;

namespace facebook {
namespace bgp {

class BBPolicyRejectionE2ETest : public E2ETestFixture {
 protected:
  /*
   * Read the numUnsupportedPolicyMsg timeseries (last-60s count). The stat is
   * DEFINE_timeseries(..., fb303::COUNT), so the exported key is ".count.60".
   * STATS_unsupportedPolicyMsg.add() runs on the RIB event-base thread and
   * updates that thread's thread-local stats, so publishStats() must run on
   * the RIB thread to flush them into the global aggregate before reading.
   */
  int64_t unsupportedPolicyMsgCount() {
    rib_->getEventBase().runInEventBaseThreadAndWait(
        [] { fb303::ThreadCachedServiceData::get()->publishStats(); });
    auto tcData = fb303::ThreadCachedServiceData::get();
    const std::string key =
        std::string(RibStatsBB::kUnsupportedPolicyMsg) + ".count.60";
    return tcData->hasCounter(key) ? tcData->getCounter(key) : 0;
  }
};

/*
 * RibBB must reject DC-only policy messages. sendUnsupportedDcPolicyMsgs()
 * pushes three of them (PathSelectionPolicyClear, RouteAttributePolicyClear,
 * RouteAttributePolicyTimer); RibBB::processRibPolicyMsgLoop bumps
 * numUnsupportedPolicyMsg once per message and applies no state change.
 */
TEST_F(BBPolicyRejectionE2ETest, RejectsDcPolicyMessages) {
  XLOG(INFO, "=== Starting RejectsDcPolicyMessages ===");

  /* Only the RIB is needed to exercise the policy-message loop. */
  createRib();

  const int64_t before = unsupportedPolicyMsgCount();

  sendUnsupportedDcPolicyMsgs();

  /* The 3 messages are processed asynchronously on the RIB policy loop. */
  WITH_RETRIES_N(
      50, { EXPECT_EVENTUALLY_EQ(before + 3, unsupportedPolicyMsgCount()); });

  XLOG(INFO, "=== RejectsDcPolicyMessages PASSED ===");
}

} // namespace bgp
} // namespace facebook
