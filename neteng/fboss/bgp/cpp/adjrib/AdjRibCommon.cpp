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

#include <folly/logging/xlog.h>
#include <algorithm>
#include <cfloat>

#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRibCommon.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStats.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/FeatureFlags.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/lib/BgpMessageSerializer.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"

using namespace facebook::nettools::bgplib;

namespace facebook::bgp {

// Utility functions for PeerConfig
bool AdjRibCommonUtils::isEBgpPeer(const PeeringParams& peeringParams) {
  return peeringParams.localAs != peeringParams.remoteAs &&
      !peeringParams.isConfedPeer;
}

bool AdjRibCommonUtils::isConfedEBgpPeer(const PeeringParams& peeringParams) {
  return peeringParams.isConfedPeer &&
      peeringParams.localAs != peeringParams.remoteAs;
}

BgpSessionType AdjRibCommonUtils::getBgpSessionType(
    const PeeringParams& peeringParams) {
  if (peeringParams.isConfedPeer &&
      peeringParams.localAs != peeringParams.remoteAs) {
    return BgpSessionType::ConfedEBGP;
  }
  if (peeringParams.localAs != peeringParams.remoteAs) {
    return BgpSessionType::EBGP;
  }
  return BgpSessionType::IBGP;
}

bool AdjRibCommonUtils::isAfiNegotiated(
    const folly::CIDRNetwork& prefix,
    bool isAfiIpv4Negotiated,
    bool isAfiIpv6Negotiated) {
  return (prefix.first.isV4() && isAfiIpv4Negotiated) ||
      (prefix.first.isV6() && isAfiIpv6Negotiated);
}

void updateAdvertiseLbwExtCommunityCommon(
    const PeeringParams& peeringParams,
    const RibOutAnnouncementEntry& update,
    const std::shared_ptr<BgpPath>& attrs) noexcept {
  if (!attrs || !peeringParams.advertiseLinkBandwidth.has_value()) {
    return;
  }

  attrs->pruneTransitiveLbwExtCommunity();

  switch (*peeringParams.advertiseLinkBandwidth) {
    case AdvertiseLinkBandwidth::DISABLE:
      attrs->pruneNonTransitiveLbwExtCommunity();
      break;
    case AdvertiseLinkBandwidth::SET_LINK_BPS:
      CHECK(peeringParams.linkBandwidthBps.has_value())
          << "linkBandwidthBps not set for UCMP SET_LINK_BPS";
      attrs->setNonTransitiveLbwExtCommunity(
          peeringParams.localAs, *peeringParams.linkBandwidthBps);
      break;
    case AdvertiseLinkBandwidth::BEST_PATH:
      if (!update.aggregateReceivedUcmpWeight.has_value()) {
        attrs->pruneNonTransitiveLbwExtCommunity();
      }
      break;
    case AdvertiseLinkBandwidth::AGGREGATE_RECEIVED:
      if (!update.aggregateReceivedUcmpWeight.has_value()) {
        attrs->pruneNonTransitiveLbwExtCommunity();
      } else {
        attrs->setNonTransitiveLbwExtCommunity(
            peeringParams.localAs, *update.aggregateReceivedUcmpWeight);
      }
      break;
    case AdvertiseLinkBandwidth::AGGREGATE_LOCAL:
      if (!update.aggregateLocalUcmpWeight.has_value()) {
        attrs->pruneNonTransitiveLbwExtCommunity();
      } else {
        attrs->setNonTransitiveLbwExtCommunity(
            peeringParams.localAs, *update.aggregateLocalUcmpWeight);
      }
      break;
    case AdvertiseLinkBandwidth::RIB_POLICY_LBW:
      if (!update.ribPolicyUcmpWeight.has_value()) {
        attrs->pruneNonTransitiveLbwExtCommunity();
      } else {
        attrs->setNonTransitiveLbwExtCommunity(
            peeringParams.localAs, *update.ribPolicyUcmpWeight);
      }
      break;
    default:
      CHECK(false);
  }
}

std::shared_ptr<BgpPolicyActionData> createPolicyActionDataCommon(
    const PeeringParams& peeringParams,
    const std::shared_ptr<const BgpPath>& attrs,
    const std::optional<size_t>& switchId,
    const std::optional<size_t>& multiPathSize,
    const std::optional<float>& aggregateReceivedUcmpWeight,
    const std::optional<float>& aggregateLocalUcmpWeight,
    const std::optional<float>& ribPolicyUcmpWeight) {
  if (attrs == nullptr) {
    return nullptr;
  }

  /*
   * Here we intend to capture the ORIGINAL LBW state prior to per-peer config.
   * e.g. per-peer: DISABLE, per-route: ACCEPT. We expect the output route to
   * contain the original LBW even if it gets pruned by per-peer config.
   * Details: https://fb.quip.com/xqf4Ai6ySsDm
   */
  LbwActionData lbwActionData{
      attrs->getNonTransitiveLbw(),
      peeringParams.localAs,
      peeringParams.linkBandwidthBps,
      aggregateReceivedUcmpWeight,
      aggregateLocalUcmpWeight,
      ribPolicyUcmpWeight};

  return std::make_shared<BgpPolicyActionData>(
      switchId, multiPathSize, std::move(lbwActionData));
}

// Global cache for policy result strings (shared between AdjRib and
// AdjRibGroup) Each AdjRibEntry contains a postPolicyResult_ string_view that
// points to the string key in this cache.
PostPolicyResultCacheT postPolicyResultCache_;

void tryUpdateAttrToPrefixMapImpl(
    const std::pair<folly::CIDRNetwork, uint32_t>& prefixPathId,
    const std::shared_ptr<const BgpPath>& oldPath,
    const std::shared_ptr<const BgpPath>& newPath,
    AttrToPrefixMap& attrToPrefixMap,
    const std::string& contextName,
    AdjRibStats& stats,
    bool isNexthopSetByPolicy) {
  auto afi = prefixPathId.first.first.isV4()
      ? nettools::bgplib::BgpUpdateAfi::AFI_IPv4
      : nettools::bgplib::BgpUpdateAfi::AFI_IPv6;

  /*
   * This helper applies the requested packing-list mutation unconditionally; it
   * does NOT dedup an unchanged advertisement (newPath == oldPath). Suppressing
   * no-op re-advertisements -- and withdrawals for prefixes that were never
   * advertised -- is the caller's responsibility: processRibAnnouncedEntry
   * compares the policy result before staging, and the withdraw call sites only
   * fire when the prefix was previously announced (postAttr non-null).
   */
  XLOGF(
      DBG5,
      "{}Updating packing list for {}",
      contextName.empty() ? "" : contextName + " ",
      prefixPathId.first.first.str());

  /*
   * Clean up prefixPathId from BOTH flag buckets.
   *
   * isNexthopSetByPolicy is part of the key {attrs, afi, isNexthopSetByPolicy},
   * so an announce and a withdraw can stage the same prefix under DIFFERENT
   * flags: the announce passes the real flag (true when a SetNexthop policy
   * ran), the withdraw defaults to false. Cleaning up only the caller's flag
   * misses the other bucket and leaves the prefix double-booked, e.g.:
   *
   *   attrToPrefixMap_ for Prefix A (the bug):
   *     announcement:  {attr1,   v6, true }  -> [Prefix A]
   *     withdrawal:    {nullptr, v6, false}  -> [Prefix A]   (A in BOTH!)
   *
   * Probe both flag states so A is associated to at most one path.
   * Note: oldPath can be nullptr (withdrawal), a valid map key.
   */
  auto cleanupPrefixAssociationInPackingList = [&](bool isNexthopSetByPolicy) {
    auto oldPathKey = BgpPathWithAfi{oldPath, afi, isNexthopSetByPolicy};
    auto oldPfxSetItr = attrToPrefixMap.find(oldPathKey);
    if (oldPfxSetItr == attrToPrefixMap.end()) {
      return;
    }

    auto& oldPfxSet = oldPfxSetItr->second;
    auto foundPfx = oldPfxSet.find(prefixPathId);

    /**
     * Two cases:
     * 1. We find the prefix in the prefix set; we did not
     *    queue this packed prefix to adjRibOutQueue_ before the next
     *    update came in.
     *
     * 2. We don't find this prefix in the prefix set. We already
     *    wrote this update to adjRibOutQueue_ and removed it from the
     *    packing list.
     *
     * Both are valid scenarios. We handle (1) below.
     */
    if (foundPfx != oldPfxSet.end()) {
      oldPfxSet.erase(foundPfx);
      BgpStats::incrementEgressTransientRouteUpdatesSuppressed();
      // Track per-peer or per-group stats
      stats.incrementTransientRouteUpdatesSuppressed();
    }

    if (oldPfxSet.empty()) {
      // Clean up since this attr doesn't have any associated pfxs
      attrToPrefixMap.erase(oldPfxSetItr);
    }
  };

  cleanupPrefixAssociationInPackingList(/*isNexthopSetByPolicy=*/true);
  cleanupPrefixAssociationInPackingList(/*isNexthopSetByPolicy=*/false);

  // Pack prefixPathId with the newPath
  auto newPathKey = BgpPathWithAfi{newPath, afi, isNexthopSetByPolicy};
  auto newPathItr = attrToPrefixMap.find(newPathKey);

  if (newPathItr == attrToPrefixMap.end()) {
    // If newPath doesn't exist, create a new entry
    PrefixSet prefixes;
    prefixes.insert(prefixPathId);
    attrToPrefixMap.emplace(newPathKey, std::move(prefixes));
  } else {
    // If newPath is already present, add to existing prefix set
    auto& prefixes = newPathItr->second;

    if (prefixes.contains(prefixPathId)) {
      // Invariant is that prefixPathId can only be associated to one attr.
      // We cleaned up the old state, so we should NOT see this prefix again.
      // If we see it, the invariant is violated and we have inconsistent state.
      XLOGF(
          WARN,
          "{}Packing list already contains prefix {}",
          contextName.empty() ? "" : contextName + " ",
          prefixPathId.first.first.str());
    }
    prefixes.insert(prefixPathId);
  }
}

/**
 * @brief Replace zeros in AS-PATH with the specified value
 *
 * @details This is a helper used by updateAsPathAttributesCommon.
 * Replaces all zero ASNs in the AS-PATH with the given replacement value.
 *
 * @param attrs - Attributes to modify (must not be published)
 * @param replaceValue - Value to replace zeros with (typically local AS)
 */
void replaceZerosInAsPath(
    std::shared_ptr<BgpPath> attrs,
    uint32_t replaceValue) noexcept {
  if (!attrs) {
    return;
  }
  CHECK(!attrs->isPublished());
  auto newAsPath = attrs->getAsPath().get();
  for (auto& asSegment : newAsPath) {
    std::replace(
        asSegment.asSequence.begin(),
        asSegment.asSequence.end(),
        (uint32_t)0,
        replaceValue);
  }
  attrs->setAsPath(std::move(newAsPath));
}

void removeConfedAsPathSegments(std::vector<BgpAttrAsPathSegmentC>& asPath) {
  for (auto it = asPath.cbegin(); it != asPath.cend();) {
    if (it->isConfedSegment()) {
      it = asPath.erase(it);
    } else {
      ++it;
    }
  }
}

void removePrivateAsns(std::vector<BgpAttrAsPathSegmentC>& asPath) {
  for (const auto& seg : asPath) {
    if (seg.hasPublicAsn()) {
      XLOG(ERR, "Update has public ASN, will not remove private ASNs");
      return;
    }
  }
  asPath.clear();
}

void prependAsPath(
    std::vector<BgpAttrAsPathSegmentC>& asPath,
    uint32_t localAs,
    bool isConfedPeer) {
  if (!isConfedPeer) {
    if (asPath.size() > 0 && asPath[0].asSequence.size()) {
      auto it = asPath[0].asSequence.cbegin();
      asPath[0].asSequence.insert(it, localAs);
    } else {
      BgpAttrAsPathSegmentC segment;
      segment.asSequence.push_back(localAs);
      auto it = asPath.cbegin();
      asPath.insert(it, segment);
    }
    return;
  }

  if (asPath.size() > 0 && asPath[0].asConfedSequence.size()) {
    auto it = asPath[0].asConfedSequence.cbegin();
    asPath[0].asConfedSequence.insert(it, localAs);
  } else {
    BgpAttrAsPathSegmentC segment;
    segment.asConfedSequence.push_back(localAs);
    auto it = asPath.cbegin();
    asPath.insert(it, segment);
  }
}

bool validateEnforceFirstAs(
    const std::shared_ptr<const BgpPath>& attrs,
    uint32_t remoteAs,
    bool isIBgpPeer) {
  // Only apply enforce-first-as for eBGP and confed eBGP peers
  if (isIBgpPeer) {
    return true;
  }

  const auto& asPathDedup = attrs->getAsPath();
  if (asPathDedup.nullOrEmpty()) {
    // Empty AS path is invalid for EBGP
    return false;
  }

  // Get the first AS path segment
  const auto& firstSegment = (*asPathDedup)[0];

  // Check if the first AS in the path matches the accepted remote AS
  if (!firstSegment.asSequence.empty()) {
    auto firstAs = firstSegment.asSequence[0];
    if (firstAs != remoteAs) {
      XLOGF(
          ERR,
          "Enforce-first-as validation failed: first AS {} does not match peer AS {}",
          firstAs,
          remoteAs);
      return false;
    }
  } else if (!firstSegment.asConfedSequence.empty()) {
    auto firstAs = firstSegment.asConfedSequence[0];
    if (firstAs != remoteAs) {
      XLOGF(
          ERR,
          "Enforce-first-as validation failed: first AS {} does not match peer AS {}",
          firstAs,
          remoteAs);
      return false;
    }
  }

  return true;
}

void updateAsPathAttributesCommon(
    const PeeringParams& peeringParams,
    std::shared_ptr<BgpPath> attrsToUpdate) noexcept {
  replaceZerosInAsPath(attrsToUpdate, peeringParams.localAs);

  if (AdjRibCommonUtils::isEBgpPeer(peeringParams)) {
    auto newAsPath = attrsToUpdate->getAsPath().get();

    removeConfedAsPathSegments(newAsPath);

    if (peeringParams.removePrivateAs) {
      removePrivateAsns(newAsPath);
    }

    const auto asnToPrepend = peeringParams.asConfedId
        ? *peeringParams.asConfedId
        : peeringParams.localAs;
    prependAsPath(newAsPath, asnToPrepend, false);
    attrsToUpdate->setAsPath(std::move(newAsPath));
  } else if (AdjRibCommonUtils::isConfedEBgpPeer(peeringParams)) {
    auto newAsPath = attrsToUpdate->getAsPath().get();
    prependAsPath(newAsPath, peeringParams.localAs, true);
    attrsToUpdate->setAsPath(std::move(newAsPath));
  }
}

void updateLocalPrefCommon(
    const PeeringParams& peeringParams,
    std::shared_ptr<BgpPath> attrsToUpdate) noexcept {
  if (AdjRibCommonUtils::isEBgpPeer(peeringParams)) {
    attrsToUpdate->setLocalPref(std::nullopt);
  }
}

void updateMedCommon(
    const PeeringParams& peeringParams,
    std::shared_ptr<BgpPath> attrsToUpdate,
    const PostPolicyInfo& postPolicyInfo) noexcept {
  const FeatureFlags::BgpBestpathFeatures& bgpBestpathFeatures =
      FeatureFlags::getBgpBestpathFeatures();

  if (AdjRibCommonUtils::isEBgpPeer(peeringParams)) {
    if (!bgpBestpathFeatures.enableMedComparison ||
        !postPolicyInfo.isMedSetByPolicy) {
      attrsToUpdate->unSetMed();
    }
  }
}

void updateOriginAndClusterListCommon(
    const PeeringParams& peeringParams,
    const RibOutAnnouncementEntry& update,
    std::shared_ptr<BgpPath> attrsToUpdate) noexcept {
  if (AdjRibCommonUtils::isEBgpPeer(peeringParams) ||
      AdjRibCommonUtils::isConfedEBgpPeer(peeringParams)) {
    attrsToUpdate->setOriginatorId(0);
    attrsToUpdate->setClusterList({});
  }

  if (peeringParams.isRrClient) {
    if (update.peer.sessionType != BgpSessionType::IBGP ||
        update.peer.addr.isZero()) {
      attrsToUpdate->setOriginatorId(peeringParams.localBgpId.toLongHBO());
    } else if (!attrsToUpdate->getOriginatorId()) {
      attrsToUpdate->setOriginatorId(update.peer.routerId);
    }
    auto newClusterList = attrsToUpdate->getClusterList().get();
    newClusterList.insert(
        newClusterList.begin(), peeringParams.localClusterId.toLongHBO());
    attrsToUpdate->setClusterList(std::move(newClusterList));
  }
}

void applyPartialDrainCommunities(
    const std::shared_ptr<BgpPath>& attrsToUpdate) noexcept {
  auto comms = attrsToUpdate->getCommunities().get();
  BgpAttrCommunitiesC toAdd;
  toAdd.push_back(kDrainCommunity);
  BgpAttrCommunitiesC toRemove;
  toRemove.push_back(kLiveCommunity);
  comms = addCommunities(comms, toAdd);
  comms = removeCommunities(comms, toRemove);
  attrsToUpdate->setCommunities(std::move(comms));
}

void pruneLbwExtCommunitiesCommon(BgpAttrExtCommunitiesC& communities) {
  float lowestLbw = FLT_MAX;
  bool shouldPrune = false;

  union {
    uint32_t intVal;
    float floatVal;
  } tmp{};
  // To avoid lint error
  tmp.floatVal = 0.0f;

  /* Find lowest non-negative LBW value. */
  for (auto& comm : communities) {
    if (comm.isLinkBandwidthCommunity()) {
      tmp.intVal = comm.getRawValueInWords().second;
      auto lbw = tmp.floatVal;
      /*
       * RFC indicates negative value should not be attached or originated
       * by any BGP speaker.
       */
      if (lbw < 0) {
        shouldPrune = true;
        continue;
      }

      /*
       * If the lowest lbw has already been updated, we know
       * there is more than one LBW present.
       */
      shouldPrune |= (lowestLbw != FLT_MAX);
      lowestLbw = std::min(lowestLbw, lbw);
    }
  }

  if (!shouldPrune) {
    return;
  }

  /* Shift all unwanted elements to the end and resize vector. */
  std::erase_if(communities, [&](const BgpAttrExtCommunityC& comm) {
    if (comm.isLinkBandwidthCommunity()) {
      tmp.intVal = comm.getRawValueInWords().second;
      return (tmp.floatVal > lowestLbw) || (tmp.floatVal < 0);
    }
    return false;
  });
}

void updateExtCommunitiesCommon(
    const PeeringParams& peeringParams,
    const PolicyAttributesMask* mask,
    std::shared_ptr<BgpPath> attrsToUpdate) noexcept {
  if (!attrsToUpdate) {
    return;
  }
  /**
   * If UCMP/GAR is enabled, then we make no modifications to the
   * LBW extCommunities that have already been modified via config actions
   * and/or local policy. This is to preserve the original forwarding behavior
   * for extended communities in DC when UCMP/GAR is explicitly enabled.
   *
   * To infer UCMP/GAR is enabled (i.e. that the current speaker is
   * participating in UCMP/GAR routing intent as a sender), at least one of the
   * following must be true:
   *
   *   1. LbwExtCommunityAction is in the peer's configured policy
   *   2. AdvertiseLinkBandwidth is configured in peer's config
   */
  bool keepNonTransitiveLbw = (mask && mask->customizedLbwEnabled) ||
      peeringParams.advertiseLinkBandwidth.has_value();

  BgpAttrExtCommunitiesC newExtCommunities;
  auto& extCommunities = attrsToUpdate->getExtCommunities().get();

  if (AdjRibCommonUtils::isEBgpPeer(peeringParams)) {
    /**
     * For EBGP peers, remove non-transitive extended communities (RFC 4360).
     * If a custom LBW is in effect, keep it on the extCommunities attribute.
     * Note, if AdvertiseLinkBandwidth::DISABLE was activated in either policy
     * or config, then the non-transitive LBW has already been removed from
     * the post policy attribute.
     */
    for (auto& community : extCommunities) {
      if (community.isTransitive() ||
          (keepNonTransitiveLbw &&
           community.isNonTransitiveLinkBandwidthCommunity())) {
        newExtCommunities.push_back(community);
      }
    }
  } else {
    /*
     * No non-transitive pruning needed for IBGP, EBGP-Confed, but
     * still need to prune the lowest LBW value, so we make a copy here
     * as well.
     */
    newExtCommunities = extCommunities;
  }

  /*
   * Forward only the lowest transitive and lowest non-transitive LBW.
   */
  pruneLbwExtCommunitiesCommon(newExtCommunities);

  /*
   * If size isn't the same, we know that pruning occurred and modifications
   * were made. Update the extCommunities field.
   */
  if (newExtCommunities.size() != extCommunities.size()) {
    attrsToUpdate->setExtCommunities(std::move(newExtCommunities));
  }
}

void overridePrePolicyAttributesCommon(
    const PolicyAttributesMask* mask,
    const std::shared_ptr<const BgpPath>& policyCachedAttrs,
    std::shared_ptr<BgpPath> attrsToOverride) noexcept {
  CHECK(mask);
  CHECK(attrsToOverride);
  CHECK(policyCachedAttrs);
  if (mask->origin) {
    attrsToOverride->setOrigin(policyCachedAttrs->getOrigin());
  }
  if (mask->asPath) {
    attrsToOverride->setAsPath(policyCachedAttrs->getAsPath().get());
  }
  if (mask->nexthop) {
    attrsToOverride->setNexthop(policyCachedAttrs->getNexthop());
  }
  if (mask->med) {
    attrsToOverride->setMed(policyCachedAttrs->getMed());
  }
  if (mask->weight) {
    attrsToOverride->setWeight(policyCachedAttrs->getWeight());
  }
  if (mask->backupAddr) {
    attrsToOverride->setBackupAddr(policyCachedAttrs->getBackupAddr());
  }
  if (mask->localPref) {
    attrsToOverride->setLocalPref(policyCachedAttrs->getLocalPref());
  }
  if (mask->atomicAggregate) {
    attrsToOverride->setAtomicAggregate(
        policyCachedAttrs->getAtomicAggregate());
  }
  if (mask->aggregator) {
    attrsToOverride->setAggregator(policyCachedAttrs->getAggregator());
  }
  if (mask->communities) {
    attrsToOverride->setCommunities(policyCachedAttrs->getCommunities().get());
  }
  if (mask->originatorId) {
    attrsToOverride->setOriginatorId(policyCachedAttrs->getOriginatorId());
  }
  if (mask->clusterList) {
    attrsToOverride->setClusterList(policyCachedAttrs->getClusterList().get());
  }
  if (mask->extCommunities) {
    attrsToOverride->setExtCommunities(
        policyCachedAttrs->getExtCommunities().get());
    /**
     * TopologyInfo can be set by policy, and we need to pass this information
     * for GAR weights programing.
     *
     * Note that as of right now, topology info can only be set by the
     * the LbwExtCommunity DECODE_ALL. All pre policy attrs always have
     * an empty topologyInfo map. We also know that if two paths have
     * the same extCommunities, then they must have the same topologyInfo.
     */
    attrsToOverride->setTopologyInfo(policyCachedAttrs->getTopologyInfo());
  }
}

void updateAttributesOutWithoutNexthopCommon(
    const PeerConfig& config,
    const RibOutAnnouncementEntry& update,
    const std::shared_ptr<const BgpPath>& policyResultAttrs,
    std::shared_ptr<BgpPath> attrsToUpdate,
    const PostPolicyInfo& postPolicyInfo) noexcept {
  if (!attrsToUpdate || !policyResultAttrs) {
    return;
  }
  CHECK(!attrsToUpdate->isPublished());

  const PolicyAttributesMask* mask = nullptr;
  if (config.egressPolicyName.has_value() && config.policy) {
    mask = config.policy->getPolicyAttributesMask(*config.egressPolicyName);
    overridePrePolicyAttributesCommon(mask, policyResultAttrs, attrsToUpdate);
  }

  updateAsPathAttributesCommon(config.peeringParams, attrsToUpdate);
  updateLocalPrefCommon(config.peeringParams, attrsToUpdate);
  updateMedCommon(config.peeringParams, attrsToUpdate, postPolicyInfo);
  updateOriginAndClusterListCommon(config.peeringParams, update, attrsToUpdate);
  updateExtCommunitiesCommon(config.peeringParams, mask, attrsToUpdate);
}

uint32_t packPrefixesCommon(
    PrefixSet& prefixPathIds,
    std::vector<nettools::bgplib::RiggedIPPrefix>& bgpUpdatePrefixes,
    bool sendAddPath,
    const std::string& context) noexcept {
  if (prefixPathIds.empty()) {
    if (!context.empty()) {
      XLOGF_EVERY_MS(WARN, 10000, "Unexpected empty prefix set in {}", context);
    } else {
      XLOGF_EVERY_MS(WARN, 10000, "Unexpected empty prefix set");
    }
    return 0;
  }
  int packedPfxs = 0;
  for (const auto& [prefix, pathId] : prefixPathIds) {
    nettools::bgplib::RiggedIPPrefix rPrefix;
    rPrefix.prefix() = network::toIPPrefix(prefix);
    // if add path enabled.
    if (sendAddPath) {
      rPrefix.pathId() = pathId;
    }
    bgpUpdatePrefixes.emplace_back(std::move(rPrefix));

    ++packedPfxs;
  }
  prefixPathIds.clear();
  return packedPfxs;
}

uint32_t packPrefixesWithLimitCommon(
    const uint32_t approximateSerializedAttrLen,
    PrefixSet& prefixPathIds,
    std::vector<nettools::bgplib::RiggedIPPrefix>& bgpUpdatePrefixes,
    bool sendAddPath,
    const std::string& context) noexcept {
  if (prefixPathIds.empty()) {
    if (!context.empty()) {
      XLOGF_EVERY_MS(WARN, 10000, "Unexpected empty prefix set in {}", context);
    } else {
      XLOGF_EVERY_MS(WARN, 10000, "Unexpected empty prefix set");
    }
    return 0;
  }

  // Determine if prefixes are IPv4 or IPv6 by checking the first prefix
  const auto& firstPrefix = prefixPathIds.cbegin()->first;
  const bool isV4 = firstPrefix.first.isV4();

  // Get the maximum prefix length based on whether we have path IDs and address
  // family
  const size_t maxPrefixLen =
      nettools::bgplib::BgpMessageSerializer::getMaxPrefixLen(
          sendAddPath, isV4);

  // Compute the prefix limit based on available space and chain length
  // Formula: (kMaxBgpMsgLen - kBgpMsgHeaderLen - approximateSerializedAttrLen)
  // * (kMaxSerializedChainLen / maxPrefixLen)
  static const int32_t kRemainingBytes =
      nettools::bgplib::kMaxBgpMsgLen - nettools::bgplib::kBgpMsgHeaderLen;
  const uint32_t prefixLimit =
      (kRemainingBytes - approximateSerializedAttrLen) *
      kMaxSerializedChainLen / maxPrefixLen;

  uint32_t packedPfxs = 0;

  for (auto it = prefixPathIds.cbegin(); it != prefixPathIds.cend();) {
    if (packedPfxs >= prefixLimit) {
      break;
    }
    auto& prefix = it->first;

    nettools::bgplib::RiggedIPPrefix rPrefix;
    rPrefix.prefix() = network::toIPPrefix(prefix);
    if (sendAddPath) {
      rPrefix.pathId() = it->second;
    }
    bgpUpdatePrefixes.emplace_back(std::move(rPrefix));

    it = prefixPathIds.erase(it);
    ++packedPfxs;
  }
  return packedPfxs;
}

} // namespace facebook::bgp
