#include "MptsOutputManager.h"

#include "utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <set>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#ifdef SO_BINDTODEVICE
#include <net/if.h>
#endif

namespace {

constexpr size_t kTsPacketSize = 188;
constexpr size_t kPacketsPerDatagram = 7;
constexpr size_t kMaxInputQueueBytes = 32U * 1024U * 1024U;
constexpr uint16_t kNullPid = 0x1FFF;
constexpr uint16_t kInvalidPid = 0xFFFF;
constexpr auto kPatPmtInterval = std::chrono::milliseconds(100);
constexpr auto kSdtInterval = std::chrono::milliseconds(500);
constexpr auto kServiceStaleTimeout = std::chrono::seconds(15);

uint16_t packetPid(const uint8_t* packet) {
    return static_cast<uint16_t>(((packet[1] & 0x1FU) << 8) | packet[2]);
}

void setPacketPid(uint8_t* packet, uint16_t pid) {
    packet[1] = static_cast<uint8_t>((packet[1] & 0xE0U) | ((pid >> 8) & 0x1FU));
    packet[2] = static_cast<uint8_t>(pid & 0xFFU);
}

size_t payloadOffset(const uint8_t* packet) {
    if (!packet || packet[0] != 0x47) return kTsPacketSize;
    const uint8_t afc = static_cast<uint8_t>((packet[3] >> 4) & 0x03U);
    if (afc == 0 || afc == 2) return kTsPacketSize;
    if (afc == 1) return 4;
    const size_t adaptationLength = packet[4];
    const size_t offset = 5 + adaptationLength;
    return offset <= kTsPacketSize ? offset : kTsPacketSize;
}

uint32_t mpegCrc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U) ? (crc << 1) ^ 0x04C11DB7U : (crc << 1);
        }
    }
    return crc;
}

void appendCrc(std::vector<uint8_t>& section) {
    const uint32_t crc = mpegCrc32(section.data(), section.size());
    section.push_back(static_cast<uint8_t>((crc >> 24) & 0xFFU));
    section.push_back(static_cast<uint8_t>((crc >> 16) & 0xFFU));
    section.push_back(static_cast<uint8_t>((crc >> 8) & 0xFFU));
    section.push_back(static_cast<uint8_t>(crc & 0xFFU));
}

bool sectionCrcValid(const std::vector<uint8_t>& section) {
    if (section.size() < 4) return false;
    const size_t payloadSize = section.size() - 4;
    const uint32_t expected =
        (static_cast<uint32_t>(section[payloadSize]) << 24) |
        (static_cast<uint32_t>(section[payloadSize + 1]) << 16) |
        (static_cast<uint32_t>(section[payloadSize + 2]) << 8) |
        static_cast<uint32_t>(section[payloadSize + 3]);
    return mpegCrc32(section.data(), payloadSize) == expected;
}

void setSectionLength(std::vector<uint8_t>& section) {
    if (section.size() < 3) return;
    const size_t length = section.size() - 3 + 4; // bytes after section_length + CRC
    section[1] = static_cast<uint8_t>((section[1] & 0xF0U) | ((length >> 8) & 0x0FU));
    section[2] = static_cast<uint8_t>(length & 0xFFU);
}

std::vector<uint8_t> filterCaDescriptors(const uint8_t* data, size_t size) {
    std::vector<uint8_t> result;
    size_t offset = 0;
    while (offset + 2 <= size) {
        const uint8_t tag = data[offset];
        const size_t descriptorSize = static_cast<size_t>(data[offset + 1]) + 2;
        if (offset + descriptorSize > size) break;
        // CA descriptors contain ECM/EMM PID references from the source SPTS.
        // MPTS is built from the already-decoded channel output, so carrying
        // stale CA descriptors would advertise invalid PIDs in the new mux.
        if (tag != 0x09) {
            result.insert(result.end(), data + offset, data + offset + descriptorSize);
        }
        offset += descriptorSize;
    }
    return result;
}

struct PsiAssembler {
    std::vector<uint8_t> pending;
    size_t expected = 0;

    void reset() {
        pending.clear();
        expected = 0;
    }

    void appendBytes(const uint8_t* data, size_t size,
                     std::vector<std::vector<uint8_t>>& completed,
                     bool allowNewSections) {
        size_t offset = 0;
        while (offset < size) {
            if (pending.empty()) {
                if (!allowNewSections || data[offset] == 0xFF) return;
                expected = 0;
            }

            if (expected == 0) {
                const size_t headerNeed = 3 - std::min<size_t>(pending.size(), 3);
                const size_t take = std::min(headerNeed, size - offset);
                pending.insert(pending.end(), data + offset, data + offset + take);
                offset += take;
                if (pending.size() < 3) return;
                const size_t sectionLength =
                    (static_cast<size_t>(pending[1] & 0x0FU) << 8) | pending[2];
                if (sectionLength < 4 || sectionLength > 1021) {
                    reset();
                    return;
                }
                expected = 3 + sectionLength;
            }

            if (pending.size() < expected) {
                const size_t take = std::min(expected - pending.size(), size - offset);
                pending.insert(pending.end(), data + offset, data + offset + take);
                offset += take;
            }
            if (expected && pending.size() == expected) {
                completed.push_back(pending);
                reset();
                allowNewSections = true;
            }
        }
    }

    std::vector<std::vector<uint8_t>> feed(const uint8_t* packet) {
        std::vector<std::vector<uint8_t>> completed;
        if (!packet || packet[0] != 0x47) return completed;
        const size_t payload = payloadOffset(packet);
        if (payload >= kTsPacketSize) return completed;
        const bool pusi = (packet[1] & 0x40U) != 0;
        const uint8_t* bytes = packet + payload;
        size_t size = kTsPacketSize - payload;

        if (!pusi) {
            if (!pending.empty()) appendBytes(bytes, size, completed, false);
            return completed;
        }
        if (size == 0) return completed;
        const size_t pointer = bytes[0];
        ++bytes;
        --size;
        if (pointer > size) {
            reset();
            return completed;
        }

        if (!pending.empty() && pointer > 0) {
            appendBytes(bytes, pointer, completed, false);
            if (!pending.empty()) reset();
        } else if (!pending.empty()) {
            reset();
        }
        bytes += pointer;
        size -= pointer;
        if (size > 0) appendBytes(bytes, size, completed, true);
        return completed;
    }
};

struct ElementaryStreamInfo {
    uint8_t streamType = 0;
    uint16_t sourcePid = kNullPid;
    std::vector<uint8_t> descriptors;
};

struct ProgramInfo {
    uint16_t sourceServiceId = 0;
    uint16_t sourcePcrPid = kNullPid;
    std::vector<uint8_t> programDescriptors;
    std::vector<ElementaryStreamInfo> streams;
    uint32_t sourcePmtCrc = 0;
};

bool parsePat(const std::vector<uint8_t>& section, uint16_t preferredSid,
              uint16_t& serviceId, uint16_t& pmtPid) {
    if (section.size() < 12 || section[0] != 0x00 || !sectionCrcValid(section)) return false;
    const size_t end = section.size() - 4;
    if (end < 8) return false;

    uint16_t firstSid = 0;
    uint16_t firstPmt = kNullPid;
    for (size_t offset = 8; offset + 4 <= end; offset += 4) {
        const uint16_t sid = static_cast<uint16_t>((section[offset] << 8) | section[offset + 1]);
        const uint16_t pid = static_cast<uint16_t>(((section[offset + 2] & 0x1FU) << 8) |
                                                   section[offset + 3]);
        if (sid == 0) continue;
        if (firstSid == 0) {
            firstSid = sid;
            firstPmt = pid;
        }
        if (preferredSid != 0 && sid == preferredSid) {
            serviceId = sid;
            pmtPid = pid;
            return true;
        }
    }
    if (firstSid == 0 || firstPmt == kNullPid) return false;
    serviceId = firstSid;
    pmtPid = firstPmt;
    return true;
}

bool parsePmt(const std::vector<uint8_t>& section, ProgramInfo& info) {
    if (section.size() < 16 || section[0] != 0x02 || !sectionCrcValid(section)) return false;
    const size_t end = section.size() - 4;
    info = ProgramInfo{};
    info.sourceServiceId = static_cast<uint16_t>((section[3] << 8) | section[4]);
    info.sourcePcrPid = static_cast<uint16_t>(((section[8] & 0x1FU) << 8) | section[9]);
    const size_t programInfoLength =
        (static_cast<size_t>(section[10] & 0x0FU) << 8) | section[11];
    size_t offset = 12;
    if (offset + programInfoLength > end) return false;
    info.programDescriptors = filterCaDescriptors(section.data() + offset, programInfoLength);
    offset += programInfoLength;

    while (offset + 5 <= end) {
        ElementaryStreamInfo es;
        es.streamType = section[offset];
        es.sourcePid = static_cast<uint16_t>(((section[offset + 1] & 0x1FU) << 8) |
                                             section[offset + 2]);
        const size_t esInfoLength =
            (static_cast<size_t>(section[offset + 3] & 0x0FU) << 8) | section[offset + 4];
        offset += 5;
        if (offset + esInfoLength > end) return false;
        es.descriptors = filterCaDescriptors(section.data() + offset, esInfoLength);
        offset += esInfoLength;
        info.streams.push_back(std::move(es));
    }
    if (info.streams.empty()) return false;
    const size_t crcOffset = section.size() - 4;
    info.sourcePmtCrc =
        (static_cast<uint32_t>(section[crcOffset]) << 24) |
        (static_cast<uint32_t>(section[crcOffset + 1]) << 16) |
        (static_cast<uint32_t>(section[crcOffset + 2]) << 8) |
        static_cast<uint32_t>(section[crcOffset + 3]);
    return true;
}

bool isMulticastAddress(const std::string& host) {
    in_addr address{};
    if (::inet_pton(AF_INET, host.c_str(), &address) != 1) return false;
    const uint32_t value = ntohl(address.s_addr);
    const uint8_t first = static_cast<uint8_t>((value >> 24) & 0xFFU);
    return first >= 224 && first <= 239;
}

std::string interfaceNameForAddress(const std::string& address) {
    if (address.empty()) return {};
    for (const auto& iface : enumerateNetworkInterfaces(true)) {
        if (normalizeIpAddress(iface.address) == normalizeIpAddress(address)) return iface.name;
    }
    return {};
}

} // namespace

struct MptsOutputManager::Runtime {
    struct InputChunk {
        std::string streamId;
        std::vector<uint8_t> data;
    };

    struct ServiceState {
        MptsServiceConfig config;
        std::string name;
        std::string provider;
        uint16_t preferredSourceSid = 0;
        uint16_t outputServiceId = 1;
        uint16_t outputPmtPid = 0x1000;
        uint16_t pidRangeBase = 0x0100;
        uint16_t sourcePmtPid = kNullPid;
        uint16_t sourceServiceId = 0;
        PsiAssembler patAssembler;
        PsiAssembler pmtAssembler;
        ProgramInfo program;
        std::array<uint16_t, 8192> pidMap{};
        std::array<bool, 8192> allowedSourcePids{};
        std::set<uint16_t> allocatedOutputPids;
        uint8_t pmtCc = 0;
        bool ready = false;
        uint64_t inputPackets = 0;
        std::chrono::steady_clock::time_point lastData{};
        // GstBuffer boundaries are not guaranteed to match 188-byte TS packet
        // boundaries. Keep at most one partial packet between callbacks so a
        // split packet is never discarded by the MPTS tap.
        std::vector<uint8_t> tsRemainder;

        ServiceState() {
            pidMap.fill(kInvalidPid);
            allowedSourcePids.fill(false);
        }
    };

    explicit Runtime(MptsOutputConfig cfg, const std::vector<StreamConfig>& streamConfigs)
        : config(std::move(cfg)) {
        std::map<std::string, StreamConfig> byId;
        for (const auto& stream : streamConfigs) byId[stream.id] = stream;

        services.reserve(config.services.size());
        std::set<uint16_t> assignedServiceIds;
        for (size_t index = 0; index < config.services.size(); ++index) {
            ServiceState service;
            service.config = config.services[index];
            const auto source = byId.find(service.config.streamId);
            if (source != byId.end()) {
                service.name = source->second.name.empty() ? source->second.id : source->second.name;
                service.provider = source->second.serviceProvider.empty()
                    ? "TVStreammerSAT5" : source->second.serviceProvider;
                service.preferredSourceSid = static_cast<uint16_t>(
                    std::min<uint32_t>(source->second.serviceId, 0xFFFFU));
            } else {
                service.name = service.config.streamId;
                service.provider = "TVStreammerSAT5";
            }
            uint32_t sid = service.config.serviceId;
            if (sid == 0) sid = config.serviceIdBase + static_cast<uint32_t>(index);
            sid = std::clamp<uint32_t>(sid, 1, 0xFFFFU);
            uint16_t selectedSid = static_cast<uint16_t>(sid);
            while (assignedServiceIds.count(selectedSid) && selectedSid < 0xFFFFU) ++selectedSid;
            if (assignedServiceIds.count(selectedSid)) {
                selectedSid = 1;
                while (assignedServiceIds.count(selectedSid) && selectedSid < 0xFFFFU) ++selectedSid;
            }
            assignedServiceIds.insert(selectedSid);
            service.outputServiceId = selectedSid;
            service.outputPmtPid = static_cast<uint16_t>(
                std::clamp<uint32_t>(config.pmtPidBase + static_cast<uint32_t>(index), 0x20U, 0x1FFEU));
            service.pidRangeBase = static_cast<uint16_t>(0x0100U + static_cast<uint32_t>(index) * 0x20U);
            services.push_back(std::move(service));
        }
        for (size_t index = 0; index < services.size(); ++index) {
            serviceIndex[services[index].config.streamId] = index;
        }
    }

    ~Runtime() { stop(); }

    void setLastError(const std::string& value) {
        std::lock_guard<std::mutex> lock(errorMutex);
        lastError = value;
    }

    bool sameConfig(const MptsOutputConfig& other) const {
        return config.toJson() == other.toJson();
    }

    bool start(std::string& error) {
        std::lock_guard<std::mutex> lock(mutex);
        if (running.load(std::memory_order_relaxed)) return true;
        error.clear();
        socketFd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (socketFd < 0) {
            error = "unable to create UDP socket";
            setLastError(error);
            return false;
        }
        int sendBuffer = 4 * 1024 * 1024;
        ::setsockopt(socketFd, SOL_SOCKET, SO_SNDBUF, &sendBuffer, sizeof(sendBuffer));

        sockaddr_in destination{};
        destination.sin_family = AF_INET;
        destination.sin_port = htons(static_cast<uint16_t>(config.outputPort));
        if (::inet_pton(AF_INET, config.outputHost.c_str(), &destination.sin_addr) != 1) {
            error = "invalid MPTS output address: " + config.outputHost;
            setLastError(error);
            ::close(socketFd);
            socketFd = -1;
            return false;
        }
        destinationAddress = destination;

        if (!config.interfaceAddress.empty() && config.interfaceAddress != "0.0.0.0") {
            in_addr localAddress{};
            if (::inet_pton(AF_INET, config.interfaceAddress.c_str(), &localAddress) != 1) {
                error = "invalid MPTS interface address: " + config.interfaceAddress;
                setLastError(error);
                ::close(socketFd);
                socketFd = -1;
                return false;
            }
#ifdef SO_BINDTODEVICE
            const std::string interfaceName = interfaceNameForAddress(config.interfaceAddress);
            if (!interfaceName.empty() &&
                ::setsockopt(socketFd, SOL_SOCKET, SO_BINDTODEVICE,
                    interfaceName.c_str(), static_cast<socklen_t>(interfaceName.size() + 1)) != 0) {
                error = "failed to bind MPTS output device: " + interfaceName;
                setLastError(error);
                ::close(socketFd);
                socketFd = -1;
                return false;
            }
#endif
            sockaddr_in local{};
            local.sin_family = AF_INET;
            local.sin_port = htons(0);
            local.sin_addr = localAddress;
            if (::bind(socketFd, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
                error = "failed to bind MPTS output interface: " + config.interfaceAddress;
                setLastError(error);
                ::close(socketFd);
                socketFd = -1;
                return false;
            }
            if (isMulticastAddress(config.outputHost)) {
                ::setsockopt(socketFd, IPPROTO_IP, IP_MULTICAST_IF,
                    &localAddress, sizeof(localAddress));
            }
        }

        stopRequested = false;
        running = true;
        setLastError("");
        incoming.clear();
        incomingBytes = 0;
        sendAccumulator.clear();
        {
            std::lock_guard<std::mutex> stateLock(serviceMutex);
            for (auto& service : services) {
                service.sourcePmtPid = kNullPid;
                service.sourceServiceId = 0;
                service.program = ProgramInfo{};
                service.pidMap.fill(kInvalidPid);
                service.allowedSourcePids.fill(false);
                service.allocatedOutputPids.clear();
                service.ready = false;
                service.pmtCc = 0;
                service.inputPackets = 0;
                service.tsRemainder.clear();
                service.patAssembler.reset();
                service.pmtAssembler.reset();
                service.lastData = std::chrono::steady_clock::time_point{};
            }
        }
        lastPatPmt = std::chrono::steady_clock::time_point{};
        lastSdt = std::chrono::steady_clock::time_point{};
        try {
            // Runtime is owned by MptsOutputManager. The worker must not own a
            // shared_ptr back to Runtime: that creates a self-retaining lifetime
            // where destruction alone cannot stop the thread. Runtime::~Runtime
            // calls stop(), which joins this raw-this worker before memory is freed.
            worker = std::thread([this] { run(); });
        } catch (const std::exception& ex) {
            running = false;
            stopRequested = true;
            error = std::string("unable to start MPTS worker: ") + ex.what();
            setLastError(error);
            ::close(socketFd);
            socketFd = -1;
            return false;
        }
        std::cerr << "MPTS output started: id=" << config.id
                  << " name=\"" << config.name << "\""
                  << " udp=" << config.outputHost << ":" << config.outputPort
                  << " iface=" << (config.interfaceAddress.empty() ? "auto" : config.interfaceAddress)
                  << " services=" << services.size()
                  << " mode=packet-aggregate source_timestamps=preserved null_input=drop" << std::endl;
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!running.load(std::memory_order_relaxed) && !worker.joinable()) {
                if (socketFd >= 0) {
                    ::close(socketFd);
                    socketFd = -1;
                }
                return;
            }
            stopRequested = true;
            running = false;
        }
        condition.notify_all();
        if (worker.joinable()) worker.join();
        std::lock_guard<std::mutex> lock(mutex);
        if (socketFd >= 0) {
            ::close(socketFd);
            socketFd = -1;
        }
        std::deque<InputChunk>().swap(incoming);
        incomingBytes = 0;
        std::vector<uint8_t>().swap(sendAccumulator);
        {
            std::lock_guard<std::mutex> stateLock(serviceMutex);
            for (auto& service : services) {
                service.program = ProgramInfo{};
                service.allocatedOutputPids.clear();
                service.pidMap.fill(kInvalidPid);
                service.allowedSourcePids.fill(false);
                service.ready = false;
                service.sourcePmtPid = kNullPid;
                service.sourceServiceId = 0;
                std::vector<uint8_t>().swap(service.tsRemainder);
                service.patAssembler.reset();
                service.pmtAssembler.reset();
            }
        }
        std::cerr << "MPTS output stopped: id=" << config.id << std::endl;
    }

    void enqueue(const std::string& streamId, const uint8_t* data, size_t size) {
        if (!running.load(std::memory_order_relaxed) || !data || size == 0) return;
        InputChunk chunk;
        chunk.streamId = streamId;
        chunk.data.assign(data, data + size);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopRequested || !running.load(std::memory_order_relaxed)) return;
            while (!incoming.empty() && incomingBytes + chunk.data.size() > kMaxInputQueueBytes) {
                droppedInputBytes += incoming.front().data.size();
                incomingBytes -= incoming.front().data.size();
                incoming.pop_front();
            }
            if (chunk.data.size() > kMaxInputQueueBytes) {
                droppedInputBytes += chunk.data.size();
                return;
            }
            incomingBytes += chunk.data.size();
            incoming.push_back(std::move(chunk));
        }
        condition.notify_one();
    }

    uint16_t allocateMappedPid(ServiceState& service, uint16_t sourcePid) {
        if (sourcePid >= service.pidMap.size()) return kInvalidPid;
        if (service.pidMap[sourcePid] != kInvalidPid) return service.pidMap[sourcePid];

        auto available = [&](uint16_t pid) {
            if (pid < 0x20 || pid == kNullPid) return false;
            for (const auto& candidate : services) {
                if (candidate.outputPmtPid == pid || candidate.allocatedOutputPids.count(pid)) return false;
            }
            return true;
        };

        uint16_t candidate = service.pidRangeBase;
        for (uint32_t attempt = 0; attempt < 0x1F00U; ++attempt) {
            if (candidate >= 0x1FFF) candidate = 0x0020;
            if (available(candidate)) {
                service.pidMap[sourcePid] = candidate;
                service.allocatedOutputPids.insert(candidate);
                return candidate;
            }
            ++candidate;
        }
        return kInvalidPid;
    }

    bool applyPmt(ServiceState& service, const std::vector<uint8_t>& section) {
        ProgramInfo parsed;
        if (!parsePmt(section, parsed)) return false;
        if (service.program.sourcePmtCrc == parsed.sourcePmtCrc && service.ready) return false;

        std::array<bool, 8192> wantedSourcePids{};
        wantedSourcePids.fill(false);
        if (parsed.sourcePcrPid != kNullPid && parsed.sourcePcrPid < wantedSourcePids.size()) {
            wantedSourcePids[parsed.sourcePcrPid] = true;
        }
        for (const auto& es : parsed.streams) {
            if (es.sourcePid < wantedSourcePids.size()) wantedSourcePids[es.sourcePid] = true;
        }

        // Release mappings for PIDs which disappeared from a changed PMT.
        // Without this, repeated PMT changes would permanently consume output
        // PIDs until the allocator eventually ran out.
        for (size_t sourcePid = 0; sourcePid < service.pidMap.size(); ++sourcePid) {
            if (wantedSourcePids[sourcePid] || service.pidMap[sourcePid] == kInvalidPid) continue;
            service.allocatedOutputPids.erase(service.pidMap[sourcePid]);
            service.pidMap[sourcePid] = kInvalidPid;
        }
        service.allowedSourcePids = wantedSourcePids;
        if (parsed.sourcePcrPid != kNullPid) allocateMappedPid(service, parsed.sourcePcrPid);
        for (const auto& es : parsed.streams) allocateMappedPid(service, es.sourcePid);
        service.program = std::move(parsed);
        const bool wasReady = service.ready;
        service.ready = !service.program.streams.empty();
        if (!wasReady || service.ready) {
            psiVersion = static_cast<uint8_t>((psiVersion + 1) & 0x1FU);
            psiDirty = true;
        }
        if (!wasReady && service.ready) {
            std::cerr << "MPTS service ready: output=" << config.id
                      << " stream=" << service.config.streamId
                      << " sid=" << service.outputServiceId
                      << " pmt_pid=" << service.outputPmtPid
                      << " es=" << service.program.streams.size() << std::endl;
        }
        return true;
    }

    std::vector<uint8_t> buildPat() const {
        std::vector<uint8_t> section = {
            0x00, 0xB0, 0x00,
            static_cast<uint8_t>((config.transportStreamId >> 8) & 0xFFU),
            static_cast<uint8_t>(config.transportStreamId & 0xFFU),
            static_cast<uint8_t>(0xC1U | ((psiVersion & 0x1FU) << 1)),
            0x00, 0x00
        };
        for (const auto& service : services) {
            if (!service.ready) continue;
            section.push_back(static_cast<uint8_t>((service.outputServiceId >> 8) & 0xFFU));
            section.push_back(static_cast<uint8_t>(service.outputServiceId & 0xFFU));
            section.push_back(static_cast<uint8_t>(0xE0U | ((service.outputPmtPid >> 8) & 0x1FU)));
            section.push_back(static_cast<uint8_t>(service.outputPmtPid & 0xFFU));
        }
        setSectionLength(section);
        appendCrc(section);
        return section;
    }

    std::vector<uint8_t> buildPmt(const ServiceState& service) const {
        const uint16_t sourcePcr = service.program.sourcePcrPid;
        uint16_t outputPcr = kNullPid;
        if (sourcePcr < service.pidMap.size() && service.pidMap[sourcePcr] != kInvalidPid) {
            outputPcr = service.pidMap[sourcePcr];
        }
        std::vector<uint8_t> section = {
            0x02, 0xB0, 0x00,
            static_cast<uint8_t>((service.outputServiceId >> 8) & 0xFFU),
            static_cast<uint8_t>(service.outputServiceId & 0xFFU),
            static_cast<uint8_t>(0xC1U | ((psiVersion & 0x1FU) << 1)),
            0x00, 0x00,
            static_cast<uint8_t>(0xE0U | ((outputPcr >> 8) & 0x1FU)),
            static_cast<uint8_t>(outputPcr & 0xFFU),
            static_cast<uint8_t>(0xF0U | ((service.program.programDescriptors.size() >> 8) & 0x0FU)),
            static_cast<uint8_t>(service.program.programDescriptors.size() & 0xFFU)
        };
        section.insert(section.end(), service.program.programDescriptors.begin(),
                       service.program.programDescriptors.end());
        for (const auto& es : service.program.streams) {
            if (es.sourcePid >= service.pidMap.size()) continue;
            const uint16_t outputPid = service.pidMap[es.sourcePid];
            if (outputPid == kInvalidPid) continue;
            section.push_back(es.streamType);
            section.push_back(static_cast<uint8_t>(0xE0U | ((outputPid >> 8) & 0x1FU)));
            section.push_back(static_cast<uint8_t>(outputPid & 0xFFU));
            section.push_back(static_cast<uint8_t>(0xF0U | ((es.descriptors.size() >> 8) & 0x0FU)));
            section.push_back(static_cast<uint8_t>(es.descriptors.size() & 0xFFU));
            section.insert(section.end(), es.descriptors.begin(), es.descriptors.end());
        }
        setSectionLength(section);
        appendCrc(section);
        return section;
    }

    std::vector<std::vector<uint8_t>> buildSdtSections() const {
        std::vector<std::vector<uint8_t>> result;
        std::vector<const ServiceState*> ready;
        for (const auto& service : services) if (service.ready) ready.push_back(&service);
        size_t index = 0;
        uint8_t sectionNumber = 0;
        while (index < ready.size() || (ready.empty() && result.empty())) {
            std::vector<uint8_t> section = {
                0x42, 0xF0, 0x00,
                static_cast<uint8_t>((config.transportStreamId >> 8) & 0xFFU),
                static_cast<uint8_t>(config.transportStreamId & 0xFFU),
                static_cast<uint8_t>(0xC1U | ((psiVersion & 0x1FU) << 1)),
                sectionNumber, 0x00,
                static_cast<uint8_t>((config.originalNetworkId >> 8) & 0xFFU),
                static_cast<uint8_t>(config.originalNetworkId & 0xFFU),
                0xFF
            };
            while (index < ready.size()) {
                const auto& service = *ready[index];
                std::string provider = service.provider.empty() ? "TVStreammerSAT5" : service.provider;
                std::string name = service.name.empty() ? service.config.streamId : service.name;
                if (provider.size() > 63) provider.resize(63);
                if (name.size() > 63) name.resize(63);
                const size_t descriptorPayload = 3 + provider.size() + name.size();
                const size_t descriptorSize = descriptorPayload + 2;
                const size_t serviceEntrySize = 5 + descriptorSize;
                if (section.size() + serviceEntrySize + 4 > 1000 && section.size() > 11) break;

                section.push_back(static_cast<uint8_t>((service.outputServiceId >> 8) & 0xFFU));
                section.push_back(static_cast<uint8_t>(service.outputServiceId & 0xFFU));
                section.push_back(0xFC); // EIT flags off + reserved
                const uint16_t loopLength = static_cast<uint16_t>(descriptorSize);
                section.push_back(static_cast<uint8_t>(0x80U | ((loopLength >> 8) & 0x0FU))); // running
                section.push_back(static_cast<uint8_t>(loopLength & 0xFFU));
                section.push_back(0x48);
                section.push_back(static_cast<uint8_t>(descriptorPayload));
                section.push_back(0x01); // digital television service
                section.push_back(static_cast<uint8_t>(provider.size()));
                section.insert(section.end(), provider.begin(), provider.end());
                section.push_back(static_cast<uint8_t>(name.size()));
                section.insert(section.end(), name.begin(), name.end());
                ++index;
            }
            setSectionLength(section);
            appendCrc(section);
            result.push_back(std::move(section));
            ++sectionNumber;
            if (ready.empty()) break;
        }
        const uint8_t last = result.empty() ? 0 : static_cast<uint8_t>(result.size() - 1);
        // last_section_number is covered by CRC; update and recalculate.
        for (size_t i = 0; i < result.size(); ++i) {
            auto& section = result[i];
            if (section.size() < 12) continue;
            section[6] = static_cast<uint8_t>(i);
            section[7] = last;
            section.resize(section.size() - 4);
            appendCrc(section);
        }
        return result;
    }

    void packetizeSection(uint16_t pid, const std::vector<uint8_t>& section, uint8_t& cc) {
        size_t offset = 0;
        bool first = true;
        while (offset < section.size()) {
            std::array<uint8_t, kTsPacketSize> packet{};
            packet.fill(0xFF);
            packet[0] = 0x47;
            packet[1] = static_cast<uint8_t>(((pid >> 8) & 0x1FU) | (first ? 0x40U : 0x00U));
            packet[2] = static_cast<uint8_t>(pid & 0xFFU);
            packet[3] = static_cast<uint8_t>(0x10U | (cc & 0x0FU));
            cc = static_cast<uint8_t>((cc + 1) & 0x0FU);
            size_t payload = 4;
            if (first) packet[payload++] = 0x00;
            const size_t copy = std::min(section.size() - offset, kTsPacketSize - payload);
            std::memcpy(packet.data() + payload, section.data() + offset, copy);
            offset += copy;
            sendPacket(packet.data());
            first = false;
        }
    }

    void emitPatPmt() {
        auto pat = buildPat();
        packetizeSection(0x0000, pat, patCc);
        for (auto& service : services) {
            if (!service.ready) continue;
            auto pmt = buildPmt(service);
            packetizeSection(service.outputPmtPid, pmt, service.pmtCc);
        }
        psiDirty = false;
        lastPatPmt = std::chrono::steady_clock::now();
    }

    void emitSdt() {
        for (const auto& section : buildSdtSections()) {
            packetizeSection(0x0011, section, sdtCc);
        }
        lastSdt = std::chrono::steady_clock::now();
    }

    void sendPacket(const uint8_t* packet) {
        sendAccumulator.insert(sendAccumulator.end(), packet, packet + kTsPacketSize);
        if (sendAccumulator.size() >= kPacketsPerDatagram * kTsPacketSize) flushDatagram();
    }

    void flushDatagram() {
        if (sendAccumulator.empty() || socketFd < 0) return;
        const ssize_t sent = ::sendto(socketFd, sendAccumulator.data(), sendAccumulator.size(), MSG_NOSIGNAL,
                                      reinterpret_cast<const sockaddr*>(&destinationAddress),
                                      sizeof(destinationAddress));
        if (sent < 0) {
            ++sendErrors;
            setLastError("MPTS UDP send failed");
        } else {
            bytesOut += static_cast<uint64_t>(sent);
            packetsOut += static_cast<uint64_t>(sent) / kTsPacketSize;
        }
        sendAccumulator.clear();
    }

    void processChunk(InputChunk&& chunk) {
        std::lock_guard<std::mutex> stateLock(serviceMutex);
        const auto found = serviceIndex.find(chunk.streamId);
        if (found == serviceIndex.end()) return;
        auto& service = services[found->second];

        // Reassemble TS independently per source stream. A pad probe may hand
        // us 1..N packets plus an arbitrary trailing fragment. Never align one
        // stream using bytes from another stream.
        std::vector<uint8_t> bytes;
        if (!service.tsRemainder.empty()) {
            bytes.reserve(service.tsRemainder.size() + chunk.data.size());
            bytes.insert(bytes.end(), service.tsRemainder.begin(), service.tsRemainder.end());
            bytes.insert(bytes.end(), chunk.data.begin(), chunk.data.end());
            service.tsRemainder.clear();
        } else {
            bytes = std::move(chunk.data);
        }

        size_t offset = 0;
        while (offset + kTsPacketSize <= bytes.size()) {
            if (bytes[offset] != 0x47) {
                // Resynchronise conservatively. When two packets are available,
                // require the next 188-byte boundary to be sync as well.
                size_t candidate = offset + 1;
                for (; candidate < bytes.size(); ++candidate) {
                    if (bytes[candidate] != 0x47) continue;
                    if (candidate + kTsPacketSize >= bytes.size() ||
                        bytes[candidate + kTsPacketSize] == 0x47) break;
                }
                offset = candidate;
                continue;
            }

            const uint8_t* packet = bytes.data() + offset;
            ++service.inputPackets;
            service.lastData = std::chrono::steady_clock::now();
            const uint16_t pid = packetPid(packet);

            if (pid == 0x0000) {
                for (const auto& section : service.patAssembler.feed(packet)) {
                    uint16_t sourceSid = 0;
                    uint16_t sourcePmt = kNullPid;
                    if (parsePat(section, service.preferredSourceSid, sourceSid, sourcePmt)) {
                        if (service.sourcePmtPid != sourcePmt || service.sourceServiceId != sourceSid) {
                            service.sourcePmtPid = sourcePmt;
                            service.sourceServiceId = sourceSid;
                            service.pmtAssembler.reset();
                            service.ready = false;
                            psiVersion = static_cast<uint8_t>((psiVersion + 1) & 0x1FU);
                            psiDirty = true;
                        }
                    }
                }
            } else if (pid == service.sourcePmtPid && pid != kNullPid) {
                for (const auto& section : service.pmtAssembler.feed(packet)) {
                    const bool changed = applyPmt(service, section);
                    if (changed && psiDirty) {
                        emitPatPmt();
                        emitSdt();
                    }
                }
            } else if (pid >= 0x0020 && pid != kNullPid && service.ready &&
                       pid < service.allowedSourcePids.size() && service.allowedSourcePids[pid]) {
                const uint16_t mapped = service.pidMap[pid];
                if (mapped != kInvalidPid) {
                    std::array<uint8_t, kTsPacketSize> output{};
                    std::memcpy(output.data(), packet, kTsPacketSize);
                    setPacketPid(output.data(), mapped);
                    sendPacket(output.data());
                }
            }
            offset += kTsPacketSize;
        }

        if (offset < bytes.size()) {
            service.tsRemainder.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
            // A valid remainder is <188 bytes. If resync left a larger tail,
            // bound it to one packet so corrupt input cannot grow memory.
            if (service.tsRemainder.size() >= kTsPacketSize) {
                const auto keep = std::min(service.tsRemainder.size(), kTsPacketSize - 1);
                service.tsRemainder.erase(service.tsRemainder.begin(),
                    service.tsRemainder.end() - static_cast<std::ptrdiff_t>(keep));
            }
        }
    }

    void run() {
        while (true) {
            InputChunk chunk;
            bool haveChunk = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait_for(lock, std::chrono::milliseconds(50), [&] {
                    return stopRequested || !incoming.empty();
                });
                if (stopRequested && incoming.empty()) break;
                if (!incoming.empty()) {
                    chunk = std::move(incoming.front());
                    incomingBytes -= chunk.data.size();
                    incoming.pop_front();
                    haveChunk = true;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> stateLock(serviceMutex);
                bool serviceSetChanged = false;
                for (auto& service : services) {
                    if (service.ready && service.lastData.time_since_epoch().count() != 0 &&
                        now - service.lastData >= kServiceStaleTimeout) {
                        service.ready = false;
                        serviceSetChanged = true;
                        std::cerr << "MPTS service stale: output=" << config.id
                                  << " stream=" << service.config.streamId
                                  << " timeout_ms="
                                  << std::chrono::duration_cast<std::chrono::milliseconds>(kServiceStaleTimeout).count()
                                  << std::endl;
                    }
                }
                if (serviceSetChanged) {
                    psiVersion = static_cast<uint8_t>((psiVersion + 1) & 0x1FU);
                    psiDirty = true;
                }
                if (psiDirty || lastPatPmt.time_since_epoch().count() == 0 || now - lastPatPmt >= kPatPmtInterval) {
                    emitPatPmt();
                }
                if (lastSdt.time_since_epoch().count() == 0 || now - lastSdt >= kSdtInterval) {
                    emitSdt();
                }
            }
            if (haveChunk) processChunk(std::move(chunk));
            if (!haveChunk) flushDatagram();
        }
        flushDatagram();
    }

    Json::Value snapshot() const {
        Json::Value root = config.toJson();
        root["active"] = running.load(std::memory_order_relaxed);
        root["bytes_out"] = Json::UInt64(bytesOut.load(std::memory_order_relaxed));
        root["packets_out"] = Json::UInt64(packetsOut.load(std::memory_order_relaxed));
        root["send_errors"] = Json::UInt64(sendErrors.load(std::memory_order_relaxed));
        root["dropped_input_bytes"] = Json::UInt64(droppedInputBytes.load(std::memory_order_relaxed));
        {
            std::lock_guard<std::mutex> lock(mutex);
            root["queue_bytes"] = Json::UInt64(incomingBytes);
        }
        {
            std::lock_guard<std::mutex> errorLock(errorMutex);
            root["last_error"] = lastError;
        }
        std::lock_guard<std::mutex> stateLock(serviceMutex);
        Json::Value runtimeServices(Json::arrayValue);
        for (const auto& service : services) {
            Json::Value item = service.config.toJson();
            item["name"] = service.name;
            item["provider"] = service.provider;
            item["service_id"] = service.outputServiceId;
            item["pmt_pid"] = service.outputPmtPid;
            item["ready"] = service.ready;
            item["source_service_id"] = service.sourceServiceId;
            item["source_pmt_pid"] = service.sourcePmtPid;
            item["input_packets"] = Json::UInt64(service.inputPackets);
            Json::Value mappings(Json::arrayValue);
            if (service.ready) {
                for (const auto& es : service.program.streams) {
                    Json::Value mapping;
                    mapping["stream_type"] = es.streamType;
                    mapping["source_pid"] = es.sourcePid;
                    mapping["output_pid"] = service.pidMap[es.sourcePid];
                    mappings.append(mapping);
                }
            }
            item["pid_map"] = mappings;
            runtimeServices.append(item);
        }
        root["runtime_services"] = runtimeServices;
        return root;
    }

    MptsOutputConfig config;
    std::vector<ServiceState> services;
    std::unordered_map<std::string, size_t> serviceIndex;

    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<InputChunk> incoming;
    size_t incomingBytes = 0;
    bool stopRequested = false;
    std::atomic<bool> running{false};
    std::thread worker;
    int socketFd = -1;
    sockaddr_in destinationAddress{};
    std::vector<uint8_t> sendAccumulator;
    uint8_t patCc = 0;
    uint8_t sdtCc = 0;
    uint8_t psiVersion = 0;
    bool psiDirty = true;
    std::chrono::steady_clock::time_point lastPatPmt{};
    std::chrono::steady_clock::time_point lastSdt{};
    std::atomic<uint64_t> bytesOut{0};
    std::atomic<uint64_t> packetsOut{0};
    std::atomic<uint64_t> sendErrors{0};
    std::atomic<uint64_t> droppedInputBytes{0};
    mutable std::mutex serviceMutex;
    mutable std::mutex errorMutex;
    std::string lastError;
};

MptsOutputManager::MptsOutputManager() = default;
MptsOutputManager::~MptsOutputManager() { stopAll(); }

void MptsOutputManager::configure(const std::vector<MptsOutputConfig>& outputs,
                                  const std::vector<StreamConfig>& streams) {
    std::unordered_map<std::string, std::shared_ptr<Runtime>> next;
    std::vector<std::shared_ptr<Runtime>> retired;
    std::vector<std::shared_ptr<Runtime>> autoStart;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& cfg : outputs) {
            if (cfg.id.empty() || cfg.services.empty()) continue;
            auto existing = outputs_.find(cfg.id);
            if (existing != outputs_.end() && existing->second->sameConfig(cfg)) {
                next[cfg.id] = existing->second;
                if (cfg.autoStart && !existing->second->running.load(std::memory_order_relaxed)) {
                    autoStart.push_back(existing->second);
                }
            } else {
                const bool preserveRunning = existing != outputs_.end() &&
                    existing->second->running.load(std::memory_order_relaxed);
                if (existing != outputs_.end()) retired.push_back(existing->second);
                auto runtime = std::make_shared<Runtime>(cfg, streams);
                next[cfg.id] = runtime;
                if (cfg.autoStart || preserveRunning) autoStart.push_back(runtime);
            }
        }
        for (const auto& [id, runtime] : outputs_) {
            if (next.find(id) == next.end()) retired.push_back(runtime);
        }
        outputs_ = next;
        targets_.clear();
        for (const auto& [id, runtime] : outputs_) {
            (void)id;
            for (const auto& service : runtime->config.services) {
                targets_[service.streamId].push_back(runtime);
            }
        }
    }

    for (auto& runtime : retired) runtime->stop();
    for (auto& runtime : autoStart) {
        std::string error;
        if (!runtime->start(error)) {
            std::cerr << "MPTS auto-start failed: id=" << runtime->config.id
                      << " error=" << error << std::endl;
        }
    }
}

bool MptsOutputManager::start(const std::string& id, std::string* error) {
    std::shared_ptr<Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = outputs_.find(id);
        if (found == outputs_.end()) {
            if (error) *error = "MPTS output not found: " + id;
            return false;
        }
        runtime = found->second;
    }
    std::string localError;
    const bool ok = runtime->start(localError);
    if (error) *error = localError;
    return ok;
}

bool MptsOutputManager::stop(const std::string& id) {
    std::shared_ptr<Runtime> runtime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = outputs_.find(id);
        if (found == outputs_.end()) return false;
        runtime = found->second;
    }
    runtime->stop();
    return true;
}

void MptsOutputManager::stopAll() {
    std::vector<std::shared_ptr<Runtime>> runtimes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, runtime] : outputs_) {
            (void)id;
            runtimes.push_back(runtime);
        }
    }
    for (auto& runtime : runtimes) runtime->stop();
}

void MptsOutputManager::pushBuffer(const std::string& streamId, GstBuffer* buffer) {
    if (!buffer || streamId.empty()) return;
    std::vector<std::shared_ptr<Runtime>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = targets_.find(streamId);
        if (found == targets_.end()) return;
        for (const auto& weak : found->second) {
            if (auto runtime = weak.lock()) targets.push_back(std::move(runtime));
        }
    }
    if (targets.empty()) return;

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    for (auto& runtime : targets) runtime->enqueue(streamId, map.data, map.size);
    gst_buffer_unmap(buffer, &map);
}

Json::Value MptsOutputManager::snapshot() const {
    Json::Value root;
    Json::Value outputs(Json::arrayValue);
    std::vector<std::shared_ptr<Runtime>> runtimes;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, runtime] : outputs_) {
            (void)id;
            runtimes.push_back(runtime);
        }
    }
    std::sort(runtimes.begin(), runtimes.end(), [](const auto& a, const auto& b) {
        return a->config.name < b->config.name;
    });
    for (const auto& runtime : runtimes) outputs.append(runtime->snapshot());
    root["outputs"] = outputs;
    return root;
}
