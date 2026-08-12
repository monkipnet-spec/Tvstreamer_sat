#include "protocols/outputs/GstSrtOutputProtocol.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"
#include "protocols/GstProtocolTypes.h"

namespace tvs::protocols::outputs {

bool appendSrtSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    appendMpegTsMux(args, cfg);
    appendTsSmoother(args, "transcode_srt_ts_smoother", 500000);
    appendCbrPacer(args, cfg, "transcode_srt_cbr_pacer");
    appendPostMuxAvReservoir(args, "transcode_srt_av_reservoir");
    appendOutputQueueWithTime(args, "transcode_srt_output_queue", 5000000000ULL, false);

    const std::string mode = srtOutputMode(cfg);
    const bool caller = mode == "caller";
    const int port = cfg.outputPort > 0 ? cfg.outputPort : 7001;

    if (!caller) {
        const uint16_t relayPort = transcodedSrtInternalPort(cfg);
        args.insert(args.end(), {
            "udpsink",
            "host=127.0.0.1",
            "port=" + std::to_string(relayPort),
            "sync=false",
            "async=false",
            "qos=false",
            "blocksize=1316",
            "buffer-size=8388608"
        });

        assignTsPads(cfg, spec);
        const std::string advertised = safeHost(cfg.outputHost, safeHost(cfg.interfaceAddress, "0.0.0.0"));
        spec.description = "srt-listener-relay@127.0.0.1:" + std::to_string(relayPort) +
                           "->srt://" + advertised + ":" + std::to_string(port);
        return true;
    }

    const std::string uri = "srt://" + safeHost(cfg.outputHost, "127.0.0.1") + ":" +
                            std::to_string(port) + "?mode=caller";

    args.insert(args.end(), {
        "srtsink",
        "uri=" + uri,
        "latency=2500",
        "sync=false",
        "async=false",
        "qos=false",
        "max-lateness=-1",
        "blocksize=1316",
        "wait-for-connection=false",
        "poll-timeout=5000"
    });

    if (!cfg.interfaceAddress.empty() && cfg.interfaceAddress != "0.0.0.0" && cfg.interfaceAddress != "::") {
        args.push_back("localaddress=" + cfg.interfaceAddress);
    }
    args.push_back("localport=0");

    assignTsPads(cfg, spec);
    spec.description = "srt-caller@" + uri;
    return true;
}

} // namespace tvs::protocols::outputs
