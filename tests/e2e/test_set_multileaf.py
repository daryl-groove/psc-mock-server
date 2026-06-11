#!/usr/bin/env python3
"""
Multi-leaf Set coherence e2e for psc-mock-server.

One gNMI SetRequest that updates several leaves of the atomic NTP record must
surface as ONE coherent atomic ON_CHANGE notification carrying every new value
together — never split across notifications, never a half-applied record.

Server-side this is guaranteed structurally: Set collects all ops into a single
WriteBatch and commits it under one store lock (see set.cpp / leaf_store.cpp), so
a concurrent Subscribe poll observes the batch all-or-nothing. This test drives
the observable behavior over the real RPCs:

    Subscribe ON_CHANGE <ntp record>                 →  initial atomic (5 leaves)
    Set update version=6 AND association-type=PEER   →  one atomic re-send with
                                                        BOTH new values together

Usage:
  python3 tests/e2e/test_set_multileaf.py
  (server must be running: ./build/psc-mock-server --force-insecure --log-level 4)
"""

import sys
import os
import time
import threading

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2, gnmi_pb2_grpc
import grpc

SERVER = "localhost:50051"
TIMEOUT_S = 20

ADDRESS = "10.0.0.1"
NEW_VERSION = 6
NEW_ASSOC = "PEER"


def ntp_config_path(*suffixes):
    path = gnmi_pb2.Path()
    path.elem.add(name="system")
    path.elem.add(name="ntp")
    path.elem.add(name="servers")
    e = path.elem.add(name="server")
    e.key["address"] = ADDRESS
    path.elem.add(name="config")
    for name in suffixes:
        path.elem.add(name=name)
    return path


def path_to_str(path):
    parts = []
    for e in path.elem:
        seg = e.name
        for k, v in e.key.items():
            seg += f"[{k}={v}]"
        parts.append(seg)
    return "/" + "/".join(parts)


def leaf_map(notification):
    """Relative leaf name -> TypedValue for an atomic notification."""
    return {path_to_str(u.path).lstrip("/"): u.val for u in notification.update}


def build_subscribe_request():
    sub = gnmi_pb2.Subscription()
    sub.path.CopyFrom(ntp_config_path())     # whole record
    sub.mode = gnmi_pb2.ON_CHANGE
    sl = gnmi_pb2.SubscriptionList()
    sl.mode = gnmi_pb2.SubscriptionList.STREAM
    sl.subscription.append(sub)
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def request_iter():
    yield build_subscribe_request()
    while True:
        time.sleep(0.2)


def drive_set(stub, synced, errors):
    if not synced.wait(timeout=TIMEOUT_S):
        errors.append("sync_response never arrived; Set driver did not run")
        return

    time.sleep(0.5)
    # ONE SetRequest, two leaves of the same atomic record.
    req = gnmi_pb2.SetRequest()
    u1 = req.update.add()
    u1.path.CopyFrom(ntp_config_path("version"))
    u1.val.uint_val = NEW_VERSION
    u2 = req.update.add()
    u2.path.CopyFrom(ntp_config_path("association-type"))
    u2.val.string_val = NEW_ASSOC
    try:
        stub.Set(req, timeout=5)
        print(f"  [client] Set version={NEW_VERSION} + association-type={NEW_ASSOC} "
              f"(one request)")
    except grpc.RpcError as e:
        errors.append(f"Set failed: {e.code()}: {e.details()}")


def run():
    channel = grpc.insecure_channel(SERVER)
    stub = gnmi_pb2_grpc.gNMIStub(channel)

    print(f"\n=== Multi-leaf Set coherence test against {SERVER} ===\n")

    synced = threading.Event()
    errors = []
    coherent = False

    driver = threading.Thread(
        target=drive_set, args=(stub, synced, errors), daemon=True)
    driver.start()

    call = stub.Subscribe(request_iter(), timeout=TIMEOUT_S)
    deadline = time.time() + TIMEOUT_S

    try:
        for resp in call:
            which = resp.WhichOneof("response")
            if which == "sync_response":
                synced.set()
                print("  [server] sync_response\n")
                continue
            if which != "update":
                continue

            n = resp.update
            if not synced.is_set():
                continue  # initial snapshot, before the Set

            leaves = leaf_map(n)
            print(f"  [server] ON_CHANGE atomic={n.atomic} "
                  f"leaves={sorted(leaves)}")
            if not n.atomic:
                errors.append("ON_CHANGE on atomic container was NOT atomic")

            # Both edits from the single Set must arrive together in one
            # notification — coherent, not torn across two re-sends.
            if ("version" in leaves and "association-type" in leaves
                    and leaves["version"].uint_val == NEW_VERSION
                    and leaves["association-type"].string_val == NEW_ASSOC):
                coherent = True
                break
            if time.time() > deadline:
                break
    except grpc.RpcError as e:
        if e.code() != grpc.StatusCode.CANCELLED:
            print(f"\n[error] gRPC error: {e.code()}: {e.details()}")
            sys.exit(1)
    finally:
        call.cancel()

    if not coherent:
        errors.append("never saw both leaves in one coherent atomic re-send")

    print("\n=== Results ===")
    print(f"  both edits in one atomic notification: {coherent}")
    if errors:
        print("  FAIL")
        for e in errors:
            print(f"    - {e}")
        sys.exit(1)
    print("  PASS")


if __name__ == "__main__":
    run()
