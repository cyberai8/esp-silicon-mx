#include "gps_service.h"

GpsService& GpsService::Instance() {
    static GpsService inst;
    return inst;
}

bool GpsService::Start() {
    return false;
}

GpsService::Snapshot GpsService::GetSnapshot() const {
    return {};
}
