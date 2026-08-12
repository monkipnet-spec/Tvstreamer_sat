#include "protocols/outputs/GstHlsOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"

#include <algorithm>
#include <filesystem>

namespace tvs::protocols::outputs {

namespace {

uint32_t hlsVideoPid(const StreamConfig& cfg) {
    return cfg.videoPid > 0 ? cfg.videoPid : 258;
}

uint32_t hlsAudioPid(const StreamConfig& cfg) {
    return cfg.audioPid > 0 ? cfg.audioPid : 257;
}

uint32_t hlsServiceId(const StreamConfig& cfg) {
    return std::max<uint32_t>(cfg.serviceId, 1);
}

std::string prepareHlsDirectory(const StreamConfig& cfg) {
    const std::string dir = "/tmp/tvstreammersat5-hls/" + cfg.id;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    ec.clear();
    std::filesystem::create_directories(dir, ec);
    return dir;
}

void appendHlsMux(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    const uint32_t videoPid = hlsVideoPid(cfg);
    const uint32_t audioPid = hlsAudioPid(cfg);
    const uint32_t serviceId = hlsServiceId(cfg);

    args.insert(args.end(), {
        "mpegtsmux",
        "name=hls_mux",
        "alignment=7",
        "pat-interval=9000",
        "pmt-interval=9000",
        "pcr-interval=1800",
        "si-interval=9000",
        "bitrate=" + std::to_string(muxBitrate(cfg))
    });

    // HLS uses the same transport-stream CBR policy as the other transcoded
    // MPEG-TS outputs. Null-packet stuffing keeps segment size/rate stable.

    if (cfg.remapEnabled) {
        // mpegtsmux uses the numeric suffix of a requested sink_<PID> pad as
        // the elementary-stream PID. prog-map then puts both PIDs into the
        // configured MPEG-TS service/program.
        args.push_back(
            "prog-map=program_map,sink_" + std::to_string(videoPid) + "=" + std::to_string(serviceId) +
            ",sink_" + std::to_string(audioPid) + "=" + std::to_string(serviceId));
        spec.videoPad = "hls_mux.sink_" + std::to_string(videoPid);
        spec.audioPad = "hls_mux.sink_" + std::to_string(audioPid);
    } else {
        spec.videoPad = "hls_mux.";
        spec.audioPad = "hls_mux.";
    }

    args.push_back("!");
}

} // namespace

bool appendHlsSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    // HLS gets its own mux instance and explicit request pads instead of the
    // generic mux helper. This keeps PID/service remapping deterministic for
    // every newly generated segment.
    appendHlsMux(args, cfg, spec);
    appendTsSmoother(args, "transcode_hls_ts_smoother", 500000);
    appendCbrPacer(args, cfg, "transcode_hls_cbr_pacer");
    appendOutputQueueWithTime(args, "transcode_hls_output_queue", 8000000000ULL, false);

    // Remove an old playlist and stale .ts files before starting a new HLS
    // generation. Otherwise a client can briefly read segments from the
    // previous pipeline, including their old PID map.
    const std::string dir = prepareHlsDirectory(cfg);
    args.insert(args.end(), {
        "hlssink",
        "playlist-location=" + dir + "/playlist.m3u8",
        "location=" + dir + "/segment%05d.ts",
        "target-duration=6",
        "max-files=6",
        "playlist-length=3"
    });

    spec.description = "hls@" + dir + "/playlist.m3u8";
    return true;
}

} // namespace tvs::protocols::outputs
