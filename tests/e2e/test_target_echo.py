#!/usr/bin/env python3
"""
prefix.target echo e2e (backlog C5 / design-review R, spec §2.2.2.1).

target is a property of the request prefix: if a client sets prefix.target, the
server MUST reflect that same target in the prefix of EVERY corresponding response
Notification — atomic ones included (those carry their own container prefix and
previously dropped target). If the client did NOT set it, the server MUST NOT set
it.

/system is the ideal probe: one query yields both a non-atomic notification (the
/system/config scalars) and an atomic one (the NTP server record), so a single
Get exercises both notification shapes.

Usage:
  python3 tests/e2e/test_target_echo.py
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
TARGET = "psc-router-7"


def system_path():
    p = gnmi_pb2.Path()
    p.elem.add(name="system")
    return p


def get_notifications(stub, target):
    req = gnmi_pb2.GetRequest(encoding=gnmi_pb2.JSON_IETF)
    req.path.append(system_path())
    if target:
        req.prefix.target = target
    return list(stub.Get(req, timeout=5).notification)


def once_notifications(stub, target):
    sl = gnmi_pb2.SubscriptionList(mode=gnmi_pb2.SubscriptionList.ONCE,
                                   encoding=gnmi_pb2.JSON_IETF)
    if target:
        sl.prefix.target = target
    sub = sl.subscription.add()
    sub.path.CopyFrom(system_path())
    notes = []

    def it():
        yield gnmi_pb2.SubscribeRequest(subscribe=sl)
        while True:
            time.sleep(0.2)

    call = stub.Subscribe(it(), timeout=8)
    try:
        for resp in call:
            if resp.WhichOneof("response") == "update":
                notes.append(resp.update)
            elif resp.WhichOneof("response") == "sync_response":
                break
    finally:
        call.cancel()
    return notes


def check(label, notes, target, errors):
    atomic = sum(1 for n in notes if n.atomic)
    nonatomic = sum(1 for n in notes if not n.atomic)
    bad = [n for n in notes if n.prefix.target != target]
    tag = f"target={target!r}" if target else "no target"
    print(f"  {label} ({tag}): {len(notes)} notes "
          f"({atomic} atomic / {nonatomic} non-atomic), "
          f"{len(notes) - len(bad)}/{len(notes)} carry the expected target")
    if not notes:
        errors.append(f"{label}: no notifications")
    if bad:
        errors.append(f"{label}: {len(bad)} notification(s) with "
                      f"prefix.target={bad[0].prefix.target!r}, expected {target!r}")
    return atomic, nonatomic


def run():
    stub = gnmi_pb2_grpc.gNMIStub(grpc.insecure_channel(SERVER))
    errors = []
    print(f"\n=== prefix.target echo test against {SERVER} ===\n")

    # Get with a target → echoed on every notification, atomic + non-atomic.
    a, na = check("Get /system", get_notifications(stub, TARGET), TARGET, errors)
    if a == 0 or na == 0:
        errors.append("Get /system did not exercise BOTH atomic and non-atomic "
                      f"notifications (atomic={a}, non-atomic={na})")

    # Get without a target → MUST NOT be set (expected target == "").
    check("Get /system", get_notifications(stub, ""), "", errors)

    # Subscribe ONCE with a target → same echo (the buildSubscribeNotifications path).
    check("ONCE /system", once_notifications(stub, TARGET), TARGET, errors)

    print("\n=== Results ===")
    if errors:
        print("  FAIL")
        for e in errors:
            print(f"    - {e}")
        sys.exit(1)
    print("  PASS")


if __name__ == "__main__":
    run()
