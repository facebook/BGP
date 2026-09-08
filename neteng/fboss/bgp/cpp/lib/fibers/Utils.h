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

#include <chrono>
#include <optional>

#include <folly/IPAddress.h>
#include <folly/SocketAddress.h>
#include <folly/fibers/FiberManagerMap.h>
#include <folly/io/SocketOptionMap.h>
#include <folly/io/async/EventBase.h>

namespace facebook {
namespace nettools {
namespace bgplib {

/*
 * Constant time for the max waiting time before forcing a socket closure
 */
constexpr auto kSocketCloseWaitingTime = std::chrono::milliseconds(1000);

/*
 * Default jitter parameters for generateJitter()
 */
constexpr long kDefaultJitterPercent = 10;
constexpr long kDefaultJitterCapMs = 1000;

int64_t getCurrentTimeMs();

/*
 * Must be invoked inside fiber; returns the event
 * base currently used by the fiber manager
 */
folly::EventBase* getFiberEventBase();

/*
 * Suspend calling fiber for given duration using AsyncTimeout
 */
void fiberSleepFor(std::chrono::milliseconds duration);

/*
 * Get FiberManager options with custom setting
 */
folly::fibers::FiberManager::Options getFiberManagerOptions(
    const uint32_t stackSizeKB = 32);

/*
 * Use for converting IPv4 mapped IPv6 address to IPv4 for display
 */
std::string getAddressStr(folly::SocketAddress addr);

/*
 * Generate a jitter value in range (-jitterMax, +jitterMax]
 * where jitterMax = min(capMs, timeMs * jitterPercent / 100)
 * Uses folly::Random for proper entropy-based seeding.
 */
long generateJitter(
    long timeMs,
    long jitterPercent = kDefaultJitterPercent,
    long capMs = kDefaultJitterCapMs);

/*
 * True if the address is IPv6 (not IPv4 and not IPv4-mapped IPv6).
 */
inline bool isV6Peer(const folly::IPAddress& addr) {
  return !(addr.isV4() || addr.isIPv4Mapped());
}

/*
 * Build base socket option map for BGP sessions.
 * Sets TOS/TCLASS to CS6, optionally disables jumbo frames.
 */
folly::SocketOptionMap getSockOptions(bool isV6, bool disableJumboFrame);

/*
 * Build GTSM (RFC 5082) socket option map.
 * Sets outbound TTL to 255 and minimum acceptable inbound TTL.
 * Returns empty map if ttlSecurityHops is nullopt.
 */
folly::SocketOptionMap getGtsmSockOptions(
    bool isV6,
    std::optional<int32_t> ttlSecurityHops);

} // namespace bgplib
} // namespace nettools
} // namespace facebook
