#pragma once

#include "ConfigManager.h"

#include <jsoncpp/json/json.h>

namespace tvs::dvb {

// Enumerates Linux DVB adapters/frontends/CA nodes and serial USB reader
// candidates that are currently present in the system.
Json::Value enumerateDevices();

// Tunes a DVB-S/S2 transponder through GStreamer's dvbbasebin and inspects the
// MPEG-TS PAT/SDT/PMT tables. The returned JSON contains service names, SIDs,
// elementary PIDs and whether the service advertises conditional access.
Json::Value scanTransponder(const StreamConfig& cfg, unsigned timeoutMs = 7000);

} // namespace tvs::dvb
