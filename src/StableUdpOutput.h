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
    uint64_t queuedChunkPayloadBytes = 0;
    uint64_t queuedChunkCapacityBytes = 0;
    uint64_t queuedChunkCount = 0;
    uint64_t queuedChunkMaxCapacityBytes = 0;
    uint64_t inputRemainderCapacityBytes = 0;
    uint64_t cleanStartCapacityBytes = 0;
    uint64_t senderCreated = 0;
    uint64_t senderDestroyed = 0;
};

MemoryStats memoryStats();

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    std::string& error,
    std::atomic<uint64_t>* networkBytes = nullptr);

} // namespace StableUdpOutput
