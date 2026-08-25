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

#include <fb303/ThreadCachedServiceData.h>

#include "neteng/fboss/bgp/cpp/stats/StatsDC.h"

namespace facebook::bgp {

TEST(StatsDCTest, InitStatsDC_PreservesOdsKeysAndSupportsPlatformMutators) {
  EXPECT_EQ(
      "bgpd.vipServiceEnabled", std::string(BgpStatsDC::kVipServiceEnabled));
  EXPECT_EQ(
      "bgpd.runningVipServiceSessions",
      std::string(BgpStatsDC::kRunningVipServiceSessions));
  EXPECT_EQ(
      "bgpd.ucmp.auto_link_bandwidth.enabled",
      std::string(BgpStatsDC::kUcmpAlbwEnabled));
  EXPECT_EQ("bgpcpp.rib.is_partial_drain", RibStatsDC::kRibIsPartialDrain);
  EXPECT_EQ("bgpcpp.nht.fsdb.connected", FsdbStatsDC::kFsdbNhtConnected);
  EXPECT_EQ(
      "bgpcpp.nht.fsdb.nexthop_reachable",
      FsdbStatsDC::kFsdbNhtNexthopReachable);
  EXPECT_EQ(
      "bgpd.cps.artifact_read.success",
      std::string(BgpStatsDC::kCpsArtifactReadSuccess));

  initStatsDC();
  auto* stats = fb303::ThreadCachedServiceData::get();

  EXPECT_EQ(-1, stats->getCounter(BgpStatsDC::kVipServiceEnabled));
  EXPECT_EQ(-1, stats->getCounter(BgpStatsDC::kRunningVipServiceSessions));
  EXPECT_EQ(-1, stats->getCounter(BgpStatsDC::kUcmpAlbwEnabled));
  EXPECT_EQ(0, stats->getCounter(RibStatsDC::kRibIsPartialDrain));
  EXPECT_EQ(-1, stats->getCounter(FsdbStatsDC::kFsdbNhtConnected));
  EXPECT_EQ(
      0, stats->getCounter(FsdbStatsDC::kFsdbNhtNexthopReachable + ".count"));
  EXPECT_EQ(0, stats->getCounter(BgpStatsDC::kCpsArtifactReadSuccess + ".sum"));

  BgpStatsDC::setVipServiceEnabled(true);
  BgpStatsDC::setRunningVipServiceSessions(3);
  BgpStatsDC::setUcmpAlbwEnabled(true);
  BgpStatsDC::incrCpsArtifactReadSuccess();
  RibStatsDC::setIsPartialDrain(true);
  FsdbStatsDC::setFsdbNhtConnected(1);
  FsdbStatsDC::incrFsdbNhtNexthopReachable();
  stats->publishStats();

  EXPECT_EQ(1, stats->getCounter(BgpStatsDC::kVipServiceEnabled));
  EXPECT_EQ(3, stats->getCounter(BgpStatsDC::kRunningVipServiceSessions));
  EXPECT_EQ(1, stats->getCounter(BgpStatsDC::kUcmpAlbwEnabled));
  EXPECT_EQ(1, stats->getCounter(RibStatsDC::kRibIsPartialDrain));
  EXPECT_EQ(1, stats->getCounter(FsdbStatsDC::kFsdbNhtConnected));
  EXPECT_EQ(
      1, stats->getCounter(FsdbStatsDC::kFsdbNhtNexthopReachable + ".count"));
  EXPECT_EQ(1, stats->getCounter(BgpStatsDC::kCpsArtifactReadSuccess + ".sum"));
}

TEST(StatsDCTest, FsdbNhtInitCounterTest) {
  auto counters = fb303::ThreadCachedServiceData::getShared();

  FsdbStatsDC::initCounters();

  EXPECT_FALSE(counters->hasCounter("fsdbNhtNexthopReachable.count"));
  EXPECT_FALSE(counters->hasCounter("fsdbNhtNexthopUnreachable.count"));
  EXPECT_FALSE(counters->hasCounter("fsdbNhtDisconnects.count"));

  EXPECT_EQ(
      0,
      counters->getCounter(FsdbStatsDC::kFsdbNhtNexthopReachable + ".count"));
  EXPECT_EQ(
      0,
      counters->getCounter(
          FsdbStatsDC::kFsdbNhtNexthopReachable + ".count.60"));
  EXPECT_EQ(
      0,
      counters->getCounter(FsdbStatsDC::kFsdbNhtNexthopUnreachable + ".count"));
  EXPECT_EQ(
      0,
      counters->getCounter(
          FsdbStatsDC::kFsdbNhtNexthopUnreachable + ".count.60"));
  EXPECT_EQ(
      0, counters->getCounter(FsdbStatsDC::kFsdbNhtDisconnects + ".count"));
  EXPECT_EQ(
      0, counters->getCounter(FsdbStatsDC::kFsdbNhtDisconnects + ".count.60"));
  EXPECT_EQ(-1, counters->getCounter(FsdbStatsDC::kFsdbNhtConnected));
}

TEST(StatsDCTest, FsdbNhtCounterIncrementTest) {
  FsdbStatsDC::initCounters();
  auto tcData = fb303::ThreadCachedServiceData::get();

  FsdbStatsDC::incrFsdbNhtNexthopReachable();
  FsdbStatsDC::incrFsdbNhtNexthopReachable();
  FsdbStatsDC::incrFsdbNhtNexthopUnreachable();
  FsdbStatsDC::incrFsdbNhtDisconnects();
  FsdbStatsDC::setFsdbNhtConnected(1);
  tcData->publishStats();

  EXPECT_EQ(
      2, tcData->getCounter(FsdbStatsDC::kFsdbNhtNexthopReachable + ".count"));
  EXPECT_EQ(
      1,
      tcData->getCounter(FsdbStatsDC::kFsdbNhtNexthopUnreachable + ".count"));
  EXPECT_EQ(1, tcData->getCounter(FsdbStatsDC::kFsdbNhtDisconnects + ".count"));
  EXPECT_EQ(1, tcData->getCounter(FsdbStatsDC::kFsdbNhtConnected));

  FsdbStatsDC::setFsdbNhtConnected(0);
  tcData->publishStats();
  EXPECT_EQ(0, tcData->getCounter(FsdbStatsDC::kFsdbNhtConnected));
}

TEST(StatsDCTest, DirectPolicyStatsPreserveOdsKeys) {
  RibStatsDC::initCounters();
  auto* stats = fb303::ThreadCachedServiceData::get();

  EXPECT_EQ("bgpd.ribPolicy.numRcvdPsPolicy", RibStatsDC::kPsPolicyRcvd);
  EXPECT_EQ("bgpd.ribPolicy.numUpdatedRaPolicy", RibStatsDC::kRaPolicyUpdate);

  RibStatsDC::STATS_psPolicyRcvd.add(1);
  RibStatsDC::STATS_raPolicyUpdate.add(1);
  stats->publishStats();

  EXPECT_EQ(
      1, stats->getCounter(std::string(RibStatsDC::kPsPolicyRcvd) + ".count"));
  EXPECT_EQ(
      1,
      stats->getCounter(std::string(RibStatsDC::kRaPolicyUpdate) + ".count"));
}

TEST(StatsDCTest, CanonicalRibPoolStats) {
  auto* stats = fb303::ThreadCachedServiceData::get();
  RibStatsDC::setCanonicalRibPoolStats("unitTest", /*live=*/3, /*highWater=*/5);
  EXPECT_EQ(
      3, stats->getCounter("bgpcpp.rib.canonicalExporter.pool.unitTest.live"));
  EXPECT_EQ(
      5,
      stats->getCounter(
          "bgpcpp.rib.canonicalExporter.pool.unitTest.highWater"));
}

TEST(StatsDCTest, FsdbSyncerEventCounters) {
  auto* stats = fb303::ThreadCachedServiceData::get();
  FsdbStatsDC::addFsdbSyncerSubtreeEvent("unitTest", "numUpdate");
  FsdbStatsDC::addFsdbSyncerSubtreeEvent("unitTest", "numUpdate");
  FsdbStatsDC::addFsdbSyncerLifecycleEvent("numUnitTestConnect");
  stats->publishStats();
  EXPECT_EQ(
      2, stats->getCounter("bgpcpp.fsdbSyncer.unitTest.numUpdate.sum.60"));
  EXPECT_EQ(
      1, stats->getCounter("bgpcpp.fsdbSyncer.numUnitTestConnect.sum.60"));
}

TEST(StatsDCTest, CanonicalExportLatencyCounters) {
  RibStatsDC::STATS_canonicalRibExportRibThreadTimeMs.addValue(7);
  FsdbStatsDC::STATS_fsdbSyncerPublishStateEnqueueTimeMs.addValue(3);
  facebook::fb303::ServiceData::get()->getQuantileStatMap()->flushAll();

  auto* stats = fb303::ThreadCachedServiceData::get();
  EXPECT_EQ(
      7,
      stats->getCounter("bgpcpp.rib.canonicalExporter.ribThreadTimeMs.avg.60"));
  EXPECT_EQ(
      3,
      stats->getCounter("bgpcpp.fsdbSyncer.publishStateEnqueueTimeMs.avg.60"));
}

} // namespace facebook::bgp
