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

#include "neteng/fboss/bgp/cpp/lib/BgpMessageSerializer.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberSocket.h"

namespace facebook {
namespace nettools {
namespace bgplib {
/*
 * Dispatcher for outgoing BGP messages
 */
struct BgpSerializer {
  const BgpCapabilities caps_;

  explicit BgpSerializer(const BgpCapabilities& caps) : caps_{caps} {}

  std::unique_ptr<folly::IOBuf> operator()(BgpOpenMsg const& msg) {
    XLOG(DBG2, "Serializing OpenMsg");
    return BgpMessageSerializer::serializeBgpOpenMsg(msg);
  }

  std::unique_ptr<folly::IOBuf> operator()(BgpKeepAlive const&) {
    XLOG(DBG2, "Serializing KeepAlive");
    return BgpMessageSerializer::serializeBgpKeepAlive();
  }

  std::unique_ptr<folly::IOBuf> operator()(
      std::shared_ptr<const BgpUpdate2> const& msg) {
    XLOG(DBG2, "Serializing BgpUpdate");
    // will throw if msg contains NLRI of non-negotiated AFI
    validateBgpUpdate2(*msg, caps_);
    return BgpMessageSerializer::serializeBgpUpdate2(
        *msg, *caps_.as4byte(), caps_.extNHEncodingCapabilities()->size() > 0);
  }

  std::unique_ptr<folly::IOBuf> operator()(BgpEndOfRib const& msg) {
    XLOG(DBG2, "Serializing EndOfRib");
    return BgpMessageSerializer::serializeBgpEndOfRib(*msg.afi(), *msg.safi());
  }

  std::unique_ptr<folly::IOBuf> operator()(BgpNotification const& msg) {
    XLOG(DBG2, "Serializing BgpNotification");
    return BgpMessageSerializer::serializeBgpNotification(msg);
  }

  std::unique_ptr<folly::IOBuf> operator()(BgpRouteRefresh const& msg) {
    XLOG(DBG2, "Serializing BgpRouteRefresh");
    return BgpMessageSerializer::serializeBgpRouteRefresh(msg);
  }

  std::unique_ptr<folly::IOBuf> operator()(UpdateDescriptor const& desc) {
    /*
     * THREADING MODEL AND BUFFER MUTATION SAFETY
     * ===========================================
     *
     * This function mutates nexthop fields in a cloned IOBuf that shares
     * its underlying data buffer with other clones. The safety model is:
     *
     * TODAY (Single I/O Thread):
     *   - One I/O thread processes all peer queues sequentially
     *   - Mutations happen in sequence: peer1, then peer2, then peer3, etc.
     *   - Safe because no concurrent writes to the shared buffer
     *   - Each clone() increments refcount but shares data (zero-copy)
     *
     * FUTURE (Multiple I/O Threads):
     *   - Multiple I/O threads, each processing a subset of peers
     *   - To maintain safety, the group will create ONE IOBuf clone per I/O
     *     thread (not per peer)
     *   - Example: 100 peers across 4 I/O threads:
     *       Thread 1: Processes peers 1-25  with clone #1 (sequential)
     *       Thread 2: Processes peers 26-50 with clone #2 (sequential)
     *       Thread 3: Processes peers 51-75 with clone #3 (sequential)
     *       Thread 4: Processes peers 76-100 with clone #4 (sequential)
     *   - Each thread mutates its own buffer, no data races between threads
     *   - Within each thread, processing remains sequential (safe)
     *
     * SAFETY INVARIANT:
     *   Each IOBuf clone must be processed by exactly ONE I/O thread.
     *   The distribution logic (AdjRibGroup::distributeMessageToInSyncPeers)
     *   must partition UpdateDescriptors by I/O thread affinity.
     *
     * TODO(future multi-threaded I/O):
     *   1. Add I/O thread ID field to UpdateDescriptor
     *   2. In distributeMessageToInSyncPeers(), create one clone per I/O thread
     *   3. Assign each peer's UpdateDescriptor the clone for its I/O thread
     *   4. Ensure thread affinity is maintained throughout queue processing
     */
    XLOG(DBG2, "Serializing UpdateDescriptor (zero-copy)");

    if (!desc.serializedGroupPDU) {
      XLOG(ERR, "UpdateDescriptor has null serializedGroupPDU");
      return std::make_unique<folly::IOBuf>();
    }

    if (desc.nexthopOffsets.empty()) {
      // No nexthops to mutate (e.g., withdraw-only messages), just clone
      return desc.serializedGroupPDU->clone();
    }

    // Validate nexthop addresses based on what we need
    bool needV4 = false;
    bool needV6 = false;
    for (const auto& [bufferIndex, offset, isV4] : desc.nexthopOffsets) {
      if (isV4) {
        needV4 = true;
      } else {
        needV6 = true;
      }
    }

    if (needV4 && desc.v4Nexthop.family() == AF_UNSPEC) {
      XLOG(ERR, "UpdateDescriptor requires v4 nexthop but it's not set");
      return desc.serializedGroupPDU->clone();
    }
    if (needV6 && desc.v6Nexthop.family() == AF_UNSPEC) {
      XLOG(ERR, "UpdateDescriptor requires v6 nexthop but it's not set");
      return desc.serializedGroupPDU->clone();
    }

    // Clone and build a map of buffer indices
    auto [clonedPdu, bufferMap] =
        cloneAndBuildBufferMap(desc.serializedGroupPDU.get());

    if (!clonedPdu) {
      XLOG(ERR, "Failed to clone IOBuf chain");
      return std::make_unique<folly::IOBuf>();
    }

    // Mutate all nexthops
    for (const auto& [bufferIndex, offset, isV4] : desc.nexthopOffsets) {
      auto it = bufferMap.find(bufferIndex);
      if (it == bufferMap.end()) {
        XLOGF(ERR, "Buffer index {} not found in clone", bufferIndex);
        continue;
      }

      folly::IOBuf* targetBuffer = it->second;
      uint8_t* data = targetBuffer->writableData();
      size_t bufferLen = targetBuffer->length();

      const folly::IPAddress& nexthop = isV4 ? desc.v4Nexthop : desc.v6Nexthop;
      size_t nexthopSize = nexthop.byteCount();

      if (offset + nexthopSize <= bufferLen) {
        const auto* nexthopBytes = nexthop.bytes();
        std::memcpy(data + offset, nexthopBytes, nexthopSize);
        XLOGF(
            DBG3,
            "Mutated {} nexthop at buffer[{}] offset {}",
            (isV4 ? "v4" : "v6"),
            bufferIndex,
            offset);
      } else {
        XLOGF(
            ERR,
            "Nexthop offset + size exceeds buffer length: {} + {} > {}",
            offset,
            nexthopSize,
            bufferLen);
      }
    }

    return std::move(clonedPdu);
  }

 private:
  /**
   * Clone an IOBuf chain and build a map of buffer indices to buffers.
   *
   * PERFORMANCE: O(n) where n = number of buffers in chain
   * - Each cloneOne() is O(1) and clones only the current IOBuf node
   * - Each prependChain() is O(1) (just updates pointers)
   * - Total: O(n) for entire chain
   *
   * CORRECTNESS: Uses prependChain() to link each cloned node in O(1)
   * - cloneOne() preserves one cloned node per original node, so the cloned
   *   chain has the same element count and data length as the original chain
   * - prependChain() links the new node into the circular chain without walking
   *   the existing chain
   * - bufferMap stores the cloned node before linking, preserving the original
   *   buffer-index-to-node mapping used by nexthopOffsets
   *
   * @param original - Original IOBuf chain to clone
   * @return Pair of (cloned chain head, map of buffer indices to IOBuf*)
   */
  static std::pair<
      std::unique_ptr<folly::IOBuf>,
      std::unordered_map<size_t, folly::IOBuf*>>
  cloneAndBuildBufferMap(const folly::IOBuf* original) {
    if (!original) {
      return {nullptr, std::unordered_map<size_t, folly::IOBuf*>{}};
    }

    std::unique_ptr<folly::IOBuf> head;
    std::unordered_map<size_t, folly::IOBuf*> bufferMap;

    const folly::IOBuf* current = original;
    size_t index = 0;

    do {
      auto cloned = current->cloneOne();
      folly::IOBuf* clonedPtr = cloned.get();

      // Add to map
      bufferMap[index] = clonedPtr;

      if (!head) {
        // First buffer - becomes initial head
        head = std::move(cloned);
      } else {
        head->prependChain(std::move(cloned));
      }

      current = current->next();
      index++;
    } while (current != original);

    return {std::move(head), std::move(bufferMap)};
  }

 public:
  std::unique_ptr<folly::IOBuf> operator()(FiberSocketError const&) {
    // ATTN: this is dummy function and won't be called
    return nullptr;
  }

 private:
  // will throw if update contains NLRI of non-negotiated AFI
  void validateBgpUpdate2(
      BgpUpdate2 const& update,
      BgpCapabilities const& capa);
  bool isAddressFamilyAllowed(
      const BgpCapabilities& capa,
      const BgpUpdateAfi& afi,
      const BgpUpdateSafi& safi);
};
} // namespace bgplib
} // namespace nettools
} // namespace facebook
