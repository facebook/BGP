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

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibGroupSerializer.h"

#include <folly/logging/xlog.h>
#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/lib/BgpMessageSerializer.h"

namespace facebook::bgp {

nettools::bgplib::UpdateDescriptor
AdjRibGroupSerializer::serializeUpdateAndCreateDescriptor(
    const nettools::bgplib::BgpUpdate2& message,
    bool as4byte,
    bool extNhEncoding) noexcept {
  nettools::bgplib::UpdateDescriptor descriptor;

  try {
    /* Extract nexthop values from message */
    /* v4 nexthop comes from v4Nexthop field or mpAnnounced */
    folly::IPAddress v4Nexthop;
    folly::IPAddress v6Nexthop;

    if (!message.mpAnnounced()->prefixes()->empty()) {
      const auto& mpNexthop =
          network::toIPAddress(*message.mpAnnounced()->nexthop());
      if (mpNexthop.isV6()) {
        v6Nexthop = mpNexthop;
      } else if (mpNexthop.isV4()) {
        /* Use v4 from mpAnnounced if v4Nexthop wasn't set */
        v4Nexthop = mpNexthop;
      }
    }

    /* Serialize the BGP UPDATE with nexthop tracking */
    std::vector<std::tuple<size_t, size_t, bool>> nexthopOffsets;

    auto serializedPdu =
        nettools::bgplib::BgpMessageSerializer::serializeBgpUpdate2(
            message, as4byte, extNhEncoding, &nexthopOffsets);

    /* Store as shared_ptr for zero-copy distribution */
    descriptor.serializedGroupPDU =
        std::shared_ptr<const folly::IOBuf>(std::move(serializedPdu));

    /* Store nexthop information */
    descriptor.v4Nexthop = v4Nexthop;
    descriptor.v6Nexthop = v6Nexthop;
    descriptor.nexthopOffsets = std::move(nexthopOffsets);

  } catch (const std::exception& e) {
    XLOGF(ERR, "Failed to serialize BGP UPDATE message: {}", e.what());
    // Return empty descriptor on error
  }

  return descriptor;
}

} // namespace facebook::bgp
