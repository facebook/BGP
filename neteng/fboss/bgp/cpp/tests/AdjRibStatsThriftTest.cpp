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

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

namespace facebook::bgp {

using namespace facebook::neteng::fboss::bgp::thrift;

TEST(AdjRibStatsThriftTest, NestedPeerViewsPreserveTheirFields) {
  constexpr int64_t kRemoteBgpId = 0x0a000001;
  constexpr int64_t kGroupId = 7;
  constexpr auto kEgressPolicyName = "egress-policy-a";
  constexpr int64_t kPrePolicyPathCount = 9;
  constexpr int64_t kPostPolicyPathCount = 10;
  constexpr int64_t kActivePaths = 11;
  constexpr int64_t kPackingListSize = 13;

  TAdjRibInStats ribIn;
  auto& ribInPeer = ribIn.peers()->emplace_back();
  ribInPeer.peer_key()->peer_address() = "2001:db8::1";
  ribInPeer.peer_key()->remote_bgp_id() = kRemoteBgpId;
  ribInPeer.pre_policy_path_count() = kPrePolicyPathCount;
  ribInPeer.post_policy_path_count() = kPostPolicyPathCount;
  ribInPeer.active_paths() = kActivePaths;

  TAdjRibOutStats ribOut;
  auto& ribOutPeer = ribOut.peers()->emplace_back();
  ribOutPeer.peer_key() = ribInPeer.peer_key().value();
  ribOutPeer.group_key()->egress_policy_name() = kEgressPolicyName;
  ribOutPeer.group_key()->group_id() = kGroupId;
  ribOutPeer.packing_list_size() = kPackingListSize;

  ASSERT_EQ(1, ribIn.peers()->size());
  EXPECT_EQ(
      kPrePolicyPathCount,
      ribIn.peers()->front().pre_policy_path_count().value());
  EXPECT_EQ(
      kPostPolicyPathCount,
      ribIn.peers()->front().post_policy_path_count().value());
  EXPECT_EQ(kActivePaths, ribIn.peers()->front().active_paths().value());
  ASSERT_EQ(1, ribOut.peers()->size());
  EXPECT_EQ(
      kRemoteBgpId,
      ribOut.peers()->front().peer_key()->remote_bgp_id().value());
  EXPECT_EQ(
      kEgressPolicyName,
      ribOut.peers()->front().group_key()->egress_policy_name().value());
  EXPECT_EQ(kGroupId, ribOut.peers()->front().group_key()->group_id().value());
  EXPECT_EQ(
      kPackingListSize, ribOut.peers()->front().packing_list_size().value());
}

} // namespace facebook::bgp
