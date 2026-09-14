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

#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/fsdb/CanonicalRibExporter.h"
#include "neteng/fboss/bgp/cpp/rib/RibBase.h"
#include "neteng/fboss/bgp/cpp/rib/RibEntry.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {
namespace {

std::shared_ptr<const BgpPath> makePath(
    uint32_t asCount,
    const folly::IPAddress& nexthop) {
  auto path = std::make_shared<BgpPath>(
      *buildBgpPathFields(asCount, 0, 0, 0, 0, nexthop));
  path->publish();
  return path;
}

void clearAllDeduplicators() {
  nettools::bgplib::DeDuplicatedBgpPath::clearDeduplicator();
  nettools::bgplib::DeDuplicatedBgpAttributesC::clearDeduplicator();
  nettools::bgplib::DeDuplicatedAsPath::clearDeduplicator();
  nettools::bgplib::DeDuplicatedCommunities::clearDeduplicator();
  nettools::bgplib::DeDuplicatedExtCommunities::clearDeduplicator();
  nettools::bgplib::DeDuplicatedClusterList::clearDeduplicator();
}

} // namespace

class CanonicalRibExporterTest : public ::testing::Test {
 public:
  void SetUp() override {
    clearAllDeduplicators();
  }

  void TearDown() override {
    clearAllDeduplicators();
  }

  std::unique_ptr<RibEntry> selectedEntry(
      const folly::CIDRNetwork& prefix,
      const std::vector<folly::IPAddress>& nexthops) {
    auto entry = std::make_unique<RibEntry>(prefix);
    uint32_t pathId = 0;
    for (const auto& nexthop : nexthops) {
      TinyPeerInfo peer(
          nexthop,
          kPeerAsn1,
          /*routerId=*/nexthop.asV4().toLongHBO(),
          BgpSessionType::EBGP,
          /*isRrClient=*/false);
      entry->updatePath(
          peer,
          makePath(/*asCount=*/2, nexthop),
          /*installToFib=*/true,
          pathId++);
    }
    RibBase::selectBestPath(
        *entry,
        multipathSelector,
        bestpathSelector,
        /*computeUcmp=*/false,
        /*ucmpWidth=*/0);
    return entry;
  }

  std::vector<CanonicalPathInput> buildPathInputs(const RibEntry& entry) const {
    std::vector<CanonicalPathInput> inputs;
    const auto best = entry.getBestPath();
    const auto& multipaths = entry.getMultipaths();
    for (const auto& routeInfo : entry.getAllPaths()) {
      CanonicalPathInput input;
      input.path = routeInfo->attrs;
      input.peerAddr = routeInfo->peer.addr;
      input.peerRouterId = routeInfo->peer.routerId;
      input.peerDescription = routeInfo->peer.description;
      input.pathId = routeInfo->receivedPathId;
      input.isBestPath = routeInfo == best;
      input.group = routeInfo->pathIdToSend.has_value() &&
              multipaths.contains(routeInfo->pathIdToSend.value())
          ? kBestPathGroup
          : kDefaultPathGroup;
      inputs.push_back(std::move(input));
    }
    return inputs;
  }

  CanonicalRibPrefixUpdate update(
      const RibEntry& entry,
      const CanonicalEntryFields& fields = {},
      bool includePaths = true) const {
    std::optional<CanonicalRibEntryInput> entryInput;
    if (entry.getAllPathsCnt() != 0) {
      entryInput = CanonicalRibEntryInput{
          .paths = buildPathInputs(entry),
          .fields = fields,
          .includePaths = includePaths,
      };
    }
    return CanonicalRibPrefixUpdate{
        .prefix = entry.getPrefix(),
        .entry = std::move(entryInput),
    };
  }

  const bgp_thrift::TRibEntryCanonical& entryFor(
      const CanonicalRibIncrementalPayload& payload,
      const folly::CIDRNetwork& prefix) const {
    return payload.entries.at(folly::IPAddress::networkToString(prefix))
        .value();
  }

  CanonicalRibIncrementalPayload incrementalPayload(
      CanonicalRibPrefixUpdate prefixUpdate) {
    exporter_.accumulatePrefixUpdate(std::move(prefixUpdate));
    return exporter_.buildIncrementalBatch(now_);
  }

  CanonicalRibExporter exporter_;
  CanonicalRibExporter::TimePoint now_{};
};

TEST_F(CanonicalRibExporterTest, ExportsFullMultipathSet) {
  auto entry = selectedEntry(
      folly::IPAddress::createNetwork("10.10.0.0/16"),
      {folly::IPAddress("10.0.0.1"),
       folly::IPAddress("10.0.0.2"),
       folly::IPAddress("10.0.0.3")});
  ASSERT_NE(entry->getBestPath(), nullptr);
  ASSERT_FALSE(entry->getMultipaths().empty());

  auto payload = incrementalPayload(update(*entry));

  ASSERT_TRUE(payload.poolSnapshot.has_value());
  EXPECT_EQ(
      entry->getMultipaths().size(), payload.poolSnapshot->dedupedPaths.size());
}

TEST_F(CanonicalRibExporterTest, EncodesBestPathOnlyInput) {
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  auto entry = selectedEntry(
      prefix,
      {folly::IPAddress("10.0.0.1"),
       folly::IPAddress("10.0.0.2"),
       folly::IPAddress("10.0.0.3")});

  auto payload = incrementalPayload(update(*entry, {}, /*includePaths=*/false));

  ASSERT_TRUE(payload.poolSnapshot.has_value());
  EXPECT_TRUE(payload.poolSnapshot->dedupedPaths.empty());
  EXPECT_TRUE(entryFor(payload, prefix).paths()->empty());
  EXPECT_TRUE(entryFor(payload, prefix).best_path().has_value());
}

TEST_F(CanonicalRibExporterTest, NoBestPathRetainsCandidatesAndMetadata) {
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  RibEntry entry(prefix);
  const folly::IPAddress peerAddr("10.0.0.1");
  TinyPeerInfo peer(
      peerAddr,
      kPeerAsn1,
      /*routerId=*/peerAddr.asV4().toLongHBO(),
      BgpSessionType::EBGP,
      /*isRrClient=*/false);
  entry.updatePath(
      peer,
      makePath(/*asCount=*/2, peerAddr),
      /*installToFib=*/true,
      /*receivedPathId=*/0);
  ASSERT_EQ(nullptr, entry.getBestPath());

  CanonicalEntryFields fields;
  fields.pathSelectionPending = true;
  fields.activeCpsCriteria = rib_policy::TPathSelector{};
  fields.activeCteUcmpAction = rib_policy::TRouteAttributeUcmpAction{};
  auto payload = incrementalPayload(update(entry, fields));

  const auto& canonical = entryFor(payload, prefix);
  EXPECT_FALSE(canonical.rib_version().has_value());
  EXPECT_FALSE(canonical.best_path().has_value());
  EXPECT_FALSE(canonical.paths()->empty());
  EXPECT_TRUE(canonical.path_selection_pending().value());
  EXPECT_TRUE(canonical.active_cps_criteria().has_value());
  EXPECT_TRUE(canonical.active_cte_ucmp_action().has_value());
}

TEST_F(CanonicalRibExporterTest, WithdrawalProducesDelete) {
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  RibEntry withdrawal(prefix);

  auto payload = incrementalPayload(update(withdrawal));

  const auto key = folly::IPAddress::networkToString(prefix);
  ASSERT_TRUE(payload.entries.contains(key));
  EXPECT_FALSE(payload.entries.at(key).has_value());
  EXPECT_FALSE(payload.poolSnapshot.has_value());
}

TEST_F(CanonicalRibExporterTest, AccumulatesPrefixUpdatesIntoOneBatch) {
  const auto upsertPrefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  const auto withdrawalPrefix = folly::IPAddress::createNetwork("10.20.0.0/16");
  auto entry = selectedEntry(upsertPrefix, {folly::IPAddress("10.0.0.1")});
  RibEntry withdrawal(withdrawalPrefix);

  exporter_.accumulatePrefixUpdate(update(*entry));
  exporter_.accumulatePrefixUpdate(update(withdrawal));
  auto payload = exporter_.buildIncrementalBatch(now_);

  const auto upsertKey = folly::IPAddress::networkToString(upsertPrefix);
  const auto withdrawalKey =
      folly::IPAddress::networkToString(withdrawalPrefix);
  ASSERT_EQ(2, payload.entries.size());
  EXPECT_TRUE(payload.entries.at(upsertKey).has_value());
  EXPECT_FALSE(payload.entries.at(withdrawalKey).has_value());
  EXPECT_TRUE(payload.poolSnapshot.has_value());
}

TEST_F(CanonicalRibExporterTest, RetainsLatestPrefixStateWithinBatch) {
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  auto entry = selectedEntry(prefix, {folly::IPAddress("10.0.0.1")});
  RibEntry withdrawal(prefix);

  exporter_.accumulatePrefixUpdate(update(*entry));
  exporter_.accumulatePrefixUpdate(update(withdrawal));
  auto payload = exporter_.buildIncrementalBatch(now_);

  const auto key = folly::IPAddress::networkToString(prefix);
  ASSERT_EQ(1, payload.entries.size());
  EXPECT_FALSE(payload.entries.at(key).has_value());
}

TEST_F(CanonicalRibExporterTest, CompletedBatchDoesNotLeakIntoNextBatch) {
  const auto firstPrefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  const auto secondPrefix = folly::IPAddress::createNetwork("10.20.0.0/16");
  auto firstEntry = selectedEntry(firstPrefix, {folly::IPAddress("10.0.0.1")});
  RibEntry secondWithdrawal(secondPrefix);

  incrementalPayload(update(*firstEntry));
  auto secondBatch = incrementalPayload(update(secondWithdrawal));

  const CanonicalRibEntryDeltas expected{
      {folly::IPAddress::networkToString(secondPrefix), std::nullopt}};
  EXPECT_EQ(expected, secondBatch.entries);
}

TEST_F(CanonicalRibExporterTest, UnchangedPoolsAreOmitted) {
  auto entry = selectedEntry(
      folly::IPAddress::createNetwork("10.10.0.0/16"),
      {folly::IPAddress("10.0.0.1")});
  auto first = incrementalPayload(update(*entry));
  ASSERT_TRUE(first.poolSnapshot.has_value());

  auto second = incrementalPayload(update(*entry));

  EXPECT_FALSE(second.poolSnapshot.has_value());
}

TEST_F(CanonicalRibExporterTest, IncrementalKeepsReferencedPathsAlive) {
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  auto entry = selectedEntry(prefix, {folly::IPAddress("10.0.0.1")});
  auto payload = incrementalPayload(update(*entry));
  entry.reset();
  clearAllDeduplicators();

  ASSERT_TRUE(payload.poolSnapshot.has_value());
  const auto& paths = entryFor(payload, prefix).paths().value();
  ASSERT_FALSE(paths.empty());
  ASSERT_FALSE(paths.begin()->second.empty());
  const auto pathIndex = paths.begin()->second.front().path_idx().value();
  EXPECT_TRUE(payload.poolSnapshot->dedupedPaths.contains(pathIndex));

  now_ += std::chrono::minutes(3);
  RibEntry unrelatedWithdrawal(folly::IPAddress::createNetwork("10.20.0.0/16"));
  incrementalPayload(update(unrelatedWithdrawal));
  EXPECT_EQ(1, exporter_.poolStats().wholePath.live);
}

TEST_F(CanonicalRibExporterTest, BuildsFullSnapshot) {
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  auto entry = selectedEntry(prefix, {folly::IPAddress("10.0.0.1")});

  auto snapshot = exporter_.buildFullSnapshot(
      CanonicalRibFullSnapshotInput{.entries = {update(*entry)}}, now_);

  ASSERT_EQ(1, snapshot.state.rib_entries()->size());
  EXPECT_TRUE(snapshot.state.rib_entries()->contains("10.10.0.0/16"));
  EXPECT_TRUE(
      snapshot.state.rib_entries()->at("10.10.0.0/16").best_path().has_value());
}

TEST_F(CanonicalRibExporterTest, FullSnapshotKeepsReferencedPathsAlive) {
  const auto prefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  auto entry = selectedEntry(prefix, {folly::IPAddress("10.0.0.1")});
  CanonicalRibFullSnapshotInput snapshotInput{
      .entries = {update(*entry)},
  };
  entry.reset();
  clearAllDeduplicators();

  auto snapshot = exporter_.buildFullSnapshot(std::move(snapshotInput), now_);

  const auto& canonical = snapshot.state.rib_entries()->at("10.10.0.0/16");
  ASSERT_FALSE(canonical.paths()->empty());
  ASSERT_FALSE(canonical.paths()->begin()->second.empty());
  const auto pathIndex =
      canonical.paths()->begin()->second.front().path_idx().value();
  EXPECT_TRUE(snapshot.state.deduped_paths()->contains(pathIndex));

  now_ += std::chrono::minutes(3);
  RibEntry unrelatedWithdrawal(folly::IPAddress::createNetwork("10.20.0.0/16"));
  incrementalPayload(update(unrelatedWithdrawal));
  EXPECT_EQ(1, exporter_.poolStats().wholePath.live);
}

TEST_F(CanonicalRibExporterTest, FullSnapshotDropsWithdrawals) {
  const auto presentPrefix = folly::IPAddress::createNetwork("10.1.0.0/16");
  const auto withdrawnPrefix = folly::IPAddress::createNetwork("10.2.0.0/16");
  auto present = selectedEntry(presentPrefix, {folly::IPAddress("10.0.0.1")});
  auto withdrawn =
      selectedEntry(withdrawnPrefix, {folly::IPAddress("10.0.0.2")});
  RibEntry withdrawal(withdrawnPrefix);

  auto snapshot = exporter_.buildFullSnapshot(
      CanonicalRibFullSnapshotInput{
          .entries = {update(*present), update(*withdrawn), update(withdrawal)},
      },
      now_);

  ASSERT_EQ(1, snapshot.state.rib_entries()->size());
  EXPECT_TRUE(snapshot.state.rib_entries()->contains("10.1.0.0/16"));
  EXPECT_FALSE(snapshot.state.rib_entries()->contains("10.2.0.0/16"));
}

TEST_F(CanonicalRibExporterTest, FullSnapshotReplacesIncrementalState) {
  auto oldEntry = selectedEntry(
      folly::IPAddress::createNetwork("10.1.0.0/16"),
      {folly::IPAddress("10.0.0.1")});
  auto snapshotEntry = selectedEntry(
      folly::IPAddress::createNetwork("10.2.0.0/16"),
      {folly::IPAddress("10.0.0.2")});
  exporter_.accumulatePrefixUpdate(update(*oldEntry));

  auto snapshot = exporter_.buildFullSnapshot(
      CanonicalRibFullSnapshotInput{.entries = {update(*snapshotEntry)}}, now_);
  auto pendingBatch = exporter_.buildIncrementalBatch(now_);

  ASSERT_EQ(1, snapshot.state.rib_entries()->size());
  EXPECT_FALSE(snapshot.state.rib_entries()->contains("10.1.0.0/16"));
  EXPECT_TRUE(snapshot.state.rib_entries()->contains("10.2.0.0/16"));
  EXPECT_TRUE(pendingBatch.entries.empty());
  EXPECT_FALSE(pendingBatch.poolSnapshot.has_value());
}

TEST_F(CanonicalRibExporterTest, ResetDropsAllEncoderState) {
  auto entry = selectedEntry(
      folly::IPAddress::createNetwork("10.1.0.0/16"),
      {folly::IPAddress("10.0.0.1")});
  incrementalPayload(update(*entry));
  ASSERT_EQ(1, exporter_.poolStats().wholePath.live);

  exporter_.reset();

  const auto stats = exporter_.poolStats();
  EXPECT_EQ(0, stats.wholePath.live);
  EXPECT_EQ(0, stats.asPath.live);
  EXPECT_EQ(0, stats.communities.live);
  EXPECT_EQ(0, stats.extCommunities.live);
  EXPECT_EQ(0, stats.clusterList.live);
}

TEST_F(CanonicalRibExporterTest, PoolIdsRemainUntilEntryStopsReferencingThem) {
  const auto retiredPrefix = folly::IPAddress::createNetwork("10.10.0.0/16");
  const auto retainedPrefix = folly::IPAddress::createNetwork("10.20.0.0/16");
  auto retiredEntry =
      selectedEntry(retiredPrefix, {folly::IPAddress("10.0.0.1")});
  auto retainedEntry =
      selectedEntry(retainedPrefix, {folly::IPAddress("10.0.0.2")});
  auto retiredPayload = incrementalPayload(update(*retiredEntry));
  auto initial = incrementalPayload(update(*retainedEntry));
  ASSERT_TRUE(initial.poolSnapshot.has_value());
  ASSERT_EQ(2, initial.poolSnapshot->dedupedPaths.size());
  const auto& retiredPaths =
      entryFor(retiredPayload, retiredPrefix).paths().value();
  ASSERT_FALSE(retiredPaths.empty());
  ASSERT_FALSE(retiredPaths.begin()->second.empty());
  const auto retiredPathIndex =
      retiredPaths.begin()->second.front().path_idx().value();

  retiredEntry.reset();
  auto replacementEntry =
      selectedEntry(retainedPrefix, {folly::IPAddress("10.0.0.3")});
  retainedEntry.reset();
  clearAllDeduplicators();
  now_ += std::chrono::minutes(3);
  auto unrelatedPayload = incrementalPayload(update(*replacementEntry));
  ASSERT_TRUE(unrelatedPayload.poolSnapshot.has_value());
  EXPECT_TRUE(
      unrelatedPayload.poolSnapshot->dedupedPaths.contains(retiredPathIndex));
  EXPECT_EQ(2, exporter_.poolStats().wholePath.live);

  RibEntry withdrawal(retiredPrefix);
  auto withdrawalPayload = incrementalPayload(update(withdrawal));
  EXPECT_FALSE(withdrawalPayload.poolSnapshot.has_value());

  now_ += std::chrono::minutes(3);
  auto cleanupPayload = incrementalPayload(update(*replacementEntry));
  ASSERT_TRUE(cleanupPayload.poolSnapshot.has_value());
  EXPECT_EQ(1, cleanupPayload.poolSnapshot->dedupedPaths.size());
  EXPECT_FALSE(
      cleanupPayload.poolSnapshot->dedupedPaths.contains(retiredPathIndex));
}

} // namespace facebook::bgp
