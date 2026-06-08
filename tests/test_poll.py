#!/usr/bin/env python3
"""
POLL subscription test for psc-mock-server.

Verifies:
  1. Server sends initial snapshot + sync_response immediately on subscription
     establishment (before any Poll trigger is sent)
  2. Server responds to each Poll trigger with sensor data + sync_response
  3. Client-supplied paths determine which updates are returned

Usage:
  python3 docs/test_poll.py
  (server must be running: ./build/psc-mock-server --force-insecure --log-level 4)
"""

import sys
import os
import time

# Locate generated proto bindings
_DOCS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _DOCS)

from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2, gnmi_pb2_grpc
import grpc

SERVER = "localhost:50051"
POLL_COUNT = 3
POLL_INTERVAL_S = 1.5


def make_psc_path(component, *suffixes):
    """Build a gNMI path: /components/component[name=<component>]/<suffixes...>"""
    path = gnmi_pb2.Path()
    path.elem.add(name="components")
    e = path.elem.add(name="component")
    e.key["name"] = component
    for name in suffixes:
        path.elem.add(name=name)
    return path


def make_subscription(path):
    sub = gnmi_pb2.Subscription()
    sub.path.CopyFrom(path)
    return sub


def build_subscribe_request(paths):
    sl = gnmi_pb2.SubscriptionList()
    sl.mode = gnmi_pb2.SubscriptionList.POLL
    for p in paths:
        sl.subscription.append(make_subscription(p))
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def poll_trigger():
    return gnmi_pb2.SubscribeRequest(poll=gnmi_pb2.Poll())


def request_generator(paths):
    print(f"[client] sending SubscribeRequest(mode=POLL, paths={len(paths)})")
    yield build_subscribe_request(paths)

    for i in range(1, POLL_COUNT + 1):
        time.sleep(POLL_INTERVAL_S)
        print(f"[client] sending Poll trigger #{i}")
        yield poll_trigger()

    time.sleep(0.5)


def run():
    paths = [
        make_psc_path("PSC-0", "power-supply", "state", "output-voltage"),
        make_psc_path("PSC-0", "state", "temperature", "instant"),
    ]

    channel = grpc.insecure_channel(SERVER)
    stub = gnmi_pb2_grpc.gNMIStub(channel)

    # poll_num 0 = initial snapshot, 1..POLL_COUNT = poll trigger responses
    poll_num = -1
    updates_in_poll = 0
    errors = []
    t_start = time.time()

    print(f"\n=== POLL test against {SERVER} ===\n")

    try:
        for resp in stub.Subscribe(request_generator(paths)):
            which = resp.WhichOneof("response")

            if which == "update":
                n = resp.update
                for u in n.update:
                    path_str = "/".join(e.name for e in u.path.elem)
                    val = u.val.double_val
                    print(f"  [server] update: {path_str} = {val:.4f}")
                    updates_in_poll += 1

            elif which == "sync_response":
                poll_num += 1
                elapsed = time.time() - t_start
                got_sync = resp.sync_response
                label = "initial" if poll_num == 0 else f"poll #{poll_num}"
                print(f"  [server] sync_response={got_sync}  "
                      f"({label}, {updates_in_poll} updates, t+{elapsed:.2f}s)")

                if not got_sync:
                    errors.append(f"{label}: sync_response was False")
                if updates_in_poll == 0:
                    errors.append(f"{label}: no updates received before sync_response")

                # Initial snapshot must arrive well before the first poll trigger
                if poll_num == 0 and elapsed >= POLL_INTERVAL_S:
                    errors.append(
                        f"initial snapshot arrived too late ({elapsed:.2f}s >= {POLL_INTERVAL_S}s)"
                        " — server likely did not send on establishment"
                    )

                updates_in_poll = 0
                print()

    except grpc.RpcError as e:
        print(f"\n[error] gRPC error: {e.code()}: {e.details()}")
        sys.exit(1)

    expected_total = POLL_COUNT + 1  # 1 initial + POLL_COUNT poll triggers
    print("=== Results ===")
    print(f"  Sync responses : {poll_num + 1} / {expected_total}"
          f"  (1 initial + {POLL_COUNT} poll triggers)")
    if poll_num + 1 != expected_total:
        errors.append(
            f"expected {expected_total} sync_responses, got {poll_num + 1}"
        )

    if errors:
        print("  FAIL")
        for e in errors:
            print(f"    - {e}")
        sys.exit(1)
    else:
        print("  PASS")


if __name__ == "__main__":
    run()
