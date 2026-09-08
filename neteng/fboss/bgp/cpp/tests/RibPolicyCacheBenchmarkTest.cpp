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
 * Benchmark for RouteAttributePolicy Cache Preservation
 *
 * Measures performance improvement from cache preservation during policy
 * updates. Compares:
 *   - BEFORE: Full re-evaluation of all ribEntries
 *   - AFTER: Selective re-evaluation of affected prefixes only
 */

#include <folly/Benchmark.h>
#include <folly/init/Init.h>

#include <neteng/fboss/bgp/cpp/rib/RibPolicy.h>
#include <neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h>
#include <neteng/fboss/bgp/cpp/tests/RibUtils.h>

using namespace facebook::bgp;

namespace {

// Test constants
constexpr int64_t kLbwWeight = 100;
constexpr int64_t kNewLbwWeight = 200;
constexpr size_t kNumStatements = 49; // Real CTE policy statement count (show
                                      // bgp rib-policy cte on fa003-uu001.atn3)

// Generate prefixes for testing
std::vector<folly::CIDRNetwork> generatePrefixes(size_t count) {
  std::vector<folly::CIDRNetwork> prefixes;
  prefixes.reserve(count);
  for (uint32_t i = 0; i < static_cast<uint32_t>(count); ++i) {
    prefixes.emplace_back(folly::IPAddress::fromLong(0x0A000000 + i), 24);
  }
  return prefixes;
}

/*
 * Build a BgpPath modeling a CTE-managed route: a CTE class community
 * (65520:V, matched by statement setIdx % kNumStatements), padding communities
 * to reach FAUU's ~8 communities/path, and the given as-path.
 */
std::shared_ptr<BgpPath> buildBgpPath(
    size_t setIdx,
    const std::vector<uint32_t>& asSeq) {
  using namespace facebook::nettools::bgplib;
  auto path = std::make_shared<BgpPath>(*buildBgpPathFields(4, 4, 4, 4));

  BgpAttrCommunitiesC comms;
  comms.emplace_back(
      65520, static_cast<uint16_t>(500 + setIdx % kNumStatements));
  comms.emplace_back(9000, static_cast<uint16_t>(setIdx / kNumStatements));
  for (uint16_t j = 0; j < 6; ++j) {
    comms.emplace_back(static_cast<uint16_t>(9100 + j), 1);
  }
  path->setCommunities(comms);

  BgpAttrAsPathSegmentC seg;
  for (auto asn : asSeq) {
    seg.asSequence.push_back(asn);
  }
  BgpAttrAsPathC asPath;
  asPath.push_back(seg);
  path->setAsPath(asPath);

  path->setNexthop(folly::IPAddress("10.0.0.1"));
  path->publish();
  return path;
}

/*
 * Build RibEntries modeling the FAUU RIB (CTE-managed): each prefix belongs to
 * a CTE class -- its community-set matches one statement -- and has
 * pathsPerPrefix paths whose as-paths lead with an upstream AS (64981-64988,
 * matched by the action regexes). Whole BgpPath objects are pooled per
 * community-set and reused across prefixes, mirroring bgpd's attribute dedup
 * (so the whole-attribute BgpPathMatcher memo dedups the action and the
 * sub-attribute community memo the match).
 */
std::vector<RibEntry> buildFauuRibEntries(
    const std::vector<folly::CIDRNetwork>& prefixes,
    size_t pathsPerPrefix,
    size_t numDistinctCommunities,
    size_t numDistinctAsPaths) {
  const size_t asPathsPerSet = pathsPerPrefix * 2;
  std::vector<std::vector<std::shared_ptr<BgpPath>>> pool(
      numDistinctCommunities);
  for (size_t setIdx = 0; setIdx < numDistinctCommunities; ++setIdx) {
    pool[setIdx].reserve(asPathsPerSet);
    for (size_t j = 0; j < asPathsPerSet; ++j) {
      size_t asPathIdx = (setIdx * asPathsPerSet + j) % numDistinctAsPaths;
      uint32_t leadAs = static_cast<uint32_t>(64981 + (asPathIdx % 8));
      uint32_t tailAs = static_cast<uint32_t>(200000 + asPathIdx);
      pool[setIdx].push_back(buildBgpPath(setIdx, {leadAs, tailAs}));
    }
  }

  // Distinct peer per path slot so each path becomes its own RouteInfo.
  std::vector<TinyPeerInfo> peers;
  peers.reserve(pathsPerPrefix);
  for (size_t i = 0; i < pathsPerPrefix; ++i) {
    peers.emplace_back(
        folly::IPAddress::fromLong(0x02000000 + static_cast<uint32_t>(i)),
        1000 + static_cast<uint32_t>(i),
        static_cast<uint32_t>(i),
        BgpSessionType::EBGP,
        false);
  }

  std::vector<RibEntry> ribEntries;
  ribEntries.reserve(prefixes.size());
  for (size_t p = 0; p < prefixes.size(); ++p) {
    const size_t setIdx = p % numDistinctCommunities;
    RibEntry entry(prefixes[p]);
    for (size_t pathIdx = 0; pathIdx < pathsPerPrefix; ++pathIdx) {
      entry.updatePath(
          peers[pathIdx],
          pool[setIdx][pathIdx % asPathsPerSet],
          /*installToFib=*/false);
    }
    ribEntries.emplace_back(std::move(entry));
  }
  return ribEntries;
}

/*
 * Build the CTE route-attribute policy (mirrors `show bgp rib-policy cte`):
 * numStatements statements, each matching a single CTE community (65520:V, AND)
 * with a UCMP action of 8 nexthop-weight actions, each selecting an upstream
 * via an as-path regex (^64981.* .. ^64988.*). Empty prefix set forces the
 * community-match path.
 */
rib_policy::TRouteAttributePolicy buildFauuPolicy(size_t numStatements) {
  rib_policy::TRouteAttributePolicy tPolicy;
  for (size_t s = 0; s < numStatements; ++s) {
    auto communityMatch = createTBgpCommunityMatch(
        65520,
        static_cast<int32_t>(500 + s),
        routing_policy::MatchValueLogicOperator::EQUAL);
    auto communityList = createTCommunityListMatch(
        {communityMatch}, routing_policy::BooleanOperator::AND);

    rib_policy::TRouteAttributeUcmpAction ucmpAction;
    std::vector<rib_policy::TNextHopWeightAction> weightActions;
    for (int a = 1; a <= 8; ++a) {
      rib_policy::TBgpPathMatcher pathMatcher;
      pathMatcher.as_path_regex() = "^6498" + std::to_string(a) + ".*";
      rib_policy::TNextHopWeightAction weightAction;
      weightAction.path_matchers() = {pathMatcher};
      weightAction.weight() = 100;
      weightActions.push_back(weightAction);
    }
    ucmpAction.nexthop_weight_actions() = weightActions;
    ucmpAction.apply_all_actions_or_fallback_to_ecmp() = false;

    rib_policy::TRouteAttributeActions actions;
    actions.set_ucmp_weights() = ucmpAction;

    rib_policy::TRouteAttributeStatement stmt;
    stmt.matcher() = createTRibRouteMatcher({}, communityList);
    stmt.actions() = actions;
    tPolicy.statements()->emplace("stmt" + std::to_string(s), stmt);
  }
  return tPolicy;
}

/*
 * Create a policy with specified number of statements, each covering a portion
 * of prefixes
 */
rib_policy::TRibPolicy createPolicy(
    const std::vector<folly::CIDRNetwork>& prefixes,
    size_t numStatements,
    int64_t weight) {
  auto now = std::chrono::seconds(std::time(nullptr));
  auto timestamp = now.count() + 3600; // 1 hour from now

  size_t prefixesPerStmt = prefixes.size() / numStatements;

  // Create first statement
  std::vector<folly::CIDRNetwork> firstStmtPrefixes(
      prefixes.begin(), prefixes.begin() + prefixesPerStmt);
  auto tPolicy =
      createTRibPolicyLbw(firstStmtPrefixes, weight, "stmt0", timestamp);

  // Add remaining statements
  for (size_t s = 1; s < numStatements; ++s) {
    size_t startIdx = s * prefixesPerStmt;
    size_t endIdx = std::min((s + 1) * prefixesPerStmt, prefixes.size());
    std::vector<folly::CIDRNetwork> stmtPrefixes(
        prefixes.begin() + startIdx, prefixes.begin() + endIdx);

    tPolicy.route_attribute_policy()->statements()->emplace(
        "stmt" + std::to_string(s),
        createTRouteAttributeStatementLbw(stmtPrefixes, weight, timestamp));
  }

  return tPolicy;
}

} // namespace

/**
 * Benchmark: Cache hit for overwriteRouteAttributes
 *
 * Measures time to evaluate routes when cache is warm (hit) vs cold (miss).
 */
void BM_OverwriteRouteAttributes_CacheHit(uint32_t iters, size_t numPrefixes) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);
  auto tPolicy = createPolicy(prefixes, kNumStatements, kLbwWeight);
  RibPolicy policy{tPolicy};

  // Create RIB entries
  std::vector<RibEntry> ribEntries;
  ribEntries.reserve(numPrefixes);
  for (const auto& prefix : prefixes) {
    ribEntries.emplace_back(prefix);
  }

  // Warm up cache with first pass (not timed)
  RouteAttributePolicy::RibChange warmupChange;
  for (auto& entry : ribEntries) {
    policy.getRouteAttributePolicy()->overwriteRouteAttributes(
        entry, warmupChange);
  }

  suspender.dismiss(); // START timing

  while (iters--) {
    RouteAttributePolicy::RibChange ribChange;
    for (auto& entry : ribEntries) {
      policy.getRouteAttributePolicy()->overwriteRouteAttributes(
          entry, ribChange);
    }
  }

  suspender.rehire(); // STOP timing
}

/**
 * Benchmark: Cache miss for overwriteRouteAttributes
 *
 * Measures time to evaluate routes with empty cache (full statement iteration).
 */
void BM_OverwriteRouteAttributes_CacheMiss(uint32_t iters, size_t numPrefixes) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);

  suspender.dismiss(); // START timing

  while (iters--) {
    // Create fresh policy each iteration (empty cache)
    auto tPolicy = createPolicy(prefixes, kNumStatements, kLbwWeight);
    RibPolicy policy{tPolicy};

    // Create RIB entries
    std::vector<RibEntry> ribEntries;
    ribEntries.reserve(numPrefixes);
    for (const auto& prefix : prefixes) {
      ribEntries.emplace_back(prefix);
    }

    RouteAttributePolicy::RibChange ribChange;
    for (auto& entry : ribEntries) {
      policy.getRouteAttributePolicy()->overwriteRouteAttributes(
          entry, ribChange);
    }
  }

  suspender.rehire(); // STOP timing
}

/**
 * Benchmark: Cache migration (moveCache)
 *
 * Measures time to migrate cache from old policy to new policy.
 */
void BM_CacheMigration_MoveCache(uint32_t iters, size_t numPrefixes) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);
  auto tPolicy = createPolicy(prefixes, kNumStatements, kLbwWeight);

  suspender.dismiss(); // START timing

  while (iters--) {
    RibPolicy oldPolicy{tPolicy};

    // Warm up old policy cache
    std::vector<RibEntry> ribEntries;
    ribEntries.reserve(numPrefixes);
    for (const auto& prefix : prefixes) {
      ribEntries.emplace_back(prefix);
    }
    RouteAttributePolicy::RibChange warmupChange;
    for (auto& entry : ribEntries) {
      oldPolicy.getRouteAttributePolicy()->overwriteRouteAttributes(
          entry, warmupChange);
    }

    // Create new policy and move cache
    auto tNewPolicy = createPolicy(prefixes, kNumStatements, kNewLbwWeight);
    RibPolicy newPolicy{tNewPolicy};

    newPolicy.getRouteAttributePolicy()->moveCache(
        *oldPolicy.getRouteAttributePolicy());
  }

  suspender.rehire(); // STOP timing
}

/**
 * Benchmark: Selective re-evaluation (WITH cache preservation)
 *
 * Simulates policy update with cache preservation - only affected prefixes
 * are re-evaluated.
 */
void BM_PolicyUpdate_WithCachePreservation(
    uint32_t iters,
    size_t numPrefixes,
    size_t affectedPercent) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);
  auto tOldPolicy = createPolicy(prefixes, kNumStatements, kLbwWeight);
  RibPolicy oldPolicy{tOldPolicy};

  // Warm up old policy cache
  std::vector<RibEntry> ribEntries;
  ribEntries.reserve(numPrefixes);
  for (const auto& prefix : prefixes) {
    ribEntries.emplace_back(prefix);
  }
  RouteAttributePolicy::RibChange warmupChange;
  for (auto& entry : ribEntries) {
    oldPolicy.getRouteAttributePolicy()->overwriteRouteAttributes(
        entry, warmupChange);
  }

  // Calculate affected prefixes (simulate stmt1 changed)
  size_t affectedCount = (numPrefixes * affectedPercent) / 100;

  suspender.dismiss(); // START timing

  while (iters--) {
    // Create new policy
    auto tNewPolicy = createPolicy(prefixes, kNumStatements, kNewLbwWeight);
    RibPolicy newPolicy{tNewPolicy};

    // Move cache (cache preservation)
    newPolicy.getRouteAttributePolicy()->moveCache(
        *oldPolicy.getRouteAttributePolicy());

    // Re-evaluate only affected prefixes (selective)
    RouteAttributePolicy::RibChange ribChange;
    for (size_t i = 0; i < affectedCount; ++i) {
      newPolicy.getRouteAttributePolicy()->overwriteRouteAttributes(
          ribEntries[i], ribChange);
    }
  }

  suspender.rehire(); // STOP timing
}

/**
 * Benchmark: Full re-evaluation (WITHOUT cache preservation)
 *
 * Simulates policy update without cache preservation - all prefixes
 * are re-evaluated (baseline for comparison).
 */
void BM_PolicyUpdate_WithoutCachePreservation(
    uint32_t iters,
    size_t numPrefixes) {
  auto suspender = folly::BenchmarkSuspender();

  auto prefixes = generatePrefixes(numPrefixes);

  // Create RIB entries
  std::vector<RibEntry> ribEntries;
  ribEntries.reserve(numPrefixes);
  for (const auto& prefix : prefixes) {
    ribEntries.emplace_back(prefix);
  }

  suspender.dismiss(); // START timing

  while (iters--) {
    // Create new policy (empty cache - no preservation)
    auto tNewPolicy = createPolicy(prefixes, kNumStatements, kNewLbwWeight);
    RibPolicy newPolicy{tNewPolicy};

    // Re-evaluate ALL prefixes (full sync - no cache)
    RouteAttributePolicy::RibChange ribChange;
    for (auto& entry : ribEntries) {
      newPolicy.getRouteAttributePolicy()->overwriteRouteAttributes(
          entry, ribChange);
    }
  }

  suspender.rehire(); // STOP timing
}

/*
 * ============================================================================
 * Benchmark registrations
 * ============================================================================
 */

// Cache hit vs miss comparison (shows value of caching)
BENCHMARK_NAMED_PARAM(BM_OverwriteRouteAttributes_CacheHit, 10k, 10000);
BENCHMARK_NAMED_PARAM(BM_OverwriteRouteAttributes_CacheMiss, 10k, 10000);
BENCHMARK_DRAW_LINE();

BENCHMARK_NAMED_PARAM(BM_OverwriteRouteAttributes_CacheHit, 100k, 100000);
BENCHMARK_NAMED_PARAM(BM_OverwriteRouteAttributes_CacheMiss, 100k, 100000);
BENCHMARK_DRAW_LINE();

// Cache migration performance
BENCHMARK_NAMED_PARAM(BM_CacheMigration_MoveCache, 10k, 10000);
BENCHMARK_NAMED_PARAM(BM_CacheMigration_MoveCache, 100k, 100000);
BENCHMARK_NAMED_PARAM(BM_CacheMigration_MoveCache, 500k, 500000);
BENCHMARK_DRAW_LINE();

/*
 * ============================================================================
 * KEY COMPARISON: With vs Without Cache Preservation
 * ============================================================================
 */

// 100K prefixes - compare full re-eval vs selective (8% affected)
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithoutCachePreservation,
    100k_BASELINE,
    100000);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_8pct_affected,
    100000,
    8);
BENCHMARK_DRAW_LINE();

// 500K prefixes - compare full re-eval vs selective (8% affected)
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithoutCachePreservation,
    500k_BASELINE,
    500000);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    500k_8pct_affected,
    500000,
    8);
BENCHMARK_DRAW_LINE();

// Different affected percentages (shows scaling)
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_1pct_affected,
    100000,
    1);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_5pct_affected,
    100000,
    5);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_10pct_affected,
    100000,
    10);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_25pct_affected,
    100000,
    25);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_50pct_affected,
    100000,
    50);
BENCHMARK_NAMED_PARAM(
    BM_PolicyUpdate_WithCachePreservation,
    100k_100pct_affected,
    100000,
    100);

/**
 * Benchmark: FAUU-scale full walk with per-path matching (memoization is always
 * on, as in production).
 *
 * Models the real CTE policy (see `show bgp rib-policy cte`): single-community
 * match statements with 8 as-path-regex UCMP actions, at FAUU scale (37K
 * prefixes, 12 paths/prefix, ~2444 distinct community-sets, ~10K distinct
 * as-paths) drawn from bounded pools so interned attributes are shared across
 * prefixes, exactly as bgpd's dedup produces.
 */
void BM_FauuFullWalk(
    uint32_t iters,
    size_t numPrefixes,
    size_t pathsPerPrefix,
    size_t numDistinctCommunities,
    size_t numDistinctAsPaths) {
  auto suspender = folly::BenchmarkSuspender();

  std::vector<folly::CIDRNetwork> prefixes;
  prefixes.reserve(numPrefixes);
  for (uint32_t i = 0; i < static_cast<uint32_t>(numPrefixes); ++i) {
    prefixes.emplace_back(folly::IPAddress::fromLong(0x0A000000 + i), 24);
  }

  auto ribEntries = buildFauuRibEntries(
      prefixes, pathsPerPrefix, numDistinctCommunities, numDistinctAsPaths);

  suspender.dismiss();

  while (iters--) {
    /*
     * Fresh policy each iteration => cold memo, modeling the empty->full full
     * re-evaluation.
     */
    RouteAttributePolicy policy(buildFauuPolicy(kNumStatements));
    RouteAttributePolicy::RibChange change;
    for (auto& entry : ribEntries) {
      policy.overwriteRouteAttributes(entry, change);
    }
  }

  suspender.rehire();
}

/*
 * ============================================================================
 * FAUU-scale benchmarks
 * ============================================================================
 */

// Small scale for fast signal
BENCHMARK_NAMED_PARAM(
    BM_FauuFullWalk,
    5k_12paths_2444comms_10kaspath,
    5000,
    12,
    2444,
    10032);
BENCHMARK_DRAW_LINE();

// FAUU scale
BENCHMARK_NAMED_PARAM(
    BM_FauuFullWalk,
    37k_12paths_2444comms_10kaspath,
    37253,
    12,
    2444,
    10032);

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  folly::runBenchmarks();
  return 0;
}
