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

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibGroupSerializer.h"

#include <gtest/gtest.h>

#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;

using folly::IPAddress;

class AdjRibGroupSerializerTest : public ::testing::Test {
 protected:
  void SetUp() override {}

  BgpUpdate2 createIPv4Update() {
    BgpUpdate2 update;

    // Add attributes
    update.attrs()->origin() = BgpAttrOrigin::BGP_ORIGIN_IGP;

    BgpAttrAsPathSegment segment;
    segment.asSequence()->push_back(65001);
    update.attrs()->asPath()->push_back(segment);

    // Placeholder nexthop (0.0.0.0) - this is what the group uses
    update.v4Nexthop() =
        facebook::network::toBinaryAddress(folly::IPAddress("0.0.0.0"));

    update.attrs()->localPref() = 100;

    // Add a prefix
    RiggedIPPrefix prefix;
    prefix.prefix() = facebook::network::toIPPrefix(
        folly::IPAddress::createNetwork("10.1.0.0/24"));
    update.v4Announced2()->push_back(prefix);

    return update;
  }

  BgpUpdate2 createIPv6Update() {
    BgpUpdate2 update;

    // Add attributes
    update.attrs()->origin() = BgpAttrOrigin::BGP_ORIGIN_IGP;

    BgpAttrAsPathSegment segment;
    segment.asSequence()->push_back(65001);
    update.attrs()->asPath()->push_back(segment);

    update.attrs()->localPref() = 100;

    // Placeholder nexthop (::) - this is what the group uses
    update.mpAnnounced()->afi() = BgpUpdateAfi::AFI_IPv6;
    update.mpAnnounced()->safi() = BgpUpdateSafi::SAFI_UNICAST;
    update.mpAnnounced()->nexthop() =
        facebook::network::toBinaryAddress(folly::IPAddress("::"));

    // Add a prefix
    RiggedIPPrefix prefix;
    prefix.prefix() = facebook::network::toIPPrefix(
        folly::IPAddress::createNetwork("2001:db8::/64"));
    update.mpAnnounced()->prefixes()->push_back(prefix);

    return update;
  }
};

/**
 * Test that serializeUpdateAndCreateDescriptor creates a valid descriptor
 * for IPv4 announcements with 4-byte ASN
 */
TEST_F(AdjRibGroupSerializerTest, SerializeIPv4Update) {
  auto update = createIPv4Update();

  auto descriptor = AdjRibGroupSerializer::serializeUpdateAndCreateDescriptor(
      update, true /* as4byte */, false /* extNhEncoding */);

  /* Verify descriptor was created successfully */
  ASSERT_NE(descriptor.serializedGroupPDU, nullptr);
  EXPECT_GT(descriptor.serializedGroupPDU->computeChainDataLength(), 0);

  /* Verify nexthop offset information is populated */
  ASSERT_EQ(descriptor.nexthopOffsets.size(), 1);
  auto [bufferIndex, offset, isV4] = descriptor.nexthopOffsets[0];
  EXPECT_GT(offset, 0);
  EXPECT_TRUE(isV4); /* IPv4 nexthop */

  /* v4Nexthop not extracted since implementation only uses mpAnnounced */
  EXPECT_EQ(descriptor.v4Nexthop.family(), AF_UNSPEC);
  EXPECT_EQ(descriptor.v6Nexthop.family(), AF_UNSPEC);
}

/**
 * Test that serializeUpdateAndCreateDescriptor creates a valid descriptor
 * for IPv6 announcements
 */
TEST_F(AdjRibGroupSerializerTest, SerializeIPv6Update) {
  auto update = createIPv6Update();

  auto descriptor = AdjRibGroupSerializer::serializeUpdateAndCreateDescriptor(
      update, true /* as4byte */, false /* extNhEncoding */);

  /* Verify descriptor was created successfully */
  ASSERT_NE(descriptor.serializedGroupPDU, nullptr);
  EXPECT_GT(descriptor.serializedGroupPDU->computeChainDataLength(), 0);

  /* Verify nexthop offset information is populated (IPv6 = 16 bytes) */
  ASSERT_EQ(descriptor.nexthopOffsets.size(), 1);
  auto [bufferIndex, offset, isV4] = descriptor.nexthopOffsets[0];
  EXPECT_GT(offset, 0);
  EXPECT_FALSE(isV4); /* IPv6 nexthop */

  /* Verify v6 nexthop is populated */
  EXPECT_EQ(descriptor.v6Nexthop, folly::IPAddress("::"));
  EXPECT_EQ(descriptor.v4Nexthop.family(), AF_UNSPEC);
}

/**
 * Test with 2-byte ASN encoding
 */
TEST_F(AdjRibGroupSerializerTest, SerializeWith2ByteASN) {
  auto update = createIPv4Update();

  auto descriptor = AdjRibGroupSerializer::serializeUpdateAndCreateDescriptor(
      update, false /* as4byte */, false /* extNhEncoding */);

  /* Verify descriptor was created successfully */
  ASSERT_NE(descriptor.serializedGroupPDU, nullptr);
  ASSERT_EQ(descriptor.nexthopOffsets.size(), 1);
  auto [bufferIndex, offset, isV4] = descriptor.nexthopOffsets[0];
  EXPECT_GT(offset, 0);
  EXPECT_TRUE(isV4); /* IPv4 nexthop */
}

/**
 * Test with Extended Nexthop Encoding (RFC5549 - IPv4 NLRI over IPv6 nexthop)
 */
TEST_F(AdjRibGroupSerializerTest, SerializeWithExtNhEncoding) {
  BgpUpdate2 update;

  update.attrs()->origin() = BgpAttrOrigin::BGP_ORIGIN_IGP;

  BgpAttrAsPathSegment segment;
  segment.asSequence()->push_back(65001);
  update.attrs()->asPath()->push_back(segment);

  // IPv4 NLRI with IPv6 nexthop (ExtNH encoding)
  update.mpAnnounced()->afi() = BgpUpdateAfi::AFI_IPv4;
  update.mpAnnounced()->safi() = BgpUpdateSafi::SAFI_UNICAST;
  update.mpAnnounced()->nexthop() =
      facebook::network::toBinaryAddress(folly::IPAddress("::"));

  RiggedIPPrefix prefix;
  prefix.prefix() = facebook::network::toIPPrefix(
      folly::IPAddress::createNetwork("192.168.1.0/24"));
  update.mpAnnounced()->prefixes()->push_back(prefix);

  auto descriptor = AdjRibGroupSerializer::serializeUpdateAndCreateDescriptor(
      update, true /* as4byte */, true /* extNhEncoding */);

  /* Verify descriptor was created successfully */
  ASSERT_NE(descriptor.serializedGroupPDU, nullptr);
  ASSERT_EQ(descriptor.nexthopOffsets.size(), 1);
  auto [bufferIndex, offset, isV4] = descriptor.nexthopOffsets[0];
  EXPECT_GT(offset, 0);
  EXPECT_FALSE(isV4); /* IPv6 nexthop */
}

/**
 * Test that the serialized PDU is immutable (shared_ptr<const>)
 * and the offset information is correctly stored
 */
TEST_F(AdjRibGroupSerializerTest, DescriptorFieldsPopulated) {
  auto update = createIPv4Update();

  auto descriptor = AdjRibGroupSerializer::serializeUpdateAndCreateDescriptor(
      update, true /* as4byte */, false /* extNhEncoding */);

  /* Verify all descriptor fields are populated */
  ASSERT_NE(descriptor.serializedGroupPDU, nullptr);

  /* The PDU should be const (zero-copy - shared across peers) */
  EXPECT_TRUE(
      std::is_const_v<std::remove_pointer_t<
          decltype(descriptor.serializedGroupPDU.get())>>);

  /* Nexthop offset vector should have one entry for this simple message */
  ASSERT_EQ(descriptor.nexthopOffsets.size(), 1);
  auto [bufferIndex, offset, isV4] = descriptor.nexthopOffsets[0];

  /* Buffer index and offset should be reasonable */
  EXPECT_LT(bufferIndex, 10); /* Should be small for simple message */
  EXPECT_GT(offset, 0);
  EXPECT_LT(offset, 1000); /* Reasonable upper bound */
  EXPECT_TRUE(isV4); /* IPv4 nexthop */
}
