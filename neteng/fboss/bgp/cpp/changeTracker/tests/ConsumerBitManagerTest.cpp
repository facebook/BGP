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

#include "neteng/fboss/bgp/cpp/changeTracker/ConsumerBitManager.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace std;

namespace {

// Test that the initial state of ConsumerBitManager is correct
TEST(ConsumerBitManagerTest, InitialState) {
  // Create a ConsumerBitManager with a small number of bits for testing
  auto bitManager = std::make_unique<ConsumerBitManager>(10);

  // Verify initial state
  EXPECT_EQ(0, bitManager->getNumBitsInUse());
  EXPECT_EQ(10, bitManager->getNumBitsAvailable());
  EXPECT_EQ(10, bitManager->getMaxBits());
}

// Test that getting consumer bits works correctly
TEST(ConsumerBitManagerTest, GetConsumerBits) {
  // Create a ConsumerBitManager with a small number of bits for testing
  auto bitManager = std::make_unique<ConsumerBitManager>(10);

  // Get some bit positions
  size_t bit1 = bitManager->getConsumerBit();
  size_t bit2 = bitManager->getConsumerBit();
  size_t bit3 = bitManager->getConsumerBit();

  // Verify bit positions are unique and within range
  EXPECT_NE(bit1, bit2);
  EXPECT_NE(bit1, bit3);
  EXPECT_NE(bit2, bit3);
  EXPECT_LT(bit1, 10);
  EXPECT_LT(bit2, 10);
  EXPECT_LT(bit3, 10);

  // Verify bit usage counts
  EXPECT_EQ(3, bitManager->getNumBitsInUse());
  EXPECT_EQ(7, bitManager->getNumBitsAvailable());

  // Verify bits are marked as in use
  EXPECT_TRUE(bitManager->isBitInUse(bit1));
  EXPECT_TRUE(bitManager->isBitInUse(bit2));
  EXPECT_TRUE(bitManager->isBitInUse(bit3));
}

// Test that freeing consumer bits works correctly
TEST(ConsumerBitManagerTest, FreeConsumerBit) {
  // Create a ConsumerBitManager with a small number of bits for testing
  auto bitManager = std::make_unique<ConsumerBitManager>(10);

  // Get and free bit positions
  size_t bit1 = bitManager->getConsumerBit();
  size_t bit2 = bitManager->getConsumerBit();

  EXPECT_EQ(2, bitManager->getNumBitsInUse());
  EXPECT_EQ(8, bitManager->getNumBitsAvailable());

  // Free a bit position
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(bit1));

  // Verify bit usage counts after freeing
  EXPECT_EQ(1, bitManager->getNumBitsInUse());
  EXPECT_EQ(9, bitManager->getNumBitsAvailable());

  // Verify bit is no longer in use
  EXPECT_FALSE(bitManager->isBitInUse(bit1));
  EXPECT_TRUE(bitManager->isBitInUse(bit2));
}

// Test that bit allocation order works correctly
TEST(ConsumerBitManagerTest, BitAllocationOrder) {
  // Create a ConsumerBitManager with a small number of bits for testing
  auto bitManager = std::make_unique<ConsumerBitManager>(10);

  // Get some bit positions
  size_t bit1 = bitManager->getConsumerBit();
  size_t bit2 = bitManager->getConsumerBit();
  size_t bit3 = bitManager->getConsumerBit();

  // Free a bit position
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(bit1));

  // Get a new bit position - will reuse bit1 immediately
  size_t bit4 = bitManager->getConsumerBit();

  /*
   * Verify the new bit is the same as the freed bit (ConsumerBitManager reuses
   * freed bits immediately)
   */
  EXPECT_EQ(bit1, bit4);

  // Free more bit positions
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(bit2));
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(bit3));

  // Get more bit positions - should use up all available bits
  vector<size_t> allocatedBits;
  allocatedBits.reserve(7);
  for (int i = 0; i < 7; ++i) {
    allocatedBits.push_back(bitManager->getConsumerBit());
  }

  // Verify all bits are now in use
  EXPECT_EQ(8, bitManager->getNumBitsInUse());
  EXPECT_EQ(2, bitManager->getNumBitsAvailable());

  // Verify bit2 and bit3 were reused (bit1/bit4 is already in use)
  EXPECT_TRUE(
      std::find(allocatedBits.begin(), allocatedBits.end(), bit2) !=
      allocatedBits.end());
  EXPECT_TRUE(
      std::find(allocatedBits.begin(), allocatedBits.end(), bit3) !=
      allocatedBits.end());
}

// Test that an exception is thrown when all bits are in use
TEST(ConsumerBitManagerTest, ExceptionWhenAllBitsInUse) {
  // Create a ConsumerBitManager with a small number of bits for testing
  auto bitManager = std::make_unique<ConsumerBitManager>(10);

  // Allocate all bits
  for (int i = 0; i < 10; ++i) {
    bitManager->getConsumerBit();
  }

  // Try to get one more bit position - should throw an exception
  EXPECT_THROW(bitManager->getConsumerBit(), runtime_error);
}

// Test that interval merging works correctly
TEST(ConsumerBitManagerTest, IntervalMerging) {
  // Create a ConsumerBitManager with a small number of bits for testing
  auto bitManager = std::make_unique<ConsumerBitManager>(10);

  // Allocate all bits
  vector<size_t> bits;
  bits.reserve(10);
  for (int i = 0; i < 10; ++i) {
    bits.push_back(bitManager->getConsumerBit());
  }

  // Free bits in a pattern that tests interval merging
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(3));
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(5));
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(7));

  EXPECT_EQ(7, bitManager->getNumBitsInUse());
  EXPECT_EQ(3, bitManager->getNumBitsAvailable());

  // Free adjacent bits to test merging
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(4));
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(6));

  EXPECT_EQ(5, bitManager->getNumBitsInUse());
  EXPECT_EQ(5, bitManager->getNumBitsAvailable());

  // Get bits and verify they come from the merged intervals
  vector<size_t> newBits;
  newBits.reserve(5);
  for (int i = 0; i < 5; ++i) {
    newBits.push_back(bitManager->getConsumerBit());
  }

  // Verify all bits are now in use again
  EXPECT_EQ(10, bitManager->getNumBitsInUse());
  EXPECT_EQ(0, bitManager->getNumBitsAvailable());

  // Verify the new bits include the previously freed bits
  EXPECT_TRUE(std::find(newBits.begin(), newBits.end(), 3) != newBits.end());
  EXPECT_TRUE(std::find(newBits.begin(), newBits.end(), 4) != newBits.end());
  EXPECT_TRUE(std::find(newBits.begin(), newBits.end(), 5) != newBits.end());
  EXPECT_TRUE(std::find(newBits.begin(), newBits.end(), 6) != newBits.end());
  EXPECT_TRUE(std::find(newBits.begin(), newBits.end(), 7) != newBits.end());
}

// Test that invalid bit freeing returns appropriate error codes
TEST(ConsumerBitManagerTest, InvalidBitFreeing) {
  // Create a ConsumerBitManager with a small number of bits for testing
  auto bitManager = std::make_unique<ConsumerBitManager>(10);

  // Try to free an invalid bit position
  EXPECT_EQ(
      ConsumerBitError::INVALID_BIT_TOO_BIG, bitManager->freeConsumerBit(20));

  // Try to free a bit position that is not in use
  EXPECT_EQ(
      ConsumerBitError::INVALID_BIT_ALREADY_FREED,
      bitManager->freeConsumerBit(5));

  // Allocate and free a bit
  size_t bit = bitManager->getConsumerBit();
  EXPECT_EQ(ConsumerBitError::SUCCESS, bitManager->freeConsumerBit(bit));

  // Try to free the same bit again
  EXPECT_EQ(
      ConsumerBitError::INVALID_BIT_ALREADY_FREED,
      bitManager->freeConsumerBit(bit));
}

} // namespace
