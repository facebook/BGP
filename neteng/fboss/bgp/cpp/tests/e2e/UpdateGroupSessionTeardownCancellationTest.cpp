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

#include <limits>

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook::bgp {
namespace {

constexpr int kNumCongestionPrefixes = 8;

BgpPeerSpec ipv4Only(BgpPeerSpec spec) {
  spec.disableIpv6Afi = true;
  return spec;
}

std::string congestionPrefix(int index) {
  return fmt::format("105.{}.0.0/16", index);
}

struct PeerSnapshot {
  PeerUpdateState state{PeerUpdateState::DOWN};
  bool detachedOnRegistration{false};
  bool hasChangeListConsumer{false};
  uint64_t peerRibVersion{0};
  uint64_t groupRibVersion{0};
  uint64_t timesRejoined{0};
};

} // namespace

class UpdateGroupSessionTeardownCancellationTest : public SlowPeerTestBase {
 protected:
  void blockWithoutDetach(const folly::IPAddress& peerAddr) {
    setSlowPeerThresholds(
        peerAddr,
        std::chrono::milliseconds(600000),
        std::numeric_limits<uint32_t>::max(),
        std::chrono::milliseconds(600000));
    blockPeer(peerAddr);
  }

  PeerSnapshot snapshotPeer(const folly::IPAddress& peerAddr) {
    auto& evb = peerManager_->getEventBase();
    return folly::via(
               &evb,
               [this, peerAddr]() {
                 PeerSnapshot snapshot;
                 auto adjRib = getAdjRibByAddr(peerAddr);
                 if (!adjRib) {
                   return snapshot;
                 }
                 auto group = adjRib->getUpdateGroup();
                 snapshot.state = adjRib->getPeerState();
                 snapshot.detachedOnRegistration =
                     adjRib->isAdjRibFlagSet(AdjRib::DETACHED_ON_REGISTRATION);
                 snapshot.hasChangeListConsumer =
                     adjRib->getChangeListConsumer() != nullptr;
                 snapshot.peerRibVersion = adjRib->getLastSeenRibVersion();
                 snapshot.timesRejoined =
                     adjRib->getStats().getNumTimesRejoined();
                 if (group) {
                   snapshot.groupRibVersion = group->getLastSeenRibVersion();
                 }
                 return snapshot;
               })
        .get();
  }
};

TEST_P(
    UpdateGroupSessionTeardownCancellationTest,
    CancelledSendDoesNotRejoinBeforePeerManagerTermination) {
  addPeer(ipv4Only(kDefaultPeerSpec3));
  addPeer(ipv4Only(kDefaultPeerSpec4));
  addPeer(ipv4Only(kDefaultPeerSpec5));
  setupSlowPeerComponents(
      /*queueCapacity=*/3, /*queueHighWm=*/2, /*queueLowWm=*/1);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);

  blockWithoutDetach(kPeerAddr3);
  blockWithoutDetach(kPeerAddr4);
  for (int i = 1; i <= kNumCongestionPrefixes; ++i) {
    auto prefix = congestionPrefix(i);
    injectLocalRoutesAtRuntime({prefix}, {fmt::format("65000:{}", i)}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
  }
  ASSERT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerQueueBlocked(peerId4));

  bringUpPeer(kPeerAddr5);
  blockWithoutDetach(kPeerAddr5);
  sendEoRToPeer(peerId5);
  ASSERT_TRUE(waitForPeerQueueBlocked(peerId5));

  unblockPeer(kPeerAddr3);
  unblockPeer(kPeerAddr4);

  PeerSnapshot beforeStop;
  bool readyForRejoin = false;
  WITH_RETRIES_N_TIMED(40, std::chrono::milliseconds(250), {
    drainPeerQueueCompletely(peerId3, 1, 50);
    drainPeerQueueCompletely(peerId4, 1, 50);
    beforeStop = snapshotPeer(kPeerAddr5);
    readyForRejoin = beforeStop.state == PeerUpdateState::DETACHED_BLOCKED &&
        beforeStop.detachedOnRegistration && beforeStop.hasChangeListConsumer &&
        beforeStop.peerRibVersion != 0 &&
        beforeStop.peerRibVersion == beforeStop.groupRibVersion;
    EXPECT_EVENTUALLY_TRUE(readyForRejoin);
  });
  ASSERT_TRUE(readyForRejoin)
      << "state=" << static_cast<int>(beforeStop.state)
      << " detachedOnRegistration=" << beforeStop.detachedOnRegistration
      << " hasConsumer=" << beforeStop.hasChangeListConsumer
      << " peerVersion=" << beforeStop.peerRibVersion
      << " groupVersion=" << beforeStop.groupRibVersion;

  beginPeerSessionTermination(kPeerAddr5);
  peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});

  auto afterAdjRibStop = snapshotPeer(kPeerAddr5);
  EXPECT_EQ(PeerUpdateState::DETACHED_INIT_DUMP, afterAdjRibStop.state);
  EXPECT_EQ(0, afterAdjRibStop.timesRejoined);

  completePeerSessionTermination(kPeerAddr5);
  EXPECT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSessionTeardownCancellationTest,
    ::testing::Values(kNoSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace facebook::bgp
