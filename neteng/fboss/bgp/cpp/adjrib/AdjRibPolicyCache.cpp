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

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibPolicyCache.h"

namespace facebook::bgp {

std::shared_ptr<AdjRibPolicyCache> AdjRibPolicyCache::get() {
  return folly::Singleton<AdjRibPolicyCache>::try_get();
}

std::size_t AdjRibPolicyCache::PolicyCacheMaskedKeyHash::operator()(
    const PolicyCacheMaskedKey& key) const noexcept {
  std::size_t seed = 0;
  const auto mask = std::get<0>(key);
  const auto& path = std::get<2>(key);
  if (mask) {
    // The mask address uniquely identifies the policy.
    seed = folly::hash::hash_combine(seed, mask);
    // Hash prefix if indicated by mask.
    if (mask->prefix) {
      seed = folly::hash::hash_combine(seed, std::get<1>(key));
    }
    // Hash BgpPath attr if indicated by mask.
    if (!path) {
      return seed;
    }
    if (mask->origin) {
      seed = folly::hash::hash_combine(seed, path->getOrigin());
    }
    if (mask->nexthop) {
      seed = folly::hash::hash_combine(seed, path->getNexthop());
    }
    if (mask->med) {
      seed = folly::hash::hash_combine(seed, path->getMed());
    }
    if (mask->localPref) {
      seed = folly::hash::hash_combine(seed, path->getLocalPref());
    }
    if (mask->atomicAggregate) {
      seed = folly::hash::hash_combine(seed, path->getAtomicAggregate());
    }
    if (mask->originatorId) {
      seed = folly::hash::hash_combine(seed, path->getOriginatorId());
    }
    // The path attributes below need to run through custom hash first.
    if (mask->aggregator) {
      seed = folly::hash::hash_combine(seed, path->getAggregator().hash());
    }
    if (mask->asPath && path->getAsPath()) {
      seed = folly::hash::hash_combine(seed, path->getAsPath()->hash());
    }
    if (mask->communities && path->getCommunities()) {
      seed = folly::hash::hash_combine(seed, path->getCommunities()->hash());
    }
    if (mask->clusterList && path->getClusterList()) {
      seed = folly::hash::hash_combine(seed, path->getClusterList()->hash());
    }
    if (mask->extCommunities && path->getExtCommunities()) {
      seed = folly::hash::hash_combine(seed, path->getExtCommunities()->hash());
    }
    if (mask->weight) {
      seed = folly::hash::hash_combine(seed, path->getWeight());
    }
    if (mask->backupAddr) {
      seed = folly::hash::hash_combine(seed, path->getBackupAddr().has_value());
      if (path->getBackupAddr()) {
        seed = folly::hash::hash_combine(seed, *path->getBackupAddr());
      }
    }
  }
  // Hash BgpPolicyActionData.
  auto& policyActionData = std::get<3>(key);
  if (policyActionData) {
    seed = folly::hash::hash_combine(seed, policyActionData->hash());
  }
  // Always hash the partial-drain bit: it reflects a community mutation made
  // outside policy evaluation and is not captured by the masked attrs above.
  seed = folly::hash::hash_combine(seed, std::get<4>(key));
  return seed;
}

bool AdjRibPolicyCache::PolicyCacheMaskedKeyEqualTo::operator()(
    const PolicyCacheMaskedKey& lhs,
    const PolicyCacheMaskedKey& rhs) const {
  // Check equality of policy attribute mask, which uniquely identifies
  // the policy. If these are not equal, this implies the lhs and rhs policies
  // are different.
  if (get<0>(lhs) != std::get<0>(rhs)) {
    return false;
  }
  /**
   * Check equality of BgpPolicyActionData, which must use the
   * BgpPolicyActionData == operator.
   * Either (1) both are the same ptr, or (2) both are logically equivalent.
   * Otherwise lhs and rhs are not considered equal.
   */
  auto& lhsData = std::get<3>(lhs);
  auto& rhsData = std::get<3>(rhs);
  if (!(lhsData == rhsData || (lhsData && rhsData && (*lhsData == *rhsData)))) {
    return false;
  }

  // Partial-drain state must match: it reflects an out-of-policy community
  // mutation not captured by the masked attribute comparison below.
  if (std::get<4>(lhs) != std::get<4>(rhs)) {
    return false;
  }

  const auto mask = std::get<0>(lhs);
  const auto& lpath = std::get<2>(lhs);
  const auto& rpath = std::get<2>(rhs);
  if (mask) {
    // Compare prefix if indicated by mask.
    if (mask->prefix && std::get<1>(lhs) != std::get<1>(rhs)) {
      return false;
    }
    bool valid = lpath && rpath;
    // Compare BgpPath attr if indicated by mask.
    if (mask->origin && valid && lpath->getOrigin() != rpath->getOrigin()) {
      return false;
    }
    if (mask->nexthop && valid && lpath->getNexthop() != rpath->getNexthop()) {
      return false;
    }
    if (mask->med && valid && lpath->getMed() != rpath->getMed()) {
      return false;
    }
    if (mask->weight && valid && lpath->getWeight() != rpath->getWeight()) {
      return false;
    }
    if (mask->backupAddr && valid &&
        lpath->getBackupAddr() != rpath->getBackupAddr()) {
      return false;
    }
    if (mask->localPref && valid &&
        lpath->getLocalPref() != rpath->getLocalPref()) {
      return false;
    }
    if (mask->atomicAggregate && valid &&
        lpath->getAtomicAggregate() != rpath->getAtomicAggregate()) {
      return false;
    }
    if (mask->originatorId && valid &&
        lpath->getOriginatorId() != rpath->getOriginatorId()) {
      return false;
    }
    if (mask->aggregator && valid &&
        lpath->getAggregator() != rpath->getAggregator()) {
      return false;
    }
    if (mask->asPath && valid && lpath->getAsPath() != rpath->getAsPath()) {
      return false;
    }
    if (mask->communities && valid &&
        lpath->getCommunities() != rpath->getCommunities()) {
      return false;
    }
    if (mask->clusterList && valid &&
        lpath->getClusterList() != rpath->getClusterList()) {
      return false;
    }
    if (mask->extCommunities && valid &&
        lpath->getExtCommunities() != rpath->getExtCommunities()) {
      return false;
    }
  }
  return true;
}

void AdjRibPolicyCache::setCacheSize(uint32_t size) {
  maxCacheSize_ = size;
  cacheEvictionRunCount_ = maxCacheSize_ / 32;
  if (cacheEvictionRunCount_ < 1000) {
    cacheEvictionRunCount_ = 1000;
  }
  policyLruCache_.wlock()->setMaxSize(maxCacheSize_);
  XLOGF(
      DBG1,
      "Setting policy-cache size to {} eviction-run-count {}.",
      maxCacheSize_,
      cacheEvictionRunCount_);
}

void AdjRibPolicyCache::setLruClearSize(uint32_t clearSize) {
  policyLruCache_.wlock()->setClearSize(clearSize ? clearSize : 1);
}

std::optional<AdjRibPolicyCache::PolicyCacheValue>
AdjRibPolicyCache::lookupPolicyCache(
    const std::string& policyName,
    const PolicyAttributesMask* mask,
    const folly::CIDRNetwork& prefix,
    std::shared_ptr<const BgpPath> attrs,
    const std::shared_ptr<BgpPolicyActionData>& policyActionData,
    bool isPartialDrain) {
  if (maxCacheSize_ == 0) {
    return std::nullopt;
  }
  XLOGF(
      DBG5,
      "Searching {} {} in policy-cache...",
      policyName,
      folly::IPAddress::networkToString(prefix));
  auto key = AdjRibPolicyCache::PolicyCacheMaskedKey(
      mask, prefix, std::move(attrs), policyActionData, isPartialDrain);

  // Use wlock() for non-const operations
  auto guard = policyLruCache_.wlock();
  auto iter = guard->find(key);
  if (iter == guard->end()) {
    ++totalMisses_;
    return std::nullopt;
  }
  ++totalHits_;
  auto& value = iter->second;
  return value;
}

void AdjRibPolicyCache::addToPolicyCache(
    const std::string& policyName,
    const PolicyAttributesMask* mask,
    const folly::CIDRNetwork& prefix,
    std::shared_ptr<const BgpPath> attrs,
    const std::shared_ptr<BgpPolicyActionData>& policyActionData,
    std::shared_ptr<const routing::AttributesAndPolicy<BgpPath>>
        postPolicyAttrsAndTerm,
    bool isPartialDrain) {
  static uint32_t totalRuns = 0;

  if (maxCacheSize_ == 0) {
    return;
  }

  // Purge stale entries:
  // Note this is in addition to LRU eviction. We don't want to hold on to
  // some share_ptr of BgpPath long after all references to it is gone
  // just because LRU Cache has a reference to it.
  // Note that if the attrs we are holding has refcount of 1 that means
  // cache-entry is the only one referring to this and no prefix is associated
  // with this.
  if ((++totalRuns % cacheEvictionRunCount_) == 0) {
    evictFromPolicyCache();
  }

  auto key = AdjRibPolicyCache::PolicyCacheMaskedKey(
      mask,
      folly::CIDRNetwork(prefix),
      std::move(attrs),
      policyActionData,
      isPartialDrain);
  auto value = AdjRibPolicyCache::PolicyCacheValue{
      postPolicyAttrsAndTerm,
      policyActionData ? policyActionData->isMedSetByPolicy : false,
      policyActionData ? policyActionData->isNexthopSetByPolicy : false};
  policyLruCache_.wlock()->set(key, value);

  XLOGF(
      DBG4,
      "Added {} {} to policy-cache...",
      policyName,
      folly::IPAddress::networkToString(prefix));
}

void AdjRibPolicyCache::evictFromPolicyCache() {
  XLOGF(
      DBG3,
      "Policy-cache eviction loop started: size = {} misses = {} hits = {}",
      policyLruCache_.rlock()->size(),
      totalMisses_,
      totalHits_);

  // Create a list to store keys for eviction
  std::vector<AdjRibPolicyCache::PolicyCacheMaskedKey> evictionList;
  {
    auto lockedCache = policyLruCache_.rlock();
    for (auto iter = lockedCache->begin(); iter != lockedCache->end(); ++iter) {
      const auto& key = iter->first;
      const auto& value = iter->second.attrsAndPolicy;
      const auto& bgpAttributes = std::get<2>(key);

      // Check if entry is stale (no shared ownership or adjPreoutCount <= 0)
      if (bgpAttributes.unique() ||
          (bgpAttributes && bgpAttributes->getOnAdjPreoutCount() <= 0)) {
        const std::string missingPolicyResult = "MISSING POLICY RESULT";
        XLOGF(
            DBG4,
            "Evicting stale entry of {} {} from policy-cache...",
            folly::IPAddress::networkToString(std::get<1>(key)), // prefix
            value ? value->policyName : missingPolicyResult);

        // Add key to eviction list
        evictionList.emplace_back(key);
      }

      // Stop iterating if eviction list reaches clear size
      if (evictionList.size() >= kPolicyCacheClearSize) {
        break;
      }
    }
  }

  // Evict stale entries
  for (auto& entry : evictionList) {
    policyLruCache_.wlock()->erase(entry);
  }

  XLOGF(
      DBG3,
      "Policy-cache eviction loop done... total = {}",
      policyLruCache_.rlock()->size());
}

} // namespace facebook::bgp
