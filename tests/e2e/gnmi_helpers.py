"""Shared gNMI path/notification helpers for the e2e suite.

Extracted from the per-test duplication (R8): xpath parsing, the named path
builders, path stringification, the keep-stream-open subscribe iterator, and the
atomic-notification leaf extractors. Tests import what they need from here.
"""

import re
import time

from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

# --------------------------------------------------------------------------- #
# Path building
# --------------------------------------------------------------------------- #

_KEY_RE = re.compile(r"\[([^=\]]+)=([^\]]*)\]")


def _split_elems(xpath):
    """Split an xpath on `/`, ignoring `/` inside `[...]` predicate values."""
    out, buf, depth = [], [], 0
    for ch in xpath.strip("/"):
        if ch == "[":
            depth += 1
            buf.append(ch)
        elif ch == "]":
            depth -= 1
            buf.append(ch)
        elif ch == "/" and depth == 0:
            out.append("".join(buf))
            buf = []
        else:
            buf.append(ch)
    if buf:
        out.append("".join(buf))
    return out


def gpath(xpath, origin=""):
    """Parse an xpath string into a gnmi.Path. Supports `name[k1=v1][k2=v2]`
    elements (multiple keys) and an optional origin."""
    p = gnmi_pb2.Path()
    if origin:
        p.origin = origin
    for elem in _split_elems(xpath):
        name = elem.partition("[")[0]
        if not name:
            continue
        e = p.elem.add(name=name)
        for k, v in _KEY_RE.findall(elem):
            e.key[k] = v
    return p


def psc_path(component, *suffixes):
    """/components/component[name=<component>]/<suffixes...>"""
    xp = f"/components/component[name={component}]"
    for s in suffixes:
        xp += f"/{s}"
    return gpath(xp)


def ntp_config_path(*suffixes, address="10.0.0.1"):
    """/system/ntp/servers/server[address=<address>]/config/<suffixes...>"""
    xp = f"/system/ntp/servers/server[address={address}]/config"
    for s in suffixes:
        xp += f"/{s}"
    return gpath(xp)


def sys_config_path(*suffixes):
    """/system/config/<suffixes...>"""
    xp = "/system/config"
    for s in suffixes:
        xp += f"/{s}"
    return gpath(xp)


def path_to_str(path):
    """Stringify a gnmi.Path, rendering `[k=v]` predicates."""
    parts = []
    for e in path.elem:
        seg = e.name
        for k, v in e.key.items():
            seg += f"[{k}={v}]"
        parts.append(seg)
    return "/" + "/".join(parts)


# --------------------------------------------------------------------------- #
# Subscribe stream driving
# --------------------------------------------------------------------------- #


def hold_open(*requests, interval=0.2):
    """Yield each request, then hold the client→server stream open by sleeping so
    the server keeps streaming until the reader cancels the call."""
    for r in requests:
        yield r
    while True:
        time.sleep(interval)


# --------------------------------------------------------------------------- #
# Notification leaf extractors (atomic notifications carry paths relative to the
# container prefix)
# --------------------------------------------------------------------------- #


def leaf_names(notification):
    """Relative leaf names of a notification's updates."""
    return {path_to_str(u.path).lstrip("/") for u in notification.update}


def leaf_map(notification):
    """Relative leaf name -> TypedValue for a notification's updates."""
    return {path_to_str(u.path).lstrip("/"): u.val for u in notification.update}
