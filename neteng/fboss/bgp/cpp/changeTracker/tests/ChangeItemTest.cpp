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
#include <memory>

#include "neteng/fboss/bgp/cpp/changeTracker/ChangeItem.h"
#include "neteng/fboss/bgp/cpp/changeTracker/ConsumerBitmap.h"
#include "neteng/fboss/bgp/cpp/changeTracker/TrackableObject.h"

// Forward declaration for Consumer
template <typename T>
class Consumer;

namespace {

// Simple test class to use with TrackableObject
class TestObject {
 public:
  explicit TestObject(int value) : value_(value) {}

  int getValue() const {
    return value_;
  }

  void setValue(int value) {
    value_ = value;
  }

 private:
  int value_;
};

// Consumer-specific functionality will be added in a later diff

// Test that the initial state of ChangeItem is correct
TEST(ChangeItemTest, InitialState) {
  // Create a TrackableObject
  auto trackableObject =
      std::make_unique<TrackableObject<TestObject>>(TestObject(42));

  // Create a ConsumerBitmap
  ConsumerBitmap bitmap;
  BitmapUtils::setBit(bitmap, 0);
  BitmapUtils::setBit(bitmap, 1);

  // Create a ChangeItem
  auto changeItem =
      std::make_unique<ChangeItem<TestObject>>(trackableObject.get(), bitmap);

  // Verify initial state
  EXPECT_EQ(trackableObject.get(), changeItem->trackableObject);
  EXPECT_FALSE(changeItem->changeListHook.is_linked());
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 0));
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 1));
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 2));
  EXPECT_TRUE(changeItem->pendingConsumers.empty());
  EXPECT_FALSE(changeItem->allConsumersProcessed());
}

// Test that the getTypedObject() method returns the correct object
TEST(ChangeItemTest, GetTypedObject) {
  // Create a TrackableObject
  auto trackableObject =
      std::make_unique<TrackableObject<TestObject>>(TestObject(42));

  // Create a ConsumerBitmap
  ConsumerBitmap bitmap;
  BitmapUtils::setBit(bitmap, 0);

  // Create a ChangeItem
  auto changeItem =
      std::make_unique<ChangeItem<TestObject>>(trackableObject.get(), bitmap);

  // Verify getTypedObject() returns the correct object
  EXPECT_EQ(42, changeItem->getTypedObject().getValue());

  // Modify the object and verify getTypedObject() returns the updated object
  trackableObject->get().setValue(100);
  EXPECT_EQ(100, changeItem->getTypedObject().getValue());
}

// Test that the markConsumerProcessed() method works correctly
TEST(ChangeItemTest, MarkConsumerProcessed) {
  // Create a TrackableObject
  auto trackableObject =
      std::make_unique<TrackableObject<TestObject>>(TestObject(42));

  // Create a ConsumerBitmap with multiple bits set
  ConsumerBitmap bitmap;
  BitmapUtils::setBit(bitmap, 0);
  BitmapUtils::setBit(bitmap, 1);
  BitmapUtils::setBit(bitmap, 2);

  // Create a ChangeItem
  auto changeItem =
      std::make_unique<ChangeItem<TestObject>>(trackableObject.get(), bitmap);

  // Verify initial state
  EXPECT_FALSE(changeItem->allConsumersProcessed());
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 0));
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 1));
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 2));

  // Mark consumers as processed one by one
  changeItem->markConsumerProcessed(0);
  EXPECT_FALSE(changeItem->allConsumersProcessed());
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 0));
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 1));
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 2));

  changeItem->markConsumerProcessed(1);
  EXPECT_FALSE(changeItem->allConsumersProcessed());
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 0));
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 1));
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 2));

  changeItem->markConsumerProcessed(2);
  EXPECT_TRUE(changeItem->allConsumersProcessed());
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 0));
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 1));
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 2));
}

// Test that the setConsumerBitMap() method works correctly
TEST(ChangeItemTest, SetConsumerBitMap) {
  // Create a TrackableObject
  auto trackableObject =
      std::make_unique<TrackableObject<TestObject>>(TestObject(42));

  // Create an initial ConsumerBitmap
  ConsumerBitmap initialBitmap;
  BitmapUtils::setBit(initialBitmap, 0);

  // Create a ChangeItem
  auto changeItem = std::make_unique<ChangeItem<TestObject>>(
      trackableObject.get(), initialBitmap);

  // Verify initial state
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 0));
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 1));
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 2));

  // Create a new ConsumerBitmap
  ConsumerBitmap newBitmap;
  BitmapUtils::setBit(newBitmap, 1);
  BitmapUtils::setBit(newBitmap, 2);

  // Set the new bitmap
  changeItem->setConsumerBitMap(newBitmap);

  // Verify the bitmap was updated
  EXPECT_FALSE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 0));
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 1));
  EXPECT_TRUE(BitmapUtils::isBitSet(changeItem->consumerBitmap, 2));
}

/*
 * Test for removePendingConsumer() will be added in a later diff
 * when consumer-specific functionality is implemented
 */

// Test the get_next() function
TEST(ChangeItemTest, GetNext) {
  // Create a list of TrackableObjects
  std::vector<std::unique_ptr<TrackableObject<TestObject>>> trackableObjects;
  trackableObjects.reserve(3);
  for (int i = 0; i < 3; ++i) {
    trackableObjects.push_back(
        std::make_unique<TrackableObject<TestObject>>(TestObject(i)));
  }

  // Create a ConsumerBitmap
  ConsumerBitmap bitmap;
  BitmapUtils::setBit(bitmap, 0);

  // Create a list of ChangeItems
  std::vector<std::unique_ptr<ChangeItem<TestObject>>> changeItems;
  changeItems.reserve(3);
  for (int i = 0; i < 3; ++i) {
    changeItems.push_back(
        std::make_unique<ChangeItem<TestObject>>(
            trackableObjects[i].get(), bitmap));
  }

  // Create an intrusive list
  ChangeItemList<TestObject> changeList;

  // Add the items to the list
  for (auto& item : changeItems) {
    changeList.push_back(*item);
  }

  // Verify the list has the correct size
  EXPECT_EQ(3, changeList.size());

  // Test get_next() function
  ChangeItem<TestObject>* item1 = changeItems[0].get();
  ChangeItem<TestObject>* item2 = changeItems[1].get();
  ChangeItem<TestObject>* item3 = changeItems[2].get();

  // Get the next item after item1
  ChangeItem<TestObject>* next1 = get_next(item1, changeList);
  EXPECT_EQ(item2, next1);

  // Get the next item after item2
  ChangeItem<TestObject>* next2 = get_next(item2, changeList);
  EXPECT_EQ(item3, next2);

  // Get the next item after item3 (should be nullptr)
  ChangeItem<TestObject>* next3 = get_next(item3, changeList);
  EXPECT_EQ(nullptr, next3);

  // Test get_next() with an item not in the list
  auto standaloneObject =
      std::make_unique<TrackableObject<TestObject>>(TestObject(99));
  auto standaloneItem =
      std::make_unique<ChangeItem<TestObject>>(standaloneObject.get(), bitmap);
  ChangeItem<TestObject>* standaloneItemPtr = standaloneItem.get();

  // Get the next item after the standalone item (should be nullptr)
  ChangeItem<TestObject>* nextStandalone =
      get_next(standaloneItemPtr, changeList);
  EXPECT_EQ(nullptr, nextStandalone);

  // Remove items from the list
  changeList.clear();

  // Verify the list is empty
  EXPECT_TRUE(changeList.empty());
}

} // namespace
