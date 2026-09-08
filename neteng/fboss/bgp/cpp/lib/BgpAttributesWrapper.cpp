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

#include "BgpAttributesWrapper.h"
#include "BgpAttributesSmartSet.h"
#include "neteng/fboss/bgp/cpp/lib/BgpUtil.h"

#include <folly/logging/xlog.h>

namespace facebook {
namespace nettools {
namespace bgplib {

BgpAttributesWrapper::BgpAttributesWrapper()
    : attrsPtr_(nullptr), attrsSetPtr_(nullptr) {}

BgpAttributesWrapper::BgpAttributesWrapper(
    std::shared_ptr<bgplib::BgpAttributes> attrsPtr,
    BgpAttributesSmartSet* setPtr)
    : attrsPtr_(std::move(attrsPtr)), attrsSetPtr_(setPtr) {}

BgpAttributesWrapper::BgpAttributesWrapper(const BgpAttributesWrapper& copy) {
  attrsPtr_ = copy.attrsPtr_;
  attrsSetPtr_ = copy.attrsSetPtr_;
}

BgpAttributesWrapper::BgpAttributesWrapper(
    BgpAttributesWrapper&& copy) noexcept {
  attrsPtr_ = std::move(copy.attrsPtr_);
  attrsSetPtr_ = copy.attrsSetPtr_;
}

BgpAttributesWrapper::~BgpAttributesWrapper() {
  freeFromSet();
}

bgplib::BgpAttributes BgpAttributesWrapper::get() const {
  return *attrsPtr_.get();
}

void BgpAttributesWrapper::freeFromSet() {
  /*
   * The refernece count is 2 if the BgpAttributes is not used by others:
   * One reference by the BgpAttributesSmartSet and one by this object.
   */
  if (attrsPtr_.use_count() == 2 && attrsSetPtr_ != nullptr) {
    attrsSetPtr_->removeEntry(*this);
  }
}

bool BgpAttributesWrapper::operator==(const BgpAttributesWrapper& other) const {
  if (attrsPtr_ == nullptr && other.attrsPtr_ == nullptr) {
    return true;
  }
  if (attrsPtr_ == nullptr || other.attrsPtr_ == nullptr) {
    return false;
  }
  return *(this->attrsPtr_) == *(other.attrsPtr_);
}

bool BgpAttributesWrapper::operator!=(const BgpAttributesWrapper& other) const {
  return !(*this == other);
}

BgpAttributesWrapper::operator bool() const {
  return attrsPtr_ != nullptr;
}

BgpAttributesWrapper& BgpAttributesWrapper::operator=(
    const BgpAttributesWrapper& other) {
  freeFromSet();
  attrsPtr_ = other.attrsPtr_;
  attrsSetPtr_ = other.attrsSetPtr_;
  return *this;
}

BgpAttributesWrapper& BgpAttributesWrapper::operator=(
    BgpAttributesWrapper&& other) {
  if (this != &other) {
    freeFromSet();
    attrsPtr_ = std::move(other.attrsPtr_);
    attrsSetPtr_ = other.attrsSetPtr_;
    other.attrsSetPtr_ = nullptr;
  }
  return *this;
}

} // namespace bgplib
} // namespace nettools
} // namespace facebook

namespace std {

/**
 * Make BgpAttributesWrapper hashable
 */
size_t hash<facebook::nettools::bgplib::BgpAttributesWrapper>::operator()(
    facebook::nettools::bgplib::BgpAttributesWrapper const& attrs) const {
  XCHECK(attrs);
  return hash<facebook::nettools::bgplib::BgpAttributes>{}(attrs.get());
}

} // namespace std
