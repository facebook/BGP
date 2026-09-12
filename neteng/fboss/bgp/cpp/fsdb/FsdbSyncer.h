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

#include <atomic>
#include <memory>
#include <optional>

#include <folly/Function.h>
#include <folly/Synchronized.h>
#include <folly/io/async/EventBase.h>

#include "fboss/fsdb/client/FsdbPubSubManager.h"
#include "fboss/fsdb/if/FsdbModel.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_route_types_types.h"

namespace bgp_thrift = facebook::neteng::fboss::bgp::thrift;

DECLARE_bool(publish_rib_to_fsdb);

namespace facebook::bgp {

/*
 * Publishes BgpData subtrees to FSDB via the raw FsdbPubSubManager patch API
 * (createStatePatchPublisher + publishState(Patch)). Unlike FsdbSyncManager it
 * keeps no local copy-on-write tree. The small config, policy, and
 * partial-drain subtrees are retained as plain values on a dedicated EventBase
 * so they can be re-published on reconnect.
 *
 * FSDB drops any publish issued before the publisher reaches CONNECTED and
 * resets the publish queue on every disconnect, so publishers must re-sync
 * their full state on each CONNECTED. Public setters only enqueue work onto
 * the FsdbSyncer EventBase. FsdbSyncer retains the latest small values and
 * publishes them without involving the RIB computation boundary.
 *
 * Threading: retained state, patch assembly, and publication run only on the
 * caller-supplied syncerEventBase_. FSDB lifecycle callbacks normally run on
 * the publisher stream thread. Each callback records the new stream state,
 * advances PublicationState::generation, and clears initialSnapshotSubmitted
 * under the lock, then queues follow-up work onto the FsdbSyncer thread.
 * Retained state remains confined to the FsdbSyncer thread; only
 * publicationState_ is shared.
 */
class FsdbSyncer {
 public:
  /**
   * Construct FsdbSyncer on a caller-owned EventBase.
   *
   * @param syncerEventBase Serial executor for retained-state updates, patch
   *     construction, and calls to FsdbPubSubManager.
   *
   * The EventBase must remain alive and running through stop() and destruction.
   * FsdbSyncer drains its own work but never terminates the EventBase.
   */
  explicit FsdbSyncer(folly::EventBase& syncerEventBase);
  ~FsdbSyncer();

  FsdbSyncer(const FsdbSyncer&) = delete;
  FsdbSyncer& operator=(const FsdbSyncer&) = delete;
  FsdbSyncer(FsdbSyncer&&) = delete;
  FsdbSyncer& operator=(FsdbSyncer&&) = delete;

  /**
   * Queue and retain the current BGP configuration. Submit it incrementally if
   * the current connection already has its initial snapshot; otherwise include
   * it in the next initial snapshot.
   *
   * @param config Complete configuration value.
   */
  void setConfig(thrift::BgpConfig config);

  /**
   * Queue an update that replaces or clears the route-attribute policy subtree
   * and retain the latest state. Submit it incrementally if the current
   * connection already has its initial snapshot; otherwise include it in the
   * next initial snapshot.
   *
   * @param routeAttributePolicy New value, or std::nullopt to delete the field.
   */
  void setRouteAttributePolicy(
      std::optional<rib_policy::TRouteAttributePolicy>&& routeAttributePolicy);

  /**
   * Queue an update that replaces or clears the path-selection policy subtree
   * and retain the latest state. Submit it incrementally if the current
   * connection already has its initial snapshot; otherwise include it in the
   * next initial snapshot.
   *
   * @param pathSelectionPolicy New value, or std::nullopt to delete the field.
   */
  void setPathSelectionPolicy(
      std::optional<rib_policy::TPathSelectionPolicy>&& pathSelectionPolicy);

  /**
   * Queue an update that replaces or clears the route-filter policy subtree and
   * retain the latest state. Submit it incrementally if the current connection
   * already has its initial snapshot; otherwise include it in the next initial
   * snapshot.
   *
   * @param routeFilterPolicy New value, or std::nullopt to delete the field.
   */
  void setRouteFilterPolicy(
      std::optional<rib_policy::TRouteFilterPolicy>&& routeFilterPolicy);

  /**
   * Queue an update that replaces or clears the device partial-drain state
   * subtree and retain the latest state. Submit it incrementally if the current
   * connection already has its initial snapshot; otherwise include it in the
   * next initial snapshot.
   *
   * @param partialDrainState New value, or std::nullopt to delete the field.
   */
  void setPartialDrainState(
      std::optional<bgp_thrift::TPartialDrainState>&& partialDrainState);

  /**
   * Return whether the current connection has submitted its initial snapshot.
   */
  bool isIncrementalPublicationReady() const;

  /**
   * Create the /bgp patch publisher and begin handling FSDB connections.
   * This is a one-shot lifecycle operation; an instance cannot be restarted
   * after start() or stop(). This is a no-op when state publication is
   * disabled.
   */
  void start();

  /**
   * Destroy the publisher and drain its callbacks. Safe to call repeatedly
   * during single-threaded teardown. External producers, notably RibDC, must
   * already be quiesced. Must be called off the FsdbSyncer EventBase while it
   * is still running so queued operations and publisher callbacks are drained
   * before the object can be destroyed. Does not terminate the caller-owned
   * EventBase.
   */
  void stop();

 private:
#ifdef FSDB_SYNCER_TEST_FRIENDS
  FSDB_SYNCER_TEST_FRIENDS
#endif
  /**
   * Callback registered with the FSDB publisher for state transitions.
   *
   * FsdbStreamClient invokes this inline from setState(). Normal connection
   * transitions arrive on the publisher stream EventBase. Teardown cancellation
   * arrives synchronously on the thread removing the publisher and is ignored
   * because stop() has already invalidated publication. Other transitions
   * record the new stream state, advance the locked publisher-state generation,
   * clear initialSnapshotSubmitted, and queue the remaining work onto the
   * FsdbSyncer thread.
   *
   * @param oldState Publisher stream state before the transition.
   * @param newState Publisher stream state after the transition.
   */
  void onPublisherStateChanged(
      fboss::fsdb::FsdbStreamClient::State oldState,
      fboss::fsdb::FsdbStreamClient::State newState);

  /**
   * Handle a publisher state transition on the FsdbSyncer thread.
   *
   * This is queued by onPublisherStateChanged(); it is not another publisher
   * callback. The connection state has already changed before it runs. It
   * ignores superseded generations, updates lifecycle state and observability,
   * and publishes or requests the required snapshot.
   *
   * @param oldState Publisher stream state before the transition.
   * @param newState Publisher stream state after the transition.
   * @param publisherStateGeneration Generation captured synchronously by
   *     onPublisherStateChanged(); stale queued transitions are ignored.
   */
  void handlePublisherStateChangeOnSyncerThread(
      fboss::fsdb::FsdbStreamClient::State oldState,
      fboss::fsdb::FsdbStreamClient::State newState,
      uint64_t publisherStateGeneration);

  /**
   * Schedule work on the FsdbSyncer thread without blocking the caller.
   *
   * Main synchronously quiesces the RIB producer before stopping FsdbSyncer,
   * so no retained-state setter can race teardown. stop() separately rejects
   * and drains publisher callbacks before the caller-owned EventBase is
   * stopped.
   *
   * @param operation Work to execute serially on the FsdbSyncer EventBase.
   */
  void enqueue(folly::Function<void()> operation);

  /**
   * Classifies a patch by its ordering role on the current FSDB connection.
   * The initial snapshot must be submitted before any incremental patch.
   */
  enum class PublicationKind {
    INITIAL_SNAPSHOT,
    INCREMENTAL,
  };

  /**
   * Submit an already-built patch when it is valid for the current connection.
   *
   * @param patch For `INITIAL_SNAPSHOT`, a complete `/bgp` replacement. For
   *     `INCREMENTAL`, a patch rooted at `/bgp` or one of its subtrees.
   * @param kind `INITIAL_SNAPSHOT` requires that this connection has not yet
   *     submitted its complete state and enables later incremental submission.
   *     `INCREMENTAL` requires that the initial snapshot was already submitted.
   * @param expectedGeneration Publisher-state generation captured before
   *     building the patch. A different generation rejects stale work.
   * @return True when the current connection, generation, and ordering checks
   *     pass and `publishState()` is called. This is not bounded-queue
   *     acceptance or an FSDB acknowledgement.
   *
   * The publisher-state lock covers both validation and the `publishState()`
   * handoff. This prevents a disconnect/reconnect from swapping the manager's
   * current publisher between those two operations and receiving an
   * incremental patch before its initial snapshot.
   */
  bool trySubmitPatch(
      fboss::fsdb::Patch&& patch,
      PublicationKind kind,
      uint64_t expectedGeneration);

  /**
   * Build and submit the initial `/bgp` snapshot from retained state.
   *
   * This layer owns config, policy, and partial-drain state. Route-state fields
   * are intentionally left unset; their producer is separate from
   * retained-state publication. trySubmitPatch() performs the shared connection
   * and ordering checks before submission.
   */
  void publishRetainedStateSnapshot();

  /**
   * Combine all dirty retained subtrees into one incremental patch and clear
   * their dirty bits only after trySubmitPatch() hands the patch to
   * FsdbPubSubManager.
   */
  void publishPendingRetainedState();

  /** Clear dirty bits after retained state is included in a submitted patch. */
  void clearRetainedDirty();

  /*
   * Retained copies of the small subtrees, re-published on each reconnect.
   * BgpData requires config, so it starts default-constructed. An absent
   * optional means that policy or partial-drain subtree is unset.
   */
  struct RetainedState {
    thrift::BgpConfig config;
    std::optional<rib_policy::TRouteAttributePolicy> routeAttributePolicy;
    std::optional<rib_policy::TPathSelectionPolicy> pathSelectionPolicy;
    std::optional<rib_policy::TRouteFilterPolicy> routeFilterPolicy;
    std::optional<bgp_thrift::TPartialDrainState> partialDrainState;
    bool configDirty{false};
    bool routeAttributePolicyDirty{false};
    bool pathSelectionPolicyDirty{false};
    bool routeFilterPolicyDirty{false};
    bool partialDrainStateDirty{false};
  };

  struct PublicationState {
    bool connected{false};
    bool initialSnapshotSubmitted{false};
    uint64_t generation{0};
  };

  folly::EventBase& syncerEventBase_;
  std::unique_ptr<fboss::fsdb::FsdbPubSubManager> fsdbPubSubMgr_;
  bool publisherCreated_{false};
  /* Rejects publisher callbacks once teardown begins. */
  std::atomic<bool> stopping_{false};
  /*
   * Serializes stream state changes with patch validation and the publishState
   * handoff so a patch cannot cross from one stream generation into the next.
   */
  folly::Synchronized<PublicationState> publicationState_;
  RetainedState retained_;
};

} // namespace facebook::bgp
