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

#include "neteng/fboss/bgp/cpp/rib/FibOut.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include <folly/container/F14Map.h>
#include <folly/hash/Hash.h>

namespace facebook::bgp {

bool FibOutNexthop::operator==(const FibOutNexthop& other) const {
  return address == other.address && interfaceName == other.interfaceName &&
      weight == other.weight && role == other.role &&
      connected == other.connected;
}

bool FibOutNexthop::operator<(const FibOutNexthop& other) const {
  if (address != other.address) {
    return address < other.address;
  }
  return std::tie(interfaceName, weight, role, connected) <
      std::tie(other.interfaceName, other.weight, other.role, other.connected);
}

FibOutRouteMetadata FibOutRouteMetadata::program(
    std::optional<int32_t> adminDistance,
    std::optional<int32_t> classId) {
  FibOutRouteMetadata metadata;
  metadata.operation_ = FibOutOperation::PROGRAM;
  if (adminDistance) {
    metadata.adminDistance_ = *adminDistance;
    metadata.flags_ |= kHasAdminDistance;
  }
  if (classId) {
    metadata.classId_ = *classId;
    metadata.flags_ |= kHasClassId;
  }
  return metadata;
}

FibOutOperation FibOutRouteMetadata::operation() const {
  return operation_;
}

std::optional<int32_t> FibOutRouteMetadata::adminDistance() const {
  return flags_ & kHasAdminDistance ? std::make_optional(adminDistance_)
                                    : std::nullopt;
}

std::optional<int32_t> FibOutRouteMetadata::classId() const {
  return flags_ & kHasClassId ? std::make_optional(classId_) : std::nullopt;
}

bool FibOutRouteMetadata::operator==(const FibOutRouteMetadata& other) const {
  return adminDistance_ == other.adminDistance_ && classId_ == other.classId_ &&
      operation_ == other.operation_ && flags_ == other.flags_;
}

size_t FibOutNexthopHash::operator()(const FibOutNexthop& nexthop) const {
  const auto interfaceName = nexthop.interfaceName
      ? std::string_view{*nexthop.interfaceName}
      : std::string_view{};
  return folly::hash::hash_combine(
      nexthop.address,
      nexthop.interfaceName.has_value(),
      interfaceName,
      nexthop.weight,
      static_cast<uint8_t>(nexthop.role),
      nexthop.connected.has_value(),
      nexthop.connected.value_or(false));
}

size_t FibNexthopSetHash::operator()(const FibNexthopSet& nexthops) const {
  size_t hash = nexthops.size();
  for (const auto& nexthop : nexthops) {
    hash = folly::hash::hash_combine(hash, FibOutNexthopHash{}(nexthop));
  }
  return hash;
}

struct FibNexthopSets::State {
  /** Own one canonical set and unregister it when its final owner releases. */
  struct InternedSet {
    /** Construct an owner for one normalized set and its registry bucket. */
    InternedSet(
        FibNexthopSet nexthops,
        std::weak_ptr<State> registry,
        size_t hash)
        : nexthops(std::move(nexthops)),
          registry(std::move(registry)),
          hash(hash) {}

    /** Remove this set's weak registry entry when its last owner releases. */
    ~InternedSet();

    InternedSet(const InternedSet&) = delete;
    InternedSet& operator=(const InternedSet&) = delete;
    InternedSet(InternedSet&&) = delete;
    InternedSet& operator=(InternedSet&&) = delete;

    FibNexthopSet nexthops;
    std::weak_ptr<State> registry;
    size_t hash;
  };

  /** Identify one interned owner without keeping the canonical set alive. */
  struct Candidate {
    const InternedSet* address;
    std::weak_ptr<const InternedSet> owner;
  };

  /** Group weak candidates by content hash; equality resolves collisions. */
  folly::F14FastMap<size_t, std::vector<Candidate>> setsByHash;

  /** Remove one released owner from its hash bucket. */
  void unregister(size_t hash, const InternedSet* released) {
    const auto bucket = setsByHash.find(hash);
    if (bucket == setsByHash.end()) {
      return;
    }

    auto& candidates = bucket->second;
    candidates.erase(
        std::remove_if(
            candidates.begin(),
            candidates.end(),
            [released](const Candidate& candidate) {
              return candidate.address == released;
            }),
        candidates.end());
    if (candidates.empty()) {
      setsByHash.erase(bucket);
    }
  }
};

FibNexthopSets::State::InternedSet::~InternedSet() {
  if (auto state = registry.lock()) {
    state->unregister(hash, this);
  }
}

FibNexthopSets::FibNexthopSets() : state_(std::make_shared<State>()) {}

std::shared_ptr<const FibNexthopSet> FibNexthopSets::getOrCreate(
    FibNexthopSet nexthops) {
  std::sort(nexthops.begin(), nexthops.end());
  nexthops.erase(std::unique(nexthops.begin(), nexthops.end()), nexthops.end());

  const auto hash = FibNexthopSetHash{}(nexthops);
  auto& candidates = state_->setsByHash[hash];
  for (const auto& candidate : candidates) {
    // Registry access and final owner release are serialized by the caller.
    // InternedSet unregisters during final release, so an expired candidate
    // that remains in this bucket is a serialization-contract violation.
    auto owner = candidate.owner.lock();
    if (!owner) {
      throw std::logic_error(
          "FibNexthopSets contains an expired registry entry");
    }
    if (owner->nexthops == nexthops) {
      const auto* value = &owner->nexthops;
      return std::shared_ptr<const FibNexthopSet>{std::move(owner), value};
    }
  }

  auto owner =
      std::make_shared<State::InternedSet>(std::move(nexthops), state_, hash);
  const auto* value = &owner->nexthops;
  candidates.push_back(
      State::Candidate{.address = owner.get(), .owner = owner});
  return std::shared_ptr<const FibNexthopSet>{std::move(owner), value};
}

size_t FibNexthopSets::size() const {
  size_t result{0};
  for (const auto& [_, candidates] : state_->setsByHash) {
    result += candidates.size();
  }
  return result;
}

void FibNexthopSets::forEach(
    folly::FunctionRef<void(const FibNexthopSet&, size_t)> visitor) const {
  for (const auto& [_, candidates] : state_->setsByHash) {
    for (const auto& candidate : candidates) {
      const auto externalOwnerCount = candidate.owner.use_count();
      auto owner = candidate.owner.lock();
      if (!owner) {
        throw std::logic_error(
            "FibNexthopSets contains an expired registry entry");
      }
      visitor(owner->nexthops, static_cast<size_t>(externalOwnerCount));
    }
  }
}

void FibNexthopSets::clear() {
  state_->setsByHash.clear();
}

} // namespace facebook::bgp
