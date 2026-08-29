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

#include <tuple>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <folly/ExceptionString.h>
#include <folly/IPAddress.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/logging/xlog.h>

#include <folly/init/Init.h>
#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/lib/BgpMessageParser.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

#include "BgpMessageParserTestData.h"

namespace facebook {
namespace nettools {
namespace bgplib {
using facebook::network::toCIDRNetwork;
using facebook::network::toIPAddress;
using folly::IPAddress;

BgpCapabilities capabilities;
BgpCapabilities capabilitiesWithAddPath;
BgpCapabilities capabilitiesExtNhEncoding;
BgpCapabilities capabilitiesTwoByteAs;

void setDefaultCapabilities(BgpCapabilities& caps) {
  *caps.mpExtV4Unicast() = true;
  *caps.mpExtV6Unicast() = true;
  *caps.mpExtV4LU() = true;
  *caps.mpExtV6LU() = true;
  *caps.as4byte() = true;
}

void init() {
  // capabilities
  setDefaultCapabilities(capabilities);
  // 2 byte ASes
  setDefaultCapabilities(capabilitiesTwoByteAs);
  capabilitiesTwoByteAs.as4byte() = false;

  // capabilitiesWithAddPath
  setDefaultCapabilities(capabilitiesWithAddPath);
  BgpAddPathCapability capability1;
  capability1.afi() = BgpUpdateAfi::AFI_IPv4;
  capability1.safi() = BgpUpdateSafi::SAFI_UNICAST;
  capability1.sor() = BgpAddPathSendRec::RECEIVE;
  capabilitiesWithAddPath.addPathCapabilities()->push_back(
      std::move(capability1));
  BgpAddPathCapability capability2;
  capability2.afi() = BgpUpdateAfi::AFI_IPv6;
  capability2.safi() = BgpUpdateSafi::SAFI_UNICAST;
  capability2.sor() = BgpAddPathSendRec::RECEIVE;
  capabilitiesWithAddPath.addPathCapabilities()->push_back(
      std::move(capability2));
  // capabilitiesExtNhEncoding
  setDefaultCapabilities(capabilitiesExtNhEncoding);
  BgpExtNHEncodingCapability capability3;
  capability3.nlriAfi() = BgpUpdateAfi::AFI_IPv4;
  capability3.nlriSafi() = BgpUpdateSafi::SAFI_UNICAST;
  capability3.nhAfi() = BgpUpdateAfi::AFI_IPv6;
  capabilitiesExtNhEncoding.extNHEncodingCapabilities()->push_back(
      std::move(capability3));
  BgpExtNHEncodingCapability capability4;
  capability4.nlriAfi() = BgpUpdateAfi::AFI_IPv4;
  capability4.nlriSafi() = BgpUpdateSafi::SAFI_LABELED_UNICAST;
  capability4.nhAfi() = BgpUpdateAfi::AFI_IPv6;
  capabilitiesExtNhEncoding.extNHEncodingCapabilities()->push_back(
      std::move(capability4));
}

TEST(BgpMessageParser2, BgpUpdateWithdrawV4Attr) {
  std::vector<uint8_t> msg = kBgpUpdateWithdrawV4AttrMsg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));

  ASSERT_EQ(1, update->v4Withdrawn()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("9.8.7.6/32"),
      toCIDRNetwork(update->v4Withdrawn()[0]));
  ASSERT_EQ(1, update->v4Announced()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("6.5.4.3/32"),
      toCIDRNetwork(update->v4Announced()[0]));
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_IGP, *update->attrs()->origin());

  ASSERT_EQ(1, update->attrs()->asPath()->size());
  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(0, path.asSet()->size());
    EXPECT_EQ(1, path.asSequence()->size());
    ASSERT_TRUE(!path.asSequence()->empty());
    EXPECT_EQ(32934, path.asSequence()[0]);
  }

  EXPECT_EQ(folly::IPAddress("1.2.3.4"), toIPAddress(*update->v4Nexthop()));
  EXPECT_EQ(32, *update->attrs()->med());
  ASSERT_TRUE(update->attrs()->localPref().has_value());
  EXPECT_EQ(100, *update->attrs()->localPref());
  EXPECT_TRUE(*update->attrs()->atomicAggregate());
  EXPECT_EQ(4660, *update->attrs()->aggregator()->asn());
  // TODO: Get rig of all use of strings for IP addresses
  EXPECT_EQ("3.4.5.6", *update->attrs()->aggregator()->ip());

  ASSERT_EQ(1, update->attrs()->communities()->size());
  EXPECT_EQ(65530, *update->attrs()->communities()[0].asn());
  EXPECT_EQ(15800, *update->attrs()->communities()[0].value());

  EXPECT_EQ(0x12030000, *update->attrs()->originatorId());
  ASSERT_EQ(2, update->attrs()->clusterList()->size());

  EXPECT_EQ(0x10010000, update->attrs()->clusterList()[0]);
  EXPECT_EQ(0x86070000, update->attrs()->clusterList()[1]);

  ASSERT_EQ(1, update->attrs()->extCommunities()->size());

  auto extCommunity = update->attrs()->extCommunities()[0];
  EXPECT_EQ(0x2272a, *extCommunity.firstWord());
  EXPECT_EQ(0x232f, *extCommunity.secondWord());
}

TEST(BgpMessageParser2, BgpUpdateWithdrawV6Attr) {
  auto msg = kBgpUpdateWithdrawV6AttrMsg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));

  ASSERT_EQ(1, update->mpWithdrawn()->prefixes()->size());
  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *update->mpWithdrawn()->afi());
  EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *update->mpWithdrawn()->safi());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("face:b00c::3000/122"),
      toCIDRNetwork(*update->mpWithdrawn()->prefixes()[0].prefix()));

  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *update->mpAnnounced()->afi());
  EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *update->mpAnnounced()->safi());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("fd00::3000/122"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[0].prefix()));
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());

  ASSERT_EQ(1, update->attrs()->asPath()->size());
  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(1, path.asSet()->size());
    EXPECT_EQ(0, path.asSequence()->size());
    EXPECT_NE(path.asSet()->end(), path.asSet()->find(32934));
  }

  EXPECT_EQ(
      folly::IPAddress("fd00::1"),
      toIPAddress(*update->mpAnnounced()->nexthop()));
  EXPECT_EQ(32, *update->attrs()->med());
  ASSERT_TRUE(update->attrs()->localPref().has_value());
  EXPECT_EQ(100, *update->attrs()->localPref());
  EXPECT_FALSE(*update->attrs()->atomicAggregate());

  ASSERT_EQ(1, update->attrs()->communities()->size());
  EXPECT_EQ(65530, *update->attrs()->communities()[0].asn());
  EXPECT_EQ(15800, *update->attrs()->communities()[0].value());
  EXPECT_EQ(0x86070000, *update->attrs()->originatorId());
}

TEST(BgpMessageParser2, BgpUpdateWithdrawV6Only) {
  auto msg = kBgpUpdateWithdrawV6OnlyMsg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));

  ASSERT_EQ(1, update->mpWithdrawn()->prefixes()->size());
  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *update->mpWithdrawn()->afi());
  EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *update->mpWithdrawn()->safi());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("face:b00c::3000/122"),
      toCIDRNetwork(*update->mpWithdrawn()->prefixes()[0].prefix()));
}

TEST(BgpMessageParser2, BgpUpdateWithdrawV4V6) {
  auto msg = kBgpUpdateWithdrawV4V6Msg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));

  ASSERT_EQ(1, update->v4Withdrawn()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("1.2.0.0/16"),
      toCIDRNetwork(update->v4Withdrawn()[0]));

  ASSERT_EQ(1, update->mpWithdrawn()->prefixes()->size());
  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *update->mpWithdrawn()->afi());
  EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *update->mpWithdrawn()->safi());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("face:b00c::3000/122"),
      toCIDRNetwork(*update->mpWithdrawn()->prefixes()[0].prefix()));

  ASSERT_EQ(1, update->mpAnnounced()->prefixes()->size());
  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *update->mpAnnounced()->afi());
  EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *update->mpAnnounced()->safi());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("fd00::3000/122"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[0].prefix()));

  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());
  EXPECT_EQ(1, update->attrs()->asPath()->size());

  ASSERT_EQ(1, update->attrs()->asPath()->size());
  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(1, path.asSet()->size());
    EXPECT_EQ(0, path.asSequence()->size());
    EXPECT_NE(path.asSet()->end(), path.asSet()->find(32934));
  }

  EXPECT_EQ(
      toIPAddress(*update->mpAnnounced()->nexthop()),
      folly::IPAddress("fd00::1"));
  ASSERT_TRUE(update->attrs()->localPref().has_value());
  EXPECT_EQ(100, *update->attrs()->localPref());

  ASSERT_EQ(1, update->v4Announced()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("4.5.0.0/16"),
      toCIDRNetwork(update->v4Announced()[0]));
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());
  ASSERT_EQ(1, update->attrs()->asPath()->size());
  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(1, path.asSet()->size());
    EXPECT_EQ(0, path.asSequence()->size());
    EXPECT_NE(path.asSet()->end(), path.asSet()->find(32934));
  }
  EXPECT_EQ(folly::IPAddress("7.6.5.4"), toIPAddress(*update->v4Nexthop()));
  ASSERT_TRUE(update->attrs()->localPref().has_value());
  EXPECT_EQ(100, *update->attrs()->localPref());
}

TEST(BgpMessageParser2, BgpUpdateWithdrawV4WithLabels) {
  auto msg = kBgpUpdateWithdrawV4WithLabelsMsg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));

  EXPECT_EQ(BgpUpdateAfi::AFI_IPv4, *update->mpWithdrawn()->afi());
  EXPECT_EQ(
      BgpUpdateSafi::SAFI_LABELED_UNICAST, *update->mpWithdrawn()->safi());
  ASSERT_EQ(1, update->mpWithdrawn()->prefixes()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("4.5.6.0/24"),
      toCIDRNetwork(*update->mpWithdrawn()->prefixes()[0].prefix()));

  ASSERT_EQ(2, update->mpWithdrawn()->prefixes()[0].labels()->size());
  EXPECT_EQ(8, update->mpWithdrawn()->prefixes()[0].labels()[0]);
  EXPECT_EQ(9, update->mpWithdrawn()->prefixes()[0].labels()[1]);

  EXPECT_EQ(BgpUpdateAfi::AFI_IPv4, *update->mpAnnounced()->afi());
  EXPECT_EQ(
      BgpUpdateSafi::SAFI_LABELED_UNICAST, *update->mpAnnounced()->safi());
  ASSERT_EQ(1, update->mpAnnounced()->prefixes()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("4.5.6.0/23"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[0].prefix()));
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());
  ASSERT_EQ(2, update->mpAnnounced()->prefixes()[0].labels()->size());
  EXPECT_EQ(5, update->mpAnnounced()->prefixes()[0].labels()[0]);
  EXPECT_EQ(4, update->mpAnnounced()->prefixes()[0].labels()[1]);

  ASSERT_EQ(1, update->attrs()->asPath()->size());
  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(1, path.asSet()->size());
    EXPECT_EQ(0, path.asSequence()->size());
    EXPECT_TRUE(path.asSet()->find(32934) != path.asSet()->end());
  }

  EXPECT_EQ(
      folly::IPAddress("1.2.3.4"),
      toIPAddress(*update->mpAnnounced()->nexthop()));

  ASSERT_TRUE(update->attrs()->localPref().has_value());
  EXPECT_EQ(100, *update->attrs()->localPref());
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());
}

TEST(BgpMessageParser2, BgpUpdateWithdrawV6WithLabels) {
  auto msg = kBgpUpdateWithdrawV6WithLabelsMsg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));

  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *update->mpWithdrawn()->afi());
  EXPECT_EQ(
      BgpUpdateSafi::SAFI_LABELED_UNICAST, *update->mpWithdrawn()->safi());
  ASSERT_EQ(1, update->mpWithdrawn()->prefixes()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("abcd:ef00::/24"),
      toCIDRNetwork(*update->mpWithdrawn()->prefixes()[0].prefix()));

  ASSERT_EQ(2, update->mpWithdrawn()->prefixes()[0].labels()->size());
  EXPECT_EQ(8, update->mpWithdrawn()->prefixes()[0].labels()[0]);
  EXPECT_EQ(9, update->mpWithdrawn()->prefixes()[0].labels()[1]);

  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *update->mpAnnounced()->afi());
  EXPECT_EQ(
      BgpUpdateSafi::SAFI_LABELED_UNICAST, *update->mpAnnounced()->safi());
  ASSERT_EQ(1, update->mpAnnounced()->prefixes()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("dead::/23"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[0].prefix()));
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());
  ASSERT_EQ(2, update->mpAnnounced()->prefixes()[0].labels()->size());
  EXPECT_EQ(5, update->mpAnnounced()->prefixes()[0].labels()[0]);
  EXPECT_EQ(4, update->mpAnnounced()->prefixes()[0].labels()[1]);

  ASSERT_EQ(1, update->attrs()->asPath()->size());
  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(1, path.asSet()->size());
    EXPECT_EQ(0, path.asSequence()->size());
    EXPECT_TRUE(path.asSet()->find(32934) != path.asSet()->end());
  }

  EXPECT_EQ(
      folly::IPAddress("fd00::1"),
      toIPAddress(*update->mpAnnounced()->nexthop()));

  ASSERT_TRUE(update->attrs()->localPref().has_value());
  EXPECT_EQ(100, *update->attrs()->localPref());
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());
}

TEST(BgpMessageParser, BgpUpdateV4PathId) {
  auto msg = kBgpUpdateV4PathIdMsg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilitiesWithAddPath));

  EXPECT_EQ(
      folly::IPAddress("10.127.254.1"), toIPAddress(*update->v4Nexthop()));

  EXPECT_EQ(1, update->v4Announced2()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("4.5.0.0/16"),
      toCIDRNetwork(*update->v4Announced2()[0].prefix()));
  EXPECT_EQ(26, *update->v4Announced2()[0].pathId());
}

TEST(BgpMessageParser, BgpUpdateV4NexthopV6) {
  auto msg = kBgpUpdateV4NexthopV6Msg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilitiesExtNhEncoding));

  EXPECT_EQ(BgpUpdateAfi::AFI_IPv4, *update->mpAnnounced()->afi());
  EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *update->mpAnnounced()->safi());

  ASSERT_EQ(1, update->mpAnnounced()->prefixes()->size());

  EXPECT_EQ(
      folly::IPAddress::createNetwork("4.5.0.0/16"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[0].prefix()));

  ASSERT_EQ(1, update->attrs()->asPath()->size());

  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(0, path.asSet()->size());
    EXPECT_EQ(1, path.asSequence()->size());

    ASSERT_EQ(1, path.asSequence()->size());
    EXPECT_EQ(65001, path.asSequence()[0]);
  }

  EXPECT_EQ(
      folly::IPAddress("2001:db8::1"),
      toIPAddress(*update->mpAnnounced()->nexthop()));
  EXPECT_EQ(0, *update->attrs()->med());

  EXPECT_FALSE(update->attrs()->localPref().has_value());

  EXPECT_THROW(
      {
        try {
          auto innerBuf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
          BgpMessageParser2::parseBgpUpdateRaw(*innerBuf, capabilities);
        } catch (BgpUpdateMsgException& e) {
          EXPECT_EQ(
              BgpNotifUpdateMsgErrSubCode::BN_UM_OPTIONAL_ATTR_ERROR,
              e.getSubCode());
          throw;
        }
      },
      BgpUpdateMsgException);
}

TEST(BgpMessageParser2, BgpUpdateV4WithLabelsNexthopV6) {
  auto msg = kBgpUpdateV4WithLabelsNexthopV6Msg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilitiesExtNhEncoding));

  EXPECT_EQ(BgpUpdateAfi::AFI_IPv4, *update->mpAnnounced()->afi());
  EXPECT_EQ(
      BgpUpdateSafi::SAFI_LABELED_UNICAST, *update->mpAnnounced()->safi());
  ASSERT_EQ(1, update->mpAnnounced()->prefixes()->size());
  EXPECT_EQ(
      folly::IPAddress::createNetwork("4.5.6.0/23"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[0].prefix()));
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());
  ASSERT_EQ(2, update->mpAnnounced()->prefixes()[0].labels()->size());
  EXPECT_EQ(5, update->mpAnnounced()->prefixes()[0].labels()[0]);
  EXPECT_EQ(4, update->mpAnnounced()->prefixes()[0].labels()[1]);

  ASSERT_EQ(1, update->attrs()->asPath()->size());
  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(1, path.asSet()->size());
    EXPECT_EQ(0, path.asSequence()->size());
    EXPECT_TRUE(path.asSet()->find(32934) != path.asSet()->end());
  }

  EXPECT_EQ(
      folly::IPAddress("2001:db8::1"),
      toIPAddress(*update->mpAnnounced()->nexthop()));

  ASSERT_TRUE(update->attrs()->localPref().has_value());
  EXPECT_EQ(100, *update->attrs()->localPref());
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *update->attrs()->origin());

  EXPECT_THROW(
      {
        try {
          auto innerBuf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
          BgpMessageParser2::parseBgpUpdateRaw(*innerBuf, capabilities);
        } catch (BgpUpdateMsgException& e) {
          EXPECT_EQ(
              BgpNotifUpdateMsgErrSubCode::BN_UM_OPTIONAL_ATTR_ERROR,
              e.getSubCode());
          throw;
        }
      },
      BgpUpdateMsgException);
}

TEST(BgpMessageParser, BgpUpdateV6Nexthop32) {
  auto msg = kBgpUpdateV6Nexthop32Msg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));

  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *update->mpAnnounced()->afi());
  EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *update->mpAnnounced()->safi());

  ASSERT_EQ(3, update->mpAnnounced()->prefixes()->size());

  EXPECT_EQ(
      folly::IPAddress::createNetwork("2001:db8:1:2::/64"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[0].prefix()));
  EXPECT_EQ(
      folly::IPAddress::createNetwork("2001:db8:1:1::/64"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[1].prefix()));
  EXPECT_EQ(
      folly::IPAddress::createNetwork("2001:db8:1::/64"),
      toCIDRNetwork(*update->mpAnnounced()->prefixes()[2].prefix()));
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_IGP, *update->attrs()->origin());

  ASSERT_EQ(1, update->attrs()->asPath()->size());

  {
    auto path = update->attrs()->asPath()[0];
    EXPECT_EQ(0, path.asSet()->size());
    EXPECT_EQ(1, path.asSequence()->size());

    ASSERT_EQ(1, path.asSequence()->size());
    EXPECT_EQ(65001, path.asSequence()[0]);
  }

  EXPECT_EQ(
      folly::IPAddress("2001:db8::1"),
      toIPAddress(*update->mpAnnounced()->nexthop()));
  EXPECT_EQ(0, *update->attrs()->med());

  EXPECT_FALSE(update->attrs()->localPref().has_value());
}

TEST(BgpMessageParser2, BgpUpdateMessageWithoutLocalPref) {
  auto msg = kBgpUpdateMessageWithoutLocalPref;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));
  EXPECT_FALSE(update->attrs()->localPref().has_value());
}

TEST(BgpMessageParser2, BgpUpdateMessageWithoutMed) {
  auto msg = kBgpUpdateMessageWithoutMed;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));
  EXPECT_FALSE(update->attrs()->isMedSet().value());
}

TEST(BgpMessageParser, BgpUpdateV4EOR) {
  auto msg = kBgpUpdateV4EORMsg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto eor = std::get<BgpEndOfRib>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));
  EXPECT_FALSE(*eor.isMpEor());
}

TEST(BgpMessageParser, BgpUpdateV6EOR) {
  auto msg = kBgpUpdateV6EORMsg;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto eor = std::get<BgpEndOfRib>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));

  EXPECT_TRUE(*eor.isMpEor());
  EXPECT_EQ(BgpUpdateAfi::AFI_IPv6, *eor.afi());
  EXPECT_EQ(BgpUpdateSafi::SAFI_UNICAST, *eor.safi());
}

//
// UPDATE message errors
//
class BgpHeaderFixtureInvalidLength
    : public BgpUpdateMessageErrorFixture,
      public testing::WithParamInterface<size_t> {};

TEST_P(BgpHeaderFixtureInvalidLength, InvalidHeaderLength) {
  const unsigned int headerlen = GetParam();

  msg[16] = (uint8_t)(headerlen >> 8);
  msg[17] = headerlen & 127;

  EXPECT_NO_THROW({
    try {
      auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities);
      ADD_FAILURE();
    } catch (BgpHeaderException const& err) {
      EXPECT_EQ(BgpNotifMsgHdrErrSubCode::BN_MH_BAD_MSG_LEN, err.getSubCode());
      EXPECT_EQ(
          std::string(reinterpret_cast<const char*>(&msg[16]), 2),
          err.getData());
    }
  });
}

/* Test for values that should fail header checks, so less than minimum
 * UPDATE size or greater than overall message size
 */
INSTANTIATE_TEST_CASE_P(
    BgpHeaderInvalidLengthValues,
    BgpHeaderFixtureInvalidLength,
    ::testing::Values(0, 1, 2, 8, 19 - 1, 19, 20, 19 + 3, 2048, 4096, 5000));

class BgpHeaderFuzzCaseInvalidLen
    : public BgpUpdateMessageErrorFixture,
      public testing::WithParamInterface<std::vector<uint8_t>> {};

class BgpParserCbStub : public BgpMessageParser2::BgpMessageParserCallbacks {
 public:
  void rcvdBgpOpenMsg(BgpOpenMsg) override {}
  void rcvdBgpNotification(BgpNotification) override {}
  void rcvdBgpKeepAlive() override {}
  void rcvdBgpUpdate(BgpUpdate2) override {}
  void rcvdBgpEndOfRib(BgpEndOfRib) override {}
  void rcvdBgpRouteRefresh(BgpRouteRefresh) override {}
  void handleBgpException(const BgpException&) override {}
  void handleBgpFsmException(const BgpFsmException&) override {}
  void handleBgpHeaderException(const BgpHeaderException&) override {}
  void handleBgpOpenMsgException(const BgpOpenMsgException&) override {}
  void handleBgpRouteRefreshMsgException(
      const BgpRouteRefreshMsgException&) override {}
  void handleBgpUpdateMsgException(const BgpUpdateMsgException&) override {}
};

class BgpParserCbStubDefaultFail : public BgpParserCbStub {
 public:
  void rcvdBgpOpenMsg(BgpOpenMsg) override {
    ADD_FAILURE();
  }
  void rcvdBgpNotification(BgpNotification) override {
    ADD_FAILURE();
  }
  void rcvdBgpKeepAlive() override {
    ADD_FAILURE();
  }
  void rcvdBgpUpdate(BgpUpdate2) override {
    ADD_FAILURE();
  }
  void rcvdBgpEndOfRib(BgpEndOfRib) override {
    ADD_FAILURE();
  }
  void handleBgpException(const BgpException&) override {
    ADD_FAILURE();
  }
  void handleBgpFsmException(const BgpFsmException&) override {
    ADD_FAILURE();
  }
  void handleBgpHeaderException(const BgpHeaderException&) override {
    ADD_FAILURE();
  }
  void handleBgpOpenMsgException(const BgpOpenMsgException&) override {
    ADD_FAILURE();
  }
  void handleBgpUpdateMsgException(const BgpUpdateMsgException&) override {
    ADD_FAILURE();
  }
};

TEST_P(BgpHeaderFuzzCaseInvalidLen, InvalidHeaderLength) {
  std::vector<uint8_t> msg = GetParam();

  EXPECT_NO_THROW({
    class tmpcb : public BgpParserCbStubDefaultFail {
     public:
      tmpcb(std::vector<uint8_t>& msg) : msg_{msg} {}
      void handleBgpHeaderException(const BgpHeaderException& err) {
        EXPECT_EQ(
            BgpNotifMsgHdrErrSubCode::BN_MH_BAD_MSG_LEN, err.getSubCode());
        EXPECT_EQ(
            std::string(reinterpret_cast<const char*>(&msg_[16]), 2),
            err.getData());
      }

     private:
      std::vector<uint8_t>& msg_;
    } tmpcb(msg);
    BgpMessageParser2::parseBgpMessage(
        &tmpcb, msg.data(), msg.size(), capabilities);
  });
}

INSTANTIATE_TEST_CASE_P(
    BgpHeaderFuzzCaseInvalidLenCases,
    BgpHeaderFuzzCaseInvalidLen,
    ::testing::Values(
        kBgpLengthInvalidLongMsg,
        kBgpLengthLongerThanMsgNoAttrs,
        kBgpNotifyShortLen));

class BgpOpenFuzzCase : public BgpHeaderFuzzCaseInvalidLen {};

TEST_P(BgpOpenFuzzCase, UnSpecificError) {
  std::vector<uint8_t> msg = GetParam();

  EXPECT_NO_THROW({
    class tmpcb : public BgpParserCbStubDefaultFail {
     public:
      void handleBgpOpenMsgException(const BgpOpenMsgException& err) {
        EXPECT_EQ(
            BgpNotifOpenMsgErrSubCode::BN_OM_UNSPECIFIC, err.getSubCode());
      }
    } tmpcb;
    BgpMessageParser2::parseBgpMessage(
        &tmpcb, msg.data(), msg.size(), capabilities);
  });
}

INSTANTIATE_TEST_CASE_P(
    BgpOpenFuzzCases,
    BgpOpenFuzzCase,
    ::testing::Values(kBgpOpenLongOptParamLenNoOpts));

TEST_F(BgpUpdateMessageErrorFixture, WrongAttrFlags) {
  // the attr flags for BGP_ATTR_ORIGIN is 0x40 (well-known mandatory)
  // here we set it as 0x80 (optional non-transitive)
  msg[26] = 0x80;
  std::vector<uint8_t> expect = {0x80, 0x01, 0x01, 0x01};
  EXPECT_THROW(
      {
        try {
          auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
          BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities);
        } catch (BgpUpdateMsgException& e) {
          EXPECT_EQ(
              BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_FLAGS_ERR,
              e.getSubCode());
          std::string expectStr(expect.begin(), expect.end());
          EXPECT_EQ(e.getData(), expectStr);
          throw;
        }
      },
      BgpUpdateMsgException);

  // the attr flags for BGP_ATTR_AS_PATH is 0x40 (well-known mandatory)
  // here we set it as 0xc0 (optional transitive)
  msg[26] = 0x40;
  msg[30] = 0xc0;
  expect = {0xc0, 0x02, 0x06, 0x01, 0x01, 0x00, 0x00, 0x80, 0xa6};
  EXPECT_THROW(
      {
        try {
          auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
          BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities);
        } catch (BgpUpdateMsgException& e) {
          EXPECT_EQ(
              BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_FLAGS_ERR,
              e.getSubCode());
          std::string expectStr(expect.begin(), expect.end());
          EXPECT_EQ(e.getData(), expectStr);
          throw;
        }
      },
      BgpUpdateMsgException);

  // the attr flags for BGP_ATTR_NEXT_HOP is 0x40 (well-known mandatory)
  // here we set it as 0x90 (optional non-transitive extended)
  msg[30] = 0x40;
  msg[39] = 0x90;
  expect = {0x90, 0x03, 0x04, 0x07, 0x06, 0x05, 0x04, 0x40, 0x05, 0x04, 0x00,
            0x00, 0x00, 0x64, 0x90, 0x0e, 0x00, 0x26, 0x00, 0x02, 0x01, 0x10,
            0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x7a, 0xfd, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30,
            0x00, 0x90, 0x0f, 0x00, 0x14, 0x00, 0x02, 0x01, 0x7a, 0xfa, 0xce,
            0xb0, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x30, 0x00, 0x40, 0x06, 0x00, 0xc0, 0x07, 0x08, 0x00, 0x00,
            0x12, 0x34, 0x03, 0x04, 0x05, 0x06};
  EXPECT_THROW(
      {
        try {
          auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
          BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities);
        } catch (BgpUpdateMsgException& e) {
          EXPECT_EQ(
              BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_FLAGS_ERR,
              e.getSubCode());
          std::string expectStr(expect.begin(), expect.end());
          EXPECT_EQ(e.getData(), expectStr);
          throw;
        }
      },
      BgpUpdateMsgException);

  // the attr flags for BGP_ATTR_LOCAL_PREF is 0x40 (well-known
  // mandatory) here we set it as 0xd0 (optional transitive extended)
  msg[39] = 0x40;
  msg[46] = 0xd0;
  expect = {0xd0, 0x05, 0x04, 0x00, 0x00, 0x00, 0x64, 0x90, 0x0e, 0x00, 0x26,
            0x00, 0x02, 0x01, 0x10, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x7a,
            0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x30, 0x00, 0x90, 0x0f, 0x00, 0x14, 0x00, 0x02,
            0x01, 0x7a, 0xfa, 0xce, 0xb0, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00, 0x40, 0x06, 0x00, 0xc0,
            0x07, 0x08, 0x00, 0x00, 0x12, 0x34, 0x03, 0x04, 0x05, 0x06};
  EXPECT_THROW(
      {
        try {
          auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
          BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities);
        } catch (BgpUpdateMsgException& e) {
          EXPECT_EQ(
              BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_FLAGS_ERR,
              e.getSubCode());
          std::string expectStr(expect.begin(), expect.end());
          EXPECT_EQ(e.getData(), expectStr);
          throw;
        }
      },
      BgpUpdateMsgException);

  // the attr flags for BGP_ATTR_MP_REACH_NLRI is 0x90
  // (optional non-transitive extended), here we set it as 0xd0
  // (optional transitive extended)
  msg[46] = 0x40;
  msg[53] = 0xd0;
  expect = {0xd0, 0x0e, 0x00, 0x26, 0x00, 0x02, 0x01, 0x10, 0xfd, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x01, 0x00, 0x7a, 0xfd, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x00};
  EXPECT_THROW(
      {
        try {
          auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
          BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities);
        } catch (BgpUpdateMsgException& e) {
          EXPECT_EQ(
              BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_FLAGS_ERR,
              e.getSubCode());
          std::string expectStr(expect.begin(), expect.end());
          EXPECT_EQ(e.getData(), expectStr);
          throw;
        }
      },
      BgpUpdateMsgException);

  // the attr flags for BGP_ATTR_AGGREGATOR is 0xc0
  // (optional transitive without partial bit), here we set it as 0xe0
  // (optional transitive with partial bit)
  msg[53] = 0x90;
  msg[122] = 0xe0;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());
  auto update = std::get<std::shared_ptr<const BgpUpdate2>>(
      BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities));
  ASSERT_EQ(4660, *update->attrs()->aggregator()->asn());
  ASSERT_EQ("3.4.5.6", *update->attrs()->aggregator()->ip());
}

TEST_F(BgpUpdateMessageErrorFixture, TruncatedMessage) {
  try {
    // truncate the size of the message
    auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size() - 6);
    BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities);

    ADD_FAILURE();
  } catch (BgpHeaderException const& err) {
    EXPECT_EQ(BgpNotifMsgHdrErrSubCode::BN_MH_BAD_MSG_LEN, err.getSubCode());
  } catch (std::exception const& err) {
    XLOGF(ERR, "Unexpected exception: {}", folly::exceptionStr(err));
    ADD_FAILURE();
  }
}

class BgpUpdateMessageAsEncodingFixture
    : public BgpUpdateMessageErrorFixture,
      public testing::WithParamInterface<std::tuple<
          std::vector<uint8_t>,
          uint8_t /* AS encoding, 2 or 4 bytes */>> {};

TEST_P(BgpUpdateMessageAsEncodingFixture, BgpUpdateMessageAsEncoding) {
  const auto param = GetParam();
  std::vector<uint8_t> msg;
  int aslen;

  std::tie(msg, aslen) = param;

  BgpCapabilities& correctCaps =
      (aslen == 2 ? capabilitiesTwoByteAs : capabilities);
  BgpCapabilities& wrongCaps =
      (aslen == 2 ? capabilities : capabilitiesTwoByteAs);

  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());

  /* Parse with the correct encoding, should work */
  try {
    XLOGF(
        INFO,
        "Parse with correct encoding ({} bytes, as4cap: {})",
        aslen,
        *correctCaps.as4byte());
    BgpMessageParser2::parseBgpUpdateRaw(*buf, correctCaps);
  } catch (std::exception const& err) {
    XLOGF(ERR, "Unexpected exception: {}", folly::exceptionStr(err));
    FAIL();
  }
  /* And try the wrong way, should fail of course. */
  try {
    XLOGF(
        INFO,
        "Parse with incorrect encoding (as4 cap: {})",
        *correctCaps.as4byte());
    BgpMessageParser2::parseBgpUpdateRaw(*buf, wrongCaps);
    FAIL();
  } catch (BgpUpdateMsgException const& err) {
    EXPECT_EQ(
        BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_LEN_ERR, err.getSubCode());
  } catch (std::exception const& err) {
    XLOGF(ERR, "Unexpected exception: {}", folly::exceptionStr(err));
    FAIL();
  }
}

INSTANTIATE_TEST_CASE_P(
    BgpUpdateMessageAsEncodingTests,
    BgpUpdateMessageAsEncodingFixture,
    ::testing::Values(
        /* 0 */ std::make_tuple(kUpdateMessage2ByteAs, 2),
        /* 1 */ std::make_tuple(kUpdateMessage4ByteAs, 4)));

class BgpUpdateMessageAttrsFixture
    : public BgpUpdateMessageErrorFixture,
      public testing::WithParamInterface<std::tuple<
          std::vector<uint8_t>,
          std::vector<uint8_t>,
          bgplib::BgpNotifUpdateMsgErrSubCode>> {};

TEST_P(BgpUpdateMessageAttrsFixture, BgpUpdateMessageAttrs) {
  const auto param = GetParam();
  std::vector<uint8_t> msg;
  std::vector<uint8_t> expect;
  BgpNotifUpdateMsgErrSubCode subcode;

  std::tie(msg, expect, subcode) = param;
  auto buf = folly::IOBuf::wrapBuffer(msg.data(), msg.size());

  try {
    BgpMessageParser2::parseBgpUpdateRaw(*buf, capabilities);
    ADD_FAILURE();
  } catch (BgpUpdateMsgException const& err) {
    EXPECT_EQ(subcode, err.getSubCode());
    std::string expectStr(expect.begin(), expect.end());
    EXPECT_EQ(err.getData(), expectStr);
  } catch (std::exception const& err) {
    XLOGF(ERR, "Unexpected exception: {}", folly::exceptionStr(err));
    ADD_FAILURE();
  }
}

INSTANTIATE_TEST_CASE_P(
    BgpUpdateMessageAttrsTests,
    BgpUpdateMessageAttrsFixture,
    ::testing::Values(
        /* 0 */
        std::make_tuple(
            kBgpUpdateMessageWithInvalidNumberOfAsns,
            std::vector<
                uint8_t>{0x40, 0x02, 0x06, 0x01, 0x02, 0x00, 0x00, 0x80, 0xa6},
            BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_LEN_ERR),
        /* 1 */
        std::make_tuple(
            kBgpUpdateMessageWithInvalidAsPathAttrsLength,
            std::vector<uint8_t>{0x40, 0x02, 0x01, 0x01},
            BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_LEN_ERR),
        /* 2 */
        std::make_tuple(
            kBgpUpdateMessageMissingNextHopV4Msg,
            std::vector<uint8_t>{
                static_cast<uint8_t>(BgpAttrCode::BGP_ATTR_NEXT_HOP)},
            BgpNotifUpdateMsgErrSubCode::BN_UM_MISSING_WELL_KNOWN_ATTR),
        /* 3 */
        std::make_tuple(
            kBgpUpdateMessageInvalidAttrLen,
            /* Used to be a test for BGP-LS, but that's been removed
             * Should throw now because of unsupported AFI for BGP-LS.
             */
            std::vector<uint8_t>{
                0x8a, 0x0f, 0x43, // MP_UNREACH_NLRI, 67 byte len
                0x40, 0x04, // Address family identifier: AFI_LS
                0x00, // Subsequent address family identifier
                0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x01, 0x00, 0x15, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x01, 0x00, 0x10, 0x14, 0x00, 0x00, 0x00,
                0x00, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f,
                0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f,
                0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f, 0x0f,
                0x0f, 0x0f, 0x0f, 0x0f,
            },
            BgpNotifUpdateMsgErrSubCode::BN_UM_OPTIONAL_ATTR_ERROR),
        /* 4 */
        std::make_tuple(
            kBgpUpdateMessageMissingAsPathAttrMsg,
            std::vector<uint8_t>{
                static_cast<uint8_t>(BgpAttrCode::BGP_ATTR_AS_PATH)},
            BgpNotifUpdateMsgErrSubCode::BN_UM_MISSING_WELL_KNOWN_ATTR),
        /* 5 */
        std::make_tuple(
            kBgpUpdateMessageMissingOriginAttrMsg,
            std::vector<uint8_t>{
                static_cast<uint8_t>(BgpAttrCode::BGP_ATTR_ORIGIN)},
            BgpNotifUpdateMsgErrSubCode::BN_UM_MISSING_WELL_KNOWN_ATTR),
        /* 6 */
        std::make_tuple(
            kBgpLengthOriginatorIdExtraJunk,
            std::vector<uint8_t>{
                0x86,
                0x09,
                0x0a,
                0x03,
                0x41,
                0x00,
                0x00,
                0x00,
                0x00,
                0x00,
                0x80,
                0xff,
                0xff},
            BgpNotifUpdateMsgErrSubCode::BN_UM_ATTR_LEN_ERR)));

} // namespace bgplib
} // namespace nettools
} // namespace facebook

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  folly::Init init(&argc, &argv);
  google::InstallFailureSignalHandler();

  // Init
  facebook::nettools::bgplib::init();

  // run the unittests
  return RUN_ALL_TESTS();
}
