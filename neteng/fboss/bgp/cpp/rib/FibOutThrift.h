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

#include "neteng/fboss/bgp/cpp/rib/FibOut.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_route_types_types.h"

namespace facebook::bgp {

/**
 * Convert one normalized submitted FIB request to its Thrift form.
 */
neteng::fboss::bgp::thrift::TFibOutRoute toThriftFibOutRoute(
    const FibOutState& state);

} // namespace facebook::bgp
