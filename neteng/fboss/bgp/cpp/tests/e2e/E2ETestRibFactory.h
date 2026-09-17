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

/*
 * Platform-neutral factory for the E2E test RIB.
 *
 * RibBB.h and RibDC.h each pull in a per-platform PlatformConstant.h that
 * defines kBgpPlatformType (and the FIB-agent port/timeout) as conflicting
 * `inline constexpr` values. They therefore cannot coexist in one translation
 * unit or one linked binary (ODR). To let the SAME platform-neutral E2E suite
 * run against BOTH RibBB and RibDC, makeTestRib() is declared here but DEFINED
 * in two separate translation units:
 *   - E2ETestRibBB.cpp -> builds TestRibT<RibBB> (links rib_bb)
 *   - E2ETestRibDC.cpp -> builds TestRibT<RibDC> (links rib_dc)
 * A given test binary links exactly one of them (via the e2e_test_fixture_bb vs
 * e2e_test_fixture_dc Buck library), which selects the platform. See BUCK.
 */

#pragma once

#include <memory>
#include <optional>
#include <unordered_map>

#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopCache.h"
#include "neteng/fboss/bgp/cpp/rib/RibBase.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"

namespace facebook {
namespace bgp {

class FsdbSyncer;

/*
 * Construct the platform-specific E2E TestRib (a TestRibT<RibBB|RibDC>) as a
 * RibBase. The concrete platform is decided by which definition of this
 * function is linked into the test binary. The argument list mirrors the
 * common RibBase ctor surface. The DC fixture can inject FsdbSyncer; the BB
 * fixture rejects a non-null syncer because RibBB has no FSDB integration.
 */
std::unique_ptr<RibBase> makeTestRib(
    const std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>&
        localRoutes,
    const BgpGlobalConfig& globalConfig,
    const std::optional<bgp_policy::BgpPolicies>& policyConfig,
    nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
    MonitoredMPMCQueue<RibOutMessage>& ribOutQ,
    std::shared_ptr<NexthopCache> nexthopCache,
    FsdbSyncer* fsdbSyncer = nullptr);

} // namespace bgp
} // namespace facebook
