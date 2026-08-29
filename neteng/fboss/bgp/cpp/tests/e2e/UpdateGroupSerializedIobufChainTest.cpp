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

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestFixture.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <folly/coro/BlockingWait.h>
#include <folly/logging/xlog.h>

#include "common/network/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/BgpSerializer.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeer.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibInUtils.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {
namespace {

constexpr int kRouteCount = 1200;
constexpr int kMaxDescriptorReads = 2048;
constexpr int kMaxEmptyFlushes = 50;

/*
 * Byte-level packing constants for the update-group serialization path.
 *
 * For V4 announcements the update group always wraps prefixes in
 * MP_REACH_NLRI. The per-message NLRI packer (BgpMessageSerializer::
 * serializeNlri) inspects the worst-case prefix length passed in by the
 * caller; for the mpAnnounced path that is `kMaxV6PrefixLen` (17 bytes)
 * because the V6-aware getMaxPrefixLen() is called with `v4=false`. The
 * packer's stop condition is `nlri_used + (maxPrefixLen + 1) >= bufSize`,
 * so the effective per-message reserve is 18 bytes regardless of whether
 * the prefixes themselves are V4 or V6.
 *
 * Per-prefix NLRI wire size:
 *   - V4 /24: 1 length byte + 3 prefix bytes = 4 bytes
 *   - V4 /32: 1 length byte + 4 prefix bytes = 5 bytes
 */
constexpr size_t kMpV6BasedPackerReserve =
    kMaxV6PrefixLen + sizeof(uint8_t); // 18
constexpr size_t kV4Slash24NlriBytes = 4;
constexpr size_t kV4Slash32NlriBytes = 5;

constexpr int kMaxPackingV4Slash24Count = 4000;
constexpr int kMaxPackingV4Slash32Count = 3000;
constexpr int kSmallTailV4Slash24Count = 1010;

BgpPeerId makePeerId(const folly::IPAddress& peerAddr) {
  return BgpPeerId{peerAddr, peerAddr.asV4().toLongHBO()};
}

std::shared_ptr<const BgpUpdate2> makeMultiPrefixUpdate() {
  auto update = createBgpUpdateAnnouncement(
      /*isV4=*/true,
      folly::CIDRNetwork{folly::IPAddress("44.0.0.0"), 24},
      "10.44.0.1",
      {64512, 64513},
      {"4400:1"},
      /*addPathId=*/0,
      /*localPref=*/150,
      /*med=*/25);

  for (int i = 1; i < kRouteCount; ++i) {
    RiggedIPPrefix prefix;
    prefix.prefix() = facebook::network::toIPPrefix(
        folly::CIDRNetwork{
            folly::IPAddress(
                "44." + std::to_string(i / 256) + "." +
                std::to_string(i % 256) + ".0"),
            24});
    update->v4Announced2()->push_back(std::move(prefix));
  }

  return update;
}

folly::CIDRNetwork lastRoutePrefix() {
  const int lastIndex = kRouteCount - 1;
  return folly::CIDRNetwork{
      folly::IPAddress(
          "44." + std::to_string(lastIndex / 256) + "." +
          std::to_string(lastIndex % 256) + ".0"),
      24};
}

/*
 * Build a single BgpUpdate2 announcing `count` V4 /24 prefixes that all share
 * the same path attributes. Prefixes count up from baseSecondOctet.0.0.0/24
 * so different tests can pick non-overlapping address ranges.
 *
 * Identical attributes are the precondition for maximum packing: every
 * prefix lands in the same update group and the group serializer is free to
 * pack them into the fewest possible UPDATE messages.
 */
std::shared_ptr<const BgpUpdate2> makeIdenticalAttrV4Slash24Update(
    int count,
    int baseSecondOctet,
    const std::string& nexthop,
    const std::string& communityTag) {
  auto update = createBgpUpdateAnnouncement(
      /*isV4=*/true,
      folly::CIDRNetwork{
          folly::IPAddress(std::to_string(baseSecondOctet) + ".0.0.0"), 24},
      nexthop,
      {64512, 64513},
      {communityTag},
      /*addPathId=*/0,
      /*localPref=*/150,
      /*med=*/25);

  for (int i = 1; i < count; ++i) {
    RiggedIPPrefix prefix;
    prefix.prefix() = facebook::network::toIPPrefix(
        folly::CIDRNetwork{
            folly::IPAddress(
                std::to_string(baseSecondOctet) + "." +
                std::to_string(i / 256) + "." + std::to_string(i % 256) + ".0"),
            24});
    update->v4Announced2()->push_back(std::move(prefix));
  }
  return update;
}

folly::CIDRNetwork v4Slash24PrefixAt(int baseSecondOctet, int idx) {
  return folly::CIDRNetwork{
      folly::IPAddress(
          std::to_string(baseSecondOctet) + "." + std::to_string(idx / 256) +
          "." + std::to_string(idx % 256) + ".0"),
      24};
}

/*
 * Build a single BgpUpdate2 announcing `count` V4 /32 prefixes that all share
 * the same path attributes. Hosts count up from baseSecondOctet.0.0.0/32.
 */
std::shared_ptr<const BgpUpdate2> makeIdenticalAttrV4Slash32Update(
    int count,
    int baseSecondOctet,
    const std::string& nexthop,
    const std::string& communityTag) {
  auto firstAddr = std::to_string(baseSecondOctet) + ".0.0.0";
  auto update = createBgpUpdateAnnouncement(
      /*isV4=*/true,
      folly::CIDRNetwork{folly::IPAddress(firstAddr), 32},
      nexthop,
      {64512, 64513},
      {communityTag},
      /*addPathId=*/0,
      /*localPref=*/150,
      /*med=*/25);

  for (int i = 1; i < count; ++i) {
    RiggedIPPrefix prefix;
    prefix.prefix() = facebook::network::toIPPrefix(
        folly::CIDRNetwork{
            folly::IPAddress(
                std::to_string(baseSecondOctet) + "." +
                std::to_string((i >> 16) & 0xff) + "." +
                std::to_string((i >> 8) & 0xff) + "." +
                std::to_string(i & 0xff)),
            32});
    update->v4Announced2()->push_back(std::move(prefix));
  }
  return update;
}

folly::CIDRNetwork v4Slash32PrefixAt(int baseSecondOctet, int idx) {
  return folly::CIDRNetwork{
      folly::IPAddress(
          std::to_string(baseSecondOctet) + "." +
          std::to_string((idx >> 16) & 0xff) + "." +
          std::to_string((idx >> 8) & 0xff) + "." + std::to_string(idx & 0xff)),
      32};
}

BgpCapabilities makeDefaultCaps() {
  BgpCapabilities caps;
  caps.as4byte() = true;
  caps.mpExtV4Unicast() = true;
  caps.mpExtV6Unicast() = true;
  return caps;
}

/*
 * Byte-precise validator for the "maximum packing" property of the update-group
 * serialization path, aggregated across ALL UpdateDescriptors produced for one
 * logical announcement.
 *
 * The per-consume-tick group flush can split a single announcement of
 * `totalPrefixes` prefixes across MULTIPLE UpdateDescriptors (one chain per
 * flush). How the RIB chunks delivery versus how fast the consumer drains is
 * timing dependent, so the prefixes do NOT reliably land in a single chain —
 * asserting that the first multi-element chain holds every prefix is exactly
 * what made the older single-chain validator flaky under load. This validator
 * instead takes EVERY descriptor and proves byte-for-byte that:
 *
 *   1. Every chain element respects the BGP wire limit (kMaxBgpMsgLen).
 *   2. Within each flush, every non-tail element has the SAME byte length as a
 *      full UPDATE (identical attributes ⇒ identical per-message NLRI capacity,
 *      so uniform packing is the only correct outcome).
 *   3. The derived pre-NLRI overhead is integer-consistent across the entire
 *      flattened element set (a non-integer derivation would mean prefixes were
 *      lost/duplicated or attributes diverged between flushes).
 *   4. NLRI bytes in every element are exact multiples of `prefixWireBytes`.
 *   5. The total prefix count summed across ALL descriptors equals
 *      `totalPrefixes` exactly — nothing lost, nothing duplicated end-to-end.
 *   6. Full-message slack lies in the narrow window
 *      [reserve - prefixWireBytes + 1, reserve]. Only checked when at least one
 *      flush overflowed a message (which proves `fullLength` is a genuine
 *      capacity-limited full UPDATE rather than just the largest partial).
 *   7. Each flush's tail holds at least 1 prefix and at most `nFull` prefixes.
 *   8. Each flush is internally optimal: its element count is the minimum
 *      needed for its own prefixes. The GLOBAL element count is intentionally
 *      NOT asserted to be the single-flush optimum ceil(totalPrefixes / nFull):
 *      when an announcement is split across F flushes each flush carries its
 *      own partial tail, so the total message count can legitimately exceed the
 *      single-flush optimum by up to F-1.
 */
void verifyAggregateMaxPacking(
    const std::vector<UpdateDescriptor>& descriptors,
    size_t totalPrefixes,
    size_t prefixWireBytes,
    size_t packerReserveBytes,
    folly::StringPiece caseLabel) {
  SCOPED_TRACE(caseLabel);
  ASSERT_GT(totalPrefixes, 0u);
  ASSERT_GT(prefixWireBytes, 0u);
  ASSERT_GE(packerReserveBytes, prefixWireBytes);
  ASSERT_FALSE(descriptors.empty())
      << "No UpdateDescriptors collected for the announcement";

  // Flatten every chain element of every descriptor, keeping the per-descriptor
  // grouping so each flush can be validated as its own packing run.
  std::vector<std::vector<size_t>> perFlushLengths;
  size_t totalElementCount = 0;
  int64_t sumAllLengths = 0;
  size_t fullLength = 0;
  bool sawMultiElement = false;

  for (const auto& descriptor : descriptors) {
    if (descriptor.serializedGroupPDU == nullptr) {
      continue;
    }
    std::vector<size_t> lengths;
    const auto* head = descriptor.serializedGroupPDU.get();
    const auto* current = head;
    do {
      lengths.push_back(current->length());
      current = current->next();
    } while (current != head);

    totalElementCount += lengths.size();
    if (lengths.size() >= 2) {
      sawMultiElement = true;
    }
    for (size_t len : lengths) {
      sumAllLengths += static_cast<int64_t>(len);
      if (len > fullLength) {
        fullLength = len;
      }
      // Invariant 1: every element fits within the BGP wire limit.
      EXPECT_LE(len, kMaxBgpMsgLen) << "Element exceeds BGP wire limit";
    }
    perFlushLengths.push_back(std::move(lengths));
  }

  ASSERT_GT(totalElementCount, 0u) << "All descriptors had a null PDU";
  ASSERT_GT(fullLength, 0u);

  /*
   * Invariant 3: derive the per-message header+attrs overhead (preNLRI) from
   * the flattened data. Every UPDATE for these identical-attribute prefixes
   * shares the same preNLRI and differs only in NLRI prefix count, so:
   *
   *   sum(elementLengths) = totalElementCount * preNLRI + totalPrefixes * ps
   *   => preNLRI = (sum(elementLengths) - totalPrefixes * ps) /
   * totalElementCount
   *
   * The right-hand side MUST be a non-negative exact integer; otherwise the
   * aggregate layout is internally inconsistent (prefixes lost/duplicated, or
   * mismatched attributes across flushes).
   */
  const int64_t totalNLRI = static_cast<int64_t>(totalPrefixes) *
      static_cast<int64_t>(prefixWireBytes);
  ASSERT_GE(sumAllLengths, totalNLRI)
      << "Sum of element lengths (" << sumAllLengths
      << ") is less than total NLRI bytes (" << totalNLRI
      << "); prefixes appear to be missing from the serialized output";
  ASSERT_EQ(
      0, (sumAllLengths - totalNLRI) % static_cast<int64_t>(totalElementCount))
      << "Derived pre-NLRI overhead is not an integer across "
      << totalElementCount << " elements; aggregate layout is inconsistent";
  const size_t preNLRI = static_cast<size_t>(
      (sumAllLengths - totalNLRI) / static_cast<int64_t>(totalElementCount));

  ASSERT_GT(fullLength, preNLRI);
  ASSERT_EQ(0u, (fullLength - preNLRI) % prefixWireBytes)
      << "Full-element NLRI bytes (" << (fullLength - preNLRI)
      << ") are not a multiple of prefix wire size " << prefixWireBytes;
  const size_t nFull = (fullLength - preNLRI) / prefixWireBytes;
  ASSERT_GT(nFull, 0u);

  /*
   * Invariant 6: full-message slack ∈ [reserve - prefixWireBytes + 1, reserve].
   *
   * The packer's loop is `while (nlri_used + reserve < bufSize)`; a full
   * message therefore leaves slack in that window. Only meaningful when some
   * flush actually overflowed a message — that overflow is what proves
   * `fullLength` is a genuine capacity-limited full UPDATE rather than simply
   * the largest partial seen this run.
   */
  if (sawMultiElement) {
    const size_t fullSlack = kMaxBgpMsgLen - fullLength;
    EXPECT_LE(fullSlack, packerReserveBytes)
        << "Full-message slack " << fullSlack << " exceeds packer reserve "
        << packerReserveBytes
        << "; packer could have fit another prefix in each full UPDATE";
    EXPECT_GE(fullSlack + prefixWireBytes, packerReserveBytes + 1)
        << "Full-message slack " << fullSlack
        << " is below the packer's minimum stop-condition slack ("
        << (packerReserveBytes - prefixWireBytes + 1)
        << "); packer stopped before its own stop condition fired";
  } else {
    XLOGF(
        INFO,
        "{}: no flush overflowed a single UPDATE; max-packing to capacity not exercised this run, validating prefix accounting only",
        caseLabel);
  }

  // Per-flush invariants + global prefix accounting.
  size_t accumulatedPrefixes = 0;
  for (const auto& lengths : perFlushLengths) {
    const size_t flushElements = lengths.size();
    size_t flushPrefixes = 0;
    for (size_t i = 0; i < flushElements; ++i) {
      const size_t len = lengths[i];
      // Invariant 4: NLRI bytes are exact multiples of the prefix wire size.
      ASSERT_GE(len, preNLRI);
      ASSERT_EQ(0u, (len - preNLRI) % prefixWireBytes)
          << "Element NLRI bytes (" << (len - preNLRI)
          << ") are not a multiple of prefix wire size " << prefixWireBytes;
      const size_t nPrefixes = (len - preNLRI) / prefixWireBytes;

      if (i + 1 < flushElements) {
        // Invariant 2: every non-tail element of a flush is full.
        EXPECT_EQ(fullLength, len)
            << "Non-tail element " << i
            << " of a flush is not full; the serializer should pack each "
            << "non-tail UPDATE to capacity under identical attributes";
      } else {
        // Invariant 7: the flush tail holds [1, nFull] prefixes.
        EXPECT_GT(nPrefixes, 0u) << "Flush tail holds 0 prefixes";
        EXPECT_LE(nPrefixes, nFull) << "Flush tail holds " << nPrefixes
                                    << " prefixes, more than nFull=" << nFull
                                    << "; non-tail packing was sub-optimal";
      }
      flushPrefixes += nPrefixes;
    }
    // Invariant 8: each flush uses the minimum number of UPDATEs for its own
    // prefixes.
    const size_t minFlushElements = (flushPrefixes + nFull - 1) / nFull;
    EXPECT_EQ(flushElements, minFlushElements)
        << "Flush has " << flushElements << " elements but its "
        << flushPrefixes << " prefixes need only " << minFlushElements
        << " at nFull=" << nFull;
    accumulatedPrefixes += flushPrefixes;
  }

  // Invariant 5: exactly `totalPrefixes` serialized across the whole flush
  // sequence — no loss, no duplication.
  EXPECT_EQ(totalPrefixes, accumulatedPrefixes)
      << "Aggregate prefix count mismatch across " << descriptors.size()
      << " descriptors";
}

} // namespace

class UpdateGroupSerializedIobufChainTest : public E2ETestFixture {
 protected:
  void SetUp() override {
    addPeer(kDefaultPeerSpec3);
    addPeer(kDefaultPeerSpec4);
    setDefaultQueueSizes(/*capacity=*/4096, /*highWm=*/3500, /*lowWm=*/1000);
    createRib();
    createPeerManager(
        /*enableUpdateGroup=*/true,
        /*enableEgressBackpressure=*/true,
        /*enableSerializeGroupPdu=*/true);
  }

  std::optional<FiberBgpPeer::InputMessageT> popNextMessage(
      const BgpPeerId& peerId) {
    auto queues = getPeerQueues(peerId);
    if (!queues) {
      return std::nullopt;
    }

    int emptyFlushes = 0;
    while (queues->boundedAdjRibOutQ->empty()) {
      if (emptyFlushes++ >= kMaxEmptyFlushes) {
        return std::nullopt;
      }
      rib_->getEventBase().runInEventBaseThreadAndWait([] {});
      peerManager_->getEventBase().runInEventBaseThreadAndWait([] {});
    }

    return folly::coro::blockingWait(queues->boundedAdjRibOutQ->pop());
  }

  std::optional<UpdateDescriptor> findMultiNodeUpdateDescriptor(
      const BgpPeerId& peerId) {
    for (int i = 0; i < kMaxDescriptorReads; ++i) {
      auto message = popNextMessage(peerId);
      if (!message) {
        return std::nullopt;
      }

      if (std::holds_alternative<BgpEndOfRib>(*message)) {
        continue;
      }

      auto* descriptor = std::get_if<UpdateDescriptor>(&*message);
      if (descriptor == nullptr) {
        ADD_FAILURE() << "Expected serialized update-group path to queue "
                      << "UpdateDescriptor messages";
        return std::nullopt;
      }

      EXPECT_NE(nullptr, descriptor->serializedGroupPDU);
      if (descriptor->serializedGroupPDU &&
          descriptor->serializedGroupPDU->countChainElements() > 1) {
        return *descriptor;
      }
    }

    return std::nullopt;
  }

  /*
   * Drain EVERY UpdateDescriptor the serialized update-group path queues for
   * the announcement. The per-consume-tick group flush emits descriptors in
   * bursts with timing-dependent gaps between them, so a plain "stop after N
   * consecutive empty pumps" quiescence check stops at the first inter-burst
   * gap and silently drops the rest of the announcement.
   *
   * Instead we gate completion on a hard lower bound: the serialized output
   * MUST carry at least `expectedMinBytes` (= totalPrefixes * prefixWireBytes)
   * NLRI bytes, since every full chain element also adds per-message overhead.
   * While the collected byte count is below that floor the production is
   * definitely still in flight, so we keep pumping both event bases patiently.
   * Once the floor is met we require a settle window of sustained emptiness so
   * the trailing descriptor(s) are captured before declaring the queue done.
   * The caller's verifyAggregateMaxPacking() then asserts the exact total.
   */
  std::vector<UpdateDescriptor> collectAllUpdateDescriptors(
      const BgpPeerId& peerId,
      size_t expectedMinBytes) {
    constexpr int kSettleEmptyFlushes = 300;
    constexpr int kMaxIncompleteFlushes = 4000;

    std::vector<UpdateDescriptor> descriptors;
    size_t collectedBytes = 0;
    int emptyFlushes = 0;

    for (int reads = 0; reads < kMaxDescriptorReads;) {
      auto queues = getPeerQueues(peerId);
      if (!queues) {
        break;
      }

      if (queues->boundedAdjRibOutQ->empty()) {
        const int patience = (collectedBytes < expectedMinBytes)
            ? kMaxIncompleteFlushes
            : kSettleEmptyFlushes;
        if (emptyFlushes++ >= patience) {
          break;
        }
        rib_->getEventBase().runInEventBaseThreadAndWait([] {});
        peerManager_->getEventBase().runInEventBaseThreadAndWait([] {});
        continue;
      }

      emptyFlushes = 0;
      ++reads;
      auto message =
          folly::coro::blockingWait(queues->boundedAdjRibOutQ->pop());
      if (!message) {
        continue;
      }

      if (std::holds_alternative<BgpEndOfRib>(*message)) {
        continue;
      }

      auto* descriptor = std::get_if<UpdateDescriptor>(&*message);
      if (descriptor == nullptr) {
        ADD_FAILURE() << "Expected serialized update-group path to queue "
                      << "UpdateDescriptor messages";
        break;
      }

      EXPECT_NE(nullptr, descriptor->serializedGroupPDU);
      if (descriptor->serializedGroupPDU) {
        collectedBytes +=
            descriptor->serializedGroupPDU->computeChainDataLength();
      }
      descriptors.push_back(std::move(*descriptor));
    }
    return descriptors;
  }
};

TEST_F(
    UpdateGroupSerializedIobufChainTest,
    SerializerPreservesMultiNodeSerializedGroupPduChain) {
  const auto peerId3 = makePeerId(kPeerAddr3);
  const auto peerId4 = makePeerId(kPeerAddr4);

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));

  auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  folly::coro::blockingWait(queues->adjRibInQ->push(makeMultiPrefixUpdate()));
  ASSERT_TRUE(waitForRouteInShadowRib(lastRoutePrefix(), kPeerAddr3));

  auto descriptor = findMultiNodeUpdateDescriptor(peerId4);
  ASSERT_TRUE(descriptor.has_value())
      << "Expected at least one packed UpdateDescriptor with a multi-node "
      << "serializedGroupPDU chain";
  ASSERT_NE(nullptr, descriptor->serializedGroupPDU);

  const auto originalChainElements =
      descriptor->serializedGroupPDU->countChainElements();
  const auto originalDataLength =
      descriptor->serializedGroupPDU->computeChainDataLength();
  ASSERT_GT(originalChainElements, 1);

  BgpSerializer serializer(makeDefaultCaps());
  auto serialized = serializer(*descriptor);
  ASSERT_NE(nullptr, serialized);

  EXPECT_EQ(originalChainElements, serialized->countChainElements());
  EXPECT_EQ(originalDataLength, serialized->computeChainDataLength());
}

/*
 * Byte-precise maximum-packing regression for V4 /24 announcements.
 *
 * Announces kMaxPackingV4Slash24Count (4000) /24 prefixes through one
 * BgpUpdate2 with shared attributes. The update group serializer wraps them
 * in MP_REACH_NLRI and splits across multiple BGP UPDATE messages, possibly
 * over several per-flush descriptors. The output MUST be packed byte-optimally:
 * see verifyAggregateMaxPacking() for the full list of byte-exact invariants
 * checked. The cloning path through BgpSerializer must then preserve every byte
 * end-to-end.
 */
TEST_F(
    UpdateGroupSerializedIobufChainTest,
    SerializerByteExactMaxPacksV4Slash24Prefixes) {
  const auto peerId3 = makePeerId(kPeerAddr3);
  const auto peerId4 = makePeerId(kPeerAddr4);

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));

  auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  folly::coro::blockingWait(
      queues->adjRibInQ->push(makeIdenticalAttrV4Slash24Update(
          kMaxPackingV4Slash24Count,
          /*baseSecondOctet=*/55,
          "10.55.0.1",
          "5500:1")));
  ASSERT_TRUE(waitForRouteInShadowRib(
      v4Slash24PrefixAt(/*baseSecondOctet=*/55, kMaxPackingV4Slash24Count - 1),
      kPeerAddr3));

  auto descriptors = collectAllUpdateDescriptors(
      peerId4, kMaxPackingV4Slash24Count * kV4Slash24NlriBytes);
  ASSERT_FALSE(descriptors.empty())
      << "Expected serialized UpdateDescriptors for "
      << kMaxPackingV4Slash24Count << " same-attribute /24 prefixes";

  verifyAggregateMaxPacking(
      descriptors,
      kMaxPackingV4Slash24Count,
      kV4Slash24NlriBytes,
      kMpV6BasedPackerReserve,
      "V4 /24 x 4000 prefixes");

  // The cloning path through BgpSerializer must preserve every byte of every
  // descriptor end-to-end.
  BgpSerializer serializer(makeDefaultCaps());
  for (auto& descriptor : descriptors) {
    ASSERT_NE(nullptr, descriptor.serializedGroupPDU);
    const auto chainElements =
        descriptor.serializedGroupPDU->countChainElements();
    const auto dataLength =
        descriptor.serializedGroupPDU->computeChainDataLength();
    auto serialized = serializer(descriptor);
    ASSERT_NE(nullptr, serialized);
    EXPECT_EQ(chainElements, serialized->countChainElements());
    EXPECT_EQ(dataLength, serialized->computeChainDataLength());
  }
}

/*
 * Byte-precise maximum-packing test with a different per-prefix wire size.
 *
 * V4 /32 prefixes are 5 bytes on the wire (1 length + 4 prefix) instead of 4
 * bytes for /24. This exercises the packer's stop-condition arithmetic with
 * a different `prefixWireBytes` divisor, and verifies the slack window
 * narrows accordingly: for ps=5 and reserve=18 the allowed slack is
 * [reserve - ps + 1, reserve] = [14, 18].
 */
TEST_F(
    UpdateGroupSerializedIobufChainTest,
    SerializerByteExactMaxPacksV4Slash32Prefixes) {
  const auto peerId3 = makePeerId(kPeerAddr3);
  const auto peerId4 = makePeerId(kPeerAddr4);

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));

  auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  folly::coro::blockingWait(
      queues->adjRibInQ->push(makeIdenticalAttrV4Slash32Update(
          kMaxPackingV4Slash32Count,
          /*baseSecondOctet=*/66,
          "10.66.0.1",
          "6600:1")));
  ASSERT_TRUE(waitForRouteInShadowRib(
      v4Slash32PrefixAt(/*baseSecondOctet=*/66, kMaxPackingV4Slash32Count - 1),
      kPeerAddr3));

  auto descriptors = collectAllUpdateDescriptors(
      peerId4, kMaxPackingV4Slash32Count * kV4Slash32NlriBytes);
  ASSERT_FALSE(descriptors.empty())
      << "Expected serialized UpdateDescriptors for "
      << kMaxPackingV4Slash32Count << " same-attribute /32 prefixes";

  verifyAggregateMaxPacking(
      descriptors,
      kMaxPackingV4Slash32Count,
      kV4Slash32NlriBytes,
      kMpV6BasedPackerReserve,
      "V4 /32 x 3000 prefixes");

  // The cloning path through BgpSerializer must preserve every byte of every
  // descriptor end-to-end.
  BgpSerializer serializer(makeDefaultCaps());
  for (auto& descriptor : descriptors) {
    ASSERT_NE(nullptr, descriptor.serializedGroupPDU);
    const auto chainElements =
        descriptor.serializedGroupPDU->countChainElements();
    const auto dataLength =
        descriptor.serializedGroupPDU->computeChainDataLength();
    auto serialized = serializer(descriptor);
    ASSERT_NE(nullptr, serialized);
    EXPECT_EQ(chainElements, serialized->countChainElements());
    EXPECT_EQ(dataLength, serialized->computeChainDataLength());
  }
}

/*
 * Boundary corner: smallest prefix count that still requires splitting into
 * multiple BGP UPDATE messages. kSmallTailV4Slash24Count = 1010 is just above
 * one message's worth of /24 prefixes (1003 with our attribute set, but the
 * count is robust across reasonable attrLen variation - any K_max >= 506
 * yields a 2-element chain). The tail therefore holds only a handful of
 * prefixes (~7 with attrLen=30), exercising the small-tail corner that the
 * 4000-prefix tests do not reach.
 */
TEST_F(
    UpdateGroupSerializedIobufChainTest,
    SerializerByteExactMaxPacksV4Slash24WithSmallTail) {
  const auto peerId3 = makePeerId(kPeerAddr3);
  const auto peerId4 = makePeerId(kPeerAddr4);

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  ASSERT_TRUE(waitForEoR(peerId3));
  ASSERT_TRUE(waitForEoR(peerId4));

  auto queues = getPeerQueues(peerId3);
  ASSERT_TRUE(queues.has_value());
  folly::coro::blockingWait(
      queues->adjRibInQ->push(makeIdenticalAttrV4Slash24Update(
          kSmallTailV4Slash24Count,
          /*baseSecondOctet=*/77,
          "10.77.0.1",
          "7700:1")));
  ASSERT_TRUE(waitForRouteInShadowRib(
      v4Slash24PrefixAt(/*baseSecondOctet=*/77, kSmallTailV4Slash24Count - 1),
      kPeerAddr3));

  auto descriptors = collectAllUpdateDescriptors(
      peerId4, kSmallTailV4Slash24Count * kV4Slash24NlriBytes);
  ASSERT_FALSE(descriptors.empty())
      << "Expected serialized UpdateDescriptors for "
      << kSmallTailV4Slash24Count << " same-attribute /24 prefixes; packer "
      << "should not collapse them into a single oversized UPDATE";

  verifyAggregateMaxPacking(
      descriptors,
      kSmallTailV4Slash24Count,
      kV4Slash24NlriBytes,
      kMpV6BasedPackerReserve,
      "V4 /24 x 1010 prefixes (small-tail boundary)");

  // The cloning path through BgpSerializer must preserve every byte of every
  // descriptor end-to-end.
  BgpSerializer serializer(makeDefaultCaps());
  for (auto& descriptor : descriptors) {
    ASSERT_NE(nullptr, descriptor.serializedGroupPDU);
    const auto chainElements =
        descriptor.serializedGroupPDU->countChainElements();
    const auto dataLength =
        descriptor.serializedGroupPDU->computeChainDataLength();
    auto serialized = serializer(descriptor);
    ASSERT_NE(nullptr, serialized);
    EXPECT_EQ(chainElements, serialized->countChainElements());
    EXPECT_EQ(dataLength, serialized->computeChainDataLength());
  }
}

} // namespace bgp
} // namespace facebook
