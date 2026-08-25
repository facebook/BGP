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

#include <cstdint>
#include <string>
#include <vector>

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>

#include "neteng/fboss/bgp/cpp/rib/canonical/CanonicalRibEncoding.h"

namespace facebook::bgp {

namespace bgp_thrift = ::facebook::neteng::fboss::bgp::thrift;

/*
 * One-shot canonical RIB builder. Shared encoding mechanics live in
 * canonical::Encoding; this wrapper owns only snapshot accumulation and the
 * one-shot build contract. It is not thread-safe; create and drive each
 * instance from one owning RIB or PeerManager EventBase.
 */
class CanonicalRibBuilder {
 public:
  /**
   * Encode and retain one Loc-RIB entry for the final snapshot.
   *
   * @param prefix Network prefix used both in the entry and as the snapshot
   *     map key.
   * @param ribVersion Loc-RIB version associated with this prefix.
   * @param paths Path inputs to encode. The builder emits the complete grouped
   *     path list and does not emit the separate best_path field.
   * @param entryFields Optional per-prefix operational fields.
   *
   * A duplicate prefix is rejected before any pools are mutated. DFATAL aborts
   * debug builds; production logs the error and retains the first entry.
   */
  void addEntry(
      const folly::CIDRNetwork& prefix,
      int64_t ribVersion,
      const std::vector<CanonicalPathInput>& paths,
      const CanonicalEntryFields& entryFields = {});

  /**
   * Materialize the complete canonical RIB and consume the builder.
   *
   * @return Attribute dictionaries, deduplicated paths, peers, and every entry
   *     previously supplied to addEntry(). Entry and peer maps are moved into
   *     the result to avoid an additional RIB-sized copy.
   *
   * The builder should be discarded after this call because its accumulated
   * state has been moved into the result.
   */
  bgp_thrift::TCanonicalRibState build();

 private:
  canonical::Encoding<canonical::StrongInternPool> encoding_;
  folly::F14FastMap<std::string, bgp_thrift::TRibEntryCanonical>
      canonicalRibEntries_;
  bool built_{false};
};

} // namespace facebook::bgp
