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

#include <folly/Benchmark.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>

#include "neteng/fboss/bgp/cpp/tests/AdjRibOutUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;

/*
 * Benchmark the PeerManagerBase -> AdjRibOut message processing with different
 * numbers of prefixes.
 */
void BM_ProcessAdjRibOutMsgLoop(uint32_t iters, size_t numPrefixes) {
  AdjRibOutboundFixture fixture;
  BENCHMARK_SUSPEND {
    fixture.SetUp(); // prepare fm and evb_
    fixture.setupAdjRib();
  }

  while (iters--) {
    fixture.fm_->addTask([&] {
      for (uint32_t i = 0; i < numPrefixes; ++i) {
        auto ribMsg = createRibSingleAnnounce(
            kV4Prefix1, kV4Nexthop1, fixture.localPeerV4_, true);
        fixture.adjRib_->processRibMessage(ribMsg);
      }
      BENCHMARK_SUSPEND {
        fixture.adjRibInQ_->fiberPush(FiberBgpPeer::BgpSessionStop{false});
      }
    });

    // kick start the loop
    fixture.evb_.loop();
  }
}

// Benchmark with different numbers of announcements
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibOutMsgLoop, Advertise_100, 100);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibOutMsgLoop, Advertise_500, 500);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibOutMsgLoop, Advertise_1000, 1000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibOutMsgLoop, Advertise_5000, 5000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibOutMsgLoop, Advertise_10000, 10000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibOutMsgLoop, Advertise_50000, 50000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibOutMsgLoop, Advertise_100000, 100000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibOutMsgLoop, Advertise_500000, 500000);

int main(int argc, char** argv) {
  // Initialize folly
  const folly::Init init(&argc, &argv);

  // Run the benchmarks
  folly::runBenchmarks();
  return 0;
}
