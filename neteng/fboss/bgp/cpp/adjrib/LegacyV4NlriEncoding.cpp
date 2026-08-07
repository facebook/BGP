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

#include "neteng/fboss/bgp/cpp/adjrib/LegacyV4NlriEncoding.h"

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStructs.h"

namespace facebook::bgp {

V4EncodingDecision v4EncodingDecision(
    const UpdateGroupKey& groupKey,
    bool isV4,
    const folly::IPAddress& newNexthop,
    folly::StringPiece context) noexcept {
  if (!isV4 || !groupKey.legacyV4NlriEncoding ||
      groupKey.extNhEncodingCapable) {
    return V4EncodingDecision::MpReach;
  }
  if (newNexthop.isV6()) {
    /*
     * A v6 nexthop cannot fit the 4-byte NEXT_HOP attribute, and a
     * capability-less peer cannot parse MP_REACH, so the route cannot be
     * encoded for this peer at all. Unreachable for a peer that has not
     * negotiated RFC 5549, but guard against a mis-authored egress policy
     * forcing a v6 nexthop: drop the route rather than emit an UPDATE the peer
     * would reject.
     */
    XLOGF_EVERY_MS(
        ERR,
        5000,
        "{}: IPv4 route has a v6 nexthop {} for a capability-less peer; dropping (cannot encode as classic NLRI, and the peer cannot parse MP_REACH)",
        context,
        newNexthop.str());
    return V4EncodingDecision::Drop;
  }
  return V4EncodingDecision::ClassicNlri;
}

} // namespace facebook::bgp
