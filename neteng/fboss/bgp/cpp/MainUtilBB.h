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

#include <cstdint>

namespace folly {
class EventBase;
}

namespace facebook::bgp {

/*
 * Wait until the FibEbb and Open/R FibService endpoints report CONFIGURED.
 * Return false if a termination signal stops the EventBase first.
 */
bool waitForFibService(
    const folly::EventBase& signalHandlerEvb,
    int32_t fibEbbPort,
    int32_t recvTimeoutMs,
    int32_t openrPort);

} // namespace facebook::bgp
