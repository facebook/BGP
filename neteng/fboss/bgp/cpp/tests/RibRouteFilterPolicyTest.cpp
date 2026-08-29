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

#include <folly/coro/BlockingWait.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define RibBase_TEST_FRIENDS                                                 \
  friend class RibFsdbFixture;                                               \
  FRIEND_TEST(RibFixtureAddPathTestSuite, SetGetClearRouteFilterPolicyTest); \
  FRIEND_TEST(RibFsdbAddPathTestSuite, ReplaceRouteFilterPolicyTest);        \
  FRIEND_TEST(RibFsdbAddPathTestSuite, ReplaceRouteFilterPolicyForceUpdateTest);

#define MockRib_TEST_FRIENDS                                          \
  FRIEND_TEST(RibFsdbAddPathTestSuite, ReplaceRouteFilterPolicyTest); \
  FRIEND_TEST(RibFsdbAddPathTestSuite, ReplaceRouteFilterPolicyForceUpdateTest);

#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/rib_policy_types.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibFsdbPolicyTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibUtils.h"

using namespace facebook::bgp::rib_policy;
using namespace facebook::neteng::fboss::bgp::thrift;

namespace facebook {
namespace bgp {

INSTANTIATE_TEST_SUITE_P(
    RibFixture,
    RibFixtureAddPathTestSuite,
    testing::Values(true /* addPath */));

INSTANTIATE_TEST_SUITE_P(
    RibFixture,
    RibFsdbAddPathTestSuite,
    testing::Values(true /* addPath */));

TEST_P(RibFixtureAddPathTestSuite, SetGetClearRouteFilterPolicyTest) {
  // test setRouteFilterPolicy, getRouteFilterPolicy, and
  // clearRouteFilterPolicy
  rib_->setFibBatchTime(std::chrono::milliseconds(2));

  // Create the tRouteFilterPolicy and tRibPolicy for testing
  TRouteFilterPolicy tRouteFilterPolicy = createTRouteFilterPolicy(
      {createTRouteFilterStatement({kV4Prefix1})}, 12345);

  // send EoR
  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();

  // setRouteFilterPolicy
  {
    sendRouteFilterPolicySet(tRouteFilterPolicy);

    // wait till rib policy output queue got item
    auto outputPolicy = rib_->waitForRouteFilterPolicyUpdate();

    // new policy is set
    EXPECT_EQ(tRouteFilterPolicy, outputPolicy);
    EXPECT_EQ(tRouteFilterPolicy, rib_->getRouteFilterPolicy());
    EXPECT_NE(nullptr, rib_->routeFilterPolicy_);
  }
  // clearRouteFilterPolicy
  {
    rib_->clearRouteFilterPolicy();
    // wait till rib policy output queue got item
    auto outputPolicy = rib_->waitForRouteFilterPolicyClear();

    // nothing is set
    EXPECT_EQ(TRouteFilterPolicy{}, outputPolicy);
    EXPECT_EQ(TRouteFilterPolicy{}, rib_->getRouteFilterPolicy());
    EXPECT_EQ(nullptr, rib_->routeFilterPolicy_);
  }
  // Test BgpService
  {
    TResult result;

    // Peer manager needs to be running before setting route filter policy
    auto peerMgrThread = peerManager_->runInThread();
    auto sessionMgrThread = sessionMgr_->runInThread();

    // successfully set route filter policy
    auto ribPolicyReplaceFuture = rib_->getRibPolicyReplaceFuture();
    result = *folly::coro::blockingWait(service_->co_setRouteFilterPolicy(
        std::make_unique<TRouteFilterPolicy>(tRouteFilterPolicy)));
    EXPECT_TRUE(*result.success());
    ribPolicyReplaceFuture.wait();

    auto tPolicy =
        *folly::coro::blockingWait(service_->co_getRouteFilterPolicy());
    EXPECT_EQ(tPolicy, tRouteFilterPolicy);
    EXPECT_NE(nullptr, rib_->routeFilterPolicy_);

    // set a smaller version would fail
    TRouteFilterPolicy tStaleRfPolicy = createTRouteFilterPolicy(
        {createTRouteFilterStatement({kV4Prefix3})}, 123);
    ribPolicyReplaceFuture = rib_->getRibPolicyReplaceFuture();
    result = *folly::coro::blockingWait(service_->co_setRouteFilterPolicy(
        std::make_unique<TRouteFilterPolicy>(tStaleRfPolicy)));
    EXPECT_TRUE(*result.success());
    ribPolicyReplaceFuture.wait();

    tPolicy = *folly::coro::blockingWait(service_->co_getRouteFilterPolicy());
    // tStaleRfPolicy should not be applied (not changed)
    EXPECT_EQ(tPolicy, tRouteFilterPolicy);
    EXPECT_NE(nullptr, rib_->routeFilterPolicy_);
    EXPECT_EQ(
        *tRouteFilterPolicy.version(), rib_->routeFilterPolicy_->getVersion());

    // set a equal version would succeed
    TRouteFilterPolicy tNewRfPolicyWithSameVersion = createTRouteFilterPolicy(
        {createTRouteFilterStatement({kV4Prefix3})}, 12345);

    ribPolicyReplaceFuture = rib_->getRibPolicyReplaceFuture();
    result = *folly::coro::blockingWait(service_->co_setRouteFilterPolicy(
        std::make_unique<TRouteFilterPolicy>(tNewRfPolicyWithSameVersion)));
    EXPECT_TRUE(*result.success());
    ribPolicyReplaceFuture.wait();

    tPolicy = *folly::coro::blockingWait(service_->co_getRouteFilterPolicy());
    // tNewRfPolicyWithSameVersion should be applied
    EXPECT_EQ(tPolicy, tNewRfPolicyWithSameVersion);
    EXPECT_NE(nullptr, rib_->routeFilterPolicy_);
    EXPECT_EQ(
        *tNewRfPolicyWithSameVersion.version(),
        rib_->routeFilterPolicy_->getVersion());

    // clear set policy
    ribPolicyReplaceFuture = rib_->getRibPolicyReplaceFuture();
    folly::coro::blockingWait(service_->co_clearRouteFilterPolicy());
    ribPolicyReplaceFuture.wait();

    tPolicy = *folly::coro::blockingWait(service_->co_getRouteFilterPolicy());
    EXPECT_EQ(tPolicy, TRouteFilterPolicy{});
    EXPECT_EQ(nullptr, rib_->routeFilterPolicy_);

    peerManager_->stop();
    sessionMgr_->stop();
    peerMgrThread.join();
    sessionMgrThread.join();
  }
}

TEST_P(RibFsdbAddPathTestSuite, ReplaceRouteFilterPolicyTest) {
  auto subscribedPolicy = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().routeFilterPolicy());

  rib_->setFibBatchTime(std::chrono::milliseconds(2));

  TRouteFilterPolicy tRouteFilterPolicy = createTRouteFilterPolicy(
      {createTRouteFilterStatement({kV4Prefix1})}, 12345);
  TRibPolicy tRibPolicy;

  // send EoR
  sendInitialPathComputation();

  // test backward compatibility, should not crash
  {
    // nullptr
    rib_->evb_.runInEventBaseThreadAndWait([&]() {
      rib_->replaceRibPolicy(nullptr);
      EXPECT_EQ(rib_->routeFilterPolicy_, nullptr);
    });

    rib_->evb_.runInEventBaseThreadAndWait([&]() {
      // empty rib policy
      EXPECT_THROW(
          rib_->replaceRibPolicy(std::make_unique<RibPolicy>(tRibPolicy)),
          BgpError);
      EXPECT_EQ(rib_->routeFilterPolicy_, nullptr);
    });

    tRibPolicy.route_filter_policy() = tRouteFilterPolicy;
    rib_->evb_.runInEventBaseThreadAndWait([&]() {
      // with route filter policy
      rib_->replaceRibPolicy(std::make_unique<RibPolicy>(tRibPolicy));
      EXPECT_NE(rib_->routeFilterPolicy_, nullptr);
    });

    WITH_RETRIES_N(5, {
      auto policyLk = subscribedPolicy.rlock();
      ASSERT_EVENTUALLY_TRUE(policyLk->has_value());
      EXPECT_EVENTUALLY_EQ(*policyLk, tRouteFilterPolicy);
    });

    // clean up the route filter policy
    rib_->evb_.runInEventBaseThreadAndWait([&]() {
      rib_->replaceRibPolicy(nullptr);
      EXPECT_EQ(rib_->routeFilterPolicy_, nullptr);
    });

    WITH_RETRIES_N(
        5, ASSERT_EVENTUALLY_TRUE(subscribedPolicy.rlock()->has_value()));
  }
  {
    // Add route filter policy
    // It should succeed as we have no policy before
    // We need to run replaceRouteFilterPolicy in runInEventBaseThreadAndWait
    // as we could potentially call schedulePrepareFibProgrammingTimer, which
    // should be run in the Rib event base thread
    rib_->evb_.runInEventBaseThreadAndWait([&]() {
      rib_->replaceRouteFilterPolicy(
          std::make_unique<RouteFilterPolicy>(tRouteFilterPolicy));
    });
    EXPECT_NE(rib_->routeFilterPolicy_, nullptr);

    // Pushing the same one should not replace it
    rib_->evb_.runInEventBaseThreadAndWait([&]() {
      auto hasUpdate = rib_->replaceRouteFilterPolicy(
          std::make_unique<RouteFilterPolicy>(tRouteFilterPolicy));

      EXPECT_FALSE(hasUpdate);
    });

    // change the route filter policy
    tRouteFilterPolicy.version() = 67890;
    rib_->evb_.runInEventBaseThreadAndWait([&]() {
      rib_->replaceRouteFilterPolicy(
          std::make_unique<RouteFilterPolicy>(tRouteFilterPolicy));
    });
    EXPECT_NE(rib_->routeFilterPolicy_, nullptr);

    // Clear route filter policy
    rib_->evb_.runInEventBaseThreadAndWait(
        [&]() { rib_->replaceRouteFilterPolicy(nullptr); });
    EXPECT_EQ(rib_->routeFilterPolicy_, nullptr);
  }
}

/*
 * This test verifies that replaceRouteFilterPolicy() with forceUpdate=true
 * bypasses the version check and applies the policy even when the version
 * is the same. This is used by FILE_MODE (setCrfPolicyFromFile) where the
 * version may not increment across daemon restarts.
 */
TEST_P(RibFsdbAddPathTestSuite, ReplaceRouteFilterPolicyForceUpdateTest) {
  rib_->setFibBatchTime(std::chrono::milliseconds(2));

  TRouteFilterPolicy tRouteFilterPolicy = createTRouteFilterPolicy(
      {createTRouteFilterStatement({kV4Prefix1})}, 12345);

  sendInitialPathComputation();

  // Set initial policy with version 12345
  rib_->evb_.runInEventBaseThreadAndWait([&]() {
    rib_->replaceRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(tRouteFilterPolicy));
  });
  EXPECT_NE(rib_->routeFilterPolicy_, nullptr);
  EXPECT_EQ(12345, rib_->routeFilterPolicy_->getVersion());

  // Same policy without forceUpdate - should NOT update (hasUpdate=false)
  rib_->evb_.runInEventBaseThreadAndWait([&]() {
    auto hasUpdate = rib_->replaceRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(tRouteFilterPolicy));
    EXPECT_FALSE(hasUpdate);
  });

  // Different content, same version, without forceUpdate - should update
  // (Rib accepts same version if content differs)
  TRouteFilterPolicy differentPolicy = createTRouteFilterPolicy(
      {createTRouteFilterStatement({kV4Prefix2})}, 12345);
  rib_->evb_.runInEventBaseThreadAndWait([&]() {
    auto hasUpdate = rib_->replaceRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(differentPolicy));
    EXPECT_TRUE(hasUpdate);
  });

  // Same content, lower version, without forceUpdate - should NOT update
  TRouteFilterPolicy lowerVersionPolicy = createTRouteFilterPolicy(
      {createTRouteFilterStatement({kV4Prefix2})}, 100);
  rib_->evb_.runInEventBaseThreadAndWait([&]() {
    auto hasUpdate = rib_->replaceRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(lowerVersionPolicy));
    EXPECT_FALSE(hasUpdate);
    EXPECT_EQ(12345, rib_->routeFilterPolicy_->getVersion());
  });

  // Different content, lower version, with forceUpdate=true - should update
  // (forceUpdate bypasses version check, content differs so hasUpdate=true)
  TRouteFilterPolicy forcePolicy = createTRouteFilterPolicy(
      {createTRouteFilterStatement({kV4Prefix1})}, 100);
  rib_->evb_.runInEventBaseThreadAndWait([&]() {
    auto hasUpdate = rib_->replaceRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(forcePolicy),
        /*isBootstrap=*/false,
        /*forceUpdate=*/true);
    EXPECT_TRUE(hasUpdate);
    EXPECT_EQ(100, rib_->routeFilterPolicy_->getVersion());
  });

  // Same content, same version, with forceUpdate=true - should NOT update
  // (forceUpdate only bypasses version check, not content check)
  rib_->evb_.runInEventBaseThreadAndWait([&]() {
    auto hasUpdate = rib_->replaceRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(forcePolicy),
        /*isBootstrap=*/false,
        /*forceUpdate=*/true);
    EXPECT_FALSE(hasUpdate);
  });

  // Same statements, different version, with forceUpdate=true - should update
  // (operator!= includes version in comparison, so policies are "different")
  TRouteFilterPolicy sameStmtsDiffVersion =
      createTRouteFilterPolicy({createTRouteFilterStatement({kV4Prefix1})}, 50);
  rib_->evb_.runInEventBaseThreadAndWait([&]() {
    auto hasUpdate = rib_->replaceRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(sameStmtsDiffVersion),
        /*isBootstrap=*/false,
        /*forceUpdate=*/true);
    EXPECT_TRUE(hasUpdate);
    EXPECT_EQ(50, rib_->routeFilterPolicy_->getVersion());
  });

  // nullptr with forceUpdate=true - clearing should always work
  rib_->evb_.runInEventBaseThreadAndWait([&]() {
    auto hasUpdate = rib_->replaceRouteFilterPolicy(
        nullptr,
        /*isBootstrap=*/false,
        /*forceUpdate=*/true);
    EXPECT_TRUE(hasUpdate);
    EXPECT_EQ(rib_->routeFilterPolicy_, nullptr);
  });
}

} // namespace bgp
} // namespace facebook
