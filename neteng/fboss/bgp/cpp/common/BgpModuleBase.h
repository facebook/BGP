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

#include <boost/noncopyable.hpp>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>

namespace facebook::bgp {

/*
 * Helper macro to create a coroutine function based on a function in
 * BgpModuleBase. The coroutine would be scheduled on evb_.
 */
#define MAKE_CORO_FUNCTION(function)                                        \
  template <typename... Args>                                               \
  auto co_##function(Args&&... args)                                        \
      -> folly::coro::Task<decltype(function(std::declval<Args>()...))> {   \
    auto task = [&]()                                                       \
        -> folly::coro::Task<decltype(function(std::declval<Args>()...))> { \
      co_return function(std::forward<Args>(args)...);                      \
    };                                                                      \
    co_return co_await co_withExecutor(&evb_, task());                      \
  }

/*
 * [Class for Bgp Modules]
 *
 * The base class for bgp modules, which would provide some basic monitoring
 */
class BgpModuleBase : public boost::noncopyable {
 public:
  explicit BgpModuleBase(std::string moduleName) noexcept
      : moduleName_(std::move(moduleName)) {}

  virtual ~BgpModuleBase() = default;

  virtual void run() noexcept = 0;
  virtual void stop() noexcept = 0;

  // Run evb_ continuously in a thread
  std::thread runInThread();

  const std::string& getModuleName() const noexcept {
    return moduleName_;
  }

  inline folly::EventBase& getEventBase() {
    return evb_;
  }

 protected:
  const std::string moduleName_;

  folly::EventBase evb_;
  folly::coro::CancellableAsyncScope asyncScope_;

 private:
  /**
   * @brief Schedule the heartbeat loop on evb_
   */
  virtual void scheduleHeartbeatLoop() noexcept;

  /**
   * @brief Coroutine to periodically update heartbeat counter
   */
  folly::coro::Task<void> heartbeatLoop() noexcept;

/*
 * per class placeholder for test code injection
 * only need to be setup once here
 */
#ifdef BgpModuleBase_TEST_FRIENDS
  BgpModuleBase_TEST_FRIENDS
#endif
};

} // namespace facebook::bgp
