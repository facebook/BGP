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

#include <fmt/format.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/Utils.h"

using namespace facebook::nettools::bgplib;

namespace facebook::bgp {

std::optional<size_t> getSwitchId(
    const std::optional<std::string>& deviceName) {
  if (!deviceName) {
    return std::nullopt;
  }
  std::vector<std::string> segments;
  folly::split('.', *deviceName, segments);
  if (segments.size() < 2) {
    return std::nullopt;
  }
  const auto lastNonDigit = segments[0].find_last_not_of("0123456789");
  if (lastNonDigit == segments[0].size() - 1) {
    return std::nullopt;
  }
  const auto switchIdStart =
      lastNonDigit == std::string::npos ? 0 : lastNonDigit + 1;
  try {
    return std::stoul(segments[0].substr(switchIdStart));
  } catch (const std::exception& ex) {
    XLOGF(
        ERR,
        "unable to obtain switchId from deviceName: {}, reason: {}",
        *deviceName,
        ex.what());
    return std::nullopt;
  }
}

int64_t getCurrentTimeMicroSec() {
  auto now = std::chrono::system_clock::now();
  auto value = std::chrono::time_point_cast<std::chrono::microseconds>(now)
                   .time_since_epoch()
                   .count();
  return value;
}

BgpEndOfRib buildEndOfRib(const BgpUpdateAfi& afi) {
  BgpEndOfRib eor;
  eor.isMpEor() = (afi == BgpUpdateAfi::AFI_IPv4) ? false : true;
  eor.afi() = afi;
  eor.safi() = BgpUpdateSafi::SAFI_UNICAST;
  return eor;
}

BgpRouteRefresh buildRouteRefresh(
    const BgpUpdateAfi& afi,
    const BgpRouteRefreshMessageSubtype& subtype,
    const BgpUpdateSafi& safi) {
  BgpRouteRefresh rr;
  rr.afi() = afi;
  rr.msgSubType() = subtype;
  rr.safi() = safi;
  return rr;
}

// return true if subPrefix is a subnet of parentPrefix
bool isSubnet(
    const folly::CIDRNetwork& subPfx,
    const folly::CIDRNetwork& parentPfx) noexcept {
  auto sameProtocal = !(subPfx.first.isV4() ^ parentPfx.first.isV4());
  return sameProtocal &&
      subPfx.first.inSubnet(parentPfx.first, parentPfx.second) &&
      (subPfx.second >= parentPfx.second);
}

folly::Expected<std::pair<uint16_t, uint16_t>, std::string> parseCommunityStr(
    const std::string& commStr) {
  std::vector<uint16_t> parts;
  try {
    folly::split(':', commStr, parts);
  } catch (const std::exception& ex) {
    // This will catch issues like non-numeric values (excluding delimiter :),
    // overflow of uint16_t
    auto message = fmt::format(
        "Invalid community value: {}. Exception message: {}",
        commStr,
        folly::exceptionStr(ex));
    return folly::makeUnexpected(message);
  }
  // This will ensure there are exactly 2 parts i.e. only one : in the input
  if (parts.size() != 2) {
    auto message = fmt::format("Invalid community value: {}.", commStr);
    return folly::makeUnexpected(message);
  }
  return std::make_pair(parts[0], parts[1]);
}

std::pair<bool, bool> getAddPathCapa(
    const std::optional<nettools::bgplib::BgpAddPathSendRec>& capa) {
  if (capa.has_value()) {
    switch (capa.value()) {
      case BgpAddPathSendRec::BOTH:
        return std::make_pair(true, true);
      case BgpAddPathSendRec::SEND:
        return std::make_pair(true, false);
      case BgpAddPathSendRec::RECEIVE:
        return std::make_pair(false, true);
    }
  }
  return std::make_pair(false, false);
}

std::string logRouteWithNexthops(
    const folly::CIDRNetwork& prefix,
    const std::shared_ptr<
        const folly::F14NodeMap<folly::IPAddress, unsigned int>>&
        weightedNexthops) {
  if (weightedNexthops == nullptr) {
    return fmt::format(
        "{} has been withdrawn", folly::IPAddress::networkToString(prefix));
  }
  // weightedNexthops is not nullptr

  // sort the weightedNexthops by ip address:
  // guarantees the order of next hops in each call is the same
  auto ipComparator = [](const folly::IPAddress& lhs,
                         const folly::IPAddress& rhs) {
    return lhs.str() < rhs.str();
  };

  // building map to sort the weightedNexthops with the above comparator
  std::map<folly::IPAddress, unsigned int, decltype(ipComparator)>
      sortedWeightedNexthops(ipComparator);
  sortedWeightedNexthops.insert(
      weightedNexthops->begin(), weightedNexthops->end());

  // build result string prefix
  std::string result = fmt::format(
      "{} has {} path(s):\n",
      folly::IPAddress::networkToString(prefix),
      sortedWeightedNexthops.size());

  std::string nextHopWithWeightStr;
  uint8_t index = 1;
  for (const auto& [nh, weight] : sortedWeightedNexthops) {
    // We construct the next hops with weight string depending on if weight > 0
    if (weight > 0) {
      nextHopWithWeightStr = fmt::format("({}, weight={})", nh.str(), weight);
    } else {
      // We do not print weights that are 0
      nextHopWithWeightStr = fmt::format("({})", nh.str());
    }
    result += fmt::format(
        "next hop {} of {}: {}{}",
        index,
        sortedWeightedNexthops.size(),
        nextHopWithWeightStr,
        index < sortedWeightedNexthops.size()
            ? "\n"
            : ""); // The last line should not add an extra "\n"
    index++;
  }
  return result;
}

void writeFileAtomic(folly::StringPiece path) {
  try {
    XLOGF(DBG1, "Writing exit-in-progress file {}", path);
    folly::writeFileAtomic(path, "");
  } catch (const std::exception& ex) {
    XLOGF(
        ERR,
        "Could not create exit-in-progress file {}. Exception: {}",
        path,
        folly::exceptionStr(ex));
  }
}

std::vector<folly::IPAddress> listAllIPsInCIDR(
    const folly::CIDRNetwork& prefix,
    uint64_t maxIPs) {
  std::vector<folly::IPAddress> ips;

  const auto& [ip, prefixLen] = prefix;
  // Normalize the network address
  const auto baseIP = ip.mask(prefixLen);

  if (baseIP.isV4()) {
    constexpr uint8_t maxPrefix = 32;
    uint64_t numIPs = 1ULL << (maxPrefix - prefixLen);
    if (numIPs > maxIPs) {
      XLOGF(
          DBG2,
          "IP prefix {} contains {} IPs, exceeding limit of {}. Returning empty.",
          folly::IPAddress::networkToString(prefix),
          numIPs,
          maxIPs);
      return ips;
    }
    uint32_t baseAddr = baseIP.asV4().toLongHBO();
    ips.reserve(numIPs);
    for (uint32_t i = 0; i < numIPs; ++i) {
      auto addr = folly::IPAddressV4::fromLongHBO(baseAddr + i);
      ips.emplace_back(addr);
    }
  } else {
    // If prefix length <= 64, the number of IPs would be >= 2^64,
    // which exceeds any possible maxIPs value (2^64 - 1)
    if (prefixLen <= 64) {
      XLOGF(
          DBG2,
          "IPv6 prefix {} with prefix length {} would contain >= 2^64 IPs, exceeding limit of {}. Returning empty.",
          folly::IPAddress::networkToString(prefix),
          prefixLen,
          maxIPs);
      return ips;
    }

    constexpr uint8_t maxPrefix = 128;
    uint64_t numIPs = 1ULL << (maxPrefix - prefixLen);
    if (numIPs > maxIPs) {
      XLOGF(
          DBG2,
          "IP prefix {} contains {} IPs, exceeding limit of {}. Returning empty.",
          folly::IPAddress::networkToString(prefix),
          numIPs,
          maxIPs);
      return ips;
    }
    auto baseBytes = baseIP.asV6().toByteArray();
    ips.reserve(numIPs);
    for (uint64_t i = 0; i < numIPs; ++i) {
      auto currentBytes = baseBytes;
      uint64_t carry = i;
      for (int j = 15; j >= 0 && carry > 0; --j) {
        uint64_t sum = currentBytes[j] + carry;
        currentBytes[j] = sum & 0xFF;
        carry = sum >> 8;
      }

      auto addr = folly::IPAddressV6::fromBinary(
          folly::ByteRange(currentBytes.data(), currentBytes.size()));
      ips.emplace_back(addr);
    }
  }

  return ips;
}

} // namespace facebook::bgp
