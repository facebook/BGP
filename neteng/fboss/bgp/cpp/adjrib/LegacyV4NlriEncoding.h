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

#include <folly/IPAddress.h>
#include <folly/Range.h>

namespace facebook::bgp {

struct UpdateGroupKey;

/*
 * How an IPv4-unicast announcement keyed by `groupKey` is encoded on the wire.
 *   - MpReach:     MP_REACH_NLRI (attr 14) -- the default for MP-capable peers.
 *   - ClassicNlri: RFC 4271 classic NLRI + NEXT_HOP (attr 3) for a
 *                  capability-less peer.
 *   - Drop:        a capability-less peer with a v6 nexthop -- the route cannot
 *                  be encoded (a v6 nexthop does not fit the 4-byte NEXT_HOP
 *                  attribute, and the peer cannot parse MP_REACH), so it is
 *                  dropped rather than emitted as an UPDATE the peer would
 *                  reject.
 */
enum class V4EncodingDecision {
  MpReach,
  ClassicNlri,
  Drop,
};

/*
 * Decide the v4 encoding for `groupKey`. Shared by the single-peer (AdjRib) and
 * group (AdjRibOutGroup) egress paths so the decision cannot drift between
 * them. On a v6 nexthop for a capability-less peer it logs (rate-limited, using
 * `context`) and returns Drop. Each caller passes the nexthop for its path: the
 * single-peer path passes the per-peer transformed nexthop
 * (getNewNexthopFromAttributesOut, i.e. after policy / nexthop-self); the group
 * path passes the raw path nexthop, because a group applies its per-peer
 * nexthop later at serialization. For a v4 route not using RFC 5549 (excluded
 * by the extNhEncodingCapable check) both are v4, so the two sites agree.
 */
V4EncodingDecision v4EncodingDecision(
    const UpdateGroupKey& groupKey,
    bool isV4,
    const folly::IPAddress& newNexthop,
    folly::StringPiece context) noexcept;

} // namespace facebook::bgp
