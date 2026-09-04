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

#include <folly/container/IntrusiveList.h>
#include <folly/logging/xlog.h>
#include "neteng/fboss/bgp/cpp/changeTracker/ConsumerBitmap.h"

#include <algorithm>
#include <memory>

// Forward declarations
template <typename T>
class TrackableObject;
template <typename T>
class ChangeItem;

// Define the intrusive list type for ChangeItems first (needed by Consumer.h)
template <typename T>
using ChangeItemList =
    folly::SafeIntrusiveList<ChangeItem<T>, &ChangeItem<T>::changeListHook>;

// Include Consumer.h for the pendingConsumerHook member used in intrusive list
#include "neteng/fboss/bgp/cpp/changeTracker/Consumer.h"

/**
 * ChangeItem represents an item in the change list.
 * It contains a reference to the trackable object that has changed,
 * as well as consumer-specific functionality for the pull model.
 *
 * @tparam T The type of the object being tracked
 */
template <typename T>
class ChangeItem {
 public:
  /*
   * Define the intrusive list type for pending consumers inside the class
   * This defers instantiation until ChangeItem<T> is used
   */
  using PendingConsumerList =
      folly::SafeIntrusiveList<Consumer<T>, &Consumer<T>::pendingConsumerHook>;

  /**
   * Constructor.
   * @param trackableObject The trackable object that has changed.
   * @param consumerBitmap Bitmap of consumers that need to process this item.
   */
  ChangeItem(
      TrackableObject<T>* trackableObject,
      const ConsumerBitmap& consumerBitmap)
      : trackableObject(trackableObject), consumerBitmap(consumerBitmap) {}

  /**
   * Destructor.
   */
  ~ChangeItem() {
    /*
     * Note: We don't clear the reference in the trackable object here
     * because the TrackableObject owns this ChangeItem via unique_ptr
     * and will handle the cleanup itself.
     */
    if (changeListHook.is_linked()) {
      XLOG(DBG1, "ChangeItem destroyed without unlink");
    }
  }

  /**
   * Copy constructor (deleted).
   */
  ChangeItem(const ChangeItem&) = delete;

  /**
   * Copy assignment operator (deleted).
   */
  ChangeItem& operator=(const ChangeItem&) = delete;

  /**
   * Move constructor (deleted).
   */
  ChangeItem(ChangeItem&&) = delete;

  /**
   * Move assignment operator (deleted).
   */
  ChangeItem& operator=(ChangeItem&&) = delete;

  // Check if all consumers have processed this item
  bool allConsumersProcessed() const {
    return BitmapUtils::isAllBitsCleared(consumerBitmap);
  }

  // Mark a consumer as having processed this item
  void markConsumerProcessed(size_t consumerId) {
    BitmapUtils::clearBit(consumerBitmap, consumerId);
  }

  void setConsumerBitMap(const ConsumerBitmap& bitmap) {
    this->consumerBitmap = bitmap;
  }

  // Get the typed object
  T& getTypedObject() {
    return trackableObject->get();
  }

  const T& getTypedObject() const {
    return trackableObject->get();
  }

  void removePendingConsumer(std::shared_ptr<Consumer<T>> consumer) {
    /*
     * O(1) removal using intrusive list iterator_to()
     * Check if the consumer's hook is linked (i.e., it's in a pending list)
     */
    if (consumer && consumer->pendingConsumerHook.is_linked()) {
      pendingConsumers.erase(pendingConsumers.iterator_to(*consumer));
    }
  }

  // Pointer to the trackable object (not owned)
  TrackableObject<T>* trackableObject{nullptr};

  // Hook for intrusive list
  folly::SafeIntrusiveListHook changeListHook;

  // Bitmap of consumers that need to process this item
  ConsumerBitmap consumerBitmap;

  /*
   * List of consumers that are pended on this item
   * Uses intrusive list for O(1) removal via iterator_to()
   */
  PendingConsumerList pendingConsumers;
};

/**
 * Get the next item in the change list.
 * @param item The current item.
 * @param list The list that contains the item.
 * @return Pointer to the next item, or nullptr if this is the last item or
 * not in a list.
 */
template <typename T>
inline ChangeItem<T>* get_next(
    const ChangeItem<T>* item,
    const ChangeItemList<T>& list) {
  // Check if the item is in a list
  if (item && item->changeListHook.is_linked()) {
    // Get an iterator to this item
    auto it = list.iterator_to(*const_cast<ChangeItem<T>*>(item));

    // Increment the iterator to get the next item
    ++it;

    // Check if we've reached the end of the list
    if (it != list.end()) {
      return const_cast<ChangeItem<T>*>(&(*it));
    }
  }
  return nullptr;
}
