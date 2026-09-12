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

#include <cstddef>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <folly/IPAddress.h>

#include "neteng/fboss/bgp/cpp/lib/coro/MergeQueue.h"
#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalConvert.h"

namespace facebook::bgp {

/* Immutable, owning input needed to encode one canonical RIB entry. */
struct CanonicalRibEntryInput {
  std::vector<CanonicalPathInput> paths;
  CanonicalEntryFields fields;
};

/* A complete latest state for one prefix; nullopt represents a withdrawal. */
struct CanonicalRibPrefixUpdate {
  folly::CIDRNetwork prefix;
  std::optional<CanonicalRibEntryInput> entry;
};

/*
 * Complete owning input for one Loc-RIB snapshot captured on the RIB thread.
 * Every update contains an entry; prefixes absent from the vector are
 * withdrawals.
 */
struct CanonicalRibFullSnapshotInput {
  std::vector<CanonicalRibPrefixUpdate> entries;
};

using CanonicalRibUpdate =
    std::variant<CanonicalRibPrefixUpdate, CanonicalRibFullSnapshotInput>;

/*
 * Bind each canonical update type to its valid queue semantics. Prefix updates
 * coalesce by prefix; a full snapshot supersedes every pending update. The raw
 * MergeQueue push operations remain private so callers cannot accidentally
 * purge with a prefix update or key a full snapshot.
 */
class CanonicalRibUpdateQueue {
 public:
  /* Enqueue the latest state for one prefix; return whether it coalesced. */
  bool pushPrefixUpdate(CanonicalRibPrefixUpdate update) noexcept {
    const auto prefix = update.prefix;
    return queue_.pushMerge(std::move(update), prefix);
  }

  /* Enqueue an authoritative snapshot that supersedes all pending updates. */
  void pushFullSnapshot(CanonicalRibFullSnapshotInput snapshot) noexcept {
    queue_.pushPurgeAll(std::move(snapshot));
  }

  folly::coro::Task<CanonicalRibUpdate> pop() {
    return queue_.pop();
  }

  std::optional<CanonicalRibUpdate> tryPop() {
    return queue_.tryPop();
  }

  size_t size() const noexcept {
    return queue_.size();
  }

  bool empty() const noexcept {
    return queue_.empty();
  }

 private:
  coro::MergeQueue<CanonicalRibUpdate, folly::CIDRNetwork> queue_;
};

} // namespace facebook::bgp
