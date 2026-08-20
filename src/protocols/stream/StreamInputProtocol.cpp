#include "protocols/stream/StreamInputProtocol.h"

#include "utils.h"
#include "protocols/stream/inputs/StreamFileInputProtocol.h"
#include "protocols/stream/inputs/StreamHlsInputProtocol.h"
#include "protocols/stream/inputs/StreamHttpInputProtocol.h"
#include "protocols/stream/inputs/StreamRtmpInputProtocol.h"
#include "protocols/stream/inputs/StreamRtpInputProtocol.h"
#include "protocols/stream/inputs/StreamRtspInputProtocol.h"
#include "protocols/stream/inputs/StreamSrtInputProtocol.h"
#include "protocols/stream/inputs/StreamTestInputProtocol.h"
#include "protocols/stream/inputs/StreamUdpInputProtocol.h"
#include "DvbSatellite.h"

namespace tvs::stream_protocols {

InputProtocolKind inputKind(const StreamConfig& cfg) {
    const std::string normalizedInput = normalizeInputUri(cfg.inputUri);
    const std::string input = toLower(normalizedInput);
    const std::string mode = toLower(cfg.inputMode);
    if (inputs::isTestInput(input, mode, cfg.testPattern)) return InputProtocolKind::TestPattern;
    if (inputs::isUdpInput(input, mode, cfg.testPattern)) return InputProtocolKind::Udp;
    if (inputs::isRtpInput(input, mode, cfg.testPattern)) return InputProtocolKind::Rtp;
    if (inputs::isSrtInput(input, mode, cfg.testPattern)) return InputProtocolKind::Srt;
    if (inputs::isRtspInput(input, mode, cfg.testPattern)) return InputProtocolKind::Rtsp;
    if (inputs::isRtmpInput(input, mode, cfg.testPattern)) return InputProtocolKind::Rtmp;
    if (inputs::isHlsInput(input, mode, cfg.testPattern)) return InputProtocolKind::Hls;
    if (inputs::isHttpInput(input, mode, cfg.testPattern)) return InputProtocolKind::Http;
    if (DvbSatellite::isDvbUri(normalizedInput)) return InputProtocolKind::Dvb;
    if (inputs::isFileInput(input, mode, cfg.testPattern)) return InputProtocolKind::File;
    return InputProtocolKind::Unknown;
}

std::string inputKindName(InputProtocolKind kind) {
    switch (kind) {
        case InputProtocolKind::TestPattern: return "test";
        case InputProtocolKind::Udp: return "udp";
        case InputProtocolKind::Rtp: return "rtp";
        case InputProtocolKind::Http: return "http";
        case InputProtocolKind::Hls: return "hls";
        case InputProtocolKind::Srt: return "srt";
        case InputProtocolKind::Rtsp: return "rtsp";
        case InputProtocolKind::Rtmp: return "rtmp";
        case InputProtocolKind::Dvb: return "dvb";
        case InputProtocolKind::File: return "file";
        default: return "unknown";
    }
}

std::vector<const char*> requiredElementsForInput(InputProtocolKind kind) {
    switch (kind) {
        case InputProtocolKind::TestPattern:
            return {"videotestsrc", "audiotestsrc", "x264enc", "avenc_aac", "mpegtsmux"};
        case InputProtocolKind::Udp:
            return {"udpsrc"};
        case InputProtocolKind::Rtp:
            return {"udpsrc", "rtpmp2tdepay"};
        case InputProtocolKind::Http:
            return {"souphttpsrc"};
        case InputProtocolKind::Hls:
            return {"souphttpsrc", "hlsdemux", "mpegtsmux"};
        case InputProtocolKind::Srt:
            return {"srtsrc"};
        case InputProtocolKind::Rtsp:
            return {"rtspsrc", "mpegtsmux"};
        case InputProtocolKind::Rtmp:
            return {"rtmpsrc", "flvdemux", "mpegtsmux"};
        case InputProtocolKind::Dvb:
            return {"dvbsrc", "tsparse", "tsdemux", "mpegtsmux"};
        case InputProtocolKind::File:
            return {"filesrc"};
        default:
            return {};
    }
}

bool isTestPatternInput(InputProtocolKind kind) {
    return kind == InputProtocolKind::TestPattern;
}

bool isUdpLikeInput(InputProtocolKind kind) {
    return kind == InputProtocolKind::Udp || kind == InputProtocolKind::Rtp;
}

bool isHttpLikeInput(InputProtocolKind kind) {
    return kind == InputProtocolKind::Http || kind == InputProtocolKind::Hls;
}

bool isFileInput(InputProtocolKind kind) {
    return kind == InputProtocolKind::File;
}

bool isDvbInput(InputProtocolKind kind) {
    return kind == InputProtocolKind::Dvb;
}

} // namespace tvs::stream_protocols
