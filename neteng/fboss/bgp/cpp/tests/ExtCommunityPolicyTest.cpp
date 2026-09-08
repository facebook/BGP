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

#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/bgp_policy_types.h"
#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyAction.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

using namespace nettools::bgplib;
using ::testing::HasSubstr;

class ExtCommunityPolicyTest : public ::testing::Test {};

/*
 * ============================================================================
 * parseLinkBandwidthBps Tests
 * ============================================================================
 */

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBpsValidCases) {
  /* Test with G suffix */
  auto result = parseLinkBandwidthBps("1G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1000000000); /* 1 Gbps */

  result = parseLinkBandwidthBps("10G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 10000000000); /* 10 Gbps */

  result = parseLinkBandwidthBps("100G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100000000000); /* 100 Gbps */

  /* Test with M suffix */
  result = parseLinkBandwidthBps("10M");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 10000000); /* 10 Mbps */

  result = parseLinkBandwidthBps("100M");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100000000); /* 100 Mbps */

  /* Test with K suffix */
  result = parseLinkBandwidthBps("1K");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1000); /* 1 Kbps */

  result = parseLinkBandwidthBps("100K");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100000); /* 100 Kbps */

  /* Test without suffix */
  result = parseLinkBandwidthBps("100");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100); /* 100 bps */

  result = parseLinkBandwidthBps("1000");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1000); /* 1000 bps */
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBpsDecimalValues) {
  /* Test decimal values with G suffix */
  auto result = parseLinkBandwidthBps("1.5G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1500000000); /* 1.5 Gbps */

  result = parseLinkBandwidthBps("2.5G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 2500000000); /* 2.5 Gbps */

  result = parseLinkBandwidthBps("0.5G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 500000000); /* 0.5 Gbps */

  /* Test decimal values with M suffix */
  result = parseLinkBandwidthBps("1.5M");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1500000); /* 1.5 Mbps */

  result = parseLinkBandwidthBps("10.25M");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 10250000); /* 10.25 Mbps */

  /* Test decimal values with K suffix */
  result = parseLinkBandwidthBps("1.5K");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1500); /* 1.5 Kbps */

  /* Test decimal values without suffix */
  result = parseLinkBandwidthBps("100.5");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100); /* 100.5 bps (truncated to int) */
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBpsCaseInsensitiveSuffix) {
  /* Test lowercase suffixes */
  auto result = parseLinkBandwidthBps("1g");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1000000000);

  result = parseLinkBandwidthBps("10m");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 10000000);

  result = parseLinkBandwidthBps("100k");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100000);

  /* Test uppercase suffixes */
  result = parseLinkBandwidthBps("1G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1000000000);

  result = parseLinkBandwidthBps("10M");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 10000000);

  result = parseLinkBandwidthBps("100K");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100000);
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBpsWhitespace) {
  /* Test with leading/trailing whitespace */
  auto result = parseLinkBandwidthBps("  10G  ");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 10000000000);

  result = parseLinkBandwidthBps("\t100M\t");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 100000000);

  result = parseLinkBandwidthBps(" 1.5G ");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1500000000);
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBpsInvalidCases) {
  /* Test empty string */
  auto result = parseLinkBandwidthBps("");
  EXPECT_FALSE(result.has_value());

  /* Test whitespace only */
  result = parseLinkBandwidthBps("   ");
  EXPECT_FALSE(result.has_value());

  /* Test multiple decimal points */
  result = parseLinkBandwidthBps("1.5.5G");
  EXPECT_FALSE(result.has_value());

  /* Test invalid suffix */
  result = parseLinkBandwidthBps("100T");
  EXPECT_FALSE(result.has_value());

  result = parseLinkBandwidthBps("100X");
  EXPECT_FALSE(result.has_value());

  /* Test suffix not at end */
  result = parseLinkBandwidthBps("10G0");
  EXPECT_FALSE(result.has_value());

  result = parseLinkBandwidthBps("G100");
  EXPECT_FALSE(result.has_value());

  /* Test non-numeric values */
  result = parseLinkBandwidthBps("abc");
  EXPECT_FALSE(result.has_value());

  result = parseLinkBandwidthBps("abcG");
  EXPECT_FALSE(result.has_value());

  /* Test invalid characters */
  result = parseLinkBandwidthBps("10@G");
  EXPECT_FALSE(result.has_value());

  result = parseLinkBandwidthBps("10 G");
  EXPECT_FALSE(result.has_value());
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBpsEdgeCases) {
  /* Test zero */
  auto result = parseLinkBandwidthBps("0");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);

  result = parseLinkBandwidthBps("0G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);

  /* Test very small values */
  result = parseLinkBandwidthBps("0.001G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1000000); /* 0.001 * 1,000,000,000 = 1,000,000 */

  /* Test leading zeros */
  result = parseLinkBandwidthBps("001G");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 1000000000);
}

/*
 * ============================================================================
 * parseLinkBandwidthBytesPerSec Tests
 * ============================================================================
 */

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBytesPerSecValidCases) {
  /* Test with G suffix - should be divided by 8 */
  auto result = parseLinkBandwidthBytesPerSec("1G");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 125000000.0f); /* 1 Gbps / 8 = 125 MB/s */

  result = parseLinkBandwidthBytesPerSec("10G");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1250000000.0f); /* 10 Gbps / 8 = 1.25 GB/s */

  result = parseLinkBandwidthBytesPerSec("100G");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 12500000000.0f); /* 100 Gbps / 8 = 12.5 GB/s */

  /* Test with M suffix */
  result = parseLinkBandwidthBytesPerSec("10M");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1250000.0f); /* 10 Mbps / 8 = 1.25 MB/s */

  result = parseLinkBandwidthBytesPerSec("100M");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 12500000.0f); /* 100 Mbps / 8 = 12.5 MB/s */

  /* Test with K suffix */
  result = parseLinkBandwidthBytesPerSec("8K");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1000.0f); /* 8 Kbps / 8 = 1 KB/s */

  /* Test without suffix */
  result = parseLinkBandwidthBytesPerSec("800");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 100.0f); /* 800 bps / 8 = 100 B/s */
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBytesPerSecDecimalValues) {
  /* Test decimal values with G suffix */
  auto result = parseLinkBandwidthBytesPerSec("1.5G");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 187500000.0f); /* 1.5 Gbps / 8 = 187.5 MB/s */

  result = parseLinkBandwidthBytesPerSec("2.5G");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 312500000.0f); /* 2.5 Gbps / 8 = 312.5 MB/s */

  /* Test decimal values with M suffix */
  result = parseLinkBandwidthBytesPerSec("1.5M");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 187500.0f); /* 1.5 Mbps / 8 = 187.5 KB/s */
}

TEST_F(
    ExtCommunityPolicyTest,
    ParseLinkBandwidthBytesPerSecCaseInsensitiveSuffix) {
  /* Test lowercase suffixes */
  auto result = parseLinkBandwidthBytesPerSec("1g");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 125000000.0f);

  result = parseLinkBandwidthBytesPerSec("10m");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1250000.0f);

  result = parseLinkBandwidthBytesPerSec("8k");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1000.0f);
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBytesPerSecWhitespace) {
  /* Test with leading/trailing whitespace */
  auto result = parseLinkBandwidthBytesPerSec("  10G  ");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1250000000.0f);

  result = parseLinkBandwidthBytesPerSec("\t8M\t");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1000000.0f);
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBytesPerSecInvalidCases) {
  /* Test empty string */
  auto result = parseLinkBandwidthBytesPerSec("");
  EXPECT_FALSE(result.has_value());

  /* Test whitespace only */
  result = parseLinkBandwidthBytesPerSec("   ");
  EXPECT_FALSE(result.has_value());

  /* Test multiple decimal points */
  result = parseLinkBandwidthBytesPerSec("1.5.5G");
  EXPECT_FALSE(result.has_value());

  /* Test invalid suffix */
  result = parseLinkBandwidthBytesPerSec("100T");
  EXPECT_FALSE(result.has_value());

  result = parseLinkBandwidthBytesPerSec("100X");
  EXPECT_FALSE(result.has_value());

  /* Test suffix not at end */
  result = parseLinkBandwidthBytesPerSec("10G0");
  EXPECT_FALSE(result.has_value());

  /* Test non-numeric values */
  result = parseLinkBandwidthBytesPerSec("abc");
  EXPECT_FALSE(result.has_value());

  result = parseLinkBandwidthBytesPerSec("abcG");
  EXPECT_FALSE(result.has_value());
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthBytesPerSecEdgeCases) {
  /* Test zero */
  auto result = parseLinkBandwidthBytesPerSec("0");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 0.0f);

  result = parseLinkBandwidthBytesPerSec("0G");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 0.0f);

  /* Test very small values */
  result = parseLinkBandwidthBytesPerSec("0.008G");
  ASSERT_TRUE(result.has_value());
  EXPECT_FLOAT_EQ(*result, 1000000.0f); /* 0.008 Gbps / 8 = 1 MB/s */
}

TEST_F(ExtCommunityPolicyTest, ParseLinkBandwidthConversionAccuracy) {
  /* Verify the relationship between the two functions */
  /* parseLinkBandwidthBytesPerSec should equal parseLinkBandwidthBps / 8 */

  std::vector<std::string> testCases = {
      "1G", "10G", "100G", "1.5G", "10M", "100M", "1K", "100", "2.5G", "0.5M"};

  for (const auto& testCase : testCases) {
    auto bps = parseLinkBandwidthBps(testCase);
    auto bytesPerSec = parseLinkBandwidthBytesPerSec(testCase);

    ASSERT_TRUE(bps.has_value()) << "Failed to parse: " << testCase;
    ASSERT_TRUE(bytesPerSec.has_value()) << "Failed to parse: " << testCase;

    float expectedBytesPerSec = static_cast<float>(*bps) / 8.0f;
    EXPECT_FLOAT_EQ(*bytesPerSec, expectedBytesPerSec)
        << "Conversion mismatch for: " << testCase;
  }
}

/*
 * ============================================================================
 * decodeLinkBandwidthExtCommunity Tests
 * ============================================================================
 */

TEST_F(ExtCommunityPolicyTest, DecodeLinkBandwidthExtCommunityStringValue) {
  /* Test decodeLinkBandwidthExtCommunity with string value "100G" */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40;
  extCommunity.type_low() = 0x04;
  extCommunity.value() = "100G";

  uint32_t rawValueHigh = 0;
  uint32_t rawValueLow = 0;
  uint32_t localAsn = 65001;

  decodeLinkBandwidthExtCommunity(
      extCommunity, localAsn, rawValueHigh, rawValueLow);

  /* Verify ASN encoding in rawValueHigh (65001 = 0xFDE9 in big-endian) */
  /* rawValueHigh should be: 0x40 0x04 0xFD 0xE9 */
  EXPECT_EQ(
      rawValueHigh & 0xFFFF, 0xFDE9); /* ASN in bytes 2-3 (lower 16 bits) */

  /* Verify bandwidth encoding (100G / 8 = 12,500,000,000 bytes/sec) */
  union {
    uint32_t intVal;
    float f;
  } converter{};
  converter.intVal = rawValueLow;

  float expectedBandwidth = 12500000000.0f; /* 100G / 8 */
  EXPECT_FLOAT_EQ(converter.f, expectedBandwidth);
}

TEST_F(ExtCommunityPolicyTest, DecodeLinkBandwidthExtCommunityInvalidValue) {
  /* Test with invalid string value */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40;
  extCommunity.type_low() = 0x04;
  extCommunity.value() = "invalid";

  uint32_t rawValueHigh = 0;
  uint32_t rawValueLow = 0;
  uint32_t localAsn = 65000;

  decodeLinkBandwidthExtCommunity(
      extCommunity, localAsn, rawValueHigh, rawValueLow);

  /* ASN should still be encoded */
  EXPECT_EQ(rawValueHigh & 0xFFFF, 65000);

  /* Bandwidth should be zero */
  EXPECT_EQ(rawValueLow, 0);
}

TEST_F(ExtCommunityPolicyTest, DecodeLinkBandwidthExtCommunityEmptyValue) {
  /* Test with no value set */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40;
  extCommunity.type_low() = 0x04;
  /* No value set */

  uint32_t rawValueHigh = 0;
  uint32_t rawValueLow = 0;
  uint32_t localAsn = 65000;

  decodeLinkBandwidthExtCommunity(
      extCommunity, localAsn, rawValueHigh, rawValueLow);

  /* ASN should be encoded */
  EXPECT_EQ(rawValueHigh & 0xFFFF, 65000);

  /* Bandwidth should be zero */
  EXPECT_EQ(rawValueLow, 0);
}

TEST_F(
    ExtCommunityPolicyTest,
    HandleLinkBandwidthExtCommunityVariousBandwidths) {
  /* Test various bandwidth string values */
  struct TestCase {
    std::string value;
    float expectedBandwidth;
  };

  std::vector<TestCase> testCases = {
      {"1G", 1.25e+8f},
      {"10G", 1.25e+9f},
      {"1.5G", 1.875e+8f},
      {"100M", 1.25e+7f},
      {"500M", 6.25e+7f},
  };

  uint32_t localAsn = 65001;

  for (const auto& tc : testCases) {
    bgp_policy::ExtCommunity extCommunity;
    extCommunity.type_high() = 0x40;
    extCommunity.type_low() = 0x04;
    extCommunity.value() = tc.value;

    uint32_t rawValueHigh = 0;
    uint32_t rawValueLow = 0;

    decodeLinkBandwidthExtCommunity(
        extCommunity, localAsn, rawValueHigh, rawValueLow);

    /* Extract bandwidth from rawValueLow */
    union {
      uint32_t intVal;
      float f;
    } converter{};
    converter.intVal = rawValueLow;

    EXPECT_FLOAT_EQ(converter.f, tc.expectedBandwidth)
        << "Failed for value: " << tc.value;
  }
}

TEST_F(ExtCommunityPolicyTest, DecodeLinkBandwidthExtCommunityDifferentASNs) {
  /* Test different ASN values to verify correct encoding */
  std::vector<uint32_t> asnTestCases = {0, 1, 100, 65000, 65535};

  for (uint32_t asn : asnTestCases) {
    bgp_policy::ExtCommunity extCommunity;
    extCommunity.type_high() = 0x40;
    extCommunity.type_low() = 0x04;
    extCommunity.value() = "10G";

    uint32_t rawValueHigh = 0;
    uint32_t rawValueLow = 0;

    decodeLinkBandwidthExtCommunity(
        extCommunity, asn, rawValueHigh, rawValueLow);

    /* Verify ASN encoding (only lower 16 bits used) */
    uint16_t expectedAsn = static_cast<uint16_t>(asn);
    EXPECT_EQ(rawValueHigh & 0xFFFF, expectedAsn) << "Failed for ASN: " << asn;
  }
}

TEST_F(
    ExtCommunityPolicyTest,
    DecodeLinkBandwidthExtCommunity4ByteAsnUsesAsTrans) {
  /* Test that 4-byte ASNs (> 65535) use AS_TRANS per RFC 6793 */
  std::vector<uint32_t> fourByteAsnTestCases = {65536, 100000, 4294967295};

  auto& messages = subscribeToLogMessages("", folly::LogLevel::WARN);

  for (uint32_t asn : fourByteAsnTestCases) {
    bgp_policy::ExtCommunity extCommunity;
    extCommunity.type_high() = 0x40;
    extCommunity.type_low() = 0x04;
    extCommunity.value() = "10G";

    uint32_t rawValueHigh = 0;
    uint32_t rawValueLow = 0;

    decodeLinkBandwidthExtCommunity(
        extCommunity, asn, rawValueHigh, rawValueLow);

    /* Verify AS_TRANS is used instead of truncated ASN */
    EXPECT_EQ(rawValueHigh & 0xFFFF, kAsTrans)
        << "4-byte ASN " << asn << " should use AS_TRANS (" << kAsTrans << ")";
  }

  /* Verify warning was logged for 4-byte ASN usage */
  EXPECT_GE(messages.size(), fourByteAsnTestCases.size());
}

TEST_F(
    ExtCommunityPolicyTest,
    HandleLinkBandwidthExtCommunityTransitiveVsNonTransitive) {
  /* Test that decodeLinkBandwidthExtCommunity works the same for both */
  /* transitive and non-transitive (the type_high is handled by the caller) */
  struct TestCase {
    uint8_t typeHigh;
    std::string description;
  };

  std::vector<TestCase> testCases = {
      {0x00, "transitive"},
      {0x40, "non-transitive"},
  };

  for (const auto& tc : testCases) {
    bgp_policy::ExtCommunity extCommunity;
    extCommunity.type_high() = tc.typeHigh;
    extCommunity.type_low() = 0x04;
    extCommunity.value() = "50G";

    uint32_t rawValueHigh = 0;
    uint32_t rawValueLow = 0;
    uint32_t localAsn = 65001;

    decodeLinkBandwidthExtCommunity(
        extCommunity, localAsn, rawValueHigh, rawValueLow);

    /* Verify ASN is encoded correctly regardless of transitive type (0xFDE9 =
     */
    /* 65001) */
    EXPECT_EQ(rawValueHigh & 0xFFFF, 0xFDE9) << "Failed for " << tc.description;

    /* Verify bandwidth (50G / 8 = 6.25e+9) */
    union {
      uint32_t intVal;
      float f;
    } converter{};
    converter.intVal = rawValueLow;

    EXPECT_FLOAT_EQ(converter.f, 6.25e+9f) << "Failed for " << tc.description;
  }
}

/*
 * ============================================================================
 * getBgpAttrExtCommunityC Tests
 * ============================================================================
 */

TEST_F(ExtCommunityPolicyTest, GetBgpAttrExtCommunityCLinkBandwidthTransitive) {
  /* Test transitive link-bandwidth extended community (type_high = 0x00) */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x00; /* Transitive */
  extCommunity.type_low() = 0x04; /* Link Bandwidth subtype */
  extCommunity.value() = "50G";

  auto config = createConfigWithAsn(65001);

  auto result = getBgpAttrExtCommunityC(extCommunity, &config);

  /* Verify the structure preserves transitive type */
  uint32_t high = result.getRawValueInWords().first;
  EXPECT_EQ((high >> 24) & 0xFF, 0x00); /* type_high - transitive */
  EXPECT_EQ((high >> 16) & 0xFF, 0x04); /* type_low */
  EXPECT_EQ((high >> 8) & 0xFF, 0xFD); /* ASN high byte */
  EXPECT_EQ(high & 0xFF, 0xE9); /* ASN low byte */

  /* Verify bandwidth (50G / 8 = 6.25e+9) */
  uint32_t low = result.getRawValueInWords().second;
  union {
    uint32_t intVal;
    float f;
  } converter{};
  converter.intVal = low;

  float expectedBandwidth = 6.25e+9f; /* 50G / 8 */
  EXPECT_FLOAT_EQ(converter.f, expectedBandwidth);
}

TEST_F(
    ExtCommunityPolicyTest,
    GetBgpAttrExtCommunityCLinkBandwidthNonTransitive) {
  /* Test non-transitive link-bandwidth extended community (type_high = 0x40) */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40; /* Non-transitive */
  extCommunity.type_low() = 0x04; /* Link Bandwidth subtype */
  extCommunity.value() = "50G";

  auto config = createConfigWithAsn(65001);

  auto result = getBgpAttrExtCommunityC(extCommunity, &config);

  /* Verify the structure preserves non-transitive type */
  uint32_t high = result.getRawValueInWords().first;
  EXPECT_EQ((high >> 24) & 0xFF, 0x40); /* type_high - non-transitive */
  EXPECT_EQ((high >> 16) & 0xFF, 0x04); /* type_low */
  EXPECT_EQ((high >> 8) & 0xFF, 0xFD); /* ASN high byte */
  EXPECT_EQ(high & 0xFF, 0xE9); /* ASN low byte */

  /* Verify bandwidth (50G / 8 = 6.25e+9) */
  uint32_t low = result.getRawValueInWords().second;
  union {
    uint32_t intVal;
    float f;
  } converter{};
  converter.intVal = low;

  float expectedBandwidth = 6.25e+9f; /* 50G / 8 */
  EXPECT_FLOAT_EQ(converter.f, expectedBandwidth);
}

TEST_F(
    ExtCommunityPolicyTest,
    GetBgpAttrExtCommunityCLinkBandwidthStringValue) {
  /* Test link-bandwidth extended community with string value "100G" */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40; /* Non-transitive */
  extCommunity.type_low() = 0x04; /* Link Bandwidth subtype */
  extCommunity.value() = "100G";

  auto config = createConfigWithAsn(65001);

  auto result = getBgpAttrExtCommunityC(extCommunity, &config);

  /* Verify the structure: */
  /* Byte 0: 0x40 (type_high) */
  /* Byte 1: 0x04 (type_low - link bandwidth) */
  /* Byte 2-3: ASN in network byte order (65001 = 0xFDE9) */
  /* Byte 4-7: IEEE 754 float for bandwidth in Bytes/sec */
  /*   100G bits/sec = 100,000,000,000 / 8 = 12,500,000,000 Bytes/sec */

  /* Extract the high 32 bits (first 4 bytes) */
  uint32_t high = result.getRawValueInWords().first;
  EXPECT_EQ((high >> 24) & 0xFF, 0x40); /* type_high */
  EXPECT_EQ((high >> 16) & 0xFF, 0x04); /* type_low */
  EXPECT_EQ((high >> 8) & 0xFF, 0xFD); /* ASN high byte */
  EXPECT_EQ(high & 0xFF, 0xE9); /* ASN low byte */

  /* Extract the IEEE 754 float from low 32 bits */
  uint32_t low = result.getRawValueInWords().second;
  union {
    uint32_t intVal;
    float f;
  } converter{};
  converter.intVal = low;

  float expectedBandwidth = 12500000000.0f; /* 100G / 8 */
  EXPECT_FLOAT_EQ(converter.f, expectedBandwidth);
}

TEST_F(
    ExtCommunityPolicyTest,
    GetBgpAttrExtCommunityCLinkBandwidthInvalidValue) {
  /* Test link-bandwidth extended community with invalid value */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40;
  extCommunity.type_low() = 0x04;
  extCommunity.value() = "invalid"; /* Invalid string, not 4 bytes */

  auto config = createConfigWithAsn(65000);

  auto result = getBgpAttrExtCommunityC(extCommunity, &config);

  /* Should have zeros for the bandwidth portion */
  uint32_t low = result.getRawValueInWords().second;
  EXPECT_EQ(low, 0);
}

TEST_F(ExtCommunityPolicyTest, GetBgpAttrExtCommunityCLinkBandwidthEmptyValue) {
  /* Test link-bandwidth extended community with empty value */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40;
  extCommunity.type_low() = 0x04;
  /* No value set */

  auto config = createConfigWithAsn(65000);

  auto result = getBgpAttrExtCommunityC(extCommunity, &config);

  /* Should have zeros for the bandwidth portion */
  uint32_t low = result.getRawValueInWords().second;
  EXPECT_EQ(low, 0);
}

TEST_F(
    ExtCommunityPolicyTest,
    GetBgpAttrExtCommunityCLinkBandwidthVariousSizes) {
  /* Test various bandwidth sizes */
  struct TestCase {
    std::string value;
    float expected;
  };

  std::vector<TestCase> testCases = {
      {"1G", 1.25e+8f}, /* 1 Gbps / 8 */
      {"10G", 1.25e+9f}, /* 10 Gbps / 8 */
      {"100G", 1.25e+10f}, /* 100 Gbps / 8 */
      {"1.5G", 1.875e+8f}, /* 1.5 Gbps / 8 */
      {"10M", 1.25e+6f}, /* 10 Mbps / 8 */
      {"100M", 1.25e+7f}, /* 100 Mbps / 8 */
  };

  auto config = createConfigWithAsn(65001);

  for (const auto& tc : testCases) {
    bgp_policy::ExtCommunity extCommunity;
    extCommunity.type_high() = 0x40;
    extCommunity.type_low() = 0x04;
    extCommunity.value() = tc.value;

    auto result = getBgpAttrExtCommunityC(extCommunity, &config);

    /* Extract bandwidth from low 32 bits */
    uint32_t low = result.getRawValueInWords().second;
    union {
      uint32_t intVal;
      float f;
    } converter{};
    converter.intVal = low;

    EXPECT_FLOAT_EQ(converter.f, tc.expected)
        << "Failed for value: " << tc.value;
  }
}

TEST_F(ExtCommunityPolicyTest, GetBgpAttrExtCommunityCNonLinkBandwidth) {
  /* Test non-link-bandwidth extended community - should throw BgpError */
  /* since only Link Bandwidth Extended Communities are supported */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x01; /* Route Target type */
  extCommunity.type_low() = 0x02; /* Subtype */
  /* Value: 4-byte AS (65001) + 2-byte local admin (100) */
  std::string value;
  value.push_back(0x00);
  value.push_back(0x00);
  value.push_back(0xFD);
  value.push_back(0xE9); /* AS 65001 */
  value.push_back(0x00);
  value.push_back(0x64); /* Local admin 100 */
  extCommunity.value() = value;

  auto config = createConfigWithAsn(65001);

  /* Expect BgpError to be thrown for unsupported ExtCommunity type */
  EXPECT_THROW(getBgpAttrExtCommunityC(extCommunity, &config), BgpError);
}

TEST_F(ExtCommunityPolicyTest, GetBgpAttrExtCommunityCNoConfig) {
  /* Test link-bandwidth with null config (localAsn should be 0) */
  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40;
  extCommunity.type_low() = 0x04;
  extCommunity.value() = "10G";

  auto result = getBgpAttrExtCommunityC(extCommunity, nullptr);

  /* Verify ASN is 0 */
  uint32_t high = result.getRawValueInWords().first;
  EXPECT_EQ((high >> 8) & 0xFF, 0x00); /* ASN high byte */
  EXPECT_EQ(high & 0xFF, 0x00); /* ASN low byte */

  /* Verify bandwidth is still parsed correctly */
  uint32_t low = result.getRawValueInWords().second;
  union {
    uint32_t intVal;
    float f;
  } converter{};
  converter.intVal = low;
  float expectedBandwidth = 1.25e+9f; /* 10G / 8 */
  EXPECT_FLOAT_EQ(converter.f, expectedBandwidth);
}

TEST_F(ExtCommunityPolicyTest, GetBgpAttrExtCommunityCMissingTypeHigh) {
  /* Test that missing type_high logs an error message */
  auto& messages = subscribeToLogMessages("", folly::LogLevel::ERR);
  messages.clear();

  bgp_policy::ExtCommunity extCommunity;
  /* Do not set type_high - this should trigger the error log */
  extCommunity.type_low() = 0x04; /* Link Bandwidth subtype */
  extCommunity.value() = "10G";

  auto config = createConfigWithAsn(65001);

  /* Call the function - it should log an error but not crash */
  auto result = getBgpAttrExtCommunityC(extCommunity, &config);

  /* Verify that an error message was logged */
  ASSERT_EQ(messages.size(), 1);
  EXPECT_TRUE(
      messages[0].first.getMessage().find(
          "Unexpected empty type_high; expected type_high to be set on ExtCommunity") !=
      std::string::npos);
}

TEST_F(ExtCommunityPolicyTest, DecodeLinkBandwidthExtCommunityInvalidTypeLow) {
  /* Test that invalid type_low logs an error message */
  auto& messages = subscribeToLogMessages("", folly::LogLevel::ERR);
  messages.clear();

  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0x40; /* Non-transitive */
  extCommunity.type_low() = 0x05; /* Invalid - should be 0x04 */
  extCommunity.value() = "10G";

  uint32_t rawValueHigh = 0;
  uint32_t rawValueLow = 0;
  uint32_t localAsn = 65001;

  /* Call the function - it should log an error but not crash */
  decodeLinkBandwidthExtCommunity(
      extCommunity, localAsn, rawValueHigh, rawValueLow);

  /* Verify that an error message was logged */
  ASSERT_EQ(messages.size(), 1);
  EXPECT_TRUE(
      messages[0].first.getMessage().find(
          "Invalid type_low for Link Bandwidth Extended Community") !=
      std::string::npos);
  EXPECT_TRUE(
      messages[0].first.getMessage().find("expected 0x04") !=
      std::string::npos);
  EXPECT_TRUE(
      messages[0].first.getMessage().find("got 0x05") != std::string::npos);
}

TEST_F(ExtCommunityPolicyTest, DecodeLinkBandwidthExtCommunityInvalidTypeHigh) {
  /* Test that invalid type_high logs an error message */
  auto& messages = subscribeToLogMessages("", folly::LogLevel::ERR);
  messages.clear();

  bgp_policy::ExtCommunity extCommunity;
  extCommunity.type_high() = 0xFF; /* Invalid - should be 0x00 or 0x40 */
  extCommunity.type_low() = 0x04; /* Link Bandwidth subtype */
  extCommunity.value() = "10G";

  uint32_t rawValueHigh = 0;
  uint32_t rawValueLow = 0;
  uint32_t localAsn = 65001;

  /* Call the function - it should log an error but not crash */
  decodeLinkBandwidthExtCommunity(
      extCommunity, localAsn, rawValueHigh, rawValueLow);

  /* Verify that an error message was logged */
  ASSERT_EQ(messages.size(), 1);
  EXPECT_TRUE(
      messages[0].first.getMessage().find(
          "Unexpected type_high for Link Bandwidth Extended Community") !=
      std::string::npos);
  EXPECT_TRUE(
      messages[0].first.getMessage().find("expected 0x00 or 0x40") !=
      std::string::npos);
  EXPECT_TRUE(
      messages[0].first.getMessage().find("got 0xFF") != std::string::npos);
}

/*
 * ============================================================================
 * hasExtCommunity Tests
 * ============================================================================
 */

TEST_F(ExtCommunityPolicyTest, HasExtCommunityFoundTest) {
  /* Test that hasExtCommunity returns true when extended community is found */
  std::vector<BgpAttrExtCommunityC> extCommunities = {
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x40041111, 0x501502f9},
      BgpAttrExtCommunityC{0x01010A0A, 0x0A05CAFE},
  };

  EXPECT_TRUE(hasExtCommunity(
      extCommunities, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
  EXPECT_TRUE(hasExtCommunity(
      extCommunities, BgpAttrExtCommunityC{0x40041111, 0x501502f9}));
  EXPECT_TRUE(hasExtCommunity(
      extCommunities, BgpAttrExtCommunityC{0x01010A0A, 0x0A05CAFE}));
}

TEST_F(ExtCommunityPolicyTest, HasExtCommunityNotFoundTest) {
  /* Test that hasExtCommunity returns false when extended community is not
   * found */
  std::vector<BgpAttrExtCommunityC> extCommunities = {
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x40041111, 0x501502f9},
  };

  EXPECT_FALSE(hasExtCommunity(
      extCommunities, BgpAttrExtCommunityC{0x00011234, 0x600DCAFF}));
  EXPECT_FALSE(hasExtCommunity(
      extCommunities, BgpAttrExtCommunityC{0x99999999, 0x99999999}));
}

TEST_F(ExtCommunityPolicyTest, HasExtCommunityEmptyListTest) {
  /* Test that hasExtCommunity returns false for empty list */
  std::vector<BgpAttrExtCommunityC> extCommunities;

  EXPECT_FALSE(hasExtCommunity(
      extCommunities, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
}

TEST_F(ExtCommunityPolicyTest, HasExtCommunityLinkBandwidthTest) {
  /* Test hasExtCommunity with Link Bandwidth extended communities */
  std::vector<BgpAttrExtCommunityC> extCommunities;

  /* Add LBW communities using the helper type */
  BgpExtCommunityLinkBandWidthTypeC lbw1(12345, 1.25e+9f); /* 10G */
  BgpExtCommunityLinkBandWidthTypeC lbw2(65001, 1.25e+10f); /* 100G */
  extCommunities.emplace_back(lbw1);
  extCommunities.emplace_back(lbw2);

  /* Should find the exact LBW communities */
  EXPECT_TRUE(hasExtCommunity(extCommunities, BgpAttrExtCommunityC(lbw1)));
  EXPECT_TRUE(hasExtCommunity(extCommunities, BgpAttrExtCommunityC(lbw2)));

  /* Should not find a different LBW community */
  BgpExtCommunityLinkBandWidthTypeC lbw3(
      12345, 1.25e+8f); /* 1G - different bw */
  EXPECT_FALSE(hasExtCommunity(extCommunities, BgpAttrExtCommunityC(lbw3)));
}

/*
 * ============================================================================
 * ExtCommunityAction validateAndSetExtCommunities Tests
 * ============================================================================
 */

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionAddToEmpty) {
  // Test adding ext communities to a path with no ext communities
  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");
  bgp_policy::ExtCommunity ext2 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "50G");

  auto addAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_ADD,
      {ext1, ext2});
  auto config = createConfigWithAsn(65000);
  auto action = std::make_shared<ExtCommunityAction>(addAction, &config);

  // Create a path with no ext communities
  auto path = createBgpPath();
  EXPECT_TRUE(path->getExtCommunities().nullOrEmpty());

  // Apply ADD action
  action->applyAction(path, std::nullopt);

  // Verify ext communities were added
  EXPECT_FALSE(path->getExtCommunities().nullOrEmpty());
  EXPECT_EQ(2, path->getExtCommunities()->size());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionAddToExisting) {
  /*
   * Test adding ext communities to a path with existing ext communities
   * Also tests that duplicates are not added
   */
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");
  bgp_policy::ExtCommunity ext2 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "50G");
  bgp_policy::ExtCommunity ext3 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "25G");

  // Create ext communities to add (ext2 already exists, ext3 is new)
  auto addAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_ADD,
      {ext2, ext3});
  auto action = std::make_shared<ExtCommunityAction>(addAction, &config);

  // Create a path with existing ext communities
  auto path = createBgpPath();
  BgpAttrExtCommunitiesC initialExtComms;
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext1, &config));
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext2, &config));
  path->setExtCommunities(initialExtComms);
  EXPECT_EQ(2, path->getExtCommunities()->size());

  // Apply ADD action
  action->applyAction(path, std::nullopt);

  /*
   * Verify: ext2 already exists (no duplicate), ext3 was added
   * So we should have 3 total: ext1, ext2, ext3
   */
  EXPECT_EQ(3, path->getExtCommunities()->size());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionAddDuplicates) {
  // Test that adding a duplicate ext community doesn't create a duplicate
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");

  auto addAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_ADD, {ext1});
  auto action = std::make_shared<ExtCommunityAction>(addAction, &config);

  // Create a path with existing ext community
  auto path = createBgpPath();
  BgpAttrExtCommunitiesC initialExtComms;
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext1, &config));
  path->setExtCommunities(initialExtComms);
  EXPECT_EQ(1, path->getExtCommunities()->size());

  // Apply ADD action with same ext community
  action->applyAction(path, std::nullopt);

  // Verify: no duplicate added, still only 1 ext community
  EXPECT_EQ(1, path->getExtCommunities()->size());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionSetReplace) {
  // Test that SET action replaces all ext communities
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");
  bgp_policy::ExtCommunity ext2 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "50G");
  bgp_policy::ExtCommunity ext3 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "25G");
  bgp_policy::ExtCommunity ext4 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "10G");

  auto setAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_SET,
      {ext3, ext4});
  auto action = std::make_shared<ExtCommunityAction>(setAction, &config);

  // Create a path with existing ext communities
  auto path = createBgpPath();
  BgpAttrExtCommunitiesC initialExtComms;
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext1, &config));
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext2, &config));
  path->setExtCommunities(initialExtComms);
  EXPECT_EQ(2, path->getExtCommunities()->size());

  // Apply SET action
  action->applyAction(path, std::nullopt);

  // Verify: all previous ext communities replaced with 2 new ones
  EXPECT_EQ(2, path->getExtCommunities()->size());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionSetEmpty) {
  // Test that SET action with empty list clears all ext communities
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");
  bgp_policy::ExtCommunity ext2 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "50G");

  auto setAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_SET, {});
  auto action = std::make_shared<ExtCommunityAction>(setAction, &config);

  // Create a path with existing ext communities
  auto path = createBgpPath();
  BgpAttrExtCommunitiesC initialExtComms;
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext1, &config));
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext2, &config));
  path->setExtCommunities(initialExtComms);
  EXPECT_EQ(2, path->getExtCommunities()->size());

  // Apply SET action with empty list
  action->applyAction(path, std::nullopt);

  // Verify: all ext communities cleared
  EXPECT_TRUE(path->getExtCommunities().nullOrEmpty());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionRemoveSpecific) {
  // Test removing specific ext communities
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");
  bgp_policy::ExtCommunity ext2 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "50G");
  bgp_policy::ExtCommunity ext3 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "25G");
  bgp_policy::ExtCommunity ext4 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "10G");

  auto removeAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_REMOVE,
      {ext1, ext3});
  auto action = std::make_shared<ExtCommunityAction>(removeAction, &config);

  // Create a path with ext communities
  auto path = createBgpPath();
  BgpAttrExtCommunitiesC initialExtComms;
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext1, &config));
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext2, &config));
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext3, &config));
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext4, &config));
  path->setExtCommunities(initialExtComms);
  EXPECT_EQ(4, path->getExtCommunities()->size());

  // Apply REMOVE action
  action->applyAction(path, std::nullopt);

  // Verify: ext1 and ext3 removed, ext2 and ext4 remain (2 total)
  EXPECT_EQ(2, path->getExtCommunities()->size());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionRemoveNonExistent) {
  // Test removing ext communities that don't exist
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");
  bgp_policy::ExtCommunity ext2 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "50G");
  bgp_policy::ExtCommunity ext3 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "25G");
  bgp_policy::ExtCommunity ext4 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "10G");

  auto removeAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_REMOVE,
      {ext3, ext4});
  auto action = std::make_shared<ExtCommunityAction>(removeAction, &config);

  // Create a path with different ext communities
  auto path = createBgpPath();
  BgpAttrExtCommunitiesC initialExtComms;
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext1, &config));
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext2, &config));
  path->setExtCommunities(initialExtComms);
  EXPECT_EQ(2, path->getExtCommunities()->size());

  // Apply REMOVE action (ext3 and ext4 don't exist)
  action->applyAction(path, std::nullopt);

  // Verify: no change, original 2 ext communities remain
  EXPECT_EQ(2, path->getExtCommunities()->size());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionRemoveAll) {
  // Test removing all ext communities
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");
  bgp_policy::ExtCommunity ext2 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "50G");

  auto removeAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_REMOVE,
      {ext1, ext2});
  auto action = std::make_shared<ExtCommunityAction>(removeAction, &config);

  // Create a path with ext communities
  auto path = createBgpPath();
  BgpAttrExtCommunitiesC initialExtComms;
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext1, &config));
  initialExtComms.emplace_back(getBgpAttrExtCommunityC(ext2, &config));
  path->setExtCommunities(initialExtComms);
  EXPECT_EQ(2, path->getExtCommunities()->size());

  // Apply REMOVE action
  action->applyAction(path, std::nullopt);

  // Verify: all ext communities removed
  EXPECT_TRUE(path->getExtCommunities().nullOrEmpty());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionRemoveFromEmpty) {
  // Test removing ext communities from empty list
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");

  auto removeAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_REMOVE, {ext1});
  auto action = std::make_shared<ExtCommunityAction>(removeAction, &config);

  // Create a path with no ext communities
  auto path = createBgpPath();
  EXPECT_TRUE(path->getExtCommunities().nullOrEmpty());

  // Apply REMOVE action
  action->applyAction(path, std::nullopt);

  // Verify: still empty, no errors
  EXPECT_TRUE(path->getExtCommunities().nullOrEmpty());
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionInvalidActionType) {
  /*
   * Test that ExtCommunityAction constructor throws error for invalid
   * action types (must be EXT_COMMUNITY_LIST_ADD/SET/REMOVE)
   */
  auto config = createConfigWithAsn(65000);

  bgp_policy::ExtCommunity ext1 = createExtCommunity(
      0x40, // type_high (Link Bandwidth)
      0x04, // type_low
      "100G");

  // Test with COMMUNITY_LIST_ADD (invalid, should be EXT_COMMUNITY_LIST_ADD)
  {
    auto invalidAction = createBgpPolicyExtCommunityAction(
        bgp_policy::BgpAttrChangeActionType::COMMUNITY_LIST_ADD, {ext1});
    EXPECT_THROW(
        {
          try {
            std::make_shared<ExtCommunityAction>(invalidAction, &config);
          } catch (const BgpError& e) {
            EXPECT_THAT(
                e.what(), HasSubstr("Unexpected BgpAttrChangeActionType"));
            EXPECT_THAT(e.what(), HasSubstr("ExtCommunityAction"));
            throw;
          }
        },
        BgpError);
  }

  // Test with COMMUNITY_LIST_SET (invalid, should be EXT_COMMUNITY_LIST_SET)
  {
    auto invalidAction = createBgpPolicyExtCommunityAction(
        bgp_policy::BgpAttrChangeActionType::COMMUNITY_LIST_SET, {ext1});
    EXPECT_THROW(
        {
          try {
            std::make_shared<ExtCommunityAction>(invalidAction, &config);
          } catch (const BgpError& e) {
            EXPECT_THAT(
                e.what(), HasSubstr("Unexpected BgpAttrChangeActionType"));
            EXPECT_THAT(e.what(), HasSubstr("ExtCommunityAction"));
            throw;
          }
        },
        BgpError);
  }

  /*
   * Test with COMMUNITY_LIST_REMOVE (invalid, should be
   * EXT_COMMUNITY_LIST_REMOVE)
   */
  {
    auto invalidAction = createBgpPolicyExtCommunityAction(
        bgp_policy::BgpAttrChangeActionType::COMMUNITY_LIST_REMOVE, {ext1});
    EXPECT_THROW(
        {
          try {
            std::make_shared<ExtCommunityAction>(invalidAction, &config);
          } catch (const BgpError& e) {
            EXPECT_THAT(
                e.what(), HasSubstr("Unexpected BgpAttrChangeActionType"));
            EXPECT_THAT(e.what(), HasSubstr("ExtCommunityAction"));
            throw;
          }
        },
        BgpError);
  }
}

TEST_F(ExtCommunityPolicyTest, ExtCommunityActionUnsupportedType) {
  /*
   * Test that ExtCommunityAction constructor throws error for unsupported
   * ExtCommunity types (only Link Bandwidth is supported)
   */
  auto config = createConfigWithAsn(65000);

  // Create a Route Target ExtCommunity (not Link Bandwidth)
  bgp_policy::ExtCommunity rtExtComm;
  rtExtComm.type_high() = 0x00; // AS-specific
  rtExtComm.type_low() = 0x02; // Route Target (not Link Bandwidth 0x04)
  // Value: 4-byte AS (65000) + 2-byte local admin (100)
  std::string value;
  value.push_back(0x00);
  value.push_back(0x00);
  value.push_back(0xFD);
  value.push_back(0xE8); // AS 65000
  value.push_back(0x00);
  value.push_back(0x64); // Local admin 100
  rtExtComm.value() = value;

  // Attempt to create an ExtCommunityAction with unsupported type
  auto addAction = createBgpPolicyExtCommunityAction(
      bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_ADD, {rtExtComm});

  // Should throw BgpError because Route Target is not supported
  EXPECT_THROW(
      {
        try {
          std::make_shared<ExtCommunityAction>(addAction, &config);
        } catch (const BgpError& e) {
          EXPECT_THAT(e.what(), HasSubstr("Unsupported ExtCommunity type"));
          EXPECT_THAT(
              e.what(),
              HasSubstr(
                  "Only Link Bandwidth Extended Communities are supported"));
          throw;
        }
      },
      BgpError);
}

/*
 * ============================================================================
 * addExtCommunities Tests
 * ============================================================================
 */

TEST_F(ExtCommunityPolicyTest, AddExtCommunitiesToEmptyTest) {
  /* Test adding extended communities to an empty list */
  BgpAttrExtCommunitiesC current;
  BgpAttrExtCommunitiesC toAdd{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x00011235, 0x600DCAFE},
  }};

  auto result = addExtCommunities(current, toAdd);

  EXPECT_EQ(2, result.size());
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011235, 0x600DCAFE}));
}

TEST_F(ExtCommunityPolicyTest, AddExtCommunitiesToExistingTest) {
  /* Test adding extended communities to existing list */
  BgpAttrExtCommunitiesC current{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
  }};
  BgpAttrExtCommunitiesC toAdd{{
      BgpAttrExtCommunityC{0x00011235, 0x600DCAFE},
  }};

  auto result = addExtCommunities(current, toAdd);

  EXPECT_EQ(2, result.size());
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011235, 0x600DCAFE}));
}

TEST_F(ExtCommunityPolicyTest, AddExtCommunitiesNoDuplicatesTest) {
  /* Test that duplicates are not added */
  BgpAttrExtCommunitiesC current{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x00011235, 0x600DCAFE},
  }};
  BgpAttrExtCommunitiesC toAdd{{
      BgpAttrExtCommunityC{0x00011235, 0x600DCAFE}, /* duplicate */
      BgpAttrExtCommunityC{0x00011236, 0x600DCAFE}, /* new */
  }};

  auto result = addExtCommunities(current, toAdd);

  EXPECT_EQ(3, result.size());
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011235, 0x600DCAFE}));
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011236, 0x600DCAFE}));
}

TEST_F(ExtCommunityPolicyTest, AddExtCommunitiesEmptyToAddTest) {
  /* Test adding empty list */
  BgpAttrExtCommunitiesC current{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
  }};
  BgpAttrExtCommunitiesC toAdd;

  auto result = addExtCommunities(current, toAdd);

  EXPECT_EQ(1, result.size());
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
}

/*
 * ============================================================================
 * removeExtCommunities Tests
 * ============================================================================
 */

TEST_F(ExtCommunityPolicyTest, RemoveExtCommunitiesSpecificTest) {
  /* Test removing specific extended communities */
  BgpAttrExtCommunitiesC input{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x40041111, 0x501502f9},
      BgpAttrExtCommunityC{0x01010A0A, 0x0A05CAFE},
  }};
  BgpAttrExtCommunitiesC toRemove{{
      BgpAttrExtCommunityC{0x40041111, 0x501502f9},
  }};

  auto result = removeExtCommunities(input, toRemove);

  EXPECT_EQ(2, result.size());
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
  EXPECT_FALSE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x40041111, 0x501502f9}));
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x01010A0A, 0x0A05CAFE}));
}

TEST_F(ExtCommunityPolicyTest, RemoveExtCommunitiesNonExistentTest) {
  /* Test removing extended communities that don't exist */
  BgpAttrExtCommunitiesC input{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x40041111, 0x501502f9},
  }};
  BgpAttrExtCommunitiesC toRemove{{
      BgpAttrExtCommunityC{0x99999999, 0x99999999}, /* doesn't exist */
  }};

  auto result = removeExtCommunities(input, toRemove);

  EXPECT_EQ(2, result.size());
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x40041111, 0x501502f9}));
}

TEST_F(ExtCommunityPolicyTest, RemoveExtCommunitiesAllTest) {
  /* Test removing all extended communities */
  BgpAttrExtCommunitiesC input{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x40041111, 0x501502f9},
  }};
  BgpAttrExtCommunitiesC toRemove{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x40041111, 0x501502f9},
  }};

  auto result = removeExtCommunities(input, toRemove);

  EXPECT_EQ(0, result.size());
}

TEST_F(ExtCommunityPolicyTest, RemoveExtCommunitiesFromEmptyTest) {
  /* Test removing from empty list */
  BgpAttrExtCommunitiesC input;
  BgpAttrExtCommunitiesC toRemove{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
  }};

  auto result = removeExtCommunities(input, toRemove);

  EXPECT_EQ(0, result.size());
}

TEST_F(ExtCommunityPolicyTest, RemoveExtCommunitiesEmptyToRemoveTest) {
  /* Test removing empty list */
  BgpAttrExtCommunitiesC input{{
      BgpAttrExtCommunityC{0x00011234, 0x600DCAFE},
      BgpAttrExtCommunityC{0x40041111, 0x501502f9},
  }};
  BgpAttrExtCommunitiesC toRemove;

  auto result = removeExtCommunities(input, toRemove);

  EXPECT_EQ(2, result.size());
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x00011234, 0x600DCAFE}));
  EXPECT_TRUE(
      hasExtCommunity(result, BgpAttrExtCommunityC{0x40041111, 0x501502f9}));
}

TEST_F(ExtCommunityPolicyTest, RemoveExtCommunitiesLinkBandwidthTest) {
  /* Test removing Link Bandwidth extended communities */
  BgpExtCommunityLinkBandWidthTypeC lbw1(12345, 1.25e+9f); /* 10G */
  BgpExtCommunityLinkBandWidthTypeC lbw2(65001, 1.25e+10f); /* 100G */
  BgpExtCommunityLinkBandWidthTypeC lbw3(12345, 1.25e+8f); /* 1G */

  BgpAttrExtCommunitiesC input{{
      BgpAttrExtCommunityC(lbw1),
      BgpAttrExtCommunityC(lbw2),
      BgpAttrExtCommunityC(lbw3),
  }};
  BgpAttrExtCommunitiesC toRemove{{
      BgpAttrExtCommunityC(lbw2),
  }};

  auto result = removeExtCommunities(input, toRemove);

  EXPECT_EQ(2, result.size());
  EXPECT_TRUE(hasExtCommunity(result, BgpAttrExtCommunityC(lbw1)));
  EXPECT_FALSE(hasExtCommunity(result, BgpAttrExtCommunityC(lbw2)));
  EXPECT_TRUE(hasExtCommunity(result, BgpAttrExtCommunityC(lbw3)));
}

/*
 * An LBW egress action can be reached by an injected/synthesized route (the
 * offline simulator's injection path) whose BgpPolicyActionData carries no
 * lbwActionData. applyAction must treat that as a no-op rather than
 * CHECK-aborting: the LBW ext-community already on the path is left untouched.
 */
TEST_F(ExtCommunityPolicyTest, LbwActionWithoutLbwActionDataIsNoOp) {
  auto action = std::make_shared<LbwExtCommunityAction>(
      createBgpPolicyLbwExtCommunityAction(
          bgp_policy::LbwExtCommunityActionType::DISABLE));

  auto path = createBgpPath();
  path->setNonTransitiveLbwExtCommunity(/*asn=*/65000, /*lbwValue=*/1e9);
  ASSERT_TRUE(path->hasNonTransitiveLbwExtCommunity());

  auto data = std::make_shared<BgpPolicyActionData>(
      /*switchId=*/std::nullopt,
      /*multiPathSize=*/std::nullopt,
      /*lbwActionData=*/std::nullopt);

  action->applyAction(path, data);

  /*
   * The guard returned before the DISABLE switch, so the LBW ext-community was
   * NOT pruned -- the action was a no-op.
   */
  EXPECT_TRUE(path->hasNonTransitiveLbwExtCommunity());
}

} /* namespace facebook::bgp */
