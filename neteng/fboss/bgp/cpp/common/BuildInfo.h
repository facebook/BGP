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

#include <inttypes.h>
#include <map>
#include <ostream>
#include <string>

namespace facebook::bgp {

/**
 * Placeholder class for encoding build information into binary.
 * Follows the same pattern as openr::BuildInfo.
 */
class BuildInfo {
 public:
  static const char* getBuildUser();
  static const char* getBuildTime();
  static uint64_t getBuildTimeUnix();
  static const char* getBuildHost();
  static const char* getBuildPath();
  static const char* getBuildRevision();
  static uint64_t getBuildRevisionCommitTimeUnix();
  static const char* getBuildUpstreamRevision();
  static uint64_t getBuildUpstreamRevisionCommitTimeUnix();
  static const char* getBuildPackageName();
  static const char* getBuildPackageVersion();
  static const char* getBuildPackageRelease();
  static const char* getBuildPlatform();
  static const char* getBuildRule();
  static const char* getBuildType();
  static const char* getBuildTool();
  static const char* getBuildMode();

  /** Returns build metadata using the fb303-compatible build_* keys. */
  static std::map<std::string, std::string> toMap();

  /**
   * Returns a formatted debug string with all build information.
   * Used for gflags version string (--version output).
   */
  static std::string toDebugString();

  /** Log build info to the given output stream. */
  static void log(std::ostream& os);

  /** Export build information to monitoring systems. */
  static void exportBuildInfo();

 private:
  BuildInfo() {}
};

} // namespace facebook::bgp
