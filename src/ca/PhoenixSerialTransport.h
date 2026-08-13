#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ca {

struct PhoenixProbeProfile {
    unsigned baud = 9600;
    std::string label;
};

enum class PhoenixCardPresence {
    Unknown,
    Present,
    Absent
};

struct PhoenixSerialConfig {
    std::string devicePath;
    std::string serial;
    std::string detectMode = "cd";
    std::vector<PhoenixProbeProfile> probeProfiles;
};

struct PhoenixProbeResult {
    std::string status = "unknown";
    PhoenixCardPresence presence = PhoenixCardPresence::Unknown;
    std::vector<std::uint8_t> atr;
    unsigned baud = 0;
    std::string profileLabel;
    std::string detail;
};

// Low-level Phoenix/SmartMouse transport used only for reader lifecycle,
// card-detect, reset and ATR acquisition. It deliberately exposes no generic
// APDU/CW/descrambling interface.
class PhoenixSerialTransport {
public:
    PhoenixSerialTransport();
    ~PhoenixSerialTransport();

    PhoenixSerialTransport(const PhoenixSerialTransport&) = delete;
    PhoenixSerialTransport& operator=(const PhoenixSerialTransport&) = delete;

    bool open(const PhoenixSerialConfig& config, std::string* error = nullptr);
    void close();
    bool reconnect(std::string* error = nullptr);
    bool isOpen() const;

    PhoenixCardPresence cardPresence() const;
    PhoenixProbeResult resetAndReadAtr(std::chrono::milliseconds perResetTimeout = std::chrono::milliseconds(750));

    const PhoenixSerialConfig& config() const;

    static PhoenixProbeResult probe(const PhoenixSerialConfig& config,
                                    std::chrono::milliseconds perResetTimeout = std::chrono::milliseconds(750));
    static std::vector<PhoenixProbeProfile> defaultProbeProfiles(const std::string& serial);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ca
