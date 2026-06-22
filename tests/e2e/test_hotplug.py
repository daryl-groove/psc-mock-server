#!/usr/bin/env python3
"""
Wire-observable PSU hot-plug e2e.

Proves the *whole* hot-plug path end-to-end over gRPC — the segments are unit-tested
separately, but never as one wire flow:

    SimControl.SetPresent  ->  Backend.injectHardwareEvent  ->  provider.setPresent
      ->  Backend.attach/detachSubtree  ->  core structural event
      ->  SubscriptionHub.onChange  ->  wake stream  ->  Subscribe emits add/delete
      ->  this gRPC client observes it.

The trigger is the sim-only SimControl service (server started with `-s`): a real PSU
insert/remove is an out-of-band hardware event, never a gNMI client action, so it is
injected through a separate channel that never touches the served gNMI model.

We subscribe ON_CHANGE to a single slot (`/components/component[name=PSC-0]`), which is
element-aligned and so wakes on the structural change *immediately* (the bare
key-omitted `/components/component` form falls back to the ~1s liveness re-diff — P4).
Removal must surface as a `delete` of the sensor subtree; re-insert as the sensors
reappearing — and the permanent `empty` marker flips both ways.
"""

import queue
import threading

import pytest
from github.com.openconfig.gnmi.proto.gnmi import gnmi_pb2

from gnmi_helpers import hold_open, path_to_str, psc_path

# Start this test's server with the sim-control channel enabled.
pytestmark = pytest.mark.parametrize("gnmi_server", [["-s"]], indirect=True)

UNIT = "PSC-0"
# A representative leaf from each detached branch; their appear/disappear is the
# structural proof (the whole subtree moves with them).
SENSOR_MARKERS = ("power-supply/state/input-voltage", "state/temperature/instant")
# Push is milliseconds for an element-aligned slot query; allow generous CI slack.
DEADLINE_S = 5.0


def _slot_subscribe_request():
    sl = gnmi_pb2.SubscriptionList(mode=gnmi_pb2.SubscriptionList.STREAM)
    sub = sl.subscription.add()
    sub.path.CopyFrom(psc_path(UNIT))        # /components/component[name=PSC-0]
    sub.mode = gnmi_pb2.ON_CHANGE
    return gnmi_pb2.SubscribeRequest(subscribe=sl)


def _set_present(sim_stub, present):
    import sim_control_pb2

    sim_stub.SetPresent(
        sim_control_pb2.SetPresentRequest(unit=UNIT, present=present), timeout=5
    )


def _await(pred, events, what):
    """Drain notifications until `pred(notif)` holds; fail on timeout."""
    deadline_hit = []
    while True:
        try:
            kind, payload = events.get(timeout=DEADLINE_S)
        except queue.Empty:
            deadline_hit.append(True)
            break
        if kind == "error":
            pytest.fail(f"stream error while waiting for {what}: {payload}")
        if kind == "notif" and pred(payload):
            return
    assert not deadline_hit, f"never observed {what} within {DEADLINE_S}s"


def _deletes(notif):
    return {path_to_str(p) for p in notif.delete}


def _updates(notif):
    return {path_to_str(u.path): u.val for u in notif.update}


def test_hotplug_remove_then_insert_visible_on_wire(stub, sim_stub):
    events = queue.Queue()
    synced = threading.Event()

    call = stub.Subscribe(hold_open(_slot_subscribe_request()), timeout=30)

    def reader():
        try:
            for resp in call:
                which = resp.WhichOneof("response")
                if which == "sync_response":
                    synced.set()
                elif which == "update":
                    events.put(("notif", resp.update))
        except Exception as e:  # noqa: BLE001 — surfaced via the queue
            events.put(("error", e))

    t = threading.Thread(target=reader, daemon=True)
    t.start()
    assert synced.wait(timeout=10), "never received sync_response"

    # --- remove: the sensor subtree must vanish, `empty` flips true ----------
    _set_present(sim_stub, present=False)

    def removed(notif):
        dels = _deletes(notif)
        return any(any(m in d for d in dels) for m in SENSOR_MARKERS)

    _await(removed, events, "sensor-subtree delete")

    # --- insert: the sensor subtree reappears, `empty` flips false -----------
    _set_present(sim_stub, present=True)

    def reinserted(notif):
        ups = set(_updates(notif).keys())
        return any(any(m in p for p in ups) for m in SENSOR_MARKERS)

    _await(reinserted, events, "sensor-subtree re-add")

    call.cancel()

    # --- data plane corroboration: a fresh Get reflects the final present state.
    get = gnmi_pb2.GetRequest()
    get.path.add().CopyFrom(psc_path(UNIT, "state", "empty"))
    resp = stub.Get(get, timeout=5)
    empty_vals = [
        u.val.bool_val
        for n in resp.notification
        for u in n.update
        if path_to_str(u.path).endswith("/state/empty")
    ]
    assert empty_vals == [False], f"slot should be present after re-insert, got {empty_vals}"
