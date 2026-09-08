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

#include <folly/IPAddress.h>
#include <gtest/gtest.h>
#include <random>

#include "neteng/fboss/bgp/cpp/changeTracker/ConsumerBitmap.h"
#include "neteng/fboss/bgp/cpp/common/Utils.h"
#include "neteng/fboss/bgp/cpp/lib/BgpUtil.h"
#include "neteng/fboss/bgp/cpp/rib/Utils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace ::testing;

namespace facebook::bgp {

TEST(UtilsTest, TestlogRouteWithNexthops) {
  // Test case 1: weightedNexthops is nullptr
  const folly::CIDRNetwork prefix("::", 0);
  const std::shared_ptr<folly::F14NodeMap<folly::IPAddress, unsigned int>>
      weightedNexthops = nullptr;
  auto outputMessage = logRouteWithNexthops(prefix, weightedNexthops);
  std::string expectedMessage = fmt::format(
      "{} has been withdrawn", folly::IPAddress::networkToString(prefix));

  EXPECT_EQ(outputMessage, expectedMessage);

  /*
   * Test case 2: weightedNexthops is not nullptr and all next hops have
   * non-zero weights
   */
  const folly::IPAddress nh1("10.0.1.1");
  const folly::IPAddress nh2("7.0.1.3");
  const uint32_t weight1 = 53;
  const uint32_t weight2 = 89;

  const folly::F14NodeMap<folly::IPAddress, unsigned int> weightedNexthopsMap2{
      {nh1, weight1}, {nh2, weight2}};
  const auto weightedNexthops2 =
      std::make_shared<folly::F14NodeMap<folly::IPAddress, unsigned int>>(
          weightedNexthopsMap2);

  outputMessage = logRouteWithNexthops(prefix, weightedNexthops2);

  expectedMessage = fmt::format(
      "{} has {} path(s):\nnext hop 1 of {}: ({}, weight={})\nnext hop 2 of {}: ({}, weight={})",
      folly::IPAddress::networkToString(prefix),
      weightedNexthops2->size(),
      weightedNexthops2->size(),
      nh1.str(),
      weight1,
      weightedNexthops2->size(),
      nh2.str(),
      weight2);

  EXPECT_EQ(outputMessage, expectedMessage);

  /*
   * Test case 3: weightedNexthops is not nullptr and some next hops have zero
   * weights
   */
  const folly::IPAddress nh3("3.98.77.66");
  const uint32_t weight3 = 0;

  const folly::F14NodeMap<folly::IPAddress, unsigned int> weightedNexthopsMap3{
      {nh1, weight1}, {nh2, weight2}, {nh3, weight3}};
  const auto weightedNexthops3 =
      std::make_shared<folly::F14NodeMap<folly::IPAddress, unsigned int>>(
          weightedNexthopsMap3);

  outputMessage = logRouteWithNexthops(prefix, weightedNexthops3);
  // logRouteWithNexthops has the logic to sort the all next hops
  expectedMessage = fmt::format(
      "{} has {} path(s):\nnext hop 1 of {}: ({}, weight={})\nnext hop 2 of {}: ({})\nnext hop 3 of {}: ({}, weight={})",
      folly::IPAddress::networkToString(prefix),
      weightedNexthops3->size(),
      weightedNexthops3->size(),
      nh1.str(),
      weight1,
      weightedNexthops3->size(),
      nh3.str(), // we do not print weight 0,
      weightedNexthops3->size(),
      nh2.str(),
      weight2);

  EXPECT_EQ(outputMessage, expectedMessage);
}

TEST(UtilsTest, GetMinSupportRoutesTest) {
  {
    facebook::bgp::thrift::BgpNetwork bgpNetwork;
    auto result = getMinSupportRoutes(bgpNetwork);
    EXPECT_EQ(result, 0);
  }
  {
    const int32_t kMinSupportRoutes = 10;
    facebook::bgp::thrift::BgpNetwork bgpNetwork;
    bgpNetwork.minimum_supporting_routes() = kMinSupportRoutes;
    auto result = getMinSupportRoutes(bgpNetwork);
    EXPECT_EQ(result, kMinSupportRoutes);
  }
}

TEST(UtilsTest, GetInstallToFibTest) {
  {
    facebook::bgp::thrift::BgpNetwork bgpNetwork;
    auto result = getInstallToFib(bgpNetwork);
    EXPECT_FALSE(result);
  }
  {
    facebook::bgp::thrift::BgpNetwork bgpNetwork;
    bgpNetwork.install_to_fib() = true;
    auto result = getInstallToFib(bgpNetwork);
    EXPECT_TRUE(result);
  }
  {
    facebook::bgp::thrift::BgpNetwork bgpNetwork;
    bgpNetwork.install_to_fib() = false;
    auto result = getInstallToFib(bgpNetwork);
    EXPECT_FALSE(result);
  }
}

TEST(UtilsTest, FormatRibOutAnnouncementLogTest) {
  using namespace nettools::bgplib;
  {
    facebook::bgp::RibOutAnnouncement announcement;
    const int kEntrySize = 10;
    const auto kTestPeerAddr = folly::IPAddress("127.3.0.1");
    const auto kTestLocalAs = facebook::bgp::AsNum(1);
    const auto kTestPeerRouterId = kTestPeerAddr.asV4().toLongHBO();
    const auto kTestV4Nexthop = folly::IPAddress("11.0.0.1");
    BgpUpdate2 update = buildBgpUpdateAttributes(kTestV4Nexthop);
    auto attrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(update)));
    for (int i = 0; i < kEntrySize; ++i) {
      RibOutAnnouncementEntry entry(
          folly::IPAddress::createNetwork(fmt::format("192.168.1.{}", i)),
          kDefaultPathID,
          TinyPeerInfo(
              kTestPeerAddr,
              kTestLocalAs,
              kTestPeerRouterId,
              BgpSessionType::IBGP,
              true),
          attrs);
      announcement.entries.emplace_back(entry);
    }
    auto result = formatRibOutAnnouncementLog(announcement);
    EXPECT_TRUE(
        result.find(fmt::format("{} entries", kEntrySize)) !=
        std::string::npos);
  }
}

TEST(UtilsTest, FormatRibOutWithdrawalLogTest) {
  {
    facebook::bgp::RibOutWithdrawal withdrawal;
    const int kEntrySize = 10;
    for (int i = 0; i < kEntrySize; ++i) {
      withdrawal.entries.emplace_back(
          folly::IPAddress::createNetwork(fmt::format("192.168.1.{}", i)),
          kDefaultPathID);
      withdrawal.addPathEntries.emplace_back(
          folly::IPAddress::createNetwork(fmt::format("192.168.1.{}", i)), i);
    }
    auto result = formatRibOutWithdrawalLog(withdrawal);
    auto addPathResult = formatRibOutWithdrawalLog(withdrawal, true);
    EXPECT_TRUE(
        result.find(fmt::format("{}", kEntrySize)) != std::string::npos);
    EXPECT_TRUE(result.find("add-path") == std::string::npos);
    EXPECT_TRUE(
        addPathResult.find(fmt::format("{}", kEntrySize)) != std::string::npos);
    EXPECT_TRUE(addPathResult.find("add-path") != std::string::npos);
  }
}

TEST(UtilsTest, WriteFileAtomicTest) {
  boost::filesystem::remove("some_file.txt");
  writeFileAtomic("some_file.txt");
  EXPECT_TRUE(boost::filesystem::exists("some_file.txt"));
  boost::filesystem::remove("some_file.txt");
}

/*
 * we test useLargestFreeInterval with various pathID vectors which are
 * initialized in sorted order for readability, but they should be unordered
 * for testing purposes, hence this convenient helper
 */
void shuffle(std::vector<uint32_t>& vec) {
  unsigned seed = 123; // we don't need a different order every run
  std::shuffle(vec.begin(), vec.end(), std::default_random_engine(seed));
}

void checkLargestFreeInterval(
    std::vector<uint32_t>& pathIds,
    std::pair<uint32_t, uint32_t> expectedInterval,
    uint32_t minPathId,
    uint32_t maxPathId,
    int lineNum) {
  shuffle(pathIds);
  folly::F14NodeMap<
      nettools::bgplib::BgpPeerId,
      folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>>>
      routeInfos;
  folly::F14NodeMap<uint32_t, std::shared_ptr<RouteInfo>> paths;
  uint32_t dummyRcvdPathId = 0;
  for (auto id : pathIds) {
    auto info = createRouteInfo(kV4Prefix1, kLocalRoutePeerAddr, kV4Nexthop1);
    info->pathIdToSend = id;
    auto info2 = createRouteInfo(
        kV4Prefix1,
        kLocalRoutePeerAddr,
        kV4Nexthop1); // add a routeInfo with no assigned pathID. These
                      // shouldn't affect useLargestFreePathIdInterval
    paths.insert_or_assign(dummyRcvdPathId++, std::move(info));
    paths.insert_or_assign(dummyRcvdPathId++, std::move(info2));
  }
  routeInfos.insert_or_assign(kPeerId1, paths);
  EXPECT_EQ(
      findLargestFreePathIdInterval(routeInfos, minPathId, maxPathId),
      expectedInterval)
      << lineNum;
}

TEST(UtilsTest, FindLargestFreePathIdIntervalTest) {
  auto minPathId = 0;
  auto maxPathId = 10000; // runtime should be constant wrt number of IDs

  // some basic cases. 10 paths, even or slightly more varied spread
  auto sentPathIds = std::vector<uint32_t>{
      0, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000};
  auto expectedInterval = std::make_pair<uint32_t, uint32_t>(1, 999);
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);
  sentPathIds = std::vector<uint32_t>{
      100, 200, 300, 5400, 5500, 5560, 5570, 5571, 5900, 6000};
  expectedInterval = std::make_pair<uint32_t, uint32_t>(301, 5399);
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);

  // just a few paths. Largest interval is between them
  sentPathIds = std::vector<uint32_t>{100, 9000};
  expectedInterval = std::make_pair<uint32_t, uint32_t>(101, 8999);
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);

  // no sent paths. Largest interval should just be [min, max]
  sentPathIds = std::vector<uint32_t>{};
  expectedInterval = std::make_pair<uint32_t, uint32_t>(0, 10000);
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);

  // a large number of paths
  sentPathIds.clear();
  for (uint32_t i = 0; i <= 10000; i += 100) {
    sentPathIds.push_back(i);
  }
  expectedInterval = std::make_pair<uint32_t, uint32_t>(1, 99);
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);

  // largest interval is before first elem
  sentPathIds = std::vector<uint32_t>{5000, 6000, 7000, 8000, 9000, 10000};
  expectedInterval = std::make_pair<uint32_t, uint32_t>(0, 4999);
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);

  // largest interval after first elem. Just one element
  sentPathIds = std::vector<uint32_t>{100};
  expectedInterval = std::make_pair<uint32_t, uint32_t>(101, 10000);
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);

  // some pathIDs fall outside of [min, max] range
  minPathId = 300;
  maxPathId = 1000;
  sentPathIds = std::vector<uint32_t>{100, 600, 2000, 100000};
  // Effective sentPathIds = {600};
  expectedInterval = {601, 1000};
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);

  // no free IDs. Would just give back the first exhausted interval
  minPathId = 1;
  maxPathId = 3;
  sentPathIds = std::vector<uint32_t>{1, 2, 3};
  expectedInterval = std::make_pair<uint32_t, uint32_t>(1, 0);
  checkLargestFreeInterval(
      sentPathIds, expectedInterval, minPathId, maxPathId, __LINE__);
}

// Test /31 prefix (2 IPs) with limit of 2
TEST(UtilsTest, ListAllIPsInCIDRIPv4SmallNetwork) {
  auto prefix = folly::CIDRNetwork(folly::IPAddress("192.168.1.0"), 31);
  auto result = listAllIPsInCIDR(prefix);

  EXPECT_EQ(result.size(), 2);
  EXPECT_EQ(result[0], folly::IPAddress("192.168.1.0"));
  EXPECT_EQ(result[1], folly::IPAddress("192.168.1.1"));

  prefix = folly::CIDRNetwork(folly::IPAddress("192.168.1.1"), 31);
  result = listAllIPsInCIDR(prefix);

  EXPECT_EQ(result.size(), 2);
  EXPECT_EQ(result[0], folly::IPAddress("192.168.1.0"));
  EXPECT_EQ(result[1], folly::IPAddress("192.168.1.1"));
}

// Test /32 (single IP)
TEST(UtilsTest, ListAllIPsInCIDRIPv4SingleIP) {
  auto prefix = folly::CIDRNetwork(folly::IPAddress("192.168.1.100"), 32);
  auto result = listAllIPsInCIDR(prefix, 10);

  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], folly::IPAddress("192.168.1.100"));
}

// Test /24 prefix (256 IPs) exceeds limit of 10, should return empty
TEST(UtilsTest, ListAllIPsInCIDRIPv4LargeNetworkExceedsLimit) {
  auto prefix = folly::CIDRNetwork(folly::IPAddress("10.1.2.0"), 24);
  auto result = listAllIPsInCIDR(prefix, 10);

  EXPECT_EQ(result.size(), 0);
}

// Test /127 prefix (2 IPs) with default limit of 2
TEST(UtilsTest, ListAllIPsInCIDRIPv6SmallNetwork) {
  auto prefix = folly::CIDRNetwork(folly::IPAddress("2001:db8::"), 127);
  auto result = listAllIPsInCIDR(prefix, 2);

  EXPECT_EQ(result.size(), 2);
  EXPECT_EQ(result[0], folly::IPAddress("2001:db8::"));
  EXPECT_EQ(result[1], folly::IPAddress("2001:db8::1"));

  prefix = folly::CIDRNetwork(folly::IPAddress("2001:db8::1"), 127);
  result = listAllIPsInCIDR(prefix, 2);

  EXPECT_EQ(result.size(), 2);
  EXPECT_EQ(result[0], folly::IPAddress("2001:db8::"));
  EXPECT_EQ(result[1], folly::IPAddress("2001:db8::1"));
}

// Test /128 (single IP)
TEST(UtilsTest, ListAllIPsInCIDRIPv6SingleIP) {
  auto prefix = folly::CIDRNetwork(folly::IPAddress("2001:db8::100"), 128);
  auto result = listAllIPsInCIDR(prefix, 10);

  EXPECT_EQ(result.size(), 1);
  EXPECT_EQ(result[0], folly::IPAddress("2001:db8::100"));
}

// Test /120 prefix (256 IPs) with limit of 300 to get all IPs
TEST(UtilsTest, ListAllIPsInCIDRIPv6BoundaryCase) {
  auto prefix = folly::CIDRNetwork(folly::IPAddress("2001:db8::"), 120);
  auto result = listAllIPsInCIDR(prefix, 300);

  EXPECT_EQ(result.size(), 256);
  EXPECT_EQ(result[0], folly::IPAddress("2001:db8::"));
  EXPECT_EQ(result[1], folly::IPAddress("2001:db8::1"));
  EXPECT_EQ(result[255], folly::IPAddress("2001:db8::ff"));
}

// Test /111 prefix, exceeds limit of 16
TEST(UtilsTest, ListAllIPsInCIDRIPv6NetworkTooLarge) {
  auto prefix = folly::CIDRNetwork(folly::IPAddress("2001:db8::"), 111);
  auto result = listAllIPsInCIDR(prefix, 16);

  EXPECT_EQ(result.size(), 0);
}

// Test /32 prefix, exceeds limit of 16
TEST(UtilsTest, ListAllIPsInCIDRIPv6NetworkHuge) {
  auto prefix = folly::CIDRNetwork(folly::IPAddress("2001:db8::"), 32);
  auto result = listAllIPsInCIDR(prefix, 16);

  EXPECT_EQ(result.size(), 0);
}

TEST(BitmapUtilsTest, OrBitmapsTest) {
  // Test case 1: OR two empty bitmaps
  ConsumerBitmap bitmap1;
  ConsumerBitmap bitmap2;
  auto result = BitmapUtils::orBitmaps(bitmap1, bitmap2);
  EXPECT_EQ(result.size(), 0);

  // Test case 2: OR empty bitmap with non-empty bitmap
  BitmapUtils::setBit(bitmap1, 5);
  BitmapUtils::setBit(bitmap1, 10);
  result = BitmapUtils::orBitmaps(bitmap1, bitmap2);
  EXPECT_EQ(result.size(), 1); // Should have at least one word
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 5));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 10));

  // Test case 3: OR non-empty bitmap with empty bitmap (reverse order)
  result = BitmapUtils::orBitmaps(bitmap2, bitmap1);
  EXPECT_EQ(result.size(), 1);
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 5));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 10));

  // Test case 4: OR two non-overlapping bitmaps
  bitmap2 = ConsumerBitmap();
  BitmapUtils::setBit(bitmap2, 20);
  BitmapUtils::setBit(bitmap2, 30);
  result = BitmapUtils::orBitmaps(bitmap1, bitmap2);
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 5));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 10));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 20));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 30));

  // Test case 5: OR two overlapping bitmaps
  BitmapUtils::setBit(bitmap2, 5); // Same bit as in bitmap1
  result = BitmapUtils::orBitmaps(bitmap1, bitmap2);
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 5));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 10));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 20));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 30));

  // Test case 6: OR bitmaps with different sizes (one with high bit position)
  ConsumerBitmap bitmap3;
  ConsumerBitmap bitmap4;
  BitmapUtils::setBit(bitmap3, 0);
  BitmapUtils::setBit(bitmap3, 1);
  BitmapUtils::setBit(bitmap4, 200); // This will extend the bitmap
  result = BitmapUtils::orBitmaps(bitmap3, bitmap4);
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 0));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 1));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 200));
  EXPECT_FALSE(BitmapUtils::isBitSet(result, 100)); // Random bit not set

  // Test case 7: Verify original bitmaps are not modified
  bitmap1 = ConsumerBitmap();
  bitmap2 = ConsumerBitmap();
  BitmapUtils::setBit(bitmap1, 5);
  BitmapUtils::setBit(bitmap2, 10);
  result = BitmapUtils::orBitmaps(bitmap1, bitmap2);
  // Check result
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 5));
  EXPECT_TRUE(BitmapUtils::isBitSet(result, 10));
  // Check originals unchanged
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap1, 5));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap1, 10));
  EXPECT_FALSE(BitmapUtils::isBitSet(bitmap2, 5));
  EXPECT_TRUE(BitmapUtils::isBitSet(bitmap2, 10));
}

} // namespace facebook::bgp
