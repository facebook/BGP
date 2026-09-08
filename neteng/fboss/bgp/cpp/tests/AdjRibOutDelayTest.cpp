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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define AdjRib_TEST_FRIENDS                                  \
  friend class AdjRibOutboundFixture;                        \
  FRIEND_TEST(AdjRibOutboundFixture, OutDelayTest);          \
  FRIEND_TEST(AdjRibOutboundFixture, OutDelayTimerTest);     \
  FRIEND_TEST(AdjRibOutboundFixture, OutDelayStaggeredTest); \
  FRIEND_TEST(AdjRibOutboundFixture, ImplicitWithdrawOutDelayTest);

#include <folly/Benchmark.h>
#include <folly/coro/BlockingWait.h>

#include <folly/init/Init.h>
#include "neteng/fboss/bgp/cpp/common/Types.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"

using namespace facebook::nettools::bgplib;

namespace facebook::bgp {

using namespace ::testing;

void AdjRibOutboundFixture::setupOutDelayAdjs(
    std::vector<std::tuple<
        std::shared_ptr<AdjRib::AdjRibInQueueT>&,
        std::shared_ptr<AdjRib::AdjRibOutQueueT>&,
        std::shared_ptr<AdjRib::BoundedAdjRibOutQueueT>&,
        std::shared_ptr<AdjRib>&>>&& peerInfo) {
  if (peerInfo.size() > 4 || peerInfo.size() < 1) {
    throw std::runtime_error("Invalid number of peer info passed.");
  }
  std::shared_ptr<PolicyManager> policyManager;

  PeeringParams peeringParamsInPeer{
      folly::IPAddress("90.1.1.1"),
      folly::IPAddress::createNetwork("90.1.1.0/24"),
      1001 /* global AS */,
      1001 /* local AS */,
      2001 /* remote AS (eBGP) */,
      folly::IPAddress("90.1.1.1").asV4(),
      30s /* holdtime */,
      std::nullopt /* no GR */,
      AdvertiseLinkBandwidth::DISABLE};

  PeeringParams peeringParamsOutPeer1{
      folly::IPAddress("10.1.1.1"),
      folly::IPAddress::createNetwork("10.1.1.0/24"),
      1001 /* global AS */,
      1001 /* local AS */,
      1001 /* remote AS (iBGP) */,
      folly::IPAddress("10.1.1.1").asV4(),
      30s /* holdtime */,
      std::nullopt /* no GR */,
      AdvertiseLinkBandwidth::DISABLE};

  PeeringParams peeringParamsOutPeer2{
      folly::IPAddress("20.1.1.1"),
      folly::IPAddress::createNetwork("20.1.1.0/24"),
      1001 /* global AS */,
      1001 /* local AS */,
      1001 /* remote AS (iBGP) */,
      folly::IPAddress("20.1.1.1").asV4(),
      30s /* holdtime */,
      std::nullopt /* no GR */,
      AdvertiseLinkBandwidth::DISABLE};

  PeeringParams peeringParamsOutPeer3{
      folly::IPAddress("30.1.1.1"),
      folly::IPAddress::createNetwork("30.1.1.0/24"),
      1001 /* global AS */,
      1001 /* local AS */,
      1001 /* remote AS (iBGP) */,
      folly::IPAddress("30.1.1.1").asV4(),
      30s /* holdtime */,
      std::nullopt /* no GR */,
      AdvertiseLinkBandwidth::DISABLE};

  PeeringParams peeringParamsOutPeer4{
      folly::IPAddress("40.1.1.1"),
      folly::IPAddress::createNetwork("40.1.1.0/24"),
      1001 /* global AS */,
      1001 /* local AS */,
      1001 /* remote AS (iBGP) */,
      folly::IPAddress("40.1.1.1").asV4(),
      30s /* holdtime */,
      std::nullopt /* no GR */,
      AdvertiseLinkBandwidth::DISABLE};

  // Peer1 has out-delay timer set to 1sec
  auto& adjRibPeer1InQ = std::get<0>(peerInfo[0]);
  auto& adjRibPeer1OutQ = std::get<1>(peerInfo[0]);
  auto& adjRibBoundedPeer1OutQ = std::get<2>(peerInfo[0]);
  auto& adjRibOutPeer1 = std::get<3>(peerInfo[0]);

  auto adjRibOutGroup1 = std::make_shared<AdjRibOutGroup>(evb_, "Group1");
  adjRibOutPeer1 = std::make_shared<AdjRib>(
      BgpPeerId(
          peeringParamsOutPeer1.peerAddr,
          peeringParamsOutPeer1.peerAddr.asV4().toLongHBO()),
      peeringParamsOutPeer1,
      evb_,
      ribInQ_,
      observerQ_,
      std::make_shared<folly::coro::Baton>(),
      policyManager,
      std::make_shared<std::atomic<bool>>(false) /* isSafeModeOn */,
      std::nullopt,
      std::nullopt,
      adjRibOutGroup1,
      1s);

  fm_->addTask([&] {
    adjRibOutPeer1->sessionEstablished(
        std::nullopt,
        adjRibPeer1InQ,
        adjRibPeer1OutQ,
        adjRibBoundedPeer1OutQ,
        AfiIpv4Negotiated(true),
        AfiIpv6Negotiated(false));
    adjRibOutPeer1->startMessageProcessingLoop();
  });

  if (peerInfo.size() == 1) {
    return;
  }

  // Peer-2 has out-delay 2sec
  auto& adjRibPeer2InQ = std::get<0>(peerInfo[1]);
  auto& adjRibPeer2OutQ = std::get<1>(peerInfo[1]);
  auto& adjRibBoundedPeer2OutQ = std::get<2>(peerInfo[1]);
  auto& adjRibOutPeer2 = std::get<3>(peerInfo[1]);

  auto adjRibOutGroup2 = std::make_shared<AdjRibOutGroup>(evb_, "Group2");
  adjRibOutPeer2 = std::make_shared<AdjRib>(
      BgpPeerId(
          peeringParamsOutPeer2.peerAddr,
          peeringParamsOutPeer2.peerAddr.asV4().toLongHBO()),
      peeringParamsOutPeer2,
      evb_,
      ribInQ_,
      observerQ_,
      std::make_shared<folly::coro::Baton>(),
      policyManager,
      std::make_shared<std::atomic<bool>>(false) /* isSafeModeOn */,
      std::nullopt,
      std::nullopt,
      adjRibOutGroup2,
      2s);
  fm_->addTask([&] {
    adjRibOutPeer2->sessionEstablished(
        std::nullopt,
        adjRibPeer2InQ,
        adjRibPeer2OutQ,
        adjRibBoundedPeer2OutQ,
        AfiIpv4Negotiated(true),
        AfiIpv6Negotiated(false));
    adjRibOutPeer2->startMessageProcessingLoop();
  });

  if (peerInfo.size() == 2) {
    return;
  }

  // Peer-3 has no out-delay
  auto& adjRibPeer3InQ = std::get<0>(peerInfo[2]);
  auto& adjRibPeer3OutQ = std::get<1>(peerInfo[2]);
  auto& adjRibBoundedPeer3OutQ = std::get<2>(peerInfo[2]);
  auto& adjRibOutPeer3 = std::get<3>(peerInfo[2]);

  auto adjRibOutGroup3 = std::make_shared<AdjRibOutGroup>(evb_, "Group3");
  adjRibOutPeer3 = std::make_shared<AdjRib>(
      BgpPeerId(
          peeringParamsOutPeer3.peerAddr,
          peeringParamsOutPeer3.peerAddr.asV4().toLongHBO()),
      peeringParamsOutPeer3,
      evb_,
      ribInQ_,
      observerQ_,
      std::make_shared<folly::coro::Baton>(),
      policyManager,
      std::make_shared<std::atomic<bool>>(false) /* isSafeModeOn */,
      std::nullopt,
      std::nullopt,
      adjRibOutGroup3,
      0s);
  fm_->addTask([&] {
    adjRibOutPeer3->sessionEstablished(
        std::nullopt,
        adjRibPeer3InQ,
        adjRibPeer3OutQ,
        adjRibBoundedPeer3OutQ,
        AfiIpv4Negotiated(true),
        AfiIpv6Negotiated(false));
    adjRibOutPeer3->startMessageProcessingLoop();
  });

  if (peerInfo.size() == 3) {
    return;
  }

  // Peer-4 has 1sec out-delay
  auto& adjRibPeer4InQ = std::get<0>(peerInfo[3]);
  auto& adjRibPeer4OutQ = std::get<1>(peerInfo[3]);
  auto& adjRibBoundedPeer4OutQ = std::get<2>(peerInfo[3]);
  auto& adjRibOutPeer4 = std::get<3>(peerInfo[3]);

  auto adjRibOutGroup4 = std::make_shared<AdjRibOutGroup>(evb_, "Group4");
  adjRibOutPeer4 = std::make_shared<AdjRib>(
      BgpPeerId(
          peeringParamsOutPeer4.peerAddr,
          peeringParamsOutPeer4.peerAddr.asV4().toLongHBO()),
      peeringParamsOutPeer4,
      evb_,
      ribInQ_,
      observerQ_,
      std::make_shared<folly::coro::Baton>(),
      policyManager,
      std::make_shared<std::atomic<bool>>(false) /* isSafeModeOn */,
      std::nullopt,
      std::nullopt,
      adjRibOutGroup4,
      1s);
  fm_->addTask([&] {
    adjRibOutPeer4->sessionEstablished(
        std::nullopt,
        adjRibPeer4InQ,
        adjRibPeer4OutQ,
        adjRibBoundedPeer4OutQ,
        AfiIpv4Negotiated(true),
        AfiIpv6Negotiated(false));
    adjRibOutPeer4->startMessageProcessingLoop();
  });
}

/*
 * Out Delay Feature Test:
 *  create one incoming adjrib
 *  create 4 outgoing adjrib:
 *    * 1st with out-delay set to 1sec
 *    * 2nd with out-delay set to 2sec
 *    * 3rd with out-delay set to 0sec
 *    * 4th with out-delay set to 1sec <This peer joins ~2.5sec after others>
 *
 *  Simulate sending RIB In update from RIB carrying 2 prefixes pfx1, pfx2
 *  After .5sec send an update on pfx1 and an update of pfx2
 *  After 1.5sec send an update on pfx1 and a withdraw of pfx2
 *
 *  [Deferral] verify that peer1 gets pfx1 twice and pfx2 update followed by
 *  withdraw [Update Coalescing] verify that peer2 gets pfx1 once and no pfx2
 *  verify that peer3 gets pfx1 update three times, 2 pfx2 updates followed by
 *  withdrawal
 *
 *  Redeferral Test:: [Update->Withdraw->Update]
 *  After 2sec send a new update pfx2 [With NewTimeStamp] to peer1,2,3
 *  verify that peer3 get pfx2 update right away,
 *  verify that peer1 gets pfx2 after 1 sec and peer3 gets after 2 sec
 *
 *  Peer join after prefix installed in Local-RIB.
 *  After 2.5sec send update to peer4 <peer4 has just joined>
 *  Verify that peer4 defers the pfx2 update but receives pfx1 rightaway
 *  After 3.5sec verify that peer4 gets pfx2 also.
 */
TEST_F(AdjRibOutboundFixture, OutDelayTest) {
  auto adjRibPeer1InQ = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto adjRibPeer1OutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();
  auto adjRibPeer2InQ = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto adjRibPeer2OutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();
  auto adjRibPeer3InQ = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto adjRibPeer3OutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();
  auto adjRibPeer4InQ = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto adjRibPeer4OutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();

  auto adjRibBoundedPeer1OutQ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          10 /* capacity */, 8 /* highWm */, 2 /* lowWm */);
  auto adjRibBoundedPeer2OutQ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          10 /* capacity */, 8 /* highWm */, 2 /* lowWm */);
  auto adjRibBoundedPeer3OutQ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          10 /* capacity */, 8 /* highWm */, 2 /* lowWm */);
  auto adjRibBoundedPeer4OutQ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          10 /* capacity */, 8 /* highWm */, 2 /* lowWm */);

  std::shared_ptr<AdjRib> adjRibOutPeer1;
  std::shared_ptr<AdjRib> adjRibOutPeer2;
  std::shared_ptr<AdjRib> adjRibOutPeer3;
  std::shared_ptr<AdjRib> adjRibOutPeer4;

  TinyPeerInfo inPeer{
      folly::IPAddress("90.1.1.1"),
      1001,
      folly::IPAddress("90.1.1.1").asV4().toLong(),
      BgpSessionType::EBGP,
      false};

  setupOutDelayAdjs(
      {std::make_tuple(
           std::ref(adjRibPeer1InQ),
           std::ref(adjRibPeer1OutQ),
           std::ref(adjRibBoundedPeer1OutQ),
           std::ref(adjRibOutPeer1)),
       std::make_tuple(
           std::ref(adjRibPeer2InQ),
           std::ref(adjRibPeer2OutQ),
           std::ref(adjRibBoundedPeer2OutQ),
           std::ref(adjRibOutPeer2)),
       std::make_tuple(
           std::ref(adjRibPeer3InQ),
           std::ref(adjRibPeer3OutQ),
           std::ref(adjRibBoundedPeer3OutQ),
           std::ref(adjRibOutPeer3)),
       std::make_tuple(
           std::ref(adjRibPeer4InQ),
           std::ref(adjRibPeer4OutQ),
           std::ref(adjRibBoundedPeer4OutQ),
           std::ref(adjRibOutPeer4))});

  // Peer-1 has out-delay 1sec
  auto verifyUpdates = [](const FiberBgpPeer::InputMessageT& input,
                          const folly::IPAddress& nh,
                          bool pfx1Present = true,
                          bool pfx2Present = true) {
    bool matched = false;
    folly::variant_match(
        input,
        [&](std::shared_ptr<const BgpUpdate2> update) mutable {
          EXPECT_EQ(0, update->v4Withdrawn2()->size());
          if (pfx1Present && pfx2Present) {
            EXPECT_EQ(2, update->mpAnnounced()->prefixes()->size());
            std::vector<RiggedIPPrefix> pfxList = {
                RiggedIPPrefix(), RiggedIPPrefix()};
            pfxList[0].prefix() = network::toIPPrefix(kV4Prefix1);
            pfxList[1].prefix() = network::toIPPrefix(kV4Prefix2);
            EXPECT_THAT(
                pfxList,
                testing::UnorderedElementsAreArray(
                    *update->mpAnnounced()->prefixes()));
          } else if (pfx1Present) {
            EXPECT_EQ(1, update->mpAnnounced()->prefixes()->size());
            EXPECT_EQ(
                network::toIPPrefix(kV4Prefix1),
                *update->mpAnnounced()->prefixes()[0].prefix());
          } else {
            EXPECT_EQ(1, update->mpAnnounced()->prefixes()->size());
            EXPECT_EQ(
                network::toIPPrefix(kV4Prefix2),
                *update->mpAnnounced()->prefixes()[0].prefix());
          }
          EXPECT_EQ(nh.str(), *update->attrs()->nexthop());
          matched = true;
        },
        [&](const UpdateDescriptor& /* unused */) {
          FAIL() << "Unexpected UpdateDescriptor received";
        },
        [&](const BgpEndOfRib& /* unused */) {
          FAIL() << "Unexpected EoR received";
        },
        [&](const BgpRouteRefresh& /* unused */) {
          FAIL() << "Unexpected RouteRefresh received";
        },
        [&](const BgpNotification& /* unused */) {
          FAIL() << "Unexpected Notification received";
        });
    EXPECT_EQ(true, matched);
  };

  auto verifyWithdrawal = [](const FiberBgpPeer::InputMessageT& input) {
    bool matched = false;
    folly::variant_match(
        input,
        [&](std::shared_ptr<const BgpUpdate2> update) mutable {
          EXPECT_EQ(0, update->mpAnnounced()->prefixes()->size());
          EXPECT_EQ(1, update->v4Withdrawn2()->size());
          EXPECT_EQ(
              network::toIPPrefix(kV4Prefix2),
              *update->v4Withdrawn2()[0].prefix());
          matched = true;
        },
        [&](const UpdateDescriptor& /* unused */) {
          FAIL() << "Unexpected UpdateDescriptor received";
        },
        [&](const BgpEndOfRib& /* unused */) {
          FAIL() << "Unexpected EoR received";
        },
        [&](const BgpRouteRefresh& /* unused */) {
          FAIL() << "Unexpected RouteRefresh received";
        },
        [&](const BgpNotification& /* unused */) {
          FAIL() << "Unexpected Notification received";
        });
    EXPECT_EQ(true, matched);
  };

  /*
   * Send an update to all adjs.
   * if withdrawPfx2 is false - we send prefix1 and prefix2 in the update with
   * appropriate nh else we send prefix1 in the update and send another
   * withdrawal for prefix2
   */
  auto sendUpdates =
      [&](const folly::IPAddress& nh,
          std::chrono::time_point<std::chrono::system_clock> timeStamp,
          bool sendPfx1 = true,
          bool newRibEntry = false,
          bool withdrawPfx2 = false) mutable {
        BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(nh);
        auto inAttrs = std::make_shared<facebook::bgp::BgpPath>(
            BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
        inAttrs->publish();
        RibOutAnnouncement announcement;
        if (sendPfx1) {
          RibOutAnnouncementEntry update1{
              kV4Prefix1,
              kDefaultPathID,
              inPeer,
              inAttrs,
              std::nullopt, // switchId
              std::nullopt, // multiPathSize
              std::nullopt, // aggregate received
              std::nullopt, // aggregate local
              std::nullopt, // rib policy lbw
              newRibEntry,
              timeStamp};
          announcement.entries.push_back(update1);
        }
        if (!withdrawPfx2) {
          RibOutAnnouncementEntry update2{
              kV4Prefix2,
              kDefaultPathID,
              inPeer,
              inAttrs,
              std::nullopt, // switchId
              std::nullopt, // multiPathSize
              std::nullopt, // aggregate received
              std::nullopt, // aggregate local
              std::nullopt, // rib policy lbw
              newRibEntry,
              timeStamp};
          announcement.entries.push_back(update2);
        }
        adjRibOutPeer1->processRibMessage(RibOutMessage(announcement));
        adjRibOutPeer2->processRibMessage(RibOutMessage(announcement));
        adjRibOutPeer3->processRibMessage(RibOutMessage(announcement));

        if (withdrawPfx2) {
          RibOutWithdrawal withdrawal;
          withdrawal.entries.emplace_back(kV4Prefix2, kDefaultPathID);
          adjRibOutPeer1->processRibMessage(RibOutMessage(withdrawal));
          adjRibOutPeer2->processRibMessage(RibOutMessage(withdrawal));
          adjRibOutPeer3->processRibMessage(RibOutMessage(withdrawal));
        }
      };

  fm_->addTask([&]() {
    fiberSleepFor(50ms);
    adjRibOutPeer1->egressEoRsSent_ = true;
    adjRibOutPeer2->egressEoRsSent_ = true;
    adjRibOutPeer3->egressEoRsSent_ = true;
    adjRibOutPeer4->egressEoRsSent_ = true;

    auto installTimeStamp1 = std::chrono::system_clock::now();
    // Send an update from RIB side.
    sendUpdates(kV4Nexthop1, installTimeStamp1, true, true);
    fiberSleepFor(50ms);
    /*
     * check both peer1 and peer2 have not sent anything yet
     * check peer3 has pfx1 and pfx2 sent out
     */
    {
      EXPECT_EQ(adjRibOutPeer1->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer2->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer3OutQ->size(), 1);
      auto msg = folly::coro::blockingWait(adjRibPeer3OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop1);
    }
    // After .5sec send an update on pfx1 and pfx2 with nexthop changed
    fiberSleepFor(450ms);
    // Send an update from RIB side.
    sendUpdates(kV4Nexthop2, installTimeStamp1, true);
    fiberSleepFor(50ms);
    /*
     * check both peer1 and peer2 has not sent anything yet
     * check peer3 has pfx1 and pfx2 sent out
     */
    {
      EXPECT_EQ(adjRibOutPeer1->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer2->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer3OutQ->size(), 1);
      auto msg = folly::coro::blockingWait(adjRibPeer3OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop2);
    }

    // wait for peer1's out-delay timer to expire
    fiberSleepFor(550ms);
    /*
     * Verify that peer1 has a consolidated update sent out for pfx1 and pfx2
     * pointing to nexthop2. Also verify that peer2 still has no updates sent
     * out and peer3 has no new updates sent out.
     */
    {
      EXPECT_EQ(adjRibOutPeer3->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer2->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer1OutQ->size(), 1);
      auto msg = folly::coro::blockingWait(adjRibPeer1OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop2);
    }

    //  After .5sec send an update on pfx1 and a withdraw of pfx2
    fiberSleepFor(500ms);
    sendUpdates(kV4Nexthop3, installTimeStamp1, true, false, true);
    fiberSleepFor(50ms);

    /*
     * verify that peer1 gets update of pfx1 and withdraw without any delay
     * same goes for peer3
     * for peer2 we still doesn't get anything.
     */
    {
      EXPECT_EQ(adjRibOutPeer2->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer1OutQ->size(), 2);
      auto msg = folly::coro::blockingWait(adjRibPeer1OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop3, true, false);
      msg = folly::coro::blockingWait(adjRibPeer1OutQ->pop());
      verifyWithdrawal(*msg);

      EXPECT_EQ(adjRibPeer3OutQ->size(), 2);
      msg = folly::coro::blockingWait(adjRibPeer3OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop3, true, false);
      msg = folly::coro::blockingWait(adjRibPeer3OutQ->pop());
      verifyWithdrawal(*msg);
    }

    /*
     * wait another 0.5sec for peer2's out-delay to expire
     * we should see only peer2 has pfx1 send out with updates nexthop and
     * pfx2 should never be sent out from peer2.
     * also peer1 and peer3 should have no new udpates/withdrawals.
     */
    fiberSleepFor(550ms);
    {
      EXPECT_EQ(adjRibOutPeer1->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer3->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer2OutQ->size(), 1);
      auto msg = folly::coro::blockingWait(adjRibPeer2OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop3, true, false);
    }
    /*
     *  After 2sec send a new update pfx2 [With NewTimeStamp] to peer1,2,3
     *  verify that peer1 gets pfx2 after 1 sec and peer3 gets after 2 sec
     */
    fiberSleepFor(500ms);
    auto installTimeStamp2 = std::chrono::system_clock::now();
    sendUpdates(kV4Nexthop3, installTimeStamp2, false, true, false);
    fiberSleepFor(50ms);
    //  verify that peer3 get pfx2 update right away,
    {
      EXPECT_EQ(adjRibOutPeer1->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer2->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer3OutQ->size(), 1);
      auto msg = folly::coro::blockingWait(adjRibPeer3OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop3, false);
    }
    //  After 2.5sec send update to peer4 <peer4 has just joined>
    fiberSleepFor(500ms);
    {
      BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop3);
      auto inAttrs = std::make_shared<facebook::bgp::BgpPath>(
          BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
      inAttrs->publish();
      RibOutAnnouncement announcement;
      RibOutAnnouncementEntry update1{
          kV4Prefix1,
          kDefaultPathID,
          inPeer,
          inAttrs,
          std::nullopt, // switchId
          std::nullopt, // multiPathSize
          std::nullopt, // aggregate received
          std::nullopt, // aggregate local
          std::nullopt, // rib policy lbw
          false,
          installTimeStamp1};
      announcement.entries.push_back(update1);
      RibOutAnnouncementEntry update2{
          kV4Prefix2,
          kDefaultPathID,
          inPeer,
          inAttrs,
          std::nullopt, // switchId
          std::nullopt, // multiPathSize
          std::nullopt, // aggregate received
          std::nullopt, // aggregate local
          std::nullopt, // rib policy lbw
          false,
          installTimeStamp2};
      announcement.entries.push_back(update2);
      adjRibOutPeer4->processRibMessage(RibOutMessage(announcement));
    }
    fiberSleepFor(50ms);
    //  Verify that peer4 defers the pfx2 update but receives pfx1 rightaway
    {
      EXPECT_EQ(adjRibOutPeer1->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer2->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer3->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer4OutQ->size(), 1);
      auto msg = folly::coro::blockingWait(adjRibPeer4OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop3, true, false);
    }
    fiberSleepFor(550ms);
    // After 3sec verify that peer1 gets pfx2 after its deferred timeout
    {
      EXPECT_EQ(adjRibOutPeer2->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer3->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer4->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer1OutQ->size(), 1);
      auto msg = folly::coro::blockingWait(adjRibPeer1OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop3, false, true);
    }
    fiberSleepFor(550ms);
    // After 3.5sec verify that peer4 gets pfx2 after its deferred timeout
    {
      EXPECT_EQ(adjRibOutPeer1->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer2->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibOutPeer3->adjRibOutQueue_->empty(), true);
      EXPECT_EQ(adjRibPeer4OutQ->size(), 1);
      auto msg = folly::coro::blockingWait(adjRibPeer4OutQ->pop());
      verifyUpdates(*msg, kV4Nexthop3, false, true);
    }

    FiberBgpPeer::BgpSessionStop sessionStop;
    sessionStop.gracefulRestart = false;
    adjRibPeer1InQ->fiberPush(sessionStop);
    adjRibPeer2InQ->fiberPush(sessionStop);
    adjRibPeer3InQ->fiberPush(sessionStop);
    adjRibPeer4InQ->fiberPush(sessionStop);
    // Give the fibers a chance to proerly shutdown the sessions.
    fiberSleepFor(50ms);
  });
  evb_.loop();
}

TEST_F(AdjRibOutboundFixture, OutDelayTimerTest) {
  auto adjRibPeer1InQ = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto adjRibPeer1OutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();
  auto adjRibBoundedPeer1OutQ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          10 /* capacity */, 8 /* highWm */, 2 /* lowWm */);
  std::shared_ptr<AdjRib> adjRibOutPeer1;

  TinyPeerInfo inPeer{
      folly::IPAddress("90.1.1.1"),
      1001,
      folly::IPAddress("90.1.1.1").asV4().toLong(),
      BgpSessionType::EBGP,
      false};

  setupOutDelayAdjs({std::make_tuple(
      std::ref(adjRibPeer1InQ),
      std::ref(adjRibPeer1OutQ),
      std::ref(adjRibBoundedPeer1OutQ),
      std::ref(adjRibOutPeer1))});

  // Peer-1 has out-delay 1sec
  fm_->addTask([&]() {
    fiberSleepFor(50ms);
    adjRibOutPeer1->egressEoRsSent_ = true;

    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(kV4Nexthop1);
    auto inAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inAttrs->publish();
    RibOutAnnouncement announcement;
    RibOutAnnouncementEntry update1{
        kV4Prefix1,
        kDefaultPathID,
        inPeer,
        inAttrs,
        std::nullopt, // switchId
        std::nullopt, // multiPathSize
        std::nullopt, // aggregate received
        std::nullopt, // aggregate local
        std::nullopt, // rib policy lbw
        false,
        std::chrono::system_clock::now()};
    announcement.entries.push_back(update1);
    adjRibOutPeer1->processRibMessage(RibOutMessage(announcement));

    fiberSleepFor(1s);
    std::vector<folly::CIDRNetwork> newDeferredPrefixes = {kV4Prefix1};
    AdjRibOutDelayEntry entry(
        std::chrono::system_clock::now(), newDeferredPrefixes);

    // Add 15 items to the priority queue.
    for (int i = 0; i < 15; i++) {
      adjRibOutPeer1->outDelayPQ_.push(entry);
    }

    /*
     * Each programOutDelayTimer() call handles up to
     * 8 deferred items in the queue
     * If there is any item left in the PQ,
     * it sets up a timer that calls
     * the same function after 25 ms
     */
    adjRibOutPeer1->programOutDelayTimer();
    EXPECT_TRUE(adjRibOutPeer1->outDelayPQ_.size() > 0);
    EXPECT_TRUE(adjRibOutPeer1->outDelayTimer_->isScheduled());

    // The remaining items in the queue should be flushed out after 25 ms
    fiberSleepFor(550ms);
    EXPECT_EQ(adjRibOutPeer1->outDelayPQ_.size(), 0);
    EXPECT_FALSE(adjRibOutPeer1->outDelayTimer_->isScheduled());

    FiberBgpPeer::BgpSessionStop sessionStop;
    sessionStop.gracefulRestart = false;
    adjRibPeer1InQ->fiberPush(sessionStop);
  });

  evb_.loop();
}

/**
 * Out-delay feature - Verify that we do not advertise a prefix, if
 * 1. we receive an update from RIB that results in an implicit withdraw and
 * 2. if the RIB update is received while the prefix is deferred.
 */
TEST_F(AdjRibOutboundFixture, ImplicitWithdrawOutDelayTest) {
  auto adjRibPeer1InQ = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto adjRibPeer1OutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();
  auto adjRibBoundedPeer1OutQ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          10 /* capacity */, 8 /* highWm */, 2 /* lowWm */);
  std::shared_ptr<AdjRib> adjRibOutPeer1;

  // Peer from where we receive the first route.
  TinyPeerInfo inPeer{
      folly::IPAddress("90.1.1.1"),
      1001,
      folly::IPAddress("90.1.1.1").asV4().toLong(),
      BgpSessionType::EBGP,
      false};
  /*
   * Peer from where we received the 2nd route. This is also the
   * peer to which the first route is being advertised.
   */
  TinyPeerInfo outPeer{
      folly::IPAddress("10.1.1.1"),
      1001,
      folly::IPAddress("10.1.1.1").asV4().toLong(),
      BgpSessionType::EBGP,
      false};

  // The adjRib being set inside for peer1OutQ is the same peer as outPeer.
  setupOutDelayAdjs({std::make_tuple(
      std::ref(adjRibPeer1InQ),
      std::ref(adjRibPeer1OutQ),
      std::ref(adjRibBoundedPeer1OutQ),
      std::ref(adjRibOutPeer1))});

  /*
   * Peer-1 has out-delay of 1 sec.
   * Send a RIB update to AdjRib of peer1/outPeer.
   */
  auto sendUpdates = [&](const folly::IPAddress& nh,
                         bool newRibEntry = false,
                         bool recvFromInPeer = true) mutable {
    BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(nh);
    auto inAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
    inAttrs->publish();
    RibOutAnnouncement announcement;
    RibOutAnnouncementEntry update1{
        kV4Prefix1,
        kDefaultPathID,
        recvFromInPeer ? inPeer : outPeer,
        inAttrs,
        std::nullopt, // switchId
        std::nullopt, // multiPathSize
        std::nullopt,
        std::nullopt,
        std::nullopt,
        newRibEntry};

    // Check if we're permitted to announce this route.
    auto canAnnounce = adjRibOutPeer1->canAnnounce(update1);
    // Only a route coming from a inPeer can be announced to outPeer.
    EXPECT_EQ(canAnnounce, recvFromInPeer);
    announcement.entries.push_back(update1);
    adjRibOutPeer1->processRibMessage(RibOutMessage(announcement));
  };

  fm_->addTask([&]() {
    // Mark eor sent as True or RIB message is ignored by AdjRib.
    adjRibOutPeer1->egressEoRsSent_ = true;
    // Send a RIB update to outPeer, triggered by a route received from inPeer.
    sendUpdates(kV4Nexthop1, true, true);

    {
      // Due to out-delay of 1 sec, peer hasn't computed an update.
      EXPECT_EQ(0, adjRibPeer1OutQ->size());
      /*
       * Send a RIB update to outPeer, triggered by a route received from
       * outPeer. This should lead to implicit withdraw.
       */
      sendUpdates(kV4Nexthop1, false, false);
      // Wait for out-delay to expire.
      fiberSleepFor(1000ms);
      // We should not see any update created, due to implicit withdraw.
      EXPECT_EQ(0, adjRibPeer1OutQ->size());
    }

    // Terminate the session.
    FiberBgpPeer::BgpSessionStop sessionStop;
    sessionStop.gracefulRestart = false;
    adjRibPeer1InQ->fiberPush(sessionStop);
    // Give the fibers a chance to proerly shutdown the session.
    fiberSleepFor(50ms);
  });
  evb_.loop();
}

/*
 * Out-delay feature : Staggered Update Test
 * Ensure that we do not delay initial dump.  Ensure that if we learn a new
 * prefix shortly after sending the initial dump, this prefix is delayed.
 */
TEST_F(AdjRibOutboundFixture, OutDelayStaggeredTest) {
  auto adjRibPeer1InQ = std::make_shared<AdjRib::AdjRibInQueueT>();
  auto adjRibPeer1OutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();
  auto adjRibBoundedPeer1OutQ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          10 /* capacity */, 8 /* highWm */, 2 /* lowWm */);
  std::shared_ptr<AdjRib> adjRibOutPeer1;
  TinyPeerInfo inPeer{
      folly::IPAddress("90.1.1.1"),
      1001,
      folly::IPAddress("90.1.1.1").asV4().toLong(),
      BgpSessionType::EBGP,
      false};
  setupOutDelayAdjs({std::make_tuple(
      std::ref(adjRibPeer1InQ),
      std::ref(adjRibPeer1OutQ),
      std::ref(adjRibBoundedPeer1OutQ),
      std::ref(adjRibOutPeer1))});
  /*
   * Peer-1 has out-delay 1sec
   * Send an update to all adjs.
   * sendWithEoR: set the EoR flag in the announcement
   */
  auto sendUpdates =
      [&](const folly::IPAddress& nh,
          const std::vector<
              std::pair<folly::CIDRNetwork, bool /*newRibEntry*/>>& prefixes,
          bool sendWithEoR,
          bool initialDump = true) mutable {
        BgpUpdate2 inputUpdate = buildBgpUpdateAttributes(nh);
        auto inAttrs = std::make_shared<facebook::bgp::BgpPath>(
            BgpPathFields(*BgpUpdate2toBgpPathC(inputUpdate)));
        inAttrs->publish();
        RibOutAnnouncement announcement;
        for (const auto& p : prefixes) {
          RibOutAnnouncementEntry update{
              p.first,
              kDefaultPathID,
              inPeer,
              inAttrs,
              std::nullopt, // switchId
              std::nullopt, // multiPathSize
              std::nullopt,
              std::nullopt,
              std::nullopt,
              p.second};
          announcement.entries.push_back(update);
        }
        announcement.sendWithEoR = sendWithEoR;
        announcement.initialDump = initialDump;
        adjRibOutPeer1->processRibMessage(RibOutMessage(announcement));
      };

  fm_->addTask([&]() {
    // send pfx1 update with initialDump = true, eor = true
    sendUpdates(
        kV4Nexthop1,
        std::vector{std::make_pair(kV4Prefix1, true /*newRibEntry*/)},
        true /*eor*/,
        true /*initialDump*/);
    auto initialUpdate = std::chrono::system_clock::now();
    // send pfx2 update with initialDump = false
    sendUpdates(
        kV4Nexthop1,
        std::vector{std::make_pair(kV4Prefix2, true /*newRibEntry*/)},
        false /*eor*/,
        false /*initialDump*/);

    /*
     * Make sure we did not delay initial dump (pfx1), but we did delay
     * the second update (pfx2).
     */
    {
      auto msg = folly::coro::blockingWait(adjRibPeer1OutQ->pop());
      EXPECT_TRUE(
          std::holds_alternative<std::shared_ptr<const BgpUpdate2>>(*msg));
      auto update = std::get<std::shared_ptr<const BgpUpdate2>>(*msg);
      EXPECT_EQ(1, update->mpAnnounced()->prefixes()->size());
      EXPECT_EQ(
          network::toIPPrefix(kV4Prefix1),
          *update->mpAnnounced()->prefixes()[0].prefix());

      // Verify v4 EoR
      msg = folly::coro::blockingWait(adjRibPeer1OutQ->pop());
      ASSERT_TRUE(std::holds_alternative<BgpEndOfRib>(*msg));
      auto bgpEndOfRib = std::get<BgpEndOfRib>(*msg);
      EXPECT_EQ(false, *bgpEndOfRib.isMpEor());
      EXPECT_EQ(BgpUpdateAfi::AFI_IPv4, *bgpEndOfRib.afi());
      EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *bgpEndOfRib.safi());
      EXPECT_EQ(0, adjRibPeer1OutQ->size());
    }

    // sleep for 500ms before sending additional updates for pfx2 and pfx3
    fiberSleepFor(500ms);
    auto additionalUpdate = std::chrono::system_clock::now();
    // Now update pfx2 nexthop = kV4Nexthop2
    sendUpdates(
        kV4Nexthop2,
        std::vector{
            std::make_pair(kV4Prefix2, false /*newRibEntry*/),
            std::make_pair(kV4Prefix3, true /*newRibEntry*/)},
        false /*eor*/,
        false /*initialDump*/);

    // After 1sec verify that peer1 received pfx2 with new nexthop
    {
      auto msg = folly::coro::blockingWait(adjRibPeer1OutQ->pop());
      auto current = std::chrono::system_clock::now();
      // delayed 1s from initial dump
      EXPECT_LT(1s, current - initialUpdate);
      // but pfx2 should not have additional delay after second dump
      EXPECT_GT(1s, current - additionalUpdate);
      ASSERT_TRUE(
          std::holds_alternative<std::shared_ptr<const BgpUpdate2>>(*msg));
      auto update = std::get<std::shared_ptr<const BgpUpdate2>>(*msg);
      EXPECT_EQ(1, update->mpAnnounced()->prefixes()->size());
      EXPECT_EQ(
          network::toIPPrefix(kV4Prefix2),
          *update->mpAnnounced()->prefixes()[0].prefix());
      EXPECT_EQ(kV4Nexthop2.str(), *update->attrs()->nexthop());
    }

    // After 1.5sec verify that peer1 received pfx3 with new nexthop
    {
      auto msg = folly::coro::blockingWait(adjRibPeer1OutQ->pop());
      auto current = std::chrono::system_clock::now();
      // second update should only contain prefix3, prefix2 has been send out
      EXPECT_LT(1s, current - additionalUpdate);
      ASSERT_TRUE(
          std::holds_alternative<std::shared_ptr<const BgpUpdate2>>(*msg));
      auto update = std::get<std::shared_ptr<const BgpUpdate2>>(*msg);
      EXPECT_EQ(1, update->mpAnnounced()->prefixes()->size());
      EXPECT_EQ(
          network::toIPPrefix(kV4Prefix3),
          *update->mpAnnounced()->prefixes()[0].prefix());
      EXPECT_EQ(kV4Nexthop2.str(), *update->attrs()->nexthop());
    }

    FiberBgpPeer::BgpSessionStop sessionStop;
    sessionStop.gracefulRestart = false;
    adjRibPeer1InQ->fiberPush(sessionStop);
    // Give the fibers a chance to proerly shutdown the sessions.
    fiberSleepFor(50ms);
  });
  evb_.loop();
}

} // namespace facebook::bgp

int main(int argc, char** argv) {
  // Parse command line flags
  testing::InitGoogleTest(&argc, argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  // // google::InitGoogleLogging(argv[0]);
  google::InstallFailureSignalHandler();

  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  // Run unit tests
  return RUN_ALL_TESTS();
}
