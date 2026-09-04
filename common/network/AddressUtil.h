/*
 *  Copyright (c) 2004-present, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#pragma once

#include <folly/IPAddress.h>

#include "common/network/if/gen-cpp2/Address_types.h"

namespace facebook {
namespace network {

namespace detail {

// Trait for V4 vs V6
template <typename T>
struct IPVersion;
template <>
struct IPVersion<folly::IPAddressV4> {
  static const thrift::AddressType value = thrift::AddressType::V4;
};
template <>
struct IPVersion<folly::IPAddressV6> {
  static const thrift::AddressType value = thrift::AddressType::V6;
};

template <class T>
thrift::Address toAddressImpl(const T& addr) {
  thrift::Address result;
  *result.addr() = addr.toFullyQualified();
  *result.type() = IPVersion<T>::value;
  return result;
}

} // namespace detail

inline thrift::Address toAddress(const folly::IPAddress& ip) {
  // clang-format off
  return
      ip.isV4() ? detail::toAddressImpl(ip.asV4()) :
      ip.isV6() ? detail::toAddressImpl(ip.asV6()) :
      thrift::Address();
  // clang-format on
}

template <class IPAddressVx>
thrift::BinaryAddress toBinaryAddressImpl(const IPAddressVx& addr) {
  thrift::BinaryAddress result;
  result.addr()->append(
      reinterpret_cast<const char*>(addr.bytes()), IPAddressVx::byteCount());
  return result;
}

inline thrift::BinaryAddress toBinaryAddress(const folly::IPAddress& addr) {
  // clang-format off
  return
      addr.isV4() ? toBinaryAddressImpl(addr.asV4()) :
      addr.isV6() ? toBinaryAddressImpl(addr.asV6()) :
      thrift::BinaryAddress();
  // clang-format on
}

inline folly::IPAddress toIPAddress(const thrift::Address& input) {
  return *input.type() != thrift::AddressType::VUNSPEC
      ? folly::IPAddress(*input.addr())
      : folly::IPAddress();
}

inline folly::Expected<folly::IPAddress, folly::IPAddressFormatError>
tryToIPAddress(const thrift::Address& input) {
  return folly::IPAddress::tryFromString(*input.addr());
}

inline folly::IPAddress toIPAddress(const thrift::BinaryAddress& addr) {
  return folly::IPAddress::fromBinary(
      folly::ByteRange(folly::StringPiece(*addr.addr())));
}

inline folly::Expected<folly::IPAddress, folly::IPAddressFormatError>
tryToIPAddress(const thrift::BinaryAddress& addr) {
  return folly::IPAddress::tryFromBinary(
      folly::ByteRange(folly::StringPiece(*addr.addr())));
}

/**
 * Convert a IPPrefix thrift struct to CIDR prefix.
 * If the `mask` is set, the IP address will be masked by the prefix length.
 */
inline folly::CIDRNetwork toCIDRNetwork(
    const thrift::IPPrefix& prefix,
    bool mask = false) {
  auto ip = folly::IPAddress::fromBinary(
      folly::ByteRange(folly::StringPiece(*prefix.prefixAddress()->addr())));
  auto maskLen = static_cast<uint8_t>(*prefix.prefixLength());

  return {mask ? ip.mask(maskLen) : ip, maskLen};
}

inline folly::Expected<folly::CIDRNetwork, folly::IPAddressFormatError>
tryToCIDRNetwork(const thrift::IPPrefix& prefix, bool mask = false) {
  auto maybeIp = folly::IPAddress::tryFromBinary(
      folly::ByteRange(folly::StringPiece(*prefix.prefixAddress()->addr())));
  if (maybeIp) {
    auto maskLen = static_cast<uint8_t>(*prefix.prefixLength());
    return folly::CIDRNetwork{
        mask ? (*maybeIp).mask(maskLen) : *maybeIp, maskLen};
  } else {
    return folly::makeUnexpected(maybeIp.error());
  }
}

inline thrift::IPPrefix toIPPrefix(const folly::CIDRNetwork& network) {
  thrift::IPPrefix result;
  *result.prefixAddress() = toBinaryAddress(network.first);
  *result.prefixLength() = network.second;
  return result;
}

inline thrift::IPPrefix toIPPrefix(
    const std::string& address,
    uint32_t masklen) {
  thrift::IPPrefix result;
  *result.prefixAddress() = toBinaryAddress(folly::IPAddress(address));
  *result.prefixLength() = masklen;
  return result;
}

} /* namespace network */
} /* namespace facebook */
