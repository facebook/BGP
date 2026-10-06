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

#include <folly/Try.h>
#include <folly/coro/Coroutine.h>
#include <folly/coro/Task.h>
#include <folly/executors/InlineExecutor.h>
#include <folly/futures/Future.h>

namespace facebook::bgp {

static constexpr std::chrono::seconds kRibThriftHandlerTimeout{30};
static constexpr std::chrono::seconds kPeerMgrThriftHandlerTimeout{30};
inline constexpr std::chrono::seconds kSessionMgrThriftHandlerTimeout{30};

/**
 * Run a callable on a target EventBase with a timeout.
 *
 * Chain: via(&evb, fn).semi().within(timeout).via(&InlineExecutor)
 * - .semi() detaches from the evb, producing a SemiFuture
 * - .within() on SemiFuture uses the global Timekeeper (not evb's
 *   timer wheel), so the timeout fires even if evb is stuck
 * - .via(&InlineExecutor) ensures co_await resumes inline
 *
 * Returns folly::Try<T> via co_awaitTry so callers can distinguish
 * success from timeout without catching exceptions.
 */
template <typename F, typename Rep, typename Period>
folly::coro::Task<folly::Try<std::invoke_result_t<F>>> co_runOnEvbWithTimeout(
    folly::EventBase& evb,
    F&& fn,
    std::chrono::duration<Rep, Period> timeout) {
  co_return co_await folly::coro::co_awaitTry(
      folly::via(&evb, std::forward<F>(fn))
          .semi()
          .within(timeout)
          .via(&folly::InlineExecutor::instance()));
}

} // namespace facebook::bgp
