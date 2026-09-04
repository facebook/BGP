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

#include "neteng/fboss/bgp/cpp/policy/base/PolicyStructsBase.h"

namespace facebook {
namespace routing {

/*
 * compare value against value_to_compare with operator
 * return true if value satisfies the condition
 * e.g. op = GE, value_to_compare = 1, value = 2
 *      returns true (value >= value_to_compare)
 */
bool CompareNumValue(
    const PolicyComparisonOperator& op,
    int value_to_compare,
    int value);

} // namespace routing
} // namespace facebook
