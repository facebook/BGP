# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if (NOT DEFINED BGP_SOURCE_ROOT)
  message(FATAL_ERROR "BGP_SOURCE_ROOT is required")
endif()

set(BGP_PUBLIC_BB_FILES
  neteng/fboss/bgp/cpp/BgpServiceBB.cpp
  neteng/fboss/bgp/cpp/BgpServiceBB.h
  neteng/fboss/bgp/cpp/common/platform/bb/PlatformConstant.h
  neteng/fboss/bgp/cpp/health/HealthValidatorBB.cpp
  neteng/fboss/bgp/cpp/health/HealthValidatorBB.h
  neteng/fboss/bgp/cpp/nexthopTracker/NetlinkWrapper.cpp
  neteng/fboss/bgp/cpp/nexthopTracker/NetlinkWrapper.h
  neteng/fboss/bgp/cpp/peer/PeerManagerBB.cpp
  neteng/fboss/bgp/cpp/peer/PeerManagerBB.h
  neteng/fboss/bgp/cpp/rib/FibEbb.cpp
  neteng/fboss/bgp/cpp/rib/FibEbb.h
  neteng/fboss/bgp/cpp/rib/RibBB.cpp
  neteng/fboss/bgp/cpp/rib/RibBB.h
  neteng/fboss/bgp/cpp/stats/StatsBB.cpp
  neteng/fboss/bgp/cpp/stats/StatsBB.h
)

foreach (BGP_PUBLIC_BB_FILE IN LISTS BGP_PUBLIC_BB_FILES)
  set(BGP_PUBLIC_BB_PATH "${BGP_SOURCE_ROOT}/${BGP_PUBLIC_BB_FILE}")
  if (NOT EXISTS "${BGP_PUBLIC_BB_PATH}")
    message(FATAL_ERROR "Missing OSS BB source: ${BGP_PUBLIC_BB_FILE}")
  endif()

  file(READ "${BGP_PUBLIC_BB_PATH}" BGP_PUBLIC_BB_CONTENT)
  string(FIND
    "${BGP_PUBLIC_BB_CONTENT}"
    "Confidential and proprietary"
    BGP_PROPRIETARY_HEADER_OFFSET
  )
  if (NOT BGP_PROPRIETARY_HEADER_OFFSET EQUAL -1)
    message(FATAL_ERROR "Proprietary license header: ${BGP_PUBLIC_BB_FILE}")
  endif()
endforeach()

set(BGP_CMAKE_FILES
  CMakeLists.txt
  cmake/BgpCommon.cmake
  cmake/BgpDC.cmake
  cmake/BgpBB.cmake
)

set(BGP_FORBIDDEN_CMAKE_PATHS
  "cpp/facebook/BgpServiceBB"
  "cpp/health/facebook/HealthValidatorBB"
  "cpp/peer/facebook/PeerManagerBB"
  "cpp/rib/facebook/FibEbb"
  "public_tld/openr"
  "openr/common/"
  "openr/if/"
  "openr/monitor/"
)

foreach (BGP_CMAKE_FILE IN LISTS BGP_CMAKE_FILES)
  set(BGP_CMAKE_PATH "${BGP_SOURCE_ROOT}/${BGP_CMAKE_FILE}")
  if (NOT EXISTS "${BGP_CMAKE_PATH}")
    message(FATAL_ERROR "Missing OSS CMake file: ${BGP_CMAKE_FILE}")
  endif()

  file(READ "${BGP_CMAKE_PATH}" BGP_CMAKE_CONTENT)
  foreach (BGP_FORBIDDEN_CMAKE_PATH IN LISTS BGP_FORBIDDEN_CMAKE_PATHS)
    string(FIND
      "${BGP_CMAKE_CONTENT}"
      "${BGP_FORBIDDEN_CMAKE_PATH}"
      BGP_FORBIDDEN_CMAKE_PATH_OFFSET
    )
    if (NOT BGP_FORBIDDEN_CMAKE_PATH_OFFSET EQUAL -1)
      message(FATAL_ERROR
        "OSS CMake file ${BGP_CMAKE_FILE} references internal or vendored path: "
        "${BGP_FORBIDDEN_CMAKE_PATH}"
      )
    endif()
  endforeach()
endforeach()
