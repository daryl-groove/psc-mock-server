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
"""

import grpc
from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import gpath

PATH = "/components/component"           # key-omitted list → all units, has data


def _leaf_count(response):
    return sum(len(n.update) for n in response.notification)


def _do_get(stub, path, prefix=None):
    """Returns (leaf_count, None) on OK or (None, grpc.StatusCode) on error."""
    req = gnmi_pb2.GetRequest(encoding=gnmi_pb2.JSON_IETF)
    req.path.append(path)
    if prefix is not None:
        req.prefix.CopyFrom(prefix)
    try:
        return _leaf_count(stub.Get(req, timeout=5)), None
    except grpc.RpcError as e:
        return None, e.code()


def test_origin_handling(stub):
    errors = []

    # 1. origin="openconfig" — the C1 regression: MUST return data, not UNIMPLEMENTED.
    n, code = _do_get(stub, gpath(PATH, "openconfig"))
    print(f"  origin=openconfig  → {code if code else f'{n} leaves'}")
    if code is not None:
        errors.append(f"origin=openconfig: expected data, got {code} (C1 regression)")
    elif n == 0:
        errors.append("origin=openconfig: routed but returned no leaves")

    # 2. origin="" — baseline: the default schema, must behave identically.
    n, code = _do_get(stub, gpath(PATH, ""))
    print(f"  origin=(empty)     → {code if code else f'{n} leaves'}")
    if code is not None:
        errors.append(f"origin=(empty): expected data, got {code}")

    # 3. origin="cli" — an unimplemented schema → UNIMPLEMENTED (deliberate, not a
    #    string-mismatch accident, and not INVALID_ARGUMENT).
    n, code = _do_get(stub, gpath(PATH, "cli"))
    print(f"  origin=cli         → {code}")
    if code != grpc.StatusCode.UNIMPLEMENTED:
        errors.append(f"origin=cli: expected UNIMPLEMENTED, got {code}")

    # 4. origin on the request prefix is validated too (not only the path).
    n, code = _do_get(stub, gpath(PATH, ""), prefix=gpath("", "cli"))
    print(f"  prefix origin=cli  → {code}")
    if code != grpc.StatusCode.UNIMPLEMENTED:
        errors.append(f"prefix origin=cli: expected UNIMPLEMENTED, got {code}")

    assert not errors, "\n".join(errors)
