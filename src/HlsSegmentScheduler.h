#pragma once

#include "ConfigManager.h"

#include <gst/gst.h>

#include <memory>
#include <string>

namespace tvs::hls_scheduler {

inline constexpr const char* kPipelineDataKey = "tvs-duration-hls-scheduler";

// Own HLS segment downloader used instead of hlsdemux prefetching.  Segments are
// downloaded quickly, but only when the duration already admitted downstream
// falls below the low watermark.  The scheduler preserves MPEG-TS bytes and
// leaves GstBuffer PTS/DTS/duration unset so provider PCR/PES timing remains the
// only media clock. Consumption is tracked in a private byte/duration ledger.
class Scheduler {
public:
    Scheduler(GstElement* appsrc, GstElement* terminalQueue, StreamConfig config);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    bool start(std::string& error);
    void stop(bool sendEos = true);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tvs::hls_scheduler
