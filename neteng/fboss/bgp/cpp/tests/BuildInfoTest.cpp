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

#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/common/BuildInfo.h"

using namespace facebook::bgp;

TEST(BuildInfoTest, ToDebugStringContainsBuildValues) {
  std::string debug = BuildInfo::toDebugString();
  EXPECT_NE(std::string::npos, debug.find(BuildInfo::getBuildUser()));
  EXPECT_NE(std::string::npos, debug.find(BuildInfo::getBuildHost()));
  EXPECT_NE(std::string::npos, debug.find(BuildInfo::getBuildRevision()));
  EXPECT_NE(std::string::npos, debug.find(BuildInfo::getBuildPlatform()));
  EXPECT_NE(std::string::npos, debug.find(BuildInfo::getBuildRule()));
}

TEST(BuildInfoTest, ToDebugStringContainsFieldLabels) {
  std::string debug = BuildInfo::toDebugString();
  EXPECT_NE(std::string::npos, debug.find("Built by"));
  EXPECT_NE(std::string::npos, debug.find("Built on"));
  EXPECT_NE(std::string::npos, debug.find("Build Revision"));
  EXPECT_NE(std::string::npos, debug.find("Build Platform"));
}

TEST(BuildInfoTest, LogWritesFieldLabelsToStream) {
  std::ostringstream oss;
  BuildInfo::log(oss);
  std::string output = oss.str();
  EXPECT_FALSE(output.empty());
  EXPECT_NE(std::string::npos, output.find("Built by"));
  EXPECT_NE(std::string::npos, output.find("Build Revision"));
}

TEST(BuildInfoTest, ToMapContainsCliVersionFields) {
  const auto info = BuildInfo::toMap();

  EXPECT_EQ(info.at("build_package_name"), "bgp");
  EXPECT_EQ(
      info.at("build_package_version"), BuildInfo::getBuildRevision());
  EXPECT_EQ(info.at("build_revision"), BuildInfo::getBuildRevision());
  EXPECT_EQ(info.at("build_user"), BuildInfo::getBuildUser());
  EXPECT_EQ(info.at("build_platform"), BuildInfo::getBuildPlatform());
}
