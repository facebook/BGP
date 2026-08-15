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

#include <memory>
#include <string>
#include <utility>

#include <folly/IPAddress.h>

#include <optional>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStructs.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"

namespace facebook::bgp {

class AdjRibStats;
class PolicyManager;
struct PeeringParams;
struct PolicyAttributesMask;
struct RibOutAnnouncementEntry;

/**
 * @brief Metadata returned from policy evaluation
 *
 * @details Contains information about what the egress policy modified,
 * which affects how we apply subsequent attribute transformations.
 */
struct PostPolicyInfo {
  bool isMedSetByPolicy{false};
  bool isNexthopSetByPolicy{false};
};

/**
 * @brief Configuration bundle for peer-specific attribute transformations
 *
 * @details Encapsulates all peer configuration needed for attribute updates.
 * This allows sharing the same attribute transformation logic between
 * per-peer AdjRib and group-level AdjRibOutGroup.
 */
struct PeerConfig {
  const PeeringParams& peeringParams;
  const std::optional<std::string>& egressPolicyName;
  PolicyManager* policy;
};

/**
 * @brief Utility functions for peer configuration
 */
struct AdjRibCommonUtils {
  /**
   * @brief Check if peer is an EBGP peer
   */
  static bool isEBgpPeer(const PeeringParams& peeringParams);

  /**
   * @brief Check if peer is a confederation EBGP peer
   */
  static bool isConfedEBgpPeer(const PeeringParams& peeringParams);

  /**
   * @brief Get BGP session type for peer
   */
  static BgpSessionType getBgpSessionType(const PeeringParams& peeringParams);

  /**
   * @brief Check if AFI is negotiated for the given prefix
   *
   * @details This utility function checks whether the address family (IPv4 or
   * IPv6) of the given prefix is negotiated based on the provided flags. This
   * is used to filter out announcements/withdrawals for unsupported AFIs.
   *
   * @param prefix - The network prefix to check
   * @param isAfiIpv4Negotiated - Whether IPv4 AFI is negotiated
   * @param isAfiIpv6Negotiated - Whether IPv6 AFI is negotiated
   * @return true if the prefix's AFI is negotiated, false otherwise
   */
  static bool isAfiNegotiated(
      const folly::CIDRNetwork& prefix,
      bool isAfiIpv4Negotiated,
      bool isAfiIpv6Negotiated);
};

/*
 * Apply the configured outbound link-bandwidth transformation before egress
 * policy evaluation.
 */
void updateAdvertiseLbwExtCommunityCommon(
    const PeeringParams& peeringParams,
    const RibOutAnnouncementEntry& update,
    const std::shared_ptr<BgpPath>& attrs) noexcept;

/**
 * @brief Update packing list (attrToPrefixMap) with attribute-to-prefix mapping
 *
 * @details This manages the mapping from (attrs, AFI) -> set of prefixes.
 * When attributes change, we remove the prefix from the old attr's set
 * and add it to the new attr's set.
 *
 * Two cases when cleaning up old association:
 * 1. We find the prefix in the prefix set; we did not queue this packed
 *    prefix to adjRibOutQueue_ before the next update came in.
 * 2. We don't find this prefix in the prefix set. We already wrote this
 *    update to adjRibOutQueue_ and removed it from the packing list.
 * Both are valid scenarios. We handle (1) below.
 *
 * @param prefixPathId - Prefix and path ID pair
 * @param oldPath - Previous attributes (nullptr if new prefix)
 * @param newPath - New attributes (nullptr for withdrawal)
 * @param attrToPrefixMap - The packing list to update
 * @param contextName - Context name for logging (e.g., "Group foo" or peer
 * info)
 * @param stats - Stats object to track transient route updates
 */
void tryUpdateAttrToPrefixMapImpl(
    const std::pair<folly::CIDRNetwork, uint32_t>& prefixPathId,
    const std::shared_ptr<const BgpPath>& oldPath,
    const std::shared_ptr<const BgpPath>& newPath,
    AttrToPrefixMap& attrToPrefixMap,
    const std::string& contextName,
    AdjRibStats& stats,
    bool isNexthopSetByPolicy = false);

/**
 * @brief Replace zeros in AS-PATH with the specified value
 *
 * @details Replaces all zero ASNs in the AS-PATH with the given replacement
 * value. This is used to replace ASNs that were overwritten by policy (ASN 0).
 * Inbound: replaces 0 with the neighbor's AS number
 * Outbound: replaces 0 with the locally configured AS number
 *
 * @param attrs - Attributes to modify (must not be published)
 * @param replaceValue - Value to replace zeros with (typically local or remote
 * AS)
 */
void replaceZerosInAsPath(
    std::shared_ptr<BgpPath> attrs,
    uint32_t replaceValue) noexcept;

/**
 * @brief Update AS-PATH attributes for outbound announcements
 *
 * @details Performs AS-PATH transformations:
 * - Replaces zeros in AS-PATH with local AS
 * - Removes confederation segments for EBGP
 * - Removes private ASNs if configured
 * - Prepends local AS
 *
 * @param peeringParams - Peer configuration parameters
 * @param attrsToUpdate - Attributes to modify (must not be published)
 */
void updateAsPathAttributesCommon(
    const PeeringParams& peeringParams,
    std::shared_ptr<BgpPath> attrsToUpdate) noexcept;

/**
 * @brief Remove confed as_path segments from the given as_path vector
 *
 * @param asPath - The full as_path represented in a vector of segments
 */
void removeConfedAsPathSegments(
    std::vector<nettools::bgplib::BgpAttrAsPathSegmentC>& asPath);

/**
 * @brief Remove private asn from the given as_path vector
 *
 * @param asPath - The full as_path represented in a vector of segments
 */
void removePrivateAsns(
    std::vector<nettools::bgplib::BgpAttrAsPathSegmentC>& asPath);

/**
 * @brief Prepend local AS to AS-PATH
 *
 * @param asPath - The AS-PATH to modify
 * @param localAs - Local AS number to prepend
 * @param isConfedPeer - Whether this is a confed peer
 */
void prependAsPath(
    std::vector<nettools::bgplib::BgpAttrAsPathSegmentC>& asPath,
    uint32_t localAs,
    bool isConfedPeer);

/**
 * @brief Validate enforce-first-as for eBGP peers
 *
 * @details Verifies that the first AS in the AS-PATH matches the peer's
 * remote AS number. This validation is only applied for eBGP and confed
 * eBGP peers, not for iBGP peers.
 *
 * @param attrs - shared_ptr of BgpPath including as_path
 * @param params - BGP peering parameters with remote AS
 * @param isIBgpPeer - boolean flag for iBGP
 * @return True if validation passes
 */
bool validateEnforceFirstAs(
    const std::shared_ptr<const BgpPath>& attrs,
    const PeeringParams& params,
    bool isIBgpPeer);

/**
 * @brief Update LOCAL_PREF attribute for outbound announcements
 *
 * @details Removes LOCAL_PREF for EBGP peers (RFC 4271)
 *
 * @param peeringParams - Peer configuration parameters
 * @param attrsToUpdate - Attributes to modify (must not be published)
 */
void updateLocalPrefCommon(
    const PeeringParams& peeringParams,
    std::shared_ptr<BgpPath> attrsToUpdate) noexcept;

/**
 * @brief Update MED attribute for outbound announcements
 *
 * @details Handles MED based on:
 * - Whether MED was set by policy (preserves policy-set value)
 * - Whether to send MED to EBGP (feature flag)
 * - Whether to send MED to IBGP (always sent)
 *
 * @param peeringParams - Peer configuration parameters
 * @param attrsToUpdate - Attributes to modify (must not be published)
 * @param postPolicyInfo - Metadata from policy evaluation
 */
void updateMedCommon(
    const PeeringParams& peeringParams,
    std::shared_ptr<BgpPath> attrsToUpdate,
    const PostPolicyInfo& postPolicyInfo) noexcept;

/**
 * @brief Update ORIGINATOR_ID and CLUSTER_LIST attributes
 *
 * @details For route reflection:
 * - Sets ORIGINATOR_ID to the route's originating peer
 * - Adds local router ID to CLUSTER_LIST
 *
 * @param peeringParams - Peer configuration parameters
 * @param update - The RibOutAnnouncementEntry being processed
 * @param attrsToUpdate - Attributes to modify (must not be published)
 */
void updateOriginAndClusterListCommon(
    const PeeringParams& peeringParams,
    const RibOutAnnouncementEntry& update,
    std::shared_ptr<BgpPath> attrsToUpdate) noexcept;

/**
 * @brief Attach the partial-drain community on an outbound attribute clone.
 *
 * @details Adds kDrainCommunity (65446:10) and removes kLiveCommunity
 * (65446:30) on the cloned per-peer pre-policy attributes. Used by both
 * AdjRibOut (per-peer) and AdjRibGroup (group RIB-OUT) when announcing a
 * route flagged isPartialDrain so peers deprioritize the path gracefully
 * instead of withdrawing it.
 *
 * Placement rationale: this runs on the per-peer (or per-group) clone of
 * `update.attrs` after `update.attrs->clone()` and before the egress policy
 * step. The published RIB-side BgpPath is immutable; mutating it at the RIB
 * level would force a fresh BgpPath clone per drained prefix, defeating the
 * 4-layer dedup chain (sub-attrs -> attrs -> paths -> strings) and incurring
 * significant memory overhead at scale. Riding the existing per-peer attrs
 * clone in AdjRib piggybacks on a clone that already happens.
 *
 * @param attrsToUpdate - Attributes to modify (must not be published)
 */
void applyPartialDrainCommunities(
    const std::shared_ptr<BgpPath>& attrsToUpdate) noexcept;

/**
 * @brief Keep the lowest non-negative transitive and non-transitive LBWs only.
 *
 * @details Removes all but the lowest transitive and non-transitive non-
 * negative LBWs from the vector in place. This means the method also
 * removes negative LBWs.
 *
 * @param communities Vector of BgpAttrExtCommunityC.
 */
void pruneLbwExtCommunitiesCommon(
    nettools::bgplib::BgpAttrExtCommunitiesC& communities);

/*
 * @brief Update extended communities for outbound announcements
 *
 * @details Removes non-transitive extended communities when advertising to
 * EBGP peers, as per RFC 4360. Non-transitive extended communities should
 * not be propagated across AS boundaries.
 *
 * If UCMP/GAR is enabled, allow non-transitive LBW to be persisted.
 *
 * @param peeringParams - Peer configuration parameters
 * @param mask          - Used to infer UCMP/GAR policy
 * @param attrsToUpdate - Attributes to modify (must not be published)
 */
void updateExtCommunitiesCommon(
    const PeeringParams& peeringParams,
    const PolicyAttributesMask* mask,
    std::shared_ptr<BgpPath> attrsToUpdate) noexcept;

/**
 * @brief Override pre-policy attributes with policy-modified values
 *
 * @details Uses policy attribute mask to selectively copy attributes
 * from policy-cached attrs to the attrs being updated. This ensures
 * policy-modified attributes take precedence.
 *
 * @param mask - Policy attributes mask (which attrs were checked by policy)
 * @param policyCachedAttrs - Attributes from policy cache
 * @param attrsToOverride - Attributes to modify (must not be published)
 */
void overridePrePolicyAttributesCommon(
    const PolicyAttributesMask* mask,
    const std::shared_ptr<const BgpPath>& policyCachedAttrs,
    std::shared_ptr<BgpPath> attrsToOverride) noexcept;

/**
 * @brief Update all attributes except nexthop before putting in packing list.
 *
 * @details This is the main entry point for attribute transformations.
 * Performs the following steps:
 * 1. Override attributes based on policy mask (if policy configured)
 * 2. Update AS-PATH (remove confed, remove private ASNs, prepend local AS)
 * 3. Update LOCAL_PREF (strip for EBGP)
 * 4. Update MED (based on policy and feature flags)
 * 5. Update ORIGINATOR_ID and CLUSTER_LIST (for route reflection)
 *
 * Nexthop is handled separately at send time (per-peer basis).
 *
 * @param config - Peer configuration
 * @param update - The RibOutAnnouncementEntry being processed
 * @param policyResultAttrs - Attributes after policy evaluation
 * @param attrsToUpdate - Attributes to modify (must not be published)
 * @param postPolicyInfo - Metadata from policy evaluation
 */
void updateAttributesOutWithoutNexthopCommon(
    const PeerConfig& config,
    const RibOutAnnouncementEntry& update,
    const std::shared_ptr<const BgpPath>& policyResultAttrs,
    std::shared_ptr<BgpPath> attrsToUpdate,
    const PostPolicyInfo& postPolicyInfo) noexcept;

/**
 * @brief Pack prefixes with approximate size limit based on address family,
 * addPath capability to be serialized within kMaxSerializedChainLen precision.
 *
 * @details Packs prefixes up to estimated prefix limit.
 * ERASES packed prefixes from set, leaving remaining for next iteration.
 * This enables incremental draining under backpressure.
 *
 * Common implementation shared by both AdjRib::packPrefixesWithLimit()
 * and AdjRibOutGroup::packGroupPrefixes().
 *
 * @param approximateSerializedAttrLen - Estimated attribute size in bytes
 * @param prefixPathIds - Set of (prefix, pathId) pairs (modified in place)
 * @param bgpUpdatePrefixes - Container to pack prefixes into
 * @param sendAddPath - Whether to include path IDs in packed prefixes
 * @param context - Context string for logging (e.g., peer name or group name)
 * @return Number of prefixes packed
 */
uint32_t packPrefixesWithLimitCommon(
    const uint32_t approximateSerializedAttrLen,
    PrefixSet& prefixPathIds,
    std::vector<nettools::bgplib::RiggedIPPrefix>& bgpUpdatePrefixes,
    bool sendAddPath,
    const std::string& context = "") noexcept;

/**
 * @brief Pack all prefixes into BgpUpdate2 thrift collection
 *
 * Common implementation shared by both AdjRib::packPrefixes()
 * and AdjRibOutGroup::packGroupPrefixes().
 *
 * @param prefixPathIds - Set of (prefix, pathId) pairs (modified in place)
 * @param bgpUpdatePrefixes - Container to pack prefixes into
 * @param sendAddPath - Whether to include path IDs in packed prefixes
 * @param context - Context string for logging (e.g., peer name or group name)
 * @return Number of prefixes packed
 */
uint32_t packPrefixesCommon(
    PrefixSet& prefixPathIds,
    std::vector<nettools::bgplib::RiggedIPPrefix>& bgpUpdatePrefixes,
    bool sendAddPath,
    const std::string& context = "") noexcept;

} // namespace facebook::bgp
