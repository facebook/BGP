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

#include <algorithm>
#include <memory>

#include <folly/Synchronized.h>
#include <folly/container/F14Set.h>

namespace facebook::nettools::bgplib {

/*
 * The DeDuplicator class tracks a set of shared_ptr objects. When get() is
 * called with a new shared_ptr, if an equivalent object, as per the ==()
 * operator of the underlying object, is already being tracked, then a
 * shared_ptr for the tracked object is returned. Otherwise the new object is
 * added to the tracker.
 *
 * This class is thread-safe. The caller needs to ensure thread-safe handling of
 * the returned shared_ptrs.
 */
template <typename T>
class DeDuplicator {
 public:
  /*
   * Returns a shared_ptr for an equivalent tracked object. If the input object
   * is not tracked, it is added to the set of tracked objects.
   */
  std::shared_ptr<const T> get(std::shared_ptr<T> object) {
    if (!object) {
      // Special logic for nullptr -> skip the cache, just return
      return std::move(object);
    }
    // construct wrapper outside the lock to reduce lock contention
    Wrapper wrapper(std::move(object));
    return tracked_.withWLock(
        [&](folly::F14FastSet<Wrapper, Hash, Compare>& tracked) {
          auto [it, ok] = tracked.emplace(std::move(wrapper));
          return it->object;
        });
  }

  // Returns the number of objects tracked. Used for unit tests.
  std::size_t size() const {
    return tracked_.withRLock(
        [](const auto& tracked) { return tracked.size(); });
  }

  // Clear the cache. Used for unit tests.
  void clear() {
    tracked_.withWLock([&](folly::F14FastSet<Wrapper, Hash, Compare>& tracked) {
      folly::F14FastSet<Wrapper, Hash, Compare> empty;
      /*
       * we use swap instead of clear to make sure that we shrink
       * the capacity accordingly. This is important for unit tests
       * especially the stress tests to avoid test cases affecting
       * the memory footprint of the next test cases.
       */
      tracked.swap(empty);
    });
  }

  /*
   * Clean up the cache by removing objects that are not referenced by any
   * clients.
   * Currently clean is a tight loop. We would need to reduce the entries
   * processed each time when we scale
   */
  void clean() {
    tracked_.withWLock([&](folly::F14FastSet<Wrapper, Hash, Compare>& tracked) {
      for (auto it = tracked.begin(); it != tracked.end();) {
        /*
         * Doing this is safe only because
         *  1. No entity holds a reference to the *it.
         *  2. No entity creates a weak_ptr from *it.
         */
        if (it->object.use_count() == 1) {
          it = tracked.erase(it);
        } else {
          ++it;
        }
      }
    });
  }

 private:
  // Wrapper class to hold a shared_ptr object with hash computed upon creation.
  struct Wrapper {
    std::size_t hash;
    std::shared_ptr<T> object;

    explicit Wrapper(std::shared_ptr<T>&& object)
        : hash(object->hash()), object(std::move(object)) {}
  };

  struct Hash {
    std::size_t operator()(const Wrapper& wrapper) const {
      return wrapper.hash;
    }
  };

  struct Compare {
    bool operator()(const Wrapper& lhs, const Wrapper& rhs) const {
      return *lhs.object == *rhs.object;
    }
  };

  // Tracked objects.
  folly::Synchronized<folly::F14FastSet<Wrapper, Hash, Compare>> tracked_;
};

template <typename T>
concept hasEmpty = requires(T item) {
  item.empty(); // Requires that item.empty() is a valid expression
};

template <typename T>
concept sortableContainer =
    requires(T item) { std::sort(item.begin(), item.end()); };

/*
 * The DeDuplicatedAttribute class holds a deduplicated ptr to the object
 * and a singleton pointer deDuplicator_ to the deduplicator
 * To use the class, we need to initialize the static variable in a cpp file.
 * the communitive variable could be provided to indicate that the attribute is
 * a container with commutative items
 */
template <typename T, bool commutative = false>
class DeDuplicatedAttribute {
 public:
  DeDuplicatedAttribute() {}

  explicit DeDuplicatedAttribute(const T& obj)
      : DeDuplicatedAttribute(std::make_shared<T>(obj)) {}
  explicit DeDuplicatedAttribute(T&& obj)
      : DeDuplicatedAttribute(std::make_shared<T>(std::move(obj))) {}
  explicit DeDuplicatedAttribute(const std::shared_ptr<T>& ptr)
      : ptr_(deDuplicator_.get(preprocessPtr(ptr))) {}
  explicit DeDuplicatedAttribute(std::shared_ptr<T>&& ptr)
      : ptr_(deDuplicator_.get(preprocessPtr(std::move(ptr)))) {}

  // Copy a deduplicated object, we just need to copy the ptr directly
  DeDuplicatedAttribute(const DeDuplicatedAttribute& other)
      : ptr_(other.ptr_) {}

  // Move constructor
  DeDuplicatedAttribute(DeDuplicatedAttribute&& other) noexcept
      : ptr_(std::move(other.ptr_)) {}

  virtual ~DeDuplicatedAttribute() {}

  DeDuplicatedAttribute& operator=(const DeDuplicatedAttribute& other) {
    ptr_ = other.ptr_;
    return *this;
  }
  DeDuplicatedAttribute& operator=(DeDuplicatedAttribute&& other) noexcept {
    if (this != &other) {
      ptr_ = std::move(other.ptr_);
    }
    return *this;
  }
  DeDuplicatedAttribute& operator=(const T& other) {
    DeDuplicatedAttribute tmp{other};
    *this = tmp;
    return *this;
  }
  DeDuplicatedAttribute& operator=(T&& other) noexcept {
    DeDuplicatedAttribute tmp{other};
    *this = tmp;
    return *this;
  }

  inline bool operator==(const DeDuplicatedAttribute& other) const {
    return ptr_ == other.ptr_;
  }

  const T& operator*() const {
    return *ptr_;
  }

  const std::shared_ptr<const T> operator->() const {
    return ptr_;
  }

  // Return the underlying shared_ptr directly
  const std::shared_ptr<const T>& getSharedPtr() const {
    return ptr_;
  }

  explicit operator bool() const {
    return bool(ptr_);
  }

  /*
   * get the attribute if ptr_ is not nullptr
   * otherwise, get an empty copy
   * the empty copy should be default constructible
   */
  const T& get() const
    requires std::constructible_from<T>
  {
    if (ptr_) {
      return *ptr_;
    }

    static const T emptyObj;
    return emptyObj;
  }

  bool nullOrEmpty() const
    requires hasEmpty<T>
  {
    return !ptr_ || ptr_->empty();
  }

  // for unit test
  static void clearDeduplicator() {
    deDuplicator_.clear();
  }

  static void evictDeletedEntriesFromDeduplicator() {
    deDuplicator_.clean();
  }

  // Returns the number of entries in the deduplicator
  static std::size_t deduplicatorSize() {
    return deDuplicator_.size();
  }

 protected:
  inline static DeDuplicator<T> deDuplicator_;

  std::shared_ptr<const T> ptr_;

  std::shared_ptr<T> preprocessPtr(std::shared_ptr<T> ptr) {
    if constexpr (hasEmpty<T>) {
      if (!ptr || ptr->empty()) {
        return nullptr;
      }
    }
    if constexpr (commutative && sortableContainer<T>) {
      std::sort(ptr->begin(), ptr->end());
    }
    return ptr;
  }

#ifdef DeDuplicatedAttribute_TEST_FRIENDS
  DeDuplicatedAttribute_TEST_FRIENDS
#endif
};

} // namespace facebook::nettools::bgplib
