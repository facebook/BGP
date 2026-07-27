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

from __future__ import absolute_import, division, print_function, unicode_literals

import socket
from typing import Sequence

from neteng.fboss.bgp_attr.thrift_types import (
    TAsPathSeg,
    TAsPathSegType,
    TBgpAfi,
    TBgpCommunity,
    TIpPrefix,
)


BGP_PORT = 179
DEFAULT_LOCAL_PREF = 100


class BgpCommunity:
    """BGP community parsing and storage"""

    t: TBgpCommunity
    community: int

    def __init__(self, asn: int, value: int) -> None:
        self.t = TBgpCommunity(asn=asn, value=value)
        self.community = (asn << 16) + value

    @classmethod
    def parse_str(cls, community: str) -> TBgpCommunity:
        assert ":" in community
        (asn, value) = map(int, community.split(":"))
        return TBgpCommunity(asn=asn, value=value)

    @classmethod
    def parse_int(cls, community: int) -> "BgpCommunity":
        asn, value = community >> 16, community & 65535
        return cls(asn=asn, value=value)

    def __repr__(self) -> str:
        return "BgpCommunity({:d}, {:d})".format(self.t.asn, self.t.value)

    def __str__(self) -> str:
        return "{:d}:{:d}".format(self.t.asn, self.t.value)


class IpPrefix:
    """Represent the IPv4/v6 prefix"""

    t: TIpPrefix
    prefix_str: str

    @classmethod
    def parse_cidr_prefix(cls, afi: TBgpAfi, cidr: str) -> TIpPrefix:
        if "/" not in cidr:
            if afi == TBgpAfi.AFI_IPV4:
                cidr = cidr + "/32"
            else:
                cidr = cidr + "/128"
        (addr, num_bits) = cidr.split("/")
        af = socket.AF_INET if afi == TBgpAfi.AFI_IPV4 else socket.AF_INET6
        prefix = socket.inet_pton(af, addr)
        num_bits = int(num_bits)
        return TIpPrefix(afi=afi, prefix_bin=prefix, num_bits=num_bits)

    @classmethod
    def try_parse_cidr_prefix(cls, cidr: str) -> TIpPrefix:
        try:
            return cls.parse_cidr_prefix(TBgpAfi.AFI_IPV4, cidr)
        except:  # noqa: B001 T25377293 Grandfathered in
            try:
                return cls.parse_cidr_prefix(TBgpAfi.AFI_IPV6, cidr)
            except:  # noqa: B001 T25377293 Grandfathered in
                raise

    @classmethod
    def parse_bgp_prefix(cls, afi: TBgpAfi, prefix: str, num_bits: int) -> TIpPrefix:
        """just pad here and invoke constructor"""
        max_len = 4 if afi == TBgpAfi.AFI_IPV4 else 16
        # pyrefly: ignore [bad-argument-type]
        prefix_bin = bytes(prefix) + (b"\x00" * (max_len - len(prefix)))
        return TIpPrefix(afi=afi, prefix_bin=prefix_bin, num_bits=num_bits)

    @classmethod
    def from_thrift(cls, ip_prefix: TIpPrefix) -> "IpPrefix":
        return cls(ip_prefix.afi, ip_prefix.prefix_bin, ip_prefix.num_bits)

    def __init__(self, afi: TBgpAfi, prefix_bin: bytes, num_bits: int) -> None:
        # prefix_bin stored in network byte order
        self.t = TIpPrefix(afi=afi, prefix_bin=prefix_bin, num_bits=num_bits)
        af = socket.AF_INET if afi == TBgpAfi.AFI_IPV4 else socket.AF_INET6
        self.prefix_str = socket.inet_ntop(af, prefix_bin) + "/" + str(num_bits)

    def __hash__(self) -> int:
        # same prefixes with diff prefix len will collide
        return hash(self.prefix_str)

    def __eq__(self, other: object) -> bool:
        return (
            isinstance(other, IpPrefix)
            and self.t.prefix_bin == other.t.prefix_bin
            and self.t.num_bits == other.t.num_bits
        )

    def __str__(self) -> str:
        return self.prefix_str

    def __repr__(self) -> str:
        return "IpPrefix({}, {!r}, {})".format(
            self.t.afi, self.t.prefix_bin, self.t.num_bits
        )


class AsPathSeg:
    """create TAsPathSeg with json format ASPath segment field in bgp update"""

    @classmethod
    def jsonToThrift(cls, as_path: dict[str, Sequence[int]]) -> TAsPathSeg:
        if len(as_path["asSet"]) > 0:
            return TAsPathSeg(seg_type=TAsPathSegType.AS_SET, asns=as_path["asSet"])
        elif len(as_path["asSequence"]) > 0:
            return TAsPathSeg(
                seg_type=TAsPathSegType.AS_SEQUENCE, asns=as_path["asSequence"]
            )
        elif len(as_path["asConfedSequence"]) > 0:
            return TAsPathSeg(
                seg_type=TAsPathSegType.AS_CONFED_SEQUENCE,
                asns=as_path["asConfedSequence"],
            )
        elif len(as_path["asConfedSet"]) > 0:
            return TAsPathSeg(
                seg_type=TAsPathSegType.AS_CONFED_SET, asns=as_path["asConfedSet"]
            )
        raise KeyError(
            "as_path must contain one of: asSet, asSequence, asConfedSequence, asConfedSet"
        )
