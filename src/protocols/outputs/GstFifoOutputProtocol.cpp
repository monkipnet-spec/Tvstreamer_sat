#include "protocols/outputs/GstFifoOutputProtocol.h"

#include "protocols/GstProtocolTypes.h"
#include "protocols/outputs/GstOutputProtocolUtils.h"

namespace tvs::protocols::outputs {

bool appendFifoSink(std::vector<std::string>& args, const StreamConfig& cfg, GstOutputSpec& spec) {
    // The FIFO is only an internal hand-off after encoding.  Do not shape or
    // clock the transport here: the normal StableUdpOutput stage owns the final
    // UDP CBR/VBR clock, startup reservoir, periodic PCR and NULL stuffing.
    const std::size_t muxStart = args.size();
    appendMpegTsMux(args, cfg);
    for (std::size_t i = muxStart; i < args.size(); ++i) {
        if (args[i].rfind("bitrate=", 0) == 0) {
            args[i] = "bitrate=0";
            break;
        }
    }
    appendOutputQueue(args, "transcode_fifo_output_queue", false);
    const std::string location = cfg.outputHost.empty() ? transcodedFifoRelayPath(cfg) : cfg.outputHost;
    args.insert(args.end(), {
        "filesink",
        "location=" + location,
        "sync=false",
        "async=false"
    });
    assignTsPads(cfg, spec);
    spec.description = "fifo-relay-unpaced@file://" + location;
    return true;
}

} // namespace tvs::protocols::outputs
