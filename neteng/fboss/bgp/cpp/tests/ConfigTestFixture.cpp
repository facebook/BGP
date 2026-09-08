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

#include "neteng/fboss/bgp/cpp/tests/ConfigTestFixture.h"

#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace facebook::neteng::fboss::bgp_attr;
using facebook::nettools::bgplib::constants::kBgpPort;

namespace facebook::bgp {

void ConfigTestFixture::SetUp() {
  /*
   * create bgpPeerTimers for testing, only hold_time and keep_alive is used
   * hold_time = 30, keep_alive = 10
   */
  timers1_.hold_time_seconds() = kHoldTime.count();
  timers1_.keep_alive_seconds() = kKeepAliveTime1.count();

  // hold_time = 30, keep_alive = 15
  timers2_.hold_time_seconds() = kHoldTime.count();
  timers2_.keep_alive_seconds() = kKeepAliveTime2.count();

  /*
   * pre & post route limit
   * max_routes = 12000, warning_only = true, warning_limit = 0
   */
  preRouteLimit.max_routes() = kPreMaxRoutes;
  preRouteLimit.warning_only() = true;
  preRouteLimit.warning_limit() = kPreWarningThreshold;
  postRouteLimit.max_routes() = kPostMaxRoutes;
  postRouteLimit.warning_only() = true;
  postRouteLimit.warning_limit() = kPostWarningThreshold;

  // create Peer Group, used by staticPeer3_/staticPeer4_
  peergroup1_.name() = "PEERGROUP_RSW_CSW_V4";
  peergroup1_.ingress_policy_name() = "RSW_CSW_IN";
  peergroup1_.egress_policy_name() = "RSW_CSW_OUT";
  peergroup1_.is_passive() = true;
  peergroup1_.next_hop_self() = true;
  peergroup1_.peer_tag() = kPeerTypeCsw;
  peergroup1_.bgp_peer_timers() = timers1_;
  peergroup1_.pre_filter() = groupPreRouteLimit;
  peergroup1_.post_filter() = groupPostRouteLimit;

  dynamicPeer1_ = createBgpPeer(
      kAsn2,
      kLocalAddr1,
      kPeerPrefix1,
      kNextHopV4_1,
      kNextHopV6_1,
      true,
      kPeerTypeBgpMonitor);
  dynamicPeer1_.is_rr_client() = true;
  dynamicPeer1_.next_hop_self() = true;
  dynamicPeer1_.disable_ipv4_afi() = true;
  dynamicPeer1_.disable_ipv6_afi() = true;
  dynamicPeer1_.description() = kDescription1;
  dynamicPeer1_.peer_id() = "dynamicPeer1";
  // set bgpPeerTimers for dynamicPeer1_
  dynamicPeer1_.bgp_peer_timers() = timers1_;

  dynamicPeer2_ = createBgpPeer(
      kAsn1,
      kLocalAddr2,
      kPeerPrefix2,
      kNextHopV4_2,
      kNextHopV6_2,
      true,
      kPeerTypeBgpMonitor);
  /*
   * description is not set for dynamicPeer2
   * peer_id is not set for dynamicPeer2
   * no bgpPeerTimers for dynamicPeer2
   */

  staticPeer1_ = createBgpPeer(
      kAsn1,
      kLocalAddr3,
      kPeerAddr3,
      kNextHopV4_3,
      kNextHopV6_3,
      false,
      kPeerTypeCsw,
      kIngressPolicyName,
      kEgressPolicyName,
      AdvertiseLinkBandwidth::BEST_PATH,
      true, // removePrivateAs
      true // enableStatefulHa
  );
  staticPeer1_.next_hop_self() = false;
  /*
   * description is not set for staticPeer1
   * peer_id is not set for staticPeer1
   */
  staticPeer1_.disable_ipv4_afi() = false;
  staticPeer1_.disable_ipv6_afi() = false;
  // no timers for staticPeer1
  staticPeer1_.bgp_peer_timers() = timers1_;
  staticPeer1_.pre_filter() = preRouteLimit;
  staticPeer1_.post_filter() = postRouteLimit;

  staticPeer2_ = createBgpPeer(
      kAsn1,
      kLocalAddr4,
      kPeerAddr4,
      kNextHopV4_4,
      kNextHopV6_4,
      false,
      kPeerTypeCsw,
      std::nullopt, // Populating only egress policy
      kEgressPolicyName,
      AdvertiseLinkBandwidth::DISABLE,
      false // removePrivateAs
  );
  staticPeer2_.description() = kDescription2;
  staticPeer2_.peer_id() = "staticPeer2";
  // set bgpPeerTimers for staticPeer2
  staticPeer2_.bgp_peer_timers() = timers2_;
  staticPeer2_.pre_filter() = preRouteLimit;

  // static peer with peer group config, no local overwrites
  staticPeer3_.remote_as_4_byte() = kAsn1;
  staticPeer3_.local_addr() = kLocalAddr5.str();
  staticPeer3_.peer_addr() = kPeerAddr5.str();
  staticPeer3_.next_hop4() = kNextHopV4_5.str();
  staticPeer3_.next_hop6() = kNextHopV6_5.str();
  staticPeer3_.peer_group_name() = *peergroup1_.name();
  staticPeer3_.post_filter() = postRouteLimit;

  // static peer with peer group config and local overwrites
  staticPeer4_.remote_as_4_byte() = kAsn1;
  staticPeer4_.local_addr() = kLocalAddr6.str();
  staticPeer4_.peer_addr() = kPeerAddr6.str();
  staticPeer4_.next_hop4() = kNextHopV4_6.str();
  staticPeer4_.next_hop6() = kNextHopV6_6.str();
  staticPeer4_.peer_group_name() = *peergroup1_.name();
  // overwrite peer group config
  staticPeer4_.is_passive() = false;
  staticPeer4_.is_rr_client() = true;
  staticPeer4_.disable_ipv6_afi() = true;
  staticPeer4_.egress_policy_name() = kEgressPolicyName;
  staticPeer4_.ingress_policy_name() = kIngressPolicyName;

  // create a new config
  defaultConfig_.router_id() = kLocalAddr1.str();
  defaultConfig_.local_as_4_byte() = kAsn1;
  defaultConfig_.hold_time() = kHoldTime.count();
  defaultConfig_.graceful_restart_convergence_seconds() =
      kGrRestartTime.count();
  defaultConfig_.listen_addr() = kLocalAddr1.str();
  defaultConfig_.listen_port() = kBgpPort;
  std::vector<thrift::PeerGroup> peerGroups;
  peerGroups.push_back(peergroup1_);
  defaultConfig_.peer_groups() = std::move(peerGroups);

  std::vector<thrift::BgpPeer> myPeers;
  myPeers.push_back(dynamicPeer1_);
  myPeers.push_back(dynamicPeer2_);
  myPeers.push_back(staticPeer1_);
  myPeers.push_back(staticPeer2_);
  myPeers.push_back(staticPeer3_);
  myPeers.push_back(staticPeer4_);
  *defaultConfig_.peers() = myPeers;
}

} // namespace facebook::bgp
