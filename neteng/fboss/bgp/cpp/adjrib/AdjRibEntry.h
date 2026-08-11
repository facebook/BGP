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

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStructs.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/Utils.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"

namespace facebook::bgp {

using nettools::bgplib::DeDuplicatedBgpPath;

extern PostPolicyResultCacheT postPolicyResultCache_;

// Adjacency Rib entry
struct AdjRibEntry {
  // Bitmap flags for AdjRibEntry state
  // Bit 0:    isStale - Marked for session down with GR
  // Bit 1:    nexthopSetByPolicy - egress SetNexthop action fired (CLI display)
  // Bit 2:    hasOldPathId - RIB-IN only: oldPathId_ holds a path id this
  //           entry took over during an add-path graceful restart
  // Bits 3-4: pendingOp - RIB-IN only: which operation this entry contributes
  //           when the restart flushes (PendingOp: none | announce | withdraw)
  // Bits 5-7: Reserved for future use
  uint8_t flags_{0};

  // Which RIB operation an entry contributes when an add-path restart flushes.
  enum class PendingOp : uint8_t {
    None = 0,
    Announce = 1,
    Withdraw = 2,
  };

  // Flag bit positions / masks
  static constexpr uint8_t kStaleBit = 0;
  static constexpr uint8_t kNexthopSetByPolicyBit = 1;
  static constexpr uint8_t kHasOldPathIdBit = 2;
  static constexpr uint8_t kPendingOpShift = 3;
  static constexpr uint8_t kPendingOpValueMask = 0x3; // 2 bits: flags_ bits 3-4
  /*
   * Markers that belong to RIB-IN only. copyEntryForOwner() strips them so a
   * RIB-OUT clone cannot inherit them.
   */
  static constexpr uint8_t kRibInOnlyFlagsMask =
      (1 << kHasOldPathIdBit) | (kPendingOpValueMask << kPendingOpShift);

  bool isStale() const {
    return (flags_ & (1 << kStaleBit)) != 0;
  }

  void setStale(bool stale) {
    if (stale) {
      flags_ |= (1 << kStaleBit);
    } else {
      flags_ &= ~(1 << kStaleBit);
    }
  }

  /**
   * Flag for CLI display only. Indicates the egress policy's SetNexthop
   * action fired for this entry. Used by AdjRibShow to display the
   * correct nexthop instead of blindly applying nexthop-self.
   *
   * NOTE: This flag is intentionally duplicated with
   * BgpPathWithAfi::isNexthopSetByPolicy. Packing list and
   * advertisement use the BgpPathWithAfi key flag (which is captured
   * at insertion time and is immune to backpressure-induced races).
   * This AdjRibEntry flag always reflects the latest policy result
   * and is suitable for CLI display, which should show current state.
   * See BgpPathWithAfi comment in AdjRibStructs.h for the full
   * rationale on why both flags are needed.
   */
  bool isNexthopSetByPolicy() const {
    return (flags_ & (1 << kNexthopSetByPolicyBit)) != 0;
  }

  void setNexthopSetByPolicy(bool value) {
    if (value) {
      flags_ |= (1 << kNexthopSetByPolicyBit);
    } else {
      flags_ &= ~(1 << kNexthopSetByPolicyBit);
    }
  }

  /*
   * Add-path graceful-restart markers. RIB-IN only: they are working state for
   * one restart, never copied to a RIB-OUT clone and never serialized.
   *
   * oldPathId_ holds a path id already installed in the RIB that this entry has
   * taken over from its matching stale twin. Presence is a flag bit rather than
   * a sentinel value, because 0 and UINT32_MAX are both valid path ids.
   */
  bool hasOldPathId() const {
    return (flags_ & (1 << kHasOldPathIdBit)) != 0;
  }

  // Only meaningful when hasOldPathId().
  uint32_t getOldPathId() const {
    return oldPathId_;
  }

  void setOldPathId(uint32_t oldPathId) {
    oldPathId_ = oldPathId;
    flags_ |= (1 << kHasOldPathIdBit);
  }

  void clearOldPathId() {
    oldPathId_ = 0;
    flags_ &= ~(1 << kHasOldPathIdBit);
  }

  // Which operation this entry contributes when the restart flushes.
  PendingOp getPendingOp() const {
    return static_cast<PendingOp>(
        (flags_ >> kPendingOpShift) & kPendingOpValueMask);
  }

  void setPendingOp(PendingOp op) {
    flags_ = (flags_ & ~(kPendingOpValueMask << kPendingOpShift)) |
        (static_cast<uint8_t>(op) << kPendingOpShift);
  }

  // Clears both markers. Idempotent.
  void clearAddPathGrState() {
    oldPathId_ = 0;
    flags_ &= ~kRibInOnlyFlagsMask;
  }

  explicit AdjRibEntry(uint32_t pathId) : pathId_(pathId) {}

  void setPreIn(const std::shared_ptr<const BgpPath>& attrs) {
    if (attrs) {
      /*
       * Dedup via DeDuplicatedBgpPath, as setPostAttr does: AdjRib mints ONE
       * BgpPath per UPDATE PDU (AdjRibIn.cpp, outside the NLRI loop), so
       * without interning the pre-policy footprint tracks UPDATE COUNT rather
       * than distinct values -- N updates carrying byte-identical attributes
       * and nexthop cost N objects. A table dump split per prefix, a flap
       * storm, or add-path all drive PDU count toward prefix count, and how a
       * peer packs its NLRI is not something this side controls.
       */
      DeDuplicatedBgpPath deduped(std::const_pointer_cast<BgpPath>(attrs));
      prePolicyAttrs_ = deduped.getSharedPtr();
    } else {
      prePolicyAttrs_ = nullptr;
    }
    // We only modify lastUpdateRcvdUsec_ when pre-in attributes have changed
    lastUpdateRcvdUsec_ = getCurrentTimeMicroSec();
  }

  void setPostAttr(const std::shared_ptr<const BgpPath>& attrs) {
    if (attrs) {
      // Dedup via DeDuplicatedBgpPath: identical BgpPath values share one ptr
      DeDuplicatedBgpPath deduped(std::const_pointer_cast<BgpPath>(attrs));
      postPolicyAttrs_ = deduped.getSharedPtr();
    } else {
      postPolicyAttrs_ = nullptr;
    }
  }

  void setPreOut(const std::shared_ptr<const BgpPath>& attrs) {
    if (prePolicyAttrs_) {
      prePolicyAttrs_->decOnAdjPreoutCount();
    }
    prePolicyAttrs_ = attrs;
    if (prePolicyAttrs_) {
      prePolicyAttrs_->incOnAdjPreoutCount();
    }
  }

  void setPostInPolicy(const std::string& policyName) {
    setPostPolicy(policyName);
  }

  void setPostOutPolicy(const std::string& policyName) {
    setPostPolicy(policyName);
  }

  const std::shared_ptr<const BgpPath>& getPreIn() const {
    return prePolicyAttrs_;
  }

  const std::shared_ptr<const BgpPath>& getPostAttr() const {
    return postPolicyAttrs_;
  }

  const std::shared_ptr<const BgpPath>& getPreOut() const {
    return prePolicyAttrs_;
  }

  uint32_t getPathId() {
    return pathId_;
  }

  /*
   * Compare post-policy attributes with another entry.
   * Returns true if both entries exist and have equivalent post-policy state.
   * Used during collapse to decide whether PL correction is needed.
   *
   * Note: Only compares postPolicyAttrs_. Prefix and pathId are already
   * matched by the tree key and path map key respectively before this
   * is called (see collapseLiteEntry/collapsePathEntry).
   */
  bool hasMatchingPostPolicyAttrs(const AdjRibEntry* other) const {
    return other && postPolicyAttrs_ == other->postPolicyAttrs_;
  }

  uint64_t getLastUpdateRcvdTime() const {
    return lastUpdateRcvdUsec_;
  }

  uint64_t getRibVersion() const {
    return ribVersion_;
  }

  void setRibVersion(uint64_t version) {
    ribVersion_ = version;
  }

  const PostPolicyResultT& getPostOutPolicy() const {
    return postPolicyResult_;
  }

  const PostPolicyResultT& getPostInPolicy() const {
    return postPolicyResult_;
  }

 private:
  std::shared_ptr<const BgpPath> prePolicyAttrs_;
  std::shared_ptr<const BgpPath> postPolicyAttrs_;
  PostPolicyResultT postPolicyResult_;

  uint32_t pathId_{kDefaultPathID};

  /*
   * Path id this entry has taken over from its stale twin; valid only when
   * hasOldPathId() (flags_ bit 2) is set.
   * Placed immediately after pathId_ so it occupies the 4 bytes of padding that
   * already followed it: sizeof(AdjRibEntry) stays 80 and per-entry RIB memory
   * is unchanged.
   */
  uint32_t oldPathId_{0};

  // last modified time in microseconds since epoch
  int64_t lastUpdateRcvdUsec_{0};

  // RIB version when this entry was last updated
  uint64_t ribVersion_{0};

  void clearPostPolicyResult() {
    // If there are no AdjRibEntry referencing the postPolicyResult_,
    // the base use_count() is 1 from existing in the set.
    // Hence we additionally subtract baseline use_count for pruning.
    if (postPolicyResult_ && (postPolicyResult_.use_count() - 1) == 1) {
      if (postPolicyResultCache_.erase(postPolicyResult_)) {
        RibStats::decrPostPolicyResultCacheCount();
      }
    }
  }

  void setPostPolicyResult(const PostPolicyResultT& term) {
    auto ret = postPolicyResultCache_.insert(term);
    if (ret.second) {
      RibStats::incrPostPolicyResultCacheCount();
    }
    postPolicyResult_ = *ret.first;
  }

  // Set the post policy result on AdjRibEntry after
  // inserting into cache. Prune unused policy terms.
  void setPostPolicy(const std::string& policyName) {
    clearPostPolicyResult();
    setPostPolicyResult(std::make_shared<const std::string>(policyName));
  }
};

} // namespace facebook::bgp
