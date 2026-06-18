#!/usr/bin/env python3
"""
Multi-leaf Set coherence e2e for psc-mock-server.

One gNMI SetRequest that updates several leaves of the atomic NTP record must
surface as ONE coherent atomic ON_CHANGE notification carrying every new value
together — never split across notifications, never a half-applied record.

Server-side this is guaranteed structurally: Set collects all ops into a single
WriteBatch and commits it under one store lock, so a concurrent Subscribe poll
observes the batch all-or-nothing. This test drives the observable behavior over
the real RPCs:

    Subscribe ON_CHANGE <ntp record>                 →  initial atomic (5 leaves)
    Set update version=6 AND association-type=PEER   →  one atomic re-send with
                                                        BOTH new values together
"""

import threading
import time

import grpc
from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import hold_open, leaf_map, ntp_config_path

TIMEOUT_S = 20

NEW_VERSION = 6
NEW_ASSOC = "PEER"


def _subscribe_request():
    sl = gnmi_pb2.SubscriptionList(mode=gnmi_pb2.SubscriptionList.STREAM)
    sub = sl.subscription.add()
    sub.path.CopyFrom(ntp_config_path())     # whole record
    sub.mode = gnmi_pb2.ON_CHANGE
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def _drive_set(stub, synced, errors):
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


def test_set_multileaf_coherent(stub):
    synced = threading.Event()
    errors = []
    coherent = False

    driver = threading.Thread(
        target=_drive_set, args=(stub, synced, errors), daemon=True)
    driver.start()

    call = stub.Subscribe(hold_open(_subscribe_request()), timeout=TIMEOUT_S)
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
            print(f"  [server] ON_CHANGE atomic={n.atomic} leaves={sorted(leaves)}")
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
            raise
    finally:
        call.cancel()

    if not coherent:
        errors.append("never saw both leaves in one coherent atomic re-send")
    assert not errors, "\n".join(errors)
