#!/usr/bin/env python3
"""
TARGET_DEFINED setup validation e2e (backlog C3 / spec §3.5.1.5.2, S-P5-c).

For a TARGET_DEFINED subscription the target owns the streaming interval (it picks
ON_CHANGE or SAMPLE per leaf). A client that also pins sample_interval contradicts
that, so the target MUST reject the subscription by closing the Subscribe RPC with
InvalidArgument (§3.5.1.5.2 L1790-1792). A TARGET_DEFINED subscription WITHOUT
sample_interval is valid and establishes normally (sync_response arrives).

Usage:
  python3 tests/e2e/test_target_defined.py
  (server must be running: ./build/psc-mock-server --force-insecure --log-level 1)
"""

import sys
import os
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2, gnmi_pb2_grpc
import grpc

SERVER = "localhost:50051"


def temp_path():
    path = gnmi_pb2.Path()
    path.elem.add(name="components")
    e = path.elem.add(name="component")
    e.key["name"] = "PSC-0"
    path.elem.add(name="state")
    path.elem.add(name="temperature")
    return path


def subscribe_request(sample_interval_ns):
    sl = gnmi_pb2.SubscriptionList()
    sl.mode = gnmi_pb2.SubscriptionList.STREAM
    sub = sl.subscription.add()
    sub.path.CopyFrom(temp_path())
    sub.mode = gnmi_pb2.TARGET_DEFINED            # enum value 0; set explicitly
    if sample_interval_ns:
        sub.sample_interval = sample_interval_ns
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def req_iter(sample_interval_ns):
    yield subscribe_request(sample_interval_ns)
    while True:
        time.sleep(0.2)


def negative(stub):
    """TARGET_DEFINED + sample_interval → InvalidArgument (RPC closed)."""
    call = stub.Subscribe(req_iter(1_000_000_000), timeout=10)
    try:
        for _ in call:
            pass
        return "stream stayed open (expected INVALID_ARGUMENT)"
    except grpc.RpcError as e:
        if e.code() == grpc.StatusCode.INVALID_ARGUMENT:
            return None
        return f"expected INVALID_ARGUMENT, got {e.code()}"
    finally:
        call.cancel()


def positive(stub):
    """TARGET_DEFINED without sample_interval → establishes AND streams at the
    server-default cadence, not the ~5 Hz loop floor (R1). An Operational leaf
    resolves to SAMPLE; with no client interval the server MUST apply its default
    (~1s) rather than free-run at the 200ms poll floor."""
    call = stub.Subscribe(req_iter(0), timeout=15)
    saw_sync = False
    sample_times = []                       # arrival times of steady-state ticks
    deadline = time.time() + 12
    try:
        for resp in call:
            which = resp.WhichOneof("response")
            if which == "sync_response":
                saw_sync = True
            elif which == "update" and saw_sync:
                sample_times.append(time.monotonic())
            if len(sample_times) >= 3 or time.time() > deadline:
                break
    except grpc.RpcError as e:
        return f"unexpected error: {e.code()} {e.details()}"
    finally:
        call.cancel()

    if not saw_sync:
        return "no sync_response (subscription did not establish)"
    if len(sample_times) < 2:
        return (f"expected >=2 steady-state SAMPLE updates to measure cadence, "
                f"got {len(sample_times)}")
    min_gap = min(b - a for a, b in zip(sample_times, sample_times[1:]))
    # R1: must stream at the server default (~1s), NOT the ~200ms loop floor (5 Hz).
    if min_gap < 0.5:
        return (f"SAMPLE cadence too fast ({min_gap:.3f}s between updates) — the "
                f"target-defined leaf is flooding at the loop floor instead of the "
                f"server default")
    return None


def run():
    stub = gnmi_pb2_grpc.gNMIStub(grpc.insecure_channel(SERVER))
    errors = []
    print(f"\n=== TARGET_DEFINED setup validation against {SERVER} ===\n")

    e = negative(stub)
    print(f"  TARGET_DEFINED + sample_interval → "
          f"{'INVALID_ARGUMENT' if e is None else e}")
    if e:
        errors.append(f"negative case: {e}")

    e = positive(stub)
    print(f"  TARGET_DEFINED, no interval      → "
          f"{'established + server-paced cadence' if e is None else e}")
    if e:
        errors.append(f"positive case: {e}")

    print("\n=== Results ===")
    if errors:
        print("  FAIL")
        for x in errors:
            print(f"    - {x}")
        sys.exit(1)
    print("  PASS")


if __name__ == "__main__":
    run()
