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

#include <folly/String.h>
#include <folly/gen/Base.h>
#include <folly/logging/xlog.h>
#include "magic_enum/magic_enum.hpp"

#include "neteng/fboss/bgp/cpp/common/BgpError.h"
#include "neteng/fboss/bgp/cpp/common/Utils.h"
#include "neteng/fboss/bgp/cpp/config/ConfigUtils.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyAction.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyUtils.h"
#include "thrift/lib/cpp/util/EnumUtils.h"

namespace facebook::bgp {

folly::IPAddress SetNexthop::validateAndGetNexthop(
    const bgp_policy::BgpPolicyAction& policyAction) {
  if (!policyAction.set_nexthop()) {
    throw BgpError("Missing set_nexthop");
  }
  const auto& tSetNextHop = *policyAction.set_nexthop();

  if (*tSetNextHop.set_self()) {
    throw BgpError("Unsupported nexthop config. set_self");
  }

  if (!tSetNextHop.next_hop()) {
    throw BgpError("Malformed nexthop config. next_hop missing");
  }
  const auto& tNexthop = *tSetNextHop.next_hop();

  if (tNexthop.next_hop_interface()) {
    throw BgpError("Unsupported nexthop config. next_hop_interface");
  }

  if (!tNexthop.next_hop_prefix()) {
    throw BgpError("Malformed nexthop config. next_hop_prefix missing");
  }
  const auto& nexthopStr = *tNexthop.next_hop_prefix();

  try {
    return folly::IPAddress(nexthopStr);
  } catch (folly::IPAddressFormatException&) {
    XLOGF(ERR, "Malformed nexthop config: {}", nexthopStr);
    throw BgpError("Malformed nexthop config: ", nexthopStr);
  }
}

void SetNexthop::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>> data) const noexcept {
  if (data.has_value() && *data) {
    (*data)->isNexthopSetByPolicy = true;
  }

  attr->setNexthop(nexthop_);
}

folly::IPAddress AddBackupAddr::validateAndGetBackupAddr(
    const bgp_policy::BgpPolicyAction& policyAction) {
  if (!policyAction.add_backup_addr()) {
    throw BgpError("Missing add_backup_addr");
  }

  const auto& addBackupAddr = *policyAction.add_backup_addr();
  if (!addBackupAddr.address().is_set()) {
    throw BgpError("Malformed add_backup_addr config. address missing");
  }

  const auto& address = *addBackupAddr.address();
  try {
    auto backupAddr = folly::IPAddress(address);
    if (!backupAddr.isV6()) {
      throw BgpError("Backup address must be IPv6: ", address);
    }
    return backupAddr;
  } catch (const folly::IPAddressFormatException&) {
    throw BgpError("Malformed backup address: ", address);
  }
}

void AddBackupAddr::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  attr->setBackupAddr(backupAddr_);
}

void SetMed::ValidateMed() const {
  if (med_.update_pattern()) {
    throw BgpError("Unsupported MED config. update_pattern");
  }

  if (*med_.med_action_type() != bgp_policy::MedActionType::SET) {
    throw BgpError(
        "Unsupported MED config. med_action_type: ",
        apache::thrift::util::enumNameSafe(*med_.med_action_type()));
  }

  if ((*med_.med_value() < 0) || (*med_.med_value() > UINT32_MAX)) {
    throw BgpError("Malformed MED config: ", *med_.med_value());
  }
}

void SetMed::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>> data) const noexcept {
  CHECK(data.has_value());
  auto& policyActionData = *data;
  XCHECK(policyActionData != nullptr)
      << "Expected BgpPolicyActionData for SetMed";

  policyActionData->isMedSetByPolicy = true;

  attr->setMed(*med_.med_value());
}

void SetWeight::ValidateWeight() const {
  if (*weight_.weight_action_type() != bgp_policy::WeightActionType::SET) {
    throw BgpError(
        "Unsupported Weight config. weight_action_type: ",
        apache::thrift::util::enumNameSafe(*weight_.weight_action_type()));
  }

  if ((*weight_.weight_value() < 0) || (*weight_.weight_value() > UINT16_MAX)) {
    throw BgpError("Malformed Weight config: ", *weight_.weight_value());
  }
}

void SetWeight::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  attr->setWeight(*weight_.weight_value());
}

void SetAsPath::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  nettools::bgplib::BgpAttrAsPathC asPath;
  // If overwriteList_ is empty, clear the entire as path
  if (!overwriteList_.empty()) {
    // Convert list of asn numbers into vector of BgpAttrAsPathSegmentC
    nettools::bgplib::BgpAttrAsPathSegmentC asPathSeg;
    for (const auto& asn : overwriteList_) {
      asPathSeg.asSequence.emplace_back(asn);
    }
    asPath.emplace_back(asPathSeg);
  }
  attr->setAsPath(asPath);
}

void SetAsPath::validateSetAsPath() const {
  for (const auto& asn : overwriteList_) {
    if ((asn < 0) || (asn > UINT32_MAX)) {
      throw BgpError("Malformed SetAsPath config: asn = ", asn);
    }
  }
}

void CommunityAction::validateAndSetCommunities(
    const std::vector<std::string>& communitiesFromConfig) {
  std::vector<std::string> communityStrings;

  // Separate community strings, regExs from config
  for (const auto& comm : communitiesFromConfig) {
    if (parseCommunityStr(comm).hasValue()) {
      communityStrings.emplace_back(comm);
      continue;
    }
    // try parse regex expression
    try {
      const auto& commRegex = boost::regex(comm);
      communityRegexs_.emplace_back(commRegex);
    } catch (boost::regex_error&) {
      throw BgpError("Malformed community config: ", comm);
    }
  }

  // Allow regEx's only for action type remove
  if ((*communityAction_.action_type() !=
       bgp_policy::CommunityActionType::REMOVE) &&
      !communityRegexs_.empty()) {
    std::vector<std::string> regexStrs;
    regexStrs.reserve(communityRegexs_.size());
    for (const auto& r : communityRegexs_) {
      regexStrs.emplace_back(r.str());
    }
    throw BgpError(
        "Community action with regex allowed only for "
        "CommunityActionType::REMOVE. action_name: ",
        *communityAction_.name(),
        ", action_type: ",
        apache::thrift::util::enumNameSafe(*communityAction_.action_type()),
        ", regex patterns: ",
        folly::join(", ", regexStrs));
  }

  // Convert string communities to CommunityC for fast comparision,add/del/set
  auto communities = createBgpAttrCommunitiesC(communityStrings);
  // All invalid communities will be treated as regEx and evaluated for validity
  // above. So, there can never be a invalid community in communityStrings
  CHECK_EQ(communities.size(), communityStrings.size());

  communities_.insert(
      communities_.end(), communities.begin(), communities.end());
}

void CommunityAction::PopulateReferences(
    const folly::F14NodeMap<std::string, bgp_policy::CommunityList>&
        communityListMap) {
  if (!communityAction_.community_action_list_names() ||
      communityAction_.community_action_list_names()->empty()) {
    return;
  }
  for (const auto& commListName :
       *(communityAction_.community_action_list_names())) {
    auto communityListIter = communityListMap.find(commListName);
    if (communityListIter == communityListMap.end()) {
      // TODO: optimization: have a global validation
      throw BgpError(
          "Could not find CommunityList Action reference: ", commListName);
    }
    const auto& communityListFromRef = communityListIter->second;
    if (communityListFromRef.communities()) {
      validateAndSetCommunities(*communityListFromRef.communities());
    } else {
      throw BgpError("Missing communities in CommunityList: ", commListName);
    }
  }
}

void CommunityAction::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  switch (*communityAction_.action_type()) {
    case bgp_policy::CommunityActionType::ADD: {
      // Append all communities
      // Add community only if it doesn't already exist
      auto attrCommunities = attr->getCommunities().get();
      attr->setCommunities(addCommunities(attrCommunities, communities_));
    } break;

    case bgp_policy::CommunityActionType::SET:
      attr->setCommunities(communities_);
      break;

    case bgp_policy::CommunityActionType::REMOVE: {
      // Remove community if it exits
      // Apply both regExs and community strings
      auto attrCommunities = attr->getCommunities().get();
      if (!communityRegexs_.empty()) {
        attrCommunities =
            removeCommunitiesMatchingRegexs(attrCommunities, communityRegexs_);
      }
      if (!communities_.empty()) {
        attrCommunities = removeCommunities(attrCommunities, communities_);
      }
      attr->setCommunities(attrCommunities);
    } break;

    default:
      CHECK(false) << "Invalid community action type";
  }
}

// Routes with zero or missing GAR weights are rejected
// https://docs.google.com/document/d/15o2ixlk6ox7Qa33cJZBhJjOlz6Dx1SZ13rWc-zP8uBU/edit?tab=t.0#heading=h.2n9ls543r2ye
void LbwExtCommunityAction::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>> data) const noexcept {
  CHECK(data.has_value());
  auto& policyActionData = *data;
  CHECK(policyActionData != nullptr);
  /*
   * Injected/synthesized routes (offline simulator) can reach an LBW egress
   * action without lbwActionData populated; treat that as no-op rather than
   * aborting. Production always populates it, so this guard is a sim-only path.
   */
  if (!policyActionData->lbwActionData.has_value()) {
    XLOGF_EVERY_MS(
        WARN,
        60000,
        "LBW ext-community action missing lbwActionData; treating as no-op. action_type={}",
        apache::thrift::util::enumNameSafe(*lbwExtCommunityAction_.type()));
    return;
  }
  const auto& lbwActionData = *policyActionData->lbwActionData;

  switch (*lbwExtCommunityAction_.type()) {
    case bgp_policy::LbwExtCommunityActionType::DISABLE:
      // ignore received LBW
      attr->pruneNonTransitiveLbwExtCommunity();
      break;
    case bgp_policy::LbwExtCommunityActionType::ACCEPT:
      // keep original AsnLbw state
      if (lbwActionData.originalAsnLbw.has_value()) {
        attr->setNonTransitiveLbwExtCommunity(
            lbwActionData.originalAsnLbw->first,
            lbwActionData.originalAsnLbw->second);
      } else {
        attr->pruneNonTransitiveLbwExtCommunity();
      }
      break;
    case bgp_policy::LbwExtCommunityActionType::DECODE_ALL:
      // first recover the original lbw ext community
      if (lbwActionData.originalAsnLbw.has_value()) {
        attr->setNonTransitiveLbwExtCommunity(
            lbwActionData.originalAsnLbw->first,
            lbwActionData.originalAsnLbw->second);
        assert(lbwExtCommunityAction_.encoding_scheme());
        auto encodedLbw = attr->getNonTransitiveRawLbwValue();
        if (!encodedLbw || *encodedLbw == 0) {
          policyActionData->isLbwRejected = true;
        } else {
          attr->setTopologyInfo(decodeValues(
              *encodedLbw, *lbwExtCommunityAction_.encoding_scheme()));
        }
      } else {
        attr->pruneNonTransitiveLbwExtCommunity();
        policyActionData->isLbwRejected = true;
      }
      break;
    case bgp_policy::LbwExtCommunityActionType::SET_LINK_BPS:
      // override LBW ext community with link bw from config
      CHECK(lbwActionData.linkBandwidthBps.has_value());
      attr->setNonTransitiveLbwExtCommunity(
          lbwActionData.asn, lbwActionData.linkBandwidthBps.value());
      break;
    case bgp_policy::LbwExtCommunityActionType::BEST_PATH:
      // If any ECMP path is missing LBW then prune LBW community of the best
      // path, else keep its original state.
      if (!lbwActionData.aggregateReceivedUcmpWeight.has_value()) {
        attr->pruneNonTransitiveLbwExtCommunity();
      } else {
        if (lbwActionData.originalAsnLbw.has_value()) {
          attr->setNonTransitiveLbwExtCommunity(
              lbwActionData.originalAsnLbw->first,
              lbwActionData.originalAsnLbw->second);
        } else {
          attr->pruneNonTransitiveLbwExtCommunity();
        }
      }
      break;
    case bgp_policy::LbwExtCommunityActionType::AGGREGATE_RECEIVED:
      // If any ECMP path is missing LBW then prune LBW community of the best
      // path, else advertise the aggregated value of LBW community of ECMP
      // paths.
      if (!lbwActionData.aggregateReceivedUcmpWeight.has_value()) {
        attr->pruneNonTransitiveLbwExtCommunity();
      } else {
        attr->setNonTransitiveLbwExtCommunity(
            lbwActionData.asn,
            lbwActionData.aggregateReceivedUcmpWeight.value());
      }
      break;
    case bgp_policy::LbwExtCommunityActionType::AGGREGATE_LOCAL:
      // If any ECMP path is missing peer LBW then prune LBW community of the
      // best path, else advertise the aggregated value of LBW ECMP path-peers.
      if (!lbwActionData.aggregateLocalUcmpWeight.has_value()) {
        attr->pruneNonTransitiveLbwExtCommunity();
      } else {
        attr->setNonTransitiveLbwExtCommunity(
            lbwActionData.asn, lbwActionData.aggregateLocalUcmpWeight.value());
      }
      break;
    case bgp_policy::LbwExtCommunityActionType::
        ENCODE_AGGREGATE_RECEIVED_OVERWRITE:
      if (!lbwActionData.aggregateReceivedUcmpWeight.has_value() ||
          lbwActionData.aggregateReceivedUcmpWeight.value() == 0) {
        attr->pruneNonTransitiveLbwExtCommunity();
        policyActionData->isLbwRejected = true;
      } else {
        assert(lbwExtCommunityAction_.encoding_scheme());
        assert(lbwExtCommunityAction_.encoding_id());
        auto lengths =
            encodingSchemeToVector(*lbwExtCommunityAction_.encoding_scheme());
        auto encodingId = *lbwExtCommunityAction_.encoding_id();
        auto encodedLbw = encodeValue(
            0 /* base */,
            static_cast<size_t>(
                lbwActionData.aggregateReceivedUcmpWeight.value()),
            encodingId,
            lengths);
        attr->setNonTransitiveRawLbwExtCommunity(lbwActionData.asn, encodedLbw);
      }
      break;
    case bgp_policy::LbwExtCommunityActionType::ENCODE_SWITCH_ID: {
      assert(lbwExtCommunityAction_.encoding_scheme());
      assert(lbwExtCommunityAction_.encoding_id());
      auto lengths =
          encodingSchemeToVector(*lbwExtCommunityAction_.encoding_scheme());
      auto encodingId = *lbwExtCommunityAction_.encoding_id();
      auto curLbwValue = attr->getNonTransitiveRawLbwValue();
      if (!curLbwValue && lbwActionData.originalAsnLbw) {
        // if lbw ext community is reset, we need to restore it from policy
        // action data
        attr->setNonTransitiveLbwExtCommunity(
            lbwActionData.originalAsnLbw->first,
            lbwActionData.originalAsnLbw->second);
        // update curLbwValue
        curLbwValue = attr->getNonTransitiveRawLbwValue();
      }
      XCHECK(policyActionData->switchId)
          << "switchId unset for ENCODE_SWITCH_ID";
      auto encodedLbw = encodeValue(
          curLbwValue ? *curLbwValue : 0,
          *policyActionData->switchId,
          encodingId,
          lengths);
      if (encodedLbw == 0) {
        policyActionData->isLbwRejected = true;
      }
      attr->setNonTransitiveRawLbwExtCommunity(lbwActionData.asn, encodedLbw);
    } break;
    case bgp_policy::LbwExtCommunityActionType::ENCODE_MULTIPATH: {
      assert(lbwExtCommunityAction_.encoding_scheme());
      assert(lbwExtCommunityAction_.encoding_id());
      auto lengths =
          encodingSchemeToVector(*lbwExtCommunityAction_.encoding_scheme());
      auto encodingId = *lbwExtCommunityAction_.encoding_id();
      auto curLbwValue = attr->getNonTransitiveRawLbwValue();
      if (!curLbwValue && lbwActionData.originalAsnLbw) {
        // if lbw ext community is null, we need to restore it from policy
        // action data
        attr->setNonTransitiveLbwExtCommunity(
            lbwActionData.originalAsnLbw->first,
            lbwActionData.originalAsnLbw->second);
        // update curLbwValue
        curLbwValue = attr->getNonTransitiveRawLbwValue();
      }
      XCHECK(policyActionData->multiPathSize)
          << "multiPathSize unset for ENCODE_MULTIPATH";
      auto encodedLbw = encodeValue(
          curLbwValue ? *curLbwValue : 0,
          *policyActionData->multiPathSize,
          encodingId,
          lengths);
      if (encodedLbw == 0) {
        policyActionData->isLbwRejected = true;
      }
      attr->setNonTransitiveRawLbwExtCommunity(lbwActionData.asn, encodedLbw);
    } break;
    case bgp_policy::LbwExtCommunityActionType::
        DECODE_AGGREGATE_CAPACITY_OVERWRITE: {
      assert(lbwExtCommunityAction_.encoding_scheme());
      auto curLbwValue = attr->getNonTransitiveRawLbwValue();
      if (!curLbwValue && lbwActionData.originalAsnLbw) {
        // if lbw ext community is null, we need to restore it from policy
        // action data
        attr->setNonTransitiveLbwExtCommunity(
            lbwActionData.originalAsnLbw->first,
            lbwActionData.originalAsnLbw->second);
        curLbwValue = attr->getNonTransitiveRawLbwValue();
      }
      if (!curLbwValue || *curLbwValue == 0) {
        policyActionData->isLbwRejected = true;
      }
      // then decode the aggregated capacity
      auto aggCapacity = curLbwValue
          ? decodeAndAggregateCapacity(
                *curLbwValue, *lbwExtCommunityAction_.encoding_scheme())
          : 0.0f;
      // overwrite the lbw ext community with the aggregated capacity
      attr->setNonTransitiveLbwExtCommunity(lbwActionData.asn, aggCapacity);
    } break;
    default:
      XCHECK(false) << "Unsupported LbwExtCommunityActionType";
  }
}

void ExtCommunityAction::validateActionType() const {
  switch (actionType_) {
    case bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_ADD:
    case bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_SET:
    case bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_REMOVE:
      break;
    default:
      auto err = fmt::format(
          "Unexpected BgpAttrChangeActionType {} for ExtCommunityAction",
          magic_enum::enum_name(actionType_));
      XLOGF(ERR, "{}", err);
      throw BgpError(err);
  }
}

nettools::bgplib::BgpAttrExtCommunitiesC ExtCommunityAction::getExtCommunities(
    const std::vector<bgp_policy::ExtCommunity>& ext_communities,
    const BgpGlobalConfig* config) {
  nettools::bgplib::BgpAttrExtCommunitiesC result;
  if (ext_communities.empty()) {
    return result;
  }
  for (const auto& extc : ext_communities) {
    // Derive 8-octet form of ExtCommunity which is encoded
    // in BgpAttrExtCommunityC.
    result.emplace_back(getBgpAttrExtCommunityC(extc, config));
  }
  return result;
}

void ExtCommunityAction::applyAction(
    std::shared_ptr<BgpPath>& path,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  if (!path) {
    return;
  }
  switch (actionType_) {
    case bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_ADD: {
      // Add ext communities (no duplicates)
      auto& currentExtCommunities = path->getExtCommunities().get();
      path->setExtCommunities(
          addExtCommunities(currentExtCommunities, ext_communities_));
    } break;

    case bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_REMOVE: {
      // Remove ext communities
      auto& currentExtCommunities = path->getExtCommunities().get();
      path->setExtCommunities(
          removeExtCommunities(currentExtCommunities, ext_communities_));
    } break;

    case bgp_policy::BgpAttrChangeActionType::EXT_COMMUNITY_LIST_SET:
      // Set ext communities (clear and replace)
      path->setExtCommunities(ext_communities_);
      break;

    default:
      // This should never happen as validateActionType() checks this
      XLOGF(
          ERR,
          "Unexpected BgpAttrChangeActionType {} in applyAction",
          magic_enum::enum_name(actionType_));
      break;
  }
}

void SetOrigin::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  switch (origin_) {
    case bgp_policy::Origin::IGP:
      attr->setOrigin(nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP);
      break;
    case bgp_policy::Origin::EGP:
      attr->setOrigin(nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_EGP);
      break;
    case bgp_policy::Origin::INCOMPLETE:
      attr->setOrigin(nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_INCOMPLETE);
      break;
    default:
      CHECK(false) << "Invalid origin in action";
  }
}

void SetLocalPreference::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  if (localPref_.local_pref()) {
    attr->setLocalPref(*localPref_.local_pref());
  }
}

void SetLocalPreference::ValidateLocalPreference() const {
  if (!localPref_.local_pref()) {
    throw BgpError("Malformed SetLocalPreference config: empty local_pref");
  }
  if ((*localPref_.local_pref() < 0) ||
      (*localPref_.local_pref() > UINT32_MAX)) {
    throw BgpError(
        "Malformed SetLocalPreference config: ", *localPref_.local_pref());
  }
  if (localPref_.add_value()) {
    throw BgpError("Unsupported SetLocalPreference config: add_value");
  }
  if (localPref_.local_preference_list_names() &&
      !localPref_.local_preference_list_names()->empty()) {
    throw BgpError(
        "Unsupported SetLocalPreference config: local_preference_list_names");
  }
}

void SetAsPathPrependAction::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  nettools::bgplib::BgpAttrAsPathC newAsPath = attr->getAsPath().get();
  auto prependLength = *asPathPrepend_.repeat_times();
  auto prependAsn = *asPathPrepend_.asn();
  if (newAsPath.size() > 0) {
    auto& firstSegment = newAsPath[0];
    if (!firstSegment.asSequence.empty()) {
      auto firstSegmentOccupied = firstSegment.asSequence.size();
      auto firstSegmentToOccupy = std::min(
          prependLength,
          int(nettools::bgplib::kMaxAsPathSegmentSize - firstSegmentOccupied));
      firstSegment.asSequence.insert(
          firstSegment.asSequence.begin(), firstSegmentToOccupy, prependAsn);
      prependLength -= firstSegmentToOccupy;
    }
  }
  if (prependLength > 0) {
    nettools::bgplib::BgpAttrAsPathSegmentC newSegment;
    newSegment.asSequence.insert(
        newSegment.asSequence.begin(), prependLength, prependAsn);
    newAsPath.insert(newAsPath.begin(), newSegment);
  }
  attr->setAsPath(std::move(newAsPath));
}

void SetAsPathPrependAction::validateAsPathPrepend(
    const bgp_policy::BgpPolicyAction& policyAction) {
  if (!policyAction.set_as_path_prepend()) {
    throw BgpError("Missing set_as_path_prepend");
  }
  const auto& tAsPathPrepend = *policyAction.set_as_path_prepend();

  if ((*tAsPathPrepend.asn() < 0) || (*tAsPathPrepend.asn() > UINT32_MAX)) {
    throw BgpError(
        "Malformed SetAsPathPrepend config: set_as_path_prepend.asn = ",
        *tAsPathPrepend.asn());
  }
  if ((*tAsPathPrepend.repeat_times() <= 0) ||
      (*tAsPathPrepend.repeat_times() >
       nettools::bgplib::kMaxAsPathSegmentSize)) {
    // TODO Handle > 255 repeat_times.
    throw BgpError(
        "Malformed SetAsPathPrepend config: repeat_times = ",
        *tAsPathPrepend.repeat_times());
  }
}

void AsPathToAsSet::applyAction(
    std::shared_ptr<BgpPath>& attr,
    std::optional<std::shared_ptr<BgpPolicyActionData>>) const noexcept {
  nettools::bgplib::BgpAttrAsPathC outputAsPath;
  std::set<uint32_t> outputAsSet;
  for (const auto& seg : attr->getAsPath().get()) {
    // Only one of {asSet, asSequence, asConfedSequence, asConfedSet} will
    // have elements, the rest will be empty.
    if (!seg.asConfedSequence.empty() || !seg.asConfedSet.empty()) {
      if (!outputAsSet.empty()) {
        XLOGF(
            WARN,
            "AS_CONFED_SEQUENCE/AS_CONFED_SET found after AS_SEQUENCE/AS_SET in AS_PATH. "
            "AS_PATH_TO_AS_SET action cannot be applied.\n{}",
            attr->str());
        return;
      }
      outputAsPath.emplace_back(seg);
    } else {
      outputAsSet.insert(seg.asSet.begin(), seg.asSet.end());
      outputAsSet.insert(seg.asSequence.begin(), seg.asSequence.end());
    }
  }

  if (!outputAsSet.empty()) {
    outputAsPath.emplace_back(
        nettools::bgplib::BgpAttrAsPathSegmentC::fromAsSet(outputAsSet));
  }
  attr->setAsPath(std::move(outputAsPath));
}
} // namespace facebook::bgp
