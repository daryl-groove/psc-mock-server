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
    """TARGET_DEFINED without sample_interval → establishes (sync_response)."""
    call = stub.Subscribe(req_iter(0), timeout=10)
    deadline = time.time() + 8
    try:
        for resp in call:
            if resp.WhichOneof("response") == "sync_response":
                return None
            if time.time() > deadline:
                return "no sync_response within 8s"
        return "stream closed before sync_response"
    except grpc.RpcError as e:
        return f"unexpected error: {e.code()} {e.details()}"
    finally:
        call.cancel()


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
          f"{'established (sync_response)' if e is None else e}")
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
