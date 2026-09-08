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

#include <optional>
#include <utility>

#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>

#include "neteng/fboss/bgp/cpp/lib/BgpMessageParser.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberSocket.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook {
namespace nettools {
namespace bgplib {

/*
 * This is a filter that consumes IOBufs and emits BGP messages
 * as they become fully available and parsable. Empty value on
 * input would stop the filter. There is no exceptions, everything
 * is propagated via folly::Try<>
 */
class FiberBgpParser {
 public:
  using BgpMessageT = std::variant<
      BgpOpenMsg,
      BgpNotification,
      BgpKeepAlive,
      std::shared_ptr<const BgpUpdate2>,
      UpdateDescriptor,
      BgpEndOfRib,
      FiberSocketError,
      BgpRouteRefresh>;

  FiberBgpParser() = delete;
  // movable
  FiberBgpParser(FiberBgpParser&&) = default;
  FiberBgpParser& operator=(FiberBgpParser&&) = default;

  /*
   * read IOBufs from rqueue until we have enough data to process
   * then emit new message into WQueue. Pass error along the pipeline
   * if the parser encounters any
   */
  FiberBgpParser(
      const BgpCapabilities& myCaps,
      MonitoredBackPressuredQueue<std::optional<folly::Try<BgpMessageT>>>&
          wqueue);

  std::size_t getBufCapaticy() {
    return buf_ ? buf_->capacity() : 0;
  }

  // util function to process buf, aka, folly::IOBuf
  void processBgpMsgBuf(std::unique_ptr<folly::IOBuf> buf);

  class FiberParserCallbacks
      : public BgpMessageParser2::BgpMessageParserCallbacks {
   private:
    BgpCapabilities& caps_;
    MonitoredBackPressuredQueue<std::optional<folly::Try<BgpMessageT>>>&
        wqueue_;

    /*
     * Parse errors are recorded here by the handle*Exception callbacks (which
     * run inside the parser's catch block) and pushed onto wqueue_ by
     * processBgpMsgBuf after the catch unwinds. A fiber must never suspend
     * (e.g. on a backpressured fiberPush) while an exception is in flight --
     * folly::fibers::Fiber::preempt CHECK-fails in that case.
     */
    std::optional<folly::Try<BgpMessageT>> pendingError_;

   public:
    // Hand off (and clear) any error recorded during the last parse call.
    std::optional<folly::Try<BgpMessageT>> takePendingError() {
      return std::exchange(pendingError_, std::nullopt);
    }

    FiberParserCallbacks(
        BgpCapabilities& caps,
        MonitoredBackPressuredQueue<std::optional<folly::Try<BgpMessageT>>>&
            wqueue);
    void rcvdBgpOpenMsg(BgpOpenMsg) override;
    void rcvdBgpNotification(BgpNotification) override;
    void rcvdBgpKeepAlive() override;
    void rcvdBgpUpdate(const std::variant<BgpUpdate2, BgpEndOfRib>&) {}
    void rcvdBgpUpdate(BgpUpdate2) override;
    void rcvdBgpEndOfRib(BgpEndOfRib) override;
    void rcvdBgpRouteRefresh(BgpRouteRefresh) override;
    void handleBgpException(const BgpException&) override;
    void handleBgpFsmException(const BgpFsmException&) override;
    void handleBgpHeaderException(const BgpHeaderException&) override;
    void handleBgpOpenMsgException(const BgpOpenMsgException&) override;
    void handleBgpUpdateMsgException(const BgpUpdateMsgException&) override;
    void handleBgpRouteRefreshMsgException(
        const BgpRouteRefreshMsgException&) override;
    void handleException(const std::exception&);
  };

 private:
  // non-copyable
  FiberBgpParser(FiberBgpParser const&) = delete;
  FiberBgpParser& operator=(FiberBgpParser const&) = default;

  /*
   * we use the buffer to accumulate read data blocks before we
   * parse a message.
   * we take buffers by unique_ptr since many methods in IOBuf
   * take unique_ptr most of the time
   */
  std::unique_ptr<folly::IOBuf> buf_;

  const BgpCapabilities myCaps_; // Configured capabilities
  BgpCapabilities negotiatedCaps_; // negotiated & applicable capabilities

  MonitoredBackPressuredQueue<std::optional<folly::Try<BgpMessageT>>>& wqueue_;
  FiberParserCallbacks parserCb_;

#ifdef FiberBgpParser_TEST_FRIENDS
  FiberBgpParser_TEST_FRIENDS
#endif
};

} // namespace bgplib
} // namespace nettools
} // namespace facebook
