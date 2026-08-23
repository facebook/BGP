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

#define AdjRib_TEST_FRIENDS                                                    \
  friend class AdjRibPolicyCacheFixture;                                       \
  FRIEND_TEST(AdjRibPolicyCacheFixture, PolicyCacheTest);                      \
  FRIEND_TEST(AdjRibPolicyCacheFixture, MatchOriginEgpSetMedNoopTest);         \
  FRIEND_TEST(                                                                 \
      AdjRibPolicyCacheFixture, CheckIsMedSetByPolicyWithRejectedTest);        \
  FRIEND_TEST(                                                                 \
      AdjRibPolicyCacheFixture,                                                \
      CheckIsMedSetByPolicyWithNoPolicyActionDataTest);                        \
  FRIEND_TEST(AdjRibPolicyCacheFixture, IngressEgressTest);                    \
  FRIEND_TEST(AdjRibOutPolicyCacheFixture, PolicyCacheStaleEviction);          \
  FRIEND_TEST(AdjRibOutPolicyCacheFixture, PolicyCacheLruEviction);            \
  FRIEND_TEST(AdjRibOutPolicyCacheFixture, ImmutablePolicyCacheKeyTest);       \
  FRIEND_TEST(AdjRibOutPolicyCacheFixture, MatchOriginEgpSetCommunityTest);    \
  FRIEND_TEST(                                                                 \
      AdjRibOutPolicyCacheFixture, MatchOriginEgpSetMedExpectedCacheHitTest);  \
  FRIEND_TEST(                                                                 \
      AdjRibOutPolicyCacheFixture, MatchOriginEgpSetMedExpectedCacheMissTest); \
  FRIEND_TEST(                                                                 \
      AdjRibInPolicyCacheFixture, MultipleAcceptedModifiedByPolicyTest);       \
  FRIEND_TEST(                                                                 \
      AdjRibInPolicyCacheFixture, BackupAddrPreservedAfterIngressPolicyTest);  \
  FRIEND_TEST(                                                                 \
      AdjRibInPolicyCacheFixture, MultipleAcceptedNotModifiedByPolicyTest);    \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, MultipleRejectedByPolicyTest);       \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, ImmutablePolicyCacheKeyTest);        \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, ExpectedCacheHitTest);               \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, ExpectedCacheMissTest);              \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, BgpPathTopologyInfoCacheHitTest);    \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, BgpPathTopologyInfoCacheMissTest);

#define AdjRibPolicyCache_TEST_FRIENDS                                         \
  friend class AdjRibPolicyCacheFixture;                                       \
  FRIEND_TEST(AdjRibPolicyCacheFixture, PolicyCacheTest);                      \
  FRIEND_TEST(AdjRibPolicyCacheFixture, PolicyCacheMaskedKeyHashTest);         \
  FRIEND_TEST(AdjRibPolicyCacheFixture, PolicyCacheMaskedKeyEqualToTest);      \
  FRIEND_TEST(AdjRibPolicyCacheFixture, PrefixMaskTest);                       \
  FRIEND_TEST(AdjRibPolicyCacheFixture, BackupAddrMaskTest);                   \
  FRIEND_TEST(AdjRibPolicyCacheFixture, LookupPolicyCacheTest);                \
  FRIEND_TEST(AdjRibPolicyCacheFixture, AddToPolicyCacheTest);                 \
  FRIEND_TEST(AdjRibPolicyCacheFixture, MatchOriginEgpSetMedNoopTest);         \
  FRIEND_TEST(                                                                 \
      AdjRibPolicyCacheFixture, CheckIsMedSetByPolicyWithRejectedTest);        \
  FRIEND_TEST(                                                                 \
      AdjRibPolicyCacheFixture,                                                \
      CheckIsMedSetByPolicyWithNoPolicyActionDataTest);                        \
  FRIEND_TEST(AdjRibPolicyCacheFixture, EvictFromPolicyCacheTest);             \
  FRIEND_TEST(AdjRibOutPolicyCacheFixture, PolicyCacheLruEviction);            \
  FRIEND_TEST(AdjRibOutPolicyCacheFixture, PolicyCacheStaleEviction);          \
  FRIEND_TEST(AdjRibOutPolicyCacheFixture, ImmutablePolicyCacheKeyTest);       \
  FRIEND_TEST(AdjRibOutPolicyCacheFixture, MatchOriginEgpSetCommunityTest);    \
  FRIEND_TEST(                                                                 \
      AdjRibOutPolicyCacheFixture, MatchOriginEgpSetMedExpectedCacheHitTest);  \
  FRIEND_TEST(                                                                 \
      AdjRibOutPolicyCacheFixture, MatchOriginEgpSetMedExpectedCacheMissTest); \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, ImmutablePolicyCacheKeyTest);        \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, ExpectedCacheHitTest);               \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, ExpectedCacheMissTest);              \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, BgpPathTopologyInfoCacheHitTest);    \
  FRIEND_TEST(AdjRibInPolicyCacheFixture, BgpPathTopologyInfoCacheMissTest);

#include <folly/coro/BlockingWait.h>
#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibInUtils.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"

using facebook::network::toIPPrefix;

using namespace facebook::nettools::bgplib;

namespace facebook::bgp {

using namespace ::testing;

class AdjRibPolicyCacheFixture : public AdjRibOutboundFixture {};
class AdjRibInPolicyCacheFixture : public AdjRibProcessPeerAnnouncedFixture {};
class AdjRibOutPolicyCacheFixture : public AdjRibOutboundFixture {};

/**
 * This test checks the correctness of functor
 * PolicyCacheMaskedKeyHash. There are several cases.
 *
 *  Let m1, m2 be masks, and r1, r2 be {pathAttrs, prefix},
 *  and h is the hash function.
 *
 *  There are 3 scenarios for masks, and 3 scenarios for routes.
 *  [1] m1 == m2 -> exactly the same object
 *  [2] m1 ~ m2  -> logically equal masks but different objects
 *  [3] m1 !~ m2 -> logically diff masks => guaranteed different masks.
 *
 *  [A] r1 == r2 -> exactly same routes
 *  [B] r1 ~ r2  -> logically equal routes given the same mask.
 *  [C] r1 !~ r2 -> logically diff routes given the same mask, guaranteed
 *                  different objects.
 *
 * This means we should consider 9 possible scenarios, and their expected
 * outcome. Below we provide a truth table which enumerates the test cases.
 *
 *   Mask    |  Route    |  h(m1, r1) == h(m2, r2)
 * ------------------------------------------------------
 *  m1 == m2 |  r1 == r2 |  true    [1A]
 *  m1 == m2 |  r1  ~ r2 |  true    [1B]
 *  m1 == m2 |  r1 !~ r2 |  false   [1C]
 *  m1  ~ m2 |  r1 == r2 |  false   [2A]
 *  m1  ~ m2 |  r1  ~ r2 |  false   [2B]
 *  m1  ~ m2 |  r1 !~ r2 |  false   [2C]
 *  m1 !~ m2 |  r1 == r2 |  false   [3A]
 *  m1 !~ m2 |  r1  ~ r2 |  false   [3B]
 *  m1 !~ m2 |  r1 !~ r2 |  false   [3C]
 *
 * We can see that in all cases, if the mask is not exactly the same object
 * in memory, then the hash is automatically different.
 */
TEST_F(AdjRibPolicyCacheFixture, PolicyCacheMaskedKeyHashTest) {
  const PolicyAttributesMask mask5Attrs{
      .origin = true,
      .asPath = true,
      .communities = true,
      .extCommunities = true,
      .prefix = true,
  };
  const PolicyAttributesMask mask3Attrs{
      .asPath = true,
      .communities = true,
      .prefix = true,
  };
  auto fields1 = buildBgpPathFields(
      1 /* as_count */,
      1 /* community_count */,
      2 /* ext_community_count */,
      1 /* cluster_list_count */);
  auto fields2 = buildBgpPathFields(1, 1, 1, 1);

  // Key difference between path2 and path1 is extCommunities.
  auto path1 = std::make_shared<BgpPath>(*fields1);
  auto path2 = std::make_shared<BgpPath>(*fields2);

  // For readability and auto-populating policyActionData.
  auto MaskedKey = [&](const PolicyAttributesMask& mask,
                       const folly::CIDRNetwork& prefix,
                       std::shared_ptr<const BgpPath> path,
                       bool isPartialDrain = false) {
    return AdjRibPolicyCache::PolicyCacheMaskedKey(
        &mask, prefix, path, nullptr /* policyActionData */, isPartialDrain);
  };
  AdjRibPolicyCache::PolicyCacheMaskedKeyHash hasher{};
  // BEGIN TEST CASES
  // 1A) m1 == m2, r1 == r2, true
  EXPECT_EQ(
      hasher(MaskedKey(mask5Attrs, kV4Prefix1, path1)),
      hasher(MaskedKey(mask5Attrs, kV4Prefix1, path1)));

  // 1B) m1 == m2, r1 ~ r2, true
  // r1 ~ r2: extCommunities is different, but mask3Attrs doesn't use
  // extCommunities.
  EXPECT_EQ(
      hasher(MaskedKey(mask3Attrs, kV4Prefix1, path2)),
      hasher(MaskedKey(mask3Attrs, kV4Prefix1, path1)));

  // 1C) m1 == m2, r1 !~ r2, false
  // r1 !~ r2: prefix is different, and mask3Attrs uses prefix.
  EXPECT_NE(
      hasher(MaskedKey(mask3Attrs, kV4Prefix1, path2)),
      hasher(MaskedKey(mask3Attrs, kV4Prefix2, path1)));

  const PolicyAttributesMask mask3AttrsCopy = mask3Attrs;
  // 2A) m1 ~ m2, r1 == r2, false
  EXPECT_NE(
      hasher(MaskedKey(mask3Attrs, kV4Prefix1, path2)),
      hasher(MaskedKey(mask3AttrsCopy, kV4Prefix1, path2)));

  // 2B) m1 ~ m2, r1 ~ r2, false
  // r1 ~ r2: extCommunities is different, but mask3Attrs doesn't use
  // extCommunities.
  EXPECT_NE(
      hasher(MaskedKey(mask3Attrs, kV4Prefix1, path2)),
      hasher(MaskedKey(mask3AttrsCopy, kV4Prefix1, path1)));

  // 2C) m1 ~ m2, r1 !~ r2, false
  // r1 !~ r2: prefix is different, and mask3Attrs uses prefix.
  EXPECT_NE(
      hasher(MaskedKey(mask3Attrs, kV4Prefix1, path2)),
      hasher(MaskedKey(mask3AttrsCopy, kV4Prefix2, path2)));

  // 3A) m1 !~ m2, r1 == r2, false
  EXPECT_NE(
      hasher(MaskedKey(mask5Attrs, kV4Prefix1, path2)),
      hasher(MaskedKey(mask3Attrs, kV4Prefix1, path2)));

  // 3B) m1 !~ m2, r1 ~ r2, false
  const PolicyAttributesMask mask2Attrs{.origin = true, .asPath = true};
  //  1. mask2Attrs and mask3Attrs are physically and logically different.
  //  2. r1 and r2 have the same origin, asPath, communities, and prefix, but
  //     differ in extCommunities, which is ignored. So they are considered
  //     'equivalent' in a sense, meaning both masks would consider r1, r2
  //     as equal. i.e.,
  //      m1(r1) = m1(r2), AND m2(r1) = m2(r2), WITH r1 != r2.
  EXPECT_NE(
      hasher(MaskedKey(mask2Attrs, kV4Prefix1, path2)),
      hasher(MaskedKey(mask3Attrs, kV4Prefix1, path1)));

  // 3C) m1 !~ m2, r1 !~ r2, false
  //  1. mask5Attrs and mask3Attrs are physically and logically different.
  //  2. r1 and r2 have the same origin, asPath, communities, but different
  //     prefix and extCommunities, so they can never be considered logically
  //     equivalent for either mask.
  EXPECT_NE(
      hasher(MaskedKey(mask5Attrs, kV4Prefix1, path2)),
      hasher(MaskedKey(mask3Attrs, kV4Prefix2, path1)));

  auto data1 = std::make_shared<BgpPolicyActionData>();
  auto data2 = std::make_shared<BgpPolicyActionData>();
  data1->isMedSetByPolicy = false;
  data2->isMedSetByPolicy = true;
  EXPECT_EQ(
      hasher(
          AdjRibPolicyCache::PolicyCacheMaskedKey(
              &mask5Attrs, kV4Prefix1, path1, data1, /*isPartialDrain=*/false)),
      hasher(
          AdjRibPolicyCache::PolicyCacheMaskedKey(
              &mask5Attrs,
              kV4Prefix1,
              path1,
              data2,
              /*isPartialDrain=*/false)));

  // isPartialDrain differs => different hash. The drain community is mutated
  // outside policy evaluation, so it is not captured by the masked attrs or
  // policyActionData; keying on isPartialDrain keeps the two states distinct.
  EXPECT_NE(
      hasher(
          MaskedKey(mask5Attrs, kV4Prefix1, path1, /*isPartialDrain=*/false)),
      hasher(
          MaskedKey(mask5Attrs, kV4Prefix1, path1, /*isPartialDrain=*/true)));
}

/**
 * This test checks the correctness of functor
 * PolicyCacheMaskedKeyEqualTo. The cases are the same as the hash,
 * but are included below for completeness so the test
 * description standalone.
 *
 *  Let m1, m2 be masks, and r1, r2 be {pathAttrs, prefix},
 *  and MEq is the equals function.
 *
 *  There are 3 scenarios for masks, and 3 scenarios for routes.
 *  [1] m1 == m2 -> exactly the same object
 *  [2] m1 ~ m2  -> logically equal masks but different objects
 *  [3] m1 !~ m2 -> logically diff masks => guaranteed different masks.
 *
 *  [A] r1 == r2 -> exactly same routes
 *  [B] r1 ~ r2  -> logically equal routes given the same mask.
 *  [C] r1 !~ r2 -> logically diff routes given the same mask, guaranteed
 *                  different objects.
 *
 * Below we provide a truth table which enumerates the test cases.
 *
 *   Mask    |  Route    |  {m1, r1} MEq {m2, r2}
 * ------------------------------------------------------
 *  m1 == m2 |  r1 == r2 |  true    [1A]
 *  m1 == m2 |  r1  ~ r2 |  true    [1B]
 *  m1 == m2 |  r1 !~ r2 |  false   [1C]
 *  m1  ~ m2 |  r1 == r2 |  false   [2A]
 *  m1  ~ m2 |  r1  ~ r2 |  false   [2B]
 *  m1  ~ m2 |  r1 !~ r2 |  false   [2C]
 *  m1 !~ m2 |  r1 == r2 |  false   [3A]
 *  m1 !~ m2 |  r1  ~ r2 |  false   [3B]
 *  m1 !~ m2 |  r1 !~ r2 |  false   [3C]
 *
 */
TEST_F(AdjRibPolicyCacheFixture, PolicyCacheMaskedKeyEqualToTest) {
  const PolicyAttributesMask mask5Attrs{
      .origin = true,
      .asPath = true,
      .communities = true,
      .extCommunities = true,
      .prefix = true,
  };
  const PolicyAttributesMask mask3Attrs{
      .asPath = true,
      .communities = true,
      .prefix = true,
  };
  auto fields1 = buildBgpPathFields(
      1 /* as_count */,
      1 /* community_count */,
      2 /* ext_community_count */,
      1 /* cluster_list_count */);
  auto fields2 = buildBgpPathFields(1, 1, 1, 1);

  // Key difference between path1 and path2 is extCommunities.
  auto path1 = std::make_shared<BgpPath>(*fields1);
  auto path2 = std::make_shared<BgpPath>(*fields2);

  // For readability and auto-populating policyActionData.
  auto MaskedKey = [&](const PolicyAttributesMask& mask,
                       const folly::CIDRNetwork& prefix,
                       std::shared_ptr<const BgpPath> path,
                       bool isPartialDrain = false) {
    return AdjRibPolicyCache::PolicyCacheMaskedKey(
        &mask, prefix, path, nullptr /* policyActionData */, isPartialDrain);
  };
  AdjRibPolicyCache::PolicyCacheMaskedKeyEqualTo maskedEquals{};
  // BEGIN TEST CASES
  // 1A) m1 == m2, r1 == r2, true
  EXPECT_TRUE(maskedEquals(
      MaskedKey(mask5Attrs, kV4Prefix1, path1),
      MaskedKey(mask5Attrs, kV4Prefix1, path1)));

  // 1B) m1 == m2, r1 ~ r2, true
  // r1 ~ r2: extCommunities is different, but mask3Attrs doesn't use
  // extCommunities.
  EXPECT_TRUE(maskedEquals(
      MaskedKey(mask3Attrs, kV4Prefix1, path2),
      MaskedKey(mask3Attrs, kV4Prefix1, path1)));

  // 1C) m1 == m2, r1 !~ r2, false
  // r1 !~ r2: prefix is different, and mask3Attrs uses prefix.
  EXPECT_FALSE(maskedEquals(
      MaskedKey(mask3Attrs, kV4Prefix1, path2),
      MaskedKey(mask3Attrs, kV4Prefix2, path1)));

  const PolicyAttributesMask mask3AttrsCopy = mask3Attrs;
  // 2A) m1 ~ m2, r1 == r2, false
  EXPECT_FALSE(maskedEquals(
      MaskedKey(mask3Attrs, kV4Prefix1, path2),
      MaskedKey(mask3AttrsCopy, kV4Prefix1, path2)));

  // 2B) m1 ~ m2, r1 ~ r2, false
  // r1 ~ r2: extCommunities is different, but mask3Attrs doesn't use
  // extCommunities.
  EXPECT_FALSE(maskedEquals(
      MaskedKey(mask3Attrs, kV4Prefix1, path2),
      MaskedKey(mask3AttrsCopy, kV4Prefix1, path1)));

  // 2C) m1 ~ m2, r1 !~ r2, false
  // r1 !~ r2: prefix is different, and mask3Attrs uses prefix.
  EXPECT_FALSE(maskedEquals(
      MaskedKey(mask3Attrs, kV4Prefix1, path2),
      MaskedKey(mask3AttrsCopy, kV4Prefix2, path2)));

  // 3A) m1 !~ m2, r1 == r2, false
  EXPECT_FALSE(maskedEquals(
      MaskedKey(mask5Attrs, kV4Prefix1, path2),
      MaskedKey(mask3Attrs, kV4Prefix1, path2)));

  // 3B) m1 !~ m2, r1 ~ r2, false
  const PolicyAttributesMask mask2Attrs{.origin = true, .asPath = true};
  //  1. mask2Attrs and mask3Attrs are physically and logically different.
  //  2. r1 and r2 have the same origin, asPath, communities, and prefix, but
  //     differ in extCommunities, which is ignored. So they are considered
  //     'equivalent' in a sense, meaning both masks would consider r1, r2
  //     as equal. i.e.,
  //      m1(r1) = m1(r2), AND m2(r1) = m2(r2), WITH r1 != r2.
  EXPECT_FALSE(maskedEquals(
      MaskedKey(mask2Attrs, kV4Prefix1, path2),
      MaskedKey(mask3Attrs, kV4Prefix1, path1)));

  // 3C) m1 !~ m2, r1 !~ r2, false
  //  1. mask5Attrs and mask3Attrs are physically and logically different.
  //  2. r1 and r2 have the same origin, asPath, communities, but different
  //     prefix and extCommunities, so they can never be considered logically
  //     equivalent for either mask.
  EXPECT_FALSE(maskedEquals(
      MaskedKey(mask5Attrs, kV4Prefix1, path2),
      MaskedKey(mask3Attrs, kV4Prefix2, path1)));

  auto data1 = std::make_shared<BgpPolicyActionData>();
  auto data2 = std::make_shared<BgpPolicyActionData>();
  data1->isMedSetByPolicy = false;
  data2->isMedSetByPolicy = true;
  EXPECT_TRUE(*data1 == *data2);
  EXPECT_TRUE(maskedEquals(
      AdjRibPolicyCache::PolicyCacheMaskedKey(
          &mask5Attrs, kV4Prefix1, path1, data1, /*isPartialDrain=*/false),
      AdjRibPolicyCache::PolicyCacheMaskedKey(
          &mask5Attrs, kV4Prefix1, path1, data2, /*isPartialDrain=*/false)));

  // isPartialDrain differs => keys are NOT equal, even though every masked
  // attribute is identical.
  EXPECT_FALSE(maskedEquals(
      MaskedKey(mask5Attrs, kV4Prefix1, path1, /*isPartialDrain=*/false),
      MaskedKey(mask5Attrs, kV4Prefix1, path1, /*isPartialDrain=*/true)));
  // Same isPartialDrain => keys remain equal.
  EXPECT_TRUE(maskedEquals(
      MaskedKey(mask5Attrs, kV4Prefix1, path1, /*isPartialDrain=*/true),
      MaskedKey(mask5Attrs, kV4Prefix1, path1, /*isPartialDrain=*/true)));
}

TEST_F(AdjRibPolicyCacheFixture, PrefixMaskTest) {
  const PolicyAttributesMask noPrefix{.communities = true, .prefix = false};
  const PolicyAttributesMask withPrefix{.communities = true, .prefix = true};
  auto fields = buildBgpPathFields(
      1 /* as_count */,
      1 /* community_count */,
      0 /* ext_community_count */,
      0 /* cluster_list_count */);
  auto path = std::make_shared<BgpPath>(*fields);

  AdjRibPolicyCache::PolicyCacheMaskedKeyHash hasher{};
  AdjRibPolicyCache::PolicyCacheMaskedKeyEqualTo equals{};
  // Case 1: Ignoring prefix
  {
    AdjRibPolicyCache::PolicyCacheMaskedKey key1(
        &noPrefix, kV4Prefix1, path, nullptr /* policyActionData */, false);
    AdjRibPolicyCache::PolicyCacheMaskedKey key2(
        &noPrefix, kV4Prefix2, path, nullptr /* policyActionData */, false);

    EXPECT_EQ(hasher(key1), hasher(key1));
    EXPECT_TRUE(equals(key1, key1));
    EXPECT_EQ(hasher(key2), hasher(key2));
    EXPECT_TRUE(equals(key2, key2));

    EXPECT_EQ(hasher(key1), hasher(key2));
    EXPECT_TRUE(equals(key1, key2));
  }
  // Case 2: Including prefix
  {
    AdjRibPolicyCache::PolicyCacheMaskedKey key1(
        &withPrefix, kV4Prefix1, path, nullptr /* policyActionData */, false);
    AdjRibPolicyCache::PolicyCacheMaskedKey key2(
        &withPrefix, kV4Prefix2, path, nullptr /* policyActionData */, false);

    EXPECT_EQ(hasher(key1), hasher(key1));
    EXPECT_TRUE(equals(key1, key1));
    EXPECT_EQ(hasher(key2), hasher(key2));
    EXPECT_TRUE(equals(key2, key2));

    EXPECT_NE(hasher(key1), hasher(key2));
    EXPECT_FALSE(equals(key1, key2));
  }
}

TEST_F(AdjRibPolicyCacheFixture, BackupAddrMaskTest) {
  const PolicyAttributesMask noBackupAddr;
  const PolicyAttributesMask withBackupAddr{.backupAddr = true};
  auto path1 = std::make_shared<BgpPath>(*buildBgpPathFields(0, 0, 0, 0));
  auto path2 = path1->clone();
  path1->setBackupAddr(folly::IPAddress("2001:db8::1"));
  path2->setBackupAddr(folly::IPAddress("2001:db8::2"));

  const auto maskedKey = [&](const PolicyAttributesMask& mask,
                             std::shared_ptr<const BgpPath> path) {
    return AdjRibPolicyCache::PolicyCacheMaskedKey(
        &mask,
        kV6Prefix1,
        std::move(path),
        nullptr /* policyActionData */,
        false /* isPartialDrain */);
  };
  AdjRibPolicyCache::PolicyCacheMaskedKeyHash hasher;
  AdjRibPolicyCache::PolicyCacheMaskedKeyEqualTo equals;

  const auto unmaskedKey1 = maskedKey(noBackupAddr, path1);
  const auto unmaskedKey2 = maskedKey(noBackupAddr, path2);
  EXPECT_EQ(hasher(unmaskedKey1), hasher(unmaskedKey2));
  EXPECT_TRUE(equals(unmaskedKey1, unmaskedKey2));

  const auto maskedKey1 = maskedKey(withBackupAddr, path1);
  const auto maskedKey2 = maskedKey(withBackupAddr, path2);
  EXPECT_NE(hasher(maskedKey1), hasher(maskedKey2));
  EXPECT_FALSE(equals(maskedKey1, maskedKey2));
}

TEST_F(AdjRibPolicyCacheFixture, BackupAddrPreservedOnCacheHitTest) {
  auto cache = AdjRibPolicyCache::get();
  cache->setCacheSize(1);
  const PolicyAttributesMask dummyMask;
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(0, 0, 0, 0));
  const auto backupAddr = folly::IPAddress("2001:db8::1");
  attrs->setBackupAddr(backupAddr);
  auto attrsAndPolicy =
      std::make_shared<routing::AttributesAndPolicy<BgpPath>>(attrs, "term");

  cache->addToPolicyCache(
      "Dummy",
      &dummyMask,
      kV4Prefix1,
      nullptr /* attrs */,
      nullptr /* policyActionData */,
      attrsAndPolicy);
  auto result = cache->lookupPolicyCache(
      "Dummy",
      &dummyMask,
      kV4Prefix1,
      nullptr /* attrs */,
      nullptr /* policyActionData */);

  ASSERT_TRUE(result.has_value());
  ASSERT_NE(nullptr, result->attrsAndPolicy);
  ASSERT_NE(nullptr, result->attrsAndPolicy->attrs);
  EXPECT_EQ(backupAddr, result->attrsAndPolicy->attrs->getBackupAddr());
  cache->setCacheSize(0);
}

/**
 * This test covers LRU eviction with masked keys. The test
 * flow is as follows:
 *
 *  1. Set the cache size to 2.
 *  2. Set up policy manager as deny IGP, accept all policy.
 *     Generated mask is expected to be M{.origin = true}.
 *  3. Create routes
 *      A <pfx1, attrs(origin=IGP)>
 *      B <pfx1, attrs(origin=EGP)>
 *      C <pfx2, attrs(origin=EGP)>
 *      D <pfx3, attrs(origin=INCOMPLETE)>
 *  4. Insert into policy cache in order ABCD.
 *
 * Cache state at each insertion:
 *  Post-Insert A: {
 *    <pfx1, origin=IGP> -> nullptr
 *  }
 *
 *  Post-Insert B: {
 *    <pfx1, origin=EGP> -> postAttrsEgp
 *    <pfx1, origin=IGP> -> nullptr
 *  }
 *
 *  Post-Insert C: {
 *    <pfx1, origin=EGP> -> postAttrsEgp [*]
 *    <pfx1, origin=IGP> -> nullptr
 *  }
 *    [*] due to M(B) ~ M(C), M ignoring prefix.
 *
 *  Post-Insert D: {
 *    <pfx3, origin=INCOMPLETE> -> postAttrsIncomplete
 *    <pfx1, origin=EGP> -> postAttrsEgp
 *  }
 */
TEST_F(AdjRibOutPolicyCacheFixture, PolicyCacheLruEviction) {
  // Used for fine grain control over message posting.
  folly::fibers::Baton pfx1EgpUpdateBaton;
  folly::fibers::Baton pfx2EgpUpdateBaton;
  folly::fibers::Baton pfx3IncompleteUpdateBaton;

  // Create a policy with two terms
  // Term1 match origin IGP and deny
  // Term2 permit all
  auto policyManager = setupDenyIgpOriginAcceptAllPolicy(kEgressPolicyName);
  const PolicyAttributesMask expectedMask{.origin = true};
  EXPECT_EQ(
      expectedMask, *policyManager->getPolicyAttributesMask(kEgressPolicyName));
  // IBGP peer
  setupAdjRib(policyManager, kEgressPolicyName);

  adjRib_->policyCache_->policyLruCache_.wlock()->clear();
  adjRib_->policyCache_->setCacheSize(2);
  adjRib_->policyCache_->setLruClearSize(1);

  EXPECT_EQ(2, adjRib_->policyCache_->getCacheSize());
  fm_->addTask([&] {
    {
      // Announcement 1 (IGP origin) for prefix1 will be denied by policy
      auto ribMsg = createRibSingleAnnounce(
          kV4Prefix1,
          kV4Nexthop1,
          localPeerV4_,
          true, // EOR is true.
          BgpAttrOrigin::BGP_ORIGIN_IGP);
      adjRib_->processRibMessage(ribMsg);
    }
    {
      facebook::bgp::test::boundedBatonWait(
          pfx1EgpUpdateBaton, "pfx1EgpUpdateBaton");
      // Announcement 2 (EGP origin) for prefix1 will be accepted by
      // policy
      auto ribMsg = createRibSingleAnnounce(
          kV4Prefix1,
          kV4Nexthop1,
          localPeerV4_,
          false,
          BgpAttrOrigin::BGP_ORIGIN_EGP);
      adjRib_->processRibMessage(ribMsg);
    }
    {
      facebook::bgp::test::boundedBatonWait(
          pfx2EgpUpdateBaton, "pfx2EgpUpdateBaton");
      // Announcement 3 (EGP origin) for prefix2 will be accepted by
      // policy
      auto ribMsg = createRibSingleAnnounce(
          kV4Prefix2,
          kV4Nexthop1,
          localPeerV4_,
          false,
          BgpAttrOrigin::BGP_ORIGIN_EGP);
      adjRib_->processRibMessage(ribMsg);
    }
    {
      facebook::bgp::test::boundedBatonWait(
          pfx3IncompleteUpdateBaton, "pfx3IncompleteUpdateBaton");
      // Announcement 3 (origin INCOMPLETE) for prefix3 will be accepted by
      // policy
      auto ribMsg = createRibSingleAnnounce(
          kV4Prefix3,
          kV4Nexthop1,
          localPeerV4_,
          false,
          BgpAttrOrigin::BGP_ORIGIN_INCOMPLETE);
      adjRib_->processRibMessage(ribMsg);
    }
  });

  fm_->addTask([&] {
    // Announcement 1 will not lead to any bgp update but
    // we should see v4 and v6 EoRs
    auto msg =
        facebook::bgp::test::boundedBlockingPop(*adjRibOutQ_, "adjRibOutQ_");
    ASSERT_TRUE(std::holds_alternative<BgpEndOfRib>(*msg));
    msg = facebook::bgp::test::boundedBlockingPop(*adjRibOutQ_, "adjRibOutQ_");
    ASSERT_TRUE(std::holds_alternative<BgpEndOfRib>(*msg));

    EXPECT_TRUE(adjRibOutQ_->empty());
    auto adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
    auto igpPreOut = adjRibEntry->getPreOut();
    ASSERT_NE(nullptr, igpPreOut);
    ASSERT_EQ(nullptr, adjRibEntry->getPostAttr());
    EXPECT_EQ(1, adjRib_->policyCache_->size());
    // Verify policy-cache has only one entry.
    // Entry will be for the IGP attributes.
    // As this is a drop case, we should expect the cached entry's
    // post-attrs to be nullptr.
    {
      auto cacheEntry =
          (adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*(adjRibEntry->getPreOut()), *(std::get<2>(cacheEntry->first)));
      auto& cachedAttrs = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(nullptr, cachedAttrs);
    }
    // send the 2nd update
    pfx1EgpUpdateBaton.post();
    fiberSleepFor(10ms);
    // Verifying only after Announcement 2 is sent
    msg = facebook::bgp::test::boundedBlockingPop(*adjRibOutQ_, "adjRibOutQ_");
    ASSERT_TRUE(
        std::holds_alternative<std::shared_ptr<const BgpUpdate2>>(*msg));
    auto bgpUpdate = std::get<std::shared_ptr<const BgpUpdate2>>(*msg);
    ASSERT_EQ(1, bgpUpdate->mpAnnounced()->prefixes()->size());
    EXPECT_EQ(
        facebook::network::toIPPrefix(kV4Prefix1),
        *bgpUpdate->mpAnnounced()->prefixes()[0].prefix());
    EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *bgpUpdate->attrs()->origin());

    // Verify adjrib entry is proper
    adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
    ASSERT_NE(nullptr, adjRibEntry->getPreOut());
    ASSERT_NE(nullptr, adjRibEntry->getPostAttr());
    EXPECT_EQ(adjRibEntry->getPreOut(), adjRibEntry->getPostAttr());
    EXPECT_EQ(
        BgpAttrOrigin::BGP_ORIGIN_EGP, adjRibEntry->getPreOut()->getOrigin());

    // Verify policy-cache has 2 entries:
    // first exactly same as the prev case and in addition
    // we should see a new entry for the EGP attrs.
    EXPECT_EQ(2, adjRib_->policyCache_->size());
    {
      auto cacheEntry =
          (adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*(adjRibEntry->getPreOut()), *(std::get<2>(cacheEntry->first)));
      auto& cachedAttrs = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(*(adjRibEntry->getPostAttr()), *cachedAttrs);

      cacheEntry = ++(adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*igpPreOut, *(std::get<2>(cacheEntry->first)));
      auto& cachedAttrs2 = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(nullptr, cachedAttrs2);
    }
    // Verify stats
    EXPECT_EQ(1, adjRib_->getStats().getPostOutPrefixCount());

    // Perform cache lookup -
    // egpPreOut key is MRU and igpPreOut key is LRU.
    {
      const auto preCacheHitCount = adjRib_->policyCache_->getTotalCacheHit();
      const auto preCacheMissCount = adjRib_->policyCache_->getTotalCacheMiss();
      auto result = adjRib_->policyCache_->lookupPolicyCache(
          kEgressPolicyName,
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          kV4Prefix1,
          igpPreOut,
          adjRib_->createPolicyActionData(igpPreOut));
      EXPECT_NE(std::nullopt, result);
      EXPECT_EQ(nullptr, (*result).attrsAndPolicy->attrs);
      EXPECT_EQ(
          preCacheHitCount + 1, adjRib_->policyCache_->getTotalCacheHit());
      EXPECT_EQ(preCacheMissCount, adjRib_->policyCache_->getTotalCacheMiss());

      auto egpPreOut = adjRibEntry->getPreOut();
      result = adjRib_->policyCache_->lookupPolicyCache(
          kEgressPolicyName,
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          kV4Prefix1,
          egpPreOut,
          adjRib_->createPolicyActionData(egpPreOut));
      const auto attrsAndPolicy = (*result).attrsAndPolicy;
      EXPECT_EQ(*adjRibEntry->getPostAttr(), *attrsAndPolicy->attrs);
      EXPECT_EQ(
          preCacheHitCount + 2, adjRib_->policyCache_->getTotalCacheHit());
      EXPECT_EQ(preCacheMissCount, adjRib_->policyCache_->getTotalCacheMiss());

      // Verify cache is still in tact.
      EXPECT_EQ(2, adjRib_->policyCache_->size());
    }
    {
      auto cacheEntry =
          (adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*adjRibEntry->getPreOut(), *std::get<2>(cacheEntry->first));
      auto& cachedAttrs = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(*adjRibEntry->getPostAttr(), *cachedAttrs);

      cacheEntry = ++(adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*igpPreOut, *std::get<2>(cacheEntry->first));
      auto& cachedAttrs2 = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(nullptr, cachedAttrs2);
    }

    // After sending the 3rd update Verify that LRU entry i.e. corresponding to
    // igpPreOut is not evicted because prefix2 entry has the same EGP origin,
    // and matches to existing EGP entry for prefix1. Both entries still
    // belong to prefix1.
    pfx2EgpUpdateBaton.post();
    fiberSleepFor(10ms);
    msg = facebook::bgp::test::boundedBlockingPop(*adjRibOutQ_, "adjRibOutQ_");
    EXPECT_EQ(2, adjRib_->policyCache_->size());
    {
      adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
      auto cacheEntry =
          (adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(
          adjRibEntry->getPreOut()->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);
      EXPECT_EQ(*adjRibEntry->getPreOut(), *std::get<2>(cacheEntry->first));
      auto& cachedAttrs = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(*adjRibEntry->getPostAttr(), *cachedAttrs);

      cacheEntry = ++(adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*igpPreOut, *std::get<2>(cacheEntry->first));
      auto& cachedAttrs2 = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(nullptr, cachedAttrs2);
    }

    // After sending the 3rd update Verify that LRU entry i.e. corresponding to
    // igpPreOut is evicted and is replaced with the prefix3 entry.
    // Cache should now have ORIGIN_INCOMPLETE with prefix3 and EGP with
    // prefix1.
    pfx3IncompleteUpdateBaton.post();
    fiberSleepFor(10ms);
    msg = facebook::bgp::test::boundedBlockingPop(*adjRibOutQ_, "adjRibOutQ_");
    EXPECT_EQ(2, adjRib_->policyCache_->size());
    {
      adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix3);
      auto cacheEntry =
          (adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix3, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*adjRibEntry->getPreOut(), *std::get<2>(cacheEntry->first));
      EXPECT_EQ(
          adjRibEntry->getPreOut()->getOrigin(),
          BgpAttrOrigin::BGP_ORIGIN_INCOMPLETE);
      auto& cachedAttrs = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(*adjRibEntry->getPostAttr(), *cachedAttrs);

      adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
      cacheEntry = ++(adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(
          adjRibEntry->getPreOut()->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);
      EXPECT_EQ(*adjRibEntry->getPreOut(), *std::get<2>(cacheEntry->first));
      auto& cachedAttrs2 = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(*adjRibEntry->getPostAttr(), *cachedAttrs2);
    }

    // Explicitly closing fibers to avoid unclean exit of adjrib
    terminateAdjRib();
  });

  evb_.loop();
}

TEST_F(AdjRibOutPolicyCacheFixture, PolicyCacheStaleEviction) {
  // Used for fine grain control over message posting.
  folly::fibers::Baton baton;

  // Create a policy with two terms
  // Term1 match origin IGP and deny
  // Term2 permit all
  auto policyManager = setupDenyIgpOriginAcceptAllPolicy(kEgressPolicyName);
  // IBGP peer
  setupAdjRib(policyManager, kEgressPolicyName);

  fm_->addTask([&] {
    {
      // Announcement 1 which will be denied by policy
      auto ribMsg = createRibSingleAnnounce(
          kV4Prefix1,
          kV4Nexthop1,
          localPeerV4_,
          true, // EOR is true.
          BgpAttrOrigin::BGP_ORIGIN_IGP);
      adjRib_->processRibMessage(ribMsg);
    }
    {
      baton.wait();
      // Announcement 2 (modified origin) for same prefix will be accepted by
      // policy
      auto ribMsg = createRibSingleAnnounce(
          kV4Prefix1,
          kV4Nexthop1,
          localPeerV4_,
          false,
          BgpAttrOrigin::BGP_ORIGIN_EGP);
      adjRib_->processRibMessage(ribMsg);
    }
  });

  fm_->addTask([&] {
    // Announcement 1 will not lead to any bgp update but
    // we should see v4 and v6 EoRs
    auto msg =
        facebook::bgp::test::boundedBlockingPop(*adjRibOutQ_, "adjRibOutQ_");
    ASSERT_TRUE(std::holds_alternative<BgpEndOfRib>(*msg));
    msg = facebook::bgp::test::boundedBlockingPop(*adjRibOutQ_, "adjRibOutQ_");
    ASSERT_TRUE(std::holds_alternative<BgpEndOfRib>(*msg));

    EXPECT_TRUE(adjRibOutQ_->empty());
    auto adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
    auto igpPreOut = adjRibEntry->getPreOut();
    ASSERT_NE(nullptr, igpPreOut);
    ASSERT_EQ(nullptr, adjRibEntry->getPostAttr());
    EXPECT_EQ(1, adjRib_->policyCache_->size());
    // Verify policy-cache has only one entry.
    // Entry will be for the IGP attributes.
    // As this is a drop case, we should expect the cached entry's
    // post-attrs to be nullptr.
    {
      auto cacheEntry =
          (adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*(adjRibEntry->getPreOut()), *(std::get<2>(cacheEntry->first)));
      auto& cachedAttrs = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(nullptr, cachedAttrs);
    }
    // send the 2nd update
    baton.post();
    fiberSleepFor(10ms);
    // Verifying only after Announcement 2 is sent
    msg = facebook::bgp::test::boundedBlockingPop(*adjRibOutQ_, "adjRibOutQ_");
    ASSERT_TRUE(
        std::holds_alternative<std::shared_ptr<const BgpUpdate2>>(*msg));
    auto bgpUpdate = std::get<std::shared_ptr<const BgpUpdate2>>(*msg);
    ASSERT_EQ(1, bgpUpdate->mpAnnounced()->prefixes()->size());
    EXPECT_EQ(
        facebook::network::toIPPrefix(kV4Prefix1),
        *bgpUpdate->mpAnnounced()->prefixes()[0].prefix());
    EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, *bgpUpdate->attrs()->origin());

    // Verify adjrib entry is proper
    adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
    ASSERT_NE(nullptr, adjRibEntry->getPreOut());
    ASSERT_NE(nullptr, adjRibEntry->getPostAttr());
    EXPECT_EQ(adjRibEntry->getPreOut(), adjRibEntry->getPostAttr());
    EXPECT_EQ(
        BgpAttrOrigin::BGP_ORIGIN_EGP, adjRibEntry->getPreOut()->getOrigin());

    // Verify policy-cache has 2 entries:
    // We should see a new entry for the EGP attrs.
    // Second entry should be the prev case (LRU).
    EXPECT_EQ(2, adjRib_->policyCache_->size());
    {
      auto cacheEntry =
          (adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*adjRibEntry->getPreOut(), *std::get<2>(cacheEntry->first));
      auto& cachedAttrs = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(*adjRibEntry->getPostAttr(), *cachedAttrs);

      cacheEntry = ++(adjRib_->policyCache_->policyLruCache_.rlock()->begin());
      EXPECT_EQ(
          policyManager->getPolicyAttributesMask(kEgressPolicyName),
          std::get<0>(cacheEntry->first));
      EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry->first));
      EXPECT_EQ(*igpPreOut, *(std::get<2>(cacheEntry->first)));
      auto& cachedAttrs2 = cacheEntry->second.attrsAndPolicy->attrs;
      EXPECT_EQ(nullptr, cachedAttrs2);
    }

    // Verify stats
    EXPECT_EQ(1, adjRib_->getStats().getPostOutPrefixCount());

    // Verify policy lookup - cache hits and miss.
    auto result = adjRib_->policyCache_->lookupPolicyCache(
        kEgressPolicyName,
        policyManager->getPolicyAttributesMask(kEgressPolicyName),
        kV4Prefix1,
        igpPreOut,
        adjRib_->createPolicyActionData(igpPreOut));
    EXPECT_EQ(nullptr, (*result).attrsAndPolicy->attrs);

    auto egpPreOut = adjRibEntry->getPreOut();
    result = adjRib_->policyCache_->lookupPolicyCache(
        kEgressPolicyName,
        policyManager->getPolicyAttributesMask(kEgressPolicyName),
        kV4Prefix1,
        egpPreOut,
        adjRib_->createPolicyActionData(egpPreOut));
    const auto attrsAndPolicy = (*result).attrsAndPolicy;
    EXPECT_EQ(*adjRibEntry->getPostAttr(), *attrsAndPolicy->attrs);

    auto result2 = adjRib_->policyCache_->lookupPolicyCache(
        kEgressPolicyName,
        policyManager->getPolicyAttributesMask(kEgressPolicyName),
        kV4Prefix2,
        egpPreOut,
        adjRib_->createPolicyActionData(egpPreOut));
    EXPECT_EQ(result, result2);

    auto result3 = adjRib_->policyCache_->lookupPolicyCache(
        kEgressPolicyName,
        policyManager->getPolicyAttributesMask(kEgressPolicyName),
        kV4Prefix2,
        nullptr /* attrs */,
        adjRib_->createPolicyActionData(egpPreOut));
    EXPECT_EQ(std::nullopt, result3);

    const std::string kDummyPolicyName = std::string();
    const PolicyAttributesMask dummyMask{
        .origin = true, .asPath = true, .extCommunities = true, .prefix = true};
    result = adjRib_->policyCache_->lookupPolicyCache(
        kDummyPolicyName,
        &dummyMask,
        kV4Prefix1,
        egpPreOut,
        adjRib_->createPolicyActionData(egpPreOut));
    EXPECT_EQ(std::nullopt, result);

    // Verify cache eviction for stale-entries.
    // igpPreOut & cached entry hold different ref: the cached entry
    // has LBW ext community updated
    EXPECT_EQ(1, igpPreOut.use_count());
    auto igpPreOutAttrs = igpPreOut->clone();
    // As igpPreOut is not best-path anymore it is removed from the adj's
    // preout. Thus the entry corresponding to it in cache is stale.
    // Simulate cacheEvictionRunCount_ runs of add-cache, this should
    // force eviction of stale entry.
    for (int i = 0; i < adjRib_->policyCache_->cacheEvictionRunCount_; ++i) {
      adjRib_->policyCache_->addToPolicyCache(
          kDummyPolicyName,
          &dummyMask,
          kV4Prefix2,
          nullptr,
          nullptr /* policyActionData */,
          nullptr);
    }

    // Note that this loop above creates the dummy entry in cache also.
    // Verify that the stale attr entry (igp attrs) is purged out.
    // We only have the dummy entry added above
    EXPECT_EQ(1, adjRib_->policyCache_->size());

    adjRib_->policyCache_->policyLruCache_.withRLock([&](const auto& cache) {
      for (const auto& cacheEntry : cache) {
        if (std::get<0>(cacheEntry.first) ==
            policyManager->getPolicyAttributesMask(kEgressPolicyName)) {
          EXPECT_EQ(kV4Prefix1, std::get<1>(cacheEntry.first));
          EXPECT_EQ(
              *(adjRibEntry->getPreOut()), *(std::get<2>(cacheEntry.first)));
          EXPECT_NE(*igpPreOutAttrs, *(std::get<2>(cacheEntry.first)));
          auto& cachedAttrs = cacheEntry.second.attrsAndPolicy->attrs;
          EXPECT_EQ(*adjRibEntry->getPostAttr(), *cachedAttrs);
        } else if (std::get<0>(cacheEntry.first) == &dummyMask) {
          EXPECT_EQ(kV4Prefix2, std::get<1>(cacheEntry.first));
          EXPECT_EQ(nullptr, std::get<2>(cacheEntry.first));
          EXPECT_EQ(nullptr, cacheEntry.second.attrsAndPolicy);
        } else {
          // Assert if there is any other type of entry in cache
          ASSERT_TRUE(false);
        }
      }
    });

    // Explicitly closing fibers to avoid unclean exit of adjrib
    terminateAdjRib();
  });

  evb_.loop();
}

/*
 * Unit test to check lookupPolicyCache
 * 1. Verify lookupPolicyCache returns folly::none when cache size is 0
 */
TEST_F(AdjRibPolicyCacheFixture, LookupPolicyCacheTest) {
  auto cache = AdjRibPolicyCache::get();
  const PolicyAttributesMask dummyMask;

  {
    cache->setCacheSize(0);
    EXPECT_EQ(0, cache->getCacheSize());
    EXPECT_EQ(
        cache->lookupPolicyCache(
            "Dummy",
            &dummyMask,
            kV4Prefix1,
            nullptr /* attrs */,
            nullptr /* policyActionData */),
        std::nullopt);
  }
}

/*
 * Unit test to check addToPolicyCache
 * 1. Verify addToPolicyCache does not add anything to the cache
 *    when cache size is 0
 */
TEST_F(AdjRibPolicyCacheFixture, AddToPolicyCacheTest) {
  auto cache = AdjRibPolicyCache::get();
  const PolicyAttributesMask dummyMask;

  {
    // Verify the default cache size.
    EXPECT_EQ(facebook::bgp::kMaxPolicyCacheEntries, cache->getCacheSize());
  }
  {
    cache->setCacheSize(0);
    cache->addToPolicyCache(
        "Dummy",
        &dummyMask,
        kV4Prefix1,
        nullptr /* attrs */,
        nullptr /* policyActionData */,
        nullptr /* postPolicyAttrsAndTerm */);

    // nothing is added to the cache
    EXPECT_EQ(cache->size(), 0);
  }
}

/**
 * Unit test to evict from policy cache
 * 1. Verify entries are evicted from policy cache if stale(no shared ownership
 * or adjPreOutCount <= 0)
 * 2. Verify entries are not evicted from policy cache if not stale.
 *
 * The mask used is a prefix mask such that cache entries are disambiguated
 * by prefix.
 */
TEST_F(AdjRibPolicyCacheFixture, EvictFromPolicyCacheTest) {
  auto cache = AdjRibPolicyCache::get();
  cache->setCacheSize(3);
  cache->setLruClearSize(2);
  const PolicyAttributesMask prefixMask{.prefix = true};
  // Case 1: Verify entries are evicted from policy cache if stale
  {
    // Add 3 entries to the cache
    auto attrs =
        std::make_shared<const BgpPath>(*buildBgpPathFields(4, 4, 4, 4));
    auto policyManager = setupDenyIgpOriginAcceptAllPolicy(kEgressPolicyName);
    auto policyOut = policyManager->applyPolicy(
        kEgressPolicyName, PolicyInMessage({kV4Prefix1}, attrs->clone()));

    cache->addToPolicyCache(
        kEgressPolicyName,
        &prefixMask,
        kV4Prefix1,
        attrs,
        nullptr /* policyActionData */,
        policyOut.result[kV4Prefix1]);

    cache->addToPolicyCache(
        kEgressPolicyName,
        &prefixMask,
        kV4Prefix2,
        nullptr /* attrs */,
        nullptr /* policyActionData */,
        nullptr /* postAttrsAndTerm */);

    cache->addToPolicyCache(
        kEgressPolicyName,
        &prefixMask,
        kV4Prefix3,
        attrs,
        nullptr /* policyActionData */,
        nullptr /* postAttrsAndTerm */);

    // Verify the cache size
    EXPECT_EQ(cache->size(), 3);

    // Verify cache eviction happened twice due to onAdjPreoutCount <= 0.
    auto& messages = subscribeToLogMessages("", folly::LogLevel::DBG4);

    cache->evictFromPolicyCache();

    EXPECT_EQ(cache->size(), 1);
    EXPECT_EQ(4, messages.size());
    EXPECT_TRUE(
        messages[0].first.getMessage().starts_with(
            "Policy-cache eviction loop started"));
    EXPECT_TRUE(
        messages[1].first.getMessage().starts_with(
            fmt::format(
                "Evicting stale entry of {} {} from policy-cache",
                folly::IPAddress::networkToString(kV4Prefix3),
                "MISSING POLICY RESULT")));
    EXPECT_TRUE(
        messages[2].first.getMessage().starts_with(
            fmt::format(
                "Evicting stale entry of {} {} from policy-cache",
                folly::IPAddress::networkToString(kV4Prefix1),
                policyOut.result[kV4Prefix1]->policyName)));
    EXPECT_TRUE(
        messages[3].first.getMessage().starts_with(
            "Policy-cache eviction loop done"));
  }

  // Case 2: Verify entries are not evicted from policy cache if not stale
  {
    // Both entries are not stale as adjPreOutCount is not <= 0
    cache->addToPolicyCache(
        kEgressPolicyName,
        &prefixMask,
        kV4Prefix1,
        nullptr /* attrs */,
        nullptr /* policyActionData */,
        nullptr /* postAttrsAndTerm */);
    cache->addToPolicyCache(
        kEgressPolicyName,
        &prefixMask,
        kV4Prefix2,
        nullptr /* attrs */,
        nullptr /* policyActionData */,
        nullptr /* postAttrsAndTerm */);

    // Verify the cache size
    EXPECT_EQ(cache->size(), 2);

    // Verify no entry is evicted from the cache
    cache->evictFromPolicyCache();
    EXPECT_EQ(cache->size(), 2);
  }
}

/**
 * E2E test that verifies AdjRibIn and AdjRibOut can both use
 * the policy cache with interleaving, with the pattern of
 * Ingress -> Egress -> Ingress.
 */
TEST_F(AdjRibPolicyCacheFixture, IngressEgressTest) {
  /**
   * 1. Create policy manager with ingress and egress policies that do not
   * modify attributes.
   */
  auto policyManager =
      setupSimpleTwoPolicyManager(kIngressPolicyName, kEgressPolicyName);

  // 2. Set up adjRib with evb.
  setupAdjRib(
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      false, // isRrClient
      false, // isConfedPeer
      NextHopSelfConfigured(false),
      kV4Nexthop1,
      kV6Nexthop1,
      true, // sessionEstablish
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      V4OverV6Nexthop(false),
      EnhancedRouteRefreshNegotiated(false),
      RouteRefreshNegotiated(false),
      (RemovePrivateAsConfigured(false)),
      policyManager,
      kEgressPolicyName,
      kPeerAddr1,
      false, // allowLoopbackReflection
      std::nullopt, // addPathCapa
      kIngressPolicyName);
  // 3. Verify cache is currently empty.
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  std::vector<folly::CIDRNetwork> prefixSet1{kV6Prefix1, kV6Prefix2};
  std::vector<folly::CIDRNetwork> prefixSet2{kV6Prefix2, kV6Prefix3};
  auto peerUpdate1 = createV6BgpUpdateMultipleAnnounce(prefixSet1);
  auto peerUpdate2 = createV6BgpUpdateMultipleAnnounce(prefixSet2);
  auto ribMsg = createRibSingleAnnounce(
      kV6Prefix1,
      kV6Nexthop1,
      localPeerV4_,
      true,
      BgpAttrOrigin::BGP_ORIGIN_EGP);

  // Push updates to AdjRibIn and AdjRibOut for the current adjRib.
  fm_->addTask([&] {
    {
      // Push ingress update 1.
      adjRibInQ_->fiberPush(peerUpdate1);
    }
    {
      // Push egress update 1.
      adjRib_->processRibMessage(ribMsg);
    }
    {
      // Push ingress update 2.
      adjRibInQ_->fiberPush(peerUpdate2);
    }
  });

  // Wait for adjRib to finish evaluating, and then verify the adjRibEntries.
  fm_->addTask([&] {
    fiberSleepFor(30ms);
    EXPECT_TRUE(adjRibInQ_->size() == 0);
    {
      // Verify adjRibEntries in AdjRibIn exist.
      auto inEntry1 = adjRib_->getRibEntry(true /* ingress */, kV6Prefix1);
      auto inEntry2 = adjRib_->getRibEntry(true /* ingress */, kV6Prefix2);
      auto inEntry3 = adjRib_->getRibEntry(true /* ingress */, kV6Prefix3);
      EXPECT_TRUE(inEntry1);
      EXPECT_TRUE(inEntry2);
      EXPECT_TRUE(inEntry3);

      // Verify the postAttrs and policyResult are the same for all
      // AdjRibIn entries.
      auto postTermIn = inEntry1->getPostInPolicy();
      EXPECT_EQ("Accepted/Modified by Ingress term Term1", *postTermIn);
      EXPECT_EQ(postTermIn, inEntry2->getPostInPolicy());
      EXPECT_EQ(postTermIn, inEntry3->getPostInPolicy());

      auto postInAttrs = inEntry1->getPostAttr();
      EXPECT_TRUE(*postInAttrs == *(inEntry2->getPostAttr()));
      EXPECT_TRUE(*postInAttrs == *(inEntry3->getPostAttr()));
    }
    {
      // Verify the adjRibEntry in AdjRibOut exists.
      auto outEntry = adjRib_->getRibEntry(false /* ingress */, kV6Prefix1);
      EXPECT_TRUE(outEntry);

      // Verify the policyResult for AdjRibOut entry.
      EXPECT_EQ(
          "Accepted/Modified by Egress term Term1",
          *outEntry->getPostOutPolicy());
    }
    {
      // Verify policy cache should have 2 entries 2 cache misses 2 cache hits.
      EXPECT_EQ(2, adjRib_->policyCache_->size());
      EXPECT_EQ(2, adjRib_->policyCache_->getTotalCacheMiss());
      EXPECT_EQ(2, adjRib_->policyCache_->getTotalCacheHit());
    }
    // Explicitly closing fibers to avoid unclean exit of adjrib
    terminateAdjRib();
  });
  evb_.loop();
}

/**
 * Because policy cache key contains shared_ptr, we verify
 * that methods that access the cache (processRibAnnouncedEntry)
 * do not modify the key, as map key should be immutable.
 *
 * This test should guard improper cloning on egress side.
 */
TEST_F(AdjRibOutPolicyCacheFixture, ImmutablePolicyCacheKeyTest) {
  // Set up egress policy accept all on AdjRib
  // isRrClient = false AdjRib
  setupAdjRib(
      setupMatchEgpOriginSetCommunityPolicy(kEgressPolicyName), // policyMgr
      kEgressPolicyName,
      false /* sessionEstablished */,
      false /* sendAddPath */);
  // pathIdGenerator is needed to call tryInsertRibOutEntry.
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(true);

  auto path = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 1, 1));
  auto expected = path->clone();

  auto update =
      RibOutAnnouncementEntry(kV4Prefix1, kDefaultPathID, eBgpPeer_, path);
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  adjRib_->processRibAnnouncedEntry(update);

  EXPECT_EQ(1, adjRib_->policyCache_->size());
  auto entry = adjRib_->policyCache_->policyLruCache_.rlock()->begin();
  auto keyBgpPath = std::get<2>(entry->first);
  EXPECT_TRUE(*keyBgpPath == *expected);

  adjRib_->processRibAnnouncedEntry(update);
  EXPECT_EQ(1, adjRib_->policyCache_->size());
  EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheHit());
}

/**
 * Tests that the cache override is applied correctly E2E from
 * start of processRibAnnouncedEntry to end.
 *
 * This test explicitly checks values for an egress policy that matches origin
 * EGP and sets community to kCommunity1.
 */
TEST_F(AdjRibOutPolicyCacheFixture, MatchOriginEgpSetCommunityTest) {
  /*
   * Set up egress policy accept all on AdjRib
   * isRrClient = false AdjRib
   */
  setupAdjRib(
      setupMatchEgpOriginSetCommunityPolicy(kEgressPolicyName), // policyManager
      kEgressPolicyName,
      false /* sessionEstablished */,
      false /* sendAddPath */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(true);

  // path1 and path2 both have Origin EGP.
  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));
  auto path2 = std::make_shared<BgpPath>(
      *buildBgpPathFields(2, 2 /* community_count */, 2, 2));
  EXPECT_EQ(path1->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);
  EXPECT_EQ(path2->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);
  EXPECT_FALSE(path1->getCommunities());
  EXPECT_EQ(2, path2->getCommunities()->size());
  // Fully differentiate the paths aside from origin.
  path1->setNexthop(kV4Nexthop1);
  path2->setNexthop(kV4Nexthop2);

  path1->setMed(0);
  path2->setMed(1);

  path1->setLocalPref(1);
  path2->setLocalPref(100);

  path1->setAtomicAggregate(true);
  path2->setAtomicAggregate(false);

  path1->setAggregator(BgpAttrAggregatorC{.asn = 0, .ip = kAggregatorAddr});
  path2->setAggregator(BgpAttrAggregatorC{.asn = 1, .ip = kEmptyV4PeerAddr});

  path1->setOriginatorId(0);
  path2->setOriginatorId(1);

  auto& policy = adjRib_->policyManager_;

  // match is only origin, action is only communities.
  const PolicyAttributesMask expectedMask{.origin = true, .communities = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kEgressPolicyName));
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  // 1. <kV4Prefix1, path1> should have communities set to kCommunity1.
  auto update =
      RibOutAnnouncementEntry(kV4Prefix1, kPlaceholderPathID, iBgpPeer_, path1);
  adjRib_->processRibAnnouncedEntry(update);
  EXPECT_EQ(1, adjRib_->policyCache_->size());
  /*
   * Verify the expected post-policy path. Only communities field
   * should be changed. asPath can also be changed to remove asn of 0.
   * We check every individual field below.
   */
  auto& postPolicyAttrs1 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1)->getPostAttr();
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, postPolicyAttrs1->getOrigin());
  EXPECT_EQ(1, postPolicyAttrs1->getAsPath()->size());
  EXPECT_EQ("1", postPolicyAttrs1->getFullBgpAsPathAsString().at(0));
  EXPECT_EQ(kV4Nexthop1, postPolicyAttrs1->getNexthop());
  EXPECT_EQ(0, postPolicyAttrs1->getMed());
  EXPECT_EQ(1, postPolicyAttrs1->getLocalPref());
  EXPECT_TRUE(postPolicyAttrs1->getAtomicAggregate());
  EXPECT_EQ(0, postPolicyAttrs1->getOriginatorId());
  BgpAttrExtCommunityC defaultExtComm(
      kExtCommASTypeFirstWord, kExtCommASTypeSecondWord);
  EXPECT_EQ(1, postPolicyAttrs1->getExtCommunities()->size());
  EXPECT_EQ(defaultExtComm, postPolicyAttrs1->getExtCommunities()->at(0));
  EXPECT_EQ(1, postPolicyAttrs1->getClusterList()->size());
  EXPECT_EQ(kClusterIp, postPolicyAttrs1->getClusterList()->at(0));
  EXPECT_EQ(
      (BgpAttrAggregatorC{.asn = 0, .ip = kAggregatorAddr}),
      postPolicyAttrs1->getAggregator());
  EXPECT_EQ(1, postPolicyAttrs1->getCommunities()->size());
  EXPECT_EQ(kCommunity1, postPolicyAttrs1->getCommunities()->at(0).to_string());

  // 2. <kV4Prefix2, path2> should have communities set to kCommunity1.
  auto update2 =
      RibOutAnnouncementEntry(kV4Prefix2, kPlaceholderPathID, iBgpPeer_, path2);
  adjRib_->processRibAnnouncedEntry(update2);

  /*
   * 2a. There should have been cache miss since the two paths have different
   * communities.
   */
  EXPECT_EQ(2, adjRib_->policyCache_->size());
  EXPECT_EQ(0, adjRib_->policyCache_->getTotalCacheHit());

  /*
   * 2b. Verify the expected post-policy path. Only communities field
   * should be changed. asPath can also be changed to remove asn of 0.
   */
  auto& postPolicyAttrs2 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix2)->getPostAttr();
  /*
   * Sanity check that the evaluation result has changed after processing
   * each update.
   */
  EXPECT_FALSE(*postPolicyAttrs1 == *postPolicyAttrs2);
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, postPolicyAttrs2->getOrigin());
  EXPECT_EQ(1, postPolicyAttrs2->getAsPath()->size());
  EXPECT_EQ("1_1", postPolicyAttrs2->getFullBgpAsPathAsString().at(0));
  EXPECT_EQ(kV4Nexthop2, postPolicyAttrs2->getNexthop());
  EXPECT_EQ(1, postPolicyAttrs2->getMed());
  EXPECT_EQ(100, postPolicyAttrs2->getLocalPref());
  EXPECT_FALSE(postPolicyAttrs2->getAtomicAggregate());
  EXPECT_EQ(1, postPolicyAttrs2->getOriginatorId());
  EXPECT_EQ(2, postPolicyAttrs2->getExtCommunities()->size());
  EXPECT_EQ(defaultExtComm, postPolicyAttrs2->getExtCommunities()->at(0));
  EXPECT_EQ(defaultExtComm, postPolicyAttrs2->getExtCommunities()->at(1));
  EXPECT_EQ(2, postPolicyAttrs2->getClusterList()->size());
  EXPECT_EQ(kClusterIp, postPolicyAttrs2->getClusterList()->at(0));
  EXPECT_EQ(kClusterIp, postPolicyAttrs2->getClusterList()->at(1));
  EXPECT_EQ(
      (BgpAttrAggregatorC{.asn = 1, .ip = kEmptyV4PeerAddr}),
      postPolicyAttrs2->getAggregator());
  EXPECT_EQ(1, postPolicyAttrs2->getCommunities()->size());
  EXPECT_EQ(kCommunity1, postPolicyAttrs2->getCommunities()->at(0).to_string());

  /*
   * 3. Just double checking that path3 is rejected by policy due to
   * origin as IGP. Therefore, the value stored in this adjRibEntry's
   * postPolicyAttrs_ after processRibAnnouncedEntry should be nullptr.
   */
  auto path3 = path1->clone();
  path3->setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP);
  auto update3 =
      RibOutAnnouncementEntry(kV4Prefix1, kPlaceholderPathID, iBgpPeer_, path3);
  adjRib_->processRibAnnouncedEntry(update3);
  auto& postPolicyAttrs3 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1)->getPostAttr();
  EXPECT_FALSE(postPolicyAttrs3);
}

/**
 * This test creates a policy that matches EGP and sets med,
 * or else accepts all.
 * Let cache[pfx, path] be the pair value [postPolicyAttrs, isMedSetByPolicy],
 * and let mask by the PolicyAttributesMask defined by @policy.
 *
 * Then, we test that
 *
 *   cache[pfx1, path1] = cache[pfx2, path2]
 *
 * if Mask(path1) = Mask(path2).
 */
TEST_F(AdjRibOutPolicyCacheFixture, MatchOriginEgpSetMedExpectedCacheHitTest) {
  /*
   * Set up egress policy accept all on AdjRib
   * isRrClient = false AdjRib
   */
  setupAdjRib(
      setupMatchEgpOriginSetMedAcceptAllPolicy(kEgressPolicyName),
      kEgressPolicyName,
      false /* sessionEstablished */,
      false /* sendAddPath */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(true);

  // path1, path2 both have Origin EGP.
  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));
  auto path2 = std::make_shared<BgpPath>(
      *buildBgpPathFields(2, 2 /* community_count */, 2, 2));

  EXPECT_EQ(path1->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);
  EXPECT_EQ(path2->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);

  path1->setMed(0);
  path2->setMed(0);

  auto& policy = adjRib_->policyManager_;

  // match is only origin, action is only med.
  const PolicyAttributesMask expectedMask{.origin = true, .med = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kEgressPolicyName));
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  // Helper lambda for readability.
  auto FindEntry = [&](const AdjRibEntry* entry,
                       const folly::CIDRNetwork& prefix,
                       std::shared_ptr<BgpPath>& path)
      -> std::optional<AdjRibPolicyCache::PolicyCacheValue> {
    auto key = AdjRibPolicyCache::PolicyCacheMaskedKey(
        policy->getPolicyAttributesMask(kEgressPolicyName),
        prefix,
        path,
        adjRib_->createPolicyActionData(entry->getPreOut()),
        /*isPartialDrain=*/false);
    auto guard = adjRib_->policyCache_->policyLruCache_.wlock();
    auto iter = guard->find(key);
    if (iter == guard->end()) {
      return std::nullopt;
    }
    return std::make_optional<AdjRibPolicyCache::PolicyCacheValue>(
        iter->second);
  };

  // 1. Process kV4Prefix1, path1.
  auto update1 =
      RibOutAnnouncementEntry(kV4Prefix1, kDefaultPathID, eBgpPeer_, path1);
  adjRib_->processRibAnnouncedEntry(update1);
  EXPECT_EQ(1, adjRib_->policyCache_->size());
  /*
   * 1a. Verify the expected post-policy path med value.
   */
  auto& postPolicyAttrs1 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1)->getPostAttr();
  EXPECT_EQ(kMed, postPolicyAttrs1->getMed());
  /**
   * 1b. Verify isMedSetByPolicy for path1 from cache result is true.
   */
  auto result1 = FindEntry(
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1), kV4Prefix1, path1);
  EXPECT_TRUE(result1);
  EXPECT_EQ(kMed, result1->attrsAndPolicy->attrs->getMed());
  EXPECT_TRUE(result1->isMedSetByPolicy);

  /**
   * 1c. Verify that path2 will result in cache hit from path1 already
   * being in cache because path2 origin = path1 origin and
   * path2 med = path1 med.
   */
  auto update2 =
      RibOutAnnouncementEntry(kV4Prefix2, kDefaultPathID, eBgpPeer_, path2);
  adjRib_->processRibAnnouncedEntry(update2);
  EXPECT_EQ(1, adjRib_->policyCache_->size());

  auto& postPolicyAttrs2 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix2)->getPostAttr();
  EXPECT_EQ(kMed, postPolicyAttrs2->getMed());

  auto result2 = FindEntry(
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix2), kV4Prefix2, path2);
  EXPECT_TRUE(result2);
  EXPECT_EQ(result1, result2);
}

/**
 * This test creates a policy that matches EGP and sets med,
 * or else accepts all.
 * Let cache[pfx, path] be the pair value [postPolicyAttrs, isMedSetByPolicy],
 * and let mask by the PolicyAttributesMask defined by @policy.
 *
 * Then, we test that
 *
 *   postPolicyAttrs1 != postPolicyAttrs2
 *   isMedSetByPolicy1 = isMedSetByPolicy2
 *
 * if path1 and path2 have different med values, but all other
 * attributes under the mask are the same.
 */
TEST_F(AdjRibOutPolicyCacheFixture, MatchOriginEgpSetMedExpectedCacheMissTest) {
  /*
   * Set up egress policy accept all on AdjRib
   * isRrClient = false AdjRib
   */
  setupAdjRib(
      setupMatchEgpOriginSetMedAcceptAllPolicy(kEgressPolicyName),
      kEgressPolicyName,
      false /* sessionEstablished */,
      false /* sendAddPath */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(true);

  // path1, path2 both have Origin EGP.
  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));
  auto path2 = std::make_shared<BgpPath>(
      *buildBgpPathFields(2, 2 /* community_count */, 2, 2));

  EXPECT_EQ(path1->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);
  EXPECT_EQ(path2->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);

  path1->setMed(0);
  path2->setMed(1);

  auto& policy = adjRib_->policyManager_;

  // match is only origin, action is only med.
  const PolicyAttributesMask expectedMask{.origin = true, .med = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kEgressPolicyName));
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  // Helper lambda for readability.
  auto FindEntry = [&](const AdjRibEntry* entry,
                       const folly::CIDRNetwork& prefix,
                       std::shared_ptr<BgpPath>& path)
      -> std::optional<AdjRibPolicyCache::PolicyCacheValue> {
    auto key = AdjRibPolicyCache::PolicyCacheMaskedKey(
        policy->getPolicyAttributesMask(kEgressPolicyName),
        prefix,
        path,
        adjRib_->createPolicyActionData(entry->getPreOut()),
        /*isPartialDrain=*/false);
    auto guard = adjRib_->policyCache_->policyLruCache_.wlock();
    auto iter = guard->find(key);
    if (iter == guard->end()) {
      return std::nullopt;
    }
    return std::make_optional<AdjRibPolicyCache::PolicyCacheValue>(
        iter->second);
  };

  // 1. Process kV4Prefix1, path1.
  auto update1 =
      RibOutAnnouncementEntry(kV4Prefix1, kDefaultPathID, eBgpPeer_, path1);
  adjRib_->processRibAnnouncedEntry(update1);
  EXPECT_EQ(1, adjRib_->policyCache_->size());
  /*
   * 1a. Verify the expected post-policy path med value.
   */
  auto& postPolicyAttrs1 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1)->getPostAttr();
  EXPECT_EQ(kMed, postPolicyAttrs1->getMed());
  /**
   * 1b. Verify isMedSetByPolicy for path1 from cache result is true.
   */
  auto result1 = FindEntry(
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1), kV4Prefix1, path1);
  EXPECT_TRUE(result1);
  EXPECT_EQ(kMed, result1->attrsAndPolicy->attrs->getMed());
  EXPECT_TRUE(result1->isMedSetByPolicy);

  // 2. Process kV4Prefix2, path2.
  auto update2 =
      RibOutAnnouncementEntry(kV4Prefix2, kDefaultPathID, eBgpPeer_, path2);
  adjRib_->processRibAnnouncedEntry(update2);

  /*
   * 2a. There should have been cache miss since the two paths have different
   * med value.
   */
  EXPECT_EQ(2, adjRib_->policyCache_->size());
  EXPECT_EQ(0, adjRib_->policyCache_->getTotalCacheHit());

  /*
   * 2b. Verify the expected post-policy path med value.
   */
  auto& postPolicyAttrs2 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix2)->getPostAttr();
  EXPECT_EQ(kMed, postPolicyAttrs2->getMed());

  /*
   * 2c. Verify isMedSetByPolicy for path2 from cache result is true.
   */
  auto result2 = FindEntry(
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix2), kV4Prefix2, path2);
  EXPECT_TRUE(result2);
  EXPECT_TRUE(result2->isMedSetByPolicy);
}

/**
 * This test creates a policy that matches EGP and sets med,
 * or else accepts all.
 * Let cache[pfx, path] be the pair value [postPolicyAttrs, isMedSetByPolicy],
 * and let mask by the PolicyAttributesMask defined by @policy.
 *
 * Then, we test that
 *
 *   postAttrs1, isMedSetByPolicy1 = cache[pfx1, path1]
 *   and isMedSetByPolicy1 = false
 *
 * if policy evaluation does not execute SetMed action.
 */
TEST_F(AdjRibPolicyCacheFixture, MatchOriginEgpSetMedNoopTest) {
  /*
   * Set up egress policy accept all on AdjRib
   * isRrClient = false AdjRib
   */
  setupAdjRib(
      setupMatchEgpOriginSetMedAcceptAllPolicy(kEgressPolicyName),
      kEgressPolicyName,
      false /* sessionEstablished */,
      false /* sendAddPath */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(true);

  // path1 will have origin IGP but same med value as path2.
  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));
  path1->setMed(1);
  path1->setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP);

  auto& policy = adjRib_->policyManager_;

  // match is only origin, action is only med.
  const PolicyAttributesMask expectedMask{.origin = true, .med = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kEgressPolicyName));
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  /*
   * Verify that isMedSetByPolicy for path1 is false because
   * path1 is IGP.
   */
  auto update1 =
      RibOutAnnouncementEntry(kV4Prefix1, kDefaultPathID, iBgpPeer_, path1);
  adjRib_->processRibAnnouncedEntry(update1);
  auto& postPolicyAttrs1 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1)->getPostAttr();
  EXPECT_EQ(path1->getMed(), postPolicyAttrs1->getMed());

  // Look up path1.
  auto adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
  auto key = AdjRibPolicyCache::PolicyCacheMaskedKey(
      policy->getPolicyAttributesMask(kEgressPolicyName),
      kV4Prefix1,
      path1,
      adjRib_->createPolicyActionData(adjRibEntry->getPreOut()),
      /*isPartialDrain=*/false);
  auto guard = adjRib_->policyCache_->policyLruCache_.wlock();
  auto iter = guard->find(key);

  EXPECT_FALSE(iter->second.isMedSetByPolicy);
}

/**
 * This test creates a policy that denies origin IGP
 * or else accepts all.
 * Let cache[pfx, path] be the pair value [postPolicyAttrs, isMedSetByPolicy],
 * and let mask by the PolicyAttributesMask defined by @policy.
 *
 * Then, we test that
 *
 *   postAttrs1, isMedSetByPolicy1 = cache[pfx1, path1]
 *   and isMedSetByPolicy1 = false
 *
 * and postAttrs1 is nullptr due to policy rejecting path1.
 */
TEST_F(AdjRibPolicyCacheFixture, CheckIsMedSetByPolicyWithRejectedTest) {
  /*
   * Set up egress policy accept all on AdjRib
   * isRrClient = false AdjRib
   */
  setupAdjRib(
      setupDenyIgpOriginAcceptAllPolicy(kEgressPolicyName),
      kEgressPolicyName,
      false /* sessionEstablished */,
      false /* sendAddPath */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(true);

  // path1 will have origin IGP but same med value as path2.
  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));
  path1->setMed(1);
  path1->setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP);

  auto& policy = adjRib_->policyManager_;

  // match is only origin
  const PolicyAttributesMask expectedMask{.origin = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kEgressPolicyName));
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  /*
   * Verify that isMedSetByPolicy for path1 is false because
   * path1 is IGP and was rejected.
   */
  auto update1 =
      RibOutAnnouncementEntry(kV4Prefix1, kDefaultPathID, iBgpPeer_, path1);
  adjRib_->processRibAnnouncedEntry(update1);
  auto& postPolicyAttrs1 =
      adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1)->getPostAttr();
  EXPECT_FALSE(postPolicyAttrs1);

  // Look up path1.
  auto adjRibEntry = adjRib_->getRibEntry(/*ingress=*/false, kV4Prefix1);
  auto key = AdjRibPolicyCache::PolicyCacheMaskedKey(
      policy->getPolicyAttributesMask(kEgressPolicyName),
      kV4Prefix1,
      path1,
      adjRib_->createPolicyActionData(adjRibEntry->getPreOut()),
      /*isPartialDrain=*/false);
  auto guard = adjRib_->policyCache_->policyLruCache_.wlock();
  auto iter = guard->find(key);

  EXPECT_FALSE(iter->second.attrsAndPolicy->attrs);
  EXPECT_FALSE(iter->second.isMedSetByPolicy);
}

/**
 * Check isMedSetByPolicy is false if no BgpPolicyActionData was passed
 * to addToPolicyCache.
 */
TEST_F(
    AdjRibPolicyCacheFixture,
    CheckIsMedSetByPolicyWithNoPolicyActionDataTest) {
  /*
   * Set up egress policy accept all on AdjRib
   * isRrClient = false AdjRib
   */
  setupAdjRib(
      setupDenyIgpOriginAcceptAllPolicy(kEgressPolicyName),
      kEgressPolicyName,
      false /* sessionEstablished */,
      false /* sendAddPath */);
  adjRib_->pathIdGenerator_ = std::make_unique<PathIdGenerator>(true);

  adjRib_->policyCache_->addToPolicyCache(
      kEgressPolicyName,
      adjRib_->policyManager_->getPolicyAttributesMask(kEgressPolicyName),
      kV4Prefix1,
      nullptr /* attrs */,
      nullptr /* policyActionData */,
      nullptr /* postPolicyAttrsAndTerm */);

  EXPECT_EQ(1, adjRib_->policyCache_->size());

  auto entry = adjRib_->policyCache_->policyLruCache_.rlock()->begin();
  EXPECT_FALSE(entry->second.attrsAndPolicy);
  EXPECT_FALSE(entry->second.isMedSetByPolicy);
}

/**
 * ====================== AdjRibIn Policy Cache Tests ========================
 */

/**
 * Tests that the cache override is applied correctly E2E from
 * start of processPeerAnnounced to end. Because processPeerAnnounced
 * can create multiple adjRibEntries, we check the entries to make sure
 * the post-policy attrs are as expected.
 *
 * This test explicitly checks values for an ingress policy that matches origin
 * EGP and sets community to kCommunity1 for kV4Prefix1 and kV4Prefix2.
 */
TEST_F(AdjRibInPolicyCacheFixture, MultipleAcceptedModifiedByPolicyTest) {
  auto policy = setupMatchEgpOriginSetCommunityPolicy(kIngressPolicyName);
  // Set up adjRib with policy manager.
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));

  EXPECT_EQ(path1->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);
  EXPECT_FALSE(path1->getCommunities());

  // match is only origin, action is only communities.
  const PolicyAttributesMask expectedMask{.origin = true, .communities = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kIngressPolicyName));
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);
  folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1, pfx2}, path1));

  // There should only be one cache entry.
  EXPECT_EQ(1, adjRib_->policyCache_->size());
  // Two adjRibs should be created, one for each prefix.
  EXPECT_TRUE(adjRib_->getRibEntry(true /* ingress */, kV4Prefix1));
  EXPECT_TRUE(adjRib_->getRibEntry(true /* ingress */, kV4Prefix2));

  auto& postPolicyAttrs1 =
      adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix1)->getPostAttr();
  auto& postPolicyAttrs2 =
      adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix2)->getPostAttr();

  // The postInAttrs should be the same since there is an attrs cache.
  EXPECT_EQ(postPolicyAttrs1, postPolicyAttrs2);

  /*
   * Verify the expected post-policy path for pfx1.
   * Only communities field should be changed. asPath can also be changed to
   * remove asn of 0.
   * We check every individual field below.
   */
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, postPolicyAttrs1->getOrigin());
  EXPECT_EQ(1, postPolicyAttrs1->getAsPath()->size());
  EXPECT_EQ("1", postPolicyAttrs1->getFullBgpAsPathAsString().at(0));
  EXPECT_EQ(kV4Nexthop1, postPolicyAttrs1->getNexthop());
  EXPECT_EQ(kMed, postPolicyAttrs1->getMed());
  EXPECT_EQ(kLocalPref, postPolicyAttrs1->getLocalPref());
  EXPECT_FALSE(postPolicyAttrs1->getAtomicAggregate());
  EXPECT_EQ(kOriginatorId, postPolicyAttrs1->getOriginatorId());
  BgpAttrExtCommunityC defaultExtComm(
      kExtCommASTypeFirstWord, kExtCommASTypeSecondWord);
  EXPECT_EQ(1, postPolicyAttrs1->getExtCommunities()->size());
  EXPECT_EQ(defaultExtComm, postPolicyAttrs1->getExtCommunities()->at(0));
  EXPECT_EQ(1, postPolicyAttrs1->getClusterList()->size());
  EXPECT_EQ(kClusterIp, postPolicyAttrs1->getClusterList()->at(0));
  EXPECT_EQ(
      (BgpAttrAggregatorC{.asn = 0, .ip = folly::IPAddress()}),
      postPolicyAttrs1->getAggregator());
  EXPECT_EQ(1, postPolicyAttrs1->getCommunities()->size());
  EXPECT_EQ(kCommunity1, postPolicyAttrs1->getCommunities()->at(0).to_string());
}

TEST_F(AdjRibInPolicyCacheFixture, BackupAddrPreservedAfterIngressPolicyTest) {
  const auto backupAddr = folly::IPAddress("2001:db8::1");
  bgp_policy::BgpPolicyAction action;
  action.type() = BgpPolicyActionType::ADD_BACKUP_ADDR;
  action.add_backup_addr() = bgp_policy::AddBackupAddr();
  action.add_backup_addr()->address() = backupAddr.str();
  auto policy = std::make_shared<PolicyManager>(
      createBgpPolicies(kIngressPolicyName, {}, {std::move(action)}),
      createTestBgpGlobalConfig());
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  const PolicyAttributesMask expectedMask{.backupAddr = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kIngressPolicyName));

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);
  auto path = std::make_shared<BgpPath>(*buildBgpPathFields(1, 0, 1, 1));
  folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1, pfx2}, path));

  ASSERT_EQ(1, adjRib_->policyCache_->size());
  for (const auto& prefix : {kV4Prefix1, kV4Prefix2}) {
    const auto* entry = adjRib_->getRibEntry(true /* ingress */, prefix);
    ASSERT_NE(nullptr, entry);
    ASSERT_NE(nullptr, entry->getPostAttr());
    EXPECT_EQ(backupAddr, entry->getPostAttr()->getBackupAddr());
  }
}

/**
 * Tests that the cache override is applied correctly E2E from
 * start of processPeerAnnounced to end. Because processPeerAnnounced
 * can create multiple adjRibEntries, we check the entries to make sure
 * the post-policy attrs are as expected.
 *
 * This test explicitly checks values for an ingress policy that matches
 * denies origin IGP and accepts paths for kV4Prefix1 and kV4Prefix2.
 */
TEST_F(AdjRibInPolicyCacheFixture, MultipleAcceptedNotModifiedByPolicyTest) {
  auto policy = setupDenyIgpOriginAcceptAllPolicy(kIngressPolicyName);
  // Set up adjRib with policy manager.
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));

  EXPECT_EQ(path1->getOrigin(), BgpAttrOrigin::BGP_ORIGIN_EGP);

  // Mask is only origin.
  const PolicyAttributesMask expectedMask{.origin = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kIngressPolicyName));
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);
  folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1, pfx2}, path1));

  // There should only be one cache entry.
  EXPECT_EQ(1, adjRib_->policyCache_->size());
  // Two adjRibs should be created, one for each prefix.
  EXPECT_TRUE(adjRib_->getRibEntry(true /* ingress */, kV4Prefix1));
  EXPECT_TRUE(adjRib_->getRibEntry(true /* ingress */, kV4Prefix2));

  auto& postPolicyAttrs1 =
      adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix1)->getPostAttr();
  auto& postPolicyAttrs2 =
      adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix2)->getPostAttr();

  // The postInAttrs should be the equivalent because of the attrs cache.
  EXPECT_EQ(postPolicyAttrs1, postPolicyAttrs2);

  /*
   * Verify the expected post-policy path for pfx1.
   * Only communities field should be changed. asPath can also be changed to
   * remove asn of 0.
   * We check every individual field below.
   */
  EXPECT_EQ(BgpAttrOrigin::BGP_ORIGIN_EGP, postPolicyAttrs1->getOrigin());
  EXPECT_EQ(1, postPolicyAttrs1->getAsPath()->size());
  EXPECT_EQ("1", postPolicyAttrs1->getFullBgpAsPathAsString().at(0));
  EXPECT_EQ(kV4Nexthop1, postPolicyAttrs1->getNexthop());
  EXPECT_EQ(kMed, postPolicyAttrs1->getMed());
  EXPECT_EQ(kLocalPref, postPolicyAttrs1->getLocalPref());
  EXPECT_FALSE(postPolicyAttrs1->getAtomicAggregate());
  EXPECT_EQ(kOriginatorId, postPolicyAttrs1->getOriginatorId());
  BgpAttrExtCommunityC defaultExtComm(
      kExtCommASTypeFirstWord, kExtCommASTypeSecondWord);
  EXPECT_EQ(1, postPolicyAttrs1->getExtCommunities()->size());
  EXPECT_EQ(defaultExtComm, postPolicyAttrs1->getExtCommunities()->at(0));
  EXPECT_EQ(1, postPolicyAttrs1->getClusterList()->size());
  EXPECT_EQ(kClusterIp, postPolicyAttrs1->getClusterList()->at(0));
  EXPECT_EQ(
      (BgpAttrAggregatorC{.asn = 0, .ip = folly::IPAddress()}),
      postPolicyAttrs1->getAggregator());
  EXPECT_FALSE(postPolicyAttrs1->getCommunities());
}

/**
 * Tests that the cache override is applied correctly E2E from
 * start of processPeerAnnounced to end. Because processPeerAnnounced
 * can create multiple adjRibEntries, we check the entries to make sure
 * the post-policy attrs are as expected.
 *
 * This test explicitly checks values for an ingress policy that
 * denies origin IGP and accepts paths for kV4Prefix1 and kV4Prefix2.
 */
TEST_F(AdjRibInPolicyCacheFixture, MultipleRejectedByPolicyTest) {
  auto policy = setupDenyIgpOriginAcceptAllPolicy(kIngressPolicyName);
  // Set up adjRib with policy manager.
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));
  path1->setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP);

  // Mask is only origin.
  const PolicyAttributesMask expectedMask{.origin = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kIngressPolicyName));
  EXPECT_EQ(0, adjRib_->policyCache_->size());

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);
  folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1, pfx2}, path1));

  // There should only be one cache entry.
  EXPECT_EQ(1, adjRib_->policyCache_->size());
  // Two adjRibs should be created, one for each prefix.
  EXPECT_TRUE(adjRib_->getRibEntry(true /* ingress */, kV4Prefix1));
  EXPECT_TRUE(adjRib_->getRibEntry(true /* ingress */, kV4Prefix2));

  auto& postPolicyAttrs1 =
      adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix1)->getPostAttr();
  auto& postPolicyAttrs2 =
      adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix2)->getPostAttr();

  // Both prefixes should have been rejected by policy.
  EXPECT_FALSE(postPolicyAttrs1);
  EXPECT_FALSE(postPolicyAttrs2);
}

/**
 * Because policy cache key contains shared_ptr, we verify
 * that methods that access the cache (processPeerUpdate)
 * do not modify the key, as map key should be immutable.
 *
 * This test should guard improper cloning on ingress side.
 */
TEST_F(AdjRibInPolicyCacheFixture, ImmutablePolicyCacheKeyTest) {
  // Set up ingress policy accept all on AdjRib
  // isRrClient = false AdjRib
  auto policy = setupMatchEgpOriginSetCommunityPolicy(kIngressPolicyName);
  // Set up adjRib with policy manager.
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  auto path = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 1, 1));
  auto expected = path->clone();

  EXPECT_EQ(0, adjRib_->policyCache_->size());

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);
  folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1, pfx2}, path));

  EXPECT_EQ(1, adjRib_->policyCache_->size());
  auto entry = adjRib_->policyCache_->policyLruCache_.rlock()->begin();
  auto keyBgpPath = std::get<2>(entry->first);
  EXPECT_TRUE(*keyBgpPath == *expected);
}

/**
 * Tests that two sequential calls to AdjRib::processPeerAnnounced
 * will result in a cache hit if the paths in the respective
 * updates are logically equivalent against the policy mask.
 */
TEST_F(AdjRibInPolicyCacheFixture, ExpectedCacheHitTest) {
  auto policy = setupMatchEgpOriginSetCommunityPolicy(kIngressPolicyName);
  // Set up adjRib with policy manager.
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 0 /* community_count */, 1, 1));
  auto path2 = std::make_shared<BgpPath>(
      *buildBgpPathFields(2, 0 /* community_count */, 2, 2));

  // Ensure both paths have the same origin.
  path1->setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP);
  path2->setOrigin(BgpAttrOrigin::BGP_ORIGIN_IGP);

  // match is only origin, action is only communities.
  const PolicyAttributesMask expectedMask{.origin = true, .communities = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kIngressPolicyName));

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);

  EXPECT_EQ(0, adjRib_->policyCache_->size());
  {
    // Announce pfx1 path1.
    folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1}, path1));

    // Verify cache should only have 1 entry and there is 1 cache miss.
    EXPECT_EQ(1, adjRib_->policyCache_->size());
    EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheMiss());
    EXPECT_EQ(0, adjRib_->policyCache_->getTotalCacheHit());
  }
  {
    // Announce pfx2 path2.
    folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx2}, path2));

    // Verify cache should have 1 entry, 1 cache miss, and 1 cache hit.
    EXPECT_EQ(1, adjRib_->policyCache_->size());
    EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheMiss());
    EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheHit());
  }
}

/**
 * Tests that two sequential calls to AdjRib::processPeerAnnounced
 * will result in a cache miss if the paths in the respective
 * updates are logically different against the policy mask.
 */
TEST_F(AdjRibInPolicyCacheFixture, ExpectedCacheMissTest) {
  auto policy = setupMatchEgpOriginSetCommunityPolicy(kIngressPolicyName);
  // Set up adjRib with policy manager.
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  // Ensure both paths have different communities.
  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 1 /* community_count */, 1, 1));
  auto path2 = std::make_shared<BgpPath>(
      *buildBgpPathFields(2, 2 /* community_count */, 2, 2));

  // match is only origin, action is only communities.
  const PolicyAttributesMask expectedMask{.origin = true, .communities = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kIngressPolicyName));

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);

  EXPECT_EQ(0, adjRib_->policyCache_->size());
  {
    // Announce pfx1 path1.
    folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1}, path1));

    // Verify cache should only have 1 entry and there is 1 cache miss.
    EXPECT_EQ(1, adjRib_->policyCache_->size());
    EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheMiss());
    EXPECT_EQ(0, adjRib_->policyCache_->getTotalCacheHit());
  }
  {
    // Announce pfx2 path2.
    folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx2}, path2));

    // Verify cache should have 2 entries, 2 cache misses, and 0 cache hits.
    EXPECT_EQ(2, adjRib_->policyCache_->size());
    EXPECT_EQ(2, adjRib_->policyCache_->getTotalCacheMiss());
    EXPECT_EQ(0, adjRib_->policyCache_->getTotalCacheHit());
  }
}

/**
 * Verify that TopologyInfo is overridden from policy result correctly
 * with a cache hit.
 *
 * See S554275.
 */
TEST_F(AdjRibInPolicyCacheFixture, BgpPathTopologyInfoCacheHitTest) {
  nsf_policy::NsfTeWeightEncoding encoding;
  encoding.l2_encoding() = nsf_policy::NsfL2TeWeightEncoding();
  encoding.l2_encoding()->rack_id() = 4;
  encoding.l2_encoding()->plane_id() = 4;
  encoding.l2_encoding()->remote_rack_capacity() = 8;
  encoding.l2_encoding()->spine_capacity() = 8;
  encoding.l2_encoding()->local_rack_capacity() = 8;

  int encodingId = 2;

  auto lbwExtDecodeAction = createBgpPolicyLbwExtCommunityAction(
      bgp_policy::LbwExtCommunityActionType::DECODE_ALL, encoding, encodingId);

  auto policy = std::make_shared<PolicyManager>(
      createBgpPolicies(
          kIngressPolicyName, {}, {std::move(lbwExtDecodeAction)}),
      createTestBgpGlobalConfig());

  // Set up adjRib with policy manager.
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 1, 0 /* ext_community_count */, 1));
  auto path2 = std::make_shared<BgpPath>(
      *buildBgpPathFields(2, 2, 0 /* ext_community_count */, 2));

  // Set the ext communities to be the same.
  BgpAttrExtCommunitiesC exts1;
  exts1.emplace_back(kExtCommLbwTypeFirstWord, kExtCommLbwTypeSecondWord10G);
  path1->setExtCommunities(exts1);
  path2->setExtCommunities(exts1);

  // action is extCommunities and by extension topologyInfo.
  const PolicyAttributesMask expectedMask{
      .extCommunities = true, .customizedLbwEnabled = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kIngressPolicyName));

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);

  EXPECT_EQ(0, adjRib_->policyCache_->size());
  {
    // Announce pfx1 path1.
    folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1}, path1));

    // Verify cache should only have 1 entry and there is 1 cache miss.
    EXPECT_EQ(1, adjRib_->policyCache_->size());
    EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheMiss());
    EXPECT_EQ(0, adjRib_->policyCache_->getTotalCacheHit());
  }
  {
    // Announce pfx2 path2.
    folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx2}, path2));

    // Verify cache should have 1 entry, 1 cache miss, and 1 cache hit.
    EXPECT_EQ(1, adjRib_->policyCache_->size());
    EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheMiss());
    EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheHit());
  }
  {
    // Verify the topology info on both adjRibEntries post policy are the same.
    auto entry1 = adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix1);
    auto entry2 = adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix2);

    auto expectedTopoInfo =
        decodeValues(*path1->getNonTransitiveRawLbwValue(), encoding);

    EXPECT_EQ(expectedTopoInfo, entry1->getPostAttr()->getTopologyInfo());
    EXPECT_EQ(expectedTopoInfo, entry2->getPostAttr()->getTopologyInfo());
  }
}

/**
 * Verify that TopologyInfo is overridden from policy result correctly
 * with a cache miss.
 *
 * See S554275.
 */
TEST_F(AdjRibInPolicyCacheFixture, BgpPathTopologyInfoCacheMissTest) {
  nsf_policy::NsfTeWeightEncoding encoding;
  encoding.l2_encoding() = nsf_policy::NsfL2TeWeightEncoding();
  encoding.l2_encoding()->rack_id() = 4;
  encoding.l2_encoding()->plane_id() = 4;
  encoding.l2_encoding()->remote_rack_capacity() = 8;
  encoding.l2_encoding()->spine_capacity() = 8;
  encoding.l2_encoding()->local_rack_capacity() = 8;

  int encodingId = 2;

  auto lbwExtDecodeAction = createBgpPolicyLbwExtCommunityAction(
      bgp_policy::LbwExtCommunityActionType::DECODE_ALL, encoding, encodingId);

  auto policy = std::make_shared<PolicyManager>(
      createBgpPolicies(
          kIngressPolicyName, {}, {std::move(lbwExtDecodeAction)}),
      createTestBgpGlobalConfig());

  // Set up adjRib with policy manager.
  setupAdjRib(
      kShortGrRestartTime,
      std::nullopt, // remoteGrRestartTime
      false, // callSessionEstablished
      kLocalAs1,
      kLocalAs1,
      kRemoteAs1,
      AfiIpv4Negotiated(true),
      AfiIpv6Negotiated(true),
      policy,
      kIngressPolicyName);

  auto path1 = std::make_shared<BgpPath>(
      *buildBgpPathFields(1, 1, 0 /* ext_community_count */, 1));
  auto path2 = std::make_shared<BgpPath>(
      *buildBgpPathFields(2, 2, 0 /* ext_community_count */, 2));

  // Set the ext communities to be different.
  BgpAttrExtCommunitiesC exts1;
  exts1.emplace_back(kExtCommLbwTypeFirstWord, kExtCommLbwTypeSecondWord10G);
  BgpAttrExtCommunitiesC exts2;
  exts2.emplace_back(kExtCommLbwTypeFirstWord, kExtCommLbwTypeSecondWord20G);
  path1->setExtCommunities(std::move(exts1));
  path2->setExtCommunities(std::move(exts2));

  // action is extCommunities and by extension topologyInfo.
  const PolicyAttributesMask expectedMask{
      .extCommunities = true, .customizedLbwEnabled = true};
  EXPECT_EQ(expectedMask, *policy->getPolicyAttributesMask(kIngressPolicyName));

  RiggedIPPrefix pfx1;
  pfx1.prefix() = toIPPrefix(kV4Prefix1);
  RiggedIPPrefix pfx2;
  pfx2.prefix() = toIPPrefix(kV4Prefix2);

  EXPECT_EQ(0, adjRib_->policyCache_->size());
  {
    // Announce pfx1 path1.
    folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx1}, path1));

    // Verify cache should only have 1 entry and there is 1 cache miss.
    EXPECT_EQ(1, adjRib_->policyCache_->size());
    EXPECT_EQ(1, adjRib_->policyCache_->getTotalCacheMiss());
    EXPECT_EQ(0, adjRib_->policyCache_->getTotalCacheHit());
  }
  {
    // Announce pfx2 path2.
    folly::coro::blockingWait(adjRib_->processPeerAnnounced({pfx2}, path2));

    // Verify cache should have 2 entries, 2 cache misses, and 0 cache hits.
    EXPECT_EQ(2, adjRib_->policyCache_->size());
    EXPECT_EQ(2, adjRib_->policyCache_->getTotalCacheMiss());
    EXPECT_EQ(0, adjRib_->policyCache_->getTotalCacheHit());
  }
  {
    // Verify the topology info on both adjRibEntries post policy are different.
    auto entry1 = adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix1);
    auto entry2 = adjRib_->getRibEntry(/*ingress=*/true, kV4Prefix2);

    auto expectedTopoInfo1 =
        decodeValues(*path1->getNonTransitiveRawLbwValue(), encoding);
    auto expectedTopoInfo2 =
        decodeValues(*path2->getNonTransitiveRawLbwValue(), encoding);

    EXPECT_NE(expectedTopoInfo1, expectedTopoInfo2);
    EXPECT_EQ(expectedTopoInfo1, entry1->getPostAttr()->getTopologyInfo());
    EXPECT_EQ(expectedTopoInfo2, entry2->getPostAttr()->getTopologyInfo());
  }
}

} // namespace facebook::bgp
