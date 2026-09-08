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

#include <string>

#include <folly/logging/LogLevel.h>
#include <folly/logging/LoggerDB.h>
#include "neteng/fboss/bgp/cpp/lib/BgpException.h"
#include "neteng/fboss/bgp/cpp/lib/BgpMessageParser.h"

using namespace facebook::nettools::bgplib;

namespace {

BgpCapabilities getDefaultCapabilities() {
  BgpCapabilities capabilities;

  capabilities.mpExtV4Unicast() = true;
  capabilities.mpExtV6Unicast() = true;
  capabilities.mpExtV4LU() = true;
  capabilities.mpExtV6LU() = true;
  capabilities.as4byte() = true;

  return capabilities;
}

void parseBgpMessage(const uint8_t* data, size_t len) {
  // Copied from BgpPeerSocket::parseBgpMessage
  if (len <= 18) {
    return;
  }
  uint8_t msgType = data[18];

  auto buf = folly::IOBuf::wrapBufferAsValue(data, len);
  switch (msgType) {
    case BGP_MSG_TYPE_OPEN: {
      auto msg = BgpMessageParser2::parseBgpOpenMsgRaw(buf);
    } break;
    case BGP_MSG_TYPE_UPDATE: {
      auto msg =
          BgpMessageParser2::parseBgpUpdateRaw(buf, getDefaultCapabilities());
    } break;
    case BGP_MSG_TYPE_NOTIFICATION: {
      auto msg = BgpMessageParser2::parseBgpNotificationRaw(buf);
    } break;
    case BGP_MSG_TYPE_KEEPALIVE: {
      BgpMessageParser2::parseBgpKeepAliveRaw(buf);
    } break;
  }
}

} // namespace

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
  (void)argc;
  (void)argv;
  /*
   * Disable logging to prevent I/O-based timeouts during fuzzing.
   * The timeout was caused by excessive synchronous I/O from logging
   * error messages while parsing malformed BGP capabilities.
   */
  folly::LoggerDB::get().setLevel("", folly::LogLevel::FATAL, false);
  return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* Data, size_t Size) {
  try {
    parseBgpMessage(Data, Size);
  } catch (const BgpHeaderException&) {
  } catch (const BgpUpdateMsgException&) {
  } catch (const BgpOpenMsgException&) {
  } catch (const BgpException&) {
  }

  return 0;
}
