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

#include <queue>

#include <folly/fibers/Promise.h>
#include <folly/io/async/AsyncServerSocket.h>

#include "neteng/fboss/bgp/cpp/lib/fibers/FiberSocket.h"

namespace facebook {
namespace nettools {
namespace bgplib {

/*
 * Creates AsyncServerSocket internally and handles
 * the accept() calls producing new FiberSockets
 * for every accepted connection
 */

class FiberServerSocket {
 public:
  // movable
  FiberServerSocket(FiberServerSocket&&) = default;
  FiberServerSocket& operator=(FiberServerSocket&&) = default;

  /*
   * create and bind server socket, return server socket ready
   * to accept connections; if the bind address is omitted, listen
   * on any address with random port (useful for testing)
   */
  explicit FiberServerSocket(
      std::optional<folly::SocketAddress> bindAddr,
      uint32_t listenQueueDepth = 256);

  /*
   * we need to shutdown the accepting socket when cleaning, so
   * override default d-tor
   */
  virtual ~FiberServerSocket();

  // accept next pending connection and return new FiberSocket
  virtual folly::Expected<FiberSocket, FiberSocketError> accept() noexcept;

  /*
   * get the listening address of this server - useful for unittessting,
   * when we bind to a random port
   */
  folly::SocketAddress getListenAddress() const noexcept;

  // return all bound listen addresses
  std::vector<folly::SocketAddress> getListenAddresses() const noexcept;

  // Set sock options transparently on the listening socket
  int setSockOpt(int level, int optname, const void* optval, socklen_t optlen)
      const noexcept;

  void close() noexcept;

 private:
  // non-copyable
  FiberServerSocket(const FiberServerSocket&) = delete;
  FiberServerSocket& operator=(const FiberServerSocket&) = delete;

  /*
   * Invariant state
   */

  // non-cost cause we create this during construction :(
  std::shared_ptr<folly::AsyncServerSocket> socket_;
};

} // namespace bgplib
} // namespace nettools
} // namespace facebook
