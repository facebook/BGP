# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


from __future__ import annotations

import re
from pathlib import Path


BGP_CPP_ROOT = "neteng/fboss/bgp/cpp"
CMAKE_FILE = "neteng/fboss/bgp/public_tld/CMakeLists.txt"
# CMakeLists.txt references thrift files by their path in the OSS source tree,
# which ShipIt assembles from several fbsource locations. This mirrors the
# [shipit.pathmap] in opensource/fbcode_builder/manifests/bgp: each fbsource
# directory (relative to fbcode/) maps to a prefix in the OSS tree. We scan the
# fbsource side and re-prefix to the OSS path so the comparison matches the
# add_fbthrift_cpp_library() arguments.
THRIFT_SRC_DIRS = {
    # fbsource dir (relative to repo root)        : OSS-tree prefix
    "neteng/fboss/bgp/if": "neteng/fboss/bgp/if",
    "neteng/fboss/bgp/public_tld/configerator": "configerator",
    "neteng/fboss/bgp/public_tld/common": "common",
    # fbcode/fboss/common = common in the manifest: provides common/fb303 and
    # common/network/if/Address.thrift to the OSS build (not vendored).
    "fboss/common": "common",
}
# sim/ holds the BGP simulator sources, which are not part of the OSS library.
EXCLUDED_DIRS = {"facebook", "tests", "eos_wrapper", "test", "sim"}
EXCLUDED_FILES = {
    "MainOSS.cpp",
    "PeerManagerDC.cpp",
    "VipPeerManager.cpp",
    # NetlinkWrapper.cpp pulls openr/messaging + openr/nl, neither vendored;
    # netlink-based nexthop tracking is BB-only (see CMakeLists.txt).
    "NetlinkWrapper.cpp",
    # RibBB.cpp lives in cpp/rib/ but is BB-only (its header is excluded from
    # the OSS install set).
    "RibBB.cpp",
    # StatsBB.cpp lives in cpp/stats/ but is BB-only (its header is excluded
    # from the OSS install set).
    "StatsBB.cpp",
}
EXCLUDED_THRIFT = {"BmpStructs.thrift"}


def get_repo_root() -> Path:
    candidate = Path(__file__).resolve().parents[4]
    if (candidate / CMAKE_FILE).exists():
        return candidate
    # Buck test sandbox: __file__ resolves into the link-tree under buck-out/.
    # Walk up to find the repo root (contains fbcode/), then return fbcode/.
    current = Path(__file__).resolve()
    while current != current.parent:
        fbcode = current / "fbcode"
        if fbcode.is_dir() and (fbcode / CMAKE_FILE).exists():
            return fbcode
        current = current.parent
    raise RuntimeError("Could not find repo root containing " + CMAKE_FILE)


def get_oss_eligible_sources(repo_root: Path) -> set[str]:
    cpp_root = repo_root / BGP_CPP_ROOT
    sources = set()
    for cpp_file in cpp_root.rglob("*.cpp"):
        rel = cpp_file.relative_to(repo_root)
        parts = rel.parts
        if any(excluded in parts for excluded in EXCLUDED_DIRS):
            continue
        if cpp_file.name in EXCLUDED_FILES:
            continue
        sources.add(str(rel))
    return sources


def get_cmake_sources(repo_root: Path) -> set[str]:
    cmake_path = repo_root / CMAKE_FILE
    content = cmake_path.read_text()
    # The BGP++ library is split across many tiered add_library() targets
    # (bgp_routelib, bgp_lib_core, bgp_common, ... bgp_service) rather than a
    # single target, so collect .cpp sources from every add_library() block.
    sources = set()
    for match in re.finditer(r"add_library\s*\(\s*\w+\s*\n(.*?)\)", content, re.DOTALL):
        block = match.group(1)
        for line in block.split("\n"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if not line.endswith(".cpp"):
                continue
            # Scope to bgp/cpp/ sources so vendored openr .cpp files (built via
            # their own add_library targets) are not treated as BGP sources.
            if not line.startswith(BGP_CPP_ROOT + "/"):
                continue
            if Path(line).name in EXCLUDED_FILES:
                continue
            sources.add(line)
    return sources


def get_thrift_files(repo_root: Path) -> set[str]:
    thrift_files = set()
    for src_dir, oss_prefix in THRIFT_SRC_DIRS.items():
        base = repo_root / src_dir
        if not base.exists():
            continue
        for thrift_file in base.rglob("*.thrift"):
            if thrift_file.name in EXCLUDED_THRIFT:
                continue
            rel = thrift_file.relative_to(base)
            oss_path = f"{oss_prefix}/{rel}" if oss_prefix else str(rel)
            thrift_files.add(oss_path)
    return thrift_files


def get_cmake_thrift_targets(repo_root: Path) -> set[str]:
    cmake_path = repo_root / CMAKE_FILE
    content = cmake_path.read_text()
    pattern = r"add_fbthrift_cpp_library\s*\(\s*\w+\s*\n\s*(\S+\.thrift)"
    thrift_files = set()
    for match in re.finditer(pattern, content):
        thrift_files.add(match.group(1))
    return thrift_files


def validate_sources(
    repo_root: Path,
) -> tuple[set[str], set[str]]:
    oss_sources = get_oss_eligible_sources(repo_root)
    cmake_sources = get_cmake_sources(repo_root)
    missing_from_cmake = oss_sources - cmake_sources
    extra_in_cmake = cmake_sources - oss_sources
    return missing_from_cmake, extra_in_cmake


def validate_thrift(
    repo_root: Path,
) -> tuple[set[str], set[str]]:
    thrift_files = get_thrift_files(repo_root)
    cmake_thrift = get_cmake_thrift_targets(repo_root)
    missing_from_cmake = thrift_files - cmake_thrift
    extra_in_cmake = cmake_thrift - thrift_files
    return missing_from_cmake, extra_in_cmake
