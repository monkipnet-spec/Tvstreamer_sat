#include "protocols/GstOutputProtocols.h"

#include "protocols/GstProtocolTypes.h"
#include "protocols/outputs/GstFifoOutputProtocol.h"
#include "protocols/outputs/GstHlsOutputProtocol.h"
#include "protocols/outputs/GstHttpOutputProtocol.h"
#include "protocols/outputs/GstRtmpOutputProtocol.h"
#include "protocols/outputs/GstRtpOutputProtocol.h"
#include "protocols/outputs/GstRtspOutputProtocol.h"
#include "protocols/outputs/GstSrtOutputProtocol.h"
#include "protocols/outputs/GstUdpOutputProtocol.h"

namespace tvs::protocols {

std::vector<std::string> requiredOutputElements() {
    return {"mpegtsmux", "tsparse", "identity", "udpsink", "rtpmp2tpay", "filesink", "srtsink", "tcpserversink", "hlssink", "flvmux", "rtmpsink", "rtspclientsink"};
}

std::vector<std::string> requiredElementsForOutput(OutputKind kind) {
    switch (kind) {
        case OutputKind::UdpCbr:
        case OutputKind::UdpVbr:
            return {"mpegtsmux", "tsparse", "identity", "udpsink"};
        case OutputKind::FifoRelay:
            return {"mpegtsmux", "filesink"};
        case OutputKind::Rtp:
            return {"mpegtsmux", "tsparse", "identity", "rtpmp2tpay", "udpsink"};
        case OutputKind::Srt:
            return {"mpegtsmux", "tsparse", "identity", "clocksync", "srtsink", "udpsink"};
        case OutputKind::Http:
            return {"mpegtsmux", "tsparse", "identity", "clocksync", "tcpserversink"};
        case OutputKind::Hls:
            return {"mpegtsmux", "tsparse", "identity", "hlssink"};
        case OutputKind::Rtsp:
            return {"rtspclientsink"};
        case OutputKind::Rtmp:
        case OutputKind::Youtube:
            return {"flvmux", "rtmpsink"};
        default:
            return {};
    }
}

bool appendOutputMuxAndSink(
    std::vector<std::string>& args,
    const StreamConfig& cfg,
    GstOutputSpec& spec,
    std::string& error) {
    spec.kind = outputKind(cfg);
    spec.container = isFlvOutput(spec.kind) ? ContainerKind::Flv :
        (isRtspOutput(spec.kind) ? ContainerKind::Rtsp : ContainerKind::MpegTs);

    switch (spec.kind) {
        case OutputKind::UdpCbr:
        case OutputKind::UdpVbr:
            return outputs::appendUdpSink(args, cfg, spec);
        case OutputKind::FifoRelay:
            return outputs::appendFifoSink(args, cfg, spec);
        case OutputKind::Rtp:
            return outputs::appendRtpSink(args, cfg, spec);
        case OutputKind::Srt:
            return outputs::appendSrtSink(args, cfg, spec);
        case OutputKind::Http:
            return outputs::appendHttpSink(args, cfg, spec);
        case OutputKind::Hls:
            return outputs::appendHlsSink(args, cfg, spec);
        case OutputKind::Rtsp:
            return outputs::appendRtspSink(args, cfg, spec);
        case OutputKind::Rtmp:
        case OutputKind::Youtube:
            return outputs::appendRtmpSink(args, cfg, spec);
        default:
            error = "unsupported output protocol: " + cfg.outputType;
            return false;
    }
}

} // namespace tvs::protocols
