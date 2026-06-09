#!/usr/bin/env python3
"""
gnmic end-to-end test suite for psc-mock-server.

Verifies Get and Subscribe ONCE behaviour against a running server.

Usage:
  python3 tests/e2e/test_gnmic.py
  (server must be running: ./build/psc-mock-server --force-insecure)
"""

import json
import shutil
import subprocess
import sys

ADDR = "localhost:50051"

GREEN = "\033[0;32m"
RED   = "\033[0;31m"
CYAN  = "\033[0;36m"
GRAY  = "\033[0;90m"
NC    = "\033[0m"

passed = 0
failed = 0


# ── helpers ───────────────────────────────────────────────────────────────────

def gnmic(*args):
    return subprocess.run(
        ["gnmic", "-a", ADDR, "--insecure"] + list(args),
        capture_output=True, text=True,
    )


def show(text):
    """Print text indented, skipping blank lines."""
    for line in text.splitlines():
        if line.strip():
            print(f"    {line}")


def label(title):
    print(f"\n{CYAN}▶ {title}{NC}")


def path_line(path):
    print(f"  {GRAY}path: {path}{NC}")


def pass_(msg):
    global passed
    passed += 1
    print(f"  {GREEN}✓ PASS{NC}  {msg}")


def fail_(msg):
    global failed
    failed += 1
    print(f"  {RED}✗ FAIL{NC}  {msg}")


def parse_json_stream(text):
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


# ── pre-flight ────────────────────────────────────────────────────────────────

if not shutil.which("gnmic"):
    sys.exit("gnmic not found in PATH — install from https://gnmic.openconfig.net")

if gnmic("capabilities").returncode != 0:
    sys.exit(
        f"Cannot reach server at {ADDR}\n"
        "Start it first:  ./build/psc-mock-server --force-insecure"
    )

print(f"Connected to {CYAN}{ADDR}{NC}")

# ── Get: valid paths ──────────────────────────────────────────────────────────

print(f"\n{CYAN}════ Get: valid paths ════{NC}")

# T1: single leaf
path = "/components/component[name=PSC-0]/state/temperature/instant"
label("T1 · single leaf — PSC-0 temperature")
path_line(path)
r = gnmic("get", "--path", path)
show(r.stdout)
updates = json.loads(r.stdout)[0]["updates"]
if r.returncode == 0 and len(updates) == 1:
    value = list(updates[0]["values"].values())[0]
    pass_(f"temperature = {value:.3f} °C")
else:
    fail_(f"expected 1 update, got {len(updates)}")

# T2: per-unit subtree → 6 leaves
path = "/components/component[name=PSC-1]/power-supply/state"
label("T2 · subtree — PSC-1 power-supply/state  (expect 6 leaves)")
path_line(path)
r = gnmic("get", "--path", path)
show(r.stdout)
updates = json.loads(r.stdout)[0]["updates"]
if r.returncode == 0 and len(updates) == 6:
    pass_(f"got {len(updates)} leaves (expected 6)")
else:
    fail_(f"expected 6 leaves, got {len(updates)}")

# T3: no key → fans out to both PSC-0 and PSC-1
path = "/components/component/power-supply/state/output-power"
label("T3 · no key filter — fans out to all units  (expect 2 leaves)")
path_line(path)
r = gnmic("get", "--path", path)
show(r.stdout)
updates = json.loads(r.stdout)[0]["updates"]
if r.returncode == 0 and len(updates) == 2:
    for u in updates:
        val = list(u["values"].values())[0]
        print(f"    {GRAY}→ {u['Path']} = {val:.1f} W{NC}")
    pass_(f"got {len(updates)} leaves (PSC-0 + PSC-1)")
else:
    fail_(f"expected 2 leaves, got {len(updates)}")

# ── Get: error paths ──────────────────────────────────────────────────────────

print(f"\n{CYAN}════ Get: error paths ════{NC}")

# T4: unknown leaf under known prefix → NOT_FOUND  (spec §3.3.4)
path = "/components/component[name=PSC-0]/nonexistent/leaf"
label("T4 · unknown leaf under known prefix  (expect NOT_FOUND)")
path_line(path)
r = gnmic("get", "--path", path)
show(r.stderr)
if r.returncode != 0 and "NotFound" in r.stderr:
    pass_("got NOT_FOUND")
else:
    fail_(f"expected NOT_FOUND, got exit={r.returncode}")

# T5: non-PSC unit → NOT_FOUND
path = "/components/component[name=FAN-0]/state/temperature/instant"
label("T5 · non-PSC unit (FAN-0)  (expect NOT_FOUND)")
path_line(path)
r = gnmic("get", "--path", path)
show(r.stderr)
if r.returncode != 0 and "NotFound" in r.stderr:
    pass_("got NOT_FOUND")
else:
    fail_(f"expected NOT_FOUND, got exit={r.returncode}")

# T6: entirely unknown prefix → UNIMPLEMENTED  (spec §3.3.4: no provider owns it)
path = "/interfaces/interface[name=eth0]/state/oper-status"
label("T6 · entirely unknown prefix  (expect UNIMPLEMENTED)")
path_line(path)
r = gnmic("get", "--path", path)
show(r.stderr)
if r.returncode != 0 and "Unimplemented" in r.stderr:
    pass_("got UNIMPLEMENTED")
else:
    fail_(f"expected UNIMPLEMENTED, got exit={r.returncode}")

# ── Subscribe ONCE ────────────────────────────────────────────────────────────

print(f"\n{CYAN}════ Subscribe ONCE ════{NC}")

# T7: valid path → value + sync_response
path = "/components/component[name=PSC-0]/state/temperature/instant"
label("T7 · valid path  (expect value + sync_response)")
path_line(path)
r = gnmic("subscribe", "--mode", "once", "--path", path)
show(r.stdout)
notifications = parse_json_stream(r.stdout)
updates_count = sum(1 for n in notifications if "updates" in n)
has_sync      = any(n.get("sync-response") for n in notifications)
if r.returncode == 0 and updates_count >= 1 and has_sync:
    val = list(notifications[0]["updates"][0]["values"].values())[0]
    pass_(f"temperature = {val:.3f} °C  +  sync_response")
else:
    fail_(f"updates={updates_count} sync_response={has_sync}")

# T8: unknown path → silent (sync_response only, RPC not closed)
#     spec §3.5.1.3 MUST NOT close RPC; §3.5.2.4 no value returned for ONCE
path = "/components/component[name=PSC-0]/nonexistent/leaf"
label("T8 · unknown path  (expect silent: sync_response only, RPC not closed)")
path_line(path)
r = gnmic("subscribe", "--mode", "once", "--path", path)
show(r.stdout)
notifications = parse_json_stream(r.stdout)
updates_count = sum(1 for n in notifications if "updates" in n)
has_sync      = any(n.get("sync-response") for n in notifications)
if r.returncode == 0 and updates_count == 0 and has_sync:
    pass_("silent (exit 0, 0 updates, sync_response present)")
else:
    fail_(f"exit={r.returncode} updates={updates_count} sync_response={has_sync}")

# ── Summary ───────────────────────────────────────────────────────────────────

print(f"\n{CYAN}════ Summary ════{NC}")
print(f"  Passed: {GREEN}{passed}{NC}   Failed: {RED}{failed}{NC}")
sys.exit(0 if failed == 0 else 1)
