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

#include <folly/fibers/Semaphore.h>

namespace facebook::nettools::bgplib {

/**
 * VersionNumber maintains a global version number counter
 * (globalVersionCounter) and get a new version from it by bumpUp
 */
class VersionNumber {
  /**
   * ScopedLock locks the VersionNumber during the scope where the object
   * is alive.
   * ScopedLock is not movable or copyable -- it is meant to stay in the scope
   * where it is created.
   */
  class ScopedLock {
   public:
    explicit ScopedLock(folly::fibers::Semaphore& semaphore)
        : semaphore_(semaphore) {
      semaphore_.wait();
    }

    ScopedLock(ScopedLock&& other) = delete; // move constructor (deleted)
    ScopedLock(const ScopedLock&) = delete; // copy constructor (deleted)
    ScopedLock& operator=(const ScopedLock&) = delete; // assignment operator
                                                       // (deleted)
    ScopedLock& operator=(ScopedLock&&) = delete; // move assignment operator
                                                  // (deleted)

    ~ScopedLock() {
      semaphore_.signal();
    }

   private:
    folly::fibers::Semaphore& semaphore_;
  };

 public:
  VersionNumber(uint64_t versionNumber = 0) : versionNumber_(versionNumber) {}

  ScopedLock grabScopedLock() const {
    return ScopedLock(semaphore_);
  }

  // bump up the version, return the new version
  uint64_t bumpUp() {
    auto ScopedLock = grabScopedLock();
    versionNumber_ = VersionNumber::globalVersionCounter.fetch_add(
        1, std::memory_order_relaxed);
    return versionNumber_;
  }

  // Lock the version number and get its value
  uint64_t get() const {
    auto ScopedLock = grabScopedLock();
    return versionNumber_;
  }

  // get its value without a lock
  uint64_t getWithoutLock() const {
    return versionNumber_;
  }

 private:
  /*
   * use semaphore (1) to lock the version in a fiber-safe manner
   * notice that folly::fibers::Semaphore is thread-safe already
   */
  mutable folly::fibers::Semaphore semaphore_{1};
  // actual version number, should be protected by the semaphore
  uint64_t versionNumber_{0};

  // Function for testing purpose
  static void resetGlobalVersionCounter() {
    VersionNumber::globalVersionCounter = 0;
  }

  /*
   * monotonically increasing counter indicating establish, terminate
   * state changes.
   *
   * It is critical to have this counter be static. Not only is it
   * shared across sessions, FiberBgpPeer and FiberBgpPeerManager
   * rely on this counter to avoid race conditions. See
   *   https://fburl.com/wiki/jvx8y119
   * for more details on this peering workflow.
   */
  inline static std::atomic<uint64_t> globalVersionCounter = 0;

#ifdef VersionNumber_TEST_FRIENDS
  VersionNumber_TEST_FRIENDS
#endif
};

} // namespace facebook::nettools::bgplib
