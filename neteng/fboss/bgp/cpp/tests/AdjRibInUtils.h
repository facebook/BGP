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

#pragma once
#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/fibers/EventBaseLoopController.h>
#include <folly/fibers/FiberManagerMap.h>
#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"

using facebook::nettools::bgplib::BgpAttrAsPathSegment;
using facebook::nettools::bgplib::BgpAttrCommunity;
using facebook::nettools::bgplib::BgpAttrExtCommunity;
using facebook::nettools::bgplib::BgpAttrOrigin;
using facebook::nettools::bgplib::BgpUpdate2;
using facebook::nettools::bgplib::BgpUpdateAfi;
using facebook::nettools::bgplib::BgpUpdateSafi;
using facebook::nettools::bgplib::FiberBgpPeer;
using facebook::nettools::bgplib::getFiberManagerOptions;
using facebook::nettools::bgplib::RiggedIPPrefix;
using facebook::nettools::bgplib::RWQueue;
using facebook::nettools::bgplib::constants::kBgpPort;
using facebook::network::toBinaryAddress;
using facebook::network::toIPPrefix;
using folly::fibers::EventBaseLoopController;
using folly::fibers::FiberManager;

DECLARE_bool(enable_egress_backpressure_in_adjribin_tests);

namespace facebook::bgp {

// BGP update with prefilled attributes and single v4 announced prefix
std::shared_ptr<BgpUpdate2> createV4BgpUpdateSingleAnnounce(
    const folly::CIDRNetwork& prefix = kV4Prefix1,
    const folly::IPAddress& nexthop = kV4Nexthop1,
    const uint32_t med = kMed,
    const uint32_t originatorId = kOriginatorId,
    const BgpAttrOrigin& origin = BgpAttrOrigin::BGP_ORIGIN_IGP,
    const uint32_t pathId = kDefaultPathID,
    const bool confed = false);

// BGP update with single withdraw
std::shared_ptr<BgpUpdate2> createV4BgpUpdateSingleWithdraw(
    const folly::CIDRNetwork& prefix = kV4Prefix1,
    const uint32_t pathId = kDefaultPathID);

// BGP update with prefilled attributes and multiple v4 announced prefix
std::shared_ptr<BgpUpdate2> createV4BgpUpdateMultipleAnnounce(
    const std::vector<folly::CIDRNetwork>& vec,
    const BgpAttrOrigin& origin = BgpAttrOrigin::BGP_ORIGIN_IGP,
    const int64_t lbwRawByte = kExtCommLbwTypeSecondWord10G,
    const uint32_t asNum = kAsSeqAsNum);

// BGP update with prefilled attributes and single v6 announced prefix
std::shared_ptr<BgpUpdate2> createV6BgpUpdateSingleAnnounce(
    const folly::CIDRNetwork& prefix,
    const folly::IPAddress& nexthop = kV6Nexthop1);

// BGP update with prefilled attributes and multiple v6 announced prefix
std::shared_ptr<BgpUpdate2> createV6BgpUpdateMultipleAnnounce(
    const std::vector<folly::CIDRNetwork>& vec);

// BGP update with single withdraw
std::shared_ptr<BgpUpdate2> createV6BgpUpdateSingleWithdraw(
    const folly::CIDRNetwork& prefix);

// Creates BgpUpdate2 with community
std::shared_ptr<BgpUpdate2> createV4AndV6BgpUpdateSingleAnnounce(
    const folly::CIDRNetwork& prefix,
    const std::vector<BgpAttrCommunityC>& communities);

// Creates BGPupdate2 with a mix of v4 and v6 networks
std::shared_ptr<BgpUpdate2> createV4AndV6BgpUpdateMultipleAnnounce(
    const std::vector<folly::CIDRNetwork>& vec);

std::shared_ptr<BgpUpdate2> createV4BgpUpdateWithAsLoop(
    const folly::CIDRNetwork& prefix = kV4Prefix1,
    const folly::IPAddress& nexthop = kV4Nexthop1,
    const uint32_t asn = kLocalAs1,
    const bool confed = false);

std::shared_ptr<BgpUpdate2> createV6BgpUpdateWithAsLoop(
    const folly::CIDRNetwork& prefix = kV6Prefix1,
    const folly::IPAddress& nexthop = kV6Nexthop1,
    const uint32_t asn = kLocalAs1,
    const bool confed = false);

std::shared_ptr<BgpUpdate2> createBgpUpdateWithAsLoop(
    std::shared_ptr<BgpUpdate2>& update,
    const uint32_t asn = kLocalAs1,
    const bool confed = false);

std::shared_ptr<BgpUpdate2> createV4BgpUpdateWithAsSetLoop(
    const folly::CIDRNetwork& prefix = kV4Prefix1,
    const folly::IPAddress& nexthop = kV4Nexthop1,
    const uint32_t asn = kLocalAs1,
    const bool confed = false);

// Generic BGP update announcement with flexible parameters
std::shared_ptr<BgpUpdate2> createBgpUpdateAnnouncement(
    bool isV4,
    const folly::CIDRNetwork& cidr,
    const std::string& nexthop,
    const std::vector<uint32_t>& asPathSeq = {},
    const std::vector<std::string>& communities = {},
    uint32_t addPathId = 0,
    uint32_t localPref = kLocalPref,
    uint32_t med = 0,
    std::optional<float> linkBandwidthBps = std::nullopt);

// Generic BGP update withdrawal with flexible parameters
std::shared_ptr<BgpUpdate2> createBgpUpdateWithdrawal(
    bool isV4,
    const folly::CIDRNetwork& cidr,
    uint32_t addPathId = 0);

//
// The fixture provides fiber manager and evb_ for the tests
//
class AdjRibInboundFixture : public ::testing::Test {
 public:
  AdjRibInboundFixture() = default;
  ~AdjRibInboundFixture() override = default;

  void SetUp() override {
    // Register Singleton
    folly::SingletonVault::singleton()->registrationComplete();
    AdjRibPolicyCache::get()->clearCache();
    AdjRibPolicyCache::get()->resetTotalCacheHit();
    AdjRibPolicyCache::get()->resetTotalCacheMiss();

    fm_ = std::make_unique<FiberManager>(
        std::make_unique<EventBaseLoopController>(), getFiberManagerOptions());

    static_cast<EventBaseLoopController&>(fm_->loopController())
        .attachEventBase(evb_);
  }

  // util function to terminate the running adjRib
  void terminateAdjRib(bool gracefulRestartFlag = false) {
    adjRibInQ_->fiberPush(
        nettools::bgplib::FiberBgpPeer::BgpSessionStop{gracefulRestartFlag});
  }

  void TestBody() override {}

  void setupAdjRib(
      const std::chrono::seconds& localGrRestartTime = kShortGrRestartTime,
      const std::optional<std::chrono::seconds>& remoteGrRestartTime =
          std::nullopt,
      const bool callSessionEstablished = true,
      const uint32_t globalAs = kLocalAs1,
      const uint32_t localAs = kLocalAs1,
      const uint32_t configuredRemoteAs = kRemoteAs1,
      const AfiIpv4Negotiated& isAfiIpv4Negotiated = AfiIpv4Negotiated(true),
      const AfiIpv6Negotiated& isAfiIpv6Negotiated = AfiIpv6Negotiated(true),
      const std::shared_ptr<PolicyManager>& policy = nullptr,
      const std::optional<std::string>& ingressPolicyName = std::nullopt,
      const bool isConfedPeer = kIsConfedPeerFalse,
      const std::optional<uint32_t>& localConfedAs = std::nullopt,
      const std::optional<uint32_t>& asConfedId = std::nullopt,
      const AdvertiseLinkBandwidth& advertiseLinkBandwidth =
          AdvertiseLinkBandwidth::DISABLE,
      const ReceiveLinkBandwidth& receiveLinkBandwidth =
          ReceiveLinkBandwidth::ACCEPT,
      const std::optional<float>& linkBandwidthBps = std::nullopt,
      ValidateRemoteAs validateRemoteAs = ValidateRemoteAs{true},
      const uint32_t maxRoutes = kDefaultPreMaxRoutes,
      const bool warningOnly = false,
      const uint8_t warningLimit = kDefaultPreWarningThreshold,
      const uint32_t maxAcceptedRoutes = kDefaultPostMaxRoutes,
      const bool acptWarningOnly = false,
      const uint8_t acptWarningLimit = kDefaultPostWarningThreshold,
      const std::optional<nettools::bgplib::BgpPeerId>& peerId = kPeerId1,
      const IsRedistributePeer isRedistributePeer = IsRedistributePeer{false},
      std::shared_ptr<std::atomic<bool>> isSafeModeOn =
          std::make_shared<std::atomic<bool>>(false),
      const bool enforce_first_as = false,
      const std::optional<uint32_t>& remoteAs = std::nullopt);

  // IBGP setup for policy testing
  void setupAdjRib(
      const std::shared_ptr<PolicyManager>& policy,
      const std::optional<std::string>& ingressPolicyName);

  // Stats testing
  void setupAdjRib(const nettools::bgplib::BgpPeerId& peerId);

  // UCMP testing
  void setupAdjRib(
      const std::optional<float>& linkBandwidthBps,
      const ReceiveLinkBandwidth& receiveLBW = ReceiveLinkBandwidth::DISABLE,
      const std::shared_ptr<PolicyManager>& policy = nullptr,
      const std::optional<std::string>& ingressPolicyName = std::nullopt);

  // redistribute peer
  void setupAdjRibForRedistributePeer();

  // A flavor to return adjrib shared_ptr
  std::shared_ptr<AdjRib> setupAdjRib(
      const nettools::bgplib::BgpPeerId& peerId,
      const PeeringParams& params);

  void establishSession(
      const std::optional<std::chrono::seconds>& remoteGrRestartTime =
          std::nullopt,
      const AfiIpv4Negotiated& isAfiIpv4Negotiated = AfiIpv4Negotiated(true),
      const AfiIpv6Negotiated& isAfiIpv6Negotiated = AfiIpv6Negotiated(true),
      const std::optional<uint32_t>& remoteAs = std::nullopt);

  // Re-establish session from fiber context, mimicking
  // PeerManagerBase::sessionEstablished flow by properly awaiting
  // ensureAsyncScopeInitialized() before calling sessionEstablished()
  // and startMessageProcessingLoop().
  void reEstablishSession(
      const std::optional<std::chrono::seconds>& remoteGrRestartTime =
          std::nullopt,
      const AfiIpv4Negotiated& isAfiIpv4Negotiated = AfiIpv4Negotiated(true),
      const AfiIpv6Negotiated& isAfiIpv6Negotiated = AfiIpv6Negotiated(true),
      const std::optional<uint32_t>& remoteAs = std::nullopt);

  std::unique_ptr<FiberManager> fm_;
  folly::EventBase evb_;
  folly::coro::CancellableAsyncScope asyncScope_;
  std::shared_ptr<AdjRib> adjRib_;
  std::shared_ptr<const Config> config_{nullptr};

  std::shared_ptr<AdjRib::AdjRibInQueueT> adjRibInQ_ =
      std::make_shared<AdjRib::AdjRibInQueueT>(
          nettools::bgplib::kMaxIngressQueueSize);
  std::shared_ptr<AdjRib::AdjRibOutQueueT> adjRibOutQ_ =
      std::make_shared<AdjRib::AdjRibOutQueueT>();

  std::shared_ptr<AdjRib::BoundedAdjRibOutQueueT> boundedAdjRibOutQ_ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
          nettools::bgplib::kMaxEgressQueueSize,
          nettools::bgplib::kEgressQueueHighWatermark,
          nettools::bgplib::kEgressQueueLowWatermark);

  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_{
      nettools::bgplib::kMaxIngressQueueSize};
  MonitoredMPMCQueue<AdjRib::ObservableMessageT> fromAdjRibQ_;
};

/**
 * The tests in this suite check the behavior within
 * AdjRibIn::processPeerAnnounced.
 */
class AdjRibProcessPeerAnnouncedFixture : public AdjRibInboundFixture {
 public:
  void SetUp() override {
    fm_ = std::make_unique<FiberManager>(
        std::make_unique<EventBaseLoopController>(), getFiberManagerOptions());

    static_cast<EventBaseLoopController&>(fm_->loopController())
        .attachEventBase(evb_);
    setupAdjRib(
        kShortGrRestartTime,
        std::nullopt /* remoteGrRestartTime */,
        false /* callSessionEstablished */);
  }

  // Prefixes to be withdrawn from Rib at the end of processPeerAnnounced.
  PrefixPathIds withdrawnPfxPathIds_;
  // Prefixes to be announced to Rib at the end of processPeerAnnounced.
  folly::F14NodeMap<std::shared_ptr<const BgpPath>, PrefixPathIds>
      groupAnnouncedPrefixes_;
};
} // namespace facebook::bgp
