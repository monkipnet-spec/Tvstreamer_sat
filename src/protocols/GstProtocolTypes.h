#pragma once

#include "ConfigManager.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tvs::protocols {

enum class OutputKind {
    UdpCbr,
    UdpVbr,
    Rtp,
    Srt,
    Http,
    Hls,
    Rtsp,
    Rtmp,
    Youtube,
    FifoRelay,
    Unknown
};

enum class ContainerKind {
    MpegTs,
    Flv,
    Rtsp
};

std::string normalizedOutputType(const StreamConfig& cfg);
OutputKind outputKind(const StreamConfig& cfg);
bool isUdpOutput(OutputKind kind);
bool isTsOutput(OutputKind kind);
bool isFlvOutput(OutputKind kind);
bool isRtspOutput(OutputKind kind);
bool transportCbrEnabled(const StreamConfig& cfg);

StreamOutputConfig primaryOutputConfig(const StreamConfig& cfg);
StreamConfig configForOutput(const StreamConfig& base, const StreamOutputConfig& output);
std::vector<StreamConfig> outputConfigs(const StreamConfig& cfg);

uint64_t safeVideoBitrate(const StreamConfig& cfg);
uint64_t safeAudioBitrate(const StreamConfig& cfg);
uint64_t muxBitrate(const StreamConfig& cfg);

std::string srtOutputMode(const StreamConfig& cfg);
std::string hlsDirectory(const StreamConfig& cfg);
std::string rtmpOutputLocation(const StreamConfig& cfg);
std::string rtspOutputLocation(const StreamConfig& cfg);
uint16_t transcodedHttpInternalPort(const StreamConfig& cfg);
uint16_t transcodedSrtInternalPort(const StreamConfig& cfg);
std::string transcodedFifoRelayPath(const StreamConfig& cfg);
bool prepareFifoRelay(const StreamConfig& cfg, std::string& error);
void removeFifoRelay(const StreamConfig& cfg);

} // namespace tvs::protocols
