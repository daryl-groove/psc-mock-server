#!/usr/bin/env python3
"""
Push-native ON_CHANGE latency e2e (P1/P2, S2).

Proves the ON_CHANGE path is now push-driven, not poll-driven: a Set must surface on
the subscribed stream within a few milliseconds, not at the old ~200ms poll tick. The
server's writer thread (the Set RPC) dispatches the change through the core ILeafSink
to the SubscriptionHub, which wakes this stream's loop immediately.

We measure Set-sent -> update-received latency and assert it is well under the old
poll floor. test_set_onchange.py already covers correctness (the value/delete land);
this test is specifically about *when*.
"""

import threading
import time

import grpc
from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import hold_open, sys_config_path

NEW_HOSTNAME = "edge-push"
# Push is ~milliseconds; the old loop polled every 200ms (so a change surfaced in
# [0, 200ms]). A 100ms ceiling cleanly separates push from that poll behaviour while
# leaving push a ~10x margin.
PUSH_LATENCY_MAX_S = 0.10


def _subscribe_request():
    sl = gnmi_pb2.SubscriptionList(mode=gnmi_pb2.SubscriptionList.STREAM)
    sub = sl.subscription.add()
    sub.path.CopyFrom(sys_config_path())     # /system/config (ON_CHANGE)
    sub.mode = gnmi_pb2.ON_CHANGE
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def test_onchange_push_is_immediate(stub):
    synced = threading.Event()
    got = threading.Event()
    state = {"t_arrive": None}
    errors = []

    call = stub.Subscribe(hold_open(_subscribe_request()), timeout=20)

    def reader():
        try:
            for resp in call:
                which = resp.WhichOneof("response")
                if which == "sync_response":
                    synced.set()
                elif which == "update" and synced.is_set():
                    for u in resp.update.update:
                        if (u.path.elem and u.path.elem[-1].name == "hostname"
                                and u.val.string_val == NEW_HOSTNAME):
                            state["t_arrive"] = time.monotonic()
                            got.set()
                            return
        except grpc.RpcError as e:
            if e.code() != grpc.StatusCode.CANCELLED:
                errors.append(f"stream error: {e.code()}: {e.details()}")

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    assert synced.wait(timeout=10), "never received sync_response"
    # The server takes its ON_CHANGE baseline just after sending sync_response; settle
    # past that so the Set is unambiguously a post-baseline change (not folded in).
    time.sleep(0.3)

    t_send = time.monotonic()
    upd = gnmi_pb2.SetRequest()
    u = upd.update.add()
    u.path.CopyFrom(sys_config_path("hostname"))
    u.val.string_val = NEW_HOSTNAME
    stub.Set(upd, timeout=5)

    arrived = got.wait(timeout=5)
    call.cancel()

    assert not errors, "\n".join(errors)
    assert arrived, "ON_CHANGE update for the Set never arrived"

    latency = state["t_arrive"] - t_send
    print(f"  push latency: {latency * 1000:.1f} ms")
    assert latency < PUSH_LATENCY_MAX_S, (
        f"ON_CHANGE update took {latency * 1000:.1f} ms — push should be a few ms, "
        f"not the old poll-tick latency (≤200ms)")
