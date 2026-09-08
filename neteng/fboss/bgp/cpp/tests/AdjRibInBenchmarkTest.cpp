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
#include <folly/coro/BlockingWait.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>

#include "neteng/fboss/bgp/cpp/tests/AdjRibInUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;
using namespace ::testing;

/*
 * Benchmark the BGP I/O(fiberBgpPeer) -> AdjRibIn message processing loop with
 * different numbers of prefixes.
 */
void BM_ProcessAdjRibInMsgLoop(uint32_t iters, size_t numPrefixes) {
  AdjRibInboundFixture fixture;
  BENCHMARK_SUSPEND {
    fixture.SetUp();
    fixture.setupAdjRib(
        kShortGrRestartTime, /* localGrRestartTime */
        std::nullopt, /* remoteGrRestartTime */
        true, /* sessionEstablished */
        kLocalAs1, /* globalAs */
        kLocalAs1, /* localAs */
        kRemoteAs1, /* remoteAs */
        AfiIpv4Negotiated(true), /* isAfiIpv4Negotiated */
        AfiIpv6Negotiated(true), /* isAfiIpv6Negotiated */
        nullptr, /* policyManager */
        std::nullopt, /* ingressPolicyName */
        false, /* isConfedPeer */
        std::nullopt, /* localConfedAs */
        std::nullopt, /* asConfedId */
        AdvertiseLinkBandwidth::DISABLE, /* advertiseLbw */
        ReceiveLinkBandwidth::DISABLE, /* receiveLbw */
        std::nullopt, /* lbwBps */
        true, /* validateRemoteAs */
        0, /* prefilter maxRoutes, 0 allows unlimited routes */
        true, /* prefilter warning-only */
        0, /* prefilter warning-limit */
        0, /* postfilter maxRoutes, 0 allows unlimited routes */
        true, /* postfilter warning-only */
        0 /* postfilter warning-limit */
    );
  }

  while (iters--) {
    fixture.fm_->addTask([&] {
      BENCHMARK_SUSPEND {
        std::vector<folly::CIDRNetwork> prefixSet;
        for (int i = 0; i < numPrefixes; i++) {
          auto update = createV4BgpUpdateMultipleAnnounce({folly::CIDRNetwork(
              folly::IPAddress::fromLong(kV4Prefix1.first.asV4().toLong() + i),
              24)});
          fixture.adjRibInQ_->fiberPush(std::move(update));
        }
      }
    });

    fixture.fm_->addTask([&] {
      // wait for first rib message. This indicate processing in adjrib is done.
      uint64_t cnt = 0;
      while (cnt++ != numPrefixes) {
        auto msg = folly::coro::blockingWait(fixture.ribInQ_.pop());
      }
      BENCHMARK_SUSPEND {
        fixture.terminateAdjRib();
      }
    });
    // kick start the loop
    fixture.evb_.loop();
  }
}

// Benchmark with different numbers of announcements
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibInMsgLoop, Advertise_100, 100);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibInMsgLoop, Advertise_500, 500);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibInMsgLoop, Advertise_1000, 1000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibInMsgLoop, Advertise_5000, 5000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibInMsgLoop, Advertise_10000, 10000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibInMsgLoop, Advertise_50000, 50000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibInMsgLoop, Advertise_100000, 100000);
BENCHMARK_NAMED_PARAM(BM_ProcessAdjRibInMsgLoop, Advertise_500000, 500000);

int main(int argc, char** argv) {
  // Initialize folly
  const folly::Init init(&argc, &argv);

  // Run the benchmarks
  folly::runBenchmarks();
  return 0;
}
