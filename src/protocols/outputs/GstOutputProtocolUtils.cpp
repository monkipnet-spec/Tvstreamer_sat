#include "protocols/outputs/GstOutputProtocolUtils.h"

#include "protocols/GstProtocolTypes.h"

#include <algorithm>
#include <limits>

namespace tvs::protocols::outputs {

namespace {

uint32_t effectiveVideoPid(const StreamConfig& cfg) {
    return cfg.videoPid > 0 ? cfg.videoPid : 258;
}

uint32_t effectiveAudioPid(const StreamConfig& cfg) {
    return cfg.audioPid > 0 ? cfg.audioPid : 257;
}

uint32_t effectiveServiceId(const StreamConfig& cfg) {
    return std::max<uint32_t>(cfg.serviceId, 1);
}

} // namespace

void addQueue(std::vector<std::string>& args, const std::string& name, uint64_t maxTimeNs, bool leakyDownstream) {
    args.insert(args.end(), {
        "queue",
        "name=" + name,
        "max-size-buffers=0",
        "max-size-bytes=0",
        "max-size-time=" + std::to_string(maxTimeNs)
    });
    if (leakyDownstream) {
        args.push_back("leaky=downstream");
    }
}

std::string safeHost(const std::string& host, const std::string& fallback) {
    return host.empty() ? fallback : host;
}

std::string listenerBindHost(const StreamConfig& cfg) {
    if (!cfg.interfaceAddress.empty()) return cfg.interfaceAddress;
    if (!cfg.outputHost.empty() && cfg.outputHost != "0.0.0.0" && cfg.outputHost != "::") return cfg.outputHost;
    return "0.0.0.0";
}

void appendMpegTsMux(std::vector<std::string>& args, const StreamConfig& cfg) {
    args.insert(args.end(), {
        "mpegtsmux",
        "name=mux",
        "alignment=7",
        "pat-interval=9000",
        "pmt-interval=9000",
        "pcr-interval=1800",
        "si-interval=9000"
    });

    // Every MPEG-TS output produced by the external transcoder uses one
    // deterministic transport-stream bitrate. mpegtsmux inserts NULL packets
    // when the encoded A/V payload is below the selected mux rate, so UDP,
    // SRT, HTTP, HLS, RTP and relay outputs all leave the transcoder as CBR.
    args.push_back("bitrate=" + std::to_string(muxBitrate(cfg)));

    // Remap belongs to the MPEG-TS mux, not to the network sink.  Requesting
    // sink_<PID> pads fixes the elementary PIDs.  prog-map then places both
    // streams in the configured service/program so HLS gets the same remap as
    // UDP/SRT/HTTP/RTP.
    if (cfg.remapEnabled) {
        const uint32_t videoPid = effectiveVideoPid(cfg);
        const uint32_t audioPid = effectiveAudioPid(cfg);
        const uint32_t serviceId = effectiveServiceId(cfg);
        args.push_back(
            "prog-map=program_map,sink_" + std::to_string(videoPid) + "=" + std::to_string(serviceId) +
            ",sink_" + std::to_string(audioPid) + "=" + std::to_string(serviceId));
    }

    args.push_back("!");
}

void appendTsSmoother(std::vector<std::string>& args, const std::string& name, uint32_t smoothingLatencyUs) {
    args.insert(args.end(), {
        "tsparse",
        "name=" + name,
        "set-timestamps=true",
        "smoothing-latency=" + std::to_string(smoothingLatencyUs),
        "alignment=7",
        "!"
    });
}

void appendCbrPacer(std::vector<std::string>& args, const StreamConfig& cfg, const std::string& name) {
    const uint64_t bytesPerSecond64 = std::max<uint64_t>(muxBitrate(cfg) / 8, 1);
    const uint64_t maxIdentityRate = static_cast<uint64_t>(std::numeric_limits<int>::max());
    const int bytesPerSecond = static_cast<int>(std::min<uint64_t>(bytesPerSecond64, maxIdentityRate));

    // identity.datarate retimestamps buffers from byte count and sync=true
    // releases them against the pipeline clock. This prevents the CBR mux from
    // being emitted in periodic bursts when a downstream sink has sync=false.
    args.insert(args.end(), {
        "identity",
        "name=" + name,
        "silent=true",
        "signal-handoffs=false",
        "single-segment=true",
        "sync=true",
        "datarate=" + std::to_string(bytesPerSecond),
        "!"
    });
}

void appendPostMuxAvReservoir(
    std::vector<std::string>& args,
    const std::string& name,
    uint64_t delayNs,
    uint64_t queueMaxNs) {
    // Buffer the already-remapped MPEG-TS as one A/V unit.  This deliberately
    // sits AFTER mpegtsmux/request-pad remapping and AFTER the CBR pacer, so it
    // cannot change service_id/video_pid/audio_pid assignment.  clocksync adds
    // a fixed startup delay without queue min-threshold-time, avoiding periodic
    // rebuffer pauses later in the stream.
    addQueue(args, name + "_queue", queueMaxNs, false);
    args.push_back("!");
    args.insert(args.end(), {
        "clocksync",
        "name=" + name + "_pacer",
        "sync=true",
        "sync-to-first=false",
        "ts-offset=" + std::to_string(delayNs),
        "!"
    });
}

void appendOutputQueue(std::vector<std::string>& args, const std::string& name, bool leakyDownstream) {
    addQueue(args, name, leakyDownstream ? 2000000000ULL : 5000000000ULL, leakyDownstream);
    args.push_back("!");
}

void appendOutputQueueWithTime(std::vector<std::string>& args, const std::string& name, uint64_t maxTimeNs, bool leakyDownstream) {
    addQueue(args, name, maxTimeNs, leakyDownstream);
    args.push_back("!");
}

void assignTsPads(const StreamConfig& cfg, GstOutputSpec& spec) {
    if (!cfg.remapEnabled) {
        spec.videoPad = "mux.";
        spec.audioPad = "mux.";
        return;
    }

    spec.videoPad = "mux.sink_" + std::to_string(effectiveVideoPid(cfg));
    spec.audioPad = "mux.sink_" + std::to_string(effectiveAudioPid(cfg));
}

} // namespace tvs::protocols::outputs
