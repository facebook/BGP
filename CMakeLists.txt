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

cmake_minimum_required(VERSION 3.9)
project(bgp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_WITH_LTO "BUILD_WITH_LTO" OFF)
if (BUILD_WITH_LTO)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_BINARY_DIR})

set(
  CMAKE_MODULE_PATH
  "${CMAKE_SOURCE_DIR}/build/fbcode_builder/CMake"
  "${CMAKE_SOURCE_DIR}"
  ${CMAKE_MODULE_PATH}
)
include(FBThriftCppLibrary)
include(FBGenCMakeBuildInfo)

#
# Find dependencies
#

set(REQ_BOOST_COMPONENTS
  ${REQ_BOOST_COMPONENTS}
  system thread context filesystem program_options regex
)

find_package(Boost REQUIRED COMPONENTS ${REQ_BOOST_COMPONENTS})
find_package(folly CONFIG REQUIRED)
find_package(fb303 CONFIG REQUIRED)
find_package(fizz REQUIRED)
find_package(fmt REQUIRED)
find_package(Gflags REQUIRED)
find_package(Glog REQUIRED)
find_package(FBThrift CONFIG REQUIRED)
find_package(wangle REQUIRED)
find_package(Threads REQUIRED)
find_package(magic_enum CONFIG REQUIRED)
find_package(GTest REQUIRED)
find_library(DOUBLE-CONVERSION double-conversion)
find_library(RE2 re2)
find_library(ZSTD zstd)
find_library(CFGR_FBOSS_COMMON cfgr_fboss_common_cpp2 REQUIRED)
find_library(FBOSS_ALERT_LOGGER alert_logger)
find_library(FBOSS_LOG_THRIFT_CALL log_thrift_call)

find_path(RE2_INCLUDE_DIR re2/re2.h)

# FBOSS install (D104850229 deposits headers under installed/fboss/include and
# static libs under installed/fboss/lib).  fboss does not ship a
# fbossConfig.cmake, so we wire it manually via find_path/find_library.
find_path(FBOSS_INCLUDE_DIR fboss/agent/AddressUtil.h REQUIRED)
include_directories(SYSTEM ${FBOSS_INCLUDE_DIR})
# FBOSS headers include <gtest/gtest_prod.h> for FRIEND_TEST macros;
# make the GTest include path available globally. GTestConfig.cmake
# provides imported targets but may not set GTEST_INCLUDE_DIRS;
# extract the include directory from the imported target.
get_target_property(GTEST_INCLUDE_DIR
  GTest::gtest INTERFACE_INCLUDE_DIRECTORIES)
include_directories(SYSTEM ${GTEST_INCLUDE_DIR})

set(_fboss_lib_names
  address_utils
  nodebase
  radix_tree
  exponential_back_off
  # ctrl_cpp2 and its transitive thrift dependencies.  FBOSS ships no
  # fbossConfig.cmake so the consumer must list every transitive static
  # lib explicitly.  Order matters for one-pass linkers: dependents
  # before their dependencies (deepest leaf libs last).
  ctrl_cpp2
  agent_stats_cpp2
  hardware_stats_cpp2
  asic_temp_cpp2
  mka_structs_cpp2
  transceiver_cpp2
  product_info_cpp2
  prbs_cpp2
  link_cpp2
  phy_cpp2
  platform_config_cpp2
  bcm_config_cpp2
  asic_config_cpp2
  asic_config_v2_cpp2
  switch_config_cpp2
  fboss_cpp2
  optic_cpp2
  mpls_cpp2
  io_stats_cpp2
  common_cpp2
  fboss_common_cpp2
  fboss_error
  fboss_types
  # FSDB client libraries for FsdbSyncer.  Same pattern as ctrl_cpp2 above:
  # dependents before dependencies for one-pass linkers.
  fsdb_syncer
  fsdb_pub_sub
  fsdb_sub_mgr
  fsdb_stream_client
  fsdb_flags
  fsdb_utils
  fsdb_model_cpp2
  thriftpath_lib
  fsdb_common_cpp2
  fsdb_oper_cpp2
  fsdb_cpp2
  cow_storage_mgr
  oper_path_helpers
  common_thrift_utils
  thrift_service_client
  # COW subscription instantiations (FsdbCowStateSubManager for NeighborWatcher)
  fsdb_cow_state_sub_mgr
  fsdb_cow_storage
  fsdb_cow_root
  # FSDB model transitive thrift deps (agent state/config/info types)
  switch_state_cpp2
  agent_config_cpp2
  agent_info_cpp2
  switch_reachability_cpp2
  qsfp_state_cpp2
  qsfp_stats_cpp2
  sensor_service_stats_cpp2
  # Leaf thrift deps pulled in by qsfp_state/sensor_service above.
  qsfp_config_cpp2
  port_state_cpp2
  pim_state_cpp2
  sensor_service_cpp2
  # thrift_cow C++ libraries backing the FsdbOperStateRoot COW storage
  # (CowStorage::parse*Result, PatchApplier::decompressPatch, etc.).
  # Ordering between these and the BGP++/FSDB thrift libs is handled by the
  # --start-group wrapper in the bgp_bin link rule below.
  patch_cpp2
  cow_visitor_results_cpp2
  storage
  cow_storage
  thrift_cow_serializer
  thrift_cow_nodes
  thrift_cow_visitors_common
  thrift_cow_visitors
  # FsdbOperStateRoot extern-template instantiation libs (declared deps of
  # fsdb_cow_storage). Provide the PathVisitor (visitNode), PatchApplier, and
  # ThriftStructNode instantiations over the oper-state tree.
  fsdb_cow_root_path_visitor
  fsdb_path_visitor_oper_state_instantiations
  fsdb_patch_applier_oper_state_instantiations
  fsdb_thrift_struct_oper_instantiations
  # thrift_cow_nodes depends on cfgr_fboss_common_cpp2; keep it inside the
  # link group so any references emitted by the COW node/visitor template
  # instantiations above resolve regardless of one-pass ordering.
  cfgr_fboss_common_cpp2
  # Leaf thrift libs reached through the qsfp_state / sensor / service-client
  # parts of the FsdbOperStateRoot model:
  #  - transceiver_validation_cpp2: FirmwarePair/TransceiverAttributes/VendorConfig
  #  - sensor_config_cpp2: sensor_config::Thresholds (dep of sensor_service.thrift)
  #  - qsfp_cpp2: Client<QsfpService> vtable (thrift_service_client createPlaintextClient)
  transceiver_validation_cpp2
  sensor_config_cpp2
  qsfp_cpp2
)
set(FBOSS_LIBS)
foreach(_fboss_lib IN LISTS _fboss_lib_names)
  find_library(FBOSS_${_fboss_lib}_LIB ${_fboss_lib} REQUIRED)
  list(APPEND FBOSS_LIBS ${FBOSS_${_fboss_lib}_LIB})
endforeach()

# FBOSS's OSS fsdb_model.thrift includes bgp_config.thrift via the
# public_tld-prefixed path (neteng/fboss/bgp/public_tld/configerator/...).
# BGP++ builds bgp_config from the canonical configerator/ path (since
# public_tld IS our repo root).  Create a symlink so FBOSS-installed
# headers that use the public_tld prefix resolve against our locally
# generated bgp_config headers.
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/neteng/fboss/bgp/public_tld)
file(CREATE_LINK
  ${CMAKE_BINARY_DIR}/configerator
  ${CMAKE_BINARY_DIR}/neteng/fboss/bgp/public_tld/configerator
  SYMBOLIC)

# OpenR is NOT pulled via getdeps. The minimal slice BGP++ needs
# (Network.thrift, Platform.thrift, common/NetworkUtil) is vendored under
# public_tld/openr/. See public_tld/openr/PROVENANCE.md for the removal trigger.

set(FOLLY_EXCEPTION_TRACER)
if (TARGET Folly::folly_exception_tracer)
  set(FOLLY_EXCEPTION_TRACER Folly::folly_exception_tracer)
endif()

add_compile_definitions(NO_FOLLY_EXCEPTION_TRACER)
add_compile_definitions(IS_OSS)

#
# Build thrift libs
#

set(BGP_THRIFT_LIBS)

add_fbthrift_cpp_library(
  fb303_cpp2
  common/fb303/if/fb303.thrift
  SERVICES
    FacebookService
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} fb303_cpp2)

# Vendored openr thrift IDL — see public_tld/openr/PROVENANCE.md
add_fbthrift_cpp_library(
  openr_network_cpp2
  openr/if/Network.thrift
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} openr_network_cpp2)

add_fbthrift_cpp_library(
  openr_platform_cpp2
  openr/if/Platform.thrift
  SERVICES
    FibService
  OPTIONS
    json
  DEPENDS
    openr_network_cpp2
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} openr_platform_cpp2)

# Layer 0 — no inter-project dependencies

add_fbthrift_cpp_library(
  network_address_cpp2
  common/network/if/Address.thrift
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} network_address_cpp2)

add_fbthrift_cpp_library(
  routing_policy_cpp2
  configerator/structs/neteng/bgp_policy/thrift/routing_policy.thrift
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} routing_policy_cpp2)

add_fbthrift_cpp_library(
  nsf_policy_cpp2
  configerator/structs/neteng/bgp_policy/thrift/nsf_policy.thrift
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} nsf_policy_cpp2)

add_fbthrift_cpp_library(
  bgp_attr_cpp2
  configerator/structs/neteng/fboss/bgp/if/bgp_attr.thrift
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_attr_cpp2)

add_fbthrift_cpp_library(
  netwhoami_cpp2
  configerator/structs/neteng/netwhoami/netwhoami.thrift
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} netwhoami_cpp2)

add_fbthrift_cpp_library(
  bgp_cpp2
  neteng/fboss/bgp/if/bgp.thrift
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_cpp2)

# bgp_thrift.thrift includes neteng/fboss/bgp/if/policy_thrift.thrift, so the
# OSS build needs a thrift target for policy_thrift to generate
# policy_thrift_types.h that bgp_thrift_types.h #includes.
add_fbthrift_cpp_library(
  bgp_policy_thrift_cpp2
  neteng/fboss/bgp/if/policy_thrift.thrift
  OPTIONS
    json
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_policy_thrift_cpp2)

# Layer 1 — depends on Layer 0

add_fbthrift_cpp_library(
  bgp_policy_cpp2
  configerator/structs/neteng/bgp_policy/thrift/bgp_policy.thrift
  OPTIONS
    json
  DEPENDS
    nsf_policy_cpp2
    routing_policy_cpp2
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_policy_cpp2)

add_fbthrift_cpp_library(
  bgp_structs_cpp2
  neteng/fboss/bgp/if/BgpStructs.thrift
  OPTIONS
    json
  DEPENDS
    network_address_cpp2
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_structs_cpp2)

# Layer 2 — depends on Layer 0+1

add_fbthrift_cpp_library(
  rib_policy_cpp2
  configerator/structs/neteng/bgp_policy/thrift/rib_policy.thrift
  OPTIONS
    json
  DEPENDS
    bgp_policy_cpp2
    routing_policy_cpp2
    bgp_attr_cpp2
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} rib_policy_cpp2)

add_fbthrift_cpp_library(
  bgp_route_types_cpp2
  neteng/fboss/bgp/if/bgp_route_types.thrift
  OPTIONS
    json
  DEPENDS
    bgp_attr_cpp2
    rib_policy_cpp2
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_route_types_cpp2)

add_fbthrift_cpp_library(
  bgp_config_cpp2
  configerator/structs/neteng/fboss/bgp/bgp_config.thrift
  OPTIONS
    json
  DEPENDS
    bgp_policy_cpp2
    bgp_attr_cpp2
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_config_cpp2)

# Layer 3 — service thrift files

add_fbthrift_cpp_library(
  bgp_thrift_cpp2
  neteng/fboss/bgp/if/bgp_thrift.thrift
  SERVICES
    TBgpService
  OPTIONS
    json
  DEPENDS
    rib_policy_cpp2
    bgp_policy_cpp2
    bgp_policy_thrift_cpp2
    bgp_config_cpp2
    bgp_attr_cpp2
    fb303_cpp2
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_thrift_cpp2)

add_fbthrift_cpp_library(
  bgp_stream_cpp2
  neteng/fboss/bgp/if/bgp_stream.thrift
  SERVICES
    TBgpServiceStream
  OPTIONS
    json
  DEPENDS
    bgp_structs_cpp2
    fb303_cpp2
)
set(BGP_THRIFT_LIBS ${BGP_THRIFT_LIBS} bgp_stream_cpp2)

add_build_info(build_info)

install(TARGETS
  ${BGP_THRIFT_LIBS}
  DESTINATION lib
)

#
# Common external dependencies
#
set(BGP_BASE_LIBS
  Folly::folly
  glog::glog
  gflags
  Threads::Threads
  ${Boost_LIBRARIES}
  magic_enum::magic_enum
)
# NOTE: ${FBOSS_LIBS} is intentionally NOT part of BGP_BASE_LIBS. The FBOSS
# static archives and the BGP++/FSDB thrift libs (${BGP_THRIFT_LIBS}) have
# bidirectional dependencies (e.g. FBOSS fsdb_model.thrift includes
# bgp_config.thrift, while BGP++ links the FsdbOperStateRoot COW model), so
# they must be linked together inside a single --start-group on the final
# bgp_bin executable (see below). Intermediate static libs only need the
# headers, which come from the include path.

#
# OpenR libraries (vendored — see public_tld/openr/PROVENANCE.md).
# Satisfies Network_types / Platform_types / NetworkUtil references that
# leak into core BGP++ code via nexthopTracker headers.
#
add_library(openr_networkutil openr/common/NetworkUtil.cpp)
target_link_libraries(openr_networkutil
  ${BGP_BASE_LIBS}
  openr_network_cpp2
)

# Vendored openr::SystemMetrics — used by cpp/watchdog/Watchdog.h.
add_library(openr_systemmetrics openr/monitor/SystemMetrics.cpp)
target_link_libraries(openr_systemmetrics
  ${BGP_BASE_LIBS}
  ${RE2}
)

set(OPENR_LIBS
  openr_networkutil
  openr_systemmetrics
  openr_platform_cpp2
  openr_network_cpp2
)

#
# Tier 0: Leaf libraries (no bgp/cpp cross-module deps)
#

add_library(bgp_routelib
  neteng/fboss/bgp/cpp/routelib/RouteFilter.cpp
  neteng/fboss/bgp/cpp/routelib/RouteSelector.cpp
)

target_link_libraries(bgp_routelib
  ${BGP_BASE_LIBS}
)

add_library(bgp_lib_core
  neteng/fboss/bgp/cpp/lib/BgpAttributesSmartSet.cpp
  neteng/fboss/bgp/cpp/lib/BgpAttributesWrapper.cpp
  neteng/fboss/bgp/cpp/lib/BgpMessageSerializer.cpp
  neteng/fboss/bgp/cpp/lib/BgpStructs.cpp
  neteng/fboss/bgp/cpp/lib/BgpUtil.cpp
)

target_link_libraries(bgp_lib_core
  ${BGP_THRIFT_LIBS}
  ${BGP_BASE_LIBS}
)

#
# Tier 1: Foundation
#

add_library(bgp_common
  neteng/fboss/bgp/cpp/common/BuildInfo.cpp
  neteng/fboss/bgp/cpp/common/BgpModuleBase.cpp
  neteng/fboss/bgp/cpp/common/BgpPath.cpp
  neteng/fboss/bgp/cpp/common/FeatureFlags.cpp
  neteng/fboss/bgp/cpp/common/RouteInfo.cpp
  neteng/fboss/bgp/cpp/common/Utils.cpp
  neteng/fboss/bgp/cpp/stats/Stats.cpp
  neteng/fboss/bgp/cpp/lib/BgpMessageParser.cpp
  neteng/fboss/bgp/cpp/lib/detail/BgpMessageParserUtils.cpp
)

target_link_libraries(bgp_common
  bgp_lib_core
  bgp_routelib
  build_info
  fb303::fb303
  ${BGP_THRIFT_LIBS}
  ${BGP_BASE_LIBS}
  ${OPENR_LIBS}
  ${RE2}
)

add_library(bgp_changetracker
  neteng/fboss/bgp/cpp/changeTracker/ChangeTrackerDebug.cpp
  neteng/fboss/bgp/cpp/changeTracker/ConsumerBitManager.cpp
)

target_link_libraries(bgp_changetracker
  bgp_common
)

#
# Tier 2: Infrastructure
#

add_library(bgp_config_policy
  neteng/fboss/bgp/cpp/config/Config.cpp
  neteng/fboss/bgp/cpp/config/ConfigManager.cpp
  neteng/fboss/bgp/cpp/config/ConfigStructs.cpp
  neteng/fboss/bgp/cpp/config/ConfigUtils.cpp
  neteng/fboss/bgp/cpp/policy/Policy.cpp
  neteng/fboss/bgp/cpp/policy/PolicyAction.cpp
  neteng/fboss/bgp/cpp/policy/PolicyManager.cpp
  neteng/fboss/bgp/cpp/policy/PolicyMatch.cpp
  neteng/fboss/bgp/cpp/policy/PolicyTerm.cpp
  neteng/fboss/bgp/cpp/policy/PolicyUtils.cpp
  neteng/fboss/bgp/cpp/policy/base/PolicyMatchBase.cpp
  neteng/fboss/bgp/cpp/policy/base/PolicyUtils.cpp
)

target_link_libraries(bgp_config_policy
  bgp_lib_core
  bgp_common
  ${BGP_THRIFT_LIBS}
  ${BGP_BASE_LIBS}
)

add_library(bgp_watchdog
  neteng/fboss/bgp/cpp/watchdog/MemProfiler.cpp
  neteng/fboss/bgp/cpp/watchdog/MonitoredModule.cpp
  neteng/fboss/bgp/cpp/watchdog/QueryTree.cpp
  neteng/fboss/bgp/cpp/watchdog/Watchdog.cpp
)

target_link_libraries(bgp_watchdog
  bgp_lib_core
  bgp_common
  bgp_config_policy
  ${BGP_THRIFT_LIBS}
  ${BGP_BASE_LIBS}
)

#
# Tier 3: Transport
#

add_library(bgp_lib_transport
  neteng/fboss/bgp/cpp/lib/fibers/BgpPeerDisplayInfo.cpp
  neteng/fboss/bgp/cpp/lib/fibers/BgpSerializer.cpp
  neteng/fboss/bgp/cpp/lib/fibers/FiberBgpParser.cpp
  neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeer.cpp
  neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeerManager.cpp
  neteng/fboss/bgp/cpp/lib/fibers/FiberServerSocket.cpp
  neteng/fboss/bgp/cpp/lib/fibers/FiberSocket.cpp
  neteng/fboss/bgp/cpp/lib/fibers/Utils.cpp
)

target_link_libraries(bgp_lib_transport
  bgp_lib_core
  bgp_common
  bgp_watchdog
  bgp_config_policy
)

add_library(bgp_nexthop
  neteng/fboss/bgp/cpp/nexthopTracker/InterfaceEntry.cpp
  neteng/fboss/bgp/cpp/nexthopTracker/NexthopCache.cpp
  neteng/fboss/bgp/cpp/nexthopTracker/NexthopHandler.cpp
)
# NOTE: NetlinkWrapper.cpp is intentionally excluded from the OSS build. It
# pulls openr/messaging + openr/nl which are not vendored (BGP++ doesn't
# need them in OSS — netlink-based nexthop tracking is BB-only). The
# corresponding header is filtered from the install set below.

target_link_libraries(bgp_nexthop
  bgp_common
  bgp_lib_core
  bgp_lib_transport
  bgp_watchdog
  FBThrift::thriftcpp2
  ${OPENR_LIBS}
)

add_library(bgp_fsdb_fib_watcher
  neteng/fboss/bgp/cpp/nexthopTracker/FsdbFibWatcher.cpp
)

target_link_libraries(bgp_fsdb_fib_watcher
  bgp_common
  bgp_nexthop
  ${BGP_THRIFT_LIBS}
  ${BGP_BASE_LIBS}
  FBThrift::thriftcpp2
)

#
# Tier 4: Core routing
#

# FIB implementations — separated per code boundary rules (DC / EBB / Dev)

add_library(bgp_fib_holddown
  neteng/fboss/bgp/cpp/rib/FibProgrammingHolddown.cpp
)

target_link_libraries(bgp_fib_holddown
  ${BGP_BASE_LIBS}
)

add_library(bgp_fib_dev
  neteng/fboss/bgp/cpp/rib/FibDev.cpp
)

target_link_libraries(bgp_fib_dev
  bgp_common
  bgp_lib_core
  ${CFGR_FBOSS_COMMON}
  ${BGP_BASE_LIBS}
)

add_library(bgp_fib_fboss
  neteng/fboss/bgp/cpp/rib/FibFboss.cpp
)

target_link_libraries(bgp_fib_fboss
  bgp_common
  bgp_fib_holddown
  bgp_lib_core
  ${CFGR_FBOSS_COMMON}
  FBThrift::thriftcpp2
  ${BGP_BASE_LIBS}
)

add_library(bgp_fsdb_syncer
  neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.cpp
)

target_link_libraries(bgp_fsdb_syncer
  fb303::fb303
  ${BGP_THRIFT_LIBS}
  ${BGP_BASE_LIBS}
  FBThrift::thriftcpp2
)

add_library(bgp_rib
  neteng/fboss/bgp/cpp/rib/CanonicalConvert.cpp
  neteng/fboss/bgp/cpp/rib/CanonicalRibBuilder.cpp
  neteng/fboss/bgp/cpp/rib/RibBase.cpp
  neteng/fboss/bgp/cpp/rib/RibDC.cpp
  neteng/fboss/bgp/cpp/rib/RibEntry.cpp
  neteng/fboss/bgp/cpp/rib/RibPolicy.cpp
  neteng/fboss/bgp/cpp/rib/RibPolicyLogger.cpp
  neteng/fboss/bgp/cpp/rib/RouteInfoFilter.cpp
  neteng/fboss/bgp/cpp/rib/RouteInfoSelector.cpp
  neteng/fboss/bgp/cpp/rib/Utils.cpp
)

target_link_libraries(bgp_rib
  bgp_common
  bgp_config_policy
  bgp_fib_dev
  bgp_fib_fboss
  bgp_fsdb_syncer
  bgp_lib_core
  bgp_nexthop
  bgp_routelib
  bgp_watchdog
  FBThrift::thriftcpp2
  ${OPENR_LIBS}
)

add_library(bgp_adjrib
  neteng/fboss/bgp/cpp/adjrib/AdjRib.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibCommon.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibGroup.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibGroupSerializer.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibIn.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibOut.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibPolicyCache.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibShow.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibStats.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibStructs.cpp
  neteng/fboss/bgp/cpp/adjrib/AdjRibUtil.cpp
  neteng/fboss/bgp/cpp/adjrib/PathIdGenerator.cpp
  neteng/fboss/bgp/cpp/adjrib/RouteFilterLogger.cpp
  neteng/fboss/bgp/cpp/adjrib/UpdateGroupManager.cpp
  neteng/fboss/bgp/cpp/adjrib/WellKnownCommunityFilter.cpp
)

target_link_libraries(bgp_adjrib
  bgp_changetracker
  bgp_common
  bgp_config_policy
  bgp_lib_core
  bgp_lib_transport
  bgp_rib
  bgp_watchdog
  ${FBOSS_ALERT_LOGGER}
  ${OPENR_LIBS}
)

add_library(bgp_peer
  neteng/fboss/bgp/cpp/peer/PeerManagerBase.cpp
  neteng/fboss/bgp/cpp/peer/PeerManagerUtils.cpp
  neteng/fboss/bgp/cpp/peer/SessionManager.cpp
  neteng/fboss/bgp/cpp/peer/NeighborWatcher.cpp
)

target_link_libraries(bgp_peer
  bgp_adjrib
  bgp_changetracker
  bgp_common
  bgp_config_policy
  bgp_fsdb_fib_watcher
  bgp_lib_core
  bgp_lib_transport
  bgp_nexthop
  bgp_rib
  bgp_watchdog
  FBThrift::thriftcpp2
  ${OPENR_LIBS}
)

#
# Tier 5: Application
#

add_library(bgp_service
  neteng/fboss/bgp/cpp/BgpServiceBase.cpp
  neteng/fboss/bgp/cpp/BgpServiceDC.cpp
  neteng/fboss/bgp/cpp/BgpServiceStream.cpp
  neteng/fboss/bgp/cpp/BgpServiceEventHandler.cpp
  neteng/fboss/bgp/cpp/BgpServiceUtil.cpp
  neteng/fboss/bgp/cpp/BgpConfigValidator.cpp
  neteng/fboss/bgp/cpp/BgpProfiler.cpp
  neteng/fboss/bgp/cpp/health/HealthValidator.cpp
)

target_link_libraries(bgp_service
  ${FBOSS_ALERT_LOGGER}
  ${FBOSS_LOG_THRIFT_CALL}
  bgp_common
  bgp_config_policy
  bgp_lib_core
  bgp_peer
  bgp_rib
  bgp_watchdog
  ${RE2}
  FBThrift::thriftcpp2
  fb303::fb303
  ${OPENR_LIBS}
)

install(TARGETS
  bgp_routelib
  bgp_lib_core
  bgp_common
  bgp_changetracker
  bgp_config_policy
  bgp_watchdog
  bgp_lib_transport
  bgp_nexthop
  bgp_fsdb_fib_watcher
  bgp_fib_holddown
  bgp_fib_dev
  bgp_fib_fboss
  bgp_fsdb_syncer
  bgp_rib
  bgp_adjrib
  bgp_peer
  bgp_service
  DESTINATION lib
)

#
# BGP++ binary
#

add_executable(bgp_bin
  neteng/fboss/bgp/cpp/MainOSS.cpp
)

# The BGP++/FSDB thrift libs and the FBOSS static archives cross-reference
# each other (FBOSS fsdb_model.thrift -> bgp_config.thrift; BGP++ ->
# FsdbOperStateRoot COW model; thrift_cow_nodes <-> thrift_cow_visitors), so a
# single one-pass scan cannot resolve them. Wrap both sets in a RESCAN link
# group (--start-group/--end-group) so the linker iterates until all
# cross-references are satisfied. LINK_GROUP also consolidates the same
# archives reached transitively, avoiding duplicate/out-of-group placement.
set(_bgp_grouped_libs ${BGP_THRIFT_LIBS} ${FBOSS_LIBS})
list(JOIN _bgp_grouped_libs "," _bgp_grouped_libs_csv)

target_link_libraries(bgp_bin
  bgp_service
  build_info
  "$<LINK_GROUP:RESCAN,${_bgp_grouped_libs_csv}>"
  ${BGP_BASE_LIBS}
  FBThrift::thriftcpp2
  ${FOLLY_EXCEPTION_TRACER}
)

set_target_properties(bgp_bin PROPERTIES OUTPUT_NAME bgp)

install(TARGETS
  bgp_bin
  DESTINATION sbin
)

#
# Install files
#

install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/neteng
  DESTINATION include
  FILES_MATCHING
    PATTERN "*.h"
    # facebook/ subdirs hold Meta-internal-only code (MainBB, FibEbb,
    # MemProfiler) and must not ship in OSS.
    PATTERN "facebook" EXCLUDE
    # RibBB lives in cpp/rib/ (not facebook/) but is BB-only.
    PATTERN "RibBB.h" EXCLUDE
    # NetlinkWrapper.h pulls openr/messaging + openr/nl, neither vendored.
    PATTERN "NetlinkWrapper.h" EXCLUDE
)

install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/neteng
  DESTINATION include
  FILES_MATCHING PATTERN "*.thrift"
)

install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/configerator
  DESTINATION include
  FILES_MATCHING PATTERN "*.thrift"
)

install(
  DIRECTORY ${CMAKE_SOURCE_DIR}/common
  DESTINATION include
  FILES_MATCHING PATTERN "*.thrift"
)
