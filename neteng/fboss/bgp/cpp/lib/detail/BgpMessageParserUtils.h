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
#include <folly/io/Cursor.h>
#include <folly/logging/xlog.h>
#include <optional>

#include "neteng/fboss/bgp/cpp/lib/BgpException.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"

// Helpers for BgpMessageParser
namespace facebook {
namespace nettools {
namespace bgplib {
namespace detail {

/**
 * Helper function to read numeric value from socket buffer. It converts the
 * network representation to host represent afterwards.
 * This can be used for 16/32/64 bit integers.
 */
template <typename T>
inline T readBigEndian(const uint8_t* buf) {
  T val;
  ::memcpy((void*)&val, (void*)buf, sizeof(T));
  auto len = sizeof(T);
  if (len == 2) {
    val = ntohs(val);
  } else if (len == 4) {
    val = ntohl(val);
  } else if (len == 8) {
    val = be64toh(val);
  } else {
    XLOGF(ERR, "Unknown type of length {}", len);
  }
  return val;
}

/**
 * Private structs to facilitate the parsing of BgpUpdate messages
 */
struct BgpPrefix {
  const folly::CIDRNetwork prefix;
  /*
   * ideally label should be uint32_t, but we make it
   * int32_t because of thrift types
   */
  const std::vector<int32_t> labels;

  // path id used by ADD_PATH feature.
  const std::optional<int32_t> pathId;

  // Constructor
  BgpPrefix(
      const folly::CIDRNetwork& prefix,
      const std::vector<int32_t>& labels,
      const std::optional<int32_t>& pathId = std::nullopt)
      : prefix(prefix), labels(labels), pathId(pathId) {}
};

/**
 * Utility function to parse a NLRI prefix(v4 + v6) and populate "prefix"
 * parameter.
 * @param prefixLen: Length of prefix in bits
 * @returns: CIDRNetwork
 * @throws: BgpUpdateMsgException on failure
 */
folly::CIDRNetwork
parseNlriPrefix(folly::io::Cursor cursor, uint8_t prefixLen, BgpUpdateAfi afi);

/**
 * Utility function to parse the Network Layer Reachability Information
 * (NLRI) and populate the list of prefixes.
 * @returns: vector<BgpPrefix>
 * @throws: BgpUpdateMsgException on failure
 */
std::vector<BgpPrefix> parseNlri(
    folly::io::Cursor cursor,
    BgpUpdateAfi afi,
    BgpUpdateSafi safi,
    bool multiProtocol,
    const BgpCapabilities& capabilities);

/**
 * Utility function to parse Multi Protocol Network Layer Reachability
 * Iinformation (MP_NLRI) and populate list of update/withdrawn prefixes.
 * This handles cases for different safi
 * @returns: vector<BgpPrefix>, length of consumed data (in bytes)
 * @throws: BgpUpdateMsgException on failure
 */
std::vector<BgpPrefix> parseMpNlri(
    folly::io::Cursor cursor,
    BgpUpdateAfi afi,
    BgpUpdateSafi safi,
    const BgpCapabilities& capa,
    std::string attrBeingParsed,
    BgpAttrCode attrType);

struct UpdateMsgParsingState {
  // Holds all BgpAttributes except next-hop and MP attributes
  BgpAttributes attrs;

  // Classical v4 announced/withdrawn prefixes
  std::vector<BgpPrefix> v4Announced;
  std::vector<BgpPrefix> v4Withdrawn;

  /*
   * Multiprotocol announced/withdrawn prefixes
   */
  BgpUpdateAfi mpAnnouncedAfi{static_cast<BgpUpdateAfi>(0)};
  BgpUpdateSafi mpAnnouncedSafi{static_cast<BgpUpdateSafi>(0)};
  std::vector<BgpPrefix> mpAnnounced;

  BgpUpdateAfi mpWithdrawnAfi{static_cast<BgpUpdateAfi>(0)};
  BgpUpdateSafi mpWithdrawnSafi{static_cast<BgpUpdateSafi>(0)};
  std::vector<BgpPrefix> mpWithdrawn;

  // classical v4 withdrawn route length and path attribute length
  uint16_t v4WithdrawnLen{0};
  uint16_t paLen{0};

  // this is set if message is End Of Rib
  std::optional<BgpEndOfRib> eor;

  /*
   * Bgp Nexthops in update message are parsed into following variable
   * and at the end it is populated in "attrs"
   */

  // Bgp atribute NEXT_HOP
  folly::IPAddress v4Nexthop;

  // Nexthop information encoded in MP_REACH_NLRI
  folly::IPAddress mpNexthop;

  /*
   * Booleans to keep state of different attributes
   */

  // Origin attribute was found
  bool hasOriginAttr{false};

  // AS_PATH attribute was found
  bool hasAsPathAttr{false};

  /*
   * Parsing methods
   */

  /**
   * Parse the IPv4 Withdrawn routes at the start of the message
   */
  void parseV4Withdrawn(
      folly::io::Cursor& cursor,
      const BgpCapabilities& capabilities);

  /**
   * Parse the IPv4 Announced routes at the start of the message
   */
  void parseV4Announced(
      folly::io::Cursor& cursor,
      const BgpCapabilities& capabilities);

  /**
   * Check whether the attribute flag is correct
   * If the flag is wrong, throw BN_UM_ATTR_FLAGS_ERR error
   */
  void checkPathAttributeFlag(
      uint8_t attrFlags,
      BgpAttrCode attrType,
      std::string attrBeingParsed);

  /**
   * Function to parse BgpAttributes. It also parses MP_REACH_NLRI and
   * MP_UNREACH_NLRI information and reflects the changes in parsing state
   * appropriately.
   * @throws: BgpUpdateMsgException on failure
   */
  void parsePathAttributes(
      folly::io::Cursor& cursor,
      const BgpCapabilities& capa);

  /**
   * Perform sanity checks on the UPDATE message state, throw on error
   */
  void doSanityChecks();

}; // struct UpdateMsgParsingState

/**
 * Parse and validate common BGP Header.
 * @returns: parse BGP message header
 * @throws: BgpHeaderException on failure
 */
BgpMessageHeader parseBgpMsgHdr(
    folly::io::Cursor curosr,
    std::optional<BgpMessageType> msgType = std::nullopt);

/**
 * Parse BGP Open message optional parameter Capabilities and populates
 * data in capa on successful parsing.
 * @throws: BgpOpenMsgException on failure
 */
void parseBgpCapabilities(folly::io::Cursor cursor, BgpCapabilities& capa);

} // namespace detail
} // namespace bgplib
} // namespace nettools
} // namespace facebook
