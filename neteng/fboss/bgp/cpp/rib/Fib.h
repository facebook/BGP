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

#include <boost/noncopyable.hpp>
#include <folly/IPAddress.h>
#include <folly/coro/AsyncScope.h>

#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Structs.h"
#include "neteng/fboss/bgp/cpp/lib/coro/MPMCQueue.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfo.h"

namespace folly {
class EventBase;
}

namespace facebook::bgp {

class RibBase;

/**
 * Abstract interface for FIB (Forwarding Information Base) implementations.
 * This class defines the contract that all FIB implementations must follow.
 */
class Fib : public boost::noncopyable {
 public:
  /*
   * [FIB-ACK with FibProgrammedMessage]
   *
   * This message is sent back to Rib for the prefixes that have been programmed
   * to HW. Together with each prefix, the nexthops that are installed to
   * HW are also sent back. In the case of withdraw, the 'nexthops' is nullptr.
   *
   * After Rib receives this message, Rib shall compare the nexthops in the
   * message with the nexthops in RibEntry. Only the prefix having the matching
   * nexthops are advertised to BGP peers.
   *
   * The prefixes are grouped together based on shared attribute pointer. This
   * is to to allow advertise batch of prefixes with same attributes together,
   * which would help adjRibOut to pack as many prefixes in one BgpUpdate msgs.
   *
   */
  using FibProgrammedPfxToNexthops = folly::
      F14NodeMap<folly::CIDRNetwork, std::shared_ptr<const WeightedNexthopMap>>;

  using FibProgrammedPfxs = folly::
      F14FastMap<std::shared_ptr<const BgpPath>, FibProgrammedPfxToNexthops>;

  struct FibProgrammedMessage {
    const FibProgrammedPfxs fibProgrammedPfxs;
    const bool isSync;

    FibProgrammedMessage(FibProgrammedPfxs fibProgrammedPfxs, bool isSync)
        : fibProgrammedPfxs(std::move(fibProgrammedPfxs)), isSync(isSync) {}
  };
  struct FibSyncReq {};

  using FibMessage = std::variant<FibProgrammedMessage, FibSyncReq>;
  using FibMessageQueue = bgp::coro::MPMCQueue<FibMessage>;

  virtual ~Fib() = default;

  virtual void updateUnicastRoute(
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<const BgpPath> attrsToBeAdvertised,
      std::shared_ptr<const WeightedNexthopMap> weightedNexthops,
      const bool isLocalRouteBest,
      const bool installToFib,
      const folly::F14NodeMap<folly::IPAddress, facebook::bgp::NexthopInfo>&
          nextHopInfoMap,
      const std::optional<uint32_t>& classId = std::nullopt,
      std::shared_ptr<const NexthopTopoInfoMap> nexthopTopoInfoMap = nullptr,
      const BgpRouteType routeType = BgpRouteType::UNKNOWN) = 0;

  virtual void updateUnicastRouteWithBackup(
      const folly::CIDRNetwork& prefix,
      std::shared_ptr<const BgpPath> attrsToBeAdvertised,
      std::shared_ptr<const WeightedNexthopMap> weightedNexthops,
      const bool isLocalRouteBest,
      const bool installToFib,
      const folly::F14NodeMap<folly::IPAddress, facebook::bgp::NexthopInfo>&
          nextHopInfoMap,
      const std::optional<uint32_t>& classId,
      std::shared_ptr<const NexthopTopoInfoMap> nexthopTopoInfoMap,
      const BgpRouteType routeType,
      const std::optional<folly::IPAddress>&) {
    updateUnicastRoute(
        prefix,
        std::move(attrsToBeAdvertised),
        std::move(weightedNexthops),
        isLocalRouteBest,
        installToFib,
        nextHopInfoMap,
        classId,
        std::move(nexthopTopoInfoMap),
        routeType);
  }

  virtual bool isConnected() const = 0;

  virtual bool isFullSynced() const = 0;

  virtual folly::coro::Task<void> program(bool isSync = false) = 0;

  virtual void stop() = 0;
};

} // namespace facebook::bgp
