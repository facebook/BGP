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

#include <neteng/fboss/bgp/cpp/lib/BgpStructs.h>

#include <folly/gen/Base.h>
#include <folly/hash/Hash.h>
#include <thrift/lib/cpp/util/EnumUtils.h>

namespace facebook {
namespace nettools {
namespace bgplib {

const uint8_t BgpExtCommunityBaseTypeC::kBgpExtCommASNonTransitiveType;
const uint8_t BgpExtCommunityBaseTypeC::kBgpExtCommASTransitiveType2;
const uint8_t BgpExtCommunityBaseTypeC::kBgpExtCommASTransitiveType;
const uint8_t BgpExtCommunityBaseTypeC::kBgpExtCommIPv4NonTransitiveType;
const uint8_t BgpExtCommunityBaseTypeC::kBgpExtCommIPv4TransitiveType;
const uint8_t BgpExtCommunityBaseTypeC::kBgpExtCommOpaqueNonTransitiveType;
const uint8_t BgpExtCommunityBaseTypeC::kBgpExtCommOpaqueTransitiveType;

std::optional<BgpAttrCommunityC> BgpAttrCommunityC::createBgpAttrCommunity(
    const std::string& comm) {
  static const std::map<std::string, BgpAttrCommunityC> kWellknownCommunities =
      {
          {"internet", BgpAttrCommunityC{0, 0}},
          {"no-export", BgpAttrCommunityC{0xFFFF, 0xFF01}},
          {"no-advertise", BgpAttrCommunityC{0xFFFF, 0xFF02}},
          {"no-export-subconfed", BgpAttrCommunityC{0xFFFF, 0xFF03}},
          // Local-AS is an alias of no-export-subconfed
          {"local-as", BgpAttrCommunityC{0xFFFF, 0xFF03}},
      };
  auto iter = kWellknownCommunities.find(comm);
  if (iter != kWellknownCommunities.end()) {
    return iter->second;
  }
  try {
    uint16_t asn, num;
    if (folly::split<true>(':', comm, asn, num)) {
      return BgpAttrCommunityC(asn, num);
    }
    uint32_t as_nn = folly::to<uint32_t>(comm);
    return BgpAttrCommunityC(as_nn >> 16, as_nn & 0xFFFF);
  } catch (...) {
  }
  return std::nullopt;
}

std::size_t BgpAttrCommunityC::hash() const {
  return folly::hash::hash_combine(asn, value);
}

std::size_t BgpAttrCommunitiesC::hash() const {
  std::size_t seed = 0;
  for (const auto& community : *this) {
    seed = folly::hash::hash_combine(seed, community.hash());
  }
  return seed;
}

std::size_t BgpAttrClusterListC::hash() const {
  return folly::hash::hash_range(begin(), end());
}

std::string BgpAttrAsPathSegmentC::str() const {
  std::string result;
  if (!asSet.empty()) {
    return "asSet: " + folly::join(" ", asSet);
  }
  if (!asSequence.empty()) {
    return "asSequence: " + folly::join(" ", asSequence);
  }
  if (!asConfedSequence.empty()) {
    return "asConfedSequence: " + folly::join(" ", asConfedSequence);
  }
  if (!asConfedSet.empty()) {
    return "asConfedSet: " + folly::join(" ", asConfedSet);
  }
  return result;
}

template <typename Func>
bool BgpAttrAsPathSegmentC::checkSegmentAnyAsn(Func func) const {
  auto isAny = [&](const auto& t) {
    if (std::any_of(t.begin(), t.end(), func)) {
      return true;
    }
    return false;
  };
  if (!asSequence.empty()) {
    return isAny(asSequence);
  }
  if (!asSet.empty()) {
    return isAny(asSet);
  }
  if (!asConfedSequence.empty()) {
    return isAny(asConfedSequence);
  }
  if (!asConfedSet.empty()) {
    return isAny(asConfedSet);
  }
  return false;
}

bool BgpAttrAsPathSegmentC::hasAsn(const uint32_t asn) const {
  auto isEqual = [=](const auto& elem) { return elem == asn; };
  return checkSegmentAnyAsn(isEqual);
}

bool BgpAttrAsPathSegmentC::hasPublicAsn() const {
  auto isPublicAsn = [](const auto& elem) {
    return elem < BGP_PRIVATE_AS_START;
  };
  return checkSegmentAnyAsn(isPublicAsn);
}

bool BgpAttrAsPathSegmentC::isConfedSegment() const {
  if (asConfedSequence.size() || asConfedSet.size()) {
    return true;
  }
  return false;
}

std::size_t BgpAttrAsPathSegmentC::hash() const {
  std::size_t seed = 0;
  seed = folly::hash::hash_range(asSet.begin(), asSet.end(), seed);

  // shift hash to differentiate
  seed = folly::hash::hash_combine(seed, 0);
  seed = folly::hash::hash_range(asSequence.begin(), asSequence.end(), seed);

  // shift hash to differentiate
  seed = folly::hash::hash_combine(seed, 1);
  seed = folly::hash::hash_range(
      asConfedSequence.begin(), asConfedSequence.end(), seed);

  // shift hash to differentiate
  seed = folly::hash::hash_combine(seed, 2);
  seed = folly::hash::hash_range(asConfedSet.begin(), asConfedSet.end(), seed);

  return seed;
}

std::size_t BgpAttrAsPathC::hash() const {
  std::size_t seed = 0;
  for (const auto& seg : *this) {
    seed = folly::hash::hash_combine(seed, seg.hash());
  }
  return seed;
}

std::size_t BgpAttrAggregatorC::hash() const {
  return folly::hash::hash_combine(asn, ip);
}

std::string BgpAttributesC::str() const {
  std::string result;
  result +=
      fmt::format("origin: {} ", apache::thrift::util::enumNameOrThrow(origin));
  if (!asPath.nullOrEmpty()) {
    result += std::string("asPath: ");
    for (const auto& elem : *asPath) {
      result += fmt::format("asPathSeg {} ", elem.str());
    }
  }
  if (isMedSet) {
    result += fmt::format("med: {} ", med);
  }
  result += fmt::format(
      "localPref: {} ",
      localPref == std::nullopt ? "none" : std::to_string(*localPref));
  result +=
      fmt::format("atomicAggregate: {} ", atomicAggregate ? "true" : "false");
  result += fmt::format(
      "aggregator: asn {} ip {} ",
      aggregator.asn,
      (aggregator.ip.empty() ? "" : aggregator.ip.str()));
  if (!communities.nullOrEmpty()) {
    result += std::string("communities: ");
    for (const auto& elem : communities.get()) {
      result += fmt::format("{}:{} ", elem.asn, elem.value);
    }
  }
  result += fmt::format("originatorId: {} ", originatorId);
  if (!clusterList.nullOrEmpty()) {
    result +=
        fmt::format("clusterList: {}", folly::join(" ", clusterList.get()));
  }
  if (!extCommunities.nullOrEmpty()) {
    result += std::string("extCommunities: ");
    for (const auto& elem : extCommunities.get()) {
      result += fmt::format("{} ", elem.str());
    }
  }
  result += fmt::format("weight: {} ", weight);
  return result;
}

std::size_t BgpAttributesC::hash() const {
  std::size_t seed = 0;
  seed = folly::hash::hash_combine(seed, origin);
  if (!asPath.nullOrEmpty()) {
    seed = folly::hash::hash_combine(seed, asPath->hash());
  }
  seed = folly::hash::hash_combine(seed, med);
  seed = folly::hash::hash_combine(seed, localPref);
  seed = folly::hash::hash_combine(seed, atomicAggregate);
  seed = folly::hash::hash_combine(seed, aggregator.hash());
  if (!communities.nullOrEmpty()) {
    seed = folly::hash::hash_combine(seed, communities->hash());
  }
  seed = folly::hash::hash_combine(seed, originatorId);
  if (!clusterList.nullOrEmpty()) {
    seed = folly::hash::hash_combine(seed, clusterList->hash());
  }
  if (!extCommunities.nullOrEmpty()) {
    seed = folly::hash::hash_combine(seed, extCommunities->hash());
  }
  seed = folly::hash::hash_combine(seed, weight);
  return seed;
}

BgpAttrExtCommunityC::BgpAttrExtCommunityC(
    uint32_t rawValHigh,
    uint32_t rawValLow) {
  const uint8_t type = (rawValHigh >> 24);
  const uint8_t subType = (rawValHigh >> 16);

  if ((type == BgpExtCommunityBaseTypeC::kBgpExtCommASNonTransitiveType) &&
      (subType ==
       static_cast<int>(
           BGP_EXT_COMMUNITY_SUBTYPES::LINK_BW_COMMUNITY_SUBTYPE))) {
    attr = std::make_shared<BgpExtCommunityLinkBandWidthTypeC>(
        rawValHigh, rawValLow);
  } else if (
      (type == BgpExtCommunityBaseTypeC::kBgpExtCommASNonTransitiveType) ||
      (type == BgpExtCommunityBaseTypeC::kBgpExtCommASTransitiveType) ||
      (type == BgpExtCommunityBaseTypeC::kBgpExtCommASTransitiveType2)) {
    attr = std::make_shared<BgpExtCommunityAsSpecificExtTypeC>(
        rawValHigh, rawValLow);
  } else if (
      (type == BgpExtCommunityBaseTypeC::kBgpExtCommIPv4NonTransitiveType) ||
      (type == BgpExtCommunityBaseTypeC::kBgpExtCommIPv4TransitiveType)) {
    attr = std::make_shared<BgpExtCommunityIPv4SpecificExtTypeC>(
        rawValHigh, rawValLow);
  } else if (
      (type == BgpExtCommunityBaseTypeC::kBgpExtCommOpaqueNonTransitiveType) ||
      (type == BgpExtCommunityBaseTypeC::kBgpExtCommOpaqueTransitiveType)) {
    attr =
        std::make_shared<BgpExtCommunityOpaqueExtTypeC>(rawValHigh, rawValLow);
  } else {
    attr = std::make_shared<BgpAttrExtCommunityRegularTypeC>(
        rawValHigh, rawValLow);
  }
}

// A transitive extended community might be passed on to the peers.
bool BgpAttrExtCommunityC::isTransitive() const {
  return !(
      attr->getType() & static_cast<int>(BGP_EXT_COMMUNITY_FLAGS::TRANSITIVE));
}

bool BgpAttrExtCommunityC::isRouteTarget() const {
  auto subType = attr->getSubType();
  if ((attr->getType() <=
       BgpExtCommunityBaseTypeC::kBgpExtCommASTransitiveType2) &&
      subType.has_value() &&
      (*subType ==
       static_cast<int>(
           BGP_EXT_COMMUNITY_SUBTYPES::ROUTE_TARGET_COMMUNITY_SUBTYPE))) {
    return true;
  }
  return false;
}

bool BgpAttrExtCommunityC::isRouteOrigin() const {
  auto subType = attr->getSubType();
  if ((attr->getType() <=
       BgpExtCommunityBaseTypeC::kBgpExtCommASTransitiveType2) &&
      subType.has_value() &&
      (*subType ==
       static_cast<int>(
           BGP_EXT_COMMUNITY_SUBTYPES::ROUTE_ORIGIN_COMMUNITY_SUBTYPE))) {
    return true;
  }
  return false;
}

bool BgpAttrExtCommunityC::isNonTransitiveLinkBandwidthCommunity() const {
  auto subType = attr->getSubType();
  return (attr->getType() ==
          BgpExtCommunityBaseTypeC::kBgpExtCommASNonTransitiveType) &&
      subType.has_value() &&
      (*subType ==
       static_cast<int>(BGP_EXT_COMMUNITY_SUBTYPES::LINK_BW_COMMUNITY_SUBTYPE));
}

bool BgpAttrExtCommunityC::isLinkBandwidthCommunity() const {
  auto subType = attr->getSubType();
  return subType.has_value() &&
      (*subType ==
       static_cast<int>(BGP_EXT_COMMUNITY_SUBTYPES::LINK_BW_COMMUNITY_SUBTYPE));
}

std::string BgpAttrExtCommunityC::str() const {
  return attr->str();
}

std::pair<uint32_t, uint32_t> BgpAttrExtCommunityC::getRawValueInWords() const {
  return attr->getRawValueInWords();
}

bool BgpAttrExtCommunityC::operator==(const BgpAttrExtCommunityC& other) const {
  return attr->getRawValueInWords() == other.attr->getRawValueInWords();
}

std::size_t BgpAttrExtCommunityC::hash() const {
  return folly::hash::hash_combine(getRawValueInWords());
}

std::size_t BgpAttrExtCommunitiesC::hash() const {
  std::size_t seed = 0;
  for (const auto& extCommunity : *this) {
    seed = folly::hash::hash_combine(seed, extCommunity.hash());
  }
  return seed;
}

BgpExtCommunityAsSpecificExtTypeC::BgpExtCommunityAsSpecificExtTypeC(
    uint32_t rawValHigh,
    uint32_t rawValLow)
    : BgpExtCommunityBaseTypeC(rawValHigh, rawValLow) {
  auto type = getType();
  CHECK(
      (type == kBgpExtCommASNonTransitiveType) ||
      (type == kBgpExtCommASTransitiveType) ||
      (type == kBgpExtCommASTransitiveType2));
}

BgpExtCommunityLinkBandWidthTypeC::BgpExtCommunityLinkBandWidthTypeC(
    uint32_t rawValHigh,
    uint32_t rawValLow)
    : BgpExtCommunityAsSpecificExtTypeC(rawValHigh, rawValLow) {}

uint32_t BgpExtCommunityLinkBandWidthTypeC::rawValueHigh(uint16_t asn) {
  /*
   * As per draft-ietf-idr-link-bandwidth:
   *  - the value of the high-order octet of the extended Type Field is 0x40
   *    for non-transitive extended communities.
   *  - the value of the low-order octet of the extended Type Field is 0x04
   *  - the value of the Global Administrator subfield SHOULD represent the
   *    Autonomous System of the router
   */
  uint8_t lbwType = BgpExtCommunityBaseTypeC::kBgpExtCommASNonTransitiveType;
  uint8_t lbwSubType =
      static_cast<int>(BgpAttrExtCommunityC::BGP_EXT_COMMUNITY_SUBTYPES::
                           LINK_BW_COMMUNITY_SUBTYPE);
  uint32_t rawValHigh = lbwType << 24 | lbwSubType << 16 | asn;
  return rawValHigh;
}

BgpExtCommunityLinkBandWidthTypeC::BgpExtCommunityLinkBandWidthTypeC(
    uint16_t asn,
    float val)
    : BgpExtCommunityAsSpecificExtTypeC(rawValueHigh(asn), 0) {
  union {
    uint32_t intVal;
    float floatVal;
  } tmp;
  tmp.intVal = 0; // To avoid lint error
  tmp.floatVal = val;
  rawValLow = tmp.intVal;
}

float BgpExtCommunityLinkBandWidthTypeC::getLBW() const {
  /*
   * Assuming machine has single precision binary32 float architecture.
   * Ref: IEEE-754-1985 specification
   */
  union {
    uint32_t intVal;
    float floatVal;
  } tmp;
  tmp.floatVal = 0.0f; // To avoid lint error
  tmp.intVal = BgpExtCommunityAsSpecificExtTypeC::getValue();
  return tmp.floatVal;
}

void BgpExtCommunityLinkBandWidthTypeC::setLBW(float lbwInFloat) {
  union {
    uint32_t intVal;
    float floatVal;
  } tmp;
  tmp.intVal = 0;
  tmp.floatVal = lbwInFloat;
  BgpExtCommunityAsSpecificExtTypeC::setValue(tmp.intVal);
}

BgpExtCommunityIPv4SpecificExtTypeC::BgpExtCommunityIPv4SpecificExtTypeC(
    uint32_t rawValHigh,
    uint32_t rawValLow)
    : BgpExtCommunityBaseTypeC(rawValHigh, rawValLow) {
  auto type = getType();
  CHECK(
      (type == kBgpExtCommIPv4NonTransitiveType) ||
      (type == kBgpExtCommIPv4TransitiveType));
}
BgpExtCommunityOpaqueExtTypeC::BgpExtCommunityOpaqueExtTypeC(
    uint32_t rawValHigh,
    uint32_t rawValLow)
    : BgpExtCommunityBaseTypeC(rawValHigh, rawValLow) {
  auto type = getType();
  CHECK(
      (type == kBgpExtCommOpaqueNonTransitiveType) ||
      (type == kBgpExtCommOpaqueTransitiveType));
}

std::array<uint8_t, 6> BgpExtCommunityOpaqueExtTypeC::getValue() const {
  std::array<uint8_t, 6> value;
  value[0] = rawValHigh >> 8;
  value[1] = rawValHigh;
  value[2] = rawValLow >> 24;
  value[3] = rawValLow >> 16;
  value[4] = rawValLow >> 8;
  value[5] = rawValLow;
  return value;
}

std::array<uint8_t, 7> BgpAttrExtCommunityRegularTypeC::getValue() const {
  std::array<uint8_t, 7> value;
  value[0] = rawValHigh >> 16;
  value[1] = rawValHigh >> 8;
  value[2] = rawValHigh;
  value[3] = rawValLow >> 24;
  value[4] = rawValLow >> 16;
  value[5] = rawValLow >> 8;
  value[6] = rawValLow;
  return value;
}

std::string BgpPathC::str() const {
  std::string result;

  result += attrs.get().str();

  if (!nexthop.empty()) {
    result += fmt::format("nexthop: {} ", nexthop.str());
  }

  if (topologyInfo) {
    result += topoInfoToStr(*topologyInfo);
  }

  return result;
}

std::string BgpPathC::topoInfoToStr(
    const std::unordered_map<std::string, int64_t>& topoInfo) {
  auto pairFormatter = [](const auto& entry) {
    return fmt::format("{}:{}", entry.first, entry.second);
  };
  auto stringVec = folly::gen::from(topoInfo) | folly::gen::map(pairFormatter) |
      folly::gen::as<std::vector<std::string>>();
  return fmt::format("topologyInfo: [{}] ", folly::join(",", stringVec));
}

} // namespace bgplib
} // namespace nettools
} // namespace facebook
