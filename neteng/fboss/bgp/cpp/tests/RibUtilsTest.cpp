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

#include <gtest/gtest.h>

#define MockRib_TEST_FRIENDS                                   \
  FRIEND_TEST(RibFixture, GetRibPolicyReplaceFutureTest);      \
  FRIEND_TEST(RibFixture, FulfillRibPolicyReplacePromiseTest); \
  FRIEND_TEST(RibFixture, ReplaceRibPolicyTest);               \
  FRIEND_TEST(RibFixture, ReplaceRouteAttributePolicyTest);    \
  FRIEND_TEST(RibFixture, ReplacePathSelectionPolicyTest);     \
  FRIEND_TEST(RibFixture, ReplaceRouteFilterPolicyTest);

#include "neteng/fboss/bgp/cpp/tests/RibUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

TEST_F(RibFixture, GetRibPolicyReplaceFutureTest) {
  {
    rib_->getRibPolicyReplaceFuture();

    // Has a promise
    rib_->ribPolicyReplacePromise_.withRLock(
        [&](auto& promise) { EXPECT_NE(promise, nullptr); });
  }

  {
    /*
     * getting a new future without settling the previous one would lead to
     * crash
     */
    EXPECT_DEATH(rib_->getRibPolicyReplaceFuture(), "");
  }
}

TEST_F(RibFixture, FulfillRibPolicyReplacePromiseTest) {
  auto future = rib_->getRibPolicyReplaceFuture();

  // Has a promise
  rib_->ribPolicyReplacePromise_.withRLock(
      [&](auto& promise) { EXPECT_NE(promise, nullptr); });

  // Fulfill the promise
  rib_->fulfillRibPolicyReplacePromise();

  // Promise is cleared
  rib_->ribPolicyReplacePromise_.withRLock(
      [&](auto& promise) { EXPECT_EQ(promise, nullptr); });

  // Future is fulfilled
  EXPECT_TRUE(future.isReady());
}

TEST_F(RibFixture, ReplaceRibPolicyTest) {
  auto future = rib_->getRibPolicyReplaceFuture();

  rib_->replaceRibPolicy(nullptr);

  // fulfilled
  EXPECT_TRUE(future.isReady());
}

TEST_F(RibFixture, ReplaceRouteAttributePolicyTest) {
  auto future = rib_->getRibPolicyReplaceFuture();

  rib_->replaceRouteAttributePolicy(nullptr);

  // fulfilled
  EXPECT_TRUE(future.isReady());
}

TEST_F(RibFixture, ReplacePathSelectionPolicyTest) {
  auto future = rib_->getRibPolicyReplaceFuture();

  rib_->replacePathSelectionPolicy(nullptr);

  // fulfilled
  EXPECT_TRUE(future.isReady());
}

TEST_F(RibFixture, ReplaceRouteFilterPolicyTest) {
  auto future = rib_->getRibPolicyReplaceFuture();

  rib_->replaceRouteFilterPolicy(nullptr);

  // fulfilled
  EXPECT_TRUE(future.isReady());
}

TEST_F(RibFixture, MockFibUpdateUnicastRouteTest) {
  // Create a simple prefix
  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/24");

  // Create a WeightedNexthopMap with one nexthop
  auto weightedNexthops = std::make_shared<WeightedNexthopMap>();
  weightedNexthops->emplace(folly::IPAddress("192.168.1.1"), 0);

  // Set up expectation for the updateUnicastRoute_ method call
  EXPECT_CALL(
      *fib_,
      updateUnicastRoute_(
          testing::_,
          testing::_,
          testing::_,
          testing::_,
          testing::_,
          testing::_))
      .Times(1);

  // Call updateUnicastRoute with all parameters including BgpRouteType
  fib_->updateUnicastRoute(
      prefix,
      nullptr, // attrsToBeAdvertised
      weightedNexthops,
      false, // isLocalRouteBest
      true, // installToFib
      getNexthopInfoMap(), // nexthopInfoMap
      std::nullopt, // classId
      nullptr, // nexthopTopoInfoMap
      BgpRouteType::EBGP); // routeType - this covers the untested parameter
}
} // namespace facebook::bgp
