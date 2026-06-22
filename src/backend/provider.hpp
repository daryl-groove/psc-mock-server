/*
 * Provider — a data domain that populates the Backend and may drive its values.
 *
 * The lightweight supertype for the family of data sources. A provider declares
 * its leaves and groups into the shared registry (through the Backend, in its
 * constructor) and, if it has a live source, runs a background driver that pushes
 * values via the Backend's registry (the push-bridge model — device-modelling
 * conventions §8.3). The provider owns its domain knowledge; the Backend owns
 * storage, routing, and the schema/writability plane.
 */

#pragma once

#include <string>

namespace gnmid {

class Backend;

class Provider {
public:
    explicit Provider(Backend& be) : be_(be) {}
    virtual ~Provider() = default;

    Provider(const Provider&)            = delete;
    Provider& operator=(const Provider&) = delete;

    // The namespace root this provider owns, for routing (e.g. "/system"). A path
    // at or under it is "implemented"; anything else is UNIMPLEMENTED.
    virtual std::string domainPrefix() const = 0;

    // Launch any background driver (a sensor simulator / hardware poller). Called
    // by Backend::addProvider after the provider has registered its leaves.
    // Default: no driver (config domains change only via Set).
    virtual void start() {}

    // Out-of-band hardware event: a unit was inserted (present=true) or removed.
    // Routed here by Backend::injectHardwareEvent for every provider; a provider
    // that doesn't own the unit no-ops. This is the generic seam the sim-control
    // backdoor drives — real insert/remove is out-of-band from gNMI, so it never
    // touches the served data model. Default: no hardware (config domains).
    virtual void onHardwareEvent(const std::string& /*unit*/, bool /*present*/) {}

protected:
    Backend& be_;
};

}  // namespace gnmid
