#pragma once

#include "ConfigManager.h"

#include <string>
#include <vector>

namespace tvs::stream_protocols {

enum class InputProtocolKind {
    TestPattern,
    Satellite,
    Udp,
    Rtp,
    Http,
    Hls,
    Srt,
    Rtsp,
    Rtmp,
    File,
    Unknown
};

InputProtocolKind inputKind(const StreamConfig& cfg);
std::string inputKindName(InputProtocolKind kind);
std::vector<const char*> requiredElementsForInput(InputProtocolKind kind);

bool isTestPatternInput(InputProtocolKind kind);
bool isUdpLikeInput(InputProtocolKind kind);
bool isHttpLikeInput(InputProtocolKind kind);
bool isFileInput(InputProtocolKind kind);

} // namespace tvs::stream_protocols
