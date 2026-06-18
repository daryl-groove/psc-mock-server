#!/usr/bin/env python3
"""
ON_CHANGE subscription smoke test for psc-mock-server.

This is a *wiring* test: it proves the end-to-end path
    store change → snapshot() → diff() → Notification → wire
actually streams, plus the collection-time timestamp wiring. The distinctive
ON_CHANGE semantics (suppress-when-unchanged, delete, heartbeat content) are
covered deterministically by tests/test_subscribe_emit.cpp — they are NOT
asserted here, because the sensor simulator drifts continuously and never
removes leaves, so it cannot exercise those branches cleanly.

Verifies:
  1. Initial snapshot (>=1 update) arrives before sync_response (spec MUST)
  2. sync_response is sent
  3. At least one ON_CHANGE update streams after sync (the simulator drifts on
     a 1s tick with 30% step chance per leaf, so a multi-leaf subtree changes)
  4. Notification.timestamp looks like a collection time: a positive int64 in
     nanoseconds, close to wall-clock now (not zero, not emission-far-future)
"""

import time

import grpc
from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import hold_open, psc_path

TIMEOUT_S = 20          # hard cap so the test never hangs on the open stream


def _subscribe_request(paths):
    sl = gnmi_pb2.SubscriptionList(mode=gnmi_pb2.SubscriptionList.STREAM)
    for p in paths:
        sub = sl.subscription.add()
        sub.path.CopyFrom(p)
        sub.mode = gnmi_pb2.ON_CHANGE
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def test_onchange_streams(stub):
    # Broad subtrees → several leaves → a change is near-certain within seconds.
    paths = [
        psc_path("PSC-0", "power-supply", "state"),
        psc_path("PSC-0", "state", "temperature"),
    ]

    saw_sync = False
    initial_updates = 0
    onchange_updates = 0
    errors = []

    now_ns = time.time_ns()
    ts_lo = now_ns - 120_000_000_000     # 2 min in the past
    ts_hi = now_ns + 5_000_000_000       # 5 s in the future

    call = stub.Subscribe(hold_open(_subscribe_request(paths)), timeout=TIMEOUT_S)
    deadline = time.time() + TIMEOUT_S

    try:
        for resp in call:
            which = resp.WhichOneof("response")

            if which == "update":
                n = resp.update
                count = len(n.update) + len(n.delete)

                # Timestamp must be a plausible collection time (ns since epoch).
                if not (ts_lo < n.timestamp < ts_hi):
                    errors.append(
                        f"timestamp {n.timestamp} outside plausible window "
                        f"[{ts_lo}, {ts_hi}] — not a collection time in ns")

                if not saw_sync:
                    initial_updates += count
                    print(f"  [server] initial snapshot: {count} leaves "
                          f"(ts={n.timestamp})")
                else:
                    onchange_updates += 1
                    print(f"  [server] ON_CHANGE update #{onchange_updates}: "
                          f"{len(n.update)} upd / {len(n.delete)} del "
                          f"(ts={n.timestamp})")

            elif which == "sync_response":
                saw_sync = True
                print(f"  [server] sync_response={resp.sync_response} "
                      f"(after {initial_updates} initial leaves)\n")

            if saw_sync and onchange_updates >= 1:
                break
            if time.time() > deadline:
                break

    except grpc.RpcError as e:
        if e.code() != grpc.StatusCode.CANCELLED:
            raise
    finally:
        call.cancel()

    if initial_updates == 0:
        errors.append("no initial snapshot updates before sync_response")
    if not saw_sync:
        errors.append("never received sync_response")
    if onchange_updates == 0:
        errors.append(
            f"no ON_CHANGE update streamed within {TIMEOUT_S}s "
            "(expected the simulator to drift a value)")

    assert not errors, "\n".join(errors)
