#include "protocols/GstInputProtocols.h"

#include "protocols/inputs/GstHttpInputProtocol.h"
#include "protocols/inputs/GstHlsInputProtocol.h"
#include "protocols/inputs/GstRtmpInputProtocol.h"
#include "protocols/inputs/GstRtpInputProtocol.h"
#include "protocols/inputs/GstRtspInputProtocol.h"
#include "protocols/inputs/GstSrtInputProtocol.h"
#include "protocols/inputs/GstUdpInputProtocol.h"

namespace tvs::protocols {

namespace {

void appendSatelliteSource(std::vector<std::string>& args, const StreamConfig& cfg) {
    args.insert(args.end(), {
        "dvbbasebin",
        "name=satellite_src",
        "adapter=" + std::to_string(cfg.satelliteAdapter),
        "frontend=" + std::to_string(cfg.satelliteFrontend),
        "frequency=" + std::to_string(cfg.satelliteFrequency),
        "symbol-rate=" + std::to_string(cfg.satelliteSymbolRate),
        "polarity=" + cfg.satellitePolarization,
        "delsys=" + cfg.satelliteDeliverySystem,
        "modulation=" + cfg.satelliteModulation,
        "code-rate-hp=" + cfg.satelliteFec,
        "pilot=" + cfg.satellitePilot,
        "rolloff=" + cfg.satelliteRolloff,
        "diseqc-source=" + std::to_string(cfg.satelliteDiseqcSource),
        "stream-id=" + std::to_string(cfg.satelliteStreamId),
        "lnb-lof1=" + std::to_string(cfg.satelliteLnbLof1),
        "lnb-lof2=" + std::to_string(cfg.satelliteLnbLof2),
        "lnb-slof=" + std::to_string(cfg.satelliteLnbSlof),
        "tuning-timeout=5000000000"
    });
    if (cfg.satelliteServiceId > 0) {
        args.push_back("program-numbers=" + std::to_string(cfg.satelliteServiceId));
    }
}

} // namespace

std::string inputUriForGstreamer(const StreamConfig& cfg) {
    if (cfg.satelliteEnabled) {
        return "dvb://adapter" + std::to_string(cfg.satelliteAdapter) +
               "/frontend" + std::to_string(cfg.satelliteFrontend) +
               "?frequency=" + std::to_string(cfg.satelliteFrequency) +
               "&symbol-rate=" + std::to_string(cfg.satelliteSymbolRate) +
               "&polarity=" + cfg.satellitePolarization +
               "&program=" + std::to_string(cfg.satelliteServiceId);
    }
    if (inputs::isHlsInput(cfg)) return inputs::hlsInputUri(cfg);
    if (inputs::isSrtInput(cfg)) return inputs::srtInputUri(cfg);
    if (inputs::isRtspInput(cfg)) return inputs::rtspInputUri(cfg);
    if (inputs::isRtmpInput(cfg)) return inputs::rtmpInputUri(cfg);
    if (inputs::isRtpInput(cfg)) return inputs::rtpInputUri(cfg);
    if (inputs::isUdpInput(cfg)) return inputs::udpInputUri(cfg);
    if (inputs::isHttpInput(cfg)) return inputs::httpInputUri(cfg);
    return cfg.inputUri;
}

void appendDecodeInput(std::vector<std::string>& args, const StreamConfig& cfg) {
    if (cfg.satelliteEnabled) {
        appendSatelliteSource(args, cfg);
        args.insert(args.end(), {
            "!", "queue",
            "max-size-buffers=0",
            "max-size-bytes=0",
            "max-size-time=8000000000",
            "!", "decodebin", "name=dec"
        });
        return;
    }

    // TVStreamer handles live IPTV/SRT/HTTP/HLS sources itself.  Do not enable
    // uridecodebin buffering here: on live UDP/SRT inputs it can repeatedly
    // rebuffer the whole external transcoder and cause visible stalls on every
    // output protocol (SRT, HTTP and HLS).
    args.insert(args.end(), {
        "uridecodebin",
        "name=dec",
        "uri=" + inputUriForGstreamer(cfg),
        "use-buffering=false"
    });
}

std::vector<std::string> requiredInputElements() {
    return {"uridecodebin"};
}

std::vector<std::string> requiredInputElements(const StreamConfig& cfg) {
    if (cfg.satelliteEnabled) {
        return {"dvbbasebin", "decodebin", "queue"};
    }
    return requiredInputElements();
}

} // namespace tvs::protocols
