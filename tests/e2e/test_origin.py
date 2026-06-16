#!/usr/bin/env python3
"""
Origin handling e2e for psc-mock-server (backlog C1 / D16 single-origin boundary).

gNMI Path.origin names the schema a path is rooted in. This server implements
exactly one schema ("openconfig"), so the protocol boundary (validateOrigin):

    origin = ""           →  the default schema  → accepted (routes normally)
    origin = "openconfig" →  the implemented one → accepted (routes normally)
    origin = <other>      →  an unimplemented schema → UNIMPLEMENTED

This is the C1 regression: the canonical client sends origin="openconfig", which
the old code embedded into the xpath ("/openconfig:components/...") so it missed
routing and was wrongly rejected with UNIMPLEMENTED. It must now succeed, exactly
like the empty-origin form. A non-openconfig origin must still be UNIMPLEMENTED
(not INVALID_ARGUMENT — that is reserved for a malformed path). origin is checked
on both the path and the request prefix.

Usage:
  python3 tests/e2e/test_origin.py
  (server must be running: ./build/psc-mock-server --force-insecure --log-level 1)
"""

import sys
import os

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2, gnmi_pb2_grpc
import grpc

SERVER = "localhost:50051"
PATH = "/components/component"           # key-omitted list → all units, has data


def make_path(xpath, origin=""):
    path = gnmi_pb2.Path()
    if origin:
        path.origin = origin
    for seg in xpath.strip("/").split("/"):
        if seg:
            path.elem.add(name=seg)
    return path


def leaf_count(response):
    return sum(len(n.update) for n in response.notification)


def do_get(stub, path, prefix=None):
    """Returns (leaf_count, None) on OK or (None, grpc.StatusCode) on error."""
    req = gnmi_pb2.GetRequest(encoding=gnmi_pb2.JSON_IETF)
    req.path.append(path)
    if prefix is not None:
        req.prefix.CopyFrom(prefix)
    try:
        return leaf_count(stub.Get(req, timeout=5)), None
    except grpc.RpcError as e:
        return None, e.code()


def run():
    channel = grpc.insecure_channel(SERVER)
    stub = gnmi_pb2_grpc.gNMIStub(channel)
    errors = []

    print(f"\n=== Origin handling test against {SERVER} ===\n")

    # 1. origin="openconfig" — the C1 regression: MUST return data, not UNIMPLEMENTED.
    n, code = do_get(stub, make_path(PATH, "openconfig"))
    print(f"  origin=openconfig  → {code if code else f'{n} leaves'}")
    if code is not None:
        errors.append(f"origin=openconfig: expected data, got {code} (C1 regression)")
    elif n == 0:
        errors.append("origin=openconfig: routed but returned no leaves")

    # 2. origin="" — baseline: the default schema, must behave identically.
    n, code = do_get(stub, make_path(PATH, ""))
    print(f"  origin=(empty)     → {code if code else f'{n} leaves'}")
    if code is not None:
        errors.append(f"origin=(empty): expected data, got {code}")

    # 3. origin="cli" — an unimplemented schema → UNIMPLEMENTED (deliberate, not a
    #    string-mismatch accident, and not INVALID_ARGUMENT).
    n, code = do_get(stub, make_path(PATH, "cli"))
    print(f"  origin=cli         → {code}")
    if code != grpc.StatusCode.UNIMPLEMENTED:
        errors.append(f"origin=cli: expected UNIMPLEMENTED, got {code}")

    # 4. origin on the request prefix is validated too (not only the path).
    n, code = do_get(stub, make_path(PATH, ""), prefix=make_path("", "cli"))
    print(f"  prefix origin=cli  → {code}")
    if code != grpc.StatusCode.UNIMPLEMENTED:
        errors.append(f"prefix origin=cli: expected UNIMPLEMENTED, got {code}")

    print("\n=== Results ===")
    if errors:
        print("  FAIL")
        for e in errors:
            print(f"    - {e}")
        sys.exit(1)
    print("  PASS")


if __name__ == "__main__":
    run()
