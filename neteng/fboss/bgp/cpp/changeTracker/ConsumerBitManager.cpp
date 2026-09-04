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

#include <stdexcept>

ConsumerBitManager::ConsumerBitManager(size_t maxBits)
    : maxBits_(maxBits), numBitsInUse_(0) {
  // Initially, all bits are available as a single interval [0, maxBits-1]
  availableIntervals_.insert({0, maxBits - 1});
}

size_t ConsumerBitManager::getConsumerBit() {
  if (availableIntervals_.empty()) {
    throw std::runtime_error("No bit positions available");
  }

  // Get the first available interval
  auto it = availableIntervals_.begin();
  auto [start, end] = *it;

  // Allocate the first bit of the interval
  size_t bitPosition = start;

  // Update the interval by incrementing the start position
  if (start == end) {
    // If this was a single-bit interval, remove it entirely
    availableIntervals_.erase(it);
  } else {
    // Otherwise, shrink the interval by incrementing the start position
    availableIntervals_.erase(it);
    availableIntervals_.insert({start + 1, end});
  }

  // Update the number of bits in use
  numBitsInUse_++;

  return bitPosition;
}

ConsumerBitError ConsumerBitManager::freeConsumerBit(size_t bitPosition) {
  // Check if the bit position is valid
  if (bitPosition >= maxBits_) {
    return ConsumerBitError::INVALID_BIT_TOO_BIG;
  }

  // Check if the bit is already free
  if (!isBitInUse(bitPosition)) {
    return ConsumerBitError::INVALID_BIT_ALREADY_FREED;
  }

  // Find the intervals immediately before and after the bit position
  auto upperIt = availableIntervals_.upper_bound({bitPosition, bitPosition});
  auto lowerIt = upperIt;
  if (lowerIt != availableIntervals_.begin()) {
    lowerIt--;
  } else {
    lowerIt = availableIntervals_.end();
  }

  // Check if we can merge with the lower interval
  bool mergedWithLower = false;
  size_t lowerStart = 0; // Initialize to avoid uninitialized variable warnings
  if (lowerIt != availableIntervals_.end()) {
    auto [start, end] = *lowerIt;
    lowerStart = start; // Store the start value before erasing
    if (end + 1 == bitPosition) {
      // Extend the lower interval to include the bit position
      availableIntervals_.erase(lowerIt);
      availableIntervals_.insert({lowerStart, bitPosition});
      mergedWithLower = true;
    }
  }

  // Check if we can merge with the upper interval
  bool mergedWithUpper = false;
  if (upperIt != availableIntervals_.end()) {
    auto [upperStart, upperEnd] = *upperIt;
    if (upperStart == bitPosition + 1) {
      // Extend the upper interval to include the bit position
      availableIntervals_.erase(upperIt);

      // If we've already merged with the lower interval, we need to merge both
      if (mergedWithLower) {
        // Remove the extended lower interval we just added
        auto extendedLowerIt =
            availableIntervals_.find({lowerStart, bitPosition});
        if (extendedLowerIt != availableIntervals_.end()) {
          /*
           * Create a new interval that spans from the lower start to the upper
           * end
           */
          size_t newStart = extendedLowerIt->first;
          availableIntervals_.erase(extendedLowerIt);
          availableIntervals_.insert({newStart, upperEnd});
        }
      } else {
        // Just extend the upper interval downward
        availableIntervals_.insert({bitPosition, upperEnd});
      }
      mergedWithUpper = true;
    }
  }

  // If we didn't merge with either interval, insert a new single-bit interval
  if (!mergedWithLower && !mergedWithUpper) {
    availableIntervals_.insert({bitPosition, bitPosition});
  }

  // Update the number of bits in use
  numBitsInUse_--;

  return ConsumerBitError::SUCCESS;
}

bool ConsumerBitManager::isBitInUse(size_t bitPosition) const {
  // Check if the bit position is valid
  if (bitPosition >= maxBits_) {
    return false;
  }

  /*
   * Use the optimized findInterval function to check if the bit is in any
   * available interval
   */
  auto it = findInterval(bitPosition);

  // If the bit is in an available interval, it's not in use
  return it == availableIntervals_.end();
}

size_t ConsumerBitManager::getNumBitsInUse() const {
  return numBitsInUse_;
}

size_t ConsumerBitManager::getNumBitsAvailable() const {
  return maxBits_ - numBitsInUse_;
}

size_t ConsumerBitManager::getMaxBits() const {
  return maxBits_;
}

std::set<std::pair<size_t, size_t>>::const_iterator
ConsumerBitManager::findInterval(size_t bitPosition) const {
  // Create a named result variable to help with return value optimization
  std::set<std::pair<size_t, size_t>>::const_iterator result;

  // Find the first interval whose start is >= bitPosition
  auto it = availableIntervals_.lower_bound({bitPosition, 0});

  // Check if this interval contains the bit position
  if (it != availableIntervals_.end()) {
    auto [start, end] = *it;
    if (start <= bitPosition && bitPosition <= end) {
      result = it;
      return result;
    }
  }

  // Check the previous interval (if it exists)
  if (it != availableIntervals_.begin()) {
    auto prevIt = std::prev(it);
    auto [start, end] = *prevIt;
    if (start <= bitPosition && bitPosition <= end) {
      result = prevIt;
      return result;
    }
  }

  // No interval contains the bit position
  result = availableIntervals_.end();
  return result;
}
