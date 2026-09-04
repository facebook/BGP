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

#include <sstream>

#include "neteng/fboss/bgp/cpp/watchdog/QueryTree.h"

namespace facebook::bgp {

std::unique_ptr<QueryNode>& QueryNode::findOrCreateChild(
    const std::string& childName) {
  auto it = children.find(childName);
  if (it != children.end()) {
    return it->second;
  }

  const auto& [inserted_it, _] =
      children.emplace(childName, std::make_unique<QueryNode>());
  return inserted_it->second;
}

void QueryNode::markLeaf() {
  isLeaf = true;
  children.clear();
}

void QueryTree::addPath(const std::string& path) {
  std::istringstream iss(path);
  std::string entity;
  /*
   * Must use pointer instead of reference, as we cannot
   * rebind a reference
   */
  auto current_node = &root;
  while (std::getline(iss, entity, '.')) {
    if (current_node->isLeaf) {
      break;
    }
    current_node = current_node->findOrCreateChild(entity).get();
  }
  // Mark the last node as leaf
  current_node->markLeaf();
}

} // namespace facebook::bgp
