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

#include "neteng/fboss/bgp/cpp/stats/StatsBB.h"

namespace facebook::bgp {

namespace BgpStatsBB {

void initCounters() {
  fb303::ThreadCachedServiceData::get()->setCounter(kSetPeersPolicySuccess, 0);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kSetPeersPolicySuccess, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->setCounter(kSetPeersPolicyFailure, 0);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kSetPeersPolicyFailure, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kSetPeerGroupsPolicySuccess, 0);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kSetPeerGroupsPolicySuccess, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kSetPeerGroupsPolicyFailure, 0);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kSetPeerGroupsPolicyFailure, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kUnsetPeersPolicySuccess, 0);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kUnsetPeersPolicySuccess, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->setCounter(
      kUnsetPeersPolicyFailure, 0);
  fb303::ThreadCachedServiceData::get()->addStatExportType(
      kUnsetPeersPolicyFailure, fb303::SUM);
  fb303::ThreadCachedServiceData::get()->setCounter(kAddPeersSuccess, 0);
  fb303::ThreadCachedServiceData::get()->setCounter(kAddPeersRejected, 0);
  fb303::ThreadCachedServiceData::get()->setCounter(kDelPeersSuccess, 0);
  fb303::ThreadCachedServiceData::get()->setCounter(kDelPeersRejected, 0);
}

void incrAddPeersSuccess() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(kAddPeersSuccess, 1);
}

void incrAddPeersRejected() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(kAddPeersRejected, 1);
}

void incrDelPeersSuccess() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(kDelPeersSuccess, 1);
}

void incrDelPeersRejected() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(kDelPeersRejected, 1);
}

void incrSetPeersPolicySuccess() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(
      kSetPeersPolicySuccess, 1);
}

void incrSetPeersPolicyFailure() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(
      kSetPeersPolicyFailure, 1);
}

void incrSetPeerGroupsPolicySuccess() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(
      kSetPeerGroupsPolicySuccess, 1);
}

void incrSetPeerGroupsPolicyFailure() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(
      kSetPeerGroupsPolicyFailure, 1);
}

void incrUnsetPeersPolicySuccess() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(
      kUnsetPeersPolicySuccess, 1);
}

void incrUnsetPeersPolicyFailure() {
  fb303::ThreadCachedServiceData::get()->incrementCounter(
      kUnsetPeersPolicyFailure, 1);
}

} // namespace BgpStatsBB

namespace RibStatsBB {

DEFINE_timeseries(unsupportedPolicyMsg, kUnsupportedPolicyMsg, fb303::COUNT);

} // namespace RibStatsBB

void initStatsBB() {
  BgpStatsBB::initCounters();
}

} // namespace facebook::bgp
