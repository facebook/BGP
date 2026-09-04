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

#include <string_view>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/policy/base/PolicyStructsBase.h"

namespace facebook::bgp {

// LinkBandwidth Action Data, details: https://fb.quip.com/xqf4Ai6ySsDm
struct LbwActionData {
  /*
   * original asn and lbw (prior to peer-config and policy)
   * none if not set
   */
  const std::optional<std::pair<uint16_t, float>> originalAsnLbw{std::nullopt};

  // asn from peer config
  const uint32_t asn{0};
  // link-bw from peer config
  const std::optional<float> linkBandwidthBps{std::nullopt};
  // aggregated received link-bw from all peers, none if missing any peer
  const std::optional<float> aggregateReceivedUcmpWeight{std::nullopt};
  // aggregated link-bw from all peers' config, none if missing any peer
  const std::optional<float> aggregateLocalUcmpWeight{std::nullopt};
  // rib policy set link-bandwidth, none if no rib policy specified
  const std::optional<float> ribPolicyUcmpWeight{std::nullopt};

  LbwActionData(
      const std::optional<std::pair<uint16_t, float>>& originalAsnLbw,
      const uint32_t asn,
      const std::optional<float>& linkBandwidthBps,
      const std::optional<float>& aggregateReceivedUcmpWeight = std::nullopt,
      const std::optional<float>& aggregateLocalUcmpWeight = std::nullopt,
      const std::optional<float>& ribPolicyUcmpWeight = std::nullopt)
      : originalAsnLbw(originalAsnLbw),
        asn(asn),
        linkBandwidthBps(linkBandwidthBps),
        aggregateReceivedUcmpWeight(aggregateReceivedUcmpWeight),
        aggregateLocalUcmpWeight(aggregateLocalUcmpWeight),
        ribPolicyUcmpWeight(ribPolicyUcmpWeight) {}

  bool operator==(const LbwActionData& other) const {
    return (
        originalAsnLbw == other.originalAsnLbw && asn == other.asn &&
        linkBandwidthBps == other.linkBandwidthBps &&
        aggregateReceivedUcmpWeight == other.aggregateReceivedUcmpWeight &&
        aggregateLocalUcmpWeight == other.aggregateLocalUcmpWeight &&
        ribPolicyUcmpWeight == other.ribPolicyUcmpWeight);
  }

  bool operator!=(const LbwActionData& other) const {
    return !(*this == other);
  }

  size_t hash() const {
    size_t res =
        std::hash<std::optional<std::pair<uint16_t, float>>>{}(originalAsnLbw);
    res += std::hash<uint32_t>{}(asn);
    res += std::hash<std::optional<float>>{}(linkBandwidthBps);
    res += std::hash<std::optional<float>>{}(aggregateReceivedUcmpWeight);
    res += std::hash<std::optional<float>>{}(aggregateLocalUcmpWeight);
    res += std::hash<std::optional<float>>{}(ribPolicyUcmpWeight);
    return res;
  }
};

/*
 * BgpPolicyActionData - capture any data that's needed to apply a action
 * but can NOT be pre-configured in Policy Configuration
 * (e.g some data needs to be dynamically derived on the fly).
 */
struct BgpPolicyActionData {
  // Constructor.
  explicit BgpPolicyActionData(
      const std::optional<size_t> switchId,
      const std::optional<size_t> multiPathSize,
      const std::optional<LbwActionData>& lbwActionData)
      : switchId(switchId),
        multiPathSize(multiPathSize),
        lbwActionData(lbwActionData) {}

  BgpPolicyActionData() = default;

  bool operator==(const BgpPolicyActionData& other) const {
    return switchId == other.switchId && multiPathSize == other.multiPathSize &&
        lbwActionData == other.lbwActionData;
  }

  bool operator!=(const BgpPolicyActionData& other) const {
    return !(*this == other);
  }

  size_t hash() const {
    size_t res = 0;
    if (switchId.has_value()) {
      res = folly::hash::hash_combine(res, *switchId);
    }
    if (multiPathSize.has_value()) {
      res = folly::hash::hash_combine(res, *multiPathSize);
    }
    if (lbwActionData.has_value()) {
      res = folly::hash::hash_combine(res, lbwActionData->hash());
    }
    return res;
  }

  /*
   * READ-ONLY Action data.
   * The fields specify values that are not to be modified during
   * policy evaluation. Policy evaluation also depends on these fields.
   */
  const std::optional<size_t> switchId{0};
  const std::optional<size_t> multiPathSize{std::nullopt};
  const std::optional<LbwActionData> lbwActionData{std::nullopt};

  /*
   * Transient action data.
   * The fields below can be modified during policy evaluation by
   * the policy manager and should NOT be considered immutable.
   * If BgpPolicyActionData is required to be immutable for a workflow,
   * then it is important to exclude these fields from the hash/eq operators
   * of BgpPolicyActionData.
   *
   * Because PolicyActionData is passed through every
   * term during PolicyManager::applyPolicy call, we use this
   * as a vehicle for understanding the behavior of genuinely
   * executed PolicyTerms (PolicyMatches + PolicyActions).
   */
  // Flag to indicate that MED value was set by PolicyAction on BgpPath.
  bool isMedSetByPolicy = false;
  /*
   * Flag to indicate that LBW ext community was zero/missing after
   * DECODE/ENCODE action — route should be rejected.
   */
  bool isLbwRejected = false;
  // Flag to indicate that nexthop was set by PolicyAction on BgpPath.
  bool isNexthopSetByPolicy = false;
};

/*
 * Deny reason suffix appended when a route is rejected due to invalid
 * (zero/missing) GAR weights.
 */
inline constexpr std::string_view kInvalidGarWeightsDenyReason =
    "invalid GAR weights";

/*
 * To match base class template, BgpPolicy does not need any additional
 * matchData currently
 */
struct BgpPolicyMatchData {};

/*
 * Input message to policy
 * Input attributes (BgpPath) is non-const as we have to update Attributes
 * based on actions. User can input published/unpublished BgpPath.
 * Depending on if actions need to modify or not, policy can reuse input
 * BgpPath or clone BgpPath and modify accordingly.
 */
using PolicyInMessage = routing::PolicyInMessageBase<
    BgpPath,
    std::shared_ptr<BgpPolicyActionData>,
    BgpPolicyMatchData>;

/*
 * Output message from policy
 * Only accepted prefixes are returned with modified attributes.
 * Many prefixes can share same attributes.
 * Attributes could be mix of published and unpublished
 * receiver can publish, clone etc depending on local pref and
 * other requirements.
 */
using PolicyOutMessage = routing::PolicyOutMessageBase<BgpPath>;
} // namespace facebook::bgp
