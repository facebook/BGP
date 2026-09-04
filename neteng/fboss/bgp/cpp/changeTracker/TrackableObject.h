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

#include <memory>

// Forward declarations
template <typename T>
class ChangeItem;

/**
 * Template class for trackable objects with type-safe change item storage.
 * @tparam T The type of the object being tracked
 */
template <typename T>
class TrackableObject {
 public:
  // Constructor taking an rvalue reference to avoid copying
  explicit TrackableObject(T&& obj) : object_(std::move(obj)) {}

  // Constructor taking a const reference
  explicit TrackableObject(const T& obj) : object_(obj) {}

  // Destructor
  ~TrackableObject() = default;

  // Deleted copy constructor and assignment operator to prevent copying
  TrackableObject(const TrackableObject&) = delete;
  TrackableObject& operator=(const TrackableObject&) = delete;

  /*
   * Move ops deleted: TrackableObject is always heap-allocated behind
   * unique_ptr, and moving would leave ChangeItem backpointers dangling.
   */
  TrackableObject(TrackableObject&&) = delete;
  TrackableObject& operator=(TrackableObject&&) = delete;

  // Get a reference to the typed object
  T& get() {
    return object_;
  }
  const T& get() const {
    return object_;
  }

  // Check if this object is currently on the change list
  bool isOnChangeList() const {
    return changeItem_ != nullptr;
  }

  // Set the change item for this object
  void setChangeItem(std::unique_ptr<ChangeItem<T>> item) {
    changeItem_ = std::move(item);
  }

  // Clear the change item for this object
  void clearChangeItem() {
    changeItem_.reset();
  }

  // Get the typed change item for this object
  ChangeItem<T>* getChangeItem() const {
    return changeItem_.get();
  }

 private:
  T object_;
  std::unique_ptr<ChangeItem<T>> changeItem_;
};
