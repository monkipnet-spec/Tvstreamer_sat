#pragma once

#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <string>

#include "ConfigManager.h"

namespace StableUdpOutput {

struct MemoryStats {
    uint64_t packetRingCapacityBytes = 0;
    uint64_t senderCount = 0;
};

MemoryStats memoryStats();

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    std::string& error,
    std::atomic<uint64_t>* networkBytes = nullptr);

} // namespace StableUdpOutput
