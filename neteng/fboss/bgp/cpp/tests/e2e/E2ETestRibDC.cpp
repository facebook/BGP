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
 * DC platform definition of makeTestRib(). Compiled into the
 * e2e_test_fixture_dc library. Includes RibDC.h (and therefore
 * dc/PlatformConstant.h); MUST NOT be linked into the same binary as
 * E2ETestRibBB.cpp — see E2ETestRibFactory.h.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestRibFactory.h"

#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/rib/RibDC.h"
#include "neteng/fboss/bgp/cpp/tests/e2e/E2ETestUtils.h"

namespace facebook {
namespace bgp {

std::unique_ptr<RibBase> makeTestRib(
    const std::unordered_map<folly::CIDRNetwork, thrift::BgpNetwork>&
        localRoutes,
    const BgpGlobalConfig& globalConfig,
    const std::optional<bgp_policy::BgpPolicies>& policyConfig,
    nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
    MonitoredMPMCQueue<RibOutMessage>& ribOutQ,
    std::shared_ptr<NexthopCache> nexthopCache,
    FsdbSyncer* fsdbSyncer) {
  return std::make_unique<TestRibT<RibDC>>(
      localRoutes,
      globalConfig,
      policyConfig,
      ribInQ,
      ribOutQ,
      kDevPlatform,
      fsdbSyncer,
      nexthopCache);
}

} // namespace bgp
} // namespace facebook
