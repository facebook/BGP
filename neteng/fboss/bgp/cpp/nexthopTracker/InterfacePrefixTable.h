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

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>

#include <optional>

#include "fboss/lib/RadixTree.h"

namespace facebook::bgp {

/**
 * Global table of local interface prefixes across ALL tracked interfaces, used
 * to classify whether a nexthop is directly connected. Backed by a single radix
 * tree keyed on the prefix (network/masklen), so classification is one
 * O(address-bits) longest-prefix (best) match instead of a scan over per-
 * interface tables. The tree splits v4/v6 internally, so families never mix.
 *
 * Each prefix node's value maps every contributing interface host address to
 * its interface index (e.g. a primary 10.0.0.1 and a secondary 10.0.0.2 both
 * configured on 10.0.0.0/16 of the same interface). Keeping the map of
 * contributors (not a bare refcount) makes duplicate address events idempotent
 * and keeps a prefix directly connected as long as at least one address still
 * contributes it, so removing one of several addresses in a subnet does not
 * declassify peers still reachable via another. The stored ifindex lets the
 * interface-state path resolve a covered nexthop's reachability from the
 * covering interface's link state via coveringIfIndex().
 *
 * On a routed backbone a subnet lives on exactly one L3 interface, so a covered
 * nexthop resolves to a single interface index.
 *
 * Only populated when bgp_resolve_nexthops_from_interface_state is on.
 */
class InterfacePrefixTable {
 public:
  /**
   * Record an interface address (host address + prefix length) on interface
   * ifIndex as a contributor to its prefix. Returns true only when this creates
   * the prefix node (coverage newly appears); adding a sibling into an already
   * covered prefix, or re-adding an address already recorded, returns false.
   */
  bool addPrefix(const folly::CIDRNetwork& addr, int ifIndex) {
    /*
     * RadixTree masks addr.first to addr.second internally, so two siblings in
     * the same subnet land on one node; the unmasked host address is the
     * contributor key, mapped to the interface it is configured on.
     */
    auto [it, inserted] = tree_.insert(
        addr.first, addr.second, folly::F14FastMap<folly::IPAddress, int>{});
    it->value().insert_or_assign(addr.first, ifIndex);
    return inserted;
  }

  /**
   * Drop an interface address as a contributor to its prefix. Returns true only
   * when the last contributor leaves and the prefix node is destroyed (coverage
   * disappears); removing one of several same-subnet addresses, or an address
   * that is not tracked, returns false.
   */
  bool removePrefix(const folly::CIDRNetwork& addr) {
    auto it = tree_.exactMatch(addr.first, addr.second);
    if (it.atEnd()) {
      return false;
    }
    it->value().erase(addr.first);
    if (it->value().empty()) {
      tree_.erase(addr.first, addr.second);
      return true;
    }
    return false;
  }

  /**
   * True iff some local prefix covers ip (longest/best match). ip is treated as
   * a host (full-length mask). Address families are matched.
   */
  bool isDirectlyConnected(const folly::IPAddress& ip) const {
    return !tree_.longestMatch(ip, static_cast<uint8_t>(ip.bitCount())).atEnd();
  }

  /**
   * The interface index of the prefix that best-matches ip, or std::nullopt if
   * ip is not directly connected. On a routed backbone a subnet is configured
   * on a single L3 interface, so all contributors at the best-match node share
   * one interface index; this returns it. The interface-state path checks
   * whether that interface is up to decide the nexthop's reachability. ip is
   * treated as a host (full-length mask); families are matched.
   */
  std::optional<int> coveringIfIndex(const folly::IPAddress& ip) const {
    auto it = tree_.longestMatch(ip, static_cast<uint8_t>(ip.bitCount()));
    if (it.atEnd()) {
      return std::nullopt;
    }
    /*
     * A live node always has at least one contributor (it is erased when the
     * last one leaves), and all contributors share an interface index.
     */
    return it->value().begin()->second;
  }

  /**
   * Drop all prefixes. Used to rebuild the table from a full interface snapshot
   * on resync.
   */
  void clear() {
    tree_.clear();
  }

 private:
  facebook::network::
      RadixTree<folly::IPAddress, folly::F14FastMap<folly::IPAddress, int>>
          tree_;
};

} // namespace facebook::bgp
