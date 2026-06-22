/*
 * SimControlService — the gRPC endpoint for the sim/test-only hardware-event
 * channel (proto/sim_control.proto). Translates a SetPresent RPC into a
 * Backend::injectHardwareEvent so an e2e harness can reproduce a PSU hot-plug at a
 * chosen instant. Registered alongside the gNMI service only under the server's
 * --sim flag, so it never ships in a production binary and never touches the served
 * gNMI data model. The Backend is owned by the caller and injected by reference.
 */

#ifndef _SIM_CONTROL_SERVICE_H
#define _SIM_CONTROL_SERVICE_H

#include <sim_control.grpc.pb.h>

#include "backend/backend.hpp"

class SimControlService final : public psc::sim::SimControl::Service {
public:
  explicit SimControlService(gnmid::Backend& backend) : backend_(backend) {}

  grpc::Status SetPresent(grpc::ServerContext* context,
                          const psc::sim::SetPresentRequest* request,
                          psc::sim::SetPresentResponse* response) override;

private:
  gnmid::Backend& backend_;
};

#endif  // _SIM_CONTROL_SERVICE_H
