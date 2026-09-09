#pragma once

#include "ConfigManager.h"

#include <gst/gst.h>

#include <cstdint>
#include <memory>
#include <string>

namespace tvs::hls_scheduler {

inline constexpr const char* kPipelineDataKey = "tvs-duration-hls-scheduler";
// 203.21: nonzero GINT_TO_POINTER(HTTP status) while the scheduler has
// confirmed that the HLS control resource is persistently unavailable.
inline constexpr const char* kPipelineSourceUnavailableKey =
    "tvs-duration-hls-source-unavailable";
// 203.22: wall-clock lower bound for media already admitted by the HLS
// scheduler. StreamManager uses this only to suppress a false no-input watchdog
// while already-buffered HLS media is still guaranteed to cover playback.
inline constexpr const char* kPipelineBufferedUntilSecKey =
    "tvs-duration-hls-buffered-until-sec";

// Own HLS segment downloader used instead of hlsdemux prefetching.  Segments are
// downloaded quickly, but only when the duration already admitted downstream
// falls below the low watermark.  The scheduler preserves MPEG-TS bytes and
// leaves GstBuffer PTS/DTS/duration unset so provider PCR/PES timing remains the
// only media clock. Consumption is tracked in a private byte/duration ledger.
class Scheduler {
public:
    Scheduler(GstElement* pipeline, GstElement* appsrc, GstElement* terminalQueue, StreamConfig config);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    bool start(std::string& error);
    void stop(bool sendEos = true);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// Returns 404/410 while the scheduler deliberately keeps the pipeline alive
// and probes the source in the background. Zero means normal recovery policy.
int sourceUnavailableHttpStatus(GstElement* pipeline);

// Conservative wall-clock amount of HLS media already pushed into the pipeline.
// It reaches zero automatically as real time advances even when no new segment
// is downloaded, so a dead source cannot be hidden indefinitely.
uint64_t guaranteedBufferedAheadMilliseconds(GstElement* pipeline);

} // namespace tvs::hls_scheduler
