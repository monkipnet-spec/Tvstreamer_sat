#pragma once

#include "ConfigManager.h"
#include <gst/gst.h>
#include <string>
#include <vector>

struct TranscoderCapabilities {
    bool available = false;
    std::string videoEncoder;
    bool x264Available = false;
    bool nvencAvailable = false;
    std::string audioEncoder;
    std::string aacEncoder;
    std::string mp3Encoder;
    bool deinterlaceAvailable = false;
    std::vector<std::string> missingElements;
    std::string message;
};

class TranscoderModule {
public:
    static TranscoderCapabilities inspectCapabilities();

    // Creates a completely isolated GstBin with one generic input ghost pad and one
    // MPEG-TS source ghost pad. The bin owns parsing, decoding, scaling, encoding,
    // optional original-audio passthrough, remuxing and ignored-pad draining.
    static GstElement* createBin(const StreamConfig& config, std::string& error);

    static bool resolutionSize(const std::string& resolution, int& width, int& height);
    static uint64_t recommendedVideoBitrate(const std::string& resolution);
};
