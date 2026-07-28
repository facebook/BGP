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

#include <folly/logging/xlog.h>

#include <folly/coro/BlockingWait.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define MockFibFboss_TEST_FRIENDS              \
  friend class FibFixture;                     \
  FRIEND_TEST(FibFixture, updateUnicastRoute); \
  FRIEND_TEST(FibFixture, TestStopFib);

#define FibFboss_TEST_FRIENDS                    \
  friend class FibFbossUtilFixture;              \
  FRIEND_TEST(FibFbossUtilFixture, syncFib);     \
  FRIEND_TEST(FibFbossUtilFixture, addDel);      \
  FRIEND_TEST(FibFbossUtilFixture, addFail);     \
  FRIEND_TEST(FibFbossUtilFixture, delFail);     \
  FRIEND_TEST(FibFbossUtilFixture, syncFibFail); \
  FRIEND_TEST(FibFbossUtilFixture, keepaliveFail);

#include "fboss/agent/AddressUtil.h"
#include "folly/coro/GmockHelpers.h"
#include "neteng/fboss/bgp/cpp/lib/coro/MPMCQueue.h"
#include "neteng/fboss/bgp/cpp/tests/FibFbossUtils.h"
#include "neteng/fboss/bgp/cpp/tests/MockFibFbossLegacy.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace facebook;
using namespace facebook::bgp;
using facebook::fb303::cpp2::fb_status;

using folly::CIDRNetwork;
using folly::IPAddress;
using folly::IPAddressV4;
using folly::coro::gmock_helpers::CoReturn;
using testing::_;
using testing::AnyNumber;
using ::testing::InSequence;
using testing::InvokeWithoutArgs;
using ::testing::UnorderedElementsAreArray;

class FibFixture : public ::testing::Test {
 public:
  void SetUp() override {
    evbThread_ = std::thread([&]() {
      XLOG(INFO, "Start evb for MockFib programming...");
      fib_ = MockFibFboss::createMockFibFboss(&evb_, asyncScope_, fibMsgQ_);
      evb_.loopForever();
      XLOG(INFO, "Successfully terminated evbThread");
    });
    evb_.waitUntilRunning();
  }

  void TearDown() override {
    evb_.runInEventBaseThreadAndWait([&]() { fib_->stop(); });
    evb_.terminateLoopSoon();
    evbThread_.join();
  }

  std::unique_ptr<MockFibFboss> fib_;
  folly::EventBase evb_;

  // asyncScope is used for coro task cancellation
  folly::coro::CancellableAsyncScope asyncScope_;
  std::thread evbThread_;
  bgp::coro::MPMCQueue<Fib::FibMessage> fibMsgQ_;
  folly::F14NodeMap<folly::IPAddress, facebook::bgp::NexthopInfo>
      nexthopInfoMap_;
};

struct RouteTestData {
  std::vector<std::pair<
      folly::CIDRNetwork,
      std::shared_ptr<const std::unordered_set<folly::IPAddress>>>>
      routesToAdd;
  std::vector<folly::CIDRNetwork> routesToDelete;
  std::vector<fboss::UnicastRoute> routesAddToExpect;
  std::vector<fboss::IpPrefix> routesDeleteToExpect;
};

RouteTestData genRouteTestData(int toAdd, int toDelete) {
  RouteTestData ret;
  int ip = 1;
  auto nhop1 = IPAddress("1.1.1.1");
  auto nhop2 = IPAddress("1.1.1.2");
  auto nhop3 = IPAddress("1.1.1.3");
  auto nhop4 = IPAddress("1.1.1.4");
  auto nhop5 = IPAddress("1.1.1.5");

  auto nhops1 = std::make_shared<std::unordered_set<IPAddress>>();
  nhops1->emplace(nhop1);
  nhops1->emplace(nhop2);
  nhops1->emplace(nhop3);
  nhops1->emplace(nhop4);
  auto nhopsBin1 = std::vector<network::thrift::BinaryAddress>();
  nhopsBin1.emplace_back(network::toBinaryAddress(nhop1));
  nhopsBin1.emplace_back(network::toBinaryAddress(nhop2));
  nhopsBin1.emplace_back(network::toBinaryAddress(nhop3));
  nhopsBin1.emplace_back(network::toBinaryAddress(nhop4));

  auto nhops2 = std::make_shared<std::unordered_set<IPAddress>>();
  nhops2->emplace(nhop2);
  nhops2->emplace(nhop3);
  nhops2->emplace(nhop4);
  nhops2->emplace(nhop5);
  auto nhopsBin2 = std::vector<network::thrift::BinaryAddress>();
  nhopsBin2.emplace_back(network::toBinaryAddress(nhop2));
  nhopsBin2.emplace_back(network::toBinaryAddress(nhop3));
  nhopsBin2.emplace_back(network::toBinaryAddress(nhop4));
  nhopsBin2.emplace_back(network::toBinaryAddress(nhop5));

  for (auto i = 0; i < toAdd; i++) {
    auto prefix = std::make_pair(IPAddress(IPAddressV4::fromLongHBO(ip++)), 32);
    auto& nhops = ((i % 2) == 0) ? nhops1 : nhops2;
    auto& nhopsBin = ((i % 2) == 0) ? nhopsBin1 : nhopsBin2;
    ret.routesToAdd.emplace_back(prefix, nhops);
    auto action = nhopsBin.size() ? fboss::RouteForwardAction::NEXTHOPS
                                  : fboss::RouteForwardAction::DROP;
    fboss::UnicastRoute rt{};
    rt.dest()->ip() = network::toBinaryAddress(prefix.first);
    rt.dest()->prefixLength() = prefix.second;
    rt.nextHopAddrs() = std::move(nhopsBin);
    rt.adminDistance() = fboss::AdminDistance::EBGP;
    rt.action() = action;
    ret.routesAddToExpect.emplace_back(std::move(rt));
  }
  for (auto i = 0; i < toDelete; i++) {
    auto prefix = std::make_pair(IPAddress(IPAddressV4::fromLongHBO(ip++)), 32);
    ret.routesToDelete.emplace_back(prefix);
    fboss::IpPrefix ipPrefix;
    ipPrefix.ip() = network::toBinaryAddress(prefix.first);
    ipPrefix.prefixLength() = prefix.second;
    ret.routesDeleteToExpect.push_back(std::move(ipPrefix));
  }
  return ret;
}

TEST(FibFbossTest, CreateNetworkTopoInfo) {
  const std::unordered_map<std::string, int64_t> topologyInfo{
      {"rack_id", 5}, {"remote_rack_capacity", 3}, {"spine_id", 17}};

  const auto thriftTopologyInfo = FibFboss::createNetworkTopoInfo(topologyInfo);
  EXPECT_EQ(5, thriftTopologyInfo.rack_id());
  EXPECT_EQ(3, thriftTopologyInfo.remote_rack_capacity());
  EXPECT_EQ(17, thriftTopologyInfo.spine_id());
  EXPECT_FALSE(thriftTopologyInfo.plane_id().has_value());
  EXPECT_FALSE(thriftTopologyInfo.spine_capacity().has_value());
  EXPECT_FALSE(thriftTopologyInfo.local_rack_capacity().has_value());
}

TEST_F(FibFixture, TestStopFib) {
  // Step1: MockFib is running in a separate thread. Coro tasks are scheduled
  // with SetUp() call.

  // Verify FibFboss::keepAlive() coro task has been scheduled.
  EXPECT_EQ(asyncScope_.remaining(), 1);

  // Step2: Delay 2 seconds to let coro task run. Cancel coro tasks to make sure
  // no task access the ptr/object inside Fib.
  fib_->evb_->runInEventBaseThreadAndWait(
      [&]() { folly::futures::sleep(std::chrono::seconds(2)).get(); });
  folly::coro::blockingWait(asyncScope_.cancelAndJoinAsync());

  // Verify coro tasks has been cancelled.
  EXPECT_EQ(asyncScope_.remaining(), 0);

  // Step3: Let TearDown() call to handle Fib::stop() to reset client_.
}

// This test verifies updateUnicastRoute()'s behavior for various inputs.
// The output of this function is batch_->toAdd and batch_->toDelete. The
// elements of these vectors will be converted to thrift calls to agent.
TEST_F(FibFixture, updateUnicastRoute) {
  auto lambdaCreateExpectedToAdd =
      [](const CIDRNetwork& prefix,
         std::shared_ptr<WeightedNexthopMap> nhWts,
         std::shared_ptr<NexthopTopoInfoMap> nexthopTopoInfoMap =
             nullptr) -> std::vector<fboss::UnicastRoute> {
    std::vector<fboss::NextHopThrift> tWeightedNexthops;
    XCHECK(nhWts);
    for (const auto& [addr, weight] : *nhWts) {
      fboss::NextHopThrift nht;
      nht.address() = network::toBinaryAddress(addr);
      nht.weight() = weight;
      if (nexthopTopoInfoMap) {
        fboss::NetworkTopologyInformation topoInfo;
        topoInfo.rack_id() = nexthopTopoInfoMap->at(addr).at("rack_id");
        topoInfo.plane_id() = nexthopTopoInfoMap->at(addr).at("plane_id");
        topoInfo.remote_rack_capacity() =
            nexthopTopoInfoMap->at(addr).at("remote_rack_capacity");
        topoInfo.spine_capacity() =
            nexthopTopoInfoMap->at(addr).at("spine_capacity");
        nht.topologyInfo() = std::move(topoInfo);
      }
      tWeightedNexthops.emplace_back(std::move(nht));
    }
    std::vector<fboss::UnicastRoute> expectedToAdd;
    fboss::UnicastRoute toAdd;
    toAdd.dest()->ip() = network::toBinaryAddress(prefix.first);
    toAdd.dest()->prefixLength() = prefix.second;
    toAdd.adminDistance() = fboss::AdminDistance::EBGP;
    toAdd.nextHops() = std::move(tWeightedNexthops);
    expectedToAdd.emplace_back(std::move(toAdd));
    return expectedToAdd;
  };

  auto lambdaCreateExpectedToDelete =
      [](const CIDRNetwork& prefix) -> std::vector<fboss::IpPrefix> {
    std::vector<fboss::IpPrefix> expectedToDelete;
    fboss::IpPrefix ipPrefix;
    ipPrefix.ip() = network::toBinaryAddress(prefix.first);
    ipPrefix.prefixLength() = prefix.second;
    expectedToDelete.push_back(std::move(ipPrefix));
    return expectedToDelete;
  };

  auto clearTestSetup = [&]() {
    fib_->batch_->toAdd.clear();
    fib_->batch_->toDelete.clear();
  };

  auto lambdaTestLocalRoute = [&](const CIDRNetwork& prefix,
                                  const IPAddress& nexthop) {
    {
      // input: add prefix with 0 nexthops, nhWts is empty
      auto nhWts = std::make_shared<WeightedNexthopMap>();
      fib_->updateUnicastRoute(
          prefix, nullptr, nhWts, false, true, nexthopInfoMap_);
      // expected output
      const auto& expectedToDelete = lambdaCreateExpectedToDelete(prefix);
      // test
      EXPECT_THAT(
          fib_->batch_->toDelete, UnorderedElementsAreArray(expectedToDelete));
      EXPECT_TRUE(fib_->batch_->toAdd.empty());
      clearTestSetup();
    }
    {
      // input: add prefix with 0 nexthops, nullptr is passed as nhWts
      fib_->updateUnicastRoute(
          prefix, nullptr, nullptr, false, true, nexthopInfoMap_);
      // expected output
      const auto& expectedToDelete = lambdaCreateExpectedToDelete(prefix);
      // test
      EXPECT_THAT(
          fib_->batch_->toDelete, UnorderedElementsAreArray(expectedToDelete));
      EXPECT_TRUE(fib_->batch_->toAdd.empty());
      clearTestSetup();
    }
    {
      // input: add prefix with local route as bestpath, installToFib = true
      auto nhWts = std::make_shared<WeightedNexthopMap>();
      nhWts->emplace(nexthop, 0);
      fib_->updateUnicastRoute(
          prefix, nullptr, nhWts, true, true, nexthopInfoMap_);
      // expected output: nhWts and nhs should be empty.
      nhWts->clear();
      const auto& expectedToAdd = lambdaCreateExpectedToAdd(prefix, nhWts);
      // test
      EXPECT_TRUE(fib_->batch_->toDelete.empty());
      EXPECT_THAT(
          fib_->batch_->toAdd, UnorderedElementsAreArray(expectedToAdd));
      clearTestSetup();
    }
    {
      // input: add prefix with local route as bestpath, installToFib =
      // false
      auto nhWts = std::make_shared<WeightedNexthopMap>();
      nhWts->emplace(nexthop, 0);
      fib_->updateUnicastRoute(
          prefix, nullptr, nhWts, true, false, nexthopInfoMap_);
      // expected output
      const auto& expectedToDelete = lambdaCreateExpectedToDelete(prefix);
      // test
      EXPECT_THAT(
          fib_->batch_->toDelete, UnorderedElementsAreArray(expectedToDelete));
      EXPECT_TRUE(fib_->batch_->toAdd.empty());
      clearTestSetup();
    }
    {
      // input: add prefix with peer route (redistribute peer) as bestpath,
      // installToFib = false
      auto nhWts = std::make_shared<WeightedNexthopMap>();
      nhWts->emplace(nexthop, 0);
      fib_->updateUnicastRoute(
          prefix, nullptr, nhWts, false, false, nexthopInfoMap_);
      // expected output
      const auto& expectedToDelete = lambdaCreateExpectedToDelete(prefix);
      // test
      EXPECT_THAT(
          fib_->batch_->toDelete, UnorderedElementsAreArray(expectedToDelete));
      EXPECT_TRUE(fib_->batch_->toAdd.empty());
      clearTestSetup();
    }
  };

  // test v4 local route case
  lambdaTestLocalRoute(kV4Prefix1, kLocalV4RoutePeerAddr);
  // test v6 local route case
  lambdaTestLocalRoute(kV6Prefix1, kLocalV6RoutePeerAddr);
  {
    // input: add kV4Prefix1 with 2 non-local nexthops
    auto nhWts = std::make_shared<WeightedNexthopMap>();
    nhWts->emplace(kPeerAddr1, 0);
    nhWts->emplace(kPeerAddr2, 0);
    std::unordered_map<std::string, int64_t> topoInfo{
        {"rack_id", 1},
        {"plane_id", 2},
        {"remote_rack_capacity", 34},
        {"spine_capacity", 35}};
    auto nextHopTopoInfoMap = std::make_shared<NexthopTopoInfoMap>();
    nextHopTopoInfoMap->emplace(kPeerAddr1, topoInfo);
    nextHopTopoInfoMap->emplace(kPeerAddr2, topoInfo);

    fib_->updateUnicastRoute(
        kV4Prefix1,
        nullptr,
        nhWts,
        false,
        true,
        nexthopInfoMap_,
        std::nullopt /* classId */,
        nextHopTopoInfoMap);
    // expected output
    const auto& expectedToAdd =
        lambdaCreateExpectedToAdd(kV4Prefix1, nhWts, nextHopTopoInfoMap);
    // test
    EXPECT_TRUE(fib_->batch_->toDelete.empty());
    EXPECT_THAT(fib_->batch_->toAdd, UnorderedElementsAreArray(expectedToAdd));
    clearTestSetup();
  }

  // folly::coro cancellelation
  folly::coro::blockingWait(asyncScope_.cancelAndJoinAsync());
}

TEST(FibFbossTest, toString) {
  //
  // Positive test for: toString() for fboss::IpPrefix
  //
  {
    folly::IPAddress v4{"10.1.1.1"};
    folly::IPAddress v6{"2620::1"};
    const auto v4PrefixLength = 32;
    const auto v6PrefixLength = 128;

    fboss::IpPrefix prefixV4;
    prefixV4.ip() = network::toBinaryAddress(v4);
    prefixV4.prefixLength() = v4PrefixLength;
    fboss::IpPrefix prefixV6;
    prefixV6.ip() = network::toBinaryAddress(v6);
    prefixV6.prefixLength() = v6PrefixLength;

    EXPECT_EQ(
        fmt::format("{}/{}", v4.str(), v4PrefixLength),
        FibFboss::toString(prefixV4));
    EXPECT_EQ(
        fmt::format("{}/{}", v6.str(), v6PrefixLength),
        FibFboss::toString(prefixV6));
  }

  //
  // Negative test for: toString() for fboss::IpPrefix
  //
  {
    fboss::IpPrefix prefix;
    EXPECT_EQ("", FibFboss::toString(prefix));
  }
}

namespace facebook::bgp {

/**
 * @test Test case for syncFib method.
 */
TEST_F(FibFbossUtilFixture, syncFib) {
  EXPECT_CALL(*agentServiceHandler_, co_syncFib(testing::_, testing::_))
      .WillOnce(testing::Return(returnSyncFibResp()));

  EXPECT_CALL(*agentServiceHandler_, co_aliveSince())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(kAliveSince_));

  EXPECT_CALL(*agentServiceHandler_, co_getSwitchRunState())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(fboss::SwitchRunState::CONFIGURED));

  EXPECT_CALL(
      *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
      markProgrammingSuccess())
      .Times(1);

  folly::coro::blockingWait(fib_->program(true));
}

/**
 * @test Test case for syncFib method failing.
 */
TEST_F(FibFbossUtilFixture, syncFibFail) {
  EXPECT_CALL(*agentServiceHandler_, co_syncFib(testing::_, testing::_))
      .WillOnce(
          testing::Throw(fboss::thrift::FbossBaseError("Simulated exception")));

  EXPECT_CALL(*agentServiceHandler_, co_aliveSince())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(kAliveSince_));

  EXPECT_CALL(*agentServiceHandler_, co_getSwitchRunState())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(fboss::SwitchRunState::CONFIGURED));

  // Do not expect to get call for markProgrammingSuccess()
  EXPECT_CALL(
      *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
      markProgrammingSuccess())
      .Times(0);

  // Simulate behavior of fail failure leading to hold down state.
  {
    InSequence dummy;
    // 1 call is guranteed for syncFib() fail.
    EXPECT_CALL(
        *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
        markProgrammingFail())
        .Times(1);

    // Return 1 FIB programming failure.
    EXPECT_CALL(
        *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
        getRecentFailureCount())
        .Times(1)
        .WillOnce(testing::Return(1));

    // Expect setting up for fib hold down state with 1 failure.
    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        setHoldDownState(1))
        .Times(testing::AtLeast(1));
  }

  // Simulate behavior of clearHoldDownState.
  {
    InSequence dummy;
    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        clearHoldDownState())
        .Times(4)
        .WillRepeatedly(testing::Return(false));

    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        clearHoldDownState())
        .Times(1)
        .WillOnce(testing::Return(true));
  }

  folly::coro::blockingWait(fib_->program(true));

  folly::coro::blockingWait(verifyDelayedFullSyncRequest(
      (fib_->holdDownState_.get()->kLowBackoffTicks) - 1));
}

/**
 * Test case for addDel method.
 */
TEST_F(FibFbossUtilFixture, addDel) {
  // Generate testing data with 20 update and 10 withdraw
  auto data = genRouteTestData(20, 10);

  // populate/prepare the unicast route toAdd/toDel
  for (const auto& route : data.routesToAdd) {
    auto nhWts = std::make_shared<WeightedNexthopMap>();
    for (auto& nh : *route.second) {
      nhWts->emplace(nh, 0);
    }
    fib_->updateUnicastRoute(
        route.first, nullptr, nhWts /* NHs */, false, true, nexthopInfoMap_);
  }
  for (const auto& route : data.routesToDelete) {
    fib_->updateUnicastRoute(
        route, nullptr, nullptr /* NHs */, false, true, nexthopInfoMap_);
  }

  // Set up expectations for co_aliveSince() method
  EXPECT_CALL(*agentServiceHandler_, co_aliveSince())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(kAliveSince_));

  // Set up expectations for co_getSwitchRunState() method
  EXPECT_CALL(*agentServiceHandler_, co_getSwitchRunState())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(fboss::SwitchRunState::CONFIGURED));

  // Set up expectations for co_addUnicastRoutes() method
  EXPECT_CALL(
      *agentServiceHandler_, co_addUnicastRoutes(testing::_, testing::_))
      .WillOnce(testing::Return(returnSyncFibResp()));

  // Set up expectations for co_deleteUnicastRoutes() method
  EXPECT_CALL(
      *agentServiceHandler_, co_deleteUnicastRoutes(testing::_, testing::_))
      .WillOnce(testing::Return(returnSyncFibResp()));

  // Do not expect to get call for syncFib()
  EXPECT_CALL(*agentServiceHandler_, co_syncFib(testing::_, testing::_))
      .Times(0);

  // Set up expectations for markProgrammingSuccess() method, one for add and
  // one for delete.
  EXPECT_CALL(
      *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
      markProgrammingSuccess())
      .Times(2);

  // Call the program method
  folly::coro::blockingWait(fib_->program());
}

/**
 * Test case to simulate behavior when add method fails.
 */
TEST_F(FibFbossUtilFixture, addFail) {
  // Generate testing data with 20 update and 10 withdraw
  auto data = genRouteTestData(20, 10);

  // populate/prepare the unicast route toAdd/toDel
  for (const auto& route : data.routesToAdd) {
    auto nhWts = std::make_shared<WeightedNexthopMap>();
    for (auto& nh : *route.second) {
      nhWts->emplace(nh, 0);
    }
    fib_->updateUnicastRoute(
        route.first, nullptr, nhWts /* NHs */, false, true, nexthopInfoMap_);
  }

  // Set up expectations for co_aliveSince() method
  EXPECT_CALL(*agentServiceHandler_, co_aliveSince())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(kAliveSince_));

  // Set up expectations for co_getSwitchRunState() method
  EXPECT_CALL(*agentServiceHandler_, co_getSwitchRunState())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(fboss::SwitchRunState::CONFIGURED));

  // Set up expectations for co_addUnicastRoutes() method
  EXPECT_CALL(
      *agentServiceHandler_, co_addUnicastRoutes(testing::_, testing::_))
      .WillOnce(
          testing::Throw(fboss::thrift::FbossBaseError("Simulated exception")));

  // Do not expect calls to co_deleteUnicastRoutes() method
  EXPECT_CALL(
      *agentServiceHandler_, co_deleteUnicastRoutes(testing::_, testing::_))
      .Times(0);

  // Do not expect to get call for syncFib()
  EXPECT_CALL(*agentServiceHandler_, co_syncFib(testing::_, testing::_))
      .Times(0);

  // Do not expect to get call for success()
  EXPECT_CALL(
      *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
      markProgrammingSuccess())
      .Times(0);

  // Simulate behavior of fail failure leading to hold down state.
  {
    InSequence dummy;
    // 1 call is guranteed for add() fail.
    EXPECT_CALL(
        *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
        markProgrammingFail())
        .Times(1);

    // Return 1 FIB programming failure.
    EXPECT_CALL(
        *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
        getRecentFailureCount())
        .Times(1)
        .WillOnce(testing::Return(1));

    // Expect setting up for fib hold down state with 1 failure.
    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        setHoldDownState(1))
        .Times(1);
  }
  // Simulate behavior of clearHoldDownState.
  {
    InSequence dummy;
    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        clearHoldDownState())
        .Times(4)
        .WillRepeatedly(testing::Return(false));

    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        clearHoldDownState())
        .Times(1)
        .WillOnce(testing::Return(true));
  }

  folly::coro::blockingWait(fib_->program());

  folly::coro::blockingWait(verifyDelayedFullSyncRequest(
      (fib_->holdDownState_.get()->kLowBackoffTicks) - 1));
}

/**
 * Test case to simulate behavior when del fib route fails.
 */
TEST_F(FibFbossUtilFixture, delFail) {
  // Generate testing data with 20 update and 10 withdraw
  auto data = genRouteTestData(20, 10);

  for (const auto& route : data.routesToDelete) {
    fib_->updateUnicastRoute(
        route, nullptr, nullptr /* NHs */, false, true, nexthopInfoMap_);
  }

  // Set up expectations for co_aliveSince() method
  EXPECT_CALL(*agentServiceHandler_, co_aliveSince())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(kAliveSince_));

  // Set up expectations for co_getSwitchRunState() method
  EXPECT_CALL(*agentServiceHandler_, co_getSwitchRunState())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(fboss::SwitchRunState::CONFIGURED));

  // Do not expect to get call for co_addUnicastRoutes()
  EXPECT_CALL(
      *agentServiceHandler_, co_addUnicastRoutes(testing::_, testing::_))
      .Times(0);

  // Set up expectations for co_deleteUnicastRoutes() method
  EXPECT_CALL(
      *agentServiceHandler_, co_deleteUnicastRoutes(testing::_, testing::_))
      .WillOnce(
          testing::Throw(fboss::thrift::FbossBaseError("Simulated exception")));

  // Do not expect to get call for syncFib()
  EXPECT_CALL(*agentServiceHandler_, co_syncFib(testing::_, testing::_))
      .Times(0);

  // Set up expectations for markProgrammingSuccess() method, one for add and
  // one for delete.
  EXPECT_CALL(
      *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
      markProgrammingSuccess())
      .Times(0);

  // Simulate behavior of fail failure leading to hold down state.
  {
    InSequence dummy;

    // Expect 1 call for del() failing.
    EXPECT_CALL(
        *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
        markProgrammingFail())
        .Times(1);

    // Return 1 FIB programming failure.
    EXPECT_CALL(
        *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
        getRecentFailureCount())
        .Times(testing::AtLeast(1))
        .WillOnce(testing::Return(1));

    // Expect setting up for fib hold down state with 1 failure.
    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        setHoldDownState(1))
        .Times(1);
  }

  // Simulate behavior of clearHoldDownState.
  {
    InSequence dummy;
    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        clearHoldDownState())
        .Times(4)
        .WillRepeatedly(testing::Return(false));

    EXPECT_CALL(
        *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
        clearHoldDownState())
        .Times(1)
        .WillOnce(testing::Return(true));
  }

  folly::coro::blockingWait(fib_->program());

  folly::coro::blockingWait(verifyDelayedFullSyncRequest(
      (fib_->holdDownState_.get()->kLowBackoffTicks) - 1));
}

/**
 * Test case to simulate behavior when keepalive coro detects agent restart.
 * There is no back-off in this case.
 */
TEST_F(FibFbossUtilFixture, keepaliveFail) {
  EXPECT_CALL(*agentServiceHandler_, co_syncFib(testing::_, testing::_))
      .WillOnce(testing::Return(returnSyncFibResp()));

  EXPECT_CALL(*agentServiceHandler_, co_aliveSince())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(kAliveSince_));

  EXPECT_CALL(*agentServiceHandler_, co_getSwitchRunState())
      .Times(AnyNumber())
      .WillRepeatedly(CoReturn(fboss::SwitchRunState::CONFIGURED));

  EXPECT_CALL(
      *static_cast<MockProgrammingHistory*>(fib_->programmingHistory_.get()),
      markProgrammingSuccess())
      .Times(1);

  folly::coro::blockingWait(fib_->program(true));

  // Simulate behavior of clearHoldDownState.
  {
    InSequence dummy;

    EXPECT_CALL(*agentServiceHandler_, co_aliveSince())
        .Times(1)
        .WillOnce(testing::Throw(std::exception()));

    EXPECT_CALL(*agentServiceHandler_, co_aliveSince())
        .Times(AnyNumber())
        .WillRepeatedly(CoReturn(kAliveSince_));

    fib_->evb_->runInEventBaseThreadAndWait([&]() {
      EXPECT_CALL(
          *static_cast<MockHoldDownState*>(fib_->holdDownState_.get()),
          clearHoldDownState())
          .Times(1)
          .WillOnce(testing::Return(true));
    });
  }
  folly::coro::blockingWait(verifyImmediateFullSyncRequest());
}

} // namespace facebook::bgp
