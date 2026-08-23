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

#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/rib/CanonicalRibBuilder.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

using nettools::bgplib::DeDuplicatedAsPath;
using nettools::bgplib::DeDuplicatedBgpAttributesC;
using nettools::bgplib::DeDuplicatedBgpPath;
using nettools::bgplib::DeDuplicatedClusterList;
using nettools::bgplib::DeDuplicatedCommunities;
using nettools::bgplib::DeDuplicatedExtCommunities;

namespace {
constexpr int64_t kRibVersion = 7;

folly::CIDRNetwork prefix(const std::string& cidr) {
  return folly::IPAddress::createNetwork(cidr);
}
} // namespace

class CanonicalRibBuilderTest : public ::testing::Test {
 public:
  void SetUp() override {
    clearAllDeduplicators();
  }
  void TearDown() override {
    clearAllDeduplicators();
  }

  static void clearAllDeduplicators() {
    DeDuplicatedBgpPath::clearDeduplicator();
    DeDuplicatedBgpAttributesC::clearDeduplicator();
    DeDuplicatedAsPath::clearDeduplicator();
    DeDuplicatedCommunities::clearDeduplicator();
    DeDuplicatedExtCommunities::clearDeduplicator();
    DeDuplicatedClusterList::clearDeduplicator();
  }

  /*
   * Two calls with identical (asCount, commCount, nexthop) return the same
   * deduplicated shared_ptr; a differing asCount yields a distinct path that
   * still shares the community list when commCount matches.
   */
  std::shared_ptr<const BgpPath> makePath(
      uint32_t asCount,
      uint32_t commCount,
      const folly::IPAddress& nexthop) {
    auto path = std::make_shared<BgpPath>(
        *buildBgpPathFields(asCount, commCount, 0, 0, 0, nexthop));
    return DeDuplicatedBgpPath(path).getSharedPtr();
  }

  CanonicalPathInput input(
      std::shared_ptr<const BgpPath> path,
      const folly::IPAddress& peerAddr,
      int64_t routerId,
      bool isBest = false) {
    CanonicalPathInput in;
    in.path = std::move(path);
    in.group = isBest ? kBestPathGroup : kDefaultPathGroup;
    in.isBestPath = isBest;
    in.peerAddr = peerAddr;
    in.peerRouterId = routerId;
    return in;
  }

  std::shared_ptr<const BgpPath> makePathWithTopoAndWeight(
      uint32_t asCount,
      uint32_t commCount,
      const folly::IPAddress& nexthop,
      std::unordered_map<std::string, int64_t> topoInfo,
      uint16_t weight,
      std::optional<folly::IPAddress> backupAddr = std::nullopt) {
    auto path = std::make_shared<BgpPath>(
        *buildBgpPathFields(asCount, commCount, 0, 0, 0, nexthop));
    path->setTopologyInfo(topoInfo);
    path->setWeight(weight);
    path->setBackupAddr(std::move(backupAddr));
    return DeDuplicatedBgpPath(path).getSharedPtr();
  }
};

/*
 * The same (attrs, next_hop) appearing under two prefixes is interned once and
 * shared by index.
 */
TEST_F(CanonicalRibBuilderTest, WholePathDedup) {
  auto nh = folly::IPAddress("10.0.0.1");
  auto peer = folly::IPAddress("10.0.0.2");

  CanonicalRibBuilder builder;
  builder.addEntry(
      prefix("1.1.1.0/24"), kRibVersion, {input(makePath(2, 2, nh), peer, 1)});
  builder.addEntry(
      prefix("2.2.2.0/24"), kRibVersion, {input(makePath(2, 2, nh), peer, 1)});
  auto state = builder.build();

  EXPECT_EQ(state.deduped_paths()->size(), 1);
  EXPECT_EQ(state.peers()->size(), 1);
  EXPECT_EQ(state.rib_entries()->size(), 2);

  const auto& g = std::string(kDefaultPathGroup);
  auto idx1 = state.rib_entries()
                  ->at("1.1.1.0/24")
                  .paths()
                  ->at(g)[0]
                  .path_idx()
                  .value();
  auto idx2 = state.rib_entries()
                  ->at("2.2.2.0/24")
                  .paths()
                  ->at(g)[0]
                  .path_idx()
                  .value();
  EXPECT_EQ(idx1, idx2);
}

/*
 * Distinct whole paths (different next_hop) that share a community / AS_PATH
 * list keep distinct deduped_paths entries but a single shared dict entry.
 */
TEST_F(CanonicalRibBuilderTest, SubAttrDedup) {
  auto peer = folly::IPAddress("10.0.0.2");

  CanonicalRibBuilder builder;
  builder.addEntry(
      prefix("1.1.1.0/24"),
      kRibVersion,
      {input(makePath(2, 3, folly::IPAddress("10.0.0.10")), peer, 1),
       input(makePath(2, 3, folly::IPAddress("10.0.0.11")), peer, 1)});
  auto state = builder.build();

  ASSERT_EQ(state.deduped_paths()->size(), 2);
  EXPECT_EQ(state.attr_dict()->community_lists()->size(), 1);
  EXPECT_EQ(state.attr_dict()->as_path_lists()->size(), 1);

  const auto& dp = state.deduped_paths().value();
  EXPECT_EQ(
      dp.at(0).communities_idx().value(), dp.at(1).communities_idx().value());
  EXPECT_EQ(dp.at(0).as_path_idx().value(), dp.at(1).as_path_idx().value());
}

/*
 * Peers are interned per (addr, routerId): the same peer is shared, a different
 * peer gets its own index.
 */
TEST_F(CanonicalRibBuilderTest, PeerDedup) {
  CanonicalRibBuilder builder;
  builder.addEntry(
      prefix("1.1.1.0/24"),
      kRibVersion,
      {input(
           makePath(2, 2, folly::IPAddress("10.0.0.10")),
           folly::IPAddress("10.0.0.2"),
           1),
       input(
           makePath(3, 2, folly::IPAddress("10.0.0.11")),
           folly::IPAddress("10.0.0.2"),
           1),
       input(
           makePath(4, 2, folly::IPAddress("10.0.0.12")),
           folly::IPAddress("10.0.0.3"),
           2)});
  auto state = builder.build();

  EXPECT_EQ(state.peers()->size(), 2);

  const auto& paths = state.rib_entries()
                          ->at("1.1.1.0/24")
                          .paths()
                          ->at(std::string(kDefaultPathGroup));
  EXPECT_EQ(paths[0].peer_idx().value(), paths[1].peer_idx().value());
  EXPECT_NE(paths[0].peer_idx().value(), paths[2].peer_idx().value());
}

// Best vs default grouping + is_best_path flag are preserved per entry.
TEST_F(CanonicalRibBuilderTest, BestAndDefaultGroups) {
  auto peer = folly::IPAddress("10.0.0.2");

  CanonicalRibBuilder builder;
  builder.addEntry(
      prefix("1.1.1.0/24"),
      kRibVersion,
      {input(makePath(2, 2, folly::IPAddress("10.0.0.10")), peer, 1, true),
       input(makePath(3, 2, folly::IPAddress("10.0.0.11")), peer, 1, false)});
  auto state = builder.build();

  const auto& entry = state.rib_entries()->at("1.1.1.0/24");
  EXPECT_EQ(entry.rib_version().value(), kRibVersion);

  const auto& best = entry.paths()->at(std::string(kBestPathGroup));
  const auto& def = entry.paths()->at(std::string(kDefaultPathGroup));
  ASSERT_EQ(best.size(), 1);
  ASSERT_EQ(def.size(), 1);
  EXPECT_TRUE(best[0].is_best_path().value());
  EXPECT_FALSE(def[0].is_best_path().has_value());

  // The get-rib converter does not populate best_path (it is FSDB-only).
  EXPECT_FALSE(entry.best_path().has_value());
}

/* Per-instance and entry operational fields round-trip correctly. */
TEST_F(CanonicalRibBuilderTest, OperationalFields) {
  auto peer = folly::IPAddress("10.0.0.2");
  const auto backupAddr = folly::IPAddress("2001:db8::1");
  std::unordered_map<std::string, int64_t> topoInfo = {
      {"region", 1}, {"pod", 42}};

  CanonicalRibBuilder builder;
  CanonicalPathInput in = input(
      makePathWithTopoAndWeight(
          2, 2, folly::IPAddress("10.0.0.10"), topoInfo, 100, backupAddr),
      peer,
      1,
      true);
  in.igpCost = 500;
  in.lastModifiedTime = 1234567890;
  in.pathIdToSend = 99;
  in.bestPathFilterDescr = "test_filter";
  in.policyName = "test_policy";

  CanonicalEntryFields entryFields;
  entryFields.pathSelectionPending = true;

  builder.addEntry(prefix("1.1.1.0/24"), kRibVersion, {in}, entryFields);
  auto state = builder.build();

  /* Verify deduped path carries topology_info and weight */
  const auto& dedupedPath = state.deduped_paths()->at(0);
  ASSERT_TRUE(dedupedPath.topology_info().has_value());
  EXPECT_EQ(dedupedPath.topology_info()->at("region"), 1);
  EXPECT_EQ(dedupedPath.topology_info()->at("pod"), 42);
  ASSERT_TRUE(dedupedPath.weight().has_value());
  EXPECT_EQ(dedupedPath.weight().value(), 100);
  ASSERT_TRUE(dedupedPath.backup_addr().has_value());
  EXPECT_EQ(createTIpPrefix(backupAddr), *dedupedPath.backup_addr());

  /* Verify per-instance fields */
  const auto& paths = state.rib_entries()
                          ->at("1.1.1.0/24")
                          .paths()
                          ->at(std::string(kBestPathGroup));
  ASSERT_EQ(paths.size(), 1);
  ASSERT_TRUE(paths[0].igp_cost().has_value());
  EXPECT_EQ(paths[0].igp_cost().value(), 500);
  ASSERT_TRUE(paths[0].last_modified_time().has_value());
  EXPECT_EQ(paths[0].last_modified_time().value(), 1234567890);
  ASSERT_TRUE(paths[0].path_id_to_send().has_value());
  EXPECT_EQ(paths[0].path_id_to_send().value(), 99);
  ASSERT_TRUE(paths[0].bestpath_filter_descr().has_value());
  EXPECT_EQ(paths[0].bestpath_filter_descr().value(), "test_filter");
  ASSERT_TRUE(paths[0].policy_name().has_value());
  EXPECT_EQ(paths[0].policy_name().value(), "test_policy");

  /* Verify entry-level fields */
  const auto& entry = state.rib_entries()->at("1.1.1.0/24");
  ASSERT_TRUE(entry.path_selection_pending().has_value());
  EXPECT_TRUE(entry.path_selection_pending().value());
}

} // namespace facebook::bgp
