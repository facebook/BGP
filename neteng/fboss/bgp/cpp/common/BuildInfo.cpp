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

#include <fmt/format.h>
#include <neteng/fboss/bgp/cpp/common/BuildInfo.h>
#include <neteng/fboss/bgp/cpp/common/CMakeBuildInfo.h>

namespace facebook::bgp {

const char* BuildInfo::getBuildUser() {
  return BuildInfo_kUser;
}

const char* BuildInfo::getBuildTime() {
  return BuildInfo_kTime;
}

uint64_t BuildInfo::getBuildTimeUnix() {
  return BuildInfo_kTimeUnix;
}

const char* BuildInfo::getBuildHost() {
  return BuildInfo_kHost;
}

const char* BuildInfo::getBuildPath() {
  return BuildInfo_kPath;
}

const char* BuildInfo::getBuildRevision() {
  return BuildInfo_kRevision;
}

uint64_t BuildInfo::getBuildRevisionCommitTimeUnix() {
  return BuildInfo_kRevisionCommitTimeUnix;
}

const char* BuildInfo::getBuildUpstreamRevision() {
  return "";
}

uint64_t BuildInfo::getBuildUpstreamRevisionCommitTimeUnix() {
  return 0;
}

const char* BuildInfo::getBuildPackageName() {
  return "";
}

const char* BuildInfo::getBuildPackageVersion() {
  return "";
}

const char* BuildInfo::getBuildPackageRelease() {
  return "";
}

const char* BuildInfo::getBuildPlatform() {
  return BuildInfo_kPlatform;
}

const char* BuildInfo::getBuildRule() {
  return "";
}

const char* BuildInfo::getBuildType() {
  return "";
}

const char* BuildInfo::getBuildTool() {
  return BuildInfo_kBuildTool;
}

const char* BuildInfo::getBuildMode() {
  return BuildInfo_kBuildMode;
}

std::map<std::string, std::string> BuildInfo::toMap() {
  const auto packageName = std::string(getBuildPackageName()).empty()
      ? "bgp"
      : getBuildPackageName();
  const auto packageVersion = std::string(getBuildPackageVersion()).empty()
      ? getBuildRevision()
      : getBuildPackageVersion();
  return {
      {"build_package_name", packageName},
      {"build_package_version", packageVersion},
      {"build_package_info",
       fmt::format("{}-{}", packageName, packageVersion)},
      {"build_user", getBuildUser()},
      {"build_time", getBuildTime()},
      {"build_host", getBuildHost()},
      {"build_path", getBuildPath()},
      {"build_platform", getBuildPlatform()},
      {"build_revision", getBuildRevision()},
  };
}

std::string BuildInfo::toDebugString() {
  return fmt::format(
      "\n  Built by: {}\n"
      "  Built on: {} ({})\n"
      "  Built at: {}\n"
      "  Build tool: {}\n"
      "  Build path: {}\n"
      "  Build Revision: {}\n"
      "  Build Platform: {} ({})",
      getBuildUser(),
      getBuildTime(),
      getBuildTimeUnix(),
      getBuildHost(),
      getBuildTool(),
      getBuildPath(),
      getBuildRevision(),
      getBuildPlatform(),
      getBuildMode());
}

void BuildInfo::log(std::ostream& os) {
  os << toDebugString();
}

void BuildInfo::exportBuildInfo() {
  return;
}

} // namespace facebook::bgp
