#!/usr/bin/env python3
"""
SAMPLE sample_interval floor e2e (§3.5.1.5.2 / 防呆).

The target's lowest supported SAMPLE interval is 200ms. A non-zero sample_interval
below that is unsupportable, so it MUST be rejected with InvalidArgument (and would
otherwise flood the push loop, which no longer has the old 200ms sleep cap).
sample_interval == 0 means "lowest supported" and establishes normally.
"""

import grpc
from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import hold_open, psc_path


def _sample_request(interval_ns):
    sl = gnmi_pb2.SubscriptionList(mode=gnmi_pb2.SubscriptionList.STREAM)
    sub = sl.subscription.add()
    sub.path.CopyFrom(psc_path("PSC-0", "state", "temperature"))
    sub.mode = gnmi_pb2.SAMPLE
    if interval_ns:
        sub.sample_interval = interval_ns
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def test_sample_interval_below_floor_rejected(stub):
    """1 ns is below the 200ms floor → InvalidArgument (RPC closed)."""
    call = stub.Subscribe(hold_open(_sample_request(1)), timeout=10)
    code = None
    try:
        for _ in call:
            pass
    except grpc.RpcError as e:
        code = e.code()
    finally:
        call.cancel()
    assert code == grpc.StatusCode.INVALID_ARGUMENT, \
        f"sample_interval=1ns: expected INVALID_ARGUMENT, got {code}"


def test_sample_interval_zero_establishes(stub):
    """0 means 'lowest supported' → the subscription establishes (sync_response)."""
    call = stub.Subscribe(hold_open(_sample_request(0)), timeout=10)
    saw_sync = False
    try:
        for resp in call:
            if resp.WhichOneof("response") == "sync_response":
                saw_sync = True
                break
    finally:
        call.cancel()
    assert saw_sync, "sample_interval=0 should establish (lowest supported)"
