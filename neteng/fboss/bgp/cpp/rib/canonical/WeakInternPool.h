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

#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalRibEncoding.h"

namespace facebook::bgp::canonical {

/*
 * Intern pool for continuous encoding.
 *
 * Unlike StrongInternPool, this pool must not extend the lifetime of values in
 * the live RIB. The raw pointer provides a cheap lookup key and the weak_ptr
 * proves that the object at that address is still the one originally
 * interned. A non-expired weak_ptr means the original object remains alive, so
 * its existing ID is reusable. If the weak_ptr expired, the raw address is
 * stale (and may already have been reused); the stale slot is removed and a
 * new monotonic ID is assigned. This prevents an ABA collision.
 *
 * sweep() lazily removes expired slots. IDs are never reused, so an entry can
 * never silently resolve to a different value between publications.
 */
template <typename ObjT>
class WeakInternPool {
 public:
  /**
   * Return the stable ID for a deduplicated object without retaining it.
   *
   * @param obj Non-null object whose pointer identity is the lookup key.
   * @return Stable object ID and whether this call changed the pool.
   *
   * If an expired slot has the same address, it is discarded and the object
   * receives a new monotonic ID. ID exhaustion fails an XCHECK.
   */
  InternResult internReporting(const std::shared_ptr<const ObjT>& obj) {
    const ObjT* key = obj.get();
    if (auto it = byPtr_.find(key); it != byPtr_.end()) {
      if (!it->second.weak.expired()) {
        return {.id = it->second.id, .poolChanged = false};
      }
      byPtr_.erase(it);
    }
    /*
     * Pool IDs are nonnegative signed Thrift keys and never reused within an
     * encoder epoch. Exhausting this space requires impossible churn or state
     * corruption; continuing cannot produce a valid, unambiguous reference.
     */
    XCHECK_LT(nextId_, std::numeric_limits<int64_t>::max())
        << "canonical RIB pool ID exhausted";
    const int64_t id = nextId_++;
    byPtr_.emplace(key, Slot{id, obj});
    return {.id = id, .poolChanged = true};
  }

  /**
   * Look up a live object already supplied to internReporting().
   *
   * @param obj Deduplicated object whose pointer identity is the lookup key.
   * @return The previously allocated ID.
   * Fails an XCHECK if the object is not present.
   */
  int64_t indexOf(const std::shared_ptr<const ObjT>& obj) const {
    auto it = byPtr_.find(obj.get());
    /*
     * Encoding interns each object before resolving its ID. Absence therefore
     * means the encoding sequence is broken, and continuing would publish a
     * path whose pool reference cannot be resolved.
     */
    XCHECK(it != byPtr_.end()) << "canonical RIB object was not interned";
    return it->second.id;
  }

  /**
   * Project every still-live object into an ID-keyed Thrift pool.
   *
   * @tparam ValueT Output value stored for each ID.
   * @param project Callable receiving const ObjT& and returning ValueT.
   * @return A new map containing live IDs and projected values. Expired slots
   *     are omitted but remain tracked until sweep() or address reuse.
   */
  template <typename ValueT, typename ProjectFn>
  folly::F14FastMap<int64_t, ValueT> snapshot(ProjectFn&& project) const {
    folly::F14FastMap<int64_t, ValueT> out;
    out.reserve(byPtr_.size());
    for (const auto& [_, slot] : byPtr_) {
      if (auto obj = slot.weak.lock()) {
        out.emplace(slot.id, project(*obj));
      }
    }
    return out;
  }

  /**
   * Remove slots whose source objects are no longer retained by the Loc-RIB.
   *
   * @return True if at least one slot was removed. Retired IDs are never
   *     reassigned.
   */
  bool sweep() {
    bool reclaimed = false;
    for (auto it = byPtr_.begin(); it != byPtr_.end();) {
      if (it->second.weak.expired()) {
        it = byPtr_.erase(it);
        reclaimed = true;
      } else {
        ++it;
      }
    }
    return reclaimed;
  }

  /** @return Resident slot count, including expired slots not yet swept. */
  size_t trackedCount() const {
    return byPtr_.size();
  }

  /** @return Current resident slot count and monotonic ID high-water mark. */
  PoolStats stats() const {
    return PoolStats{
        .live = trackedCount(), .highWater = static_cast<size_t>(nextId_)};
  }

 private:
  struct Slot {
    int64_t id;
    std::weak_ptr<const ObjT> weak;
  };

  folly::F14FastMap<const ObjT*, Slot> byPtr_;
  int64_t nextId_{0};
};

} // namespace facebook::bgp::canonical
