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
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include <gtest/gtest.h>

namespace facebook::bgp::test {

template <typename Condition>
void checkWithRetry(
    Condition condition,
    int retries = 10,
    std::chrono::duration<uint32_t, std::milli> timeBetweenRetries =
        std::chrono::seconds(1),
    std::optional<std::string> failureMessage = std::nullopt,
    bool retryOnException = false) {
  while (retries-- > 0) {
    try {
      if (condition()) {
        return;
      }
    } catch (...) {
      if (!retryOnException) {
        throw;
      }
    }
    // Polling arbitrary conditions provides no event or future to await.
    // Preserve CommonUtils::checkWithRetry semantics by sleeping after every
    // failed attempt, including the final attempt before reporting failure.
    // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_for)
    std::this_thread::sleep_for(timeBetweenRetries);
  }

  constexpr auto kFailureMessage = "Retry condition was never satisfied";
  throw std::runtime_error(
      failureMessage ? kFailureMessage + std::string(": ") + *failureMessage
                     : kFailureMessage);
}

namespace detail {
struct SoftAssertFailure {};
} // namespace detail

} // namespace facebook::bgp::test

#define WITH_RETRIES_N_TIMED(maxRetries, sleepTime, ...)                  \
  {                                                                       \
    int fbossBgpRetryMacroAttempt = 0;                                    \
    while (fbossBgpRetryMacroAttempt++ < (maxRetries)) {                  \
      if (fbossBgpRetryMacroAttempt != 1) {                               \
        /* Retry polling has no event or future to await. */              \
        std::this_thread::sleep_for(                                      \
            sleepTime); /* NOLINT(facebook-hte-BadCall-sleep_for) */      \
      }                                                                   \
      [[maybe_unused]] bool fbossBgpRetryMacroSoftTest =                  \
          fbossBgpRetryMacroAttempt != (maxRetries);                      \
      bool fbossBgpRetryMacroPassed = true;                               \
      try {                                                               \
        __VA_ARGS__;                                                      \
      } catch (const ::facebook::bgp::test::detail::SoftAssertFailure&) { \
        continue;                                                         \
      }                                                                   \
      if (fbossBgpRetryMacroPassed) {                                     \
        break;                                                            \
      }                                                                   \
    }                                                                     \
  }

#define WITH_RETRIES_N(maxRetries, ...) \
  WITH_RETRIES_N_TIMED(maxRetries, std::chrono::milliseconds(1000), __VA_ARGS__)

#define WITH_RETRIES(...) WITH_RETRIES_N(30, __VA_ARGS__)

#define BGP_ASSERT_EVENTUALLY(softTest, hardTest)               \
  GTEST_AMBIGUOUS_ELSE_BLOCKER_                                 \
  if (fbossBgpRetryMacroSoftTest) {                             \
    if (!(softTest)) {                                          \
      throw ::facebook::bgp::test::detail::SoftAssertFailure(); \
    }                                                           \
  } else                                                        \
    hardTest

#define BGP_EXPECT_EVENTUALLY(softTest, hardTest) \
  GTEST_AMBIGUOUS_ELSE_BLOCKER_                   \
  if (fbossBgpRetryMacroSoftTest) {               \
    fbossBgpRetryMacroPassed &= (softTest);       \
  } else                                          \
    hardTest

#define ASSERT_EVENTUALLY_TRUE(expr) \
  BGP_ASSERT_EVENTUALLY(static_cast<bool>(expr), ASSERT_TRUE(expr))
#define ASSERT_EVENTUALLY_FALSE(expr) \
  BGP_ASSERT_EVENTUALLY(!(expr), ASSERT_FALSE(expr))
#define ASSERT_EVENTUALLY_EQ(expr1, expr2) \
  BGP_ASSERT_EVENTUALLY((expr1) == (expr2), ASSERT_EQ(expr1, expr2))
#define ASSERT_EVENTUALLY_NE(expr1, expr2) \
  BGP_ASSERT_EVENTUALLY((expr1) != (expr2), ASSERT_NE(expr1, expr2))
#define ASSERT_EVENTUALLY_GT(expr1, expr2) \
  BGP_ASSERT_EVENTUALLY((expr1) > (expr2), ASSERT_GT(expr1, expr2))
#define ASSERT_EVENTUALLY_GE(expr1, expr2) \
  BGP_ASSERT_EVENTUALLY((expr1) >= (expr2), ASSERT_GE(expr1, expr2))

#define EXPECT_EVENTUALLY_TRUE(expr) \
  BGP_EXPECT_EVENTUALLY(static_cast<bool>(expr), EXPECT_TRUE(expr))
#define EXPECT_EVENTUALLY_FALSE(expr) \
  BGP_EXPECT_EVENTUALLY(!(expr), EXPECT_FALSE(expr))
#define EXPECT_EVENTUALLY_EQ(expr1, expr2) \
  BGP_EXPECT_EVENTUALLY((expr1) == (expr2), EXPECT_EQ(expr1, expr2))
#define EXPECT_EVENTUALLY_NE(expr1, expr2) \
  BGP_EXPECT_EVENTUALLY((expr1) != (expr2), EXPECT_NE(expr1, expr2))
#define EXPECT_EVENTUALLY_GT(expr1, expr2) \
  BGP_EXPECT_EVENTUALLY((expr1) > (expr2), EXPECT_GT(expr1, expr2))
#define EXPECT_EVENTUALLY_GE(expr1, expr2) \
  BGP_EXPECT_EVENTUALLY((expr1) >= (expr2), EXPECT_GE(expr1, expr2))

#define CHECK_HOLDS_FOR_DURATION_TIMED(duration, sleepTime, predicate) \
  {                                                                    \
    auto fbossBgpCheckHoldsMacroDeadline =                             \
        std::chrono::steady_clock::now() + (duration);                 \
    while (true) {                                                     \
      bool fbossBgpCheckHoldsMacroPassed = (predicate)();              \
      EXPECT_TRUE(fbossBgpCheckHoldsMacroPassed)                       \
          << "CHECK_HOLDS_FOR_DURATION predicate returned false";      \
      if (!fbossBgpCheckHoldsMacroPassed ||                            \
          std::chrono::steady_clock::now() >=                          \
              fbossBgpCheckHoldsMacroDeadline) {                       \
        break;                                                         \
      }                                                                \
      /* Retry polling has no event or future to await. */             \
      std::this_thread::sleep_for(                                     \
          sleepTime); /* NOLINT(facebook-hte-BadCall-sleep_for) */     \
    }                                                                  \
  }

#define CHECK_HOLDS_FOR_DURATION(duration, predicate) \
  CHECK_HOLDS_FOR_DURATION_TIMED(                     \
      duration, std::chrono::milliseconds(100), predicate)
