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
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

#include <folly/Function.h>
#include <folly/Synchronized.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>
#include <gflags/gflags.h>

#include "fboss/fsdb/client/FsdbPubSubManager.h"
#include "fboss/fsdb/if/FsdbModel.h"
#include "neteng/fboss/bgp/cpp/fsdb/CanonicalRibUpdateQueue.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_route_types_types.h"

namespace bgp_thrift = facebook::neteng::fboss::bgp::thrift;

namespace facebook::fboss::thrift_cow {
class StructPatch;
}

DECLARE_bool(publish_rib_to_fsdb);

namespace facebook::bgp {

class CanonicalRibExporter;
struct CanonicalRibFullSnapshot;
struct CanonicalRibIncrementalPayload;

/*
 * Publishes BgpData subtrees to FSDB via the raw FsdbPubSubManager patch API
 * (createStatePatchPublisher + publishState(Patch)). Unlike FsdbSyncManager it
 * keeps no local copy-on-write tree. The small config, policy, and
 * partial-drain subtrees are retained as plain values on a dedicated EventBase
 * so they can be re-published on reconnect.
 *
 * Canonical RIB publication follows this data path:
 *
 *   RibDC -> CanonicalRibUpdateQueue -> FsdbSyncer
 *       -> CanonicalRibExporter -> FsdbSyncer patch assembly
 *       -> FsdbPubSubManager -> FSDB.
 *
 * CanonicalRibExporter owns encoding and canonical-pool lifetime but never
 * publishes. FsdbSyncer owns queue consumption, connection ordering, patch
 * assembly, and the final handoff to FsdbPubSubManager.
 *
 * FSDB drops any publish issued before the publisher reaches CONNECTED and
 * resets the publish queue on every disconnect, so publishers must re-sync
 * their full state on each CONNECTED. Public setters only enqueue work onto
 * the FsdbSyncer EventBase. FsdbSyncer retains the latest small values and
 * publishes them without involving the RIB computation boundary. When
 * canonical RIB publication is enabled, FsdbSyncer also drains immutable
 * per-prefix inputs, performs encoding and interning, and publishes the
 * resulting patches. The RIB sees only a nullable queue capability and never
 * observes FSDB connection state.
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
   * @param publishRibToFsdb Whether to create the canonical RIB update channel
   *     and encoder. When false, reconnect publishes retained state directly.
   *
   * The EventBase must remain alive and running through stop() and destruction.
   * FsdbSyncer drains its own work but never terminates the EventBase.
   */
  explicit FsdbSyncer(
      folly::EventBase& syncerEventBase,
      bool publishRibToFsdb = FLAGS_publish_rib_to_fsdb);
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
   * Register the RIB-side producer for canonical snapshots.
   *
   * This startup-only operation returns a nullable capability. When canonical
   * RIB publication is disabled, the return value is null and the callback is
   * not retained. Otherwise the callback is invoked on the FsdbSyncer thread
   * whenever an authoritative full-RIB walk is required. The callback must
   * only schedule work on the RIB EventBase; it must not walk or mutate the
   * RIB inline. The scheduled producer captures one complete owning snapshot
   * input and enqueues it as a single full-snapshot message.
   *
   * @param requestFullSnapshot Lightweight callback that schedules an
   *     authoritative walk on the RIB EventBase.
   * @return Producer queue when canonical publication is enabled, or nullptr
   *     when it is disabled.
   */
  std::shared_ptr<CanonicalRibUpdateQueue> registerCanonicalRibSource(
      folly::Function<void()> requestFullSnapshot);

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
   *     building the patch. When set, a different current generation rejects
   *     stale work.
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
      std::optional<uint64_t> expectedGeneration);

  /**
   * Build and submit the initial `/bgp` snapshot from retained state.
   *
   * This layer owns config, policy, and partial-drain state. Route-state fields
   * (`ribMap` and `canonicalRib`) are intentionally left unset. This is the
   * complete snapshot when canonical RIB publication is disabled; the
   * canonical path uses publishCanonicalFullSnapshot() instead.
   * trySubmitPatch() performs the shared connection and ordering checks before
   * submission.
   */
  void publishRetainedStateSnapshot();

  /**
   * Combine all dirty retained subtrees into one incremental patch and clear
   * their dirty bits only after trySubmitPatch() hands the patch to
   * FsdbPubSubManager.
   */
  void publishPendingRetainedState();

  /**
   * Merge canonical and dirty retained changes into one incremental `/bgp`
   * patch and submit it for the current connection.
   *
   * @param bgpChanges Canonical child changes, or an empty patch for a
   *     retained-state-only update.
   * @param includesCanonicalRib Whether successful accounting should include
   *     the canonical RIB subtree.
   * @param expectedGeneration Optional publisher-state generation captured
   *     while accumulating canonical updates.
   * @return True when a non-empty patch was submitted for the current
   *     connection.
   */
  bool publishIncrementalPatch(
      fboss::thrift_cow::StructPatch&& bgpChanges,
      bool includesCanonicalRib,
      std::optional<uint64_t> expectedGeneration = std::nullopt);

  /**
   * Convert one exporter payload into the canonical incremental patch shape.
   *
   * @param payload Encoded pool replacements and final per-prefix changes.
   * @param expectedGeneration Publisher-state generation captured before the
   *     corresponding queue turn was encoded.
   * @return True when the combined canonical and retained patch was submitted.
   */
  bool publishCanonicalIncremental(
      CanonicalRibIncrementalPayload&& payload,
      uint64_t expectedGeneration);

  /**
   * Compose retained state with a canonical build and submit the complete
   * `/bgp` snapshot.
   *
   * @param snapshot Canonical build to consume for this publication attempt.
   * @param expectedGeneration Publisher generation for which the RIB walk was
   *     requested.
   * @return True when the complete snapshot was submitted.
   */
  bool publishCanonicalFullSnapshot(
      CanonicalRibFullSnapshot&& snapshot,
      uint64_t expectedGeneration);

  /**
   * Process one canonical queue item, then yield the FsdbSyncer thread before
   * dequeuing another. Prefix updates accumulate until the queue drains or the
   * current batch has been open for 200 ms.
   */
  folly::coro::Task<void> processCanonicalRibUpdates() noexcept;

  /**
   * Encode and submit one complete RIB-thread snapshot.
   * A response requested for an older publisher generation is discarded before
   * encoding, then a fresh snapshot is requested for the current connection.
   *
   * @param snapshot Owning input captured atomically on the RIB thread.
   */
  void processCanonicalFullSnapshot(CanonicalRibFullSnapshotInput snapshot);

  /**
   * Encode one prefix update into the current incremental publication batch.
   *
   * @param update Complete state for one prefix, or a withdrawal.
   */
  void accumulateCanonicalPrefixUpdate(CanonicalRibPrefixUpdate&& update);

  /** Publish and clear the accumulated canonical incremental batch. */
  void publishCanonicalIncrementalBatch();

  /** Clear the exporter and its pending incremental-publication metadata. */
  void resetCanonicalExporterAndPendingIncremental();

  /**
   * Request one authoritative RIB walk unless another request is outstanding.
   *
   * @param publisherStateGeneration Connected publisher generation requiring
   *     the snapshot.
   */
  void requestCanonicalFullSnapshot(uint64_t publisherStateGeneration);

  /** Build the current retained config, policy, and partial-drain state. */
  fboss::fsdb::BgpData makeRetainedSnapshot() const;

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

  struct PendingCanonicalIncrementalPublication {
    std::chrono::steady_clock::time_point firstUpdateTime;
    uint64_t publisherGeneration;
  };

  struct CanonicalRibPublicationProgress {
    /*
     * Only one untagged RIB snapshot request may be outstanding. Preserve its
     * generation across disconnect so its eventual response can be rejected
     * before requesting a fresh snapshot for the current connection.
     */
    std::optional<uint64_t> outstandingFullSnapshotRequestGeneration;
    std::optional<PendingCanonicalIncrementalPublication>
        pendingIncrementalPublication;
  };

  folly::EventBase& syncerEventBase_;
  std::unique_ptr<fboss::fsdb::FsdbPubSubManager> fsdbPubSubMgr_;
  std::shared_ptr<CanonicalRibUpdateQueue> canonicalRibUpdateQueue_;
  std::unique_ptr<CanonicalRibExporter> canonicalRibExporter_;
  CanonicalRibPublicationProgress canonicalRibPublicationProgress_;
  folly::Function<void()> requestCanonicalFullSnapshot_;
  folly::coro::CancellableAsyncScope canonicalRibConsumerTasks_;
  std::atomic<bool> started_{false};
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
