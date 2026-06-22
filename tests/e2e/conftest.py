"""Shared pytest fixtures for the psc-mock-server e2e suite.

Each test gets its **own** psc-mock-server instance on a private port (the
`gnmi_server` fixture), torn down at the end of the test. That is the isolation
boundary: tests that Set/delete leaves (the NTP record, /system/config) no longer
pollute each other, so the suite is order-independent and survives a crashed test
— no shared mutable state, no per-test state-restore bookkeeping.

Run the whole suite from the repo root:  `pytest tests/e2e`
Point at a custom binary with:            `PSC_SERVER_BIN=/path/to/server pytest ...`
"""

import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
# Put tests/e2e on sys.path so the generated proto namespace package
# (github.com.openconfig...) and the gnmi_helpers module both import, whether run
# under pytest or directly.
sys.path.insert(0, str(_HERE))

import grpc  # noqa: E402
from github.com.openconfig.gnmi.proto.gnmi import (  # noqa: E402
    gnmi_pb2,
    gnmi_pb2_grpc,
)

_REPO_ROOT = _HERE.parent.parent
_SERVER_BIN = Path(
    os.environ.get("PSC_SERVER_BIN", _REPO_ROOT / "build" / "psc-mock-server")
)
_READY_TIMEOUT_S = 10


def _free_port():
    """An ephemeral localhost port. Closed immediately; the small reuse race is
    acceptable for a local test harness."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class GnmiServer:
    """A running server instance: its `addr` (host:port) and the child process."""

    def __init__(self, addr, proc, log):
        self.addr = addr
        self.proc = proc
        self._log = log

    def log_tail(self):
        self._log.seek(0)
        return self._log.read()


@pytest.fixture
def gnmi_server(request):
    """Spawn a fresh psc-mock-server on a private port; wait until it serves; tear
    it down afterwards. Function-scoped so every test starts from pristine state.

    Extra server args can be passed via indirect parametrization, e.g.
    `@pytest.mark.parametrize("gnmi_server", [["-s"]], indirect=True)` to enable the
    sim-control channel (see the `sim_stub` fixture)."""
    if not _SERVER_BIN.exists():
        pytest.skip(
            f"server binary not built: {_SERVER_BIN}\n"
            "build it first, e.g.  meson compile -C build"
        )

    extra_args = list(getattr(request, "param", []))
    addr = f"127.0.0.1:{_free_port()}"
    # Log to a temp file (not a PIPE) so a chatty subscribe loop can never fill a
    # pipe buffer and stall the server; surfaced only if startup fails.
    log = tempfile.TemporaryFile(mode="w+")
    proc = subprocess.Popen(
        [str(_SERVER_BIN), "-f", "-b", addr, "-l", "1", *extra_args],
        stdout=log,
        stderr=subprocess.STDOUT,
    )

    channel = grpc.insecure_channel(addr)
    deadline = time.time() + _READY_TIMEOUT_S
    try:
        while True:
            if proc.poll() is not None:
                log.seek(0)
                raise RuntimeError(
                    f"server exited early (code {proc.returncode}) on {addr}:\n"
                    f"{log.read()}"
                )
            try:
                grpc.channel_ready_future(channel).result(timeout=0.3)
                break
            except grpc.FutureTimeoutError:
                if time.time() > deadline:
                    log.seek(0)
                    raise RuntimeError(
                        f"server not ready within {_READY_TIMEOUT_S}s on {addr}:\n"
                        f"{log.read()}"
                    )
    finally:
        channel.close()

    server = GnmiServer(addr, proc, log)
    try:
        yield server
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)
        log.close()


@pytest.fixture
def channel(gnmi_server):
    """An insecure channel to this test's server, closed on teardown."""
    ch = grpc.insecure_channel(gnmi_server.addr)
    try:
        yield ch
    finally:
        ch.close()


@pytest.fixture
def stub(channel):
    """A gNMI stub on this test's server."""
    return gnmi_pb2_grpc.gNMIStub(channel)


@pytest.fixture
def sim_stub(channel):
    """A SimControl stub (sim-only hardware-event channel) on this test's server.
    Requires the server to be started with `-s`; pair it with
    `@pytest.mark.parametrize("gnmi_server", [["-s"]], indirect=True)`."""
    import sim_control_pb2_grpc  # noqa: E402  (flat module on tests/e2e sys.path)

    return sim_control_pb2_grpc.SimControlStub(channel)
