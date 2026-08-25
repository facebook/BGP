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

#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalRibBuilder.h"

#include <utility>

#include <folly/logging/xlog.h>

namespace facebook::bgp {

void CanonicalRibBuilder::addEntry(
    const folly::CIDRNetwork& prefix,
    int64_t ribVersion,
    const std::vector<CanonicalPathInput>& paths,
    const CanonicalEntryFields& entryFields) {
  auto key = folly::IPAddress::networkToString(prefix);
  if (canonicalRibEntries_.contains(key)) {
    XLOGF(DFATAL, "addEntry called twice for prefix {}", key);
    return;
  }

  auto entry = encoding_.buildEntry(
      prefix,
      ribVersion,
      paths,
      /*includeBestPath=*/false,
      /*includePaths=*/true);
  if (entryFields.pathSelectionPending.has_value()) {
    entry.path_selection_pending() = entryFields.pathSelectionPending.value();
  }
  if (entryFields.activeCpsCriteria.has_value()) {
    entry.active_cps_criteria() = entryFields.activeCpsCriteria.value();
  }
  if (entryFields.activeCteUcmpAction.has_value()) {
    entry.active_cte_ucmp_action() = entryFields.activeCteUcmpAction.value();
  }
  canonicalRibEntries_.emplace(std::move(key), std::move(entry));
}

bgp_thrift::TCanonicalRibState CanonicalRibBuilder::build() {
  XCHECK(!built_) << "CanonicalRibBuilder::build() called more than once";
  built_ = true;
  bgp_thrift::TCanonicalRibState state;
  state.attr_dict() = encoding_.dictSnapshot();
  state.deduped_paths() = encoding_.pathAttrsSnapshot();
  state.peers() = std::move(encoding_).peersSnapshot();
  state.rib_entries() = std::move(canonicalRibEntries_);
  return state;
}

} // namespace facebook::bgp
