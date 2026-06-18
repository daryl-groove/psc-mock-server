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
"""

from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import gpath, hold_open

TARGET = "psc-router-7"


def _get_notifications(stub, target):
    req = gnmi_pb2.GetRequest(encoding=gnmi_pb2.JSON_IETF)
    req.path.append(gpath("/system"))
    if target:
        req.prefix.target = target
    return list(stub.Get(req, timeout=5).notification)


def _once_notifications(stub, target):
    sl = gnmi_pb2.SubscriptionList(mode=gnmi_pb2.SubscriptionList.ONCE,
                                   encoding=gnmi_pb2.JSON_IETF)
    if target:
        sl.prefix.target = target
    sub = sl.subscription.add()
    sub.path.CopyFrom(gpath("/system"))
    notes = []

    call = stub.Subscribe(hold_open(gnmi_pb2.SubscribeRequest(subscribe=sl)),
                          timeout=8)
    try:
        for resp in call:
            if resp.WhichOneof("response") == "update":
                notes.append(resp.update)
            elif resp.WhichOneof("response") == "sync_response":
                break
    finally:
        call.cancel()
    return notes


def _check(label, notes, target, errors):
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


def test_target_echo(stub):
    errors = []

    # Get with a target → echoed on every notification, atomic + non-atomic.
    a, na = _check("Get /system", _get_notifications(stub, TARGET), TARGET, errors)
    if a == 0 or na == 0:
        errors.append("Get /system did not exercise BOTH atomic and non-atomic "
                      f"notifications (atomic={a}, non-atomic={na})")

    # Get without a target → MUST NOT be set (expected target == "").
    _check("Get /system", _get_notifications(stub, ""), "", errors)

    # Subscribe ONCE with a target → same echo (the buildSubscribeNotifications path).
    _check("ONCE /system", _once_notifications(stub, TARGET), TARGET, errors)

    assert not errors, "\n".join(errors)
