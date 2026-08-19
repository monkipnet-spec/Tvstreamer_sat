#pragma once

#include "ConfigManager.h"
#include "protocols/GstOutputProtocols.h"

#include <cstdint>
#include <string>
#include <vector>

namespace tvs::protocols::outputs {

void addQueue(std::vector<std::string>& args, const std::string& name, uint64_t maxTimeNs = 3000000000ULL, bool leakyDownstream = false);
std::string safeHost(const std::string& host, const std::string& fallback);
std::string listenerBindHost(const StreamConfig& cfg);
void appendMpegTsMux(std::vector<std::string>& args, const StreamConfig& cfg);
void appendTsSmoother(std::vector<std::string>& args, const std::string& name, uint32_t smoothingLatencyUs = 200000);
void appendCbrPacer(std::vector<std::string>& args, const StreamConfig& cfg, const std::string& name);
void appendNetworkCbrPacer(std::vector<std::string>& args, const StreamConfig& cfg, const std::string& name);
void appendPostMuxAvReservoir(
    std::vector<std::string>& args,
    const std::string& name,
    uint64_t delayNs = 1500000000ULL,
    uint64_t queueMaxNs = 6000000000ULL);
void appendOutputQueue(std::vector<std::string>& args, const std::string& name, bool leakyDownstream = false);
void appendOutputQueueWithTime(std::vector<std::string>& args, const std::string& name, uint64_t maxTimeNs, bool leakyDownstream = false);
void assignTsPads(const StreamConfig& cfg, GstOutputSpec& spec);

} // namespace tvs::protocols::outputs
