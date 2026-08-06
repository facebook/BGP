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

#include <cfloat>

#include <folly/gen/Base.h>
#include <folly/hash/Hash.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Utils.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"

using namespace facebook::nettools::bgplib;
using namespace facebook::bgp::BgpStats;

namespace {
class NotImplementedException : public std::logic_error {
 public:
  NotImplementedException()
      : std::logic_error{"Function not yet implemented."} {}
};
} // namespace

namespace facebook::bgp {

folly::dynamic BgpPath::toFollyDynamic() const {
  throw NotImplementedException();
}

// Hash function for contents - member method for DeDuplicator compatibility
std::size_t BgpPath::hash() const {
  size_t seed = 0;
  seed = folly::hash::hash_combine(seed, getOrigin());

  size_t asPathHash = getAsPath().nullOrEmpty() ? 0 : getAsPath()->hash();

  seed = folly::hash::hash_combine(seed, asPathHash);
  seed = folly::hash::hash_combine(seed, getNexthop());
  seed = folly::hash::hash_combine(seed, getMed());
  if (getLocalPref()) {
    seed = folly::hash::hash_combine(seed, *getLocalPref());
  }
  seed = folly::hash::hash_combine(seed, getAtomicAggregate());
  auto aggregator = getAggregator();
  if (aggregator.asn && !aggregator.ip.empty()) {
    seed = folly::hash::hash_combine(seed, aggregator.asn);
    seed = folly::hash::hash_combine(seed, aggregator.ip);
  }

  size_t commHash =
      getCommunities().nullOrEmpty() ? 0 : getCommunities()->hash();

  seed = folly::hash::hash_combine(seed, commHash);
  seed = folly::hash::hash_combine(seed, getOriginatorId());

  size_t clusterHash =
      getClusterList().nullOrEmpty() ? 0 : getClusterList()->hash();
  seed = folly::hash::hash_combine(seed, clusterHash);

  if (!getExtCommunities().nullOrEmpty()) {
    size_t extCommHash = getExtCommunities()->hash();
    seed = folly::hash::hash_combine(seed, extCommHash);
  }
  seed = folly::hash::hash_combine(seed, getWeight());

  return seed;
}

// Hash function for contents - functor for shared_ptr
std::size_t BgpPath::Hash::operator()(
    std::shared_ptr<const BgpPath> const& attr) const {
  return attr->hash();
}

bool BgpPath::hasNonTransitiveLbwExtCommunity() const {
  for (const auto& extComm : getExtCommunities().get()) {
    if (extComm.isNonTransitiveLinkBandwidthCommunity()) {
      return true;
    }
  }
  return false;
}

// Set LBW ext community to given asn/value.  Prune existing LBW if it exists.
void BgpPath::setNonTransitiveLbwExtCommunity(uint16_t asn, float lbwValue) {
  pruneNonTransitiveLbwExtCommunity();
  auto newCommunities = writableFields()->attrs->extCommunities.get();
  newCommunities.emplace_back(BgpExtCommunityLinkBandWidthTypeC(asn, lbwValue));
  setExtCommunities(std::move(newCommunities));
}

// Set ext community to given asn/value.  Prune existing LBW if it exists.
void BgpPath::setNonTransitiveRawLbwExtCommunity(
    uint16_t asn,
    uint32_t rawLbw) {
  pruneNonTransitiveLbwExtCommunity();
  auto newCommunities = writableFields()->attrs->extCommunities.get();
  newCommunities.emplace_back(BgpExtCommunityLinkBandWidthTypeC(
      BgpExtCommunityLinkBandWidthTypeC::rawValueHigh(asn), rawLbw));
  setExtCommunities(std::move(newCommunities));
}

// Get LBW ext community attribute with the lowest bandwidth value
const std::shared_ptr<BgpExtCommunityLinkBandWidthTypeC>
BgpPath::getNonTransitiveLbwExtCommunity() const {
  float lowestLbwValue = FLT_MAX;
  union {
    uint32_t intVal;
    float floatVal;
  } tmp{};

  std::shared_ptr<BgpExtCommunityBaseTypeC> lowestLbwAttr = nullptr;

  for (const auto& extComm : getExtCommunities().get()) {
    if (extComm.isNonTransitiveLinkBandwidthCommunity()) {
      // Convert rawValLow to float using IEEE-754 binary32 representation
      tmp.intVal = extComm.getRawValueInWords().second;
      auto lbwValue = tmp.floatVal;
      if (lbwValue < 0 || (lbwValue > lowestLbwValue)) {
        continue;
      }
      lowestLbwAttr = extComm.attr;
      lowestLbwValue = lbwValue;
    }
  }
  return std::dynamic_pointer_cast<BgpExtCommunityLinkBandWidthTypeC>(
      lowestLbwAttr);
}

// Prune LBW extended community if it exists
void BgpPath::pruneNonTransitiveLbwExtCommunity() {
  auto extCommunities = writableFields()->attrs->extCommunities.get();
  auto it = extCommunities.begin();
  bool pruned = false;
  while (it != extCommunities.end()) {
    if (it->isNonTransitiveLinkBandwidthCommunity()) {
      it = extCommunities.erase(it);
      pruned = true;
    } else {
      ++it;
    }
  }
  if (pruned) {
    setExtCommunities(std::move(extCommunities));
  }
}

// Prune invalid extended communities if they exist
void BgpPath::pruneTransitiveLbwExtCommunity() {
  auto extCommunities = writableFields()->attrs->extCommunities.get();
  auto it = extCommunities.begin();
  bool pruned = false;
  while (it != extCommunities.end()) {
    // remove "transitive" lbw community as follow up for s242365
    const auto& subType = it->attr->getSubType();
    if (it->attr->getType() ==
            BgpExtCommunityAsSpecificExtTypeC::kBgpExtCommASTransitiveType &&
        subType.has_value() &&
        (*subType ==
         static_cast<uint8_t>(BgpAttrExtCommunityC::BGP_EXT_COMMUNITY_SUBTYPES::
                                  LINK_BW_COMMUNITY_SUBTYPE))) {
      it = extCommunities.erase(it);
      pruned = true;
    } else {
      ++it;
    }
  }
  if (pruned) {
    setExtCommunities(std::move(extCommunities));
  }
}

// Get value of link bandwidth in LBW ext community
std::optional<float> BgpPath::getNonTransitiveLbwValue() const {
  auto lbwCommunity = getNonTransitiveLbwExtCommunity();
  if (lbwCommunity) {
    return lbwCommunity->getLBW();
  }
  return std::nullopt;
}

// Get value of link bandwidth in encoded LBW ext community
std::optional<uint32_t> BgpPath::getNonTransitiveRawLbwValue() const {
  auto lbwCommunity = getNonTransitiveLbwExtCommunity();
  if (lbwCommunity) {
    return lbwCommunity->getValue();
  }
  return std::nullopt;
}

// Get value of ASN in LBW ext community
std::optional<uint16_t> BgpPath::getNonTransitiveLbwAsn() const {
  auto lbwCommunity = getNonTransitiveLbwExtCommunity();
  if (lbwCommunity) {
    return lbwCommunity->getAsn();
  }
  return std::nullopt;
}

std::optional<std::pair<uint16_t, float>> BgpPath::getNonTransitiveLbw() const {
  auto lbw = getNonTransitiveLbwExtCommunity();
  if (!lbw) {
    return std::nullopt;
  }
  return std::make_pair<uint16_t, float>(lbw->getAsn(), lbw->getLBW());
}

std::optional<std::pair<uint16_t, uint32_t>> BgpPath::getNonTransitiveRawLbw()
    const {
  auto lbw = getNonTransitiveLbwExtCommunity();
  if (!lbw) {
    return std::nullopt;
  }
  return std::make_pair<uint16_t, uint32_t>(lbw->getAsn(), lbw->getValue());
}

// Populate BgpUpdate2.attrs from BgpPath
// Note: User has to fill all other fields in BgpUpdate2
std::shared_ptr<BgpUpdate2> BgpPath::getBgpUpdate2() const {
  auto update = std::make_shared<BgpUpdate2>();

  update->attrs()->origin() = getOrigin();
  for (const auto& seg : getAsPath().get()) {
    BgpAttrAsPathSegment segT;

    std::set<int64_t> asSet(seg.asSet.begin(), seg.asSet.end());
    std::vector<int64_t> asSequence(
        seg.asSequence.begin(), seg.asSequence.end());
    std::vector<int64_t> asConfedSequence(
        seg.asConfedSequence.begin(), seg.asConfedSequence.end());
    std::set<int64_t> asConfedSet(
        seg.asConfedSet.begin(), seg.asConfedSet.end());

    segT.asSet() = asSet;
    segT.asSequence() = asSequence;
    segT.asConfedSequence() = asConfedSequence;
    segT.asConfedSet() = asConfedSet;

    update->attrs()->asPath()->emplace_back(segT);
  }

  if (!getNexthop().empty()) {
    update->attrs()->nexthop() = getNexthop().str();
  } else {
    update->attrs()->nexthop() = "";
  }
  update->attrs()->med() = getMed();
  update->attrs()->isMedSet() = getIsMedSet();
  if (getLocalPref()) {
    update->attrs()->localPref() = *getLocalPref();
  }
  update->attrs()->atomicAggregate() = getAtomicAggregate();
  update->attrs()->aggregator()->asn() = getAggregator().asn;
  if (!getAggregator().ip.empty()) {
    update->attrs()->aggregator()->ip() = getAggregator().ip.str();
  } else {
    update->attrs()->aggregator()->ip() = "";
  }

  for (const auto& comm : getCommunities().get()) {
    BgpAttrCommunity commT;
    commT.asn() = comm.asn;
    commT.value() = comm.value;
    update->attrs()->communities()->emplace_back(commT);
  }

  update->attrs()->originatorId() = htonl(getOriginatorId());
  for (const auto& cluster : getClusterList().get()) {
    update->attrs()->clusterList()->emplace_back(htonl(cluster));
  }

  for (const auto& extComm : getExtCommunities().get()) {
    auto ret = extComm.getRawValueInWords();
    BgpAttrExtCommunity extCommT;
    extCommT.firstWord() = ret.first;
    extCommT.secondWord() = ret.second;
    update->attrs()->extCommunities()->emplace_back(extCommT);
  }

  return update;
}

// AS PATH length used in policy. AS_SET is counted as 1 irrespective
// of number of asn in the set, for AS_SEQUENCE number of asn is counted.
int64_t BgpPath::getBgpAsPathLen() const {
  int pathLen = 0;

  for (const auto& segment : getFields()->attrs->asPath.get()) {
    if (segment.asSet.size()) {
      ++pathLen;
    } else if (segment.asSequence.size()) {
      pathLen += segment.asSequence.size();
    }
  }
  return pathLen;
}

// AS Confed PATH length used in policy. AS_SET, AS_CONFED_SET are
// counted as 1 irrespective of number of asn in the set, for AS_SEQUENCE,
// AS_CONFED_SEQUENCE, it counts number of asn.
int64_t BgpPath::getBgpAsPathLenWithConfed() const {
  int pathLen = 0;

  for (const auto& segment : getFields()->attrs->asPath.get()) {
    if (segment.asSet.size() || segment.asConfedSet.size()) {
      ++pathLen;
    } else if (segment.asSequence.size()) {
      pathLen += segment.asSequence.size();
    } else {
      pathLen += segment.asConfedSequence.size();
    }
  }

  return pathLen;
}

// add members in asSet to existing AS Paths vector
void addAsInSet(
    std::string& asPathsStr,
    const std::set<uint32_t>& asSet,
    bool isConfed) {
  std::vector<std::string> asSetStr;
  for (const auto& as : asSet) {
    asSetStr.emplace_back(std::to_string(as));
  }

  auto deltaAsSetStr = folly::join("_", asSetStr);
  if (isConfed) {
    deltaAsSetStr = "(" + deltaAsSetStr + ")";
  }
  deltaAsSetStr = "{" + deltaAsSetStr + "}";

  if (!asPathsStr.empty()) {
    asPathsStr += "_" + deltaAsSetStr;
  } else {
    asPathsStr = deltaAsSetStr;
  }

  return;
}

// add members in asSequence to existing AS Paths vector
void addAsInSequence(
    std::string& asPathsStr,
    const std::vector<uint32_t>& asSequence,
    bool isConfed) {
  std::vector<std::string> asSequenceStrs;
  for (const auto& as : asSequence) {
    asSequenceStrs.emplace_back(std::to_string(as));
  }
  auto deltaAsSeqStr = folly::join("_", asSequenceStrs);
  if (isConfed) {
    deltaAsSeqStr = "(" + deltaAsSeqStr + ")";
  }

  if (!asPathsStr.empty()) {
    asPathsStr += "_" + deltaAsSeqStr;
  } else {
    asPathsStr = deltaAsSeqStr;
  }

  return;
}

/*
 * @brief  Return full AS_PATH string as it would be seen in the
 *         output of show commands
 *
 *         Example - If a route with bgp AS_PATH attribute look like
 *                   1_{3_2}_22_{6_4}
 *
 *                   then full AS_PATH string to match against would
 *                   look like
 *                   1_{3_2}_22_{6_4}
 *
 *         Note - ASN(S) in the AS_SET here is an unordered set
 *
 *
 * @param  void
 *
 * @return std::vector<std::string>
 *         vector here though would have only one string which is
 *         full AS_PATH string. The reason to keep this as vector
 *         is to allow same common code to work either with this
 *         function or Flatten function implemented as below
 */
const std::vector<std::string> BgpPath::getFullBgpAsPathAsString() const {
  using folly::gen::as;
  using folly::gen::eachTo;
  using folly::gen::from;
  std::vector<std::string> asPathAll;
  std::string asPathsStr;

  for (const auto& asSeg : getFields()->attrs->asPath.get()) {
    if (asSeg.asSet.size()) {
      addAsInSet(asPathsStr, asSeg.asSet, false);
      continue;
    }
    if (asSeg.asSequence.size()) {
      addAsInSequence(asPathsStr, asSeg.asSequence, false);
      continue;
    }
    if (asSeg.asConfedSet.size()) {
      addAsInSet(asPathsStr, asSeg.asConfedSet, true);
      continue;
    }
    if (asSeg.asConfedSequence.size()) {
      addAsInSequence(asPathsStr, asSeg.asConfedSequence, true);
      continue;
    }
  }

  asPathAll.emplace_back(asPathsStr);
  return asPathAll;
}
} // namespace facebook::bgp
