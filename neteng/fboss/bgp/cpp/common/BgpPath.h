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

#include "fboss/agent/state/NodeBase-defs.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/FeatureFlags.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"

namespace {
using facebook::bgp::kMedMax;
uint32_t getMedNotSetValue() {
  const facebook::bgp::FeatureFlags::BgpBestpathFeatures& bgpBestpathFeatures =
      facebook::bgp::FeatureFlags::getBgpBestpathFeatures();
  if (bgpBestpathFeatures.enableMedMissingAsWorst) {
    return kMedMax;
  }
  return 0;
}
} // anonymous namespace

namespace facebook::bgp {

struct BgpPathFields : nettools::bgplib::BgpPathC {
  BgpPathFields() = default;

  explicit BgpPathFields(const nettools::bgplib::BgpPathC& pathC)
      : nettools::bgplib::BgpPathC(pathC) {}

  template <typename Fn>
  void forEachChild(Fn /*fn*/) {}

  bool operator==(const BgpPathFields& other) const {
    return nettools::bgplib::BgpPathC::operator==(other) &&
        backupAddr == other.backupAddr;
  }

  std::optional<folly::IPAddress> backupAddr;

  folly::dynamic toFollyDynamic() const;
};

class BgpPath : public fboss::NodeBaseT<BgpPath, BgpPathFields> {
 public:
  // All fields are populated in host byte order

  // ORIGIN
  const nettools::bgplib::BgpAttrOrigin& getOrigin() const {
    return getFields()->attrs.get().origin;
  }

  void setOrigin(nettools::bgplib::BgpAttrOrigin origin) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.origin = std::move(origin);
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // AS_PATH
  const nettools::bgplib::DeDuplicatedAsPath& getAsPath() const {
    return getFields()->attrs.get().asPath;
  }

  int64_t getBgpAsPathLen() const;
  int64_t getBgpAsPathLenWithConfed() const;

  /*
   * Get the full AS_PATH string with full AS_SET and AS_CONFED_SET
   * embedded along with AS_SEQUENCE and AS_CONFED_SEQUENCE, in the
   * same one string
   */
  const std::vector<std::string> getFullBgpAsPathAsString() const;

  void setAsPath(nettools::bgplib::BgpAttrAsPathC asPath) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.asPath = std::move(asPath);
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // NEXT_HOP
  const folly::IPAddress& getNexthop() const {
    return getFields()->nexthop;
  }

  void setNexthop(folly::IPAddress nexthop) {
    writableFields()->nexthop = std::move(nexthop);
  }

  bool getIsMedSet() const {
    return getFields()->attrs.get().isMedSet;
  }

  // MULTI_EXIT_DESCRIMINATOR
  uint32_t getMed() const {
    if (!getIsMedSet()) {
      return getMedNotSetValue();
    }
    return getFields()->attrs.get().med;
  }

  void setMed(uint32_t med) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.med = med;
    mutableAttrs.isMedSet = true;
    writableFields()->attrs = std::move(mutableAttrs);
  }

  void unSetMed() {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.isMedSet = false;
    mutableAttrs.med = getMedNotSetValue();
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // LOCAL_PREF
  std::optional<uint32_t> getLocalPref() const {
    return getFields()->attrs.get().localPref;
  }

  void setLocalPref(std::optional<uint32_t> localPref) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.localPref = localPref;
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // ATOMIC_AGGREGATE
  bool getAtomicAggregate() const {
    return getFields()->attrs.get().atomicAggregate;
  }

  void setAtomicAggregate(bool atomicAggregate) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.atomicAggregate = atomicAggregate;
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // AGGREGATOR
  const nettools::bgplib::BgpAttrAggregatorC& getAggregator() const {
    return getFields()->attrs.get().aggregator;
  }

  void setAggregator(nettools::bgplib::BgpAttrAggregatorC aggregator) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.aggregator = std::move(aggregator);
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // COMMUNITIES
  const nettools::bgplib::DeDuplicatedCommunities& getCommunities() const {
    return getFields()->attrs.get().communities;
  }

  void setCommunities(nettools::bgplib::BgpAttrCommunitiesC communities) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.communities = std::move(communities);
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // ORIGINATOR_ID
  uint32_t getOriginatorId() const {
    return getFields()->attrs.get().originatorId;
  }

  void setOriginatorId(uint32_t originatorId) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.originatorId = originatorId;
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // CLUSTER_LIST
  const nettools::bgplib::DeDuplicatedClusterList& getClusterList() const {
    return getFields()->attrs.get().clusterList;
  }

  void setClusterList(nettools::bgplib::BgpAttrClusterListC clusterList) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.clusterList = std::move(clusterList);
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // EXTENDED_COMMUNITIES
  const nettools::bgplib::DeDuplicatedExtCommunities& getExtCommunities()
      const {
    return getFields()->attrs.get().extCommunities;
  }

  void setExtCommunities(
      nettools::bgplib::BgpAttrExtCommunitiesC extCommunities) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.extCommunities = std::move(extCommunities);
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // WEIGHT
  uint16_t getWeight() const {
    return getFields()->attrs.get().weight;
  }

  void setWeight(uint16_t weight) {
    auto mutableAttrs = writableFields()->attrs.get();
    mutableAttrs.weight = weight;
    writableFields()->attrs = std::move(mutableAttrs);
  }

  // TOPOLOGY_INFO
  const std::optional<std::unordered_map<std::string, int64_t>>&
  getTopologyInfo() const {
    return getFields()->topologyInfo;
  }
  void setTopologyInfo(
      std::optional<std::unordered_map<std::string, int64_t>> topologyInfo) {
    writableFields()->topologyInfo = std::move(topologyInfo);
  }

  const std::optional<folly::IPAddress>& getBackupAddr() const {
    return getFields()->backupAddr;
  }
  void setBackupAddr(std::optional<folly::IPAddress> backupAddr) {
    writableFields()->backupAddr = std::move(backupAddr);
  }

  bool hasNonTransitiveLbwExtCommunity() const;
  // Set LBW ext community to given asn/value.  Prune existing LBW if it
  // exists.
  void setNonTransitiveLbwExtCommunity(uint16_t asn, float lbwValue);
  // Prune LBW extended community if it exists
  void pruneNonTransitiveLbwExtCommunity();
  // Prune invalid extended community. current use case is "transitive lbw"
  void pruneTransitiveLbwExtCommunity();
  // Get value of link bandwidth in LBW ext community
  std::optional<float> getNonTransitiveLbwValue() const;
  // Get value of ASN in LBW ext community
  std::optional<uint16_t> getNonTransitiveLbwAsn() const;
  // get LBW as a pair of (asn + value), return nullopt if not found
  std::optional<std::pair<uint16_t, float>> getNonTransitiveLbw() const;

  void setNonTransitiveRawLbwExtCommunity(uint16_t asn, uint32_t rawLbw);
  std::optional<uint32_t> getNonTransitiveRawLbwValue() const;
  std::optional<std::pair<uint16_t, uint32_t>> getNonTransitiveRawLbw() const;

  const std::shared_ptr<nettools::bgplib::BgpExtCommunityLinkBandWidthTypeC>
  getNonTransitiveLbwExtCommunity() const;

  inline int getOnAdjPreoutCount() const {
    return onAdjPreoutCount_;
  }

  inline void incOnAdjPreoutCount() const {
    ++onAdjPreoutCount_;
  }

  inline void decOnAdjPreoutCount() const {
    --onAdjPreoutCount_;
  }

  // implement virtual function in NodeBaseT
  folly::dynamic toFollyDynamic() const override;

  // This method does not compare nodeId, generation and published fields
  // Used in RouteInfo to compare pure contents of the two attributes
  inline bool operator==(const BgpPath& other) const {
    // We need to compare Address families, to avoid matching
    // 10.0.0.1 == ::ffff:10.0.0.1, one is ipv4 address and another is ipv6
    // mapped ipv4 address. == operator in IPAddress.h treats them same.
    return (this->getNexthop().family() == other.getNexthop().family()) &&
        (*(this->getFields()) == *(other.getFields()));
  }

  inline bool operator!=(const BgpPath& other) const {
    return !(*this == other);
  }

  // convert to string
  std::string str() const {
    return getFields()->str();
  }

  // Hash function for contents - member method required by DeDuplicator<T>
  std::size_t hash() const;

  // Hash function for contents - functor for shared_ptr
  struct Hash {
    std::size_t operator()(std::shared_ptr<const BgpPath> const& attr) const;
  };

  // Compare function for contents
  struct Compare {
    size_t operator()(
        std::shared_ptr<const BgpPath> const& attr1,
        std::shared_ptr<const BgpPath> const& attr2) const {
      // We need to compare Address families, to avoid matching
      // 10.0.0.1 == ::ffff:10.0.0.1, one is ipv4 address and another is ipv6
      // mapped ipv4 address. == operator in IPAddress.h treats them same,
      // but in all the code related to hashing/grouping attributes we need
      // to treat these as different.
      return (attr1->getNexthop().family() == attr2->getNexthop().family()) &&
          (*(attr1->getFields()) == *(attr2->getFields()));
    }
  };

  std::shared_ptr<nettools::bgplib::BgpUpdate2> getBgpUpdate2() const;

 private:
  mutable std::atomic<int> onAdjPreoutCount_{0};
  // Inherit the constructors required for clone()
  using NodeBaseT::NodeBaseT;
  friend class CloneAllocator;
};

} // namespace facebook::bgp

namespace facebook::nettools::bgplib {
// BgpPath constructors are private (gated through CloneAllocator), so only
// the shared_ptr<BgpPath> constructors of DeDuplicatedAttribute are usable.
using DeDuplicatedBgpPath = DeDuplicatedAttribute<facebook::bgp::BgpPath>;
} // namespace facebook::nettools::bgplib
