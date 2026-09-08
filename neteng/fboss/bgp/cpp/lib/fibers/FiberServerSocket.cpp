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

#include <folly/fibers/Promise.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/lib/fibers/FiberServerSocket.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/Utils.h"

using namespace folly::fibers;
namespace {

/*
 * accept returns the fd and the client's socket address if completed
 * successfully.
 */
struct AcceptResult {
  int fd;
  folly::SocketAddress clientAddr;
  // indicate accept() failed due to closing of socket by another fiber.
  bool interrupted{false};
};

class AcceptCallback : public folly::AsyncServerSocket::AcceptCallback {
 public:
  explicit AcceptCallback(std::shared_ptr<folly::AsyncServerSocket> socket)
      : socket_(std::move(socket)) {}

  ~AcceptCallback() override = default;

  void setPromise(Promise<AcceptResult>&& promise) {
    promise_ = std::move(promise);
  }

 private:
  /*
   * Invariant state
   */

  const std::shared_ptr<folly::AsyncServerSocket> socket_;

  // the promise to pass data from the callback
  std::optional<Promise<AcceptResult>> promise_;

  /*
   * Indicate connectionAcceped() or acceptError() has not completed yet.
   * Used by acceptStopped() to determine to throw an exception or not.
   */
  bool callbackStarted_{false};

  /*
   * AcceptCallback methods
   */

  void connectionAccepted(
      folly::NetworkSocket fdNetworkSocket,
      const folly::SocketAddress& clientAddr,
      AcceptInfo /* info */) noexcept override {
    int fd = fdNetworkSocket.toFd();

    XLOGF(
        DBG5,
        "Connection accepted from [{}]:{}",
        clientAddr.getAddressStr(),
        clientAddr.getPort());
    callbackStarted_ = true;
    SCOPE_EXIT {
      callbackStarted_ = false;
    };
    // unregister handlers while in the callback
    socket_->pauseAccepting();
    socket_->removeAcceptCallback(this, nullptr);
    AcceptResult acceptResult;
    acceptResult.fd = fd;
    acceptResult.clientAddr = clientAddr;
    promise_->setValue(acceptResult);
  }

  void acceptError(folly::exception_wrapper ex) noexcept override {
    XLOG(DBG5, "acceptError");
    callbackStarted_ = true;
    SCOPE_EXIT {
      callbackStarted_ = false;
    };
    // unregister handlers while in the callback
    socket_->pauseAccepting();
    socket_->removeAcceptCallback(this, nullptr);
    promise_->setException(std::move(ex));
  }

  void acceptStarted() noexcept override {}

  void acceptStopped() noexcept override {
    XLOG(DBG5, "acceptStopped");
    if (!callbackStarted_) {
      AcceptResult acceptResult;
      acceptResult.interrupted = true;
      promise_->setValue(acceptResult);
    }
  }

  /*
   * End AcceptCallback methods
   */
};

} // namespace

namespace facebook {
namespace nettools {
namespace bgplib {

FiberServerSocket::FiberServerSocket(
    std::optional<folly::SocketAddress> addr,
    uint32_t listenQueueDepth) {
  CHECK(folly::fibers::onFiber())
      << "Attempt to create FiberServerSocket not on fiber!";

  auto evb = getFiberEventBase();

  socket_ = folly::AsyncServerSocket::newSocket(evb);
  socket_->setReusePortEnabled(true);

  if (addr.has_value()) {
    XLOGF(
        DBG2,
        "FiberServerSocket binds on [{}]:{}",
        addr->getAddressStr(),
        addr->getPort());
    socket_->bind(*addr);
  } else {
    XLOG(DBG2, "FiberServerSocket binds on any addr, random port");
    socket_->bind(0);
  }
  socket_->listen(listenQueueDepth);
}

FiberServerSocket::~FiberServerSocket() {
  close();
}

// accept and return new FiberSocket
folly::Expected<FiberSocket, FiberSocketError>
FiberServerSocket::accept() noexcept {
  auto evb = getFiberEventBase();

  AcceptCallback cb(socket_);

  XLOG(DBG5, "accept() called");

  try {
    auto result = await([&cb, this](Promise<AcceptResult> promise) {
      cb.setPromise(std::move(promise));
      /*
       * here we rely on AsyncServerSocket (ASS) to properly
       * dispatch the callback from its primary event base.
       * By default, it uses notification queue's to talk
       * to other threads, but we need to avoid this, to be
       * able to stop/unregister callbacks on first hit.
       */
      socket_->addAcceptCallback(&cb, nullptr);
      // only dispatch signal callback to us
      socket_->setMaxAcceptAtOnce(1);
      socket_->startAccepting();
    });
    if (result.interrupted) {
      return folly::makeUnexpected<FiberSocketError>(FiberGenericSocketError{
          FiberGenericSocketErrorType::ACCEPT_STOPPED, "accept() stopped"});
    }
    const int fd = result.fd;
    return FiberSocket(
        folly::AsyncSocket::newSocket(evb, folly::NetworkSocket::fromFd(fd)));
  } catch (folly::AsyncSocketException const& ex) {
    // catch AsyncSocket exceptions
    return folly::makeUnexpected<FiberSocketError>(ex);
  } catch (std::exception const& ex) {
    // catch Standard exceptions besides ones raised by AsyncSocket
    return folly::makeUnexpected<FiberSocketError>(FiberGenericSocketError{
        FiberGenericSocketErrorType::UNKNOWN, ex.what()});
  } catch (...) {
    // catch all other remaining exceptions
    return folly::makeUnexpected<FiberSocketError>(FiberGenericSocketError{
        FiberGenericSocketErrorType::UNKNOWN, "Unknown exception"});
  }
}

folly::SocketAddress FiberServerSocket::getListenAddress() const noexcept {
  folly::SocketAddress listenAddress;
  socket_->getAddress(&listenAddress);
  return listenAddress;
}

std::vector<folly::SocketAddress> FiberServerSocket::getListenAddresses()
    const noexcept {
  return socket_->getAddresses();
}

int FiberServerSocket::setSockOpt(
    int level,
    int optname,
    const void* optval,
    socklen_t optlen) const noexcept {
  /*
   * AsyncServerSocket has a list of sockets underneath, set
   * same socket option on all the sockets.
   */
  for (auto& netsock : socket_->getNetworkSockets()) {
    if (netsock == folly::NetworkSocket()) {
      continue;
    }
    auto ret = setsockopt(netsock.toFd(), level, optname, optval, optlen);
    if (ret) {
      return ret;
    }
  }
  return 0;
}

void FiberServerSocket::close() noexcept {
  /*
   * shutdown listening socket
   * socket_ might be null if this object has been moved from
   */
  if (socket_) {
    socket_->stopAccepting();
  }
}

} // namespace bgplib
} // namespace nettools
} // namespace facebook
