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

#define DeDuplicatedAttribute_TEST_FRIENDS                                  \
  FRIEND_TEST(DeDuplicatedAttributeTests, InitializationAndComparisonTest); \
  FRIEND_TEST(DeDuplicatedAttributeTests, AssignmentTest);                  \
  FRIEND_TEST(DeDuplicatedAttributeTests, ClearDeduplicatorTest);           \
  FRIEND_TEST(DeDuplicatedAttributeTests, PreprocessPtrTest);               \
  FRIEND_TEST(DeDuplicatedAttributeTests, PreprocessPtrCommutativeTest);    \
  FRIEND_TEST(                                                              \
      DeDuplicatedAttributeTests, EvictDeletedEntriesFromDeduplicatorTest);

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/lib/DeDuplicator.h"

namespace facebook::nettools::bgplib {

using ::testing::Eq;
using ::testing::Ne;
using ::testing::SizeIs;

struct Data {
  // default constructor for get
  explicit Data() {}
  explicit Data(const std::vector<int>& data) : data(data) {}
  std::vector<int> data;

  bool operator==(const Data& other) const {
    return data == other.data;
  }

  std::size_t hash() const {
    size_t seed = 0;
    for (const int d : data) {
      seed ^= std::hash<int>{}(d) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }
    return seed;
  }

  bool empty() const {
    return data.empty();
  }
};

// This tests the basic operations of the DeDuplicator class.
TEST(DeDuplicatorTests, CRUD) {
  DeDuplicator<Data> dedup;

  // Add a new data.
  std::shared_ptr<Data> data1 = std::make_shared<Data>(std::vector<int>{1, 2});
  Data* dataPtr = data1.get();
  // Get the deduped pointer.
  std::shared_ptr<const Data> dedupedData1 = dedup.get(data1);
  // Verify that the deduped pointer is the same as the original pointer.
  EXPECT_THAT(dedupedData1.get(), dataPtr);
  EXPECT_EQ(dedup.size(), 1);

  // Add another data with the same content.
  std::shared_ptr<Data> data2 = std::make_shared<Data>(std::vector<int>{1, 2});
  Data* newDataPtr = data2.get();
  // Get the deduped pointer.
  std::shared_ptr<const Data> dedupedData2 = dedup.get(data2);
  /*
   * Verify that the deduped pointer is the same as the original pointer,
   * not the new pointer.
   */
  EXPECT_THAT(dedupedData2.get(), Eq(dataPtr));
  EXPECT_THAT(dedupedData2.get(), Ne(newDataPtr));
  EXPECT_EQ(dedup.size(), 1);
}

TEST(DeDuplicatorTests, ClearTest) {
  DeDuplicator<Data> dedup;

  EXPECT_EQ(dedup.size(), 0);

  // Add a new data.
  std::shared_ptr<Data> data = std::make_shared<Data>(std::vector<int>{1, 2});
  dedup.get(data);

  EXPECT_EQ(dedup.size(), 1);

  // Clear the dedup.
  dedup.clear();

  EXPECT_EQ(dedup.size(), 0);
}

TEST(DeDuplicatorTests, GetNullptrTest) {
  DeDuplicator<Data> dedup;

  EXPECT_EQ(dedup.size(), 0);

  dedup.get(nullptr);

  EXPECT_EQ(dedup.size(), 0);
}

TEST(DeDuplicatorTests, TestClean) {
  DeDuplicator<Data> dedup;

  EXPECT_THAT(dedup.size(), Eq(0));
  {
    {
      std::shared_ptr<const Data> data1 =
          dedup.get(std::make_shared<Data>(std::vector<int>{1, 2}));
      EXPECT_THAT(dedup.size(), Eq(1));
    }
    std::shared_ptr<const Data> data2 =
        dedup.get(std::make_shared<Data>(std::vector<int>{2, 1}));
    // data1 is out of scope, but the deduped pointer is still in the cache.
    EXPECT_THAT(dedup.size(), Eq(2));
    // Api under test.
    dedup.clean();
    /*
     * Now the data1's shared_ptr should be deleted, but data2's shared_ptr
     * should still be present.
     */
    EXPECT_THAT(dedup.size(), Eq(1));
  }
  // data2 is out of scope, but the deduped pointer is still in the cache.
  EXPECT_THAT(dedup.size(), Eq(1));
  // Clean the cache.
  dedup.clean();
  /*
   * Now the data2's shared_ptr should be deleted, and the cache should be
   * empty.
   */
  EXPECT_THAT(dedup.size(), Eq(0));
}

TEST(DeDuplicatedAttributeTests, InitializationAndComparisonTest) {
  // Test default initialization
  DeDuplicatedAttribute<Data> attr1;

  EXPECT_EQ(attr1.ptr_, nullptr);

  // Test initialization with data
  Data testData{std::vector<int>{1, 2}};
  DeDuplicatedAttribute<Data> attr2{testData};

  EXPECT_NE(attr2.ptr_, nullptr);
  EXPECT_NE(attr1, attr2);

  // Test initialization with shared pointer of data
  auto testPtr = std::make_shared<Data>(testData);
  DeDuplicatedAttribute<Data> attr3{testPtr};

  // attr2 and attr3 now hold the same deduplicated shared pointer
  EXPECT_NE(attr3.ptr_, nullptr);
  EXPECT_EQ(attr2, attr3);
  EXPECT_EQ(attr2.ptr_, attr3.ptr_);

  // Copy construction
  auto attr4 = attr3;

  EXPECT_EQ(attr2, attr4);
  EXPECT_EQ(attr3, attr4);

  // Move construction
  auto attr5 = std::move(attr4);

  EXPECT_EQ(attr2, attr5);

  // Initialization from temporary data
  auto attr6 = DeDuplicatedAttribute<Data>{Data{std::vector<int>{1, 2}}};

  EXPECT_EQ(attr2, attr6);

  // Initialization from temporary shared pointer
  auto attr8 = DeDuplicatedAttribute<Data>{std::make_shared<Data>(testData)};

  EXPECT_EQ(attr2, attr8);
}

TEST(DeDuplicatedAttributeTests, AssignmentTest) {
  Data testData{std::vector<int>{1, 2}};

  // Assignment
  {
    DeDuplicatedAttribute<Data> attr1;
    EXPECT_EQ(attr1.ptr_, nullptr);

    // Test initialization with data
    DeDuplicatedAttribute<Data> attr2{testData};
    EXPECT_NE(attr1, attr2);

    // Assignment
    attr1 = attr2;
    EXPECT_EQ(attr1, attr2);
  }

  // Move Assignment
  {
    DeDuplicatedAttribute<Data> attr1;
    EXPECT_EQ(attr1.ptr_, nullptr);

    DeDuplicatedAttribute<Data> attr2{testData};
    EXPECT_NE(attr1, attr2);

    // Move assignment
    auto attr3 = attr2;
    attr1 = std::move(attr3);
    EXPECT_EQ(attr1, attr2);
  }

  // Assignment from const reference of data
  {
    DeDuplicatedAttribute<Data> attr1;
    EXPECT_EQ(attr1.ptr_, nullptr);

    DeDuplicatedAttribute<Data> attr2{testData};
    EXPECT_NE(attr1, attr2);

    attr1 = testData;
    EXPECT_EQ(attr1, attr2);
  }

  // Move assignment from temporary of data
  {
    DeDuplicatedAttribute<Data> attr1;
    EXPECT_EQ(attr1.ptr_, nullptr);

    DeDuplicatedAttribute<Data> attr2{testData};
    EXPECT_NE(attr1, attr2);

    attr1 = std::move(testData);
    EXPECT_EQ(attr1, attr2);
  }
}

TEST(DeDuplicatedAttributeTests, DereferenceTest) {
  Data testData{std::vector<int>{1, 2}};
  DeDuplicatedAttribute<Data> attr{testData};

  EXPECT_EQ(*attr, testData);

  EXPECT_EQ(attr->hash(), testData.hash());
}

TEST(DeDuplicatedAttributeTests, BooleanOperatorTest) {
  DeDuplicatedAttribute<Data> attr1;

  EXPECT_FALSE(attr1);

  Data testData{std::vector<int>{1, 2}};
  DeDuplicatedAttribute<Data> attr2{testData};

  EXPECT_TRUE(attr2);
}

TEST(DeDuplicatedAttributeTests, GetTest) {
  DeDuplicatedAttribute<Data> attr1;

  // attr1 has nullptr
  EXPECT_FALSE(attr1);
  EXPECT_EQ(attr1.get(), Data{});

  // attr2 has some non-trivial data
  Data testData{std::vector<int>{1, 2}};
  DeDuplicatedAttribute<Data> attr2{testData};

  EXPECT_TRUE(attr2);
  EXPECT_EQ(attr2.get(), testData);

  // There is only one static copy when getting the empty attribites
  DeDuplicatedAttribute<Data> attr3;
  EXPECT_EQ(&attr1.get(), &attr3.get());
}

TEST(DeDuplicatedAttributeTests, NullOrEmptyTest) {
  Data testData{std::vector<int>{1, 2}};
  DeDuplicatedAttribute<Data> attr1{testData};

  EXPECT_FALSE(attr1.nullOrEmpty());

  // nullptr
  DeDuplicatedAttribute<Data> attr2;

  EXPECT_TRUE(attr2.nullOrEmpty());
}

TEST(DeDuplicatedAttributeTests, ClearDeduplicatorTest) {
  Data testData{std::vector<int>{1, 2}};
  DeDuplicatedAttribute<Data> attr1{testData};

  EXPECT_EQ(DeDuplicatedAttribute<Data>::deDuplicator_.size(), 1);

  DeDuplicatedAttribute<Data>::clearDeduplicator();

  EXPECT_EQ(DeDuplicatedAttribute<Data>::deDuplicator_.size(), 0);
}

TEST(DeDuplicatedAttributeTests, PreprocessPtrTest) {
  Data testData{std::vector<int>{1, 2}};
  DeDuplicatedAttribute<Data> attr1;

  auto testPtr = std::make_shared<Data>(testData);

  EXPECT_EQ(attr1.preprocessPtr(testPtr), testPtr);

  /*
   * Data has empty function, so nullptr would be returned when
   * ptr points to empty data or nullptr
   */
  EXPECT_EQ(attr1.preprocessPtr(nullptr), nullptr);

  Data emptyData(std::vector<int>{});
  EXPECT_EQ(attr1.preprocessPtr(std::make_shared<Data>(emptyData)), nullptr);
}

TEST(DeDuplicatedAttributeTests, PreprocessPtrCommutativeTest) {
  /*
   * std::vector<int> is sortable, and we could declare a commutative
   * attribute commutativeAttr for that
   * In this case, the preprocessPtr would return a pointer to the sorted
   * data
   */

  DeDuplicatedAttribute<std::vector<int>, true> commutativeAttr;

  std::vector<int> data1 = {1, 2};
  std::vector<int> data2 = {2, 1};
  std::vector<int> data3 = {1, 2, 3};

  auto ptr =
      commutativeAttr.preprocessPtr(std::make_shared<std::vector<int>>(data2));
  EXPECT_EQ(*ptr, data1);

  auto ptrDifferent =
      commutativeAttr.preprocessPtr(std::make_shared<std::vector<int>>(data3));
  EXPECT_NE(*ptrDifferent, data1);
}

TEST(DeDuplicatedAttributeTests, EvictDeletedEntriesFromDeduplicatorTest) {
  DeDuplicatedAttribute<Data>::clearDeduplicator();

  Data testDataStatic{std::vector<int>{1, 2}};

  DeDuplicatedAttribute<Data> attrStatic{testDataStatic};
  {
    Data testDataTemporary{std::vector<int>{3, 4}};

    DeDuplicatedAttribute<Data> attrTemporary{testDataTemporary};

    EXPECT_EQ(DeDuplicatedAttribute<Data>::deDuplicator_.size(), 2);
  }

  // Deduplicator still holds the data, although attrTemporary is out of scope.
  EXPECT_EQ(DeDuplicatedAttribute<Data>::deDuplicator_.size(), 2);

  // Evict the deleted entries from the deduplicator.
  DeDuplicatedAttribute<Data>::evictDeletedEntriesFromDeduplicator();

  // Deduplicator evicts the deleted entry
  EXPECT_EQ(DeDuplicatedAttribute<Data>::deDuplicator_.size(), 1);
}

TEST(DeDuplicatedAttributeTests, DeduplicatorSizesAreIndependentTest) {
  // Clear all deduplicators to start from a clean state
  DeDuplicatedAsPath::clearDeduplicator();
  DeDuplicatedCommunities::clearDeduplicator();
  DeDuplicatedClusterList::clearDeduplicator();
  DeDuplicatedExtCommunities::clearDeduplicator();

  EXPECT_EQ(DeDuplicatedAsPath::deduplicatorSize(), 0);
  EXPECT_EQ(DeDuplicatedCommunities::deduplicatorSize(), 0);
  EXPECT_EQ(DeDuplicatedClusterList::deduplicatorSize(), 0);
  EXPECT_EQ(DeDuplicatedExtCommunities::deduplicatorSize(), 0);

  // Insert 1 AS path
  BgpAttrAsPathC asPath1;
  asPath1.push_back(BgpAttrAsPathSegmentC::fromAsSeq({64501, 64502}));
  DeDuplicatedAsPath dedupAsPath1(asPath1);

  EXPECT_EQ(DeDuplicatedAsPath::deduplicatorSize(), 1);
  EXPECT_EQ(DeDuplicatedCommunities::deduplicatorSize(), 0);
  EXPECT_EQ(DeDuplicatedClusterList::deduplicatorSize(), 0);
  EXPECT_EQ(DeDuplicatedExtCommunities::deduplicatorSize(), 0);

  // Insert 2 communities
  BgpAttrCommunitiesC comm1;
  comm1.emplace_back(100, 200);
  DeDuplicatedCommunities dedupComm1(comm1);

  BgpAttrCommunitiesC comm2;
  comm2.emplace_back(100, 300);
  DeDuplicatedCommunities dedupComm2(comm2);

  EXPECT_EQ(DeDuplicatedAsPath::deduplicatorSize(), 1);
  EXPECT_EQ(DeDuplicatedCommunities::deduplicatorSize(), 2);
  EXPECT_EQ(DeDuplicatedClusterList::deduplicatorSize(), 0);
  EXPECT_EQ(DeDuplicatedExtCommunities::deduplicatorSize(), 0);

  // Insert 3 cluster lists
  BgpAttrClusterListC cl1;
  cl1.push_back(1);
  DeDuplicatedClusterList dedupCl1(cl1);

  BgpAttrClusterListC cl2;
  cl2.push_back(2);
  DeDuplicatedClusterList dedupCl2(cl2);

  BgpAttrClusterListC cl3;
  cl3.push_back(3);
  DeDuplicatedClusterList dedupCl3(cl3);

  EXPECT_EQ(DeDuplicatedAsPath::deduplicatorSize(), 1);
  EXPECT_EQ(DeDuplicatedCommunities::deduplicatorSize(), 2);
  EXPECT_EQ(DeDuplicatedClusterList::deduplicatorSize(), 3);
  EXPECT_EQ(DeDuplicatedExtCommunities::deduplicatorSize(), 0);

  // Insert 4 ext communities
  BgpAttrExtCommunitiesC ec1;
  ec1.emplace_back(0, 1);
  DeDuplicatedExtCommunities dedupEc1(ec1);

  BgpAttrExtCommunitiesC ec2;
  ec2.emplace_back(0, 2);
  DeDuplicatedExtCommunities dedupEc2(ec2);

  BgpAttrExtCommunitiesC ec3;
  ec3.emplace_back(0, 3);
  DeDuplicatedExtCommunities dedupEc3(ec3);

  BgpAttrExtCommunitiesC ec4;
  ec4.emplace_back(0, 4);
  DeDuplicatedExtCommunities dedupEc4(ec4);

  EXPECT_EQ(DeDuplicatedAsPath::deduplicatorSize(), 1);
  EXPECT_EQ(DeDuplicatedCommunities::deduplicatorSize(), 2);
  EXPECT_EQ(DeDuplicatedClusterList::deduplicatorSize(), 3);
  EXPECT_EQ(DeDuplicatedExtCommunities::deduplicatorSize(), 4);

  // Clean up
  DeDuplicatedAsPath::clearDeduplicator();
  DeDuplicatedCommunities::clearDeduplicator();
  DeDuplicatedClusterList::clearDeduplicator();
  DeDuplicatedExtCommunities::clearDeduplicator();
}

} // namespace facebook::nettools::bgplib
