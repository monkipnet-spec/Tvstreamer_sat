#include "StableUdpOutput.h"
#include "TsCcStageTrace.h"

#include <gst/app/gstappsink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <time.h>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "utils.h"

namespace {

constexpr std::size_t kTsPacketSize = 188;
constexpr std::size_t kTsPacketsPerDatagram = 7;
constexpr std::size_t kUdpPayloadSize = kTsPacketSize * kTsPacketsPerDatagram;
constexpr std::size_t kMaxBufferedBytes = 32 * 1024 * 1024;
constexpr int kSocketBufferSize = 128 * 1024 * 1024;
constexpr int kMulticastTtl = 32;
constexpr uint64_t kStartupReservoirNanoseconds = 5000ULL * 1000ULL * 1000ULL;
constexpr uint64_t kStartupPcrGraceNanoseconds = 2000ULL * 1000ULL * 1000ULL;
constexpr std::size_t kStartupMinimumPcrSamples = 1;
constexpr uint64_t kAdaptiveLowWatermarkNanoseconds = 250ULL * 1000ULL * 1000ULL;
constexpr uint64_t kLateResetIntervals = 4ULL;
constexpr uint64_t kPcrClockHz = 27000000ULL;
constexpr uint64_t kPcrBaseModulus = (1ULL << 33);
constexpr uint64_t kPcrTicksModulus = kPcrBaseModulus * 300ULL;
constexpr uint64_t kPeriodicPcrIntervalNanoseconds = 20ULL * 1000ULL * 1000ULL;
constexpr uint64_t kStatsIntervalNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr uint64_t kTimestampBackwardToleranceNanoseconds = 100ULL * 1000ULL * 1000ULL;
constexpr uint64_t kTimestampForwardJumpNanoseconds = 5ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr uint64_t kVbrTransportHeadroomBitrate = 120000ULL;
constexpr uint64_t kMinimumVbrTransportBitrate = 500000ULL;
constexpr uint64_t kMaximumTransportBitrate = 200000000ULL;

enum class UdpShapingMode {
    Cbr,
    Vbr
};

UdpShapingMode udpShapingMode(const StreamConfig& cfg) {
    std::string type = cfg.outputType;
    std::transform(type.begin(), type.end(), type.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (type == "udp-vbr" || type == "udp_vbr" || type == "udpvbr") {
        return UdpShapingMode::Vbr;
    }
    if (type == "udp" && !cfg.cbr) {
        return UdpShapingMode::Vbr;
    }
    return UdpShapingMode::Cbr;
}

const char* shapingModeName(UdpShapingMode mode) {
    return mode == UdpShapingMode::Cbr ? "CBR" : "VBR";
}

bool isMulticastHost(const std::string& host) {
    static const std::regex pattern(R"(^((22[4-9])|(23[0-9]))\.)");
    return std::regex_search(host, pattern);
}

std::string interfaceAddressFor(const std::string& address) {
    for (const auto& iface : enumerateNetworkInterfaces()) {
        if (iface.name == address || iface.address == address) {
            return iface.address;
        }
    }
    return address;
}

bool hasProperty(GstElement* element, const char* propertyName) {
    return element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), propertyName) != nullptr;
}

void setUInt64PropertyIfPresent(GstElement* element, const char* propertyName, guint64 value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

uint64_t monotonicNanoseconds() {
    timespec now {};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(now.tv_nsec);
}

timespec toTimespec(uint64_t nanoseconds) {
    timespec value {};
    value.tv_sec = static_cast<time_t>(nanoseconds / 1000000000ULL);
    value.tv_nsec = static_cast<long>(nanoseconds % 1000000000ULL);
    return value;
}

void sleepUntilMonotonic(uint64_t deadlineNanoseconds) {
    const timespec deadline = toTimespec(deadlineNanoseconds);
    int result = 0;
    do {
        result = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr);
    } while (result == EINTR);
}

uint64_t multiplyDivide(uint64_t value, uint64_t multiplier, uint64_t divisor) {
    if (divisor == 0) {
        return 0;
    }
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 product =
        static_cast<unsigned __int128>(value) * static_cast<unsigned __int128>(multiplier);
    return static_cast<uint64_t>(product / divisor);
#else
    const long double product = static_cast<long double>(value) *
        static_cast<long double>(multiplier);
    return static_cast<uint64_t>(product / static_cast<long double>(divisor));
#endif
}

uint64_t nanosecondsToPcrTicks(uint64_t nanoseconds) {
    return multiplyDivide(nanoseconds, kPcrClockHz, 1000000000ULL);
}

struct TimedChunk {
    std::vector<guint8> bytes;
    uint64_t arrivalNanoseconds = 0;
    bool timestampValid = false;
    uint64_t mediaTimestampNanoseconds = 0;
};

struct TimedTsPacket {
    std::array<guint8, kTsPacketSize> bytes {};
    uint16_t pid = 0x1FFF;
    bool hasPcr = false;
    bool discontinuity = false;
    uint64_t sourcePcrTicks = 0;
    uint64_t dueNanoseconds = 0;
};

bool parsePcr(const std::array<guint8, kTsPacketSize>& packet,
              uint64_t& pcrTicks,
              bool& discontinuity) {
    pcrTicks = 0;
    discontinuity = false;
    if (packet[0] != 0x47) {
        return false;
    }

    const guint8 adaptationFieldControl = static_cast<guint8>((packet[3] >> 4) & 0x03);
    if (adaptationFieldControl != 2 && adaptationFieldControl != 3) {
        return false;
    }

    const std::size_t adaptationLength = packet[4];
    if (adaptationLength < 1 || 5 + adaptationLength > kTsPacketSize) {
        return false;
    }

    const guint8 flags = packet[5];
    discontinuity = (flags & 0x80) != 0;
    if ((flags & 0x10) == 0 || adaptationLength < 7) {
        return false;
    }

    const guint64 base =
        (static_cast<guint64>(packet[6]) << 25) |
        (static_cast<guint64>(packet[7]) << 17) |
        (static_cast<guint64>(packet[8]) << 9) |
        (static_cast<guint64>(packet[9]) << 1) |
        (static_cast<guint64>(packet[10]) >> 7);
    const guint64 extension =
        (static_cast<guint64>(packet[10] & 0x01) << 8) |
        static_cast<guint64>(packet[11]);
    pcrTicks = (base * 300ULL + extension) % kPcrTicksModulus;
    return true;
}

void writePcr(std::array<guint8, kTsPacketSize>& packet, uint64_t pcrTicks) {
    pcrTicks %= kPcrTicksModulus;
    const uint64_t base = pcrTicks / 300ULL;
    const uint64_t extension = pcrTicks % 300ULL;

    packet[6] = static_cast<guint8>((base >> 25) & 0xFF);
    packet[7] = static_cast<guint8>((base >> 17) & 0xFF);
    packet[8] = static_cast<guint8>((base >> 9) & 0xFF);
    packet[9] = static_cast<guint8>((base >> 1) & 0xFF);
    packet[10] = static_cast<guint8>(((base & 0x01) << 7) | 0x7E |
        ((extension >> 8) & 0x01));
    packet[11] = static_cast<guint8>(extension & 0xFF);
}

uint16_t packetPid(const std::array<guint8, kTsPacketSize>& packet) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(packet[1] & 0x1F) << 8) |
        static_cast<uint16_t>(packet[2]));
}

bool packetHasPayload(const std::array<guint8, kTsPacketSize>& packet) {
    const guint8 adaptationFieldControl =
        static_cast<guint8>((packet[3] >> 4) & 0x03);
    return adaptationFieldControl == 1 || adaptationFieldControl == 3;
}

void clearPcrFlag(std::array<guint8, kTsPacketSize>& packet) {
    if (packet[0] != 0x47) {
        return;
    }
    const guint8 adaptationFieldControl =
        static_cast<guint8>((packet[3] >> 4) & 0x03);
    if (adaptationFieldControl != 2 && adaptationFieldControl != 3) {
        return;
    }
    const std::size_t adaptationLength = packet[4];
    if (adaptationLength < 1 || 5 + adaptationLength > kTsPacketSize) {
        return;
    }
    // Keep the adaptation-field size unchanged; the old PCR bytes simply
    // become stuffing after the PCR flag is cleared.
    packet[5] = static_cast<guint8>(packet[5] & ~0x10U);
}

uint32_t mpeg2SectionCrc32(const guint8* data, std::size_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U)
                ? (crc << 1) ^ 0x04C11DB7U
                : (crc << 1);
        }
    }
    return crc;
}

std::vector<guint8> dvbUtf8Text(const std::string& value, std::size_t maxBytes) {
    std::vector<guint8> out;
    if (value.empty() || maxBytes < 2) return out;
    out.push_back(0x15); // DVB UTF-8 selector
    const std::size_t copy = std::min(maxBytes - 1, value.size());
    out.insert(out.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(copy));
    return out;
}

const guint8* tsPayloadStart(
    const std::array<guint8, kTsPacketSize>& packet,
    std::size_t& available,
    bool requirePusi = false) {
    available = 0;
    if (packet[0] != 0x47) return nullptr;
    if (requirePusi && (packet[1] & 0x40U) == 0) return nullptr;

    const guint8 adaptationControl = static_cast<guint8>((packet[3] >> 4) & 0x03);
    if (adaptationControl == 0 || adaptationControl == 2) return nullptr;
    std::size_t offset = 4;
    if (adaptationControl == 3) {
        const std::size_t adaptationLength = packet[4];
        if (5 + adaptationLength > kTsPacketSize) return nullptr;
        offset = 5 + adaptationLength;
    }
    if (offset >= kTsPacketSize) return nullptr;

    if (packet[1] & 0x40U) {
        const std::size_t pointer = packet[offset];
        offset += 1 + pointer;
        if (offset >= kTsPacketSize) return nullptr;
    } else if (requirePusi) {
        return nullptr;
    }

    available = kTsPacketSize - offset;
    return packet.data() + offset;
}

void writeRemappedSdtPacket(
    std::array<guint8, kTsPacketSize>& packet,
    uint16_t serviceId,
    const std::string& serviceName,
    const std::string& serviceProvider,
    uint16_t transportStreamId,
    uint16_t originalNetworkId,
    guint8 version) {
    if (serviceId == 0) return;

    const guint8 continuity = static_cast<guint8>(packet[3] & 0x0F);
    packet.fill(0xFF);
    packet[0] = 0x47;
    packet[1] = 0x40; // PUSI + PID 0x0011
    packet[2] = 0x11;
    packet[3] = static_cast<guint8>(0x10 | continuity);
    packet[4] = 0x00; // pointer_field

    const std::string nameText = serviceName.empty()
        ? ("Service " + std::to_string(serviceId))
        : serviceName;
    auto provider = dvbUtf8Text(serviceProvider, 48);
    auto name = dvbUtf8Text(nameText, 80);
    while (provider.size() + name.size() > 140) {
        if (name.size() > 2) name.pop_back();
        else if (provider.size() > 2) provider.pop_back();
        else break;
    }

    const std::size_t descriptorPayloadLength = 3 + provider.size() + name.size();
    const std::size_t descriptorTotalLength = 2 + descriptorPayloadLength;
    const uint16_t sectionLength =
        static_cast<uint16_t>(8 + 5 + descriptorTotalLength + 4);

    guint8* section = packet.data() + 5;
    section[0] = 0x42;
    section[1] = static_cast<guint8>(0xF0 | ((sectionLength >> 8) & 0x0F));
    section[2] = static_cast<guint8>(sectionLength & 0xFF);
    section[3] = static_cast<guint8>(transportStreamId >> 8);
    section[4] = static_cast<guint8>(transportStreamId & 0xFF);
    section[5] = static_cast<guint8>(0xC1 | ((version & 0x1F) << 1));
    section[6] = 0x00;
    section[7] = 0x00;
    section[8] = static_cast<guint8>(originalNetworkId >> 8);
    section[9] = static_cast<guint8>(originalNetworkId & 0xFF);
    section[10] = 0xFF;

    std::size_t pos = 11;
    section[pos++] = static_cast<guint8>(serviceId >> 8);
    section[pos++] = static_cast<guint8>(serviceId & 0xFF);
    section[pos++] = 0xFC;
    const uint16_t loopLength = static_cast<uint16_t>(descriptorTotalLength);
    section[pos++] = static_cast<guint8>(0x80 | ((loopLength >> 8) & 0x0F));
    section[pos++] = static_cast<guint8>(loopLength & 0xFF);
    section[pos++] = 0x48;
    section[pos++] = static_cast<guint8>(descriptorPayloadLength);
    section[pos++] = 0x01;
    section[pos++] = static_cast<guint8>(provider.size());
    for (guint8 byte : provider) section[pos++] = byte;
    section[pos++] = static_cast<guint8>(name.size());
    for (guint8 byte : name) section[pos++] = byte;

    const uint32_t crc = mpeg2SectionCrc32(section, pos);
    section[pos++] = static_cast<guint8>((crc >> 24) & 0xFF);
    section[pos++] = static_cast<guint8>((crc >> 16) & 0xFF);
    section[pos++] = static_cast<guint8>((crc >> 8) & 0xFF);
    section[pos++] = static_cast<guint8>(crc & 0xFF);
}

class StableUdpSender {
public:
    StableUdpSender(const StreamConfig& cfg, std::string& error, std::atomic<uint64_t>* networkBytesCounter)
        : networkBytes(networkBytesCounter),
          preSendCcTrace(cfg.id, "PRE_SEND"),
          mode(udpShapingMode(cfg)), configuredTargetBitrate(cfg.targetBitrate),
          // StableUdpOutput creates a new paced UDP transport domain for every
          // source, not only Remap ON. Normalize final CC for both IP and DVB
          // inputs so periodic PCR insertion, reservoir pacing and upstream
          // buffer boundaries cannot expose broken continuity at the receiver.
          normalizeOutputContinuity(true),
          remapPsiNormalization(cfg.remapEnabled),
          remapOutputServiceId(static_cast<uint16_t>(
              (cfg.serviceId ? cfg.serviceId : cfg.inputServiceId) & 0xFFFFU)),
          expectedProgramId(static_cast<uint16_t>(
              ((cfg.remapEnabled
                    ? (cfg.serviceId ? cfg.serviceId : cfg.inputServiceId)
                    : (cfg.inputServiceId ? cfg.inputServiceId : cfg.serviceId))) & 0xFFFFU)),
          remapServiceName(cfg.serviceName.empty() ? cfg.name : cfg.serviceName),
          remapServiceProvider(cfg.serviceProvider) {
        if (mode == UdpShapingMode::Cbr && configuredTargetBitrate == 0) {
            error = "UDP CBR target_bitrate must be greater than zero";
            return;
        }

        const uint64_t initialTransportBitrate = mode == UdpShapingMode::Cbr
            ? configuredTargetBitrate
            : std::max<uint64_t>(kMinimumVbrTransportBitrate,
                configuredTargetBitrate > 0 ? configuredTargetBitrate : 1000000ULL);
        if (initialTransportBitrate == 0 || initialTransportBitrate > kMaximumTransportBitrate) {
            error = "UDP transport bitrate is outside the supported range";
            return;
        }
        transportBitrate.store(initialTransportBitrate, std::memory_order_relaxed);

        socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socketFd < 0) {
            error = std::string("failed to create UDP socket: ") + std::strerror(errno);
            return;
        }

        int sendBufferSize = kSocketBufferSize;
        ::setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, sizeof(sendBufferSize));

        const std::string outputHost = cfg.outputHost.empty() ? "127.0.0.1" : cfg.outputHost;
        destinationAddress.sin_family = AF_INET;
        destinationAddress.sin_port = htons(static_cast<uint16_t>(cfg.outputPort));
        if (::inet_pton(AF_INET, outputHost.c_str(), &destinationAddress.sin_addr) != 1) {
            error = "invalid UDP output host: " + outputHost;
            closeSocket();
            return;
        }

        const bool multicastOutput = isMulticastHost(outputHost);
        if (!cfg.interfaceAddress.empty()) {
            const std::string ifaceAddress = interfaceAddressFor(cfg.interfaceAddress);
            in_addr localAddress {};
            if (::inet_pton(AF_INET, ifaceAddress.c_str(), &localAddress) != 1) {
                error = "invalid UDP interface address: " + cfg.interfaceAddress;
                closeSocket();
                return;
            }

            if (multicastOutput) {
                if (::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_IF,
                        &localAddress, sizeof(localAddress)) != 0) {
                    error = std::string("failed to set UDP multicast interface: ") +
                        std::strerror(errno);
                    closeSocket();
                    return;
                }
            } else {
                sockaddr_in bindAddress {};
                bindAddress.sin_family = AF_INET;
                bindAddress.sin_port = 0;
                bindAddress.sin_addr = localAddress;
                if (::bind(socketFd, reinterpret_cast<sockaddr*>(&bindAddress),
                        sizeof(bindAddress)) != 0) {
                    error = std::string("failed to bind UDP interface: ") +
                        std::strerror(errno);
                    closeSocket();
                    return;
                }
            }
        }

        if (multicastOutput) {
            unsigned char ttl = kMulticastTtl;
            ::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        }

        ready = true;
        senderThread = std::thread(&StableUdpSender::sendLoop, this);
    }

    ~StableUdpSender() {
        stopping.store(true, std::memory_order_relaxed);
        queueReady.notify_all();
        queueSpace.notify_all();
        if (senderThread.joinable()) {
            senderThread.join();
        }
        closeSocket();
    }

    bool isReady() const {
        return ready;
    }

    GstFlowReturn pushBuffer(GstBuffer* buffer) {
        if (!ready || !buffer) {
            return GST_FLOW_ERROR;
        }

        GstMapInfo map {};
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            return GST_FLOW_ERROR;
        }

        TimedChunk chunk;
        chunk.bytes.assign(map.data, map.data + map.size);
        gst_buffer_unmap(buffer, &map);
        if (chunk.bytes.empty()) {
            return GST_FLOW_OK;
        }

        GstClockTime timestamp = GST_BUFFER_PTS(buffer);
        if (!GST_CLOCK_TIME_IS_VALID(timestamp)) {
            timestamp = GST_BUFFER_DTS(buffer);
        }
        if (GST_CLOCK_TIME_IS_VALID(timestamp)) {
            chunk.timestampValid = true;
            chunk.mediaTimestampNanoseconds = static_cast<uint64_t>(timestamp);
            ++validTimestampChunks;
        } else {
            ++missingTimestampChunks;
        }

        chunk.arrivalNanoseconds = monotonicNanoseconds();
        inputBytesReceived.fetch_add(chunk.bytes.size(), std::memory_order_relaxed);

        std::unique_lock<std::mutex> lock(queueMutex);
        queueSpace.wait(lock, [&]() {
            return stopping.load(std::memory_order_relaxed) ||
                   bufferedBytes.load(std::memory_order_relaxed) + chunk.bytes.size() <= kMaxBufferedBytes;
        });
        if (stopping.load(std::memory_order_relaxed)) {
            return GST_FLOW_FLUSHING;
        }

        bufferedBytes.fetch_add(chunk.bytes.size(), std::memory_order_relaxed);
        if (firstChunkArrivalNanoseconds == 0) {
            firstChunkArrivalNanoseconds = chunk.arrivalNanoseconds;
        }
        queuedChunks.push_back(std::move(chunk));
        lock.unlock();
        queueReady.notify_one();
        return GST_FLOW_OK;
    }

private:
    static constexpr uint64_t kRateSampleNanoseconds = 500ULL * 1000ULL * 1000ULL;
    static constexpr uint64_t kControllerUpdateNanoseconds = 100ULL * 1000ULL * 1000ULL;
    static constexpr uint64_t kTargetReservoirNanoseconds = 2500ULL * 1000ULL * 1000ULL;
    static constexpr uint64_t kLowReservoirNanoseconds = 800ULL * 1000ULL * 1000ULL;
    static constexpr uint64_t kCorrectionHorizonNanoseconds = 6ULL * 1000ULL * 1000ULL * 1000ULL;

    void sendLoop() {
        if (mode == UdpShapingMode::Vbr) {
            sendVbrPcrBlockLoop();
            return;
        }

        uint64_t nextSendNanoseconds = 0;
        // Media/PCR time is deliberately independent from the physical sender
        // deadline. If the sender thread wakes up late we may move the wall-clock
        // deadline forward, but we must never jump PCR with it: upstream PTS/DTS
        // did not jump. Keeping a continuous transport timeline prevents A/V
        // drift when several output threads contend for CPU scheduling.
        uint64_t mediaTimelineNanoseconds = 0;
        uint64_t scheduleRemainder = 0;
        uint64_t mediaRemainder = 0;
        uint64_t scheduleBitrate = 0;

        while (!stopping.load(std::memory_order_relaxed)) {
            if (nextSendNanoseconds == 0) {
                if (!waitForInitialPackets()) {
                    break;
                }
                nextSendNanoseconds = monotonicNanoseconds();
                mediaTimelineNanoseconds = nextSendNanoseconds;
                statsStartedNanoseconds = nextSendNanoseconds;
                lastStatsNanoseconds = nextSendNanoseconds;
                lastRateSampleNanoseconds = nextSendNanoseconds;
                lastRateSampleBytes = inputBytesReceived.load(std::memory_order_relaxed);
                lastControllerUpdateNanoseconds = nextSendNanoseconds;
            }

            sleepUntilMonotonic(nextSendNanoseconds);
            if (stopping.load(std::memory_order_relaxed)) {
                break;
            }

            const uint64_t now = monotonicNanoseconds();
            moveAvailableChunks();
            updateRateController(now);

            uint64_t activeBitrate = transportBitrate.load(std::memory_order_relaxed);
            activeBitrate = std::clamp<uint64_t>(
                activeBitrate, kMinimumVbrTransportBitrate, kMaximumTransportBitrate);
            if (mode == UdpShapingMode::Cbr) {
                activeBitrate = configuredTargetBitrate;
            }
            if (scheduleBitrate != activeBitrate) {
                scheduleBitrate = activeBitrate;
                scheduleRemainder = 0;
                mediaRemainder = 0;
            }

            const uint64_t datagramNumerator = kUdpPayloadSize * 8ULL * 1000000000ULL;
            const uint64_t intervalNanoseconds = datagramNumerator / activeBitrate;
            const uint64_t intervalRemainder = datagramNumerator % activeBitrate;
            if (intervalNanoseconds == 0) {
                std::cerr << "UDP shaper transport bitrate is too high: "
                          << activeBitrate << std::endl;
                break;
            }

            const uint64_t lateResetThreshold = intervalNanoseconds * kLateResetIntervals;
            if (now > nextSendNanoseconds && now - nextSendNanoseconds > lateResetThreshold) {
                const uint64_t shiftNanoseconds = now - nextSendNanoseconds;
                nextSendNanoseconds = now;
                scheduleRemainder = 0;
                schedulerTimelineShiftNanoseconds.fetch_add(
                    shiftNanoseconds, std::memory_order_relaxed);
                ++schedulerResets;
            }

            std::array<guint8, kUdpPayloadSize> datagram {};
            const FillCounts filled = fillDatagram(
                datagram.data(), mediaTimelineNanoseconds, activeBitrate);
            normalizeFinalDatagramContinuity(datagram.data());
            verifyFinalDatagramContinuity(datagram.data());
            sendDatagram(datagram.data(), datagram.size());
            totalDatagrams.fetch_add(1, std::memory_order_relaxed);
            totalRealPackets.fetch_add(filled.real, std::memory_order_relaxed);
            const std::size_t occupied = filled.real + filled.periodicPcr;
            totalNullPackets.fetch_add(
                occupied < kTsPacketsPerDatagram ? kTsPacketsPerDatagram - occupied : 0,
                std::memory_order_relaxed);
            if (filled.real > 0) {
                queueSpace.notify_all();
            }

            maybeLogStats(now);

            nextSendNanoseconds += intervalNanoseconds;
            mediaTimelineNanoseconds += intervalNanoseconds;

            scheduleRemainder += intervalRemainder;
            if (scheduleRemainder >= activeBitrate) {
                nextSendNanoseconds += scheduleRemainder / activeBitrate;
                scheduleRemainder %= activeBitrate;
            }

            // Keep PCR/media time continuous even if nextSendNanoseconds was
            // reset to the current wall clock above. It advances only by the
            // transport duration actually represented by this datagram.
            mediaRemainder += intervalRemainder;
            if (mediaRemainder >= activeBitrate) {
                mediaTimelineNanoseconds += mediaRemainder / activeBitrate;
                mediaRemainder %= activeBitrate;
            }
        }
    }

    static uint64_t pcrDeltaTicks(uint64_t previous, uint64_t current) {
        if (current >= previous) {
            return current - previous;
        }
        return (kPcrTicksModulus - previous) + current;
    }

    bool selectedSourcePcr(const TimedTsPacket& packet) const {
        return packet.hasPcr && declaredPcrPidValid && packet.pid == declaredPcrPid;
    }

    // Astra-style VBR pacing: preserve broadcaster PCR/PTS/DTS verbatim and use
    // consecutive source PCR samples only as the pacing clock. Packets between
    // PCR[n] and PCR[n+1] are emitted uniformly over exactly PCR[n+1]-PCR[n].
    // This makes each service follow its own broadcaster clock and removes any
    // dependency on EWMA bitrate estimates or competing sender-thread wakeups.
    void sendVbrPcrBlockLoop() {
        if (!waitForInitialPackets()) {
            return;
        }

        uint64_t nextPacketDeadline = monotonicNanoseconds();
        statsStartedNanoseconds = nextPacketDeadline;
        lastStatsNanoseconds = nextPacketDeadline;
        lastRateSampleNanoseconds = nextPacketDeadline;
        lastRateSampleBytes = inputBytesReceived.load(std::memory_order_relaxed);
        lastControllerUpdateNanoseconds = nextPacketDeadline;

        std::array<guint8, kUdpPayloadSize> datagram {};
        std::size_t datagramPackets = 0;
        uint64_t pacingRemainder = 0;
        uint64_t blockCount = 0;
        uint64_t invalidBlocks = 0;

        std::cerr << "UDP VBR pacing: mode=astra-source-pcr-block"
                  << " pcr_passthrough=1 pts_dts_passthrough=1"
                  << " datagram_packets=" << kTsPacketsPerDatagram << std::endl;

        while (!stopping.load(std::memory_order_relaxed)) {
            moveAvailableChunks();

            // Find the first selected-service PCR and discard only packets that
            // precede it. The startup reservoir keeps enough history for this to
            // happen once, just as Astra starts its sync buffer at the first PCR.
            while (!realPackets.empty() && !selectedSourcePcr(realPackets.front())) {
                bufferedBytes.fetch_sub(kTsPacketSize, std::memory_order_relaxed);
                realPackets.pop_front();
                queueSpace.notify_all();
            }

            if (realPackets.empty()) {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueReady.wait_for(lock, std::chrono::milliseconds(2));
                continue;
            }

            // Need the next PCR to know the exact duration of this block.
            std::size_t nextPcrIndex = 0;
            bool haveNextPcr = false;
            for (std::size_t i = 1; i < realPackets.size(); ++i) {
                if (selectedSourcePcr(realPackets[i])) {
                    nextPcrIndex = i;
                    haveNextPcr = true;
                    break;
                }
            }
            if (!haveNextPcr) {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueReady.wait_for(lock, std::chrono::milliseconds(2));
                continue;
            }

            const uint64_t firstPcr = realPackets.front().sourcePcrTicks;
            const uint64_t nextPcr = realPackets[nextPcrIndex].sourcePcrTicks;
            if (!periodicPcrInitialized) {
                periodicPcrInitialized = true;
                periodicPcrPid = declaredPcrPid;
                periodicPcrOriginTicks = firstPcr;
                std::cerr << "UDP PCR lock: program=" << declaredPcrProgram
                          << " pcr_pid=" << periodicPcrPid
                          << " source=selected-PMT mode=astra-source-pcr-block"
                          << std::endl;
            }
            const uint64_t deltaTicks = pcrDeltaTicks(firstPcr, nextPcr);
            const uint64_t blockNanoseconds = multiplyDivide(
                deltaTicks, 1000000000ULL, kPcrClockHz);

            // Match Astra's safety policy: an impossible/very large PCR interval
            // is not used to invent a replacement clock. Resync at the next PCR.
            if (blockNanoseconds == 0 || blockNanoseconds > 500ULL * 1000ULL * 1000ULL) {
                ++invalidBlocks;
                for (std::size_t i = 0; i < nextPcrIndex; ++i) {
                    bufferedBytes.fetch_sub(kTsPacketSize, std::memory_order_relaxed);
                    realPackets.pop_front();
                }
                nextPacketDeadline = monotonicNanoseconds();
                pacingRemainder = 0;
                queueSpace.notify_all();
                std::cerr << "UDP VBR PCR block resync: delta_ns=" << blockNanoseconds
                          << " packets=" << nextPcrIndex
                          << " invalid_blocks=" << invalidBlocks << std::endl;
                continue;
            }

            const std::size_t packetsInBlock = nextPcrIndex;
            if (packetsInBlock == 0) {
                continue;
            }

            const uint64_t intervalNanoseconds = blockNanoseconds / packetsInBlock;
            const uint64_t intervalRemainder = blockNanoseconds % packetsInBlock;
            pacingRemainder = 0;
            const uint64_t measuredBitrate = multiplyDivide(
                packetsInBlock * kTsPacketSize * 8ULL, 1000000000ULL, blockNanoseconds);
            if (measuredBitrate > 0) {
                estimatedInputBitrate = measuredBitrate;
                inputBitrateEstimate.store(measuredBitrate, std::memory_order_relaxed);
                realPaceBitrate.store(measuredBitrate, std::memory_order_relaxed);
                transportBitrate.store(measuredBitrate, std::memory_order_relaxed);
            }

            ++blockCount;
            for (std::size_t i = 0; i < packetsInBlock &&
                 !stopping.load(std::memory_order_relaxed); ++i) {
                sleepUntilMonotonic(nextPacketDeadline);

                TimedTsPacket packet = std::move(realPackets.front());
                realPackets.pop_front();
                bufferedBytes.fetch_sub(kTsPacketSize, std::memory_order_relaxed);

                // Source PCR, PTS and DTS are copied byte-for-byte.
                observePcrPidContinuity(packet);
                std::copy(packet.bytes.begin(), packet.bytes.end(),
                          datagram.begin() + datagramPackets * kTsPacketSize);
                ++datagramPackets;
                totalRealPackets.fetch_add(1, std::memory_order_relaxed);

                if (datagramPackets == kTsPacketsPerDatagram) {
                    normalizeFinalDatagramContinuity(datagram.data());
                    verifyFinalDatagramContinuity(datagram.data());
                    sendDatagram(datagram.data(), datagram.size());
                    totalDatagrams.fetch_add(1, std::memory_order_relaxed);
                    datagramPackets = 0;
                    queueSpace.notify_all();
                }

                nextPacketDeadline += intervalNanoseconds;
                pacingRemainder += intervalRemainder;
                if (pacingRemainder >= packetsInBlock) {
                    nextPacketDeadline += pacingRemainder / packetsInBlock;
                    pacingRemainder %= packetsInBlock;
                }
            }

            const uint64_t now = monotonicNanoseconds();
            const uint64_t bufferNow = bufferedBytes.load(std::memory_order_relaxed);
            const uint64_t bufferMs = measuredBitrate > 0
                ? multiplyDivide(bufferNow * 8ULL, 1000ULL, measuredBitrate) : 0;
            reservoirMilliseconds.store(bufferMs, std::memory_order_relaxed);
            maybeLogStats(now);

            // Do not chase a large scheduling delay by changing PCR/media time.
            // Re-anchor only the physical deadline; the next block duration still
            // comes exclusively from the next pair of source PCR values.
            if (now > nextPacketDeadline + 100ULL * 1000ULL * 1000ULL) {
                schedulerTimelineShiftNanoseconds.fetch_add(
                    now - nextPacketDeadline, std::memory_order_relaxed);
                ++schedulerResets;
                nextPacketDeadline = now;
                pacingRemainder = 0;
            }
        }

        // Normally datagrams are 7 TS packets. Flush a short final datagram only
        // during shutdown so no payload is silently lost.
        if (datagramPackets > 0) {
            const std::size_t bytes = datagramPackets * kTsPacketSize;
            preSendCcTrace.inspect(datagram.data(), bytes);
            sendDatagram(datagram.data(), bytes);
        }
    }

    bool waitForInitialPackets() {
        while (!stopping.load(std::memory_order_relaxed)) {
            uint64_t firstArrival = 0;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                if (firstChunkArrivalNanoseconds == 0 || queuedChunks.empty()) {
                    queueReady.wait_for(lock, std::chrono::milliseconds(10), [&]() {
                        return stopping.load(std::memory_order_relaxed) ||
                               (!queuedChunks.empty() && firstChunkArrivalNanoseconds != 0);
                    });
                }
                if (stopping.load(std::memory_order_relaxed)) {
                    return false;
                }
                firstArrival = firstChunkArrivalNanoseconds;
            }

            const uint64_t now = monotonicNanoseconds();
            const uint64_t startAt = firstArrival + kStartupReservoirNanoseconds;
            if (now < startAt) {
                sleepUntilMonotonic(std::min<uint64_t>(
                    startAt, now + 20ULL * 1000ULL * 1000ULL));
                continue;
            }

            moveAvailableChunks();

            // The 5-second startup reservoir is intentional and required by
            // WISI equipment.  The old code additionally required five PCR
            // packets before it would send the first UDP datagram.  That could
            // deadlock forever when a mux produced PCR less frequently (or a
            // test source had not produced five PCR samples yet).  One PCR is
            // sufficient to lock the periodic 20 ms output PCR generator.
            std::size_t startupPcrPackets = 0;
            for (const auto& packet : realPackets) {
                if (packet.hasPcr &&
                    (!declaredPcrPidValid || packet.pid == declaredPcrPid)) {
                    ++startupPcrPackets;
                }
            }

            const bool haveTransport = !realPackets.empty();
            const bool havePcrLock = startupPcrPackets >= kStartupMinimumPcrSamples;
            const bool pcrGraceExpired = now >= startAt + kStartupPcrGraceNanoseconds;

            if (haveTransport && (havePcrLock || pcrGraceExpired)) {
                const uint64_t startupBytes = bufferedBytes.load(std::memory_order_relaxed);
                startupReservoirBytes.store(startupBytes, std::memory_order_relaxed);
                startupPcrSamples.store(startupPcrPackets, std::memory_order_relaxed);
                const uint64_t elapsed = std::max<uint64_t>(1ULL, now - firstArrival);
                estimatedInputBitrate = multiplyDivide(
                    startupBytes * 8ULL, 1000000000ULL, elapsed);
                if (estimatedInputBitrate == 0) {
                    estimatedInputBitrate = mode == UdpShapingMode::Cbr
                        ? std::min<uint64_t>(configuredTargetBitrate, 1000000ULL)
                        : 1000000ULL;
                }
                inputBitrateEstimate.store(estimatedInputBitrate, std::memory_order_relaxed);
                currentRealPaceBitrate = mode == UdpShapingMode::Cbr
                    ? std::min<uint64_t>(estimatedInputBitrate, maxRealPaceBitrate())
                    : estimatedInputBitrate;
                realPaceBitrate.store(currentRealPaceBitrate, std::memory_order_relaxed);
                updateTransportBitrate();

                if (havePcrLock) {
                    std::cerr << "UDP WISI startup: 5s reservoir ready, PCR lock acquired"
                              << " samples=" << startupPcrPackets
                              << " buffered=" << startupBytes << "B" << std::endl;
                } else {
                    std::cerr << "UDP WISI startup WARNING: 5s reservoir ready but no PCR"
                              << " after additional 2s grace; starting TS to avoid deadlock"
                              << " buffered=" << startupBytes << "B" << std::endl;
                }
                return true;
            }

            sleepUntilMonotonic(now + 1000000ULL);
        }
        return false;
    }

    void moveAvailableChunks() {
        std::deque<TimedChunk> available;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            while (!queuedChunks.empty()) {
                available.push_back(std::move(queuedChunks.front()));
                queuedChunks.pop_front();
            }
        }

        while (!available.empty()) {
            queueChunk(std::move(available.front()));
            available.pop_front();
        }
    }

    void queueChunk(TimedChunk chunk) {
        if (chunk.bytes.empty() && inputRemainder.empty()) {
            return;
        }

        // Gst/UDP buffers are usually 7x188, but remap/probe/queue boundaries
        // are not guaranteed to preserve a whole TS packet in every GstBuffer.
        // Keep a short tail and join it with the next chunk instead of dropping
        // it. Dropping that tail created real packet loss and analyzer CC errors
        // on PAT/CAT/SDT/PMT/audio/video even after counter normalization.
        std::vector<guint8> bytes;
        bytes.reserve(inputRemainder.size() + chunk.bytes.size());
        bytes.insert(bytes.end(), inputRemainder.begin(), inputRemainder.end());
        bytes.insert(bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
        inputRemainder.clear();

        if (bytes.empty()) return;

        std::size_t offset = 0;
        while (offset < bytes.size() && bytes[offset] != 0x47) {
            ++offset;
        }
        if (offset > 0) {
            bufferedBytes.fetch_sub(offset, std::memory_order_relaxed);
            resyncDiscardedBytes.fetch_add(offset, std::memory_order_relaxed);
            queueSpace.notify_all();
        }

        while (offset + kTsPacketSize <= bytes.size()) {
            if (bytes[offset] != 0x47) {
                ++offset;
                bufferedBytes.fetch_sub(1, std::memory_order_relaxed);
                ++resyncDiscardedBytes;
                queueSpace.notify_all();
                continue;
            }

            // If another full packet is available, validate its sync byte.
            // When only one full packet plus a partial tail remains, keep the
            // complete packet and carry the tail into the next GstBuffer.
            if (offset + kTsPacketSize * 2 <= bytes.size() &&
                bytes[offset + kTsPacketSize] != 0x47) {
                ++offset;
                bufferedBytes.fetch_sub(1, std::memory_order_relaxed);
                ++resyncDiscardedBytes;
                queueSpace.notify_all();
                continue;
            }

            TimedTsPacket packet;
            std::copy_n(bytes.data() + offset, kTsPacketSize, packet.bytes.data());
            normalizeRemappedPsi(packet.bytes);
            observeDeclaredPcrFromPmt(packet.bytes);
            packet.pid = packetPid(packet.bytes);
            packet.hasPcr = parsePcr(packet.bytes, packet.sourcePcrTicks, packet.discontinuity);
            realPackets.push_back(std::move(packet));
            offset += kTsPacketSize;
        }

        if (offset < bytes.size()) {
            inputRemainder.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
            if (inputRemainder.size() >= kTsPacketSize) {
                // Defensive bound: a valid remainder must be shorter than one
                // TS packet. Resync rather than allowing an unbounded tail.
                const std::size_t discard = inputRemainder.size() - (kTsPacketSize - 1);
                inputRemainder.erase(inputRemainder.begin(),
                    inputRemainder.begin() + static_cast<std::ptrdiff_t>(discard));
                bufferedBytes.fetch_sub(discard, std::memory_order_relaxed);
                resyncDiscardedBytes.fetch_add(discard, std::memory_order_relaxed);
                queueSpace.notify_all();
            }
        }
    }

    void normalizeRemappedPsi(std::array<guint8, kTsPacketSize>& packet) {
        if (!remapPsiNormalization || remapOutputServiceId == 0 ||
            packet[0] != 0x47 || packetPid(packet) != 0x0011) {
            return;
        }

        // Generic IP remap passes through mpegtsmux, which regenerates a clean
        // SDT. DVB packet-level remap cannot use tsdemux/mpegtsmux safely for
        // scrambled/private streams, so reproduce that final normalization here
        // after TS packet reassembly and before the WISI reservoir.
        std::size_t available = 0;
        const guint8* section = tsPayloadStart(packet, available, true);
        if (section && available >= 11 &&
            (section[0] == 0x42 || section[0] == 0x46)) {
            sdtTransportStreamId =
                static_cast<uint16_t>((section[3] << 8) | section[4]);
            sdtVersion = static_cast<guint8>((section[5] >> 1) & 0x1F);
            sdtOriginalNetworkId =
                static_cast<uint16_t>((section[8] << 8) | section[9]);
        }

        writeRemappedSdtPacket(
            packet,
            remapOutputServiceId,
            remapServiceName,
            remapServiceProvider,
            sdtTransportStreamId,
            sdtOriginalNetworkId,
            sdtVersion);
        ++finalSdtRewrites;

        if (!finalSdtAnnounced) {
            std::cerr << "UDP remap PSI normalizer: SID=" << remapOutputServiceId
                      << " service=\"" << remapServiceName << "\""
                      << " provider=\"" << remapServiceProvider << "\""
                      << " SDT=regenerated profile=ip-remux-equivalent"
                      << std::endl;
            finalSdtAnnounced = true;
        }
    }

    void observeDeclaredPcrFromPmt(const std::array<guint8, kTsPacketSize>& packet) {
        if (packet[0] != 0x47) return;

        std::size_t available = 0;
        const guint8* section = tsPayloadStart(packet, available, true);
        if (!section || available < 12 || section[0] != 0x02) return; // PMT

        const uint16_t sectionLength = static_cast<uint16_t>(
            ((section[1] & 0x0F) << 8) | section[2]);
        const std::size_t totalSectionBytes = static_cast<std::size_t>(sectionLength) + 3U;
        if (sectionLength < 9 || totalSectionBytes > available) {
            // The service PMTs used by the DVB relay normally fit in one TS
            // packet. Do not guess from a truncated section; a later repeated
            // PMT will be observed again.
            return;
        }

        const uint16_t programNumber = static_cast<uint16_t>(
            (static_cast<uint16_t>(section[3]) << 8) | section[4]);
        if (expectedProgramId != 0 && programNumber != expectedProgramId) return;

        const uint16_t pcrPid = static_cast<uint16_t>(
            (static_cast<uint16_t>(section[8] & 0x1F) << 8) | section[9]);
        if (pcrPid >= 0x1FFF) return;

        if (!declaredPcrPidValid || declaredPcrPid != pcrPid || declaredPcrProgram != programNumber) {
            declaredPcrPid = pcrPid;
            declaredPcrProgram = programNumber;
            declaredPcrPidValid = true;
            std::cerr << "UDP PCR selector: program=" << declaredPcrProgram
                      << " declared_pcr_pid=" << declaredPcrPid
                      << " source=PMT first-PCR-lock=disabled"
                      << std::endl;
        }
    }

    uint64_t maxRealPaceBitrate() const {
        if (mode == UdpShapingMode::Vbr) {
            return kMaximumTransportBitrate > kVbrTransportHeadroomBitrate
                ? kMaximumTransportBitrate - kVbrTransportHeadroomBitrate
                : kMaximumTransportBitrate;
        }
        if (configuredTargetBitrate <= 100000ULL) {
            return configuredTargetBitrate;
        }
        return configuredTargetBitrate - 100000ULL;
    }

    void updateTransportBitrate() {
        if (mode == UdpShapingMode::Cbr) {
            transportBitrate.store(configuredTargetBitrate, std::memory_order_relaxed);
            return;
        }

        const uint64_t pace = realPaceBitrate.load(std::memory_order_relaxed);
        uint64_t desired = pace + kVbrTransportHeadroomBitrate;
        desired = std::clamp<uint64_t>(
            desired, kMinimumVbrTransportBitrate, kMaximumTransportBitrate);
        transportBitrate.store(desired, std::memory_order_relaxed);
    }

    uint64_t bytesForDuration(uint64_t bitrate, uint64_t durationNanoseconds) const {
        return multiplyDivide(bitrate, durationNanoseconds, 8ULL * 1000000000ULL);
    }

    void updateRateController(uint64_t nowNanoseconds) {
        if (lastRateSampleNanoseconds == 0) {
            lastRateSampleNanoseconds = nowNanoseconds;
            lastRateSampleBytes = inputBytesReceived.load(std::memory_order_relaxed);
        }

        if (nowNanoseconds >= lastRateSampleNanoseconds &&
            nowNanoseconds - lastRateSampleNanoseconds >= kRateSampleNanoseconds) {
            const uint64_t bytesNow = inputBytesReceived.load(std::memory_order_relaxed);
            const uint64_t deltaBytes = bytesNow >= lastRateSampleBytes
                ? bytesNow - lastRateSampleBytes : 0;
            const uint64_t deltaTime = nowNanoseconds - lastRateSampleNanoseconds;
            const uint64_t instantBitrate = deltaTime > 0
                ? multiplyDivide(deltaBytes * 8ULL, 1000000000ULL, deltaTime)
                : 0;

            if (instantBitrate > 0) {
                if (estimatedInputBitrate == 0) {
                    estimatedInputBitrate = instantBitrate;
                } else {
                    // 8-sample EWMA: reacts to real service-rate changes without
                    // following the short mpegtsmux output bursts that upset UDP.
                    estimatedInputBitrate =
                        (estimatedInputBitrate * 7ULL + instantBitrate) / 8ULL;
                }
                inputBitrateEstimate.store(estimatedInputBitrate, std::memory_order_relaxed);
            }

            lastRateSampleNanoseconds = nowNanoseconds;
            lastRateSampleBytes = bytesNow;
        }

        if (lastControllerUpdateNanoseconds != 0 &&
            nowNanoseconds - lastControllerUpdateNanoseconds < kControllerUpdateNanoseconds) {
            return;
        }
        lastControllerUpdateNanoseconds = nowNanoseconds;

        const uint64_t estimate = std::max<uint64_t>(1ULL, estimatedInputBitrate);
        const uint64_t bufferNow = bufferedBytes.load(std::memory_order_relaxed);
        const uint64_t targetBufferBytes = std::max<uint64_t>(
            kUdpPayloadSize * 32ULL,
            bytesForDuration(estimate, kTargetReservoirNanoseconds));
        const uint64_t lowBufferBytes = std::max<uint64_t>(
            kUdpPayloadSize * 8ULL,
            bytesForDuration(estimate, kLowReservoirNanoseconds));

        int64_t errorBytes = 0;
        if (bufferNow >= targetBufferBytes) {
            const uint64_t diff = bufferNow - targetBufferBytes;
            errorBytes = diff > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::max()
                : static_cast<int64_t>(diff);
        } else {
            const uint64_t diff = targetBufferBytes - bufferNow;
            errorBytes = diff > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())
                ? std::numeric_limits<int64_t>::min() + 1
                : -static_cast<int64_t>(diff);
        }

        // Drain/refill the occupancy error slowly over four seconds. This is a
        // leaky-bucket controller, not a timestamp scheduler: it keeps a real
        // jitter reservoir while spreading useful TS packets uniformly through
        // the configured CBR slots.
        int64_t correctionBitrate = 0;
#if defined(__SIZEOF_INT128__)
        const __int128 numerator = static_cast<__int128>(errorBytes) * 8 * 1000000000LL;
        correctionBitrate = static_cast<int64_t>(numerator / kCorrectionHorizonNanoseconds);
#else
        const long double correction = static_cast<long double>(errorBytes) * 8.0L * 1000000000.0L /
            static_cast<long double>(kCorrectionHorizonNanoseconds);
        correctionBitrate = static_cast<int64_t>(correction);
#endif

        int64_t desired = static_cast<int64_t>(std::min<uint64_t>(
            estimate, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))) + correctionBitrate;

        if (bufferNow < lowBufferBytes) {
            // Refill rather than chasing an upstream gap. The output remains
            // strict CBR because the freed transport slots become NULL packets.
            desired = std::min<int64_t>(desired,
                static_cast<int64_t>(estimate * 85ULL / 100ULL));
            ++lowWatermarkEvents;
        }

        const uint64_t maximum = maxRealPaceBitrate();
        if (desired < 0) {
            desired = 0;
        }
        currentRealPaceBitrate = std::min<uint64_t>(
            static_cast<uint64_t>(desired), maximum);
        realPaceBitrate.store(currentRealPaceBitrate, std::memory_order_relaxed);
        updateTransportBitrate();
        targetReservoirBytes.store(targetBufferBytes, std::memory_order_relaxed);

        const uint64_t bufferMs = estimate > 0
            ? multiplyDivide(bufferNow * 8ULL, 1000ULL, estimate)
            : 0;
        reservoirMilliseconds.store(bufferMs, std::memory_order_relaxed);
    }

    struct FillCounts {
        std::size_t real = 0;
        std::size_t periodicPcr = 0;
    };

    FillCounts fillDatagram(
        guint8* destination,
        uint64_t datagramMediaNanoseconds,
        uint64_t activeTransportBitrate) {
        FillCounts counts;
        if (!destination) {
            return counts;
        }

        const uint64_t pace = realPaceBitrate.load(std::memory_order_relaxed);
        for (std::size_t slot = 0; slot < kTsPacketsPerDatagram; ++slot) {
            const uint64_t slotOffset = multiplyDivide(
                slot * kTsPacketSize * 8ULL, 1000000000ULL, activeTransportBitrate);
            const uint64_t slotTime = datagramMediaNanoseconds + slotOffset;
            guint8* outputPacket = destination + slot * kTsPacketSize;

            // Accumulate useful-data entitlement on every transport slot,
            // including slots reserved for periodic PCR-only packets.
            realTokenAccumulator += pace;

            // VBR passthrough keeps the broadcaster's original PCR values intact.
            // PTS/DTS are not rewritten, so replacing source PCR with a scheduler-derived
            // clock creates an unnecessary second time base and can produce A/V offset
            // under multi-channel load. CBR still uses periodic synthetic PCR because
            // stuffing changes the transport clock by design.
            if (mode == UdpShapingMode::Cbr && periodicPcrInitialized &&
                slotTime >= nextPeriodicPcrNanoseconds) {
                makePeriodicPcrPacket(outputPacket, slotTime);
                ++counts.periodicPcr;

                uint64_t skipped = 0;
                do {
                    nextPeriodicPcrNanoseconds += kPeriodicPcrIntervalNanoseconds;
                    if (nextPeriodicPcrNanoseconds <= slotTime) {
                        ++skipped;
                    }
                } while (nextPeriodicPcrNanoseconds <= slotTime);
                if (skipped > 0) {
                    missedPeriodicPcrIntervals.fetch_add(skipped, std::memory_order_relaxed);
                }
                continue;
            }

            bool sendReal = false;
            if (realTokenAccumulator >= activeTransportBitrate) {
                realTokenAccumulator -= activeTransportBitrate;
                sendReal = !realPackets.empty();
                if (!sendReal) {
                    ++realUnderflowSlots;
                    // Do not accumulate a catch-up burst after an upstream gap.
                    realTokenAccumulator = std::min<uint64_t>(
                        realTokenAccumulator, activeTransportBitrate - 1ULL);
                }
            }

            if (sendReal) {
                TimedTsPacket packet = std::move(realPackets.front());
                realPackets.pop_front();

                if (packet.hasPcr) {
                    if (!periodicPcrInitialized) {
                        // Never lock the WISI output clock to an arbitrary PCR
                        // seen during shared-DVB full-TS warmup.  The PMT for the
                        // selected service is authoritative.  v142 could lock to
                        // another service (for example PID 461 while SID 470
                        // declares PCR PID 471), after which periodic PCR was
                        // emitted forever on the wrong PID.
                        const bool selectedPcr = declaredPcrPidValid &&
                            packet.pid == declaredPcrPid;
                        if (selectedPcr) {
                            periodicPcrPid = declaredPcrPid;
                            periodicPcrOriginTicks = packet.sourcePcrTicks;
                            periodicPcrOriginNanoseconds = slotTime;
                            nextPeriodicPcrNanoseconds =
                                slotTime + kPeriodicPcrIntervalNanoseconds;
                            periodicPcrInitialized = true;
                            if (mode == UdpShapingMode::Cbr) {
                                writePcr(packet.bytes, periodicPcrOriginTicks);
                                ++rewrittenPcrPackets;
                            }
                            std::cerr << "UDP PCR lock: program=" << declaredPcrProgram
                                      << " pcr_pid=" << periodicPcrPid
                                      << " source=selected-PMT"
                                      << " mode="
                                      << (mode == UdpShapingMode::Vbr
                                              ? "source-passthrough"
                                              : "synthetic-cbr")
                                      << std::endl;
                        }
                    } else if (packet.pid == periodicPcrPid &&
                               mode == UdpShapingMode::Cbr) {
                        // CBR owns a synthetic transport clock because stuffing changes
                        // the wire time base. VBR keeps every source PCR untouched.
                        clearPcrFlag(packet.bytes);
                        ++strippedSourcePcrPackets;
                    }
                }

                observePcrPidContinuity(packet);
                std::copy(packet.bytes.begin(), packet.bytes.end(), outputPacket);
                bufferedBytes.fetch_sub(kTsPacketSize, std::memory_order_relaxed);
                ++counts.real;
            } else {
                makeNullPacket(outputPacket);
            }
        }
        return counts;
    }

    void observePcrPidContinuity(const TimedTsPacket& packet) {
        if (!periodicPcrInitialized || packet.pid != periodicPcrPid ||
            !packetHasPayload(packet.bytes)) {
            return;
        }
        pcrPidContinuityCounter = static_cast<guint8>(packet.bytes[3] & 0x0F);
        pcrPidContinuityValid = true;
    }

    void makePeriodicPcrPacket(guint8* destination, uint64_t slotTimeNanoseconds) {
        std::array<guint8, kTsPacketSize> packet {};
        packet.fill(0xFF);
        packet[0] = 0x47;
        packet[1] = static_cast<guint8>((periodicPcrPid >> 8) & 0x1F);
        packet[2] = static_cast<guint8>(periodicPcrPid & 0xFF);
        // Adaptation-only packets do not advance the payload continuity
        // counter. Reuse the most recent payload CC observed on the PCR PID.
        packet[3] = static_cast<guint8>(
            0x20 | (pcrPidContinuityValid ? (pcrPidContinuityCounter & 0x0F) : 0));
        packet[4] = 183;
        packet[5] = 0x10; // PCR flag

        const uint64_t elapsedNanoseconds =
            slotTimeNanoseconds >= periodicPcrOriginNanoseconds
                ? slotTimeNanoseconds - periodicPcrOriginNanoseconds
                : 0;
        const uint64_t pcrTicks =
            (periodicPcrOriginTicks + nanosecondsToPcrTicks(elapsedNanoseconds)) %
            kPcrTicksModulus;
        writePcr(packet, pcrTicks);
        std::copy(packet.begin(), packet.end(), destination);
        ++insertedPeriodicPcrPackets;
    }

    void makeNullPacket(guint8* packet) {
        packet[0] = 0x47;
        packet[1] = 0x1F;
        packet[2] = 0xFF;
        packet[3] = static_cast<guint8>(0x10 | (nullContinuityCounter & 0x0F));
        std::fill(packet + 4, packet + kTsPacketSize, 0xFF);
        nullContinuityCounter = static_cast<guint8>((nullContinuityCounter + 1) & 0x0F);
    }

    void normalizeFinalDatagramContinuity(guint8* data) {
        if (!normalizeOutputContinuity || !data) return;

        for (std::size_t slot = 0; slot < kTsPacketsPerDatagram; ++slot) {
            guint8* packet = data + slot * kTsPacketSize;
            if (packet[0] != 0x47) continue;
            const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
            if (pid >= 0x1FFF) continue;
            const guint8 adaptationControl = static_cast<guint8>((packet[3] >> 4) & 0x03);
            if (adaptationControl == 0) continue;

            // IP remap through mpegtsmux creates a fresh transport continuity
            // domain.  For DVB packet-level remap do the same: an upstream
            // discontinuity flag must not reset the newly-normalized output CC.
            // Clear the flag after absorbing it into the new transport domain.
            if ((adaptationControl == 2 || adaptationControl == 3) &&
                packet[4] > 0 && packet[4] <= 183 &&
                (packet[5] & 0x80U) != 0) {
                packet[5] = static_cast<guint8>(packet[5] & ~0x80U);
                ++finalDiscontinuitiesCleared;
            }

            const guint8 incoming = static_cast<guint8>(packet[3] & 0x0F);
            const bool hasPayload = adaptationControl == 1 || adaptationControl == 3;
            guint8 output = incoming;
            if (hasPayload) {
                if (finalContinuityValid[pid]) {
                    output = static_cast<guint8>((finalContinuity[pid] + 1) & 0x0F);
                }
                finalContinuity[pid] = output;
                finalContinuityValid[pid] = true;
            } else if (finalContinuityValid[pid]) {
                output = finalContinuity[pid];
            } else {
                finalContinuity[pid] = output;
                finalContinuityValid[pid] = true;
            }

            if (output != incoming) ++finalContinuityRewrites;
            packet[3] = static_cast<guint8>((packet[3] & 0xF0) | (output & 0x0F));
        }

        if (!finalContinuityAnnounced) {
            std::cerr << "UDP final TS continuity guard: scope=all-stable-udp stage=pre-send"
                      << " remap=" << (remapPsiNormalization ? "on" : "off")
                      << " all-pids=normalized after-PCR-insertion"
                      << " discontinuity=absorbed-and-cleared"
                      << " profile=single-output-transport-domain"
                      << std::endl;
            finalContinuityAnnounced = true;
        }
    }

    void verifyFinalDatagramContinuity(const guint8* data) {
        if (!normalizeOutputContinuity || !data) return;

        for (std::size_t slot = 0; slot < kTsPacketsPerDatagram; ++slot) {
            const guint8* packet = data + slot * kTsPacketSize;
            if (packet[0] != 0x47) continue;
            const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
            if (pid >= 0x1FFF) continue;
            const guint8 adaptationControl = static_cast<guint8>((packet[3] >> 4) & 0x03);
            if (adaptationControl == 0) continue;

            const guint8 cc = static_cast<guint8>(packet[3] & 0x0F);
            const bool hasPayload = adaptationControl == 1 || adaptationControl == 3;
            if (!finalVerifyContinuityValid[pid]) {
                finalVerifyContinuity[pid] = cc;
                finalVerifyContinuityValid[pid] = true;
                continue;
            }

            const guint8 expected = hasPayload
                ? static_cast<guint8>((finalVerifyContinuity[pid] + 1) & 0x0F)
                : finalVerifyContinuity[pid];
            if (cc != expected) {
                ++finalContinuityVerifyErrors;
            }
            if (hasPayload) finalVerifyContinuity[pid] = cc;
        }
    }

    void sendDatagram(const guint8* data, std::size_t size) {
        // Passive v152 checkpoint after PCR insertion/final CC normalization
        // and immediately before the kernel sendto() call. No bytes are changed.
        preSendCcTrace.inspect(data, size);
        const auto* destination = reinterpret_cast<const sockaddr*>(&destinationAddress);
        const socklen_t destinationSize = sizeof(destinationAddress);
        const ssize_t sent = ::sendto(socketFd, data, size, 0, destination, destinationSize);
        if (sent < 0) {
            ++sendErrors;
            std::cerr << "UDP send failed: " << std::strerror(errno) << std::endl;
            return;
        }
        if (static_cast<std::size_t>(sent) != size) {
            ++sendErrors;
            std::cerr << "UDP partial datagram send: requested=" << size
                      << " sent=" << sent << std::endl;
            return;
        }
        ++sentDatagrams;
        sentBytes.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
        if (networkBytes) {
            networkBytes->fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
        }
    }

    void maybeLogStats(uint64_t nowNanoseconds) {
        if (lastStatsNanoseconds == 0 || nowNanoseconds < lastStatsNanoseconds ||
            nowNanoseconds - lastStatsNanoseconds < kStatsIntervalNanoseconds) {
            return;
        }
        lastStatsNanoseconds = nowNanoseconds;

        const uint64_t real = totalRealPackets.load(std::memory_order_relaxed);
        const uint64_t nulls = totalNullPackets.load(std::memory_order_relaxed);
        const uint64_t elapsed = nowNanoseconds > statsStartedNanoseconds
            ? nowNanoseconds - statsStartedNanoseconds : 0;
        const uint64_t realBitrate = elapsed > 0
            ? multiplyDivide(real * kTsPacketSize * 8ULL, 1000000000ULL, elapsed)
            : 0;
        const uint64_t sentBytesNow = sentBytes.load(std::memory_order_relaxed);
        const uint64_t wireBitrate = elapsed > 0
            ? multiplyDivide(sentBytesNow * 8ULL, 1000000000ULL, elapsed)
            : 0;

        std::cerr << "UDP shaper stats: mode=" << shapingModeName(mode)
                  << " configured_target=" << configuredTargetBitrate
                  << " transport=" << transportBitrate.load(std::memory_order_relaxed)
                  << " real=" << realBitrate
                  << " input_est=" << inputBitrateEstimate.load(std::memory_order_relaxed)
                  << " real_pace=" << realPaceBitrate.load(std::memory_order_relaxed)
                  << " wire=" << wireBitrate
                  << " sent_datagrams=" << sentDatagrams.load(std::memory_order_relaxed)
                  << " send_errors=" << sendErrors.load(std::memory_order_relaxed)
                  << " buffered=" << bufferedBytes.load(std::memory_order_relaxed) << "B"
                  << " buffer_ms=" << reservoirMilliseconds.load(std::memory_order_relaxed)
                  << " target_buffer=" << targetReservoirBytes.load(std::memory_order_relaxed) << "B"
                  << " null_packets=" << nulls
                  << " pcr_rewritten=" << rewrittenPcrPackets.load(std::memory_order_relaxed)
                  << " pcr_inserted=" << insertedPeriodicPcrPackets.load(std::memory_order_relaxed)
                  << " pcr_source_stripped=" << strippedSourcePcrPackets.load(std::memory_order_relaxed)
                  << " pcr_missed_intervals=" << missedPeriodicPcrIntervals.load(std::memory_order_relaxed)
                  << " pcr_pid=" << periodicPcrPid
                  << " pcr_declared=" << (declaredPcrPidValid ? declaredPcrPid : 0x1FFF)
                  << " pcr_program=" << declaredPcrProgram
                  << " timing=reservoir_rate_controller_periodic_pcr"
                  << " pcr_clock="
                  << (mode == UdpShapingMode::Vbr
                          ? "astra_source_pcr_block"
                          : "continuous_transport_media")
                  << " pcr_source_passthrough="
                  << (mode == UdpShapingMode::Vbr ? 1 : 0)
                  << " vbr_pacing="
                  << (mode == UdpShapingMode::Vbr ? "astra_pcr_block" : "cbr_slot_clock")
                  << " startup_reservoir="
                  << startupReservoirBytes.load(std::memory_order_relaxed) << "B"
                  << " startup_pcr_samples="
                  << startupPcrSamples.load(std::memory_order_relaxed)
                  << " low_water_events=" << lowWatermarkEvents.load(std::memory_order_relaxed)
                  << " underflow_slots=" << realUnderflowSlots.load(std::memory_order_relaxed)
                  << " ts_valid=" << validTimestampChunks.load(std::memory_order_relaxed)
                  << " ts_missing=" << missingTimestampChunks.load(std::memory_order_relaxed)
                  << " timeline_shift_ms="
                  << (schedulerTimelineShiftNanoseconds.load(std::memory_order_relaxed) / 1000000ULL)
                  << " clock_resets=" << schedulerResets.load(std::memory_order_relaxed)
                  << " resync_bytes=" << resyncDiscardedBytes.load(std::memory_order_relaxed)
                  << " final_cc_rewrites=" << finalContinuityRewrites.load(std::memory_order_relaxed)
                  << " final_cc_discontinuities_cleared="
                  << finalDiscontinuitiesCleared.load(std::memory_order_relaxed)
                  << " final_cc_verify_errors="
                  << finalContinuityVerifyErrors.load(std::memory_order_relaxed)
                  << " final_sdt_rewrites=" << finalSdtRewrites.load(std::memory_order_relaxed)
                  << std::endl;
    }

    void closeSocket() {
        if (socketFd >= 0) {
            ::close(socketFd);
            socketFd = -1;
        }
        ready = false;
    }

    std::atomic<uint64_t>* networkBytes = nullptr;
    TsCcStageTrace preSendCcTrace;
    int socketFd = -1;
    bool ready = false;
    UdpShapingMode mode = UdpShapingMode::Cbr;
    uint64_t configuredTargetBitrate = 0;
    bool normalizeOutputContinuity = true;
    bool remapPsiNormalization = false;
    uint16_t remapOutputServiceId = 0;
    uint16_t expectedProgramId = 0;
    uint16_t declaredPcrPid = 0x1FFF;
    uint16_t declaredPcrProgram = 0;
    bool declaredPcrPidValid = false;
    std::string remapServiceName;
    std::string remapServiceProvider;
    uint16_t sdtTransportStreamId = 1;
    uint16_t sdtOriginalNetworkId = 1;
    guint8 sdtVersion = 0;
    bool finalSdtAnnounced = false;
    std::array<guint8, 8192> finalContinuity {};
    std::array<bool, 8192> finalContinuityValid {};
    std::array<guint8, 8192> finalVerifyContinuity {};
    std::array<bool, 8192> finalVerifyContinuityValid {};
    bool finalContinuityAnnounced = false;
    std::atomic<uint64_t> finalContinuityRewrites{0};
    std::atomic<uint64_t> finalDiscontinuitiesCleared{0};
    std::atomic<uint64_t> finalContinuityVerifyErrors{0};
    std::atomic<uint64_t> finalSdtRewrites{0};
    std::atomic<uint64_t> transportBitrate{0};
    sockaddr_in destinationAddress {};

    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> bufferedBytes{0};
    std::thread senderThread;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::condition_variable queueSpace;
    std::deque<TimedChunk> queuedChunks;
    std::deque<TimedTsPacket> realPackets;
    std::vector<guint8> inputRemainder;
    uint64_t firstChunkArrivalNanoseconds = 0;

    uint64_t estimatedInputBitrate = 0;
    uint64_t currentRealPaceBitrate = 0;
    uint64_t realTokenAccumulator = 0;
    uint64_t lastRateSampleNanoseconds = 0;
    uint64_t lastRateSampleBytes = 0;
    uint64_t lastControllerUpdateNanoseconds = 0;

    bool periodicPcrInitialized = false;
    uint16_t periodicPcrPid = 0x1FFF;
    uint64_t periodicPcrOriginTicks = 0;
    uint64_t periodicPcrOriginNanoseconds = 0;
    uint64_t nextPeriodicPcrNanoseconds = 0;
    guint8 pcrPidContinuityCounter = 0;
    bool pcrPidContinuityValid = false;
    guint8 nullContinuityCounter = 0;

    uint64_t statsStartedNanoseconds = 0;
    uint64_t lastStatsNanoseconds = 0;
    std::atomic<uint64_t> inputBytesReceived{0};
    std::atomic<uint64_t> inputBitrateEstimate{0};
    std::atomic<uint64_t> realPaceBitrate{0};
    std::atomic<uint64_t> reservoirMilliseconds{0};
    std::atomic<uint64_t> targetReservoirBytes{0};
    std::atomic<uint64_t> totalDatagrams{0};
    std::atomic<uint64_t> sentDatagrams{0};
    std::atomic<uint64_t> sentBytes{0};
    std::atomic<uint64_t> sendErrors{0};
    std::atomic<uint64_t> totalRealPackets{0};
    std::atomic<uint64_t> totalNullPackets{0};
    std::atomic<uint64_t> rewrittenPcrPackets{0};
    std::atomic<uint64_t> insertedPeriodicPcrPackets{0};
    std::atomic<uint64_t> strippedSourcePcrPackets{0};
    std::atomic<uint64_t> missedPeriodicPcrIntervals{0};
    std::atomic<uint64_t> validTimestampChunks{0};
    std::atomic<uint64_t> missingTimestampChunks{0};
    std::atomic<uint64_t> startupReservoirBytes{0};
    std::atomic<uint64_t> startupPcrSamples{0};
    std::atomic<uint64_t> lowWatermarkEvents{0};
    std::atomic<uint64_t> realUnderflowSlots{0};
    std::atomic<uint64_t> schedulerTimelineShiftNanoseconds{0};
    std::atomic<uint64_t> schedulerResets{0};
    std::atomic<uint64_t> resyncDiscardedBytes{0};
};

GstFlowReturn onNewSample(GstAppSink* sink, gpointer userData) {
    auto* sender = static_cast<StableUdpSender*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    const GstFlowReturn result = sender ? sender->pushBuffer(buffer) : GST_FLOW_ERROR;
    gst_sample_unref(sample);
    return result;
}

void destroySender(gpointer data) {
    delete static_cast<StableUdpSender*>(data);
}

} // namespace

namespace StableUdpOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    std::string& error,
    std::atomic<uint64_t>* networkBytes) {
    const UdpShapingMode mode = udpShapingMode(config);
    if (mode == UdpShapingMode::Cbr && config.targetBitrate == 0) {
        error = "UDP CBR target_bitrate must be greater than zero";
        return nullptr;
    }

    GstElement* sink = gst_element_factory_make(
        "appsink",
        sinkName.empty() ? "stable_udp_output_sink" : sinkName.c_str());
    if (!sink || !gst_bin_add(GST_BIN(pipeline), sink)) {
        if (sink) {
            gst_object_unref(sink);
        }
        error = "failed to create stable UDP appsink";
        return nullptr;
    }

    auto* sender = new StableUdpSender(config, error, networkBytes);
    if (!sender->isReady()) {
        delete sender;
        gst_bin_remove(GST_BIN(pipeline), sink);
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true");
    g_object_set(sink,
        "caps", caps,
        "emit-signals", FALSE,
        "sync", FALSE,
        "async", FALSE,
        "qos", FALSE,
        "max-lateness", static_cast<gint64>(-1),
        "enable-last-sample", FALSE,
        "drop", FALSE,
        "max-buffers", static_cast<guint>(0),
        nullptr);
    setUInt64PropertyIfPresent(sink, "processing-deadline", 0);
    gst_caps_unref(caps);

    GstAppSinkCallbacks callbacks {};
    callbacks.new_sample = onNewSample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &callbacks, sender, destroySender);

    std::cerr << "Unified UDP periodic-PCR reservoir TS shaper: mode="
              << shapingModeName(mode)
              << " target_bitrate=" << (mode == UdpShapingMode::Cbr ? config.targetBitrate : 0)
              << " vbr_rate=auto"
              << " packetization=7x188 startup_reservoir_ms=5000"
              << " startup_pcr_min=1 startup_pcr_grace_ms=2000"
              << " target_reservoir_ms=2500 low_watermark_ms=800"
              << " null_pid=0x1fff source_timing=reservoir-rate-controller"
              << " pcr_mode=periodic-pcr-only-20ms source_pcr=stripped-after-lock"
              << " pcr_restamp=continuous-transport-media"
              << " sender_clock=clock_nanosleep-abstime"
              << " pcr_scheduler_decoupled=1 busywait=off"
              << std::endl;
    return sink;
}

} // namespace StableUdpOutput
