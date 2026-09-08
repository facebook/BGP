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

#include <gflags/gflags.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/Random.h>

#include "neteng/fboss/bgp/cpp/adjrib/facebook/RouteFilterLogger.h"
#include "neteng/fboss/bgp/cpp/tests/MockScubaData.h"

DECLARE_bool(disable_route_filter_scuba_logging);

namespace facebook::bgp {
using namespace ::testing;

TEST(RouteFilterLoggerTest, BasicTest) {
  // test with nullptr scuba logger to verfiy no crash
  {
    auto logger = std::make_unique<ScubaRouteFilterLogger>(
        "rsw001", "rsw.*", "fsw001", nullptr);
    EXPECT_EQ(
        0,
        logger->log(
            true, folly::CIDRNetwork("192.0.2.1", 32), true, false, {}));
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
              EXPECT_EQ("rsw.*", sample.getNormalValue("statement"));
              EXPECT_EQ(1, sample.getIntValue("egress"));
              EXPECT_EQ("fsw001", sample.getNormalValue("peer"));
              EXPECT_EQ("192.0.2.1/32", sample.getNormalValue("prefix"));
              EXPECT_EQ(1, sample.getIntValue("allow"));
              EXPECT_EQ(0, sample.getIntValue("permissive"));
              EXPECT_EQ("123:456", sample.getNormVectorValue("communities")[0]);
              return 1;
            }));

    auto logger = std::make_unique<ScubaRouteFilterLogger>(
        "rsw001", "rsw.*", "fsw001", mockScuba);
    EXPECT_LT(
        0,
        logger->log(
            true,
            folly::CIDRNetwork("192.0.2.1", 32),
            true,
            false,
            {"123:456"}));
  }
  /*
   * test with prod scuba to verify that creating a logger for
   * non-existent table and logging to it won't crash
   */
  {
    auto rand32 = folly::Random::rand32();
    auto scuba = std::make_shared<rfe::ScubaData>(
        fmt::format("there_is_no_way_this_table_exists_{}", rand32));

    auto logger = std::make_unique<ScubaRouteFilterLogger>(
        "rsw001", "rsw.*", "fsw001", scuba);
    EXPECT_LT(
        0,
        logger->log(
            true, folly::CIDRNetwork("192.0.2.1", 32), true, false, {}));
  }
}

/*
 * When disable_route_filter_scuba_logging is set,
 * createRouteFilterLoggerFactory still returns a usable factory, but the
 * loggers it creates have no Scuba backend, so log() is inert and returns 0.
 */
TEST(RouteFilterLoggerTest, DisabledFactoryProducesInertLogger) {
  gflags::FlagSaver flagSaver;
  FLAGS_disable_route_filter_scuba_logging = true;

  auto factory = createRouteFilterLoggerFactory();
  ASSERT_NE(nullptr, factory);

  auto logger = factory->create("rsw001", "rsw.*", "fsw001");
  ASSERT_NE(nullptr, logger);
  EXPECT_EQ(
      0,
      logger->log(true, folly::CIDRNetwork("192.0.2.1", 32), true, false, {}));
}

} // namespace facebook::bgp
