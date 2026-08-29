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

#include "BgpStressTest.h"

#include <folly/Benchmark.h>
#include <folly/IPAddress.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/init/Init.h>
#include <folly/io/IOBuf.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/test/SocketPair.h>
#include <folly/logging/xlog.h>
#include <gflags/gflags.h>
#include <gtest/gtest.h>
#include <sigar.h>

#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/lib/BgpException.h"
#include "neteng/fboss/bgp/cpp/lib/BgpMessageParser.h"
#include "neteng/fboss/bgp/cpp/lib/BgpMessageSerializer.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberSocket.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

DEFINE_uint32(
    msg_size_level,
    10,
    "Bgp message size level in scale of 1-10, 1->487b, 10->4051b (default = "
    "10)");
// Average as-path length is 4
// Some data points collected from http://bgp.potaroo.net/index-bgp.html
DEFINE_uint32(as_count, 4, "Number of AS's in path (default = 4)");
DEFINE_uint32(
    community_count,
    0,
    "Number of communities in path (default = 0)");
DEFINE_uint32(
    ext_community_count,
    0,
    "Number of extended communities in path (default = 0)");
DEFINE_uint32(
    cluster_list_count,
    0,
    "Number of cluster list entries in path (default = 0)");
// Average fib prefixes per as-path 7
// NOTE: Careful with default values while testing, if we want to test only
// v4 update or v6 update, we have to set the other prefix count to zero
// I can not set both to zero as default, we can not make a update packet
DEFINE_uint32(
    v6_prefix_count,
    1,
    "Number of IPv6 prefixes in a route (default = 1)");
// Average fib prefixes per as-path 7
DEFINE_uint32(
    v4_prefix_count,
    1,
    "Number of IPv4 prefixes in a route (default = 1)");

using facebook::nettools::bgplib::BgpMessageParser2;

using namespace facebook::nettools::bgplib;
using namespace folly::fibers;

using facebook::network::toBinaryAddress;
using facebook::network::toIPPrefix;
namespace thrift = facebook::network::thrift;
using folly::IPAddress;
using folly::IPAddressV4;
using folly::IPAddressV6;

//
// The fixture provides fiber manager and evb for the tests
//
class StressTestFixture {
 public:
  StressTestFixture() {
    // sanity check on inputs
    CHECK_LE(1, FLAGS_msg_size_level) << "msg_size_level must be in [1, 10]";
    CHECK_GE(10, FLAGS_msg_size_level) << "msg_size_level must be in [1, 10]";

    CHECK_EQ(SIGAR_OK, sigar_open(&sigar_));
    CHECK_EQ(SIGAR_OK, sigar_mem_get(sigar_, &sysMem_));
    pid_ = getpid();
  }

  ~StressTestFixture() {
    sigar_close(sigar_);
  }

  // Builds BgpUpdate2 using msg_size_level
  BgpUpdate2 buildBgpUpdate2() {
    BgpUpdate2 update;
    // max = 1963 bytes Path Attr(except mpAnnounced) length
    update.attrs()->origin() = BgpAttrOrigin::BGP_ORIGIN_EGP;
    BgpAttrAsPathSegment segment;
    // max = 120 * 4 = 480 > 255
    for (int i = 0; i < 12 * FLAGS_msg_size_level; i++) {
      segment.asSequence()->push_back(i);
    }
    update.attrs()->asPath()->push_back(segment);
    update.attrs()->med() = 32;
    update.attrs()->isMedSet() = true;
    update.attrs()->localPref() = 100;
    BgpAttrCommunity community;
    community.asn() = 65530;
    community.value() = 15800;
    // max = 120 * 4 = 480
    for (int i = 0; i < 12 * FLAGS_msg_size_level; i++) {
      update.attrs()->communities()->push_back(community);
    }
    update.attrs()->originatorId() = 0x86070000; // ip: "0.0.7.134"
    // max = 120 * 4 = 480
    for (int i = 0; i < 12 * FLAGS_msg_size_level; i++) {
      update.attrs()->clusterList()->push_back(0x10010000); // ip: "0.0.1.16"
    }
    BgpAttrExtCommunity extCommunity;
    extCommunity.firstWord() = 0x2272a;
    extCommunity.secondWord() = 0x232f;
    // max = 60 * 8 = 480
    for (int i = 0; i < 6 * FLAGS_msg_size_level; i++) {
      update.attrs()->extCommunities()->push_back(extCommunity);
    }
    // 2 + 1 + 1 + 16 + 1 = 21
    update.mpAnnounced()->afi() = BgpUpdateAfi::AFI_IPv6;
    update.mpAnnounced()->safi() = BgpUpdateSafi::SAFI_UNICAST;
    update.mpAnnounced()->nexthop() =
        toBinaryAddress(folly::IPAddress("fd00::1"));
    // max = 17 * 60 * 2 = 2040
    for (int i = 1; i <= 6 * FLAGS_msg_size_level; i++) {
      RiggedIPPrefix prefix1, prefix2;
      prefix1.prefix() = toIPPrefix(
          folly::IPAddress::createNetwork(fmt::format("fd00::{}/122", i)));
      update.mpAnnounced()->prefixes()->push_back(prefix1);
      prefix2.prefix() = toIPPrefix(
          folly::IPAddress::createNetwork(fmt::format("fd00::{}/122", 60 + i)));
      update.mpAnnounced()->prefixes()->push_back(prefix2);
    }
    // max = 19 + 2 + 0 + 2 + (1963 + (4+21 + 2040)) = 4051
    return update;
  }

  BgpCapabilities getBgpCapabilities() {
    BgpCapabilities capabilities;
    capabilities.mpExtV4Unicast() = true;
    capabilities.mpExtV6Unicast() = true;
    capabilities.as4byte() = true;
    return capabilities;
  }

  void getProcStats(folly::UserCounters& counters, bool enableLogging) {
    sigar_proc_cpu_t cpu;
    ASSERT_EQ(SIGAR_OK, sigar_proc_cpu_get(sigar_, pid_, &cpu));

    // process cpu usage
    XLOGF_IF(INFO, enableLogging, "Cpu utilization: {}%", 100 * cpu.percent);

    sigar_proc_mem_t mem;
    ASSERT_EQ(SIGAR_OK, sigar_proc_mem_get(sigar_, pid_, &mem));

    // process memory usage
    double memPercent = ((double)mem.resident / (double)sysMem_.total) * 100;
    XLOGF_IF(INFO, enableLogging, "Memory utilization: {}%", memPercent);

    if (enableLogging) {
      counters["cpu_utilization_percent"] = 100 * cpu.percent;
      counters["memory_utilization_thousandth"] = memPercent * 10;
    }
  }

  std::shared_ptr<BgpAttrCommunitiesC> getCommunities(int index) {
    BgpAttrCommunitiesC communities;

    uint16_t asn = index >> 16;
    uint16_t value = index % (1 << 16);
    for (int i = 0; i < FLAGS_msg_size_level; i++, asn++, value++) {
      BgpAttrCommunityC community{asn, value};
      communities.push_back(community);
    }

    return std::make_shared<BgpAttrCommunitiesC>(std::move(communities));
  }

  // process related info
  sigar_t* sigar_;
  sigar_proc_list_t proclist_;
  sigar_mem_t sysMem_;
  int pid_;
};

class MemoryTestFixture {
 public:
  MemoryTestFixture() {
    // Due to as-path prepending it can go high. Average is 4
    // Max is 57
    CHECK_LE(1, FLAGS_as_count) << "as_count must be in [1, 100]";
    CHECK_GE(100, FLAGS_as_count) << "as_count must be in [1, 100]";

    CHECK_GE(20, FLAGS_community_count) << "community_count must be in [0, 20]";

    CHECK_GE(20, FLAGS_ext_community_count)
        << "ext_community_count must be in [0, 20]";

    CHECK_GE(20, FLAGS_cluster_list_count)
        << "cluster_list_count must be in [0, 20]";

    // NOTE: Rough maximums, depending on other attributes size
    // of the update message may exceed maximum packet size.
    CHECK_GE(200, FLAGS_v6_prefix_count)
        << "v6_prefix_count must be in [0, 200]";

    CHECK_GE(750, FLAGS_v4_prefix_count)
        << "v4_prefix_count must be in [0, 750]";

    DeDuplicatedAsPath::clearDeduplicator();
    DeDuplicatedCommunities::clearDeduplicator();
    DeDuplicatedClusterList::clearDeduplicator();
    DeDuplicatedExtCommunities::clearDeduplicator();
    DeDuplicatedBgpAttributesC::clearDeduplicator();

    CHECK_EQ(SIGAR_OK, sigar_open(&sigar_));
    CHECK_EQ(SIGAR_OK, sigar_mem_get(sigar_, &sysMem_));
    pid_ = getpid();
  }

  ~MemoryTestFixture() {
    sigar_close(sigar_);
  }

  void verifyAndGetRawUpdateLen(BgpUpdate2& update, size_t& msgLen) {
    // sanity check on BgpUpdate2
    auto serMsg = BgpMessageSerializer::serializeBgpUpdate2(update, true);
    msgLen = serMsg->computeChainDataLength();

    // Maximum update size should be 4096 - 19 = 4077
    ASSERT_GE(4077, msgLen)
        << "Exceeding maximum update message size (should be less than 4077)";
  }

  void getProcStats(bool enableLogging) {
    sigar_proc_cpu_t cpu;
    ASSERT_EQ(SIGAR_OK, sigar_proc_cpu_get(sigar_, pid_, &cpu));

    // process cpu usage
    XLOGF_IF(INFO, enableLogging, "Cpu utilization: {}%", 100 * cpu.percent);

    sigar_proc_mem_t mem;
    ASSERT_EQ(SIGAR_OK, sigar_proc_mem_get(sigar_, pid_, &mem));

    // process memory usage
    double memPercent = ((double)mem.resident / (double)sysMem_.total) * 100;
    XLOGF_IF(INFO, enableLogging, "Memory utilization: {}%", memPercent);
  }

  // process related info
  sigar_t* sigar_;
  sigar_proc_list_t proclist_;
  sigar_mem_t sysMem_;
  int pid_;

  // No need to create unique prefixes.
  folly::CIDRNetwork prefixV4_ =
      folly::IPAddress::createNetwork("11.11.11.11/32");
  folly::CIDRNetwork prefixV6_ = folly::IPAddress::createNetwork("fd00::1/128");
  folly::IPAddress nexthopV4_ = folly::IPAddress("10.10.10.10");
  folly::IPAddress nexthopV6_ = folly::IPAddress("fd00::1");
};

// Builds dynamic bgp update with configurable elements
std::shared_ptr<BgpUpdate2> buildDynBgpUpdate2(
    uint32_t as_count,
    uint32_t community_count,
    uint32_t ext_community_count,
    uint32_t cluster_list_count,
    uint32_t v6_prefix_count,
    uint32_t v4_prefix_count) {
  auto update = std::make_shared<BgpUpdate2>();

  update->attrs()->origin() = BgpAttrOrigin::BGP_ORIGIN_EGP;

  BgpAttrAsPathSegment segment;
  for (int i = 0; i < as_count; i++) {
    segment.asSequence()->push_back(i);
  }
  update->attrs()->asPath()->push_back(segment);

  update->attrs()->med() = 32;
  update->attrs()->isMedSet() = true;
  update->attrs()->localPref() = 100;

  if (community_count) {
    BgpAttrCommunity community;
    community.asn() = 65530;
    community.value() = 15800;
    for (int i = 0; i < community_count; i++) {
      update->attrs()->communities()->push_back(community);
    }
  }

  if (ext_community_count) {
    BgpAttrExtCommunity extCommunity;
    extCommunity.firstWord() = 0x2272a;
    extCommunity.secondWord() = 0x232f;
    for (int i = 0; i < ext_community_count; i++) {
      update->attrs()->extCommunities()->push_back(extCommunity);
    }
  }

  update->attrs()->originatorId() = 0x01010101; // ip: "1.1.1.1"

  if (cluster_list_count) {
    for (int i = 0; i < cluster_list_count; i++) {
      update->attrs()->clusterList()->push_back(0x0a0a0a0a); // ip:
                                                             // "10.10.10.10"
    }
  }

  if (v6_prefix_count) {
    update->mpAnnounced()->afi() = BgpUpdateAfi::AFI_IPv6;
    update->mpAnnounced()->safi() = BgpUpdateSafi::SAFI_UNICAST;
    update->mpAnnounced()->nexthop() =
        toBinaryAddress(folly::IPAddress("fd00::1"));
    // No need to create unique prefixes.
    RiggedIPPrefix prefix;
    prefix.prefix() =
        toIPPrefix(folly::IPAddress::createNetwork("fd00::1/128"));
    for (int i = 1; i <= v6_prefix_count; i++) {
      update->mpAnnounced()->prefixes()->push_back(prefix);
    }
  }

  if (v4_prefix_count) {
    update->v4Nexthop() = toBinaryAddress(folly::IPAddress("10.10.10.10"));
    // No need to create unique prefixes.
    for (int i = 1; i <= v4_prefix_count; i++) {
      update->v4Announced()->push_back(
          toIPPrefix(folly::IPAddress::createNetwork("11.11.11.11/32")));
    }
  }

  return update;
}

// Builds dynamic attribute with configurable elements
std::shared_ptr<BgpAttributes> buildDynBgpAttributes(
    uint32_t as_count,
    uint32_t community_count,
    uint32_t ext_community_count,
    uint32_t cluster_list_count) {
  auto attrs = std::make_shared<BgpAttributes>();

  attrs->origin() = BgpAttrOrigin::BGP_ORIGIN_EGP;

  BgpAttrAsPathSegment segment;
  for (int i = 0; i < as_count; i++) {
    segment.asSequence()->push_back(i);
  }
  attrs->asPath()->push_back(segment);

  attrs->med() = 32;
  attrs->isMedSet() = true;
  attrs->localPref() = 100;

  if (community_count) {
    BgpAttrCommunity community;
    community.asn() = 65530;
    community.value() = 15800;
    for (int i = 0; i < community_count; i++) {
      attrs->communities()->push_back(community);
    }
  }

  if (ext_community_count) {
    BgpAttrExtCommunity extCommunity;
    extCommunity.firstWord() = 0x2272a;
    extCommunity.secondWord() = 0x232f;
    for (int i = 0; i < ext_community_count; i++) {
      attrs->extCommunities()->push_back(extCommunity);
    }
  }

  attrs->originatorId() = 0x01010101; // ip: "1.1.1.1"

  if (cluster_list_count) {
    for (int i = 0; i < cluster_list_count; i++) {
      attrs->clusterList()->push_back(0x0a0a0a0a); // ip: "10.10.10.10"
    }
  }

  return attrs;
}

// Builds dynamic attribute with configurable elements
// All C++ structs instead of thrift struct
std::shared_ptr<BgpAttributesC> buildDynBgpAttributesOpt(
    uint32_t as_count,
    uint32_t community_count,
    uint32_t ext_community_count,
    uint32_t cluster_list_count) {
  auto attrs = std::make_shared<BgpAttributesC>();

  attrs->origin = BgpAttrOrigin::BGP_ORIGIN_EGP;

  BgpAttrAsPathC asPath;
  BgpAttrAsPathSegmentC segment;
  segment.asSequence.reserve(as_count);
  for (int i = 0; i < as_count; i++) {
    segment.asSequence.emplace_back(i);
  }
  asPath.emplace_back(std::move(segment));
  attrs->asPath = std::move(asPath);

  attrs->med = 32;
  attrs->localPref = 100;

  if (community_count) {
    BgpAttrCommunityC community;
    BgpAttrCommunitiesC communities;
    community.asn = 65530;
    community.value = 15800;
    for (int i = 0; i < community_count; i++) {
      communities.emplace_back(community);
    }
    attrs->communities = std::move(communities);
  }

  if (ext_community_count) {
    BgpAttrExtCommunitiesC extCommunities;
    for (int i = 0; i < ext_community_count; i++) {
      extCommunities.emplace_back(0x0002272a, 0x0000232f);
    }
    attrs->extCommunities = std::move(extCommunities);
  }

  attrs->originatorId = 0x01010101; // ip: "1.1.1.1"

  if (cluster_list_count) {
    BgpAttrClusterListC clusterList;
    clusterList.reserve(cluster_list_count);
    for (int i = 0; i < cluster_list_count; i++) {
      clusterList.push_back(0x0a0a0a0a); // ip: "10.10.10.10"
    }
    attrs->clusterList = std::move(clusterList);
  }

  return attrs;
}

// NOTE: Run memory tests independently (one at a time)
// If you run together memory used by previous test cases effects the result
// Typical values to use as_count=3-5,
// community_count/ext_community_count=4-10,
// v4_prefix_count/v6_prefix_count=5-10. Depends on peerings etc
static void BM_MemoryUsingBgpUpdate2Test(
    folly::UserCounters& counters,
    uint32_t /* unused */,
    uint32_t numOfObjects) {
  auto suspender = folly::BenchmarkSuspender();
  MemoryTestFixture memoryTestFixture;
  // test for memory consumption
  auto update = buildDynBgpUpdate2(
      FLAGS_as_count,
      FLAGS_community_count,
      FLAGS_ext_community_count,
      FLAGS_cluster_list_count,
      FLAGS_v6_prefix_count,
      FLAGS_v4_prefix_count);

  // We need to get average over large enough objects to avoid measurement
  // error associated with other memory allocations, freeing of the test infra
  // and amoritize vectors etc
  size_t msgLen;

  memoryTestFixture.verifyAndGetRawUpdateLen(*update, msgLen);

  suspender.dismiss();
  // process memory usage
  sigar_proc_mem_t mem;
  ASSERT_EQ(
      SIGAR_OK,
      sigar_proc_mem_get(
          memoryTestFixture.sigar_, memoryTestFixture.pid_, &mem));
  auto start = mem.resident;

  std::vector<BgpEntry> prefixTree; // not a tree. :-) just simulating
  // Reserve exact space so that vector doesn't add additional space
  prefixTree.reserve(
      numOfObjects * (FLAGS_v6_prefix_count + FLAGS_v4_prefix_count));

  for (int i = 0; i < numOfObjects; i++) {
    auto update2 = buildDynBgpUpdate2(
        FLAGS_as_count,
        FLAGS_community_count,
        FLAGS_ext_community_count,
        FLAGS_cluster_list_count,
        FLAGS_v6_prefix_count,
        FLAGS_v4_prefix_count);
    // NOTE: If we store BgpUpdate2 then prefix, nexthop will be present
    // in both BgpUpdate2 and in the prefixTree

    // Number of prefixes sharing same BgpUpdate2
    for (int j = 0; j < FLAGS_v6_prefix_count; j++) {
      BgpEntry prefixEntry;
      prefixEntry.update = update2;
      prefixEntry.nexthop = memoryTestFixture.nexthopV6_;
      prefixEntry.prefix = memoryTestFixture.prefixV6_;
      prefixTree.emplace_back(prefixEntry);
    }

    for (int j = 0; j < FLAGS_v4_prefix_count; j++) {
      BgpEntry prefixEntry;
      prefixEntry.update = update2;
      prefixEntry.nexthop = memoryTestFixture.nexthopV4_;
      prefixEntry.prefix = memoryTestFixture.prefixV4_;
      prefixTree.emplace_back(prefixEntry);
    }
  }

  // No sleep needed to get proc memory
  ASSERT_EQ(
      SIGAR_OK,
      sigar_proc_mem_get(
          memoryTestFixture.sigar_, memoryTestFixture.pid_, &mem));
  suspender.rehire();

  auto end = mem.resident;

  XLOGF(INFO, "Raw Bgp Update message size {}", msgLen);
  XLOGF(INFO, "Start: {} Bytes End: {} Bytes", start, end);
  XLOGF(
      INFO,
      "Memory consumed by {} updates is {} KB",
      numOfObjects,
      (end - start) / 1024);

  // Average value is more meaningfull than measuring for 1 object
  XLOGF(
      INFO,
      "Average Memory consumed for each Bgp Update (with {} v4 {} v6 prefixes) is {} Bytes",
      FLAGS_v4_prefix_count,
      FLAGS_v6_prefix_count,
      (end - start) / numOfObjects);
  counters["memory_all_updates_bytes"] = end - start;
  counters["memory_per_update_bytes"] = (end - start) / numOfObjects;
}

// Run this test independently (one at a time)

// This test uses BgpAttributes as is from thrift definition but avoids
// overhead related to BgpUpdate2
static void BM_MemoryUsingThriftAttrTest(
    folly::UserCounters& counters,
    uint32_t /* unused */,
    uint32_t numOfObjects) {
  auto suspender = folly::BenchmarkSuspender();
  MemoryTestFixture memoryTestFixture;
  // test for memory consumption
  auto update = buildDynBgpUpdate2(
      FLAGS_as_count,
      FLAGS_community_count,
      FLAGS_ext_community_count,
      FLAGS_cluster_list_count,
      FLAGS_v6_prefix_count,
      FLAGS_v4_prefix_count);

  // We need to get average over large enough objects to avoid measurement
  // error associated with other memory allocations, freeing of the test infra
  // and amoritize vectors etc
  size_t msgLen;

  memoryTestFixture.verifyAndGetRawUpdateLen(*update, msgLen);

  // process memory usage
  suspender.dismiss();
  sigar_proc_mem_t mem;
  ASSERT_EQ(
      SIGAR_OK,
      sigar_proc_mem_get(
          memoryTestFixture.sigar_, memoryTestFixture.pid_, &mem));
  auto start = mem.resident;

  std::vector<BgpEntryOpt> prefixTree; // not a tree. :-) just simulating

  // Reserve exact space so that vector doesn't add additional space
  prefixTree.reserve(
      numOfObjects * (FLAGS_v6_prefix_count + FLAGS_v4_prefix_count));

  for (int i = 0; i < numOfObjects; i++) {
    auto attrs = buildDynBgpAttributes(
        FLAGS_as_count,
        FLAGS_community_count,
        FLAGS_ext_community_count,
        FLAGS_cluster_list_count);

    // Number of prefixes sharing same attributes
    for (int j = 0; j < FLAGS_v6_prefix_count; j++) {
      BgpEntryOpt prefixEntry;
      prefixEntry.attrs = attrs;
      prefixEntry.nexthop = memoryTestFixture.nexthopV6_;
      prefixEntry.prefix = memoryTestFixture.prefixV6_;
      prefixTree.emplace_back(prefixEntry);
    }

    for (int j = 0; j < FLAGS_v4_prefix_count; j++) {
      BgpEntryOpt prefixEntry;
      prefixEntry.attrs = attrs;
      prefixEntry.nexthop = memoryTestFixture.nexthopV4_;
      prefixEntry.prefix = memoryTestFixture.prefixV4_;
      prefixTree.emplace_back(prefixEntry);
    }
  }

  // No sleep needed to get proc memory
  ASSERT_EQ(
      SIGAR_OK,
      sigar_proc_mem_get(
          memoryTestFixture.sigar_, memoryTestFixture.pid_, &mem));

  auto end = mem.resident;
  suspender.rehire();

  XLOGF(INFO, "Raw Bgp Update message size {}", msgLen);
  XLOGF(INFO, "Start: {} Bytes End: {} Bytes", start, end);
  XLOGF(
      INFO,
      "Memory consumed by {} updates is {} KB",
      numOfObjects,
      (end - start) / 1024);

  // Average value is more meaningfull than measuring for 1 object
  XLOGF(
      INFO,
      "Average Memory consumed for each Bgp Update (with {} v4 {} v6 prefixes) is {} Bytes",
      FLAGS_v4_prefix_count,
      FLAGS_v6_prefix_count,
      (end - start) / numOfObjects);
  counters["memory_all_updates_bytes"] = end - start;
  counters["memory_per_update_bytes"] = (end - start) / numOfObjects;
}

// Run memory tests independently (one at a time)
// If you run together memory used by previous test cases effects the result

// This test uses all C++ structures instead of thrift
static void BM_MemoryUsingCppAttrTest(
    folly::UserCounters& counters,
    uint32_t /* unused */,
    uint32_t numOfObjects) {
  auto suspender = folly::BenchmarkSuspender();
  MemoryTestFixture memoryTestFixture;
  // test for memory consumption
  auto update = buildDynBgpUpdate2(
      FLAGS_as_count,
      FLAGS_community_count,
      FLAGS_ext_community_count,
      FLAGS_cluster_list_count,
      FLAGS_v6_prefix_count,
      FLAGS_v4_prefix_count);

  // We need to get average over large enough objects to avoid measurement
  // error associated with other memory allocations, freeing of the test infra
  // and amoritize vectors etc
  size_t msgLen;

  memoryTestFixture.verifyAndGetRawUpdateLen(*update, msgLen);

  suspender.dismiss();
  // process memory usage
  sigar_proc_mem_t mem;
  ASSERT_EQ(
      SIGAR_OK,
      sigar_proc_mem_get(
          memoryTestFixture.sigar_, memoryTestFixture.pid_, &mem));
  auto start = mem.resident;

  std::vector<BgpEntryOpt2> prefixTree; // not a tree. :-) just simulating

  // Reserve exact space so that vector doesn't add additional space
  prefixTree.reserve(
      numOfObjects * (FLAGS_v6_prefix_count + FLAGS_v4_prefix_count));

  for (int i = 0; i < numOfObjects; i++) {
    auto attrs = buildDynBgpAttributesOpt(
        FLAGS_as_count,
        FLAGS_community_count,
        FLAGS_ext_community_count,
        FLAGS_cluster_list_count);

    // Number of prefixes sharing same attributes
    for (int j = 0; j < FLAGS_v6_prefix_count; j++) {
      BgpEntryOpt2 prefixEntry;
      prefixEntry.attrs = attrs;
      prefixEntry.nexthop = memoryTestFixture.nexthopV6_;
      prefixEntry.prefix = memoryTestFixture.prefixV6_;
      prefixTree.emplace_back(prefixEntry);
    }

    for (int j = 0; j < FLAGS_v4_prefix_count; j++) {
      BgpEntryOpt2 prefixEntry;
      prefixEntry.attrs = attrs;
      prefixEntry.nexthop = memoryTestFixture.nexthopV4_;
      prefixEntry.prefix = memoryTestFixture.prefixV4_;
      prefixTree.emplace_back(prefixEntry);
    }
  }

  // No sleep needed to get proc memory
  ASSERT_EQ(
      SIGAR_OK,
      sigar_proc_mem_get(
          memoryTestFixture.sigar_, memoryTestFixture.pid_, &mem));

  auto end = mem.resident;
  suspender.rehire();

  XLOGF(INFO, "Raw Bgp Update message size {}", msgLen);
  XLOGF(INFO, "Start: {} Bytes End: {} Bytes", start, end);
  XLOGF(
      INFO,
      "Memory consumed by {} updates is {} KB",
      numOfObjects,
      (end - start) / 1024);

  // Average value is more meaningfull than measuring for 1 object
  XLOGF(
      INFO,
      "Average Memory consumed for each Bgp Update (with {} v4 {} v6 prefixes) is {} Bytes",
      FLAGS_v4_prefix_count,
      FLAGS_v6_prefix_count,
      (end - start) / numOfObjects);
  counters["memory_all_updates_bytes"] = end - start;
  counters["memory_per_update_bytes"] = (end - start) / numOfObjects;
}

// This is to display various structures sizes for information
// Doesn't test anything but displays for quick reference and possible
// optimizations
static void BM_MemorySizeTest() {
  XLOG(INFO, "Thrift structs sizes vs cpp");
  XLOG(INFO, "------------------------------");
  XLOGF(INFO, "BgpNlri               : {} na", sizeof(BgpNlri));
  XLOGF(
      INFO,
      "BgpAttrAggregator     : {} {}",
      sizeof(BgpAttrAggregator),
      sizeof(BgpAttrAggregatorC));
  XLOGF(
      INFO,
      "BgpAttrCommunity      : {} {}",
      sizeof(BgpAttrCommunity),
      sizeof(BgpAttrCommunityC));
  XLOGF(
      INFO,
      "BgpAttrAsPathSegment  : {} {}",
      sizeof(BgpAttrAsPathSegment),
      sizeof(BgpAttrAsPathSegmentC));
  XLOGF(
      INFO,
      "BgpAttrExtCommunity   : {} {}",
      sizeof(BgpAttrExtCommunity),
      sizeof(BgpAttrCommunityC));
  XLOGF(INFO, "RiggedIPPrefix        : {} na", sizeof(RiggedIPPrefix));

  XLOG(INFO, "Thrift structs");
  XLOG(INFO, "--------------");
  XLOGF(INFO, "thrift::IPPrefix      : {}", sizeof(thrift::IPPrefix));
  XLOGF(INFO, "thrift::BinaryAddress : {}", sizeof(thrift::BinaryAddress));
  XLOGF(INFO, "Thrift BgpAttributes  : {}", sizeof(BgpAttributes));
  XLOGF(INFO, "Thrift BgpUpdate2     : {}", sizeof(BgpUpdate2));

  XLOG(INFO, "Cpp structs");
  XLOG(INFO, "-----------");
  XLOGF(INFO, "folly::CIDRNetwork     : {}", sizeof(folly::CIDRNetwork));
  XLOGF(INFO, "folly::IPAddress       : {}", sizeof(folly::IPAddress));
  XLOGF(INFO, "folly::IPAddressV4     : {}", sizeof(folly::IPAddressV4));
  XLOGF(INFO, "folly::IPAddressV6     : {}", sizeof(folly::IPAddressV6));
  XLOGF(INFO, "Cpp BgpAttributesC     : {}", sizeof(BgpAttributesC));
  XLOGF(INFO, "Cpp BgpEntryOpt        : {}", sizeof(BgpEntryOpt));
}

static void BM_FullCommunicationTest(
    folly::UserCounters& counters,
    uint32_t /* unused */,
    uint32_t total_msg_cnt) {
  auto suspender = folly::BenchmarkSuspender();
  StressTestFixture stressTestFixture;
  // set up server and client event loop
  auto serverEvb = std::make_unique<folly::EventBase>();

  FiberManager::Options options;
  // this is needed due to nested recursion with large stack
  // due to installed exception handlers...
  options.stackSize = 256 * 1024;
  auto serverManager = std::make_unique<FiberManager>(
      std::make_unique<EventBaseLoopController>(), options);

  static_cast<EventBaseLoopController&>(serverManager->loopController())
      .attachEventBase(*serverEvb);

  auto clientEvb = std::make_unique<folly::EventBase>();
  auto clientManager = std::make_unique<FiberManager>(
      std::make_unique<EventBaseLoopController>(), options);

  static_cast<EventBaseLoopController&>(clientManager->loopController())
      .attachEventBase(*clientEvb);

  // build message based on user input
  auto update = stressTestFixture.buildBgpUpdate2();
  // sanity check on BgpUpdate2
  auto msgLen = size_t{};
  {
    auto serMsg = BgpMessageSerializer::serializeBgpUpdate2(update, true);
    ASSERT_EQ(1, serMsg->countChainElements());
    msgLen = serMsg->length();
  }
  // get capability for message parsing
  auto capabilities = stressTestFixture.getBgpCapabilities();

  // grab the socket pair
  folly::SocketPair sp;
  auto serverFd = sp.extractFD0();
  auto clientFd = sp.extractFD1();

  auto serverAs = folly::to_shared_ptr(
      folly::AsyncSocket::newSocket(
          serverEvb.get(), folly::NetworkSocket::fromFd(serverFd)));
  auto clientAs = folly::to_shared_ptr(
      folly::AsyncSocket::newSocket(
          clientEvb.get(), folly::NetworkSocket::fromFd(clientFd)));

  int serverDuration = 0;
  suspender.dismiss();
  // server sends serialized messages
  serverManager->addTask([serverAs = std::move(serverAs),
                          total_msg_cnt,
                          update,
                          &serverDuration]() mutable {
    FiberSocket serverFs(serverAs);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < total_msg_cnt; i++) {
      auto serMsg = BgpMessageSerializer::serializeBgpUpdate2(update, true);
      auto result = serverFs.write(
          folly::IOBuf::wrapBuffer(serMsg->data(), serMsg->length()));
      ASSERT_TRUE(result.hasValue());
    }
    auto finish = std::chrono::steady_clock::now();
    serverDuration =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
            .count();
    serverFs.close();
    XLOG(DBG4, "Stopping server");
  });

  int clientDuration = 0;
  // client reads and deserializes messages
  clientManager->addTask([clientAs = std::move(clientAs),
                          total_msg_cnt,
                          update,
                          capabilities,
                          msgLen,
                          &clientDuration]() mutable {
    FiberSocket clientFs(clientAs);
    int cnt = 0;
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < total_msg_cnt; i++) {
      // result is folly expected
      auto result = clientFs.read(msgLen).then(
          [capabilities, update, &cnt](auto bufRead) {
            auto parsedUpdate = std::get<std::shared_ptr<const BgpUpdate2>>(
                BgpMessageParser2::parseBgpUpdateRaw(*bufRead, capabilities));
            EXPECT_EQ(update, *parsedUpdate);
            cnt++;
            if (cnt % 1000 == 0) {
              XLOGF(DBG4, "Client: Received {} messages.", cnt);
            }
          });

      if (result.hasError()) {
        auto error = result.error();
        FiberSocketErrorVisitor errProcessor;
        auto errorStr = std::visit(errProcessor, error);
        XLOGF(ERR, "{}", errorStr);
      }

      ASSERT_TRUE(result.hasValue());
    }
    auto finish = std::chrono::steady_clock::now();
    clientDuration =
        std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
            .count();
    clientFs.close();
    XLOG(DBG4, "Stopping client");
  });

  std::thread serverThread(
      [serverEvb = std::move(serverEvb)] { serverEvb->loop(); });
  std::thread clientThread(
      [clientEvb = std::move(clientEvb)] { clientEvb->loop(); });

  // Get first snapshot of cpu and memory utilization
  stressTestFixture.getProcStats(counters, false /* enableLogging */);

  serverThread.join();
  clientThread.join();
  suspender.rehire();

  XLOGF(
      INFO,
      "Server: Finish Serializating and sending {} BgpUpdate2(size={}) messages in {}ms",
      total_msg_cnt,
      msgLen,
      serverDuration);
  XLOGF(
      INFO,
      "Client: Finish reading and deserialization of {} BgpUpdate2(size={}) messages in {}ms",
      total_msg_cnt,
      msgLen,
      clientDuration);
  counters["server_duration"] = serverDuration;
  counters["client_duration"] = clientDuration;
  // Log cpu and memory utilization
  stressTestFixture.getProcStats(counters, true /* enableLogging */);
}

static void BM_SerializationTest(
    folly::UserCounters& counters,
    uint32_t /* unused */,
    uint32_t total_msg_cnt) {
  auto suspender = folly::BenchmarkSuspender();
  // test for pure serialization time
  StressTestFixture stressTestFixture;
  auto update = stressTestFixture.buildBgpUpdate2();
  auto capabilities = stressTestFixture.getBgpCapabilities();
  // sanity check on BgpUpdate2
  auto serMsg = BgpMessageSerializer::serializeBgpUpdate2(update, true);
  ASSERT_EQ(1, serMsg->countChainElements());
  auto msgLen = serMsg->length();

  // Get first snapshot of cpu and memory utilization
  stressTestFixture.getProcStats(counters, false /* enableLogging */);

  suspender.dismiss();
  auto start = std::chrono::steady_clock::now();
  std::vector<std::unique_ptr<folly::IOBuf>> serMsgs;
  for (int i = 0; i < total_msg_cnt; i++) {
    auto iobuf = BgpMessageSerializer::serializeBgpUpdate2(update, true);
    serMsgs.emplace_back(iobuf->cloneCoalesced());
  }
  auto finish = std::chrono::steady_clock::now();
  suspender.rehire();
  auto serializationDuration =
      std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
          .count();
  XLOGF(
      INFO,
      "Serialized {} BgpUpdate2(size={}) messages in {}ms",
      total_msg_cnt,
      msgLen,
      serializationDuration);
  counters["serialization_duration"] = serializationDuration;
  // Log cpu and memory utilization for seriliazation
  stressTestFixture.getProcStats(counters, true /* enableLogging */);

  suspender.dismiss();
  start = std::chrono::steady_clock::now();
  std::vector<BgpUpdate2> parseUpdates;
  for (int i = 0; i < total_msg_cnt; i++) {
    auto msgBuf =
        folly::IOBuf::wrapBuffer(serMsgs[i]->data(), serMsgs[i]->length());
    auto parsedUpdate = std::get<std::shared_ptr<const BgpUpdate2>>(
        BgpMessageParser2::parseBgpUpdateRaw(*msgBuf, capabilities));
    parseUpdates.push_back(*parsedUpdate);
    if (i % 1000 == 0) {
      XLOGF(DBG4, "Deserialized {} messages.", i);
    }
  }
  finish = std::chrono::steady_clock::now();
  suspender.rehire();
  auto deserializationDuration =
      std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
          .count();
  XLOGF(
      INFO,
      "Deserialized {} BgpUpdate2(size={}) messages in {}ms",
      total_msg_cnt,
      msgLen,
      deserializationDuration);
  counters["deserialization_duration"] = deserializationDuration;
  // Log cpu and memory utilization for deserilization
  stressTestFixture.getProcStats(counters, true /* enableLogging */);
}

static void BM_BuildBgpUpdate2Test(
    folly::UserCounters& counters,
    uint32_t /* unused */,
    uint32_t total_msg_cnt) {
  // TEST_F(StressTestFixture, stressTestFixture.buildBgpUpdat2eTest) {
  auto suspender = folly::BenchmarkSuspender();
  StressTestFixture stressTestFixture;
  // test for pure serialization time
  auto update = stressTestFixture.buildBgpUpdate2();
  auto capabilities = stressTestFixture.getBgpCapabilities();
  // sanity check on BgpUpdate2
  auto serMsg = BgpMessageSerializer::serializeBgpUpdate2(update, true);
  ASSERT_EQ(1, serMsg->countChainElements());
  auto msgLen = serMsg->length();

  // Get first snapshot of cpu and memory utilization
  stressTestFixture.getProcStats(counters, false /* enableLogging */);
  suspender.dismiss(); // Start measuring benchmark time

  auto start = std::chrono::steady_clock::now();
  std::vector<BgpUpdate2> updates;
  for (int i = 0; i < total_msg_cnt; i++) {
    auto update2 = stressTestFixture.buildBgpUpdate2();
    updates.emplace_back(update2);
  }
  auto finish = std::chrono::steady_clock::now();
  suspender.rehire();
  XLOGF(
      INFO,
      "Built {} BgpUpdate2(size={}) messages in {}ms",
      total_msg_cnt,
      msgLen,
      std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
          .count());
  // Log cpu and memory utilization for seriliazation
  stressTestFixture.getProcStats(counters, true /* enableLogging */);
}

// Measure number of fiber context switches per second
// Stress test with large number of fibers and how many context switches we can
// achieve (Snake test)
// NOTE: Test this in opt mode to see accurate results.
// From strobelight Over 70% of the cost is in baton etc fiber code instead of
// context switch itself, if it's real required context switch post/wait cost is
// part of the running itself, but for fairness if we decide to yield much more
// frequently, the additional cost of baton post, wait etc
// should be accounted too as part of fairness implementation.

// Based on tests number of active fibers effect how many context swiches we can
// achieve, (could be cache performance or fiber scheduling costs etc)
// With 2 fibers we can achieve 9.5 Million switches
// With 100 fibers we can achieve 7.7 Million switches
// With 1000 fibers we can achieve 6.6 Million switches
// With 10000 fibers we can achieve 2.4 Million switches
static void BM_FiberContextSwitch(
    folly::UserCounters& counters,
    uint32_t /* unused */,
    const uint32_t numberOfFibers,
    const uint32_t numberOfFiberSwitches) {
  auto suspender = folly::BenchmarkSuspender();
  auto evb = std::make_unique<folly::EventBase>();

  FiberManager::Options options;
  options.stackSize = 1024 * 100;
  auto fm = std::make_unique<FiberManager>(
      std::make_unique<EventBaseLoopController>(), options);

  static_cast<EventBaseLoopController&>(fm->loopController())
      .attachEventBase(*evb);

  // Create fibers and using queues cause snake test among them
  std::vector<RWQueue<uint8_t>> syncQueues;
  syncQueues.resize(numberOfFibers);

  auto funcQueue =
      [&syncQueues, &numberOfFibers, &numberOfFiberSwitches](int taskId) {
        // auto syncQueues = *syncQueuesPtr;
        int i = numberOfFiberSwitches / numberOfFibers;
        auto reader = syncQueues[taskId].getReader();
        auto writer = syncQueues[(taskId + 1) % numberOfFibers].getWriter();
        if (taskId == 0) {
          // Start from snake head
          syncQueues[0].getWriter().put(1);
        }
        while (i--) {
          reader.get();
          writer.put(1);
        }
      };

  for (int id = 0; id < numberOfFibers; id++) {
    fm->addTask([&, taskId = id]() mutable -> void { funcQueue(taskId); });
  }

  suspender.dismiss();
  auto start = std::chrono::steady_clock::now();

  std::thread queueThread([&] { evb->loop(); });
  queueThread.join();

  auto finish = std::chrono::steady_clock::now();
  suspender.rehire();

  auto queueBasedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
          .count();
  XLOGF(
      INFO,
      "Time taken for {} queue based context switches with {} fibers is {}ms",
      numberOfFiberSwitches,
      numberOfFibers,
      queueBasedTime);
  counters["queue_based_time"] = queueBasedTime;
  // Create fibers and using batons cause snake test among them
  std::vector<folly::fibers::Baton> syncBatons(numberOfFibers);

  auto funcBaton =
      [&syncBatons, &numberOfFibers, &numberOfFiberSwitches](int taskId) {
        int i = numberOfFiberSwitches / numberOfFibers;
        if (taskId == 0) {
          // Start from snake head
          syncBatons[0].post();
        }
        while (i--) {
          syncBatons[taskId].wait();
          syncBatons[taskId].reset();
          syncBatons[(taskId + 1) % numberOfFibers].post();
        }
      };

  for (int id = 0; id < numberOfFibers; id++) {
    fm->addTask([&, taskId = id]() mutable -> void { funcBaton(taskId); });
  }

  suspender.dismiss();
  start = std::chrono::steady_clock::now();

  std::thread batonThread([&] { evb->loop(); });
  batonThread.join();

  finish = std::chrono::steady_clock::now();
  suspender.rehire();

  auto batonBasedTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
          .count();
  XLOGF(
      INFO,
      "Time taken for {} baton based context switches with {} fibers is {}ms",
      numberOfFiberSwitches,
      numberOfFibers,
      batonBasedTime);
  counters["baton_based_time"] = batonBasedTime;
}

static void BM_DeDuplicatorStressTest(
    folly::UserCounters& counters,
    uint32_t /* unused */,
    uint32_t numOfExistingObjects,
    uint32_t numOfObjects) {
  auto suspender = folly::BenchmarkSuspender();
  // test deduplicator processing time
  StressTestFixture stressTestFixture;

  DeDuplicator<BgpAttrCommunitiesC> deduplicator;

  {
    for (int i = 0; i < numOfExistingObjects; i++) {
      deduplicator.get(stressTestFixture.getCommunities(i));
    }
  }

  std::vector<std::shared_ptr<BgpAttrCommunitiesC>> testEntries;
  testEntries.reserve(numOfObjects);
  for (int i = 0; i < numOfObjects; i++) {
    testEntries.emplace_back(stressTestFixture.getCommunities(i));
  }

  suspender.dismiss();
  auto start = std::chrono::steady_clock::now();

  for (const auto& entry : testEntries) {
    deduplicator.get(entry);
  }

  auto finish = std::chrono::steady_clock::now();
  suspender.rehire();

  auto deduplicateTime =
      std::chrono::duration_cast<std::chrono::milliseconds>(finish - start)
          .count();

  XLOGF(
      INFO,
      "Time taken to deduplicate {} objects is {}ms",
      numOfObjects,
      deduplicateTime);
  counters["deduplicate_time"] = deduplicateTime;
}

//
// Memory
// Add memory to user counters
//

// parameter: numOfObjects
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingBgpUpdate2Test, 100);
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingBgpUpdate2Test, 1000);
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingBgpUpdate2Test, 10000);
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingThriftAttrTest, 100);
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingThriftAttrTest, 1000);
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingThriftAttrTest, 10000);
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingCppAttrTest, 100);
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingCppAttrTest, 1000);
BENCHMARK_COUNTERS_PARAM(BM_MemoryUsingCppAttrTest, 10000);
BENCHMARK(BM_MemorySizeTest);

//
// Time
//

// parameter: total_msg_cnt
BENCHMARK_COUNTERS_PARAM(BM_FullCommunicationTest, 200);
BENCHMARK_COUNTERS_PARAM(BM_FullCommunicationTest, 2000);
BENCHMARK_COUNTERS_PARAM(BM_FullCommunicationTest, 20000);

// parameters: numberOfFibers, numberOfFiberSwitches
// With 2 fibers we can achieve 9.5 Million switches
// With 100 fibers we can achieve 7.7 Million switches
// With 1000 fibers we can achieve 6.6 Million switches
// With 10000 fibers we can achieve 2.4 Million switches
BENCHMARK_COUNTERS_NAMED_PARAM(BM_FiberContextSwitch, 2_9m, 2, 9500000);
BENCHMARK_COUNTERS_NAMED_PARAM(BM_FiberContextSwitch, 100_7m, 100, 7700000);
BENCHMARK_COUNTERS_NAMED_PARAM(BM_FiberContextSwitch, 1000_6m, 1000, 6600000);
BENCHMARK_COUNTERS_NAMED_PARAM(BM_FiberContextSwitch, 10000_2m, 10000, 2400000);
BENCHMARK_COUNTERS_PARAM(BM_SerializationTest, 200);
BENCHMARK_COUNTERS_PARAM(BM_SerializationTest, 2000);
BENCHMARK_COUNTERS_PARAM(BM_SerializationTest, 20000);
BENCHMARK_COUNTERS_PARAM(BM_BuildBgpUpdate2Test, 200);
BENCHMARK_COUNTERS_PARAM(BM_BuildBgpUpdate2Test, 2000);
BENCHMARK_COUNTERS_PARAM(BM_BuildBgpUpdate2Test, 20000);
// clean insertion
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_DeDuplicatorStressTest,
    clean_10000,
    0,
    10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_DeDuplicatorStressTest,
    clean_100000,
    0,
    100000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_DeDuplicatorStressTest,
    clean_1000000,
    0,
    1000000);
// insertion with pre-existing objects
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_DeDuplicatorStressTest,
    preexisting_10000,
    10000,
    10000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_DeDuplicatorStressTest,
    preexisting_100000,
    100000,
    100000);
BENCHMARK_COUNTERS_NAMED_PARAM(
    BM_DeDuplicatorStressTest,
    preexisting_1000000,
    1000000,
    1000000);

int main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
