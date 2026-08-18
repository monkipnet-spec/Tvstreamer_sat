#include "UdpTsOutput.h"

#include <gst/app/gstappsink.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
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
constexpr std::size_t kMaxQueuedBytes = 8 * 1024 * 1024;
constexpr std::size_t kMinStrictPendingBytes = 256 * 1024;
constexpr std::size_t kMaxStrictPendingBytes = 2 * 1024 * 1024;
constexpr uint64_t kStrictPendingMilliseconds = 1500ULL;
constexpr int kSocketBufferSize = 128 * 1024 * 1024;
constexpr int kMulticastTtl = 32;
constexpr uint64_t kPcrClockHz = 27000000ULL;
constexpr uint64_t kPcrWrap = (1ULL << 33) * 300ULL;
constexpr uint64_t kMinPacedBitrate = 512000ULL;
constexpr uint64_t kMaxPacedBitrate = 200000000ULL;
constexpr uint64_t kPaceRiseLimitPercent = 125ULL;
constexpr auto kMaxPaceSleep = std::chrono::milliseconds(20);
constexpr auto kArrivalRateWindow = std::chrono::seconds(1);

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

uint64_t clampPacedBitrate(uint64_t bitrate) {
    return std::clamp(bitrate, kMinPacedBitrate, kMaxPacedBitrate);
}

uint64_t pacedBitrateWithHeadroom(uint64_t bitrate, uint64_t headroomPercent) {
    const uint64_t capped = std::min(bitrate, kMaxPacedBitrate);
    return clampPacedBitrate(capped * headroomPercent / 100ULL);
}

class UdpTsSender {
public:
    UdpTsSender(
        const StreamConfig& cfg,
        UdpTsOutput::PacingConfig pacing,
        std::string& error)
        : pacingConfig(pacing),
          nextSend(std::chrono::steady_clock::now())
    {
        if (pacingConfig.headroomPercent == 0) {
            pacingConfig.headroomPercent = 100;
        }
        if (pacingConfig.configuredBitrate > 0) {
            configuredBitrate = clampPacedBitrate(pacingConfig.configuredBitrate);
        }
        if (pacingConfig.initialBitrate > 0) {
            pacedBitrate = clampPacedBitrate(pacingConfig.initialBitrate);
        }

        socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socketFd < 0) {
            error = std::string("failed to create UDP socket: ") + std::strerror(errno);
            return;
        }

        int sendBufferSize = kSocketBufferSize;
        ::setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, sizeof(sendBufferSize));

        const std::string rawOutputHost = cfg.outputHost.empty() ? "127.0.0.1" : cfg.outputHost;
        std::string outputHost;
        int outputPort = cfg.outputPort;
        if (!normalizeUdpEndpoint(rawOutputHost, cfg.outputPort, outputHost, outputPort)) {
            error = "invalid UDP output endpoint: " + rawOutputHost + ":" + std::to_string(cfg.outputPort);
            closeSocket();
            return;
        }
        sockaddr_in destination {};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(static_cast<uint16_t>(outputPort));
        if (::inet_pton(AF_INET, outputHost.c_str(), &destination.sin_addr) != 1) {
            error = "invalid UDP output host: " + outputHost;
            closeSocket();
            return;
        }
        destinationAddress = destination;

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
                if (::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_IF, &localAddress, sizeof(localAddress)) != 0) {
                    error = std::string("failed to set multicast interface: ") + std::strerror(errno);
                    closeSocket();
                    return;
                }
            } else {
                sockaddr_in bindAddress {};
                bindAddress.sin_family = AF_INET;
                bindAddress.sin_port = 0;
                bindAddress.sin_addr = localAddress;
                if (::bind(socketFd, reinterpret_cast<sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
                    error = std::string("failed to bind UDP output interface: ") + std::strerror(errno);
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
        try {
            senderThread = std::thread(&UdpTsSender::sendLoop, this);
        } catch (const std::exception& ex) {
            error = std::string("failed to create UDP sender thread: ") + ex.what();
            std::cerr << "Resource guard: " << error << std::endl;
            ready = false;
            closeSocket();
            return;
        }
    }

    ~UdpTsSender() {
        stopping = true;
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

        std::vector<guint8> chunk(map.data, map.data + map.size);
        gst_buffer_unmap(buffer, &map);

        if (pacingConfig.enabled && pacingConfig.updateFromArrivalRate) {
            updatePacingFromArrival(chunk.size());
        }

        return enqueueChunk(std::move(chunk)) ? GST_FLOW_OK : GST_FLOW_FLUSHING;
    }

private:
    bool enqueueChunk(std::vector<guint8>&& chunk) {
        if (chunk.empty() || stopping) {
            return !stopping;
        }

        std::unique_lock<std::mutex> lock(queueMutex);
        queueSpace.wait(lock, [&]() {
            return stopping || queuedBytes < kMaxQueuedBytes;
        });
        if (stopping) {
            return false;
        }

        queuedBytes += chunk.size();
        queuedChunks.push_back(std::move(chunk));
        lock.unlock();
        queueReady.notify_one();
        return true;
    }

    bool strictCbrEnabled() const {
        return pacingConfig.enabled &&
            pacingConfig.holdConfiguredRateWhenSafe &&
            configuredBitrate > 0 &&
            !pacingConfig.updateFromPcr &&
            !pacingConfig.updateFromArrivalRate;
    }

    void sendLoop() {
        if (strictCbrEnabled()) {
            sendStrictCbrLoop();
            return;
        }

        while (!stopping) {
            std::vector<guint8> chunk;
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                queueReady.wait(lock, [&]() {
                    return stopping || !queuedChunks.empty();
                });
                if (stopping) {
                    break;
                }

                chunk = std::move(queuedChunks.front());
                queuedBytes -= chunk.size();
                queuedChunks.pop_front();
            }
            queueSpace.notify_one();
            appendAndSend(chunk.data(), chunk.size());
        }
    }

    void sendStrictCbrLoop() {
        bool transmissionStarted = false;
        nextSend = std::chrono::steady_clock::now();

        while (!stopping) {
            std::vector<std::vector<guint8>> chunks;
            const std::size_t pendingTarget = strictPendingTargetBytes();
            {
                std::unique_lock<std::mutex> lock(queueMutex);
                if (!transmissionStarted) {
                    queueReady.wait(lock, [&]() {
                        return stopping || !queuedChunks.empty();
                    });
                } else {
                    // Wait for the next pacing deadline even when the producer queue
                    // already contains data. Waking immediately on a non-empty queue
                    // causes a busy loop and lets file sources run ahead of real time.
                    queueReady.wait_until(lock, nextSend, [&]() {
                        return stopping.load(std::memory_order_relaxed);
                    });
                }

                if (stopping) {
                    break;
                }

                // Move only a bounded amount into the sender-side buffer. Keeping
                // the remaining data in queuedChunks provides backpressure to
                // appsink, so a replacement file is read at approximately playback
                // speed instead of reaching EOS immediately.
                while (!queuedChunks.empty() && pending.size() < pendingTarget) {
                    queuedBytes -= queuedChunks.front().size();
                    chunks.push_back(std::move(queuedChunks.front()));
                    queuedChunks.pop_front();
                }
            }
            if (!chunks.empty()) {
                queueSpace.notify_all();
                for (const auto& chunk : chunks) {
                    appendPending(chunk.data(), chunk.size());
                }
            }

            if (!transmissionStarted) {
                if (!hasAlignedDatagram()) {
                    continue;
                }
                transmissionStarted = true;
                nextSend = std::chrono::steady_clock::now();
                pacingRemainder = 0;
            }

            auto now = std::chrono::steady_clock::now();
            if (now < nextSend) {
                std::this_thread::sleep_until(nextSend);
                now = std::chrono::steady_clock::now();
            }

            const auto nominalInterval = datagramInterval(configuredBitrate);
            if (now - nextSend > nominalInterval * 2) {
                // Do not compensate scheduler delays with a burst of packets.
                nextSend = now;
                pacingRemainder = 0;
            }

            std::array<guint8, kUdpPayloadSize> datagram {};
            if (!takeAlignedDatagram(datagram.data())) {
                makeNullDatagram(datagram.data());
            }
            sendDatagramNow(datagram.data(), datagram.size());
            advanceStrictCbrSchedule(configuredBitrate);
        }
    }

    std::size_t strictPendingTargetBytes() const {
        const uint64_t bytesForWindow = configuredBitrate > 0
            ? configuredBitrate * kStrictPendingMilliseconds / 8000ULL
            : kMinStrictPendingBytes;
        return static_cast<std::size_t>(std::clamp<uint64_t>(
            bytesForWindow,
            kMinStrictPendingBytes,
            kMaxStrictPendingBytes));
    }

    void appendPending(const guint8* data, std::size_t size) {
        if (!data || size == 0) {
            return;
        }
        pending.insert(pending.end(), data, data + size);
        resyncPending();
    }

    void appendAndSend(const guint8* data, std::size_t size) {
        appendPending(data, size);

        while (!stopping && pending.size() >= kUdpPayloadSize) {
            if (!isAlignedDatagram(pending.data())) {
                pending.erase(pending.begin());
                resyncPending();
                continue;
            }

            if (pacingConfig.enabled && pacingConfig.updateFromPcr) {
                updatePacingFromDatagram(pending.data(), kUdpPayloadSize);
            }
            sendDatagram(pending.data(), kUdpPayloadSize);
            pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(kUdpPayloadSize));
        }
    }

    bool hasAlignedDatagram() {
        resyncPending();
        return pending.size() >= kUdpPayloadSize && isAlignedDatagram(pending.data());
    }

    bool takeAlignedDatagram(guint8* destination) {
        if (!destination || !hasAlignedDatagram()) {
            return false;
        }
        std::copy_n(pending.data(), kUdpPayloadSize, destination);
        pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(kUdpPayloadSize));
        return true;
    }

    void makeNullDatagram(guint8* destination) {
        for (std::size_t packetIndex = 0; packetIndex < kTsPacketsPerDatagram; ++packetIndex) {
            guint8* packet = destination + packetIndex * kTsPacketSize;
            packet[0] = 0x47;
            packet[1] = 0x1F;
            packet[2] = 0xFF;
            packet[3] = static_cast<guint8>(0x10 | (nullContinuityCounter & 0x0F));
            std::fill(packet + 4, packet + kTsPacketSize, 0xFF);
            nullContinuityCounter = static_cast<guint8>((nullContinuityCounter + 1) & 0x0F);
        }
    }

    static std::chrono::nanoseconds datagramInterval(uint64_t bitrate) {
        const uint64_t safeBitrate = std::max<uint64_t>(bitrate, 1);
        const uint64_t numerator = kUdpPayloadSize * 8ULL * 1000000000ULL;
        return std::chrono::nanoseconds(numerator / safeBitrate);
    }

    void advanceStrictCbrSchedule(uint64_t bitrate) {
        const uint64_t safeBitrate = std::max<uint64_t>(bitrate, 1);
        const uint64_t numerator = kUdpPayloadSize * 8ULL * 1000000000ULL;
        uint64_t nanos = numerator / safeBitrate;
        pacingRemainder += numerator % safeBitrate;
        if (pacingRemainder >= safeBitrate) {
            nanos += pacingRemainder / safeBitrate;
            pacingRemainder %= safeBitrate;
        }
        nextSend += std::chrono::nanoseconds(nanos);
    }

    void resyncPending() {
        while (pending.size() >= kTsPacketSize && pending.front() != 0x47) {
            auto found = std::find(pending.begin() + 1, pending.end(), 0x47);
            pending.erase(pending.begin(), found);
        }
    }

    bool isAlignedDatagram(const guint8* data) const {
        for (std::size_t offset = 0; offset < kUdpPayloadSize; offset += kTsPacketSize) {
            if (data[offset] != 0x47) {
                return false;
            }
        }
        return true;
    }

    static bool extractPcr(const guint8* packet, uint64_t& pcr) {
        if (!packet || packet[0] != 0x47) {
            return false;
        }

        const guint8 adaptationControl = (packet[3] >> 4) & 0x03;
        if (adaptationControl != 2 && adaptationControl != 3) {
            return false;
        }

        const guint8 adaptationLength = packet[4];
        if (adaptationLength < 7 || 5 + adaptationLength > kTsPacketSize) {
            return false;
        }

        const guint8 flags = packet[5];
        if ((flags & 0x10) == 0) {
            return false;
        }

        const uint64_t pcrBase =
            (static_cast<uint64_t>(packet[6]) << 25) |
            (static_cast<uint64_t>(packet[7]) << 17) |
            (static_cast<uint64_t>(packet[8]) << 9) |
            (static_cast<uint64_t>(packet[9]) << 1) |
            (static_cast<uint64_t>(packet[10]) >> 7);
        const uint64_t pcrExt =
            ((static_cast<uint64_t>(packet[10]) & 0x01) << 8) |
            static_cast<uint64_t>(packet[11]);
        pcr = pcrBase * 300ULL + pcrExt;
        return true;
    }

    void updatePacingFromDatagram(const guint8* data, std::size_t size) {
        for (std::size_t offset = 0; offset + kTsPacketSize <= size; offset += kTsPacketSize) {
            bytesSinceLastPcr += kTsPacketSize;

            uint64_t pcr = 0;
            if (!extractPcr(data + offset, pcr)) {
                continue;
            }

            if (haveLastPcr) {
                const uint64_t pcrDelta = pcr >= lastPcr
                    ? pcr - lastPcr
                    : kPcrWrap - lastPcr + pcr;
                if (pcrDelta > kPcrClockHz / 1000 && pcrDelta < kPcrClockHz * 2 && bytesSinceLastPcr > 0) {
                    const uint64_t estimatedBitrate = bytesSinceLastPcr * 8ULL * kPcrClockHz / pcrDelta;
                    updatePacedBitrate(estimatedBitrate);
                }
            }

            lastPcr = pcr;
            haveLastPcr = true;
            bytesSinceLastPcr = 0;
        }
    }

    void updatePacedBitrate(uint64_t estimatedBitrate) {
        if (estimatedBitrate < kMinPacedBitrate || estimatedBitrate > kMaxPacedBitrate) {
            return;
        }

        estimatedBitrate = pacedBitrateWithHeadroom(
            estimatedBitrate,
            pacingConfig.headroomPercent);

        if (pacingConfig.holdConfiguredRateWhenSafe && configuredBitrate > 0) {
            const uint64_t configuredSafetyCeiling = pacedBitrateWithHeadroom(
                configuredBitrate,
                pacingConfig.headroomPercent);
            if (estimatedBitrate <= configuredSafetyCeiling) {
                pacedBitrate.store(configuredBitrate, std::memory_order_relaxed);
                return;
            }
        }

        uint64_t currentBitrate = pacedBitrate.load(std::memory_order_relaxed);
        if (currentBitrate == 0) {
            pacedBitrate.store(estimatedBitrate, std::memory_order_relaxed);
            return;
        }

        if (estimatedBitrate > currentBitrate) {
            const uint64_t riseLimit = currentBitrate * kPaceRiseLimitPercent / 100ULL;
            estimatedBitrate = std::min(estimatedBitrate, std::max(riseLimit, currentBitrate + 1));
        }
        currentBitrate = (currentBitrate * 7ULL + estimatedBitrate * 3ULL) / 10ULL;
        pacedBitrate.store(currentBitrate, std::memory_order_relaxed);
    }

    void updatePacingFromArrival(std::size_t bytes) {
        const auto now = std::chrono::steady_clock::now();
        if (!haveArrivalWindow) {
            arrivalWindowStart = now;
            haveArrivalWindow = true;
        }

        arrivalWindowBytes += bytes;
        const auto elapsed = now - arrivalWindowStart;
        if (elapsed < kArrivalRateWindow) {
            return;
        }

        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        if (micros > 0 && arrivalWindowBytes > 0) {
            const uint64_t estimatedBitrate = arrivalWindowBytes * 8ULL * 1000000ULL /
                static_cast<uint64_t>(micros);
            updatePacedBitrate(estimatedBitrate);
        }

        arrivalWindowBytes = 0;
        arrivalWindowStart = now;
    }

    void sendDatagramNow(const guint8* data, std::size_t size) {
        const auto* destination = reinterpret_cast<const sockaddr*>(&destinationAddress);
        const socklen_t destinationSize = sizeof(destinationAddress);
        ssize_t sent = ::sendto(socketFd, data, size, 0, destination, destinationSize);
        if (sent < 0) {
            std::cerr << "UDP output send failed: " << std::strerror(errno) << std::endl;
        }
    }

    void sendDatagram(const guint8* data, std::size_t size) {
        pace(size);
        const auto* destination = reinterpret_cast<const sockaddr*>(&destinationAddress);
        const socklen_t destinationSize = sizeof(destinationAddress);
        ssize_t sent = ::sendto(socketFd, data, size, 0, destination, destinationSize);
        if (sent < 0) {
            std::cerr << "UDP output send failed: " << std::strerror(errno) << std::endl;
        }
    }

    void pace(std::size_t bytes) {
        const uint64_t currentBitrate = pacedBitrate.load(std::memory_order_relaxed);
        if (!pacingConfig.enabled || currentBitrate == 0) {
            return;
        }

        const uint64_t nanos = static_cast<uint64_t>(bytes) * 8ULL * 1000000000ULL / currentBitrate;
        const auto interval = std::chrono::nanoseconds(nanos);
        const auto now = std::chrono::steady_clock::now();

        if (now > nextSend && now - nextSend > interval * 2) {
            nextSend = now;
        }

        if (nextSend > now) {
            const auto wait = nextSend - now;
            if (wait <= kMaxPaceSleep) {
                std::this_thread::sleep_until(nextSend);
            } else {
                nextSend = now;
            }
        }

        nextSend += interval;
    }

    void closeSocket() {
        if (socketFd >= 0) {
            ::close(socketFd);
            socketFd = -1;
        }
        ready = false;
    }

    int socketFd = -1;
    bool ready = false;
    UdpTsOutput::PacingConfig pacingConfig;
    uint64_t configuredBitrate = 0;
    std::atomic<uint64_t> pacedBitrate{0};
    bool haveLastPcr = false;
    uint64_t lastPcr = 0;
    uint64_t bytesSinceLastPcr = 0;
    bool haveArrivalWindow = false;
    uint64_t arrivalWindowBytes = 0;
    std::chrono::steady_clock::time_point arrivalWindowStart;
    sockaddr_in destinationAddress {};
    std::vector<guint8> pending;
    std::chrono::steady_clock::time_point nextSend;
    uint64_t pacingRemainder = 0;
    guint8 nullContinuityCounter = 0;
    std::atomic<bool> stopping{false};
    std::thread senderThread;
    std::mutex queueMutex;
    std::condition_variable queueReady;
    std::condition_variable queueSpace;
    std::deque<std::vector<guint8>> queuedChunks;
    std::size_t queuedBytes = 0;
};

GstFlowReturn onNewSample(GstAppSink* sink, gpointer userData) {
    auto* sender = static_cast<UdpTsSender*>(userData);
    GstSample* sample = gst_app_sink_pull_sample(sink);
    if (!sample) {
        return GST_FLOW_ERROR;
    }

    GstBuffer* buffer = gst_sample_get_buffer(sample);
    GstFlowReturn result = sender ? sender->pushBuffer(buffer) : GST_FLOW_ERROR;
    gst_sample_unref(sample);
    return result;
}

void destroySender(gpointer data) {
    delete static_cast<UdpTsSender*>(data);
}

} // namespace

namespace UdpTsOutput {

GstElement* createSink(
    GstElement* pipeline,
    const StreamConfig& config,
    const std::string& sinkName,
    const PacingConfig& pacing,
    std::string& error) {
    GstElement* sink = gst_element_factory_make(
        "appsink",
        sinkName.empty() ? "output_sink" : sinkName.c_str());
    if (!sink || !gst_bin_add(GST_BIN(pipeline), sink)) {
        if (sink) {
            gst_object_unref(sink);
        }
        error = "failed to create UDP appsink";
        return nullptr;
    }

    auto* sender = new UdpTsSender(config, pacing, error);
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
    return sink;
}

} // namespace UdpTsOutput
