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

#include "RouteInfo.h"

#include <folly/container/Enumerate.h>
#include <folly/gen/Base.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/Utils.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfoBase.h"

namespace facebook::bgp {

RouteInfo::RouteInfo(
    const folly::CIDRNetwork& prefixIn, // v4 or v6 prefix
    const TinyPeerInfo& peerIn,
    std::shared_ptr<const BgpPath> attrsIn,
    uint32_t receivedPathId,
    RibEntry& ribEntry,
    std::optional<uint32_t> pathIdToSend,
    bool installToFib)
    : prefix(prefixIn),
      peer(peerIn),
      attrs(std::move(attrsIn)),
      receivedPathId(receivedPathId),
      pathIdToSend(pathIdToSend),
      installToFib(installToFib),
      ribEntry(ribEntry) {
  /*
   * RouteInfo is used in Rib, by the time Rib receive a prefix
   * local preference should have a value or is default to 100
   */
  CHECK(attrs->isPublished()) << "Attributes is not published.";
  CHECK(attrs->getLocalPref()) << "Local Preference should have a value.";

  // set EBGP/Local route flag for announcements accordingly
  if (peer.addr == kLocalRoutePeerAddr) {
    setRouteLocal();
  } else if (peer.sessionType == BgpSessionType::EBGP) {
    // RFC5065 section 5.3 item 4: all confed peer should be treated as internal
    setRouteExternal();
  } else if (peer.sessionType == BgpSessionType::ConfedEBGP) {
    setRouteConfedExternal();
  }
  // If LinkBW Extended community is present, extract the normalized value.
  auto lbw = attrs->getNonTransitiveLbwValue();
  if (lbw.has_value()) {
    if (lbw.value() >= 0) {
      // UCMP weight of a route refers to link-bandwidth-value
      ucmpWeight_ = lbw.value();
    } else {
      XLOGF(ERR, "Invalid LBW:{}", lbw.value());
    }
  }
  lastModifiedTime_ = getCurrentTimeMicroSec();
}

float RouteInfo::getUcmpWeight() const {
  if (ucmpWeight_.has_value()) {
    return ucmpWeight_.value();
  } else {
    return 0.0f;
  }
}

folly::CIDRNetwork RouteInfo::getPrefix() const {
  return prefix;
}

uint8_t RouteInfo::getBgpPrefixLength() const {
  return prefix.second;
}

int64_t RouteInfo::getBgpLocalPreference() const {
  return *(attrs->getLocalPref());
}

int64_t RouteInfo::getBgpAsPathLen() const {
  int pathLen = 0;
  for (const auto& segment : attrs->getAsPath().get()) {
    if (segment.asSet.size()) {
      ++pathLen;
    } else if (segment.asSequence.size()) {
      pathLen += segment.asSequence.size();
    }
    // ignore Confed AS segments as per RFC 5065 sec 5.3
  }
  return pathLen;
}

int64_t RouteInfo::getBgpAsPathLenWithConfed() const {
  return attrs->getBgpAsPathLenWithConfed();
}

std::pair<uint32_t /* origin asn */, uint32_t /* peer asn */>
RouteInfo::getOriginAsnAndPeerAsn() const {
  using folly::gen::appendTo;
  using folly::gen::from;

  /*
   * flatten as path. Note that this does not return the right as path(s) in
   * case asSet is used. But, the first and last of asPath willl contain one
   * right origin asn and one right peer asn.
   */
  std::vector<uint32_t> asPath;
  for (auto const& asSeg : attrs->getAsPath().get()) {
    (from(asSeg.asSet) + from(asSeg.asSequence)) | appendTo(asPath);
    // ignore Confed AS segments as per RFC 5065 sec 5.3
  }

  uint32_t originAsn{0}, peerAsn{0};
  if (asPath.size()) {
    originAsn = asPath.back();
    peerAsn = asPath.front();
  }

  return std::make_pair(originAsn, peerAsn);
}

/*
 * Notice this returns the combination of originator ID, if originator id is
 * present, and router id for bestpath calculation
 * We need both the originator ID (or peerIp) and peer router ID is due to
 * multiple sessions could be injected by VIP injector, that caused issue that
 * when the prefix was injected by multiple sessions on the same peer the best
 * route selector cannot break tie just based on peer IP. Therefore, we need to
 * add the router ID to break tie in route selection.
 */
uint64_t RouteInfo::getBgpRouterId() const {
  // cast it to 64-bit to prepare for the shifting
  uint64_t originatorId =
      attrs->getOriginatorId() ? attrs->getOriginatorId() : peer.routerId;
  return (originatorId << 32) + peer.routerId;
}

__uint128_t RouteInfo::transformIP2Int(const folly::IPAddress& addr) const {
  __uint128_t ipBytes = 0;

  /*
   * capture the address bytes in an iterable structure
   *
   * bytes are captured in network byte ordering to ensure that we can run
   * the controller on ARM processors (obviously, this is happening real soon)
   */
  const auto& bytes =
      addr.isV4() ? addr.asV4().toBinary() : addr.asV6().toBinary();
  const auto& numBytes = bytes.size();

  for (const auto it : folly::enumerate(bytes)) {
    const auto& byteIdx = it.index;
    const auto& byteValue = *it;
    const auto& intByteNum = numBytes - byteIdx;
    const auto& shift = ((intByteNum - 1) * 8);
    ipBytes |= ((__uint128_t)byteValue) << shift;
  }

  return ipBytes;
}

__uint128_t RouteInfo::getBgpPeerIPAsInt() const {
  return transformIP2Int(peer.addr);
}

__uint128_t RouteInfo::getBgpNexthopAsInt() const {
  return transformIP2Int(attrs->getNexthop());
}

int64_t RouteInfo::getBgpOriginCode() const {
  return static_cast<int64_t>(attrs->getOrigin());
}

/*
 * MED value is reset in inbound BGP policy at PR/BRs and "always-compare-med"
 * is configured on them. Thus, we will compare route MEDs even if the routes
 * are being propagated by different peer AS.
 */
int64_t RouteInfo::getBgpMedValue() const {
  return attrs->getMed();
}

uint16_t RouteInfo::getBgpWeightValue() const {
  return attrs->getWeight();
}

int64_t RouteInfo::getBgpClusterListLen() const {
  return attrs->getClusterList().nullOrEmpty()
      ? 0
      : attrs->getClusterList()->size();
}

uint32_t RouteInfo::getIgpCostValue() const {
  std::optional<uint32_t> cost{std::nullopt};
  // Defensive null check to prevent crashes from dangling pointer
  if (nexthopInfo_ != nullptr) {
    cost = nexthopInfo_->getIgpCost();
  }
  return cost.has_value() ? cost.value() : std::numeric_limits<uint32_t>::max();
}

std::vector<uint32_t> RouteInfo::getBgpClusterList() const {
  return attrs->getClusterList().get();
}

bool RouteInfo::getIsRouteExternal() const {
  return (status & RouteStateFlags::EXTERNAL) == RouteStateFlags::EXTERNAL;
}

void RouteInfo::setRouteExternal() {
  status = status | RouteStateFlags::EXTERNAL;
}

bool RouteInfo::getIsRouteConfedExternal() const {
  return (status & RouteStateFlags::CONFED_EXTERNAL) ==
      RouteStateFlags::CONFED_EXTERNAL;
}

void RouteInfo::setRouteConfedExternal() {
  status = status | RouteStateFlags::CONFED_EXTERNAL;
}

bool RouteInfo::getIsRoutePreferred() const {
  return (status & RouteStateFlags::PREFERRED) == RouteStateFlags::PREFERRED;
}

void RouteInfo::setRoutePreferred() {
  status = status | RouteStateFlags::PREFERRED;
}

void RouteInfo::clearRoutePreferred() {
  status = status & ~RouteStateFlags::PREFERRED;
}

bool RouteInfo::getIsRouteDeleted() const {
  return (status & RouteStateFlags::DELETED) == RouteStateFlags::DELETED;
}

void RouteInfo::setRouteDeleted() {
  status = status | RouteStateFlags::DELETED;
}

bool RouteInfo::getIsRouteLocal() const {
  return (status & RouteStateFlags::LOCAL) == RouteStateFlags::LOCAL;
}

void RouteInfo::setRouteLocal() {
  status = status | RouteStateFlags::LOCAL;
}

int64_t RouteInfo::getRouterLevelPreferenceFromControllerCommunities() const {
  /* not implemented as not needed */
  CHECK(false) << "RouteInfo's not implemented function is used";
}

int64_t RouteInfo::getMetroLevelPreferenceFromControllerCommunities() const {
  /* not implemented as not needed */
  CHECK(false) << "RouteInfo's not implemented function is used";
}

std::vector<uint32_t> RouteInfo::getBgpAsPath() const {
  /* not implemented as not needed */
  CHECK(false) << "RouteInfo::getBgpAsPath() is not implemented";
}

void RouteInfo::clearBestPathFilterCriteria() {
  bestPathFilterConfig.reset();
}

void RouteInfo::setBestPathFilterCriteria(
    const nettools::edge::RouteFilterConfig& filterConfig) {
  bestPathFilterConfig = filterConfig;
}

std::string RouteInfo::getBestPathFilterDescr() {
  if (bestPathFilterConfig) {
    return bestPathFilterConfig->str();
  }
  return "";
}

bool RouteInfo::isOnNextHopList() const {
  return nextHopListHook_.is_linked();
}

void RouteInfo::setNexthopInfo(const NexthopInfoBase* nexthopInfo) {
  nexthopInfo_ = nexthopInfo;
}

bool RouteInfo::isNextHopReachable() const {
  // Local routes are always considered reachable
  if (getIsRouteLocal()) {
    return true;
  }
  return getIgpCostValue() != std::numeric_limits<uint32_t>::max();
}

bool RouteInfo::isResolvedForSelection() const {
  // Local routes are always considered resolved
  if (getIsRouteLocal()) {
    return true;
  }
  return nexthopInfo_ != nullptr && nexthopInfo_->isResolvedForSelection();
}

RibEntry& RouteInfo::getRibEntry() const {
  return ribEntry;
}

std::string RouteInfo::str() const {
  return fmt::format(
      "prefix: {} peerAddr: {} peerAsn: {} peerRouterId: {} attrs: {} "
      "status: {}",
      folly::IPAddress::networkToString(prefix),
      peer.addr.str(),
      peer.asn,
      peer.routerId,
      attrs->str(),
      static_cast<std::underlying_type<RouteStateFlags>::type>(status));
}

} // namespace facebook::bgp
