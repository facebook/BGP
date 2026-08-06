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

#include <sys/resource.h>
#include <csignal>

#include <fboss/agent/if/gen-cpp2/ctrl_clients.h>
#include <fboss/agent/if/gen-cpp2/ctrl_types.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp/server/TServerEventHandler.h>

#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/FeatureFlags.h"
#include "neteng/fboss/bgp/cpp/common/ThriftClientUtils.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/config/ThriftServerUtils.h"
#include "neteng/fboss/bgp/cpp/stats/StatsBase.h"
#include "openr/if/gen-cpp2/FibService.h"

namespace facebook::bgp {

struct NamedThriftServer {
  std::shared_ptr<apache::thrift::ThriftServer> thriftServerPtr;
  std::string name;
};

class BgpSignalHandler : public folly::AsyncSignalHandler {
 public:
  explicit BgpSignalHandler(folly::EventBase* evb)
      : folly::AsyncSignalHandler(evb) {
    registerSignalHandler(SIGINT);
    registerSignalHandler(SIGTERM);
    registerSignalHandler(SIGQUIT);
  }

  void signalReceived(int signal) noexcept override {
    static folly::once_flag shutdownOnce;

    folly::call_once(shutdownOnce, [this, signal]() {
      BgpStats::markPlannedExit();
      XLOGF(INFO, "Caught signal: {}. Stopping BGP++...", signal);

      unregisterSignalHandler(SIGINT);
      unregisterSignalHandler(SIGTERM);
      unregisterSignalHandler(SIGQUIT);

      getEventBase()->terminateLoopSoon();
    });
  }
};

namespace {
using namespace apache::thrift::server;
class ServerEventHandler : public TServerEventHandler {
 public:
  explicit ServerEventHandler(char const* serverName)
      : serverName_(serverName) {}
  void preServe(const folly::SocketAddress* address) override {
    XLOGF(INFO, "The {} thrift port is {}", serverName_, address->getPort());
  }

  void newConnection(TConnectionContext* ctx) override {
    if (!ctx->getPeerAddress()->isLoopbackAddress()) {
      XLOGF(
          DBG3,
          "New Thrift connection from {}",
          ctx->getPeerAddress()->getAddressStr());
    }
  }

  void connectionDestroyed(TConnectionContext* ctx) override {
    if (!ctx->getPeerAddress()->isLoopbackAddress()) {
      XLOGF(
          DBG3,
          "Thrift connection from {} destroyed",
          ctx->getPeerAddress()->getAddressStr());
    }
  }

 private:
  std::string serverName_;
};
} // namespace

struct Svc {
  std::shared_ptr<apache::thrift::ThriftServer> thriftServer;
  std::string fwkName;
};

inline std::shared_ptr<apache::thrift::ThriftServer> makeThriftServer(
    const std::string& fwkName,
    std::shared_ptr<wangle::SSLContextConfig> sslContext,
    bool isThriftServerTlsEnabled,
    apache::thrift::SSLPolicy sslPolicy) {
  // Start server
  auto server = std::make_shared<apache::thrift::ThriftServer>();
  // Set server common properties
  server->setAllowPlaintextOnLoopback(true);
  // We register the Server event handler to print out the thrift port
  auto eventHandler = std::make_unique<ServerEventHandler>(fwkName.c_str());
  server->setServerEventHandler(std::move(eventHandler));

  // Configure SSL/TLS settings
  if (isThriftServerTlsEnabled) {
    server->setSSLConfig(sslContext);
    server->setSSLPolicy(sslPolicy);
  }

  return server;
}

/*
 * This is a blocking call for Fib Agent. This requires Fib Agent to be
 * ready to take thrift calls before instantiating other threads.
 *
 * ATTN: Terminiation signal will stop the evb and signal this
 *       function to return.
 *
 * @param evb - this is the eventbase with signal handler.
 * @return
 *    true - indicating FibAgent correctly started;
 *    false - indicating termination signal received. Exit and return;
 */
inline bool waitForFibService(
    const folly::EventBase& signalHandlerEvb,
    const std::string& platform,
    int32_t agentThriftPort,
    int32_t agentThriftRecvTimeoutMs,
    int32_t openrFibAgentPort) {
  std::chrono::steady_clock::time_point startTime =
      std::chrono::steady_clock::now();
  folly::EventBase evb;

  std::string serviceName =
      (platform == kFbossPlatform) ? "FibService" : "FibEbbService";
  bool ready = false;

  // Main wait loop
  while (signalHandlerEvb.isRunning() && !ready) {
    try {
      if (platform == kFbossPlatform) {
        auto bgpFibClient = createThriftClient<
            apache::thrift::Client<facebook::fboss::FbossCtrl>>(
            evb,
            kLoopBackAddressV6,
            agentThriftPort,
            kFbossAgentConnTimeout,
            kFbossAgentSendTimeout,
            std::chrono::milliseconds(agentThriftRecvTimeoutMs));
        ::facebook::fboss::SwitchRunState bgpFibClientState =
            bgpFibClient->sync_getSwitchRunState();
        ready =
            (bgpFibClientState == facebook::fboss::SwitchRunState::CONFIGURED);
      } else if (platform == kEbbPlatform) {
        auto bgpFibClient = createThriftClient<
            apache::thrift::Client<openr::thrift::FibService>>(
            evb,
            kLoopBackAddressV6,
            agentThriftPort,
            kFibEbbConnTimeout,
            kFibEbbSendTimeout,
            std::chrono::milliseconds(agentThriftRecvTimeoutMs));
        ::openr::thrift::SwitchRunState bgpFibClientState =
            bgpFibClient->sync_getSwitchRunState();

        auto openrFibClient = createThriftClient<
            apache::thrift::Client<openr::thrift::FibService>>(
            evb,
            kLoopBackAddressV6,
            openrFibAgentPort,
            kFibOpenrConnTimeout,
            kFibOpenrSendTimeout,
            kFibOpenrRecvTimeout);
        ::openr::thrift::SwitchRunState openrFibClientState =
            openrFibClient->sync_getSwitchRunState();

        ready =
            (bgpFibClientState == openr::thrift::SwitchRunState::CONFIGURED) &&
            (openrFibClientState == openr::thrift::SwitchRunState::CONFIGURED);
      } else {
        throw BgpError(
            "Unsupported platform for FIB service waiting: '", platform, "'");
      }
    } catch (const std::exception&) {
    }

    if (!ready) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      XLOGF(INFO, "Waiting for {} to come up...", serviceName);
    }
  }

  std::chrono::milliseconds::rep elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - startTime)
          .count();

  if (!ready) {
    XLOGF(INFO, "Termination signal received. Waited for {}ms", elapsed);
    return false;
  }

  XLOGF(INFO, "{} is up. Waited for {}ms", serviceName, elapsed);
  return true;
}

// Override default values of flags based on value in config file
inline void initFlagsFromConfig(
    const std::string& configFile,
    const std::string& policyFile) {
  // Get config file.  Set peerSubnetLbwMap to nullopt and populateConfigDb to
  // false in Config constructor because we are not yet fully initialized.
  // Our intent at this stage is to only get myConfig.defaultCommandLineArgs
  // from the config file
  auto config = Config(configFile);
  bool splitConfigPolicy = !policyFile.empty();
  if (splitConfigPolicy) {
    XLOG(INFO, "Explicit BGP policy config input");
    // Validates JSON syntax via folly::parseJson() first
    config.setPolicyConfigFromFile(policyFile);
  }
  auto myConfig = config.getConfig();
  FeatureFlags::LoadFromThriftConfig(myConfig);

  // Iterate over defaultCommandLineArgs params and use them to override
  // hardcoded defaults
  for (const auto& item : *myConfig.defaultCommandLineArgs()) {
    // logging not initialized yet, need to use std::cerr
    std::cerr << "Overriding default flag from config: " << item.first.c_str()
              << "=" << item.second.c_str() << std::endl;
    // set flag default based on value in config file
    gflags::SetCommandLineOptionWithMode(
        item.first.c_str(), item.second.c_str(), gflags::SET_FLAGS_DEFAULT);
  }
}

inline bool setFileDescriptorSoftLimit(int32_t softLimit) {
  if (softLimit <= 0) {
    return false;
  }
  struct rlimit rlim{};
  if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    XLOGF(WARNING, "Failed to get fd limits: {}", folly::errnoStr(errno));
    return false;
  }

  rlim.rlim_cur = std::min(static_cast<rlim_t>(softLimit), rlim.rlim_max);

  if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    XLOGF(WARNING, "Failed to set fd limit: {}", folly::errnoStr(errno));
    return false;
  }

  XLOGF(INFO, "Set fd limit: {}, (was: {})", rlim.rlim_cur, rlim.rlim_max);
  return true;
}

} // namespace facebook::bgp
