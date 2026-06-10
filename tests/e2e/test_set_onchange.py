#!/usr/bin/env python3
"""
Set-driven ON_CHANGE end-to-end test for psc-mock-server.

Unlike test_onchange.py (which leans on the sensor simulator's drift and so can
only smoke-test the wiring), this test *deterministically* drives the two
ON_CHANGE branches the simulator cannot: a value Update and a delete. It does so
over the real gNMI Set RPC against the writable /system/config provider:

    Subscribe ON_CHANGE /system/config   →  initial snapshot + sync_response
    Set update /system/config/hostname   →  ON_CHANGE Update with the new value
    Set delete /system/config/motd-banner →  ON_CHANGE Notification.delete

The poll+diff loop runs at ~5 Hz, so each Set surfaces within a few hundred ms.

Usage:
  python3 tests/e2e/test_set_onchange.py
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

NEW_HOSTNAME = "edge-psc-set"
DELETE_LEAF = "motd-banner"


def sys_config_path(*suffixes):
    path = gnmi_pb2.Path()
    path.elem.add(name="system")
    path.elem.add(name="config")
    for name in suffixes:
        path.elem.add(name=name)
    return path


def path_to_str(path):
    return "/" + "/".join(e.name for e in path.elem)


def build_subscribe_request():
    sub = gnmi_pb2.Subscription()
    sub.path.CopyFrom(sys_config_path())     # whole /system/config subtree
    sub.mode = gnmi_pb2.ON_CHANGE
    sl = gnmi_pb2.SubscriptionList()
    sl.mode = gnmi_pb2.SubscriptionList.STREAM
    sl.subscription.append(sub)
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def request_iter():
    yield build_subscribe_request()
    while True:
        time.sleep(0.2)


def drive_sets(stub, synced, errors):
    """After the initial sync, mutate config so the stream emits an Update then
    a delete. Spaced past the ~200ms poll tick so each lands as its own diff."""
    if not synced.wait(timeout=TIMEOUT_S):
        errors.append("sync_response never arrived; Set driver did not run")
        return

    time.sleep(0.5)
    upd = gnmi_pb2.SetRequest()
    u = upd.update.add()
    u.path.CopyFrom(sys_config_path("hostname"))
    u.val.string_val = NEW_HOSTNAME
    try:
        stub.Set(upd, timeout=5)
        print(f"  [client] Set update hostname={NEW_HOSTNAME!r}")
    except grpc.RpcError as e:
        errors.append(f"Set update failed: {e.code()}: {e.details()}")

    time.sleep(0.5)
    dele = gnmi_pb2.SetRequest()
    dele.delete.append(sys_config_path(DELETE_LEAF))
    try:
        stub.Set(dele, timeout=5)
        print(f"  [client] Set delete {DELETE_LEAF}")
    except grpc.RpcError as e:
        errors.append(f"Set delete failed: {e.code()}: {e.details()}")


def run():
    channel = grpc.insecure_channel(SERVER)
    stub = gnmi_pb2_grpc.gNMIStub(channel)

    print(f"\n=== Set-driven ON_CHANGE test against {SERVER} ===\n")

    synced = threading.Event()
    errors = []

    initial_leaves = 0
    got_hostname_update = False
    got_motd_delete = False

    driver = threading.Thread(
        target=drive_sets, args=(stub, synced, errors), daemon=True)
    driver.start()

    call = stub.Subscribe(request_iter(), timeout=TIMEOUT_S)
    deadline = time.time() + TIMEOUT_S

    try:
        for resp in call:
            which = resp.WhichOneof("response")

            if which == "sync_response":
                synced.set()
                print(f"  [server] sync_response (after {initial_leaves} "
                      f"initial leaves)\n")

            elif which == "update":
                n = resp.update
                if not synced.is_set():
                    initial_leaves += len(n.update)
                    continue

                for u in n.update:
                    p = path_to_str(u.path)
                    print(f"  [server] ON_CHANGE update {p} = "
                          f"{u.val.string_val!r} (ts={n.timestamp})")
                    if p.endswith("/config/hostname"):
                        if u.val.string_val == NEW_HOSTNAME:
                            got_hostname_update = True
                        else:
                            errors.append(
                                f"hostname update had {u.val.string_val!r}, "
                                f"expected {NEW_HOSTNAME!r}")
                for d in n.delete:
                    p = path_to_str(d)
                    print(f"  [server] ON_CHANGE delete {p} (ts={n.timestamp})")
                    if p.endswith(f"/config/{DELETE_LEAF}"):
                        got_motd_delete = True

            if got_hostname_update and got_motd_delete:
                break
            if time.time() > deadline:
                break

    except grpc.RpcError as e:
        if e.code() != grpc.StatusCode.CANCELLED:
            print(f"\n[error] gRPC error: {e.code()}: {e.details()}")
            sys.exit(1)
    finally:
        call.cancel()

    # ---- assertions ----
    if initial_leaves < 3:
        errors.append(
            f"initial snapshot had {initial_leaves} leaves, expected >=3 "
            "(hostname, login-banner, motd-banner)")
    if not got_hostname_update:
        errors.append("never saw ON_CHANGE Update for the Set hostname change")
    if not got_motd_delete:
        errors.append("never saw ON_CHANGE delete for the Set motd-banner delete")

    print("\n=== Results ===")
    print(f"  initial leaves     : {initial_leaves}")
    print(f"  hostname update    : {got_hostname_update}")
    print(f"  motd-banner delete : {got_motd_delete}")

    if errors:
        print("  FAIL")
        for e in errors:
            print(f"    - {e}")
        sys.exit(1)
    print("  PASS")


if __name__ == "__main__":
    run()
