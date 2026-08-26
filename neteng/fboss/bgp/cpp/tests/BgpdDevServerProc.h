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

#include <chrono>
#include <thread>

#include <folly/Expected.h>
#include <folly/Subprocess.h>

#include <gflags/gflags.h>

#include "neteng/fboss/bgp/if/gen-cpp2/TBgpService.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

namespace facebook::bgp {

/**
 * The class to spin a bgpd process on Bgp in DevServer mode (i.e. without fboss
 * agent) with folly::Subprocess.
 * It is used in bgpd related stand alone tests.
 */
class BgpdDevServerProc final {
 public:
  explicit BgpdDevServerProc(
      const std::string& config,
      const std::string& policy,
      const std::string& bgpdLoc);

  ~BgpdDevServerProc() {
    if (!stopFlag_) {
      stop();
    }
  }

  /** start the bgpd process */
  bool run();

  /** stop the bgpd process */
  void stop();

  /**
   * We spin the bgpd process with ephemeral thrift port and bgp protocol port
   * to avoid port conflict.
   *
   * Motivation:
   * In product version (i.e. with fboss agent) has the fixed thrift port (6909)
   * and fixed BGP protocol port (179). In testing, the fix port could cause
   * conflict; imagine two stand alone tests running on the same container. With
   * ephemeral ports the problem would be fixed, and therefore we need the APIs
   * to get the ephemeral ports. The following two APIs are for this purpose.
   */
  folly::Expected<int, std::string> getThriftPort() const;
  folly::Expected<int, std::string> getBgpProtocolPort() const;

 private:
  // The thrift service (ephemeral) port
  int thriftPort_{0};

  // The BGP protocol (ephemeral) port
  int bgpProtocolPort_{0};

  // The subprocess of bgpd
  std::unique_ptr<folly::Subprocess> bgpdProc_{nullptr};

  // The thread running the bgpd subprocess
  std::unique_ptr<std::thread> bgpdThread_{nullptr};

  // indicator for whether the bgpd process is spinned on
  std::atomic<bool> isStarted_{false};
  std::atomic<bool> bgpPortParsed_{false};
  std::atomic<bool> thriftPortParsed_{false};
  std::atomic<bool> policyMatched_{false};
  std::atomic<bool> validateAPIPassed_{true};

  // Stand alone config file name
  std::string config_;
  // Stand alone policy file name
  std::string policy_;

  // Path to bgpd_cpp binary
  std::string bgpdLoc_;

  // stop flag to avoid data race
  std::atomic<bool> stopFlag_{false};

  /*
   * bgpd logs asynchronously from several threads, so the thrift port, the BGP
   * protocol port and the EOR_SENT readiness marker can reach our reader in any
   * order. Every consumer of these three signals must gate on all of them
   * together; treating isStarted_ alone as "done" silently drops whichever port
   * line happens to arrive last.
   */
  bool allStartupSignalsCaptured() const noexcept;

  void matchPolicyConfig(
      const std::string& policy,
      const std::string& configStr);

  std::unique_ptr<
      apache::thrift::Client<facebook::neteng::fboss::bgp::thrift::TBgpService>>
  getThriftClient(
      const std::string& ipv6_str,
      int thriftPort,
      folly::EventBase* evb,
      const std::string& devicename);
};
} // namespace facebook::bgp
