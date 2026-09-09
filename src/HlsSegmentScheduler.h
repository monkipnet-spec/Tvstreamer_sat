#pragma once

#include "ConfigManager.h"

#include <gst/gst.h>

#include <memory>
#include <string>

namespace tvs::hls_scheduler {

// Own HLS segment downloader used instead of hlsdemux prefetching.  Segments are
// downloaded quickly, but only when the duration already admitted downstream
// falls below the low watermark.  The scheduler preserves MPEG-TS bytes; only
// GstBuffer timing metadata is generated so downstream queues can account time.
class Scheduler {
public:
    Scheduler(GstElement* appsrc, GstElement* terminalQueue, StreamConfig config);
    ~Scheduler();

    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;

    bool start(std::string& error);
    void stop();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tvs::hls_scheduler
