#!/usr/bin/env python3
"""
GetRequest.type (CONFIG/STATE/OPERATIONAL) filtering e2e for psc-mock-server.

gNMI Get may scope a query to a data class (spec §3.3.4). The spec treats CONFIG/
STATE/OPERATIONAL as a disjoint partition annotated per leaf, so each leaf belongs
to exactly one class. The server classifies /system config as CONFIG and the
power-supply sensor readings as OPERATIONAL (runtime measurements, the spec's own
operational example). This drives the real Get RPC over both provider shapes:

    /system            type=CONFIG       →  config scalars + the atomic NTP record
    /system            type=STATE        →  NOT_FOUND (no state leaves under /system)
    /components/...     type=OPERATIONAL  →  the read-only sensor leaves
    /components/...     type=STATE        →  NOT_FOUND (sensors are operational, not state)
    /components/...     type=CONFIG       →  NOT_FOUND (sensors carry no config)

A filtered-to-empty result is NOT_FOUND: the path exists but holds nothing of the
requested type. Atomic containers are homogeneous (a `.../config` container is
wholly config-true), so the NTP record survives a CONFIG filter intact, never
torn.
"""

import grpc
from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import gpath

SENSORS = "/components/component"


def _leaf_paths(response):
    """Set of relative leaf path strings across every notification."""
    paths = set()
    for n in response.notification:
        prefix = "/".join(e.name for e in n.prefix.elem)
        for u in n.update:
            tail = "/".join(e.name for e in u.path.elem)
            paths.add(f"{prefix}/{tail}" if prefix else tail)
    return paths


def _do_get(stub, xpath, dtype):
    """Returns (response, None) on OK or (None, grpc.StatusCode) on error."""
    req = gnmi_pb2.GetRequest(encoding=gnmi_pb2.JSON_IETF, type=dtype)
    req.path.append(gpath(xpath))
    try:
        return stub.Get(req, timeout=5), None
    except grpc.RpcError as e:
        return None, e.code()


def test_get_datatype_filtering(stub):
    errors = []

    # /system CONFIG → config data present, including the atomic NTP record whole.
    resp, code = _do_get(stub, "/system", gnmi_pb2.GetRequest.CONFIG)
    if code is not None:
        errors.append(f"/system CONFIG: expected data, got {code}")
    else:
        paths = _leaf_paths(resp)
        print(f"  /system CONFIG  → {len(paths)} leaves")
        if not any("config/hostname" in p for p in paths):
            errors.append("/system CONFIG missing config/hostname")
        ntp = [p for p in paths if "ntp/servers" in p]
        if len(ntp) != 5:                       # whole atomic record, never torn
            errors.append(f"/system CONFIG NTP record not whole: {sorted(ntp)}")

    # /system STATE → nothing of that class lives here (it is all config).
    resp, code = _do_get(stub, "/system", gnmi_pb2.GetRequest.STATE)
    print(f"  /system STATE        → {code}")
    if code != grpc.StatusCode.NOT_FOUND:
        errors.append(f"/system STATE: expected NOT_FOUND, got {code}")

    # Sensors are operational (runtime measurements), not plain state.
    resp, code = _do_get(stub, SENSORS, gnmi_pb2.GetRequest.OPERATIONAL)
    if code is not None:
        errors.append(f"sensors OPERATIONAL: expected data, got {code}")
    else:
        paths = _leaf_paths(resp)
        print(f"  sensors OPERATIONAL  → {len(paths)} leaves")
        if not paths:
            errors.append("sensors OPERATIONAL returned no leaves")

    # ...so STATE must NOT return them (disjoint: operational is not state).
    resp, code = _do_get(stub, SENSORS, gnmi_pb2.GetRequest.STATE)
    print(f"  sensors STATE        → {code}")
    if code != grpc.StatusCode.NOT_FOUND:
        errors.append(f"sensors STATE: expected NOT_FOUND (operational≠state), got {code}")

    # ...and carry no config.
    resp, code = _do_get(stub, SENSORS, gnmi_pb2.GetRequest.CONFIG)
    print(f"  sensors CONFIG       → {code}")
    if code != grpc.StatusCode.NOT_FOUND:
        errors.append(f"sensors CONFIG: expected NOT_FOUND, got {code}")

    assert not errors, "\n".join(errors)
