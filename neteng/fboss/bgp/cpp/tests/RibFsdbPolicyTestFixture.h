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

#include "fboss/fsdb/tests/utils/FsdbTestServer.h"
#include "fboss/fsdb/tests/utils/FsdbTestSubscriber.h"
#include "neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.h"
#include "neteng/fboss/bgp/cpp/tests/RetryUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibUtils.h"

namespace facebook::bgp {

// Fixture that adds FSDB test infrastructure on top of RibFixture.
// Sets up FsdbTestServer, FsdbTestSubscriber, and FsdbSyncer, then
// assigns the syncer to the Rib instance.
class RibFsdbFixture : public RibFixture {
 public:
  void SetUp() override {
    RibFixture::SetUp();

    fsdbServer_ = std::make_unique<fboss::fsdb::test::FsdbTestServer>();
    FLAGS_fsdbPort = fsdbServer_->getFsdbPort();
    FLAGS_publish_state_to_fsdb = true;
    FLAGS_publish_stats_to_fsdb = true;
    fsdbSubscriber_ = std::make_unique<fboss::fsdb::test::FsdbTestSubscriber>(
        "test-subscriber");
    fsdbSyncer_ = std::make_unique<FsdbSyncer>();
    fsdbSyncer_->start();
    rib_->evb_.runInEventBaseThreadAndWait([this]() {
      rib_->fsdbSyncer_ = fsdbSyncer_.get();
      rib_->fsdbSyncerStarted_ = true;
    });

    // Wait for publisher to register with FSDB server
    WITH_RETRIES_N(5, {
      auto metadata = fsdbServer_->getPublisherRootMetadata("bgp", false);
      ASSERT_EVENTUALLY_TRUE(metadata.has_value());
    });
  }
};

// Parameterized variant for policy tests that use WithParamInterface<bool>.
class RibFsdbAddPathTestSuite : public RibFsdbFixture,
                                public testing::WithParamInterface<bool> {};

} // namespace facebook::bgp
