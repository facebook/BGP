#!/usr/bin/env python3
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

# pyre-strict

from __future__ import annotations

from collections.abc import Awaitable, Callable, Mapping, Sequence
from typing import TypeVar

from facebook.servicerouter.error.types import ErrorReason
from neteng.fboss.bgp_attr.types import TBgpAfi, TIpPrefix
from neteng.fboss.bgp_route_types import types as bgp_route_types
from neteng.fboss.bgp_thrift.clients import TBgpService
from servicerouter.py3.exceptions import ServiceRouterError
from thrift.py3.exceptions import ApplicationError, ApplicationErrorType
from thrift.python.common import RpcOptions


BEST_PATH_GROUP = "best"
T = TypeVar("T")


def _lookup_optional(
    pool: Mapping[int, T], index: int | None, default: T, reference_name: str
) -> T:
    if index is None:
        return default
    try:
        return pool[index]
    except KeyError:
        raise ValueError(f"Missing {reference_name} reference {index}") from None


def _resolve_path(
    path: bgp_route_types.TBgpPathCanonical,
    state: bgp_route_types.TCanonicalRibState,
) -> bgp_route_types.TBgpPath:
    try:
        deduped = state.deduped_paths[path.path_idx]
    except KeyError:
        raise ValueError(f"Missing deduped path reference {path.path_idx}") from None

    peer_id = None
    router_id = None
    peer_description = None
    if path.peer_idx is not None:
        try:
            peer = state.peers[path.peer_idx]
        except KeyError:
            raise ValueError(f"Missing peer reference {path.peer_idx}") from None
        peer_id = peer.peer_id
        router_id = peer.router_id
        # Empty means the referenced peer has no description; None means the
        # path has no peer reference.
        peer_description = peer.peer_description or ""

    return bgp_route_types.TBgpPath(
        next_hop=deduped.next_hop,
        as_path=_lookup_optional(
            state.attr_dict.as_path_lists, deduped.as_path_idx, [], "AS path"
        ),
        communities=_lookup_optional(
            state.attr_dict.community_lists,
            deduped.communities_idx,
            [],
            "communities",
        ),
        extCommunities=_lookup_optional(
            state.attr_dict.ext_community_lists,
            deduped.ext_communities_idx,
            [],
            "extended communities",
        ),
        cluster_list=_lookup_optional(
            state.attr_dict.cluster_lists,
            deduped.cluster_list_idx,
            [],
            "cluster list",
        ),
        origin=deduped.origin,
        local_pref=deduped.local_pref,
        med=deduped.med,
        atomic_aggregate=deduped.atomic_aggregate,
        originator_id=deduped.originator_id,
        aggregator=deduped.aggregator,
        topologyInfo=deduped.topology_info,
        weight=deduped.weight,
        peer_id=peer_id,
        router_id=router_id,
        peer_description=peer_description,
        is_best_path=path.is_best_path,
        next_hop_weight=path.next_hop_weight,
        path_id=path.path_id,
        igp_cost=path.igp_cost,
        last_modified_time=path.last_modified_time or 0,
        path_id_to_send=path.path_id_to_send,
        bestpath_filter_descr=path.bestpath_filter_descr,
        policy_name=path.policy_name,
    )


def resolve_canonical_rib_state(
    state: bgp_route_types.TCanonicalRibState,
) -> list[bgp_route_types.TRibEntry]:
    """Resolve the complete state or reject it on any invalid pool reference.

    This fail-fast contract matches the thrift-python compatibility utility;
    callers never receive a partial RIB assembled from malformed canonical data.
    """
    entries = []
    for canonical_entry in state.rib_entries.values():
        paths = {
            group: [
                _resolve_path(canonical_path, state)
                for canonical_path in canonical_paths
            ]
            for group, canonical_paths in canonical_entry.paths.items()
        }
        best_path = next(
            (
                path
                for group_paths in paths.values()
                for path in group_paths
                if path.is_best_path
            ),
            None,
        )
        entries.append(
            bgp_route_types.TRibEntry(
                prefix=canonical_entry.prefix,
                paths=paths,
                # BGP always names its selected/ECMP group "best", including
                # transient states where none of that group's paths is selected.
                best_group=BEST_PATH_GROUP,
                best_next_hop=(
                    best_path.next_hop if best_path is not None else TIpPrefix()
                ),
                best_path=best_path,
                rib_version=canonical_entry.rib_version,
                path_selection_pending=canonical_entry.path_selection_pending,
                active_cps_criteria=canonical_entry.active_cps_criteria,
                active_cte_ucmp_action=canonical_entry.active_cte_ucmp_action,
            )
        )
    return entries


async def _get_with_fallback(
    canonical_call: Callable[[], Awaitable[bgp_route_types.TCanonicalRibState]],
    legacy_call: Callable[[], Awaitable[Sequence[bgp_route_types.TRibEntry]]],
) -> list[bgp_route_types.TRibEntry]:
    try:
        return resolve_canonical_rib_state(await canonical_call())
    except ApplicationError as error:
        if error.type != ApplicationErrorType.UNKNOWN_METHOD:
            raise
    except ServiceRouterError as error:
        if (
            error.error_reason is None
            or error.error_reason.value != ErrorReason.UNKNOWN_METHOD.value
        ):
            raise
    return list(await legacy_call())


async def get_rib_entries(
    client: TBgpService,
    afi: TBgpAfi,
    *,
    rpc_options: RpcOptions | None = None,
) -> list[bgp_route_types.TRibEntry]:
    return await _get_with_fallback(
        lambda: client.getRibEntriesCanonical(afi, rpc_options=rpc_options),
        lambda: client.getRibEntries(afi, rpc_options=rpc_options),
    )


async def get_rib_prefix(
    client: TBgpService,
    prefix: str,
    *,
    rpc_options: RpcOptions | None = None,
) -> list[bgp_route_types.TRibEntry]:
    return await _get_with_fallback(
        lambda: client.getRibPrefixCanonical(prefix, rpc_options=rpc_options),
        lambda: client.getRibPrefix(prefix, rpc_options=rpc_options),
    )


async def get_rib_entries_for_community(
    client: TBgpService,
    afi: TBgpAfi,
    community_id: str,
    *,
    rpc_options: RpcOptions | None = None,
) -> list[bgp_route_types.TRibEntry]:
    return await _get_with_fallback(
        lambda: client.getRibEntriesForCommunityCanonical(
            afi, community_id, rpc_options=rpc_options
        ),
        lambda: client.getRibEntriesForCommunity(
            afi, community_id, rpc_options=rpc_options
        ),
    )


async def get_rib_entries_for_communities(
    client: TBgpService,
    afi: TBgpAfi,
    community_ids: Sequence[str],
    *,
    rpc_options: RpcOptions | None = None,
) -> list[bgp_route_types.TRibEntry]:
    ids = list(community_ids)
    return await _get_with_fallback(
        lambda: client.getRibEntriesForCommunitiesCanonical(
            afi, ids, rpc_options=rpc_options
        ),
        lambda: client.getRibEntriesForCommunities(afi, ids, rpc_options=rpc_options),
    )


async def get_rib_subprefixes(
    client: TBgpService,
    prefix: str,
    *,
    rpc_options: RpcOptions | None = None,
) -> list[bgp_route_types.TRibEntry]:
    return await _get_with_fallback(
        lambda: client.getRibSubprefixesCanonical(prefix, rpc_options=rpc_options),
        lambda: client.getRibSubprefixes(prefix, rpc_options=rpc_options),
    )


async def get_shadow_rib_entries(
    client: TBgpService,
    afi: TBgpAfi,
    *,
    rpc_options: RpcOptions | None = None,
) -> list[bgp_route_types.TRibEntry]:
    return await _get_with_fallback(
        lambda: client.getShadowRibEntriesCanonical(afi, rpc_options=rpc_options),
        lambda: client.getShadowRibEntries(afi, rpc_options=rpc_options),
    )


async def get_change_list_entries(
    client: TBgpService,
    afi: TBgpAfi,
    *,
    rpc_options: RpcOptions | None = None,
) -> list[bgp_route_types.TRibEntry]:
    return await _get_with_fallback(
        lambda: client.getChangeListEntriesCanonical(afi, rpc_options=rpc_options),
        lambda: client.getChangeListEntries(afi, rpc_options=rpc_options),
    )
