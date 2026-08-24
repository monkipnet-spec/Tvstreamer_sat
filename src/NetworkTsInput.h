#pragma once

#include "ConfigManager.h"

#include <gst/gst.h>
#include <string>

struct StreamState;

namespace tvs::network_input {

using ConfigureTsMuxFn = void (*)(GstElement* mux, const StreamConfig& cfg);

// Isolated TVStreamer5-style network MPEG-TS input path.  DVB and UDP/RTP
// inputs deliberately do not enter this module.
bool handles(const StreamConfig& cfg);

GstElement* build(
    StreamState* state,
    GstElement* pipeline,
    GstElement*& terminalElement,
    GCallback hlsPadAddedCallback,
    ConfigureTsMuxFn configureTsMux,
    std::string& error);

} // namespace tvs::network_input
