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

#include <folly/Random.h>

#include "neteng/fboss/bgp/cpp/rib/facebook/RibPolicyLogger.h"
#include "neteng/fboss/bgp/cpp/tests/MockScubaData.h"

namespace facebook::bgp {
using namespace ::testing;

TEST(RibPolicyLoggerTest, BasicTest) {
  // test with nullptr scuba logger to verfiy no crash
  {
    auto logger = std::make_unique<ScubaRibPolicyLogger>("rsw001", nullptr);
    EXPECT_EQ(0, logger->log(1, 2));
  }
  // test with mocked scuba logger to verify data is populated correctly
  {
    auto mockScuba = std::make_shared<MockScubaData>();
    EXPECT_CALL(*mockScuba, addSample(_, _, _, _))
        .Times(1)
        .WillOnce(Invoke(
            [&](const auto& sample,
                auto /* unused */,
                auto /* unused */,
                auto /* unused */) -> size_t {
              EXPECT_EQ("rsw001", sample.getNormalValue("device"));
              EXPECT_EQ("1", sample.getNormalValue("ps_policy_version"));
              EXPECT_EQ("2", sample.getNormalValue("rf_policy_version"));
              return 1;
            }));
    auto logger = std::make_unique<ScubaRibPolicyLogger>("rsw001", mockScuba);
    EXPECT_LT(0, logger->log(1, 2));
  }
  /*
   * test with prod scuba to verify that creating a logger for
   * non-existent table and logging to it won't crash
   */
  {
    auto rand32 = folly::Random::rand32();
    auto scuba = std::make_shared<rfe::ScubaData>(
        fmt::format("non_existent_table_{}", rand32));
    auto logger = std::make_unique<ScubaRibPolicyLogger>("rsw001", scuba);
    EXPECT_LT(0, logger->log(1, 2));
  }
}
} // namespace facebook::bgp
