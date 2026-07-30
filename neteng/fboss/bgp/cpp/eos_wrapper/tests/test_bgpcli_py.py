# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.
# pyre-strict

import sys
from unittest import TestCase
from unittest.mock import MagicMock, patch


# BgpCli imports Arista-only runtime modules (CliExtension, Tac) that do not
# exist in the test environment. Stub them so BgpCli can be imported. The
# command base classes must be real classes since BgpCli subclasses them.
_cli_extension_stub = MagicMock()
_cli_extension_stub.CliCommandClass = object
_cli_extension_stub.ShowCommandClass = object
sys.modules.setdefault("CliExtension", _cli_extension_stub)
sys.modules.setdefault("Tac", MagicMock())

# BgpCli.py is pulled in via its export (//neteng/fboss/bgp/cpp:eos_wrapper/
# BgpCli.py), which buck places under an eos_wrapper/ subdir of this test
# package -- hence the .eos_wrapper import path. The synthetic subpackage only
# exists in the buck-materialized runtime, so it is unresolvable to the static
# type checker.
# pyrefly: ignore [missing-import]
from .eos_wrapper import BgpCli


_MODULE = "neteng.fboss.bgp.cpp.eos_wrapper.tests.eos_wrapper.BgpCli"


def _ctx(*args: object) -> MagicMock:
    """Build a ctx whose .args mimics the EOS positional-arg ordered dict."""
    ctx = MagicMock()
    ctx.args = {str(i): a for i, a in enumerate(args)}
    return ctx


class ClearBgpCppCountersTest(TestCase):
    """clear bgpcpp counters -> bgpcli clear bgp counters wiring."""

    @patch(f"{_MODULE}.Tac")
    @patch(f"{_MODULE}.use_docker", return_value=False)
    def test_clear_specific_peer(
        self, _use_docker: MagicMock, mock_tac: MagicMock
    ) -> None:
        # A specific peer clears just that peer and needs no confirmation.
        mock_tac.run.return_value = ""
        BgpCli.ClearBgpCppCounters().handler(
            _ctx("clear", "bgpcpp", "counters", "2401:db00::1")
        )
        self.assertEqual(
            mock_tac.run.call_args[0][0],
            [
                "bgpcli",
                "--ssl-policy=plaintext",
                "clear",
                "bgp",
                "counters",
                "2401:db00::1",
            ],
        )

    @patch(f"{_MODULE}.Tac")
    @patch(f"{_MODULE}.use_docker", return_value=False)
    def test_clear_no_peer_raises(
        self, _use_docker: MagicMock, mock_tac: MagicMock
    ) -> None:
        # The grammar requires a peer address; a peerless invocation is rejected
        # in the handler rather than issuing a peerless (and rejected) bgpcli
        # call.
        with self.assertRaises(ValueError):
            BgpCli.ClearBgpCppCounters().handler(_ctx("clear", "bgpcpp", "counters"))
        mock_tac.run.assert_not_called()
