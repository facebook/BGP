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

from facebook.servicerouter.error.types import ErrorReason
from later.unittest import TestCase
from neteng.fboss.bgp.client.canonical_rib import (
    _get_with_fallback,
    _get_with_fallback_sync,
    resolve_canonical_rib_state,
)
from neteng.fboss.bgp_attr.thrift_types import TBgpCommunity, TIpPrefix
from neteng.fboss.bgp_route_types import thrift_types as bgp_route_types
from servicerouter.python.exceptions import ServiceRouterError
from thrift.python.exceptions import (
    ApplicationError,
    ApplicationErrorType,
    TransportErrorType,
)


def _make_state(
    *,
    selected: bool = True,
    peer_idx: int | None = 13,
    peer_description: str | None = "peer",
) -> bgp_route_types.TCanonicalRibState:
    prefix = TIpPrefix(prefix_bin=b"\x0a\x00\x00\x00", num_bits=24)
    next_hop = TIpPrefix(prefix_bin=b"\x0a\x00\x00\x01", num_bits=32)
    backup_addr = TIpPrefix(prefix_bin=b"\x20\x01\x0d\xb8" + b"\x00" * 12, num_bits=128)
    peer_id = TIpPrefix(prefix_bin=b"\x0a\x00\x00\x02", num_bits=32)
    community = TBgpCommunity(asn=65000, value=100, community=4259840100)
    return bgp_route_types.TCanonicalRibState(
        attr_dict=bgp_route_types.TBgpAttrDict(community_lists={7: [community]}),
        deduped_paths={
            11: bgp_route_types.TBgpDedupedPath(
                next_hop=next_hop,
                communities_idx=7,
                local_pref=321,
                backup_addr=backup_addr,
            )
        },
        peers={
            13: bgp_route_types.TCanonicalPeer(
                peer_id=peer_id,
                router_id=42,
                peer_description=peer_description,
            )
        },
        rib_entries={
            "10.0.0.0/24": bgp_route_types.TRibEntryCanonical(
                prefix=prefix,
                paths={
                    "best": [
                        bgp_route_types.TBgpPathCanonical(
                            path_idx=11,
                            peer_idx=peer_idx,
                            is_best_path=True if selected else None,
                            last_modified_time=99,
                        )
                    ]
                },
                rib_version=5,
            )
        },
    )


class CanonicalRibTest(TestCase):
    def test_resolves_pooled_path_and_peer(self) -> None:
        entries = resolve_canonical_rib_state(_make_state())

        self.assertEqual(1, len(entries))
        entry = entries[0]
        self.assertEqual(5, entry.rib_version)
        self.assertEqual("best", entry.best_group)
        self.assertEqual(entry.paths["best"][0], entry.best_path)
        path = entry.paths["best"][0]
        self.assertEqual(321, path.local_pref)
        self.assertEqual(42, path.router_id)
        self.assertEqual("peer", path.peer_description)
        self.assertEqual(99, path.last_modified_time)
        self.assertEqual(entry.best_next_hop, path.next_hop)
        self.assertEqual(
            TIpPrefix(prefix_bin=b"\x20\x01\x0d\xb8" + b"\x00" * 12, num_bits=128),
            path.backup_addr,
        )

    def test_preserves_no_selected_best_state(self) -> None:
        entry = resolve_canonical_rib_state(_make_state(selected=False))[0]

        self.assertEqual("best", entry.best_group)
        self.assertIsNone(entry.best_path)
        self.assertEqual(TIpPrefix(), entry.best_next_hop)

    def test_distinguishes_missing_peer_from_empty_description(self) -> None:
        path_without_peer = resolve_canonical_rib_state(_make_state(peer_idx=None))[
            0
        ].paths["best"][0]
        self.assertIsNone(path_without_peer.peer_description)

        path_with_unnamed_peer = resolve_canonical_rib_state(
            _make_state(peer_description=None)
        )[0].paths["best"][0]
        self.assertEqual("", path_with_unnamed_peer.peer_description)

    def test_rejects_missing_references(self) -> None:
        prefix = TIpPrefix(prefix_bin=b"\x0a\x00\x00\x00", num_bits=24)
        state = bgp_route_types.TCanonicalRibState(
            deduped_paths={11: bgp_route_types.TBgpDedupedPath(communities_idx=99)},
            rib_entries={
                "10.0.0.0/24": bgp_route_types.TRibEntryCanonical(
                    prefix=prefix,
                    paths={"best": [bgp_route_types.TBgpPathCanonical(path_idx=11)]},
                )
            },
        )
        with self.assertRaisesRegex(ValueError, "Missing communities reference 99"):
            resolve_canonical_rib_state(state)

        state = bgp_route_types.TCanonicalRibState(
            rib_entries={
                "10.0.0.0/24": bgp_route_types.TRibEntryCanonical(
                    prefix=prefix,
                    paths={"best": [bgp_route_types.TBgpPathCanonical(path_idx=99)]},
                )
            }
        )
        with self.assertRaisesRegex(ValueError, "Missing deduped path reference 99"):
            resolve_canonical_rib_state(state)

        state = bgp_route_types.TCanonicalRibState(
            deduped_paths={11: bgp_route_types.TBgpDedupedPath()},
            rib_entries={
                "10.0.0.0/24": bgp_route_types.TRibEntryCanonical(
                    prefix=prefix,
                    paths={
                        "best": [
                            bgp_route_types.TBgpPathCanonical(path_idx=11, peer_idx=99)
                        ]
                    },
                )
            },
        )
        with self.assertRaisesRegex(ValueError, "Missing peer reference 99"):
            resolve_canonical_rib_state(state)

    async def test_falls_back_only_for_unknown_method(self) -> None:
        legacy_entry = bgp_route_types.TRibEntry()

        async def unknown_method() -> bgp_route_types.TCanonicalRibState:
            raise ApplicationError(
                ApplicationErrorType.UNKNOWN_METHOD, "canonical RPC unavailable"
            )

        async def legacy() -> list[bgp_route_types.TRibEntry]:
            return [legacy_entry]

        self.assertEqual(
            [legacy_entry], await _get_with_fallback(unknown_method, legacy)
        )

        async def internal_error() -> bgp_route_types.TCanonicalRibState:
            raise ApplicationError(ApplicationErrorType.INTERNAL_ERROR, "boom")

        with self.assertRaises(ApplicationError):
            await _get_with_fallback(internal_error, legacy)

        async def malformed_state() -> bgp_route_types.TCanonicalRibState:
            return _make_state(peer_idx=99)

        with self.assertRaisesRegex(ValueError, "Missing peer reference 99"):
            await _get_with_fallback(malformed_state, legacy)

    async def test_falls_back_for_servicerouter_unknown_method(self) -> None:
        legacy_entry = bgp_route_types.TRibEntry()

        async def unknown_method() -> bgp_route_types.TCanonicalRibState:
            raise ServiceRouterError(
                type=TransportErrorType.UNKNOWN,
                message="canonical RPC unavailable",
                errno=0,
                options=0,
                reason=ErrorReason.UNKNOWN_METHOD,
            )

        async def legacy() -> list[bgp_route_types.TRibEntry]:
            return [legacy_entry]

        self.assertEqual(
            [legacy_entry], await _get_with_fallback(unknown_method, legacy)
        )

        async def receive_timeout() -> bgp_route_types.TCanonicalRibState:
            raise ServiceRouterError(
                type=TransportErrorType.TIMED_OUT,
                message="timeout",
                errno=0,
                options=0,
                reason=ErrorReason.RECV_TIMEOUT,
            )

        with self.assertRaises(ServiceRouterError):
            await _get_with_fallback(receive_timeout, legacy)

    def test_sync_falls_back_only_for_unknown_method(self) -> None:
        legacy_entry = bgp_route_types.TRibEntry()

        def unknown_method() -> bgp_route_types.TCanonicalRibState:
            raise ApplicationError(
                ApplicationErrorType.UNKNOWN_METHOD, "canonical RPC unavailable"
            )

        def legacy() -> list[bgp_route_types.TRibEntry]:
            return [legacy_entry]

        self.assertEqual(
            [legacy_entry], _get_with_fallback_sync(unknown_method, legacy)
        )

        def internal_error() -> bgp_route_types.TCanonicalRibState:
            raise ApplicationError(ApplicationErrorType.INTERNAL_ERROR, "boom")

        with self.assertRaises(ApplicationError):
            _get_with_fallback_sync(internal_error, legacy)

        with self.assertRaisesRegex(ValueError, "Missing peer reference 99"):
            _get_with_fallback_sync(lambda: _make_state(peer_idx=99), legacy)
