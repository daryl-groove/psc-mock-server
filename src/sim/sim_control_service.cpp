#include "sim/sim_control_service.h"

grpc::Status SimControlService::SetPresent(grpc::ServerContext* /*context*/,
                                           const psc::sim::SetPresentRequest* request,
                                           psc::sim::SetPresentResponse* /*response*/) {
  backend_.injectHardwareEvent(request->unit(), request->present());
  return grpc::Status::OK;
}
