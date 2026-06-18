#!/usr/bin/env python3
"""
TARGET_DEFINED setup validation e2e (backlog C3 / spec §3.5.1.5.2, S-P5-c).

For a TARGET_DEFINED subscription the target owns the streaming interval (it picks
ON_CHANGE or SAMPLE per leaf). A client that also pins sample_interval contradicts
that, so the target MUST reject the subscription by closing the Subscribe RPC with
InvalidArgument (§3.5.1.5.2 L1790-1792). A TARGET_DEFINED subscription WITHOUT
sample_interval is valid and establishes normally (sync_response arrives) and
streams at the server-default cadence, not the ~5 Hz loop floor (R1).
"""

import time

import grpc
from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import hold_open, psc_path


def _target_defined_request(sample_interval_ns=0):
    sl = gnmi_pb2.SubscriptionList(mode=gnmi_pb2.SubscriptionList.STREAM)
    sub = sl.subscription.add()
    sub.path.CopyFrom(psc_path("PSC-0", "state", "temperature"))
    sub.mode = gnmi_pb2.TARGET_DEFINED            # enum value 0; set explicitly
    if sample_interval_ns:
        sub.sample_interval = sample_interval_ns
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def test_target_defined_rejects_pinned_interval(stub):
    """TARGET_DEFINED + sample_interval → InvalidArgument (RPC closed)."""
    call = stub.Subscribe(hold_open(_target_defined_request(1_000_000_000)),
                          timeout=10)
    code = None
    try:
        for _ in call:
            pass
    except grpc.RpcError as e:
        code = e.code()
    finally:
        call.cancel()
    assert code == grpc.StatusCode.INVALID_ARGUMENT, \
        f"expected INVALID_ARGUMENT, got {code}"


def test_target_defined_establishes_and_paces(stub):
    """TARGET_DEFINED without sample_interval → establishes AND streams at the
    server-default cadence, not the ~5 Hz loop floor (R1). An Operational leaf
    resolves to SAMPLE; with no client interval the server MUST apply its default
    (~1s) rather than free-run at the 200ms poll floor."""
    call = stub.Subscribe(hold_open(_target_defined_request(0)), timeout=15)
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
    finally:
        call.cancel()

    assert saw_sync, "no sync_response (subscription did not establish)"
    assert len(sample_times) >= 2, (
        f"expected >=2 steady-state SAMPLE updates to measure cadence, "
        f"got {len(sample_times)}")
    min_gap = min(b - a for a, b in zip(sample_times, sample_times[1:]))
    # R1: must stream at the server default (~1s), NOT the ~200ms loop floor (5 Hz).
    assert min_gap >= 0.5, (
        f"SAMPLE cadence too fast ({min_gap:.3f}s between updates) — the "
        f"target-defined leaf is flooding at the loop floor instead of the "
        f"server default")
