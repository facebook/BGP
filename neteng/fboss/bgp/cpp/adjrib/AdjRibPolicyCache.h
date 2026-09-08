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

#include <boost/noncopyable.hpp>

#include <folly/IPAddress.h>
#include <folly/Singleton.h>
#include <folly/container/EvictingCacheMap.h>
#include <folly/hash/Hash.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/policy/Policy.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyAttributesMask.h"

namespace facebook::bgp {

// Policy Cache for AdjRib.
class AdjRibPolicyCache : boost::noncopyable {
  friend class folly::Singleton<AdjRibPolicyCache>;

 public:
  // Returns the singleton instance of this class.
  static std::shared_ptr<AdjRibPolicyCache> get();

  struct PolicyCacheValue {
    std::shared_ptr<const routing::AttributesAndPolicy<BgpPath>> attrsAndPolicy;
    bool isMedSetByPolicy{false};
    bool isNexthopSetByPolicy{false};

    bool operator==(const PolicyCacheValue& other) const = default;
  };

  /*
   * lookup the global policy cache for the passed in prefix and policy
   * and attrs - if matched return the cached entry
   */
  std::optional<PolicyCacheValue> lookupPolicyCache(
      const std::string& policyName,
      const PolicyAttributesMask* mask,
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<const BgpPath> attrs,
      const std::shared_ptr<BgpPolicyActionData>& policyActionData,
      bool isPartialDrain = false);

  // add policy result to cache
  void addToPolicyCache(
      const std::string& policyName,
      const PolicyAttributesMask* mask,
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<const BgpPath> attrs,
      const std::shared_ptr<BgpPolicyActionData>& policyActionData,
      std::shared_ptr<const routing::AttributesAndPolicy<BgpPath>>
          postPolicyAttrsAndTerm,
      bool isPartialDrain = false);

  // Set max size of the LRU Cache.
  void setCacheSize(uint32_t size);

  // Number of entries to be cleared when LRU eviction kicks in.
  void setLruClearSize(uint32_t size);

  // Method to evict stale entries from the cache
  void evictFromPolicyCache();

  /**
   * @brief Return the policy cache hit from AdjRibPolicyCache
   */
  uint64_t getTotalCacheHit() const {
    return totalHits_;
  }

  /**
   * @brief Return the policy cache misses from AdjRibPolicyCache
   */
  uint64_t getTotalCacheMiss() const {
    return totalMisses_;
  }

  /**
   * @brief Reset the policy cache hit count
   */
  void resetTotalCacheHit() {
    totalHits_ = 0;
  }

  /**
   * @brief Reset the policy cache miss count
   */
  void resetTotalCacheMiss() {
    totalMisses_ = 0;
  }

  /**
   * @brief Return the number of entries inside policy cache.
   * @note: This is not the max size of the cache size.
   */
  uint64_t size() const {
    return policyLruCache_.rlock()->size();
  }

  /**
   * @brief Return the max number of entries the policy cache can hold.
   */
  std::size_t getCacheSize() const {
    return policyLruCache_.rlock()->getMaxSize();
  }

  uint64_t getCacheMemUsage() const {
    return policyLruCache_.rlock()->kApproximateEntryMemUsage *
        policyLruCache_.rlock()->size();
  }

  /**
   * @brief Reset the policy cache
   */
  void clearCache() {
    policyLruCache_.wlock()->clear();
  }

 private:
  AdjRibPolicyCache()
      : policyLruCache_(
            std::in_place,
            kMaxPolicyCacheEntries,
            kPolicyCacheClearSize) {}

  /*
   * @brief: Force purging of stale entries every `cacheEvictionRunCount_`
   * iterations.
   */
  uint32_t cacheEvictionRunCount_{kDefaultEvictionRunCount};

  /*
   * @brief: Max size to which the cache is allowed to grow.
   * @note: A size of 0 effectively means caching is disabled.
   */
  uint32_t maxCacheSize_{kMaxPolicyCacheEntries};

  /*
   * isPartialDrain (the trailing bool) is part of the key because partial
   * drain mutates standard communities (kDrainCommunity/kLiveCommunity) on the
   * pre-policy clone OUTSIDE of policy evaluation. The masked key otherwise
   * only reflects communities when the egress policy itself matches/sets them
   * (PolicyAttributesMask::communities), so a drained and an undrained clone of
   * the same prefix would collide and return each other's post-policy result.
   * Keying on isPartialDrain keeps the two states distinct.
   */
  using PolicyCacheMaskedKey = std::tuple<
      const PolicyAttributesMask*,
      folly::CIDRNetwork,
      std::shared_ptr<const BgpPath>,
      std::shared_ptr<BgpPolicyActionData>,
      bool>;

  struct PolicyCacheMaskedKeyHash {
   public:
    std::size_t operator()(const PolicyCacheMaskedKey& key) const noexcept;
  };

  struct PolicyCacheMaskedKeyEqualTo {
   public:
    bool operator()(
        const PolicyCacheMaskedKey& lhs,
        const PolicyCacheMaskedKey& rhs) const;
  };

  // LRU Cache to cache policy-lookup keys.
  using PolicyCacheType = folly::Synchronized<folly::EvictingCacheMap<
      PolicyCacheMaskedKey,
      PolicyCacheValue,
      PolicyCacheMaskedKeyHash,
      PolicyCacheMaskedKeyEqualTo>>;

  PolicyCacheType policyLruCache_;

  uint64_t totalHits_{0};
  uint64_t totalMisses_{0};

/*
 * per class placeholder for test code injection
 * only need to be setup once here
 */
#ifdef AdjRibPolicyCache_TEST_FRIENDS
  AdjRibPolicyCache_TEST_FRIENDS
#endif
};

} // namespace facebook::bgp
