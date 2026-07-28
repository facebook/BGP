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

#include <string_view>

#include <folly/IPAddress.h>
#include <folly/Portability.h>
#include <folly/coro/AsyncScope.h>
#include <folly/coro/Baton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/adjrib/RouteFilterLogger.h"
#include "neteng/fboss/bgp/cpp/adjrib/ShadowRibTypes.h"
#include "neteng/fboss/bgp/cpp/adjrib/UpdateGroupManager.h"
#include "neteng/fboss/bgp/cpp/changeTracker/ChangeItem.h"
#include "neteng/fboss/bgp/cpp/changeTracker/ChangeTracker.h"
#include "neteng/fboss/bgp/cpp/changeTracker/ConsumerBitmap.h"
#include "neteng/fboss/bgp/cpp/common/BgpModuleBase.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/BgpPeerDisplayInfo.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeerManager.h"
#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredModule.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"
#include "neteng/fboss/bgp/if/gen-cpp2/BgpStructs_types.h"
#include "neteng/fboss/bgp/if/gen-cpp2/TBgpServiceStream.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_stream_types.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

#include <gflags/gflags_declare.h>

DECLARE_int32(peer_start_delay_ms);
DECLARE_int32(min_conn_retry_time_ms);
DECLARE_int32(max_conn_retry_time_ms);
DECLARE_int32(conn_timeout_ms);
DECLARE_int32(min_session_retry_time_ms);
DECLARE_int32(max_session_retry_time_ms);
DECLARE_int32(max_session_dampen_time_ms);
DECLARE_int32(counter_update_time_s);
DECLARE_string(gr_state_file);
DECLARE_string(safemode_file);

namespace facebook {
namespace bgp {
class ConfigManager;
struct BgpPeerConfig;

struct StreamSubscriber {
  explicit StreamSubscriber(nettools::bgplib::BgpPeerId peerId)
      : peerId(peerId) {}

  nettools::bgplib::BgpPeerId peerId;
  std::shared_ptr<nettools::bgplib::FiberBgpPeer::InputQueueT> peerInputQ =
      std::make_shared<nettools::bgplib::FiberBgpPeer::InputQueueT>();

  std::shared_ptr<nettools::bgplib::FiberBgpPeer::BoundedInputQueueT>
      boundedPeerInputQ =
          std::make_shared<nettools::bgplib::FiberBgpPeer::BoundedInputQueueT>(
              nettools::bgplib::kMaxEgressQueueSize,
              nettools::bgplib::kEgressQueueHighWatermark,
              nettools::bgplib::kEgressQueueLowWatermark);

  std::shared_ptr<nettools::bgplib::FiberBgpPeer::OutputQueueT> peerOutputQ =
      std::make_shared<nettools::bgplib::FiberBgpPeer::OutputQueueT>(
          nettools::bgplib::kMaxIngressQueueSize);

  std::unique_ptr<apache::thrift::ServerStreamPublisher<
      neteng::fboss::bgp::thrift::TBgpRouteDelta>>
      publisher;

  neteng::fboss::bgp::thrift::TBgpPeerState state;
  std::chrono::steady_clock::time_point upSince;
  uint32_t numFlaps{0};
  uint32_t publisherId{0};
};

struct GrLoadResult {
  bool loaded;
  std::unique_ptr<std::unordered_set<nettools::bgplib::BgpPeerId>> peers;

  static GrLoadResult NotLoaded(void) {
    return {.loaded = false, .peers = nullptr};
  }
};

class PeerManagerBase : public BgpModuleBase, public MonitoredModule {
 public:
  //
  // Creates PeerManagerBase instance with given configuration
  //
  PeerManagerBase(
      std::shared_ptr<ConfigManager> configManager,
      const std::shared_ptr<PolicyManager> policyManager,
      nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
      MonitoredMPMCQueue<RibOutMessage>& ribOutQ,
      std::optional<MonitoredMPMCQueue<NeighborWatcherMessage>>&
          nbrRouteChangeQ,
      // When true (DC production): defer notifyRibInitialPathComputation
      // until both all-peer-EORs AND the RibOutNexthopResolutionReceived
      // signal from RIB are observed — required for switches with
      // conditional routes so the initial syncFib doesn't wipe FibAgent's
      // GR-retained conditional routes. When false (default — EBB
      // production, PM-only tests, e2e tests with TestRib): the NDP
      // precondition is pre-satisfied at construction.
      bool requireNexthopResolution = false,
      std::chrono::milliseconds minConnRetryDur_ =
          std::chrono::milliseconds(FLAGS_min_conn_retry_time_ms),
      std::chrono::milliseconds maxConnRetryDur_ =
          std::chrono::milliseconds(FLAGS_max_conn_retry_time_ms),
      std::chrono::milliseconds connTimeout =
          std::chrono::milliseconds(FLAGS_conn_timeout_ms),
      std::chrono::milliseconds minSessionRetryDur_ =
          std::chrono::milliseconds(FLAGS_min_session_retry_time_ms),
      std::chrono::milliseconds maxSessionRetryDur_ =
          std::chrono::milliseconds(FLAGS_max_session_retry_time_ms),
      std::chrono::milliseconds maxSessionDampenDur_ =
          std::chrono::milliseconds(FLAGS_max_session_dampen_time_ms));

  virtual ~PeerManagerBase();

  //
  // Bgp peer manager main event loop. This creates child fibers.
  //
  virtual void run() noexcept override;

  //
  // Terminates peer connections and kills all fibers.
  //
  void stop() noexcept override;

  /**
   * @brief Mark all AdjRibs for BGP daemon shutdown
   *
   * @details When called before stop(), this allows
   * sessionTerminated() to skip expensive O(n) cleanup operations that would
   * otherwise cause systemd timeout during shutdown.
   */
  void markDaemonShutdown();

  /**
   * @brief Save GR state information to a file before restarting.
   *
   * @details Must be called before SessionManager::stop() triggers
   * sessionTerminated() which clears establishedGrPeers_.
   */
  void saveGrState();

  void setSessionManager(std::shared_ptr<SessionManager> sessionManager);

  const std::shared_ptr<SessionManager>& getSessionManager() {
    return sessionMgr_;
  }

  /*
   * A global tracker tracking changes from the RIB
   * Every changes to the ShadowRib must be published to this change list
   * tracker
   */
  const std::shared_ptr<ChangeTracker<ShadowRibEntry>>& getChangeListTracker() {
    return changeListTracker_;
  }

  // Various UI/CLI/Thrift service handlers
  // These will be invoked from a service handler thread, will be
  // scheduled to run in appropriate thread
  // Get pre/post in/out networks information
  virtual void getNetworks(
      std::map<
          facebook::neteng::fboss::bgp_attr::TIpPrefix,
          facebook::neteng::fboss::bgp::thrift::TBgpPath>& prefixToPath,
      const std::unique_ptr<std::string>& peer,
      const RouteFilterType& type,
      const std::optional<std::unique_ptr<std::string>>& dryRunConfigFileName =
          std::nullopt) noexcept;

  virtual void getNetworks(
      std::map<
          facebook::neteng::fboss::bgp_attr::TIpPrefix,
          facebook::neteng::fboss::bgp::thrift::TBgpPath>& prefixToPath,
      const std::unique_ptr<std::string>& peer,
      const std::unique_ptr<std::string>& sessionBgpId,
      const RouteFilterType& type,
      const std::optional<std::unique_ptr<std::string>>& dryRunConfigFileName =
          std::nullopt) noexcept;

  // Various UI/CLI/Thrift service handlers
  // These will be invoked from a service handler thread, will be
  // scheduled to run in appropriate thread
  // Get pre/post in/out networks information
  virtual void getNetworks2(
      std::map<
          facebook::neteng::fboss::bgp_attr::TIpPrefix,
          std::vector<facebook::neteng::fboss::bgp::thrift::TBgpPath>>&
          prefixToPath,
      const std::unique_ptr<std::string>& peer,
      const RouteFilterType& type,
      const std::optional<std::unique_ptr<std::string>>& dryRunConfigFileName =
          std::nullopt) noexcept;

  virtual void getNetworks2(
      std::map<
          facebook::neteng::fboss::bgp_attr::TIpPrefix,
          std::vector<facebook::neteng::fboss::bgp::thrift::TBgpPath>>&
          prefixToPath,
      const std::unique_ptr<std::string>& peer,
      const std::unique_ptr<std::string>& sessionBgpId,
      const RouteFilterType& type,
      const std::optional<std::unique_ptr<std::string>>& dryRunConfigFileName =
          std::nullopt) noexcept;

  void getSubscriberNetworks(
      std::map<
          facebook::neteng::fboss::bgp_attr::TIpPrefix,
          std::vector<facebook::neteng::fboss::bgp::thrift::TBgpPath>>&
          prefixToPath,
      const int32_t peerID,
      const RouteFilterType& type) noexcept;

  // Render BgpPeerDisplayInfos into TBgpSessions (with adjRibs enrichment)
  virtual std::vector<neteng::fboss::bgp::thrift::TBgpSession> getSessionInfos(
      const std::unordered_multimap<
          folly::IPAddress,
          std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>>&
          allPeers) noexcept;

  std::vector<neteng::fboss::bgp::thrift::TBgpStreamSession>
  getBgpStreamSummary() noexcept;

  // Render BgpPeerDisplayInfos into detailed TBgpSessions
  std::vector<neteng::fboss::bgp::thrift::TBgpSession> getDetailSessionInfos(
      const std::unordered_multimap<
          folly::IPAddress,
          std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>>&
          allPeers) noexcept;

  neteng::fboss::bgp::thrift::TBgpSession getDetailSessionInfo(
      const folly::IPAddress& ipAddr,
      const std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>& peerInfo,
      const std::shared_ptr<bgp::BgpGlobalConfig>& bgpGlobalConfig) noexcept;

  // common code to get bgp summary or neighbors
  neteng::fboss::bgp::thrift::TBgpSession getSessionInfo(
      const folly::IPAddress& peerAddr,
      const std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>&
          peerInfo) noexcept;

  /* Get detailed update group information for CLI/thrift. */
  std::vector<neteng::fboss::bgp::thrift::TUpdateGroupInfo> getUpdateGroupInfo(
      std::optional<int64_t> groupIdFilter = std::nullopt);

  /* Get egress statistics on all peers. */
  std::vector<neteng::fboss::bgp::thrift::TPeerEgressStats> getPeerEgressStats(
      std::unordered_multimap<
          folly::IPAddress,
          std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>> allPeers);

  /* Get hold timer information on all peers. */
  std::vector<neteng::fboss::bgp::thrift::THoldTimerInfo> getHoldTimerInfos(
      const std::unordered_multimap<
          folly::IPAddress,
          std::shared_ptr<nettools::bgplib::BgpPeerDisplayInfo>>&
          allPeers) noexcept;

  // Highest RibVersion seen across all RibOutAnnouncement/RibOutWithdrawal
  // entries processed from the RIB.
  uint64_t getMaxRibVersion() const noexcept {
    return maxRibVersion_;
  }

  // Get current shadowRibEntries by address family
  std::vector<neteng::fboss::bgp::thrift::TRibEntry> getShadowRibEntries(
      neteng::fboss::bgp_attr::TBgpAfi afi);

  // Get current shadowRibEntries by address family in canonical form
  neteng::fboss::bgp::thrift::TCanonicalRibState getShadowRibEntriesCanonical(
      neteng::fboss::bgp_attr::TBgpAfi afi);

  // Get current changeList entries by address family
  std::vector<neteng::fboss::bgp::thrift::TRibEntry> getChangeListEntries(
      neteng::fboss::bgp_attr::TBgpAfi afi);

  // Get current changeList entries by address family in canonical form
  neteng::fboss::bgp::thrift::TCanonicalRibState getChangeListEntriesCanonical(
      neteng::fboss::bgp_attr::TBgpAfi afi);

  /**
   * Utility method to turn a RibEntry to tRibEntry
   * entry: prefix and associated ribEntry
   */
  neteng::fboss::bgp::thrift::TRibEntry createTRibEntry(
      const std::pair<const folly::CIDRNetwork, facebook::bgp::ShadowRibEntry>&
          entry);

  /**
   * Utility method to turn a RibEntry to tRibEntry after applying pathFilter.
   *
   * entry: prefix and associated ribEntry
   * pathFilter: unary predicate to check if a path should be in o/p list.
   */
  std::optional<neteng::fboss::bgp::thrift::TRibEntry>
  createTRibEntryWithFilter(
      const std::pair<const folly::CIDRNetwork, facebook::bgp::ShadowRibEntry>&
          entry,
      const std::function<bool(const RouteInfo&)>& pathFilter);

  // get attribute stats
  neteng::fboss::bgp::thrift::TAttributeStats getAttributeStats() noexcept;

  /**
   * Get BGP path attribute statistics with optional filtering.
   * Returns aggregated statistics (use count, community entries, AS path
   * length, etc.) for BGP path attributes that match the provided filter
   * criteria.
   *
   * @param filter Optional filter to restrict which attributes are included
   *               in the statistics (e.g., filter by peer, prefix, etc.)
   * @return TAttributeStats containing aggregated statistics for the filtered
   *         attributes
   */
  neteng::fboss::bgp::thrift::TAttributeStats getAttributeStatsFiltered(
      const std::unique_ptr<neteng::fboss::bgp::thrift::TAttributeStatsFilter>&
          filter) noexcept;

  // get policy stats
  void getPolicyStats(
      neteng::routing::policy::thrift::TPolicyStats& stats) const noexcept {
    if (policyManager_) {
      return policyManager_->getPolicyStats(stats);
    }
  }

  // get policy manager
  const std::shared_ptr<PolicyManager>& getPolicyManager() const noexcept {
    return policyManager_;
  }

  // update entry stats
  void updateEntryStats(
      neteng::fboss::bgp::thrift::TEntryStats& stats) const noexcept;

  // Return an active stream of Multi-path routes from RIB
  apache::thrift::ServerStream<neteng::fboss::bgp::thrift::TBgpRouteDelta>
  subscribe(const std::unique_ptr<std::string>& subscriberName);

  // public for unit test
  void addPeersToSessionMgr();

  /**
   * Add peers to the session manager at runtime in a single coroutine.
   * All peers are added sequentially on the SessionManager's evb.
   *
   * @param peerConfigs: Vector of peer configurations to add
   * @return folly::Expected with Unit on success, or ErrorCode on first failure
   */
  folly::coro::Task<folly::Expected<
      folly::Unit,
      nettools::bgplib::FiberBgpPeerManager::ErrorCode>>
  addPeers(const std::vector<std::shared_ptr<BgpPeerConfig>>& peerConfigs);

  /**
   * Remove peers from the session manager at runtime in a single coroutine.
   * Each peer is dropped sequentially on the SessionManager's evb via
   * co_dropPeer().
   *
   * @param peerAddrs: Vector of peer IP addresses to remove
   * @return folly::Expected with Unit on success, or ErrorCode on first failure
   */
  folly::coro::Task<folly::Expected<
      folly::Unit,
      nettools::bgplib::FiberBgpPeerManager::ErrorCode>>
  delPeers(const std::vector<folly::IPAddress>& peerAddrs);

  /**
   * Number of established subscriber sessions.
   *
   * @return Number of established subscriber sessions.
   */
  uint32_t numStreamSubscribers(void);

  /**
   * Checks if the number of established stream peers exceeds the stream
   * susbscriber limit.
   *
   * @return true if the number of established stream peers exceeds the stream
   * subscriber limit, false otherwise.
   */
  bool exceedsStreamSubscriberLimit();

  void logEoRPeers(const bool isIngressEoR) noexcept;

  void setRouteFilterPolicy(
      std::unique_ptr<RouteFilterPolicy> policy,
      bool forceUpdate = false) noexcept;
  void clearGoldenPrefixesPolicy() noexcept;
  void clearIngressEgressRouteFiltersPolicy() noexcept;

  void updateIngressEgressPolicyNames(
      std::unique_ptr<PeerToPolicyMap> peerToPolicyNames) noexcept;

  bool getIsInitialized() const;
  bool getIsSafeModeOn() const;
  void removeSafeModeFile();
  bool getIsGoldenPrefixPolicyActive() const;

  const std::unique_ptr<RouteFilterPolicy>& getRouteFilterPolicy() const {
    return routeFilterPolicy_;
  }

  std::string buildAdjRibOutGroupName(
      const nettools::bgplib::BgpPeerId& peerId,
      const PeeringParams& peeringParams) noexcept;

  /**
   * @brief Triggers a route refresh request for multiple peers and returns a
   * list of failed peer IDs.
   * @param peerIds: list of peerIds to trigger route refresh request for
   * @return list of peerIds that route refresh request failed to be sent to
   */
  std::vector<nettools::bgplib::BgpPeerId> triggerRouteRefreshRequestsForPeers(
      std::vector<nettools::bgplib::BgpPeerId> peerIds);

 protected:
  void scheduleCoroTasks() noexcept;

  /*
   * EoR-convergence timer policy seam. createAndScheduleTimers() CREATES the
   * timers (on the evb thread, from run()) and installs the startup policy: the
   * base creates eorTimer_ + initializedMaxWaitTimer_ and arms eorTimer_ for
   * the configured eor_time_s (DC/OSS), treating session establishment as a
   * no-op (onSessionEstablishedEorHook). Platform subclasses (e.g.
   * PeerManagerBB) override these to instead create + arm their own
   * boot-relative safety-net timer and defer the eor_time_s wait to the first
   * established session. createEorTimer()/createInitializedMaxWaitTimer() are
   * the shared make helpers (single source of the AsyncTimeout wiring);
   * scheduleTimer() is the shared, guarded arm helper (no-op if the timer is
   * null) that also logs the timer name + duration, used directly to arm
   * eorTimer_ (protected), initializedMaxWaitTimer_, and the BB timer.
   */
  virtual void createAndScheduleTimers() noexcept;
  virtual void onSessionEstablishedEorHook() noexcept {}
  void createEorTimer() noexcept;
  void createInitializedMaxWaitTimer() noexcept;

  /*
   * Shared arm helper: schedules `timer` for `duration` (no-op if null, e.g.
   * reset after convergence-notify / stop(), or not yet created without run()),
   * logging `name` and the duration. Returns true iff the timer was (re-)armed.
   */
  bool scheduleTimer(
      std::unique_ptr<folly::AsyncTimeout>& timer,
      std::string_view name,
      std::chrono::seconds duration) noexcept;

  /*
   * Shared EoR-convergence timeout handler: force initial RIB path computation
   * (once) and log the peers whose EoR was still outstanding. Invoked by the
   * base eorTimer_, and by platform max-wait timers (e.g. PeerManagerBB's
   * boot-relative RIB-computation timer) so all paths drive the same one-shot
   * convergence kick-off.
   */
  void onEorConvergenceTimeout() noexcept;

  /*
   * initializedMaxWaitTimer_ callback: publishes the INITIALIZED signal (the
   * last line of defense) and latches initialized_. Runs when that timer fires.
   */
  void onInitializedMaxWaitTimeout() noexcept;

  /*
   * EoR wait timer: bounds the wait for peers' End-of-RIB before forcing
   * initial RIB path computation. Protected (not private) because a platform
   * subclass arms it — PeerManagerBB creates + arms it on the first established
   * session via createEorTimer() + scheduleTimer(). Created in
   * createAndScheduleTimers() on the evb thread; callback is
   * onEorConvergenceTimeout().
   */
  std::unique_ptr<folly::AsyncTimeout> eorTimer_;

  /*
   * Cached EoR wait (eor_time_s) as a duration, read once from config at
   * construction. Drives the EoR-timer seam above and the initialized-signal
   * max cap; protected so platform subclasses (e.g. PeerManagerBB) can derive
   * their timer policy without reaching into the private config.
   */
  const std::chrono::seconds eorWaitDuration_;

  // Peer session manager
  std::shared_ptr<SessionManager> sessionMgr_;

  // A tracker for tracking ShadowRibEntries_ updates
  std::shared_ptr<ChangeTracker<ShadowRibEntry>> changeListTracker_{nullptr};

  // Bitmap of add-path capable consumers for selective notification
  ConsumerBitmap addPathConsumerBitmap_;

  // Bitmap of non-add-path consumers for bestpath changes
  ConsumerBitmap nonAddPathConsumerBitmap_;

  /*
   * Get the appropriate consumer bitmap for a change.
   * - Bestpath changes use nonAddPathConsumerBitmap_
   * - Multipath changes use addPathConsumerBitmap_
   * ChangeTracker handles ORing if item is already on changelist
   */
  const ConsumerBitmap& getConsumerBitmapForChange(bool isBestpathChange);

  /*
   * Test-only: defer the shadow RIB walk for a specific peer.
   * When deferred, processRibDumpReq re-buffers the request instead of
   * walking, keeping that peer in DETACHED_INIT_DUMP.
   */
  void testOnlySetDeferInitDump(const folly::IPAddress& peerAddr, bool defer) {
    evb_.runInEventBaseThreadAndWait([this, peerAddr, defer]() {
      for (auto& [peerId, adjRib] : adjRibs_) {
        if (peerId.peerAddr == peerAddr) {
          adjRib->testOnlyDeferInitDump = defer;
        }
      }
    });
  }

  /*
   * Test-only: defer DRJ acceptance for a specific peer.
   * Delegates to the update group manager's shared groups (not per-peer
   * adjRibOutGroups_). maybeAcceptDSPPeer runs on the shared update
   * group, so the deferred flag must be set there.
   */
  void testOnlySetDeferDrjAcceptance(
      const folly::IPAddress& peerAddr,
      bool defer) {
    evb_.runInEventBaseThreadAndWait([this, peerAddr, defer]() {
      if (updateGroupManager_) {
        updateGroupManager_->testOnlySetDeferDrjAcceptance(peerAddr, defer);
      }
    });
  }

 private:
  // Struct to accumulate attribute statistics
  struct AttributeStatsAccumulator {
    uint64_t totalUseCount{0};
    uint64_t totalCommunityEntries{0};
    uint64_t totalExtCommunityEntries{0};
    uint64_t totalAsPathLen{0};
    uint64_t totalClusterListLen{0};
    uint64_t totalTopologyInfoLen{0};
  };

  void getAttributeStatsHelper(
      const std::shared_ptr<const BgpPath>& attr,
      std::unordered_set<std::shared_ptr<const BgpPath>>& allAttributes,
      std::unordered_set<
          std::shared_ptr<const BgpPath>,
          facebook::bgp::BgpPath::Hash,
          facebook::bgp::BgpPath::Compare>& uniqueAttributes,
      AttributeStatsAccumulator& accumulator) noexcept;

  folly::coro::Task<void> updatePeerCounters();

  /**
   * Coroutine to re-evaluate all adjRibs by applying policies to
   * all the valid (non-stale) prefixes in the adjRib. During this operation,
   * best-path computation and FIB programming are paused in the RIB.
   *
   * Triggered under any of the following cases:
   * 1. BGP++ enters safe mode during prefix overload scenarios.
   * 2. Ingress RouteFilterPolicy is updated.
   * 3. Ingress routing policy is updated.
   *
   * @param cause: Reason for triggering re-evaluation and
   * pausing/resuming best-path computation and Fib programming
   */
  folly::coro::Task<void> startAdjRibReEvaluationRoutine(
      RibPauseResumeCause cause);

  /**
   * Coroutine to process a single AdjRib re-evaluation including policy
   * application and cleanup operations. This method provides cooperative
   * scheduling to avoid blocking the event loop during re-evaluation.
   *
   * @param adjRib: The AdjRib to process
   * @param cause: The cause for re-evaluation
   * @param policyUpdate: Whether this is a policy update operation
   */
  folly::coro::Task<void> processAdjRibReEvaluationTask(
      std::shared_ptr<AdjRib> adjRib,
      RibPauseResumeCause cause,
      bool policyUpdate);

  /**
   * Helper coroutine to coordinate ingress policy change by issuing a
   * re-evaluation and egress policy change by issuing RibDumpReq
   *
   * @param ingressAffectedCount: Number of peers affected by ingress policy
   * change
   * @param egressAffectedCount: Number of peers affected by egress policy
   */
  folly::coro::Task<void> processIngressAndEgressRouteFilterUpdate(
      size_t ingressAffectedCount,
      size_t egressAffectedCount);

  // Handle an egress route filter/routing policy update: schedule egress
  // policy re-evaluation for affected update groups (or per-peer re-evaluation
  // when update groups are disabled).
  void handleEgressPolicyUpdate();

  // Collect update groups with pending egress policy re-evaluation
  folly::F14NodeSet<std::shared_ptr<AdjRibOutGroup>>
  getPolicyReEvalPendingGroups();

  // Schedule per-peer egress policy re-evaluation (non-update-group mode)
  void schedulePolicyReEvalForAdjRibs();

  /*
   * Fleet-wide egress policy re-evaluation for update groups. Rebuilds the
   * UpdateGroupKey of every affected group's members, then reconciles group
   * membership in one pass: re-keys in-place groups whose shared (non-override)
   * key changed, and moves/splits per-peer override peers into the group
   * matching their new key, scheduling the necessary group/detached RIB walks.
   */
  folly::coro::Task<void> processUpdateGroupsEgressPolicyReevaluation();

  /*
   * Orchestrate group-level + detached peer egress policy re-evaluation.
   * Runs synchronously (no suspension points).
   */
  void processGroupEgressPolicyReEvaluation(
      std::shared_ptr<AdjRibOutGroup> group);

  /*
   * Re-evaluate a single peer's egress policy inline: cancel any scheduled rib
   * dump, walk the shadow RIB, then consume the peer's change list to the end
   * so its RIB-OUT is fully rebuilt within this event-loop turn. Does NOT clear
   * the peer's pending-egress-policy flag -- callers do that.
   */
  void processDetachedPeerEgressPolicyReEvaluation(
      const std::shared_ptr<AdjRib>& adjRib);

  /* Helper method to distribute RibOutAnnouncement to all adjRibs */
  void distributeRibOutAnnouncementToAdjRibs(
      const RibOutAnnouncement& announcement);

  // Helper coroutine to process RibDumpReq for a specific peer during egress
  // policy update (non-update-group path).
  folly::coro::Task<void> processRibDumpReqForEgressPolicyUpdate(
      const nettools::bgplib::BgpPeerId& peerId,
      std::shared_ptr<AdjRib> adjRib);

  // Coroutine to periodically evict all stale entries from policy cache
  folly::coro::Task<void> startPeriodicPolicyCacheEvictionRoutine();

  // Coroutine to periodically update peer counter
  folly::coro::Task<void> startPeriodicUpdatePeerCountersRoutine();

  // Coroutine to handle RibDumpReq
  // NOTE: we intended to do the pass-by-value to prevent memory use-after-free
  folly::coro::Task<void> processRibDumpReqCoro(RibDumpReq ribDumpReq);
  /*
   * Cancellable variant used by scheduleRibDumpForAdjRib for update-group
   * peers: the dump is tracked by the AdjRib's cancellation source so it can be
   * superseded or cancelled on teardown.
   */
  folly::coro::Task<void> processRibDumpReqWithCancellationCoro(
      std::shared_ptr<AdjRib> adjRib);
  void processRibDumpReq(
      const std::shared_ptr<AdjRib>& adjRib,
      bool sendAddPath,
      bool sendWithEoR);

  /*
   * Whether a rib dump is pending for this peer in either form: buffered in
   * pendingRibDumpReqs_ (waiting for the drain) or scheduled / in flight on
   * asyncScope_ (its cancellation source is armed). Pure predicate.
   */
  bool isRibDumpScheduledForAdjRib(const std::shared_ptr<AdjRib>& adjRib) const;

  /*
   * Cancel a peer's pending rib dump regardless of state: drop a buffered
   * request from pendingRibDumpReqs_ and cancel an armed/in-flight one via the
   * AdjRib's cancellation source. Counterpart to scheduleRibDumpForAdjRib.
   */
  void cancelRibDumpForAdjRib(const std::shared_ptr<AdjRib>& adjRib);

  /*
   * Schedule a rib dump for a peer on asyncScope_, but only if one is not
   * already scheduled or in flight (checked via isRibDumpScheduledForAdjRib).
   * The AdjRib owns the cancellation source so teardown can cancel the dump;
   * its token is passed into asyncScope_.add().
   */
  void scheduleRibDumpForAdjRib(const std::shared_ptr<AdjRib>& adjRib);

  /**
   * When sessionEstablished is called, the peer should eventually
   * receive the routes from this BGP speaker's RIB.
   * There are two vehicles to get the Rib dump.
   *
   * A) Peer calls for a RibDumpReq through PeerManagerBase who serves
   * the @shadowRibEntries_ that were received from RIB while handling
   * RibOutAnnouncements processed from ribOutQ_ in processRibOutMsgLoop.
   *
   * B) In the same method processRibOutMsgLoop, we are also sending
   * announcements to peers as we receive them.
   *
   * Case 1: The peer comes up before Rib has started initial announcement.
   * Then the peer should depend on vehicle B to get the RIB dump.
   *
   * Case 2: The peer comes up after Rib has finished initial announcement.
   * Then the peer should depend on vehicle A to get the RIB dump.
   *
   * Case 3: The peer comes up while Rib is in initial announcement.
   * Suppose Rib has announced the first K of N announcements.
   *
   * With vehicle A, peer will only get 1 to K and then EOR because shadow
   * RIB only has 1 to K.
   * With vehicle B, peer will only get K + 1 to N and then EOR because
   * 1 to K were already processed and dequeued.
   * We cannot only use one, but if we try to use both (i.e., let peer request
   * RibDumpReq while Rib is also announcing) we will send two EOR.
   *
   * Solution for case 3 is to submit a RibDumpReq after Rib has finished
   * initial announcement.
   *
   * This method handles all three cases above.
   */
  void maybeBufferRibDumpReq(const std::shared_ptr<AdjRib>& adjRib);

  /**
   * All RibDumpReqs that were buffered during initial announcement
   * will be scheduled as coro tasks per peer in this method.
   */
  folly::coro::Task<void> handleBufferedRibDumpReqs();

  /**
   * Update-group equivalent of maybeBufferRibDumpReq for detached peers that
   * must catch up independently before joining their group. Buffers the AdjRib
   * in pendingRibDumpAdjRibs_ to be served once the initial announcement is
   * done.
   */
  void maybeBufferRibDumpForDetachedPeer(const std::shared_ptr<AdjRib>& adjRib);

  /**
   * Update-group equivalent of handleBufferedRibDumpReqs. Drains the buffered
   * detached-peer RibDumpReqs in pendingRibDumpAdjRibs_ one at a time,
   * co_awaiting each per-peer dump inline with a small sleep between dumps to
   * pace the ShadowRib walks.
   */
  folly::coro::Task<void> handleBufferedRibDumpsForDetachedPeers();

  /**
   * @brief Determine if this is a dynamic peer.
   *
   * @param peerAddr The IP address of the peer.
   *
   * @return True if the peer is dynamic, false otherwise.
   */
  bool isPeerDynamic(const folly::IPAddress& peerAddr);

  // process observable event received from adj rib notify queue
  folly::coro::Task<void> processAdjRibMsgLoop() noexcept;
  virtual folly::coro::Task<void> processAdjRibEvent(
      AdjRib::ObservableMessageT&& evt) noexcept;

  // process Rib message and send to target AdjRibs
  folly::coro::Task<void> processRibOutMsgLoop() noexcept;

  // coro tasks for periodic publish all outstanding updates to
  // thrift stream subscribers
  folly::coro::Task<void> publishUpdatesRoutine();
  folly::coro::Task<void> publishUpdates();

  // coro task to process neighbor event from FSDB
  // Bring down session if no arp/ndp entry resolution
  folly::coro::Task<void> processNeighborRouteChangeLoop() noexcept;

  // Handlers for different NeighborWatcherMessages
  // received in processNeighborRouteChangeLoop.
  virtual folly::coro::Task<void> handleNeighborEventMsg(
      const NeighborEventMsg& msg) noexcept;
  virtual folly::coro::Task<void> handleNeighborReachabilityMsg() noexcept;

  /**
   * @brief Process neighbor route change event for peerAddr during
   * initialization to improve GR convergence time
   *
   * @param peerId peerId of the peer
   */
  void processNeighborRouteChangeDuringInitialization(
      const nettools::bgplib::BgpPeerId& peerId) noexcept;

  // Coroutine to process peer events from session manager's notify queue
  folly::coro::Task<void> processPeerEventLoop() noexcept;

  // Wait for session termination baton (coroutine version)
  folly::coro::Task<void> waitForSessionTerminateBaton(
      const nettools::bgplib::BgpPeerId& peerId) noexcept;

  // Full peer state cleanup after session termination (Phase 3).
  // Waits for AdjRib message loops to exit, then erases all per-peer state.
  // Must be co_awaited from delPeers before returning SUCCESS.
  folly::coro::Task<void> cleanupPeerState(
      const nettools::bgplib::BgpPeerId& peerId,
      const folly::IPAddress& peerAddr) noexcept;

  // Process session established state event for peerAddr (coroutine version)
  folly::coro::Task<void> sessionEstablished(
      nettools::bgplib::FiberBgpPeer::ObservableStateT stateEvt) noexcept;

  // Process session terminated state event for peerAddr.
  folly::coro::Task<void> sessionTerminated(
      const nettools::bgplib::FiberBgpPeer::ObservableStateT&
          stateEvt) noexcept;

  /**
   * Create adjRibOutGroup
   */
  std::shared_ptr<AdjRibOutGroup> createAdjRibOutGroup(
      const std::string& groupName) noexcept;

  /**
   * Find adjRibOutGroup
   */
  std::shared_ptr<AdjRibOutGroup> findAdjRibOutGroup(
      const std::string& groupName) noexcept;

  // Create adjRib
  std::shared_ptr<AdjRib> createAdjRib(
      const nettools::bgplib::BgpPeerId& peerId,
      const PeeringParams& peeringParams) noexcept;

  // Find adjRib
  std::shared_ptr<AdjRib> findAdjRib(
      const nettools::bgplib::BgpPeerId& peerId) noexcept;

  void resetSubscriberAdjRib(StreamSubscriber& subscriber);
  void setSubscriberAdjRib(
      StreamSubscriber& subscriber,
      std::shared_ptr<AdjRib>& adjRib);

  void cancelSubscriberStream(
      nettools::bgplib::BgpPeerId peerId,
      const std::string& subscriberName,
      uint32_t publisherId);

  /*
   * [AdjRib Event]
   *
   * AdjRib(per session) currently sends 3 types of messages to PeerManagerBase:
   *  - AdjRib::EoR: this is th INGRESS_EOR received from peers via socket;
   *  - AdjRib::EgressEoR: this is the EGRESS_EOR received from local adjribs
   *                       to mark initialization event;
   *  - AdjRib::Shutdown: this is the explicit signal to shut down peer;
   *  - AdjRib::TriggerSafeMode: this is the explicit signal to trigger safe
   * mode when total path scale or unique prefix limit is reached.
   */

  // Process EoR received from this peer and notify RIB if we received all EoRs
  void processPeerEoR(const nettools::bgplib::BgpPeerId& peerId) noexcept;

  // Process EgressEoR to mark successful sending EgressEoR towards this peer
  void processEgressEoR(const nettools::bgplib::BgpPeerId& peerId) noexcept;

  // Process TriggerSafeMode generated internally from AdjRib when total
  // path scale or unique prefix limit is reached.
  void processTriggerSafeMode() noexcept;

  // Util function to check if all EoR received from expected peers
  // and notify RIB if not already notified
  void checkAndNotifyAllEoRReceived() noexcept;

  // Called from the ribOutQ_ processing loop when the
  // RibOutNexthopResolutionReceived signal arrives from RIB. Marks the
  // NDP-received precondition as satisfied and notifies RIB to start
  // initial path computation if all peer EORs have also been received.
  // See `nexthopResolutionReceived_` / `allPeerEorsReceived_` for the
  // two-precondition gating rationale (prevents the initial syncFib from
  // wiping FibAgent's GR-retained conditional routes).
  void handleRibOutNexthopResolutionReceived() noexcept;

  // Calls notifyRibInitialPathComputation only when both peer EORs and the
  // NeighborWatcher initial-sync signal have been observed. The eorTimer_
  // callback bypasses this helper and calls notifyRibInitialPathComputation
  // directly with timerFired=true as the unconditional max-cap.
  void maybeNotifyRibInitialPathComputation() noexcept;

  // Util function to check if all EoR sent to expected peers
  bool checkAllEoRSent();

  // Util function to check initial Fib sync is done and Rib is
  // finished with initial announcement.
  bool isRibInitialAnnouncementStart();

  // Move peer-manager state to ribInitialAnnouncementDone_
  // and handle certain work as a result of reaching to this
  // state
  void markRibInitialAnnouncementDone() noexcept;

  // One-time signal to notify RIB to start best-path computation.
  void notifyRibInitialPathComputation(bool timerFired) noexcept;

  // We are possibly at the stage where we can declare all the
  // initialization complete
  void maybeMarkInitialized() noexcept;

  // Update non-graceful peer counters (state changed for peerAddr)
  void updateNonGracefulCounters(
      const folly::IPAddress& peerAddr,
      bool isTerminated) noexcept;

  /**
   * @brief Helper method for updateIngressEgressPolicyNames to process each
   * adjRib
   *
   * @param adjRib Shared pointer to the AdjRib to update policy names for
   * @param peerToPolicyNames Map from peer IP address to direction-specific
   * policy names (std::nullopt means clear, value means set)
   * @return std::tuple<bool, bool> A tuple containing (ingressChanged,
   * egressChanged)
   *         - first element: true if ingress policy name was changed, false
   * otherwise
   *         - second element: true if egress policy name was changed, false
   * otherwise
   *
   */
  std::tuple<bool, bool> updateIngressEgressPolicyNamesForAdjRib(
      std::shared_ptr<AdjRib> adjRib,
      const PeerToPolicyMap& peerToPolicyNames) noexcept;

  // Read saved GR state. Returns nullptr if no valid previous state exits.
  GrLoadResult readGrState() const noexcept;

  /**
   * @brief Creates and initializes peering parameters for thrift stream
   *        subscribers.
   *
   * Stream subscribers receive BGP route updates via thrift streaming rather
   * than standard BGP sessions. This function creates a synthetic PeeringParams
   * configuration with sensible defaults for these virtual peers:
   * - Uses localhost (::1) as the peer address
   * - Enables both IPv4 and IPv6 address families
   * - Configures as route reflector client with ADD-PATH send capability
   *
   * @return A pair containing:
   *         - folly::IPAddress: The peer address (::1 localhost)
   *         - std::unique_ptr<PeeringParams>: The initialized peering
   * parameters
   */
  std::pair<folly::IPAddress, std::unique_ptr<PeeringParams>>
  getStreamPeeringParams();

  /*
   * Apply (or, with a null policy, clear) the route filter policy and
   * propagate it to affected adjRibs. MUST be called on the evb_ thread;
   * setRouteFilterPolicy() is the public entry point that schedules this.
   */
  void applyRouteFilterPolicy(
      std::unique_ptr<RouteFilterPolicy> policy,
      bool forceUpdate,
      bool applyGoldenPrefixPolicy = true) noexcept;

  std::tuple<bool, bool> setRouteFilterStatement(
      std::shared_ptr<AdjRib> adjRib) noexcept;
  bool setGoldenPrefixPolicy(
      std::shared_ptr<AdjRib> adjRib,
      bool initializeAdjRib = false) noexcept;

  /**
   * @brief: Schedule periodic evict from deduplicators coroutine on
   * CancellableAsyncScope.
   */
  folly::coro::Task<void> periodicEvictFromDeduplicatorLoop() noexcept;

  /**
   * Trigger route refresh request for a peer under the following conditions:
   * 1. Peer is in established state.
   * 2. Enhanced route refresh is negotiated with the peer.
   * 3. Enhanced route refresh is not already in progress.
   */
  bool triggerRouteRefreshRequestForPeer(
      const nettools::bgplib::BgpPeerId& peerId) noexcept;

  void processChangeItemCompleteCallback(
      TrackableObject<ShadowRibEntry>* trackedObject);

  /*
   * @brief: create/update shadow rib entries to local shadowRib collection
   *
   * @param: RibOutAnnouncement from ribOutQ to create/update local collection
   */
  void handleShadowRibEntryAnnouncement(const RibOutAnnouncement& announcement);

  /*
   * @brief: delete shadow rib entries to local shadowRib collection
   *
   * @param: RibOutWithdrawal from ribOutQ to remove local collection
   */
  void handleShadowRibEntryWithdrawal(const RibOutWithdrawal& withdrawal);

  PathId getPathId(const RibOutAnnouncementEntry& entry);

  std::optional<PathId> getPathId(const RibOutWithdrawalEntry& entry);
  /*
   * @brief: util function to update the common attribute of shadow rib entries
   *
   * @param: srEntry - this is the ShadowRibEntry to be updated
   * @param: entry - this is RibOut entry used as the SoT
   */
  void updateShadowRibEntryUtil(
      ShadowRibEntry& srEntry,
      const RibOutAnnouncementEntry& entry);

  folly::IPAddress streamPeerAddr_;

  std::unique_ptr<PeeringParams> streamPeeringParams_;
  folly::F14NodeMap<
      std::string, /* subscriberName */
      StreamSubscriber>
      streamSubscribers_;

  uint32_t lastStreamPeerId_{0};

  // Config Manager
  std::shared_ptr<ConfigManager> configManager_;

  // Track last applied policy version for race avoidance
  // Only accessed from EVB thread (same as adjRibs_)
  uint64_t lastAppliedPolicyVersion_{0};

  // Policy Manager
  const std::shared_ptr<PolicyManager> policyManager_;

  // Route Filter Policy
  std::unique_ptr<RouteFilterPolicy> routeFilterPolicy_{nullptr};

  // Builds per-statement route filter loggers; null when logging is
  // unavailable.
  std::unique_ptr<RouteFilterLoggerFactory> routeFilterLoggerFactory_{nullptr};

  /*
   * [AdjRib/PeerManagerBase -> Rib]
   *
   * ribInQ_ represents the uni-directional flow from AdjRib/PeerManagerBase ->
   * Rib
   *
   * As the name suggests, Rib will be the ONLY reader of the queue;
   *
   * Message types can be:
   *  - RibInAnnouncement;
   *  - RibInWithdrawal;
   *  - RibDumpReq;
   *  - RibInEnfOfRib;
   */
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ_;

  /*
   * [Rib -> AdjRib/PeerManagerBase]
   *
   * ribOutQ_ represents the uni-directional flow from Rib ->
   * AdjRib/PeerManagerBase
   *
   * As the name suggests, Rib will be the ONLY writer of the queue;
   *
   * Message types can be:
   *  - RibOutAnnouncement;
   *  - RibOutWithdrawal;
   */
  MonitoredMPMCQueue<RibOutMessage>& ribOutQ_;

  /*
   * [NeighborWatcher -> PeerManagerBase]
   *
   * Wait on nbr route events if supported
   */
  std::optional<MonitoredMPMCQueue<NeighborWatcherMessage>>& nbrRouteChangeQ_;

  /*
   * [AdjRib -> PeerManagerBase]
   *
   * fromAdjRibQ_ is the MPSC (Multiple-Producer-Single-Consumer) queue between
   * multiple adjribs and peer-manager to transmit multiple adjRib events:
   *  - INGRESS_EOR;
   *  - EGRESS_EOR;
   *  - SHUTDOWN;
   *
   * ATTN: both INGRESS and EGRESS EoR information will be tracked using the
   * same collection of peers.
   */
  MonitoredMPMCQueue<AdjRib::ObservableMessageT> fromAdjRibQ_;

  // [Static Peer]: tracking INGRESS + EGRESS EoR status
  folly::F14NodeMap<
      folly::IPAddress,
      std::pair<bool /* ingress EoR */, bool /* egress EoR */>>
      staticPeerEoRReceived_{};

  // [Dynamic Peer]: tracking INGRESS + EGRESS EoR status
  //
  // NOTE: Dynamic peers can have multiple peerings with the same ip address
  // but different remote bgp peerId.
  folly::F14NodeMap<
      nettools::bgplib::BgpPeerId,
      std::pair<bool /* ingress EoR */, bool /* egress EoR */>>
      dynamicPeerEoRReceived_{};

  // Each peer adjacency rib which is created based on configuration
  folly::F14NodeMap<nettools::bgplib::BgpPeerId, std::shared_ptr<AdjRib>>
      adjRibs_;

  /**
   * Collection of Adjacency Groups created for to be advertised routes
   */
  folly::F14NodeMap<std::string, std::shared_ptr<AdjRibOutGroup>>
      adjRibOutGroups_;

  /**
   * Update group manager for dynamic peer grouping based on egress policies.
   * Manages lifecycle of update groups and peer-to-group associations.
   */
  std::unique_ptr<UpdateGroupManager> updateGroupManager_;

  // Stores peerAddr to peerIds mapping
  folly::F14NodeMap<
      folly::IPAddress /* peerAddr */,
      std::unordered_set<nettools::bgplib::BgpPeerId>>
      peerAddrToIds_;

  /*
   * [Shadow Rib]
   *
   * Shadow rib collection is the data structure which stores the:
   *
   * prefix -> ShadowRibEntry
   *
   * This collection will be used to handle RibDumpReq independently without
   * adding extra burdens to ribOut message queue.
   */
  ShadowRibEntriesMap shadowRibEntries_;

  /*
   * Highest RibVersion observed across every RibOutAnnouncement and
   * RibOutWithdrawal entry processed from the RIB. Survives an emptied shadow
   * RIB (unlike a version derived by walking @shadowRibEntries_). Only accessed
   * from the EVB thread (same as @shadowRibEntries_).
   */
  uint64_t maxRibVersion_{0};

  void setMaxRibVersion(uint64_t ribVersion) noexcept {
    maxRibVersion_ = std::max(maxRibVersion_, ribVersion);
  }

  // AdjRib will post this baton when session is terminated (both message
  // processing loops have completed). Used to do sequential synchronization
  // between adjRib and peerManager. Latch semantics: passes through between
  // post() and reset(), ensuring rapid session flaps don't hang.
  folly::F14NodeMap<
      nettools::bgplib::BgpPeerId,
      std::shared_ptr<folly::coro::Baton>>
      sessionTerminateBatons_;

  //
  // Timers
  //

  // max cap count-down to publish initialized signal
  std::unique_ptr<folly::AsyncTimeout> initializedMaxWaitTimer_;

  // One-time flag, best-path start signal should only be sent once
  bool ribInitPathComputationNotified_{false};
  // One-time flag, maintain state of Rib's flag initialEorSent_
  bool ribInitialAnnouncementStarted_{false};
  // One-time flag, all egress EoRs should only be sent once
  bool allEgressEoRSent_{false};
  // One-time flag, mark BGP++ initialized after restart
  std::atomic<bool> initialized_{false};
  /*
   * Flag indicating daemon shutdown is in progress. Set by
   * markDaemonShutdown() before SessionManager::stop() to prevent
   * cross-module coroutine calls to a stopped SessionManager evb.
   */
  std::atomic<bool> daemonShutdown_{false};
  // One-time flag, mark true when all initial announcements are done
  bool ribInitialAnnouncementDone_{false};
  // One-time flag to prevent scheduling multiple handleBufferedRibDumpReqs
  // tasks
  bool handleRibDumpsScheduled_{false};

  /**
   * One-time collection of pending RibDumpReqs from peers who come up
   * and call sessionEstablished during initial announcement.
   * Note that we only maintain the latest RibDumpReq from a peer;
   * this means there is only one RibDumpReq per peer.
   *
   * Because RibDumpReqs are read from @shadowRibEntries_, we can
   * only serve RibDumpReqs once we are certain that
   * Shadow RIB and RIB are in sync. This map stores the RibDumpReqs
   * that are requested during the initial announcement (i.e.
   * while initialAnnouncementDone_ = false); these will be processed
   * once initialAnnouncementDone_ = true.
   */
  folly::F14NodeMap<nettools::bgplib::BgpPeerId, bool /* sendAddPath */>
      pendingRibDumpReqs_;

  /**
   * Update-group equivalent of pendingRibDumpReqs_. One-time collection of
   * AdjRibs with a pending RibDumpReq from detached peers that come up and must
   * catch up independently before joining their group. Held as a set of AdjRib
   * shared_ptrs; since there is one AdjRib per peer, this keeps at most one
   * pending RibDumpReq per peer.
   */
  folly::F14FastSet<std::shared_ptr<AdjRib>> pendingRibDumpAdjRibs_;

  //
  // Stats
  //

  // Running Bgp sessions
  uint32_t runningSessions_{0};
  uint32_t runningVipSessions_{0};

  // Peers in established state with GR capability
  std::unordered_set<nettools::bgplib::BgpPeerId> establishedGrPeers_;

  bool grStateSaved_{false};
  bool grStateLoaded_{false};

  // Connection retry params
  const std::chrono::milliseconds minConnRetryDur_;
  const std::chrono::milliseconds maxConnRetryDur_;
  const std::chrono::milliseconds connTimeout_;
  const std::chrono::milliseconds minSessionRetryDur_;
  const std::chrono::milliseconds maxSessionRetryDur_;
  const std::chrono::milliseconds maxSessionDampenDur_;

  // This is used to eliminate false alarms in convergence time measurement.
  // In case EOR timeout fired, it indicates some peer went down and so
  // measured convergence doesn't indicate true convergence but timeout value.
  bool eorTimerExpired_{false};

  /*
   * Two preconditions for notifying RIB to start initial path computation.
   * notifyRibInitialPathComputation(timerFired=false) is only sent when both
   * flags are true; the eorTimer_ callback bypasses these and fires
   * unconditionally as the max-cap fallback.
   *
   * Set on the PeerManagerBase evb thread:
   *  - allPeerEorsReceived_: flipped inside checkAndNotifyAllEoRReceived
   *    after all expected static + dynamic peers have sent EOR.
   *  - nexthopResolutionReceived_: flipped by the
   *    RibOutNexthopResolutionReceived signal pushed by RIB after the first
   *    NexthopResolutionUpdate has been processed (conditional routes
   *    advertised, if any).
   *
   * The NDP precondition prevents the initial syncFib from running before
   * conditional routes are in RIB, which would otherwise wipe FibAgent's
   * GR-retained conditional routes on BGP daemon restart.
   */
  bool allPeerEorsReceived_{false};
  // Initial value derived from PeerManagerBase's requireNexthopResolution ctor
  // parameter: false → gate pre-satisfied at construction (EBB / tests
  // without a real NDP source — the default). true → gate enabled, flipped
  // when the RibOutNexthopResolutionReceived signal arrives from RIB (DC
  // production where conditional routes exist).
  bool nexthopResolutionReceived_;

  // One-time flag, marked when safe mode is triggered
  folly::not_null_shared_ptr<std::atomic<bool>> isSafeModeOn_ =
      std::make_shared<std::atomic<bool>>(false);
  std::atomic<bool> goldenPrefixesPolicyActive_{false};

  /*
   * SwitchLimitConfig
   *
   * This is the device level limit to make sure BGP operate in a pre-qualified
   * scale without the risk of instability or memory exhaustion.
   */
  const std::shared_ptr<thrift::BgpSwitchLimitConfig> switchLimitConfig_;

  // Periodic policy cache eviction interval
  std::chrono::seconds periodicPolicyCacheEvictionInterval_{
      kPolicyCachePeriodicEvictionInterval};

  /**
   * Enable dynamic policy evaluation to support runtime policy changes for
   *  1. Ingress Route Filter Policy
   *  2. Ingress and Egress routing policy
   */
  bool enableDynamicPolicyEvaluation_{false};

  /*
   * Set to true when a policy re-evaluation has been triggered (any scope:
   * PEER or PEER_GROUP) and the resulting RIB dumps / group re-evaluation are
   * still in flight. Used only when enableUpdateGroup_ = true.
   */
  bool egressPolicyUpdateForUpdateGroupsScheduled_{false};

  /*
   * Enable BGP Update Groups for shared update generation
   */
  bool enableUpdateGroup_{false};

  /*
   * Enable group PDU serialization for zero-copy distribution
   * Checked globally at PeerManagerBase level
   */
  bool enableSerializeGroupPdu_{false};

  /**
   * How many messages from RIB out queue to drain
   * The number picked here is simply based on the testing results
   * that showed working better for a given implementation existed
   * during the time of testing. If any major architectural/design
   * changes occur for BGP++, this number shall be tuned accordingly.
   */
  uint32_t announcmentsProcessBatch_{50};

  uint32_t ribOutQLowWatermark_{kRibOutQueueSizeResumeThreshold};

  friend class PeerManagerDC;

// per class placeholder for test code injection
// only need to be setup once here
#ifdef PeerManager_TEST_FRIENDS
  PeerManager_TEST_FRIENDS
#endif
};
} // namespace bgp
} // namespace facebook
