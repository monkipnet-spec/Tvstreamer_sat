#include "protocols/GstInputProtocols.h"

#include "protocols/inputs/GstHttpInputProtocol.h"
#include "protocols/inputs/GstHlsInputProtocol.h"
#include "protocols/inputs/GstRtmpInputProtocol.h"
#include "protocols/inputs/GstRtpInputProtocol.h"
#include "protocols/inputs/GstRtspInputProtocol.h"
#include "protocols/inputs/GstSrtInputProtocol.h"
#include "protocols/inputs/GstUdpInputProtocol.h"

#include <filesystem>
#include <gst/gst.h>

namespace tvs::protocols {

std::string inputUriForGstreamer(const StreamConfig& cfg) {
    if (inputs::isHlsInput(cfg)) return inputs::hlsInputUri(cfg);
    if (inputs::isSrtInput(cfg)) return inputs::srtInputUri(cfg);
    if (inputs::isRtspInput(cfg)) return inputs::rtspInputUri(cfg);
    if (inputs::isRtmpInput(cfg)) return inputs::rtmpInputUri(cfg);
    if (inputs::isRtpInput(cfg)) return inputs::rtpInputUri(cfg);
    if (inputs::isUdpInput(cfg)) return inputs::udpInputUri(cfg);
    if (inputs::isHttpInput(cfg)) return inputs::httpInputUri(cfg);

    // The backup-file library stores filesystem paths. External GStreamer
    // URI sources require file://, so normalize a plain local path here.
    if (!cfg.inputUri.empty() && cfg.inputUri.find("://") == std::string::npos) {
        std::error_code ec;
        std::filesystem::path path(cfg.inputUri);
        if (!path.is_absolute()) {
            path = std::filesystem::absolute(path, ec);
        }
        if (!ec) {
            GError* uriError = nullptr;
            gchar* uri = gst_filename_to_uri(path.string().c_str(), &uriError);
            if (uri) {
                std::string result(uri);
                g_free(uri);
                if (uriError) g_error_free(uriError);
                return result;
            }
            if (uriError) g_error_free(uriError);
        }
    }

    return cfg.inputUri;
}

void appendDecodeInput(std::vector<std::string>& args, const StreamConfig& cfg) {
    // TVStreammerSAT5 handles live IPTV/SRT/HTTP/HLS sources itself.  Do not enable
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

} // namespace tvs::protocols
