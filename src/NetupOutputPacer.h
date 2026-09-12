#pragma once

#include <gst/gst.h>

#include <cstdint>
#include <string>

namespace NetupOutputPacer {

// 203.53: Create an in-pipeline byte reservoir for strict NETUP HTTP/SRT CBR
// remux outputs. The returned GstBin exposes a normal sink/src pad pair while
// internally bridging appsink -> bounded byte reservoir -> monotonic sender ->
// appsrc. Upstream may arrive in HLS-sized bursts; downstream receives fixed
// 7x188-byte MPEG-TS chunks at the configured transport bitrate.
GstElement* create(
    uint64_t targetBitrate,
    const std::string& streamId,
    const std::string& outputType,
    const std::string& elementName,
    std::string& error);

} // namespace NetupOutputPacer
