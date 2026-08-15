#pragma once

#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "ConfigManager.h"

namespace StableUdpOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    std::string& error,
    std::atomic<uint64_t>* networkBytes = nullptr);

} // namespace StableUdpOutput
