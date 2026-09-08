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

#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/lib/fibers/Utils.h"

using namespace facebook::nettools::bgplib;

class GenerateJitterTest : public ::testing::Test {};

// Test that jitter returns 0 for zero or negative input
TEST_F(GenerateJitterTest, ZeroAndNegativeInput) {
  EXPECT_EQ(generateJitter(0), 0);
  EXPECT_EQ(generateJitter(-100), 0);
  EXPECT_EQ(generateJitter(-1), 0);
}

// Test that jitter is within expected range for various base times
TEST_F(GenerateJitterTest, JitterWithinExpectedRange) {
  /*
   * For each base time, jitterMax = min(1000, baseTime * 10 / 100)
   * Formula: jitterMax - (rand32() % (2 * jitterMax))
   * Range: (-jitterMax, +jitterMax] i.e. [-jitterMax + 1, +jitterMax]
   */

  struct TestCase {
    long baseTimeMs;
    long expectedJitterMax;
  };

  std::vector<TestCase> testCases = {
      {100, 10}, // 10% of 100 = 10
      {1000, 100}, // 10% of 1000 = 100
      {5000, 500}, // 10% of 5000 = 500
      {10000, 1000}, // 10% of 10000 = 1000 (at cap)
      {20000, 1000}, // 10% of 20000 = 2000, but capped at 1000
      {50000, 1000}, // 10% of 50000 = 5000, but capped at 1000
  };

  for (const auto& tc : testCases) {
    // Run multiple iterations to test randomness
    for (int i = 0; i < 100; ++i) {
      auto jitter = generateJitter(tc.baseTimeMs);
      EXPECT_GT(jitter, -tc.expectedJitterMax);
      EXPECT_LE(jitter, tc.expectedJitterMax);
    }
  }
}

// Test that jitter values are varied (entropy-based seeding works)
TEST_F(GenerateJitterTest, JitterValuesAreVaried) {
  // Generate 100 samples and verify they're not all the same
  std::set<long> uniqueValues;
  for (int i = 0; i < 100; ++i) {
    uniqueValues.insert(generateJitter(10000));
  }

  EXPECT_GT(uniqueValues.size(), 10);
}
