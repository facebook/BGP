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

#include <folly/Function.h>
#include <folly/logging/xlog.h>

#include <string>

#include "neteng/fboss/bgp/cpp/common/FeatureFlags.h"

namespace facebook::neteng::fboss::bgp::changetracker {

/**
 * Debug facility for ChangeTracker library.
 * Provides exhaustive tracing with zero performance impact when disabled.
 */
class ChangeTrackerDebug {
 public:
  /*
   * Callback type for consumer display - consumer provides this to display
   * itself
   */
  using ConsumerDisplayCallback = folly::Function<std::string()>;

  /**
   * Check if debug is enabled - inlined for zero cost when disabled.
   */
  static inline bool isDebugEnabled() {
    return ::facebook::bgp::FeatureFlags::IsFeatureEnabled(
        "change_list_tracker_debug");
  }
};

} // namespace facebook::neteng::fboss::bgp::changetracker

// Macro for debug logging with zero performance impact when disabled
#define CT_DEBUG_LOG(level, fmt, ...)                       \
  do {                                                      \
    if (UNLIKELY(                                           \
            ::facebook::neteng::fboss::bgp::changetracker:: \
                ChangeTrackerDebug::isDebugEnabled())) {    \
      XLOGF(level, fmt, ##__VA_ARGS__);                     \
    }                                                       \
  } while (0)
