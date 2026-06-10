#!/usr/bin/env python3
"""
Atomic-container end-to-end test for psc-mock-server (spec §2.1.1 / §3.5.2.5).

The NTP server record /system/ntp/servers/server[address=10.0.0.1]/config is an
atomic container: its leaves (address, port, version, iburst, association-type)
are delivered as ONE atomic Notification — prefix = the container, paths relative,
atomic=true — and any change re-sends the COMPLETE current state (an omitted leaf
is implicitly deleted, never an explicit delete).

This drives, over the real gNMI RPCs:

    Subscribe ON_CHANGE <ntp record>     →  initial atomic Notification (5 leaves)
    Set update .../version = 5           →  atomic re-send, full record, version=5
    Set delete .../iburst                →  atomic re-send, 4 leaves, iburst gone
    Get <ntp record>                     →  atomic Notification too (§3.5.2.5)

Usage:
  python3 tests/e2e/test_atomic.py
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
NEW_VERSION = 5
DELETE_LEAF = "iburst"


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


def leaf_names(notification):
    """Effective leaf names of an atomic notification (paths are relative)."""
    return {path_to_str(u.path).lstrip("/") for u in notification.update}


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


def drive_sets(stub, synced, errors):
    if not synced.wait(timeout=TIMEOUT_S):
        errors.append("sync_response never arrived; Set driver did not run")
        return

    time.sleep(0.5)
    upd = gnmi_pb2.SetRequest()
    u = upd.update.add()
    u.path.CopyFrom(ntp_config_path("version"))
    u.val.uint_val = NEW_VERSION
    try:
        stub.Set(upd, timeout=5)
        print(f"  [client] Set update version={NEW_VERSION}")
    except grpc.RpcError as e:
        errors.append(f"Set update failed: {e.code()}: {e.details()}")

    time.sleep(0.5)
    dele = gnmi_pb2.SetRequest()
    dele.delete.append(ntp_config_path(DELETE_LEAF))
    try:
        stub.Set(dele, timeout=5)
        print(f"  [client] Set delete {DELETE_LEAF}")
    except grpc.RpcError as e:
        errors.append(f"Set delete failed: {e.code()}: {e.details()}")


def check_get(stub, errors):
    """§3.5.2.5: Get on the atomic container must also come back atomic."""
    req = gnmi_pb2.GetRequest(encoding=gnmi_pb2.JSON_IETF)
    req.path.append(ntp_config_path())
    try:
        resp = stub.Get(req, timeout=5)
    except grpc.RpcError as e:
        errors.append(f"Get failed: {e.code()}: {e.details()}")
        return
    if not resp.notification:
        errors.append("Get returned no notification")
        return
    n = resp.notification[0]
    print(f"  [server] Get  atomic={n.atomic} prefix={path_to_str(n.prefix)} "
          f"leaves={sorted(leaf_names(n))}")
    if not n.atomic:
        errors.append("Get on atomic container returned a NON-atomic notification")
    if not path_to_str(n.prefix).endswith("/config"):
        errors.append(f"Get atomic prefix wrong: {path_to_str(n.prefix)}")


def run():
    channel = grpc.insecure_channel(SERVER)
    stub = gnmi_pb2_grpc.gNMIStub(channel)

    print(f"\n=== Atomic-container test against {SERVER} ===\n")

    synced = threading.Event()
    errors = []

    initial_ok = False
    got_version_update = False
    got_iburst_delete = False

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
                print("  [server] sync_response\n")
                continue

            if which != "update":
                continue

            n = resp.update
            names = leaf_names(n)

            if not synced.is_set():
                # Initial atomic snapshot of the whole record.
                print(f"  [server] initial atomic={n.atomic} "
                      f"prefix={path_to_str(n.prefix)} leaves={sorted(names)}")
                if (n.atomic and path_to_str(n.prefix).endswith("/config")
                        and names == {"address", "port", "version",
                                      "iburst", "association-type"}):
                    initial_ok = True
                else:
                    errors.append("initial atomic notification malformed")
                continue

            print(f"  [server] ON_CHANGE atomic={n.atomic} "
                  f"prefix={path_to_str(n.prefix)} leaves={sorted(names)} "
                  f"deletes={[path_to_str(d) for d in n.delete]}")

            if not n.atomic:
                errors.append("ON_CHANGE on atomic container was NOT atomic")

            # Version change → full record re-sent (5 leaves), version == NEW.
            # (Only the version-change notification carries the full 5-leaf set;
            # the later iburst-delete one legitimately has 4, so don't flag it.)
            full = {"address", "port", "version", "iburst", "association-type"}
            if not got_version_update and names == full:
                for u in n.update:
                    if path_to_str(u.path).lstrip("/") == "version" \
                            and u.val.uint_val == NEW_VERSION:
                        got_version_update = True

            # iburst delete → re-send omits iburst (implicit delete), 4 leaves.
            if "iburst" not in names and "version" in names:
                if names == {"address", "port", "version", "association-type"}:
                    got_iburst_delete = True

            if got_version_update and got_iburst_delete:
                break
            if time.time() > deadline:
                break

    except grpc.RpcError as e:
        if e.code() != grpc.StatusCode.CANCELLED:
            print(f"\n[error] gRPC error: {e.code()}: {e.details()}")
            sys.exit(1)
    finally:
        call.cancel()

    check_get(stub, errors)

    if not initial_ok:
        errors.append("never saw a well-formed initial atomic notification")
    if not got_version_update:
        errors.append("never saw the atomic full re-send for the version Set")
    if not got_iburst_delete:
        errors.append("never saw the atomic re-send with iburst implicitly deleted")

    print("\n=== Results ===")
    print(f"  initial atomic record : {initial_ok}")
    print(f"  version full re-send  : {got_version_update}")
    print(f"  iburst implicit delete: {got_iburst_delete}")

    if errors:
        print("  FAIL")
        for e in errors:
            print(f"    - {e}")
        sys.exit(1)
    print("  PASS")


if __name__ == "__main__":
    run()
