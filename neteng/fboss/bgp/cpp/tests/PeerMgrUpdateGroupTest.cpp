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

#define PeerManager_TEST_FRIENDS                                               \
  FRIEND_TEST(PeerManagerUpdateGroupTestFixture, UpdateGroupConstructionTest); \
  FRIEND_TEST(                                                                 \
      PeerManagerUpdateGroupTestFixture,                                       \
      GetUpdateGroupInfoSurfacesGroupStats);

#define AdjRibOutGroup_TEST_FRIENDS      \
  FRIEND_TEST(                           \
      PeerManagerUpdateGroupTestFixture, \
      GetUpdateGroupInfoSurfacesGroupStats);

#include <folly/coro/BlockingWait.h>
#include <folly/fibers/FiberManagerMap.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/tests/PeerManagerTestUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using ::testing::_;
using namespace facebook::nettools::bgplib;

namespace facebook::bgp {

class PeerManagerUpdateGroupTestFixture : public PeerManagerTestFixture {
 public:
  void SetUp() override {
    PeerManagerTestFixture::SetUp();
    auto config = getConfig(
        true, /* includeStaticPeer */
        false, /* includeDynamicShivPeer */
        false, /* includeDynamicMonitorPeer */
        false, /* includeDynamicVipInjectorPeer */
        false, /* enableStatefulHa */
        false, /* enableVipServer */
        kDefaultEorTimeS, /* eorTimeS */
        false, /* enableSubscriberLimit */
        false, /* enableSwitchLimit */
        false, /* applyGoldenPrefixPolicy */
        {}, /* bgpFeatures */
        false, /* enableDynamicPolicyEvaluation */
        true /* enableUpdateGroup */);

    auto configManager = std::make_shared<ConfigManager>(config);
    peerMgr_ = std::make_shared<PeerManagerBase>(
        configManager, nullptr, ribInQ_, ribOutQ_, nbrRouteChangeQ_);

    auto versionNumber = std::make_shared<VersionNumber>(version_);
    auto mockInfo = mockInfo1_;
    // Set AFI to v4 so that AdjRibOut doesn't skip building announcements.
    mockInfo.negotiatedCapabilities.mpExtV4Unicast() = true;

    sessionInfo_ = FiberBgpPeer::getObservableSessionInfo(
        mockInfo, adjRibOutQ_, boundedAdjRibOutQ_, adjRibInQ_, versionNumber);
  }

  void cleanUp() {
    adjRibInQ_->fiberPush(
        FiberBgpPeer::BgpSessionStop{GracefulRestartFlag{false}});
    folly::coro::blockingWait(asyncScope_.cancelAndJoinAsync());
    peerMgr_->stop();
  }

  std::shared_ptr<PeerManagerBase> peerMgr_;
  folly::coro::CancellableAsyncScope asyncScope_;

  uint64_t version_ = 0x100;
  std::shared_ptr<FiberBgpPeer::ObservableSessionInfo> sessionInfo_;

  std::shared_ptr<AdjRib::AdjRibInQueueT> adjRibInQ_ =
      std::make_shared<AdjRib::AdjRibInQueueT>();
  std::shared_ptr<AdjRib::AdjRibOutQueueT> adjRibOutQ_ =
      std::make_shared<AdjRib::AdjRibOutQueueT>();
  std::shared_ptr<AdjRib::BoundedAdjRibOutQueueT> boundedAdjRibOutQ_ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          kMaxEgressQueueSize,
          kEgressQueueHighWatermark,
          kEgressQueueLowWatermark);
};

/*
 * This test verifies the updateGroup creation when directly invoking
 * sessionEstablished() call from PeerManagerBase.
 */
TEST_F(PeerManagerUpdateGroupTestFixture, UpdateGroupConstructionTest) {
  auto& evb = peerMgr_->getEventBase();
  auto& fm = folly::fibers::getFiberManager(peerMgr_->getEventBase(), options_);

  FiberBgpPeer::ObservableStateT stateEvent{
      .peerId = kPeerId3,
      .versionNumber = version_,
      .sessionInfo = sessionInfo_};

  peerMgr_->ribInitialAnnouncementStarted_ = true;
  peerMgr_->ribInitialAnnouncementDone_ = true;

  fm.addTask([&] {
    EXPECT_EQ(0, peerMgr_->pendingRibDumpReqs_.size());
    folly::coro::blockingWait(peerMgr_->sessionEstablished(stateEvent));

    // Validation of update-group creation
    auto adjRib = peerMgr_->adjRibs_.at(kPeerId3);
    auto updateGroupKey = adjRib->getUpdateGroupKey();
    EXPECT_EQ(1, peerMgr_->updateGroupManager_->getGroupCount());
    EXPECT_TRUE(peerMgr_->updateGroupManager_->hasGroup(updateGroupKey));
    auto adjRibOutGroup = peerMgr_->adjRibOutGroups_.begin()->second;
    auto updateGroup =
        peerMgr_->updateGroupManager_->findOrCreateGroup(updateGroupKey);
    EXPECT_NE(adjRibOutGroup, updateGroup);

    // trigger stop of task
    cleanUp();
  });
  evb.loop();
  SUCCEED();
}

/*
 * getUpdateGroupInfo() surfaces the group's OWN AdjRibStats (announcement /
 * withdrawal / UPDATE / EoR PDU counts) into TUpdateGroupStats. For in-sync
 * members the UPDATE is built once at the group, so the members' per-peer
 * counters stay 0 and the group's counters are the source of truth. This guards
 * the group-stats -> thrift wiring (previously the announce/withdraw fields
 * were summed from the zeroed per-peer counters and read 0).
 */
TEST_F(
    PeerManagerUpdateGroupTestFixture,
    GetUpdateGroupInfoSurfacesGroupStats) {
  auto& evb = peerMgr_->getEventBase();
  auto& fm = folly::fibers::getFiberManager(peerMgr_->getEventBase(), options_);

  FiberBgpPeer::ObservableStateT stateEvent{
      .peerId = kPeerId3,
      .versionNumber = version_,
      .sessionInfo = sessionInfo_};

  peerMgr_->ribInitialAnnouncementStarted_ = true;
  peerMgr_->ribInitialAnnouncementDone_ = true;

  fm.addTask([&] {
    folly::coro::blockingWait(peerMgr_->sessionEstablished(stateEvent));

    auto adjRib = peerMgr_->adjRibs_.at(kPeerId3);
    auto updateGroupKey = adjRib->getUpdateGroupKey();
    // Read the group from the same manager getUpdateGroupInfo() iterates.
    auto group =
        peerMgr_->updateGroupManager_->findOrCreateGroup(updateGroupKey);
    ASSERT_NE(group, nullptr);

    // Inject known group-level counts directly (the send path would set these;
    // here we set them to keep the test focused on getUpdateGroupInfo wiring).
    constexpr uint64_t kAnnV4 = 11;
    constexpr uint64_t kAnnV6 = 12;
    constexpr uint64_t kWithdrawals = 13;
    for (uint64_t i = 0; i < kAnnV4; ++i) {
      group->stats_.incrementSentAnnouncementsIpv4();
    }
    for (uint64_t i = 0; i < kAnnV6; ++i) {
      group->stats_.incrementSentAnnouncementsIpv6();
    }
    for (uint64_t i = 0; i < kWithdrawals; ++i) {
      group->stats_.incrementSentWithdrawals();
    }

    const auto groupId = static_cast<int64_t>(group->getGroupId());
    auto infos = peerMgr_->getUpdateGroupInfo(groupId);
    ASSERT_EQ(1, infos.size());
    const auto& stats = infos[0].stats().value();
    EXPECT_EQ(kAnnV4, stats.total_sent_announcement_msgs_ipv4().value());
    EXPECT_EQ(kAnnV6, stats.total_sent_announcement_msgs_ipv6().value());
    EXPECT_EQ(kWithdrawals, stats.total_sent_withdrawal_msgs().value());

    cleanUp();
  });
  evb.loop();
  SUCCEED();
}

} // namespace facebook::bgp
