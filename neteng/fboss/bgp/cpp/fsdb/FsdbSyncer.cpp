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

#include "neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.h"

#include <chrono>
#include <utility>

#include <folly/Utility.h>
#include <gflags/gflags.h>
#include <thrift/lib/cpp2/TypeClass.h>
#include <thrift/lib/cpp2/op/Get.h>

#include "fboss/fsdb/common/Flags.h"
#include "fboss/fsdb/if/gen-cpp2/fsdb_oper_types.h"
#include "fboss/thrift_cow/gen-cpp2/patch_types.h"
#include "fboss/thrift_cow/nodes/Serializer.h"
#include "neteng/fboss/bgp/cpp/stats/StatsDC.h"

DEFINE_bool(
    publish_rib_to_fsdb,
    false,
    "Enable publishing RIB entries to FSDB");

DEFINE_bool(
    publish_partial_drain_state_to_fsdb,
    false,
    "Enable RibDC publishing device partial-drain state to FSDB");

namespace facebook::bgp {
namespace {
namespace fsdb = facebook::fboss::fsdb;
namespace tcow = facebook::fboss::thrift_cow;
namespace tc = apache::thrift::type_class;
namespace k_fsdb_model = apache::thrift::ident;

const thriftpath::RootThriftPath<fsdb::FsdbOperStateRoot> fsdbStateRootPath;
const auto bgpPath = fsdbStateRootPath.bgp();
const auto kPublisherId = "bgpd";

constexpr std::string_view kConfig = "config";
constexpr std::string_view kRouteAttributePolicy = "routeAttributePolicy";
constexpr std::string_view kPathSelectionPolicy = "pathSelectionPolicy";
constexpr std::string_view kRouteFilterPolicy = "routeFilterPolicy";
constexpr std::string_view kPartialDrainState = "partialDrainState";

void recordSubtreeEvent(std::string_view subtree, std::string_view event) {
  FsdbStatsDC::addFsdbSyncerSubtreeEvent(subtree, event);
}

void recordRetainedUpdate(std::string_view subtree, bool isClear) {
  recordSubtreeEvent(subtree, "numUpdate");
  if (isClear) {
    recordSubtreeEvent(subtree, "numClear");
  }
  XLOGF(
      INFO,
      "[FsdbSyncer] Updated retained subtree: subtree={}, action={}",
      subtree,
      isClear ? "clear" : "set");
}

fsdb::OperMetadata makeMetadata() {
  fsdb::OperMetadata metadata;
  metadata.lastConfirmedAt() =
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();
  return metadata;
}

/*
 * A patch leaf carrying a whole subtree value, serialized with the patch's
 * protocol (COMPACT).
 */
template <typename TC, typename T>
tcow::PatchNode valNode(const T& value) {
  tcow::PatchNode node;
  node.set_val(tcow::serializeBuf<TC>(fsdb::OperProtocol::COMPACT, value));
  return node;
}

tcow::PatchNode delNode() {
  tcow::PatchNode node;
  node.set_del();
  return node;
}

template <typename Struct, typename Ident>
int16_t fieldId() {
  return folly::to_underlying(
      apache::thrift::op::get_field_id_v<Struct, Ident>);
}

/*
 * Wrap a single BgpData field's PatchNode as a patch rooted at the bgp subtree.
 * basePath is the publisher root ({"bgp"}); the struct patch child is keyed by
 * the field's thrift id, matching FSDB's canonical patch encoding.
 */
fsdb::Patch bgpPatch(tcow::StructPatch structPatch) {
  tcow::PatchNode root;
  root.set_struct_node(std::move(structPatch));

  fsdb::Patch patch;
  patch.basePath() = bgpPath.tokens();
  patch.patch() = std::move(root);
  patch.protocol() = fsdb::OperProtocol::COMPACT;
  patch.metadata() = makeMetadata();
  return patch;
}

fsdb::Patch fullBgpPatch(const fsdb::BgpData& bgp) {
  fsdb::Patch patch;
  patch.basePath() = bgpPath.tokens();
  patch.patch() = valNode<tc::structure>(bgp);
  patch.protocol() = fsdb::OperProtocol::COMPACT;
  patch.metadata() = makeMetadata();
  return patch;
}

} // namespace

FsdbSyncer::FsdbSyncer(folly::EventBase& syncerEventBase)
    : syncerEventBase_(syncerEventBase),
      fsdbPubSubMgr_(
          std::make_unique<fboss::fsdb::FsdbPubSubManager>(kPublisherId)) {}

FsdbSyncer::~FsdbSyncer() {
  stop();
}

void FsdbSyncer::start() {
  XCHECK(!stopping_.load(std::memory_order_acquire))
      << "FsdbSyncer cannot restart after stop";
  if (!FLAGS_publish_state_to_fsdb) {
    XLOG(INFO, "[FsdbSyncer] FSDB state publication is disabled");
    return;
  }
  syncerEventBase_.runImmediatelyOrRunInEventBaseThreadAndWait([this]() {
    XCHECK(fsdbPubSubMgr_) << "FsdbSyncer cannot restart after stop";
    XCHECK(!publisherCreated_) << "FsdbSyncer::start called more than once";
    XLOG(INFO, "[FsdbSyncer] Starting /bgp FSDB patch publisher");
    fsdbPubSubMgr_->createStatePatchPublisher(
        bgpPath.tokens(), [this](auto oldState, auto newState) {
          onPublisherStateChanged(oldState, newState);
        });
    publisherCreated_ = true;
  });
}

void FsdbSyncer::stop() {
  XCHECK(!syncerEventBase_.isInEventBaseThread())
      << "FsdbSyncer::stop must run off the FsdbSyncer EventBase";
  if (stopping_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  bool initialSnapshotSubmitted{false};
  uint64_t publisherStateGeneration{0};
  /*
   * Closing publication under the same lock used by trySubmitPatch() waits for
   * any in-flight handoff to finish. Later queued work observes disconnected
   * state and cannot access the publisher.
   */
  {
    auto publication = publicationState_.wlock();
    initialSnapshotSubmitted = publication->initialSnapshotSubmitted;
    publisherStateGeneration = publication->generation;
    publication->connected = false;
    publication->initialSnapshotSubmitted = false;
    ++publication->generation;
  }
  XLOGF(
      INFO,
      "[FsdbSyncer] Stopping /bgp FSDB patch publisher: initialSnapshotSubmitted={}, publisherStateGeneration={}",
      initialSnapshotSubmitted,
      publisherStateGeneration);
  fsdbPubSubMgr_->removeStatePatchPublisher();
  /*
   * Publisher removal waits for stream callbacks to finish. This fence drains
   * handlers they queued, along with producer work queued before shutdown.
   */
  syncerEventBase_.runInEventBaseThreadAndWait(
      [this]() { publisherCreated_ = false; });
  fsdbPubSubMgr_.reset();
}

void FsdbSyncer::onPublisherStateChanged(
    fboss::fsdb::FsdbStreamClient::State oldState,
    fboss::fsdb::FsdbStreamClient::State newState) {
  /*
   * Publisher destruction reports CANCELLED synchronously while holding the
   * manager's publisher mutex. stop() has already invalidated publication, so
   * do not acquire publicationState_ in that callback. Patch submission takes
   * publicationState_ before the manager mutex.
   */
  if (newState == fboss::fsdb::FsdbStreamClient::State::CANCELLED) {
    return;
  }
  const bool connected =
      newState == fboss::fsdb::FsdbStreamClient::State::CONNECTED;
  uint64_t publisherStateGeneration;
  {
    auto publication = publicationState_.wlock();
    /*
     * stop() sets stopping_ before invalidating publication under this lock.
     * Checking here prevents an already-running callback from reopening
     * publication after stop() has closed it.
     */
    if (stopping_.load(std::memory_order_acquire)) {
      return;
    }
    publication->connected = connected;
    publication->initialSnapshotSubmitted = false;
    publisherStateGeneration = ++publication->generation;
  }
  enqueue([this, oldState, newState, publisherStateGeneration]() {
    handlePublisherStateChangeOnSyncerThread(
        oldState, newState, publisherStateGeneration);
  });
}

void FsdbSyncer::handlePublisherStateChangeOnSyncerThread(
    fboss::fsdb::FsdbStreamClient::State oldState,
    fboss::fsdb::FsdbStreamClient::State newState,
    uint64_t publisherStateGeneration) {
  const bool connected =
      newState == fboss::fsdb::FsdbStreamClient::State::CONNECTED;
  {
    const auto publication = publicationState_.rlock();
    if (publication->generation != publisherStateGeneration ||
        publication->connected != connected) {
      return;
    }
  }
  if (!connected) {
    if (oldState == fboss::fsdb::FsdbStreamClient::State::CONNECTED) {
      FsdbStatsDC::addFsdbSyncerLifecycleEvent("numDisconnect");
      XLOGF(
          INFO,
          "[FsdbSyncer] FSDB publisher disconnected: newState={}, publisherStateGeneration={}",
          folly::to_underlying(newState),
          publisherStateGeneration);
    } else {
      XLOGF(
          DBG2,
          "[FsdbSyncer] FSDB publisher state transition while disconnected: oldState={}, newState={}, publisherStateGeneration={}",
          folly::to_underlying(oldState),
          folly::to_underlying(newState),
          publisherStateGeneration);
    }
    return;
  }
  /*
   * FSDB treats the first patch after each CONNECTED transition as the
   * publisher's snapshot. Keep incremental publication disabled until the
   * authoritative owner can replace /bgp in one patch; otherwise a config or
   * policy patch could expose a partial snapshot to subscribers.
   */
  FsdbStatsDC::addFsdbSyncerLifecycleEvent("numConnect");
  XLOGF(
      INFO,
      "[FsdbSyncer] FSDB publisher connected: oldState={}, publisherStateGeneration={}",
      folly::to_underlying(oldState),
      publisherStateGeneration);
  publishRetainedStateSnapshot();
}

void FsdbSyncer::enqueue(folly::Function<void()> operation) {
  syncerEventBase_.runInEventBaseThread(std::move(operation));
}

void FsdbSyncer::setConfig(thrift::BgpConfig config) {
  enqueue([this, config = std::move(config)]() mutable {
    retained_.config = std::move(config);
    retained_.configDirty = true;
    recordRetainedUpdate(kConfig, /*isClear=*/false);
    publishPendingRetainedState();
  });
}

void FsdbSyncer::setRouteAttributePolicy(
    std::optional<rib_policy::TRouteAttributePolicy>&& routeAttributePolicy) {
  enqueue(
      [this, routeAttributePolicy = std::move(routeAttributePolicy)]() mutable {
        const bool isClear = !routeAttributePolicy.has_value();
        retained_.routeAttributePolicy = std::move(routeAttributePolicy);
        retained_.routeAttributePolicyDirty = true;
        recordRetainedUpdate(kRouteAttributePolicy, isClear);
        publishPendingRetainedState();
      });
}

void FsdbSyncer::setPathSelectionPolicy(
    std::optional<rib_policy::TPathSelectionPolicy>&& pathSelectionPolicy) {
  enqueue(
      [this, pathSelectionPolicy = std::move(pathSelectionPolicy)]() mutable {
        const bool isClear = !pathSelectionPolicy.has_value();
        retained_.pathSelectionPolicy = std::move(pathSelectionPolicy);
        retained_.pathSelectionPolicyDirty = true;
        recordRetainedUpdate(kPathSelectionPolicy, isClear);
        publishPendingRetainedState();
      });
}

void FsdbSyncer::setRouteFilterPolicy(
    std::optional<rib_policy::TRouteFilterPolicy>&& routeFilterPolicy) {
  enqueue([this, routeFilterPolicy = std::move(routeFilterPolicy)]() mutable {
    const bool isClear = !routeFilterPolicy.has_value();
    retained_.routeFilterPolicy = std::move(routeFilterPolicy);
    retained_.routeFilterPolicyDirty = true;
    recordRetainedUpdate(kRouteFilterPolicy, isClear);
    publishPendingRetainedState();
  });
}

void FsdbSyncer::setPartialDrainState(
    std::optional<bgp_thrift::TPartialDrainState>&& partialDrainState) {
  enqueue([this, partialDrainState = std::move(partialDrainState)]() mutable {
    const bool isClear = !partialDrainState.has_value();
    retained_.partialDrainState = std::move(partialDrainState);
    retained_.partialDrainStateDirty = true;
    recordRetainedUpdate(kPartialDrainState, isClear);
    publishPendingRetainedState();
  });
}

bool FsdbSyncer::trySubmitPatch(
    fsdb::Patch&& patch,
    PublicationKind kind,
    uint64_t expectedGeneration) {
  const bool isInitialSnapshot = kind == PublicationKind::INITIAL_SNAPSHOT;
  const auto publishStart = std::chrono::steady_clock::now();
  bool rejected{false};
  {
    auto publication = publicationState_.wlock();
    rejected = !publication->connected || !fsdbPubSubMgr_ ||
        publication->generation != expectedGeneration ||
        (isInitialSnapshot && publication->initialSnapshotSubmitted) ||
        (!isInitialSnapshot && !publication->initialSnapshotSubmitted);
    if (!rejected) {
      /*
       * The lock intentionally spans publishState(). FsdbPubSubManager selects
       * its current publisher inside this call; releasing the lock after the
       * checks would let a reconnect swap publishers and accept an old
       * incremental as the new stream's first patch. publishState() only
       * enqueues to the publisher pipe, so it cannot invoke the stream state
       * callback inline and re-enter this lock.
       */
      fsdbPubSubMgr_->publishState(std::move(patch));
      if (isInitialSnapshot) {
        publication->initialSnapshotSubmitted = true;
      }
    }
  }
  if (rejected) {
    if (isInitialSnapshot) {
      FsdbStatsDC::addFsdbSyncerLifecycleEvent("numStaleSnapshotDrop");
    } else {
      FsdbStatsDC::addFsdbSyncerLifecycleEvent("numRejectedIncremental");
    }
    return false;
  }
  FsdbStatsDC::STATS_fsdbSyncerPublishStateEnqueueTimeMs.addValue(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - publishStart)
          .count());
  return true;
}

void FsdbSyncer::publishRetainedStateSnapshot() {
  uint64_t expectedGeneration;
  {
    const auto publication = publicationState_.rlock();
    if (!publication->connected || publication->initialSnapshotSubmitted ||
        !fsdbPubSubMgr_) {
      return;
    }
    expectedGeneration = publication->generation;
  }

  fsdb::BgpData fullState;
  fullState.config() = retained_.config;
  if (retained_.routeAttributePolicy) {
    fullState.routeAttributePolicy() = *retained_.routeAttributePolicy;
  }
  if (retained_.pathSelectionPolicy) {
    fullState.pathSelectionPolicy() = *retained_.pathSelectionPolicy;
  }
  if (retained_.routeFilterPolicy) {
    fullState.routeFilterPolicy() = *retained_.routeFilterPolicy;
  }
  if (retained_.partialDrainState) {
    fullState.partialDrainState() = *retained_.partialDrainState;
  }
  if (!trySubmitPatch(
          fullBgpPatch(fullState),
          PublicationKind::INITIAL_SNAPSHOT,
          expectedGeneration)) {
    return;
  }

  clearRetainedDirty();

  FsdbStatsDC::addFsdbSyncerLifecycleEvent("numSnapshotPublish");
  XLOGF(
      INFO,
      "[FsdbSyncer] Submitted retained-state /bgp snapshot: publisherStateGeneration={}, routeAttributePolicy={}, pathSelectionPolicy={}, routeFilterPolicy={}, partialDrainState={}",
      expectedGeneration,
      fullState.routeAttributePolicy().has_value(),
      fullState.pathSelectionPolicy().has_value(),
      fullState.routeFilterPolicy().has_value(),
      fullState.partialDrainState().has_value());
}

void FsdbSyncer::publishPendingRetainedState() {
  uint64_t expectedGeneration;
  {
    const auto publication = publicationState_.rlock();
    if (!publication->connected || !publication->initialSnapshotSubmitted ||
        !fsdbPubSubMgr_) {
      return;
    }
    expectedGeneration = publication->generation;
  }

  const bool configDirty = retained_.configDirty;
  const bool routeAttributePolicyDirty = retained_.routeAttributePolicyDirty;
  const bool pathSelectionPolicyDirty = retained_.pathSelectionPolicyDirty;
  const bool routeFilterPolicyDirty = retained_.routeFilterPolicyDirty;
  const bool partialDrainStateDirty = retained_.partialDrainStateDirty;
  tcow::StructPatch bgpChanges;
  if (configDirty) {
    bgpChanges.children()[fieldId<fsdb::BgpData, k_fsdb_model::config>()] =
        valNode<tc::structure>(retained_.config);
  }
  if (routeAttributePolicyDirty) {
    bgpChanges.children()
        [fieldId<fsdb::BgpData, k_fsdb_model::routeAttributePolicy>()] =
        retained_.routeAttributePolicy
        ? valNode<tc::structure>(*retained_.routeAttributePolicy)
        : delNode();
  }
  if (pathSelectionPolicyDirty) {
    bgpChanges.children()
        [fieldId<fsdb::BgpData, k_fsdb_model::pathSelectionPolicy>()] =
        retained_.pathSelectionPolicy
        ? valNode<tc::structure>(*retained_.pathSelectionPolicy)
        : delNode();
  }
  if (routeFilterPolicyDirty) {
    bgpChanges
        .children()[fieldId<fsdb::BgpData, k_fsdb_model::routeFilterPolicy>()] =
        retained_.routeFilterPolicy
        ? valNode<tc::structure>(*retained_.routeFilterPolicy)
        : delNode();
  }
  if (partialDrainStateDirty) {
    bgpChanges
        .children()[fieldId<fsdb::BgpData, k_fsdb_model::partialDrainState>()] =
        retained_.partialDrainState
        ? valNode<tc::structure>(*retained_.partialDrainState)
        : delNode();
  }

  if (bgpChanges.children()->empty()) {
    return;
  }
  if (!trySubmitPatch(
          bgpPatch(std::move(bgpChanges)),
          PublicationKind::INCREMENTAL,
          expectedGeneration)) {
    return;
  }

  clearRetainedDirty();

  FsdbStatsDC::addFsdbSyncerLifecycleEvent("numIncrementalPublish");
  if (configDirty) {
    recordSubtreeEvent(kConfig, "numIncrementalPublish");
  }
  if (routeAttributePolicyDirty) {
    recordSubtreeEvent(kRouteAttributePolicy, "numIncrementalPublish");
  }
  if (pathSelectionPolicyDirty) {
    recordSubtreeEvent(kPathSelectionPolicy, "numIncrementalPublish");
  }
  if (routeFilterPolicyDirty) {
    recordSubtreeEvent(kRouteFilterPolicy, "numIncrementalPublish");
  }
  if (partialDrainStateDirty) {
    recordSubtreeEvent(kPartialDrainState, "numIncrementalPublish");
  }
}

bool FsdbSyncer::isIncrementalPublicationReady() const {
  const auto publication = publicationState_.rlock();
  return publication->connected && publication->initialSnapshotSubmitted;
}

void FsdbSyncer::clearRetainedDirty() {
  retained_.configDirty = false;
  retained_.routeAttributePolicyDirty = false;
  retained_.pathSelectionPolicyDirty = false;
  retained_.routeFilterPolicyDirty = false;
  retained_.partialDrainStateDirty = false;
}

} // namespace facebook::bgp
