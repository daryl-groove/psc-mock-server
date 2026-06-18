#!/usr/bin/env python3
"""
gnmic end-to-end test for psc-mock-server.

Verifies Get and Subscribe ONCE behaviour through the real gnmic CLI. Skipped
automatically when the gnmic binary is not on PATH.
"""

import json
import shutil
import subprocess

import pytest

pytestmark = pytest.mark.gnmic


def _gnmic(addr, *args):
    return subprocess.run(
        ["gnmic", "-a", addr, "--insecure", *args],
        capture_output=True, text=True,
    )


def _parse_json_stream(text):
    """Parse one or more concatenated JSON objects from a string."""
    decoder = json.JSONDecoder()
    pos, results = 0, []
    text = text.strip()
    while pos < len(text):
        while pos < len(text) and text[pos].isspace():
            pos += 1
        if pos >= len(text):
            break
        obj, pos = decoder.raw_decode(text, pos)
        results.append(obj)
    return results


def _updates(r):
    """The `updates` list of the first notification in a gnmic --format json get,
    or [] if the output is not the expected shape."""
    try:
        return json.loads(r.stdout)[0]["updates"]
    except (json.JSONDecodeError, IndexError, KeyError, TypeError):
        return []


def test_gnmic(gnmi_server):
    if not shutil.which("gnmic"):
        pytest.skip("gnmic not found in PATH — install from https://gnmic.openconfig.net")

    addr = gnmi_server.addr

    def gnmic(*args):
        return _gnmic(addr, *args)

    assert gnmic("capabilities").returncode == 0, \
        f"cannot reach server at {addr} via gnmic capabilities"

    failures = []

    # ── Get: valid paths ──────────────────────────────────────────────────────

    # T1: single leaf
    path = "/components/component[name=PSC-0]/state/temperature/instant"
    r = gnmic("get", "--path", path)
    if not (r.returncode == 0 and len(_updates(r)) == 1):
        failures.append(f"T1 single leaf: expected 1 update, got {len(_updates(r))} "
                        f"(rc={r.returncode}) {r.stderr.strip()}")

    # T2: per-unit subtree → 6 leaves
    path = "/components/component[name=PSC-1]/power-supply/state"
    r = gnmic("get", "--path", path)
    if not (r.returncode == 0 and len(_updates(r)) == 6):
        failures.append(f"T2 subtree: expected 6 leaves, got {len(_updates(r))} "
                        f"(rc={r.returncode}) {r.stderr.strip()}")

    # T3: no key → fans out to both PSC-0 and PSC-1
    path = "/components/component/power-supply/state/output-power"
    r = gnmic("get", "--path", path)
    if not (r.returncode == 0 and len(_updates(r)) == 2):
        failures.append(f"T3 no-key fan-out: expected 2 leaves, got {len(_updates(r))} "
                        f"(rc={r.returncode}) {r.stderr.strip()}")

    # ── Get: error paths ──────────────────────────────────────────────────────

    # T4: unknown leaf under known prefix → NOT_FOUND (spec §3.3.4)
    path = "/components/component[name=PSC-0]/nonexistent/leaf"
    r = gnmic("get", "--path", path)
    if not (r.returncode != 0 and "NotFound" in r.stderr):
        failures.append(f"T4 unknown leaf: expected NOT_FOUND, got rc={r.returncode} "
                        f"{r.stderr.strip()}")

    # T5: non-PSC unit → NOT_FOUND
    path = "/components/component[name=FAN-0]/state/temperature/instant"
    r = gnmic("get", "--path", path)
    if not (r.returncode != 0 and "NotFound" in r.stderr):
        failures.append(f"T5 non-PSC unit: expected NOT_FOUND, got rc={r.returncode} "
                        f"{r.stderr.strip()}")

    # T6: entirely unknown prefix → UNIMPLEMENTED (spec §3.3.4: no provider owns it)
    path = "/interfaces/interface[name=eth0]/state/oper-status"
    r = gnmic("get", "--path", path)
    if not (r.returncode != 0 and "Unimplemented" in r.stderr):
        failures.append(f"T6 unknown prefix: expected UNIMPLEMENTED, got rc={r.returncode} "
                        f"{r.stderr.strip()}")

    # ── Subscribe ONCE ────────────────────────────────────────────────────────

    # T7: valid path → value + sync_response
    path = "/components/component[name=PSC-0]/state/temperature/instant"
    r = gnmic("subscribe", "--mode", "once", "--path", path)
    notes = _parse_json_stream(r.stdout)
    updates_count = sum(1 for n in notes if "updates" in n)
    has_sync = any(n.get("sync-response") for n in notes)
    if not (r.returncode == 0 and updates_count >= 1 and has_sync):
        failures.append(f"T7 once valid: updates={updates_count} sync={has_sync} "
                        f"rc={r.returncode}")

    # T8: unknown path → silent (sync_response only, RPC not closed)
    #     spec §3.5.1.3 MUST NOT close RPC; §3.5.2.4 no value returned for ONCE
    path = "/components/component[name=PSC-0]/nonexistent/leaf"
    r = gnmic("subscribe", "--mode", "once", "--path", path)
    notes = _parse_json_stream(r.stdout)
    updates_count = sum(1 for n in notes if "updates" in n)
    has_sync = any(n.get("sync-response") for n in notes)
    if not (r.returncode == 0 and updates_count == 0 and has_sync):
        failures.append(f"T8 once unknown: updates={updates_count} sync={has_sync} "
                        f"rc={r.returncode}")

    assert not failures, "\n".join(failures)
