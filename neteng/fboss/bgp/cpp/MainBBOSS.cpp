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

#include <folly/FileUtil.h>
#include <folly/Singleton.h>
#include <folly/init/Init.h>
#include <folly/system/ThreadName.h>
#include <gflags/gflags.h>

#include <fb303/ThreadCachedServiceData.h>

#include "magic_enum/magic_enum.hpp"
#include "neteng/fboss/bgp/cpp/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/BgpServiceEventHandler.h"
#include "neteng/fboss/bgp/cpp/BgpServiceStream.h"
#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/MainUtil.h"
#include "neteng/fboss/bgp/cpp/MainUtilBB.h"
#include "neteng/fboss/bgp/cpp/common/BuildInfo.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/config/ConfigBB.h"
#include "neteng/fboss/bgp/cpp/config/ConfigManager.h"
#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NetlinkWrapper.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopHandler.h"
#include "neteng/fboss/bgp/cpp/peer/PeerManagerBB.h"
#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/rib/FibEbb.h"
#include "neteng/fboss/bgp/cpp/rib/RibBB.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBB.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "neteng/fboss/bgp/cpp/watchdog/MonitoredQueue.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"

using namespace facebook::bgp;

using facebook::neteng::fboss::bgp::thrift::BgpInitializationEvent;

DEFINE_int32(thrift_port, 6909, "BGP Thrift service port");
DEFINE_int32(stream_port, 6910, "BGP streaming service port");
DEFINE_int32(
    num_thrift_io_threads,
    1,
    "Number of I/O threads used by the Thrift server");
DEFINE_int32(
    num_thrift_stream_io_threads,
    1,
    "Number of I/O threads used by the Thrift stream server");
DEFINE_int64(max_rss_size, 2, "RSS limit in GB");
DEFINE_int32(bgp_policy_cache_size, 0, "Policy cache maximum size");
DEFINE_int32(
    max_thrift_requests,
    20,
    "Maximum number of active Thrift requests");
DEFINE_int32(
    max_thrift_listen_backlog,
    20,
    "Maximum number of queued incoming connections");
DEFINE_int32(
    max_thrift_connections,
    256,
    "Maximum number of active incoming connections");
DEFINE_int32(
    fd_soft_limit,
    0,
    "File descriptor soft limit override; zero keeps the current limit");
DEFINE_int32(
    openr_fib_agent_port,
    facebook::bgp::kDefaultFibAgentPort,
    "Open/R FIB agent Thrift port");

DEFINE_string(platform, kEbbPlatform, "Platform name: ebb or dev");
DEFINE_string(config, "", "Initial BGP configuration file");
DEFINE_string(policy, "", "Initial BGP policy configuration file");

namespace {
using BgpSignalHandler = facebook::bgp::BgpSignalHandler;
} // namespace

int main(int argc, char** argv) {
  gflags::SetVersionString(facebook::bgp::BuildInfo::toDebugString());

  /*
   * Parse without removing the arguments so FLAGS_config and FLAGS_policy are
   * available below. initFlagsFromConfig() applies config values, then
   * folly::Init reparses the command line so command-line values take
   * precedence.
   */
  gflags::ParseCommandLineFlags(&argc, &argv, false);
  facebook::bgp::BuildInfo::exportBuildInfo();

  bool splitConfigPolicy = false;
  if (!FLAGS_policy.empty()) {
    int fd = folly::openNoInt(FLAGS_policy.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd != -1) {
      folly::closeNoInt(fd);
      splitConfigPolicy = true;
    }
  }

  folly::SingletonVault::singleton()->setType(
      folly::SingletonVault::Type::Relaxed);
  BgpStats::handlePreviousExit();

  const std::string emptyPolicy;
  facebook::bgp::initFlagsFromConfig(
      FLAGS_config, splitConfigPolicy ? FLAGS_policy : emptyPolicy);
  folly::Init init(&argc, &argv);
  folly::setThreadName("bgpd_main");

  if (FLAGS_fd_soft_limit > 0) {
    facebook::bgp::setFileDescriptorSoftLimit(FLAGS_fd_soft_limit);
  }

  BgpStats::logInitializationEvent(
      "Main", BgpInitializationEvent::INITIALIZING);
  initStatsBase();
  initStatsBB();
  BgpStats::setPolicySymlink(splitConfigPolicy ? 1 : 0);
  facebook::fb303::ThreadCachedServiceData::get()->startPublishThread(
      std::chrono::milliseconds{1000});

  if (FLAGS_platform != kEbbPlatform && FLAGS_platform != kDevPlatform) {
    throw BgpError("Unsupported platform, '", FLAGS_platform, "'");
  }

  folly::EventBase signalHandlerEvb;
  auto signalHandler = std::make_unique<BgpSignalHandler>(&signalHandlerEvb);
  auto signalHandlerEvbThread =
      std::thread([&]() { signalHandlerEvb.loopForever(); });
  signalHandlerEvb.waitUntilRunning();

  if (FLAGS_platform == kEbbPlatform) {
    if (!facebook::bgp::waitForFibService(
            signalHandlerEvb,
            FLAGS_agent_thrift_port,
            FLAGS_agent_thrift_recv_timeout_ms,
            FLAGS_openr_fib_agent_port)) {
      signalHandlerEvb.terminateLoopSoon();
      signalHandlerEvbThread.join();
      XLOGF(INFO, "Stopping BGP++ daemon: pid = {}", getpid());
      return 0;
    }
    XLOG(INFO, "BGP FIB services are ready");
  }

  BgpStats::logInitializationEvent(
      "Main", BgpInitializationEvent::AGENT_CONFIGURED);

  /*
   * BB has no NeighborWatcher producer. Keep this disengaged so PeerManager
   * does not start an idle neighbor route-change consumer.
   */
  std::optional<MonitoredMPMCQueue<NeighborWatcherMessage>> neighborEventQ;
  facebook::nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ{
      facebook::nettools::bgplib::kMaxIngressQueueSize};
  MonitoredMPMCQueue<RibOutMessage> ribOutQ;

  auto config = std::make_shared<ConfigBB>(FLAGS_config);
  SystemResourceLimits resourceLimits;
  resourceLimits.rssLimitBytes = FLAGS_max_rss_size * 1024LL * 1024 * 1024;
  Watchdog watchdog(config, std::move(resourceLimits));
  auto watchdogThread = watchdog.runInThread();
  auto configManager = std::make_shared<ConfigManager>(
      config, FLAGS_config, /*splitConfigPolicy=*/splitConfigPolicy);

  openr::messaging::ReplicateQueue<openr::fbnl::NetlinkEvent>
      netlinkEventsQueue;
  auto nexthopCache = std::make_shared<NexthopCache>();
  auto nlWrapper = std::make_shared<NetlinkWrapper>(
      nexthopCache,
      ribInQ,
      netlinkEventsQueue,
      config->getBgpGlobalConfig()->includeInterfaceRegexes,
      config->getBgpGlobalConfig()->enableNetlinkDampening,
      std::optional<std::chrono::milliseconds>{},
      FLAGS_openr_fib_agent_port);
  auto nlThread = nlWrapper->runInThread();

  auto nexthopHandler = std::make_unique<NexthopHandler>(
      nexthopCache, ribInQ, FLAGS_openr_fib_agent_port);
  auto nexthopHandlerThread = nexthopHandler->runInThread();

  if (splitConfigPolicy) {
    XLOG(INFO, "Explicit BGP policy config input");
    config->setPolicyConfigFromFile(FLAGS_policy);
  }
  const auto& myConfig = config->getConfig();
  XLOGF(
      INFO,
      "Start BGP with router_id = {}, local_as = {}",
      *myConfig.router_id(),
      config->getBgpGlobalConfig()->localAsn);

  auto policyManager = Config::createPolicyManager(config);
  XLOGF(INFO, "Setting policy cache size to {}", FLAGS_bgp_policy_cache_size);
  AdjRibPolicyCache::get()->setCacheSize(FLAGS_bgp_policy_cache_size);

  RibBB rib(
      config->getLocalRoutes(),
      *(config->getBgpGlobalConfig()),
      config->getPolicies(),
      ribInQ,
      ribOutQ,
      FLAGS_platform,
      nexthopCache,
      FLAGS_agent_thrift_port,
      FLAGS_agent_thrift_recv_timeout_ms);
  watchdog.monitorModule(rib.getModuleName(), rib);
  auto ribThread = rib.runInThread();

  auto sessionMgr = std::make_shared<SessionManager>(
      *(config->getBgpGlobalConfig()),
      false, /* enableMessagesOverNotifyQueue */
      true); /* enableCoroNotifyQueue */

  PeerManagerBB peerMgr(
      configManager, policyManager, ribInQ, ribOutQ, neighborEventQ);
  peerMgr.setSessionManager(sessionMgr);
  watchdog.monitorModule(peerMgr.getModuleName(), peerMgr);

  auto tRouteFilterPolicy = rib.getRouteFilterPolicy();
  auto validationResult =
      isPeerGroupConfigValid(tRouteFilterPolicy, config->getPeerGroups());
  if (validationResult != PeerGroupValidationResult::SUCCESS) {
    throw BgpError(
        "Route filter policy validation failed: ",
        std::string(magic_enum::enum_name(validationResult)));
  }

  peerMgr.setRouteFilterPolicy(
      std::make_unique<RouteFilterPolicy>(tRouteFilterPolicy));
  auto peerMgrThread = peerMgr.runInThread();
  auto sessionMgrThread = sessionMgr->runInThread();

  std::shared_ptr<wangle::SSLContextConfig> sslContext;
  if (config->isThriftServerTlsEnabled()) {
    sslContext = std::make_shared<wangle::SSLContextConfig>();
    sslContext->setCertificate(
        config->getThriftServerCertPath(),
        config->getThriftServerKeyPath(),
        "");
    sslContext->clientCAFiles =
        std::vector<std::string>{config->getThriftServerCaPath()};
    sslContext->sessionContext = "bgpd";
    sslContext->setNextProtocols(
        **apache::thrift::ThriftServer::defaultNextProtocols());
    sslContext->clientVerification =
        getThriftServerClientVerification(*config->getBgpGlobalConfig());
    sslContext->eccCurveName = config->getThriftServerEccCurveName();
  }

  auto bgpServer = facebook::bgp::makeThriftServer(
      "bgpd",
      sslContext,
      config->isThriftServerTlsEnabled(),
      getThriftServerSSLPolicy(*config->getBgpGlobalConfig()));
  auto processorEventHandler = std::make_shared<BgpServiceEventHandler>();
  apache::thrift::TProcessorBase::addProcessorEventHandler_deprecated(
      std::move(processorEventHandler));
  auto bgpHandler = std::make_shared<BgpServiceBB>(
      peerMgr, configManager, rib, watchdog, nlWrapper, false);
  bgpServer->setInterface(std::move(bgpHandler));
  bgpServer->setPort(FLAGS_thrift_port);
  bgpServer->setMaxRequests(FLAGS_max_thrift_requests);
  bgpServer->setListenBacklog(FLAGS_max_thrift_listen_backlog);
  bgpServer->setMaxConnections(FLAGS_max_thrift_connections);
  if (FLAGS_num_thrift_io_threads > 0) {
    bgpServer->setNumIOWorkerThreads(FLAGS_num_thrift_io_threads);
  }

  auto streamServer = facebook::bgp::makeThriftServer(
      "BgpStreamService",
      sslContext,
      config->isThriftServerTlsEnabled(),
      getThriftServerSSLPolicy(*config->getBgpGlobalConfig()));
  auto streamHandler = std::make_shared<BgpServiceStream>(&peerMgr);
  streamServer->setInterface(std::move(streamHandler));
  streamServer->setPort(FLAGS_stream_port);
  if (FLAGS_num_thrift_stream_io_threads > 0) {
    streamServer->setNumIOWorkerThreads(FLAGS_num_thrift_stream_io_threads);
  }

  std::thread thriftServiceThread([&]() { bgpServer->serve(); });
  std::thread streamServiceThread([&]() { streamServer->serve(); });

  signalHandlerEvbThread.join();
  XLOGF(INFO, "Stopping BGP++ daemon: pid = {}", getpid());

  bgpServer->stop();
  streamServer->stop();
  thriftServiceThread.join();
  streamServiceThread.join();

  netlinkEventsQueue.close();
  nlWrapper->stop();
  nexthopHandler->stop();
  rib.stop();

  peerMgr.markDaemonShutdown();
  peerMgr.saveGrState();
  sessionMgr->stop();
  peerMgr.stop();
  watchdog.stop();

  nlThread.join();
  nlWrapper.reset();
  nexthopHandlerThread.join();
  nexthopHandler.reset();
  nexthopCache.reset();
  ribThread.join();
  peerMgrThread.join();
  sessionMgrThread.join();
  watchdogThread.join();

  XLOGF(INFO, "Successfully stopped BGP++ daemon: pid = {}", getpid());
  return 0;
}
