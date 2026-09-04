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

#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include <folly/logging/xlog.h>

namespace facebook::bgp {

BgpUcmpQuantizer::BgpUcmpQuantizer(
    uint64_t minStepBps,
    double errorPctThreshold,
    const std::vector<uint64_t>& fixedQuantizedBpsList)
    : minStepBps(minStepBps),
      errorPctThreshold(errorPctThreshold),
      fixedQuantizedBpsList(fixedQuantizedBpsList) {
  CHECK_GT(fixedQuantizedBpsList.size(), 0);

  XLOGF(DBG4, "minStepBps: {}", minStepBps);
  XLOGF(DBG4, "errorPctThreshold: {}", errorPctThreshold);
  XLOG(DBG4, "fixedQuantizedBpsList:");
  for (const auto& bps : fixedQuantizedBpsList) {
    XLOGF(DBG4, "{}", bps);
  }

  std::set<uint64_t> fixedQuantizedBpsSet{
      fixedQuantizedBpsList.begin(), fixedQuantizedBpsList.end()};

  /*
   * start from specified max-bps, go top down by each minStepBps
   * set new output_bps if
   * 1) error >= threshold or
   * 2) input_bps is specified in fixedQuantizedBpsList.
   */
  uint64_t input_bps = *fixedQuantizedBpsSet.rbegin();
  uint64_t output_bps = *fixedQuantizedBpsSet.rbegin();
  while (input_bps > 0) {
    if (fixedQuantizedBpsSet.contains(input_bps) ||
        (static_cast<double>(output_bps) / input_bps - 1 >=
         errorPctThreshold)) {
      output_bps = input_bps;
    }
    quantizedBpsMap.emplace(input_bps, output_bps);
    if (input_bps < minStepBps) {
      break;
    }
    input_bps -= minStepBps;
  }
}

float BgpUcmpQuantizer::quantize(float inputBytesPerSec) const {
  CHECK_GT(quantizedBpsMap.size(), 0);
  uint64_t inputBps = static_cast<uint64_t>(inputBytesPerSec) * 8;
  /*
   * find closest key, return corresponding value
   * find lower (1st value >= inputBps)
   */
  auto lower = quantizedBpsMap.lower_bound(inputBps);
  if (lower == quantizedBpsMap.end()) {
    // inputBps > max-key
    return static_cast<float>(quantizedBpsMap.rbegin()->second) / 8;
  }

  if (lower == quantizedBpsMap.begin()) {
    // inputBps <= min-key
    return static_cast<float>(lower->second) / 8;
  }

  // find the closest key
  auto pre = std::prev(lower);
  if (inputBps - pre->first < lower->first - inputBps) {
    return static_cast<float>(pre->second) / 8;
  } else {
    return static_cast<float>(lower->second) / 8;
  }
}

} // namespace facebook::bgp
