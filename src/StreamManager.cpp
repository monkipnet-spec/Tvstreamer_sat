#include "StreamManager.h"
#include "TranscoderModule.h"
#include "StableUdpOutput.h"
#include "TsCcStageTrace.h"
#include "UdpInput.h"
#include "DvbSatellite.h"
#include "CardManager.h"
#include "CaBackend.h"
#include "protocols/GstProtocolTypes.h"
#include "protocols/stream/StreamInputProtocol.h"
#include "protocols/stream/StreamOutputProtocol.h"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <functional>
#include <thread>
#include <utility>
#include <vector>

#include <glib.h>
#include <gio/gio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <curl/curl.h>
#define GST_USE_UNSTABLE_API
#include <gst/mpegts/mpegts.h>
#include <gst/app/gstappsrc.h>

namespace {

constexpr guint kTsPacketSize = 188;
constexpr guint kTsPacketsPerUdpBuffer = 7;
constexpr guint64 kUdpQueueLatency = 10 * GST_SECOND;
constexpr guint64 kStableUdpAudioReservoir = 1500 * GST_MSECOND;
constexpr guint64 kStableUdpAudioReservoirMax = 3 * GST_SECOND;
constexpr auto kInputFailoverDelay = std::chrono::seconds(5);
constexpr auto kPrimaryRetryInterval = std::chrono::seconds(10);
constexpr auto kHlsSessionTtl = std::chrono::seconds(15);
constexpr int kSrtRestartAttempts = 4;
constexpr auto kSrtRestartRetryDelay = std::chrono::milliseconds(250);
constexpr GstClockTime kFastDvbReleaseWait = 250 * GST_MSECOND;
constexpr const char* kTestPatternUri = "test://bars";

bool dvbDiagnosticsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TVS_DVB_DIAGNOSTICS");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

class CardReservationGuard {
public:
    CardReservationGuard(const StreamConfig& config, std::string* error)
        : streamId_(config.id), managed_(!config.conditionalAccessClient.empty()) {
        if (!managed_) {
            reserved_ = true;
            return;
        }
        reserved_ = CardManager::instance().reserveService(config, error);
    }

    ~CardReservationGuard() {
        if (managed_ && reserved_ && !committed_) {
            CardManager::instance().releaseService(streamId_);
        }
    }

    bool ok() const { return reserved_; }

    void commit() {
        if (managed_ && reserved_) CardManager::instance().activateService(streamId_);
        committed_ = true;
    }

private:
    std::string streamId_;
    bool managed_ = false;
    bool reserved_ = false;
    bool committed_ = false;
};

bool hasElementFactory(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

std::string missingElementStatus(const std::string& elementName) {
    return "missing element: " + elementName;
}

bool isHlsUri(const std::string& inputLower, const std::string& inputMode) {
    return inputLower.rfind("hls://", 0) == 0 ||
           toLower(inputMode) == "hls" ||
           inputLower.find(".m3u8") != std::string::npos;
}

bool isDirectHttpMpegTsConfig(const StreamConfig& cfg) {
    const std::string input = toLower(cfg.inputUri);
    if (input.rfind("http://", 0) != 0 && input.rfind("https://", 0) != 0) return false;
    const std::string mode = toLower(cfg.inputMode);
    if (mode == "hls") return false;
    if (mode == "http-ts" || mode == "http-mpegts" || mode == "mpegts") return true;
    // v187: Auto is deterministic and single-request for opaque HTTP IPTV URLs.
    // HLS with .m3u8 remains automatic; an HLS URL without an extension must be
    // explicitly marked HLS. This avoids consuming tokenized MPEG-TS sessions
    // with a separate sniff request before the real stream connection.
    return (mode.empty() || mode == "auto") && input.find(".m3u8") == std::string::npos;
}

bool addElementOrFail(GstElement* pipeline, GstElement* element) {
    return element != nullptr && gst_bin_add(GST_BIN(pipeline), element);
}

void drainDynamicPad(GstElement* anchor, GstPad* pad) {
    if (!anchor || !pad || gst_pad_is_linked(pad)) return;
    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(anchor));
    if (!pipeline) return;
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (!queue || !sink || !gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), sink) || !gst_element_link(queue, sink)) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
        gst_object_unref(pipeline);
        return;
    }
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    GstPad* queueSink = gst_element_get_static_pad(queue, "sink");
    if (queueSink) {
        gst_pad_link(pad, queueSink);
        gst_object_unref(queueSink);
    }
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(sink);
    gst_object_unref(pipeline);
}

std::string socketAddressToString(GSocketAddress* address) {
    if (!address) {
        return {};
    }

    auto* inetAddress = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address));
    if (!inetAddress) {
        return {};
    }

    gchar* raw = g_inet_address_to_string(inetAddress);
    if (!raw) {
        return {};
    }
    std::string result = normalizeIpAddress(raw);
    g_free(raw);
    return result;
}

struct SrtAccessContext {
    StreamManager* manager = nullptr;
    std::string streamId;
};

void freeSrtAccessContext(gpointer data) {
    delete static_cast<SrtAccessContext*>(data);
}

// DVB-S/S2 service PID filtering in dvbsrc removes the PES/PCR packets of
// unrelated services, but Linux DVB demux still returns the original PAT/SDT
// tables from the complete transponder.  Players such as VLC therefore see
// every service advertised by the original PAT even though only one service's
// media PIDs are present.  Keep the selected media packets byte-for-byte and
// rewrite PAT/SDT to advertise only the selected service.
// This avoids another tsdemux/mpegtsmux cycle, which previously broke valid
// FTA/private streams, while producing a standards-compliant single-program TS.
struct DvbSingleProgramPsiContext {
    // serviceId is the service selected on the input transponder.  When
    // remapEnabled is true the output can advertise a different SID/PIDs,
    // while the elementary payload itself is kept byte-for-byte.
    uint16_t serviceId = 0;
    uint16_t pmtPid = 0x1FFF;
    uint16_t transportStreamId = 1;
    uint16_t originalNetworkId = 1;
    uint8_t patVersion = 0;
    uint8_t sdtVersion = 0;
    std::string serviceName;
    std::string serviceProvider;
    bool remapEnabled = false;
    uint16_t outputServiceId = 0;
    uint16_t requestedVideoPid = 0;
    uint16_t requestedAudioPid = 0;
    uint16_t inputVideoPid = 0;
    uint16_t inputAudioPid = 0;
    bool remapPmtRewritten = false;
    bool remapAnnounced = false;
    // When the physical DVB frontend is shared, every service sees the full
    // transponder. Compact the buffer to the PID set discovered by the channel
    // scan instead of remuxing with tsdemux/mpegtsmux. This preserves the
    // working v122+ media path and keeps VBR at the service bitrate.
    std::array<bool, 8192> allowedPids {};
    bool filterPids = false;
    bool announced = false;
    bool pidSelfHealAnnounced = false;
    bool pidFilterWarmupAnnounced = false;
    std::vector<uint8_t> patSectionBuffer;
    size_t patSectionExpected = 0;
    std::vector<uint8_t> pmtSectionBuffer;
    size_t pmtSectionExpected = 0;

    // Remapping creates a new logical SPTS. Regenerate continuity counters
    // on final output PIDs instead of inheriting unrelated MPTS sequences.
    std::array<uint8_t, 8192> remapContinuity {};
    std::array<bool, 8192> remapContinuityValid {};
    bool remapContinuityAnnounced = false;
};

// Final continuity guard for DVB packet-level remap. The service relay already
// regenerates CC after PID/SID rewriting, but the selected SPTS still crosses
// the localhost relay, optional CA backend and the main pipeline before it
// reaches StableUdpOutput. Repair the final transport sequence immediately
// before the output sink.
const uint8_t* tsSectionStart(const uint8_t* packet, size_t& available);
void writeSingleProgramSdt(uint8_t* packet, const DvbSingleProgramPsiContext& ctx);

struct DvbRemapFinalPsiContext {
    uint16_t serviceId = 0;
    std::string serviceName;
    std::string serviceProvider;
    bool announced = false;
};

GstPadProbeReturn dvbRemapFinalPsiProbe(
    GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* ctx = static_cast<DvbRemapFinalPsiContext*>(userData);
    if (!ctx || ctx->serviceId == 0 || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer* original = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!original) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = gst_buffer_make_writable(original);
    if (!buffer) return GST_PAD_PROBE_OK;
    if (buffer != original) GST_PAD_PROBE_INFO_DATA(info) = buffer;

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) return GST_PAD_PROBE_OK;

    size_t start = 0;
    while (start < map.size && map.data[start] != 0x47) ++start;
    for (size_t offset = start; offset + kTsPacketSize <= map.size; offset += kTsPacketSize) {
        uint8_t* packet = map.data + offset;
        if (packet[0] != 0x47) break;
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        if (pid != 0x0011) continue;

        DvbSingleProgramPsiContext out;
        out.serviceId = ctx->serviceId;
        out.remapEnabled = true;
        out.outputServiceId = ctx->serviceId;
        out.remapPmtRewritten = true;
        out.serviceName = ctx->serviceName;
        out.serviceProvider = ctx->serviceProvider;

        size_t available = 0;
        const uint8_t* section = tsSectionStart(packet, available);
        if (section && available >= 11 && (section[0] == 0x42 || section[0] == 0x46)) {
            out.transportStreamId = static_cast<uint16_t>((section[3] << 8) | section[4]);
            out.sdtVersion = static_cast<uint8_t>((section[5] >> 1) & 0x1F);
            out.originalNetworkId = static_cast<uint16_t>((section[8] << 8) | section[9]);
        }
        writeSingleProgramSdt(packet, out);
        if (!ctx->announced) {
            std::cerr << "DVB final SDT remap: SID=" << ctx->serviceId
                      << " service=\"" << ctx->serviceName << "\""
                      << " provider=\"" << ctx->serviceProvider << "\""
                      << " stage=pre-output-sink" << std::endl;
            ctx->announced = true;
        }
    }
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
}

struct DvbRemapFinalContinuityContext {
    std::string streamId;
    size_t branchIndex = 0;
    std::array<uint8_t, 8192> continuity {};
    std::array<bool, 8192> continuityValid {};
    uint64_t repairedPackets = 0;
    uint64_t invalidPackets = 0;
    bool announced = false;
};

GstPadProbeReturn dvbRemapFinalContinuityProbe(
    GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* ctx = static_cast<DvbRemapFinalContinuityContext*>(userData);
    if (!ctx || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    GstBuffer* original = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!original) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = gst_buffer_make_writable(original);
    if (!buffer) return GST_PAD_PROBE_OK;
    if (buffer != original) GST_PAD_PROBE_INFO_DATA(info) = buffer;
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) return GST_PAD_PROBE_OK;

    const size_t packetCount = map.size / kTsPacketSize;
    for (size_t index = 0; index < packetCount; ++index) {
        uint8_t* packet = map.data + index * kTsPacketSize;
        if (packet[0] != 0x47) { ++ctx->invalidPackets; continue; }
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        if (pid >= 0x1FFF) continue;
        const uint8_t adaptationControl = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
        if (adaptationControl == 0) { ++ctx->invalidPackets; continue; }
        const bool hasPayload = adaptationControl == 1 || adaptationControl == 3;
        const uint8_t incomingCc = static_cast<uint8_t>(packet[3] & 0x0F);
        uint8_t outputCc = incomingCc;
        if (hasPayload) {
            if (ctx->continuityValid[pid]) outputCc = static_cast<uint8_t>((ctx->continuity[pid] + 1) & 0x0F);
            ctx->continuity[pid] = outputCc;
            ctx->continuityValid[pid] = true;
        } else if (ctx->continuityValid[pid]) {
            outputCc = ctx->continuity[pid];
        } else {
            ctx->continuity[pid] = outputCc;
            ctx->continuityValid[pid] = true;
        }
        if (outputCc != incomingCc) ++ctx->repairedPackets;
        packet[3] = static_cast<uint8_t>((packet[3] & 0xF0) | (outputCc & 0x0F));
    }
    gst_buffer_unmap(buffer, &map);
    if (!ctx->announced) {
        std::cerr << "DVB remap final continuity guard: stream=" << ctx->streamId
                  << " branch=" << ctx->branchIndex
                  << " stage=pre-output-sink all-pids=normalized"
                  << " payload-aware adaptation-only=no-increment" << std::endl;
        ctx->announced = true;
    }
    return GST_PAD_PROBE_OK;
}


// Shared DVB UDP is a byte stream from the application's point of view: a
// GstBuffer may end in the middle of a 188-byte MPEG-TS packet.  Keep the tail
// and prepend it to the next buffer before any PID/PSI filtering.  Unlike
// tsparse this helper does not parse programs, drop packets, alter timestamps,
// rewrite CC/PCR, or touch payload bytes; it only restores packet boundaries.
struct SharedDvbTsFramerContext {
    std::string streamId;
    std::vector<uint8_t> remainder;
    uint64_t buffers = 0;
    uint64_t framedPackets = 0;
    uint64_t droppedPrefixBytes = 0;
    uint64_t resyncs = 0;
    bool announced = false;
};

bool sharedDvbLooksAligned(const uint8_t* data, size_t size, size_t candidate) {
    if (!data || candidate >= size || data[candidate] != 0x47) return false;
    // Validate up to two following packet boundaries when they are present.
    for (size_t step = 1; step <= 2; ++step) {
        const size_t next = candidate + step * kTsPacketSize;
        if (next >= size) break;
        if (data[next] != 0x47) return false;
    }
    return true;
}

// v155 diagnostic-only byte audit for the shared DVB ingest path.  It never
// changes a GstBuffer.  The source and pre-UDP probes produce cumulative exact
// 188-byte packet fingerprints at the same packet counts.  The receiver probe
// additionally correlates packets against a bounded source history, so payload
// changes or packet loss across the internal loopback UDP hop can be separated
// from corruption already present at dvbsrc output.
constexpr size_t kDvbByteAuditHistoryLimit = 65536;
constexpr uint64_t kDvbByteAuditLogIntervalPackets = 10000;
constexpr uint64_t kDvbByteAuditFNVOffset = 1469598103934665603ULL;
constexpr uint64_t kDvbByteAuditFNVPrime = 1099511628211ULL;

struct DvbByteAuditPacket {
    uint64_t seq = 0;
    uint64_t fingerprint = 0;
    uint16_t pid = 0;
    uint8_t cc = 0;
};

struct DvbByteAuditHistory {
    uint64_t generation = 0;
    uint64_t nextSeq = 1;
    std::deque<DvbByteAuditPacket> packets;
};

std::mutex gDvbByteAuditMutex;
std::map<std::string, DvbByteAuditHistory> gDvbByteAuditHistories;

uint64_t dvbByteAuditHash(const uint8_t* data, size_t size) {
    uint64_t hash = kDvbByteAuditFNVOffset;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(data[i]);
        hash *= kDvbByteAuditFNVPrime;
    }
    return hash;
}

uint64_t dvbByteAuditRoll(uint64_t current, uint64_t packetFingerprint) {
    // Fold the exact packet fingerprint into an order-sensitive cumulative
    // digest. Source and pre-internal-UDP stages must have identical values at
    // every equal packet count when no bytes were modified in the pipeline.
    current ^= packetFingerprint;
    current *= kDvbByteAuditFNVPrime;
    return current;
}

struct DvbByteAuditContext {
    std::string frontendKey;
    std::string streamId;
    std::string stage;
    std::vector<uint8_t> remainder;
    bool publishSourceHistory = false;
    bool compareWithSourceHistory = false;
    bool sourceHistoryStarted = false;
    bool receiverAligned = false;
    bool receiverPending = false;
    uint64_t pendingFingerprint = 0;
    uint16_t pendingPid = 0;
    uint8_t pendingCc = 0;
    uint64_t sourceGeneration = 0;
    uint64_t expectedSourceSeq = 0;
    uint64_t packets = 0;
    uint64_t rollingHash = kDvbByteAuditFNVOffset;
    uint64_t unalignedBuffers = 0;
    uint64_t alignmentMisses = 0;
    uint64_t resyncBytes = 0;
    uint64_t matched = 0;
    uint64_t payloadMismatches = 0;
    uint64_t sourceSkippedPackets = 0;
    uint64_t receiverDuplicates = 0;
    uint64_t receiverUnmatched = 0;
    uint64_t receiverRealigns = 0;
    uint64_t nextLogPacket = kDvbByteAuditLogIntervalPackets;
};

void dvbByteAuditStartSourceHistory(DvbByteAuditContext* ctx) {
    if (!ctx || !ctx->publishSourceHistory || ctx->sourceHistoryStarted) return;
    std::lock_guard<std::mutex> lock(gDvbByteAuditMutex);
    auto& history = gDvbByteAuditHistories[ctx->frontendKey];
    ++history.generation;
    history.nextSeq = 1;
    history.packets.clear();
    ctx->sourceGeneration = history.generation;
    ctx->sourceHistoryStarted = true;
}

void dvbByteAuditPublishSourcePacket(
    DvbByteAuditContext* ctx,
    uint64_t fingerprint,
    uint16_t pid,
    uint8_t cc) {
    if (!ctx || !ctx->publishSourceHistory) return;
    dvbByteAuditStartSourceHistory(ctx);
    std::lock_guard<std::mutex> lock(gDvbByteAuditMutex);
    auto it = gDvbByteAuditHistories.find(ctx->frontendKey);
    if (it == gDvbByteAuditHistories.end()) return;
    auto& history = it->second;
    if (history.generation != ctx->sourceGeneration) return;
    history.packets.push_back(DvbByteAuditPacket{
        history.nextSeq++, fingerprint, pid, cc});
    while (history.packets.size() > kDvbByteAuditHistoryLimit) {
        history.packets.pop_front();
    }
}

bool dvbByteAuditPacketEquals(
    const DvbByteAuditPacket& source,
    uint64_t fingerprint,
    uint16_t pid,
    uint8_t cc) {
    return source.fingerprint == fingerprint && source.pid == pid && source.cc == cc;
}

void dvbByteAuditCompareReceiverPacket(
    DvbByteAuditContext* ctx,
    uint64_t fingerprint,
    uint16_t pid,
    uint8_t cc) {
    if (!ctx || !ctx->compareWithSourceHistory) return;
    std::lock_guard<std::mutex> lock(gDvbByteAuditMutex);
    auto it = gDvbByteAuditHistories.find(ctx->frontendKey);
    if (it == gDvbByteAuditHistories.end() || it->second.packets.empty()) {
        ++ctx->receiverUnmatched;
        ctx->receiverAligned = false;
        ctx->receiverPending = false;
        return;
    }
    const auto& history = it->second;
    if (ctx->sourceGeneration != history.generation) {
        ctx->sourceGeneration = history.generation;
        ctx->receiverAligned = false;
        ctx->receiverPending = false;
        ctx->expectedSourceSeq = 0;
    }

    const auto findMatchingSeq = [&](uint64_t minSeq, uint64_t maxSeq) -> uint64_t {
        for (auto rit = history.packets.rbegin(); rit != history.packets.rend(); ++rit) {
            if (rit->seq < minSeq) break;
            if (rit->seq > maxSeq) continue;
            if (dvbByteAuditPacketEquals(*rit, fingerprint, pid, cc)) return rit->seq;
        }
        return 0;
    };

    if (!ctx->receiverAligned) {
        // Use two consecutive packets to establish the source sequence. This
        // avoids false alignment on frequently repeated PAT/CAT/SI packets.
        if (!ctx->receiverPending) {
            ctx->receiverPending = true;
            ctx->pendingFingerprint = fingerprint;
            ctx->pendingPid = pid;
            ctx->pendingCc = cc;
            return;
        }

        const uint64_t historyBack = history.packets.back().seq;
        const uint64_t searchMin = historyBack > 16384
            ? historyBack - 16384 : history.packets.front().seq;
        uint64_t currentSeq = 0;
        for (auto rit = history.packets.rbegin(); rit != history.packets.rend(); ++rit) {
            if (rit->seq < searchMin) break;
            if (!dvbByteAuditPacketEquals(*rit, fingerprint, pid, cc)) continue;
            if (rit->seq <= history.packets.front().seq) continue;
            const size_t previousIndex = static_cast<size_t>(
                (rit->seq - 1) - history.packets.front().seq);
            if (previousIndex >= history.packets.size()) continue;
            const auto& previous = history.packets[previousIndex];
            if (dvbByteAuditPacketEquals(
                    previous,
                    ctx->pendingFingerprint,
                    ctx->pendingPid,
                    ctx->pendingCc)) {
                currentSeq = rit->seq;
                break;
            }
        }
        if (currentSeq != 0) {
            ctx->receiverAligned = true;
            ctx->receiverPending = false;
            ctx->expectedSourceSeq = currentSeq + 1;
            ctx->matched += 2;
            ++ctx->receiverRealigns;
            return;
        }

        ++ctx->receiverUnmatched;
        ctx->pendingFingerprint = fingerprint;
        ctx->pendingPid = pid;
        ctx->pendingCc = cc;
        return;
    }

    if (ctx->expectedSourceSeq < history.packets.front().seq) {
        ++ctx->receiverUnmatched;
        ctx->receiverAligned = false;
        ctx->receiverPending = false;
        return;
    }
    if (ctx->expectedSourceSeq > history.packets.back().seq) {
        // The receiver should normally trail the source by the queue/socket
        // latency. If scheduling briefly reverses that order, retry alignment on
        // the next packet rather than classifying it as payload corruption.
        ++ctx->receiverUnmatched;
        ctx->receiverAligned = false;
        ctx->receiverPending = false;
        return;
    }

    const size_t expectedIndex = static_cast<size_t>(
        ctx->expectedSourceSeq - history.packets.front().seq);
    if (expectedIndex < history.packets.size() &&
        dvbByteAuditPacketEquals(history.packets[expectedIndex], fingerprint, pid, cc)) {
        ++ctx->matched;
        ++ctx->expectedSourceSeq;
        return;
    }

    // First look forward a short distance. A match means source packets were
    // absent at the receiver, not that the received packet payload changed.
    const uint64_t forwardMax = std::min<uint64_t>(
        history.packets.back().seq, ctx->expectedSourceSeq + 64);
    const uint64_t forwardSeq = findMatchingSeq(ctx->expectedSourceSeq + 1, forwardMax);
    if (forwardSeq != 0) {
        ctx->sourceSkippedPackets += forwardSeq - ctx->expectedSourceSeq;
        ++ctx->matched;
        ctx->expectedSourceSeq = forwardSeq + 1;
        return;
    }

    // A packet identical to one of the immediately preceding source packets is
    // a receiver-side duplicate. Keep it separate from byte mismatches.
    const uint64_t backwardMin = ctx->expectedSourceSeq > 8
        ? ctx->expectedSourceSeq - 8 : history.packets.front().seq;
    const uint64_t backwardMax = ctx->expectedSourceSeq > 0
        ? ctx->expectedSourceSeq - 1 : 0;
    if (backwardMax >= backwardMin &&
        findMatchingSeq(backwardMin, backwardMax) != 0) {
        ++ctx->receiverDuplicates;
        return;
    }

    // No exact source packet exists around the expected sequence: the packet
    // bytes differ. Advance one source slot so a single damaged packet does not
    // turn the rest of the stream into a cascade of false mismatches.
    ++ctx->payloadMismatches;
    if (ctx->payloadMismatches <= 8 && expectedIndex < history.packets.size()) {
        const auto& expected = history.packets[expectedIndex];
        std::cerr << "DVB BYTE AUDIT MISMATCH: frontend=" << ctx->frontendKey
                  << " stream=" << ctx->streamId
                  << " source_seq=" << ctx->expectedSourceSeq
                  << " source_pid=" << expected.pid
                  << " recv_pid=" << pid
                  << " source_cc=" << static_cast<unsigned int>(expected.cc)
                  << " recv_cc=" << static_cast<unsigned int>(cc)
                  << " source_hash=0x" << std::hex << expected.fingerprint
                  << " recv_hash=0x" << fingerprint << std::dec
                  << std::endl;
    }
    ++ctx->expectedSourceSeq;
}

void dvbByteAuditLog(const DvbByteAuditContext& ctx) {
    std::ostringstream line;
    line << "DVB BYTE AUDIT: stage=" << ctx.stage
         << " frontend=" << ctx.frontendKey;
    if (!ctx.streamId.empty()) line << " stream=" << ctx.streamId;
    line << " packets=" << ctx.packets
         << " hash=0x" << std::hex << ctx.rollingHash << std::dec
         << " remainder=" << ctx.remainder.size()
         << " unaligned_buffers=" << ctx.unalignedBuffers
         << " alignment_misses=" << ctx.alignmentMisses
         << " resync_bytes=" << ctx.resyncBytes;
    if (ctx.compareWithSourceHistory) {
        line << " matched=" << ctx.matched
             << " payload_mismatches=" << ctx.payloadMismatches
             << " source_skipped=" << ctx.sourceSkippedPackets
             << " receiver_duplicates=" << ctx.receiverDuplicates
             << " unmatched=" << ctx.receiverUnmatched
             << " realigns=" << ctx.receiverRealigns
             << " source_generation=" << ctx.sourceGeneration
             << " expected_source_seq=" << ctx.expectedSourceSeq;
    } else if (ctx.publishSourceHistory) {
        line << " source_generation=" << ctx.sourceGeneration;
    }
    std::cerr << line.str() << std::endl;
}

void dvbByteAuditProcessPacket(DvbByteAuditContext* ctx, const uint8_t* packet) {
    if (!ctx || !packet || packet[0] != 0x47) return;
    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    const uint8_t cc = static_cast<uint8_t>(packet[3] & 0x0F);
    const uint64_t fingerprint = dvbByteAuditHash(packet, kTsPacketSize);
    ctx->rollingHash = dvbByteAuditRoll(ctx->rollingHash, fingerprint);
    ++ctx->packets;
    if (ctx->publishSourceHistory) {
        dvbByteAuditPublishSourcePacket(ctx, fingerprint, pid, cc);
    }
    if (ctx->compareWithSourceHistory) {
        dvbByteAuditCompareReceiverPacket(ctx, fingerprint, pid, cc);
    }
    if (ctx->packets >= ctx->nextLogPacket) {
        dvbByteAuditLog(*ctx);
        while (ctx->packets >= ctx->nextLogPacket) {
            ctx->nextLogPacket += kDvbByteAuditLogIntervalPackets;
        }
    }
}

GstPadProbeReturn dvbByteAuditProbe(
    GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* ctx = static_cast<DvbByteAuditContext*>(userData);
    if (!ctx || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
    if ((map.size % kTsPacketSize) != 0) ++ctx->unalignedBuffers;

    std::vector<uint8_t> bytes;
    bytes.reserve(ctx->remainder.size() + map.size);
    bytes.insert(bytes.end(), ctx->remainder.begin(), ctx->remainder.end());
    bytes.insert(bytes.end(), map.data, map.data + map.size);
    ctx->remainder.clear();
    gst_buffer_unmap(buffer, &map);

    size_t start = std::string::npos;
    const size_t maxOffset = std::min<size_t>(kTsPacketSize, bytes.size());
    for (size_t candidate = 0; candidate < maxOffset; ++candidate) {
        if (sharedDvbLooksAligned(bytes.data(), bytes.size(), candidate)) {
            start = candidate;
            break;
        }
    }
    if (start == std::string::npos) {
        ++ctx->alignmentMisses;
        const size_t keep = std::min<size_t>(bytes.size(), kTsPacketSize * 2 - 1);
        if (keep > 0) ctx->remainder.assign(bytes.end() - keep, bytes.end());
        return GST_PAD_PROBE_OK;
    }
    if (start > 0) ctx->resyncBytes += start;

    size_t offset = start;
    for (; offset + kTsPacketSize <= bytes.size(); offset += kTsPacketSize) {
        const uint8_t* packet = bytes.data() + offset;
        if (packet[0] != 0x47) {
            ++ctx->alignmentMisses;
            break;
        }
        dvbByteAuditProcessPacket(ctx, packet);
    }
    if (offset < bytes.size()) {
        ctx->remainder.assign(bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());
    }
    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn sharedDvbTsFramerProbe(
    GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* ctx = static_cast<SharedDvbTsFramerContext*>(userData);
    if (!ctx || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;

    GstBuffer* original = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!original) return GST_PAD_PROBE_OK;

    GstBuffer* buffer = gst_buffer_make_writable(original);
    if (!buffer) return GST_PAD_PROBE_OK;
    if (buffer != original) GST_PAD_PROBE_INFO_DATA(info) = buffer;
    ++ctx->buffers;

    if (!ctx->remainder.empty()) {
        const gsize prefixSize = static_cast<gsize>(ctx->remainder.size());
        gpointer prefixData = g_malloc(prefixSize);
        if (!prefixData) return GST_PAD_PROBE_OK;
        std::memcpy(prefixData, ctx->remainder.data(), prefixSize);
        GstMemory* prefixMemory = gst_memory_new_wrapped(
            static_cast<GstMemoryFlags>(0),
            prefixData,
            prefixSize,
            0,
            prefixSize,
            prefixData,
            g_free);
        if (!prefixMemory) {
            g_free(prefixData);
            return GST_PAD_PROBE_OK;
        }
        gst_buffer_prepend_memory(buffer, prefixMemory);
        ctx->remainder.clear();
    }

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

    size_t packetStart = std::string::npos;
    if (map.size > 0 && sharedDvbLooksAligned(map.data, map.size, 0)) {
        packetStart = 0;
    } else {
        const size_t searchLimit = std::min<size_t>(map.size, kTsPacketSize);
        for (size_t candidate = 1; candidate < searchLimit; ++candidate) {
            if (sharedDvbLooksAligned(map.data, map.size, candidate)) {
                packetStart = candidate;
                break;
            }
        }
    }

    if (packetStart == std::string::npos) {
        // Not enough bytes to establish a boundary, or a transient damaged
        // fragment.  Preserve at most two packet lengths so the next buffer can
        // establish alignment without unbounded growth.
        const size_t keep = std::min<size_t>(map.size, kTsPacketSize * 2 - 1);
        ctx->remainder.assign(map.data + (map.size - keep), map.data + map.size);
        gst_buffer_unmap(buffer, &map);
        return GST_PAD_PROBE_DROP;
    }

    if (packetStart > 0) {
        ctx->droppedPrefixBytes += packetStart;
        ++ctx->resyncs;
    }

    size_t framedBytes = 0;
    size_t pos = packetStart;
    while (pos + kTsPacketSize <= map.size) {
        if (map.data[pos] != 0x47) break;
        framedBytes += kTsPacketSize;
        pos += kTsPacketSize;
    }

    const size_t tailStart = packetStart + framedBytes;
    if (tailStart < map.size) {
        ctx->remainder.assign(map.data + tailStart, map.data + map.size);
    } else {
        ctx->remainder.clear();
    }
    gst_buffer_unmap(buffer, &map);

    if (framedBytes == 0) return GST_PAD_PROBE_DROP;

    // The buffer now contains only complete TS packets.  gst_buffer_resize()
    // trims the optional resync prefix and the saved partial tail without
    // copying or changing any packet bytes.
    gst_buffer_resize(
        buffer,
        static_cast<gssize>(packetStart),
        static_cast<gssize>(framedBytes));
    ctx->framedPackets += framedBytes / kTsPacketSize;

    if (!ctx->announced) {
        std::cerr << "DVB service relay TS framer active: stream=" << ctx->streamId
                  << " packet_size=188 mode=persistent-remainder"
                  << " parser=none payload=passthrough" << std::endl;
        ctx->announced = true;
    }
    return GST_PAD_PROBE_OK;
}

struct SharedDvbPidStatsContext {
    std::string stage = "dvbsrc-src";
    std::string label;
    std::array<uint64_t, 8192> packets {};
    uint64_t totalPackets = 0;
    std::vector<uint8_t> remainder;
    std::chrono::steady_clock::time_point windowStarted =
        std::chrono::steady_clock::now();
};

GstPadProbeReturn sharedDvbPidStatsProbe(
    GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* ctx = static_cast<SharedDvbPidStatsContext*>(userData);
    if (!ctx || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
    std::vector<uint8_t> bytes;
    bytes.reserve(ctx->remainder.size() + map.size);
    bytes.insert(bytes.end(), ctx->remainder.begin(), ctx->remainder.end());
    bytes.insert(bytes.end(), map.data, map.data + map.size);
    ctx->remainder.clear();
    gst_buffer_unmap(buffer, &map);

    size_t start = std::string::npos;
    const size_t maxOffset = std::min<size_t>(kTsPacketSize, bytes.size());
    for (size_t candidate = 0; candidate < maxOffset; ++candidate) {
        if (bytes[candidate] == 0x47 &&
            candidate + kTsPacketSize < bytes.size() &&
            bytes[candidate + kTsPacketSize] == 0x47) {
            start = candidate;
            break;
        }
    }
    if (start == std::string::npos) {
        const size_t keep = std::min<size_t>(bytes.size(), kTsPacketSize * 2 - 1);
        ctx->remainder.assign(bytes.end() - keep, bytes.end());
        return GST_PAD_PROBE_OK;
    }

    size_t offset = start;
    for (; offset + kTsPacketSize <= bytes.size(); offset += kTsPacketSize) {
        const uint8_t* packet = bytes.data() + offset;
        if (packet[0] != 0x47) break;
        const uint16_t pid = static_cast<uint16_t>(
            ((packet[1] & 0x1F) << 8) | packet[2]);
        ++ctx->packets[pid];
        ++ctx->totalPackets;
    }
    ctx->remainder.assign(
        bytes.begin() + static_cast<std::ptrdiff_t>(offset), bytes.end());

    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(
        now - ctx->windowStarted).count();
    if (seconds < 10.0) return GST_PAD_PROBE_OK;

    const uint64_t totalKbps = static_cast<uint64_t>(
        ctx->totalPackets * kTsPacketSize * 8.0 / seconds / 1000.0);
    std::ostringstream present;
    bool first = true;
    for (size_t pid = 0; pid < ctx->packets.size(); ++pid) {
        if (ctx->packets[pid] == 0) continue;
        if (!first) present << ',';
        first = false;
        const uint64_t kbps = static_cast<uint64_t>(
            ctx->packets[pid] * kTsPacketSize * 8.0 / seconds / 1000.0);
        present << pid << '=' << kbps;
    }
    std::cerr << "Shared DVB PID stats: stage=" << ctx->stage
              << " label=" << ctx->label
              << " total=" << totalKbps << "kbps"
              << " present=" << (first ? "none" : present.str()) << std::endl;
    ctx->packets.fill(0);
    ctx->totalPackets = 0;
    ctx->windowStarted = now;
    return GST_PAD_PROBE_OK;
}
std::string sharedDvbFrontendKey(const DvbSatelliteParams& params) {
    return std::to_string(params.adapter) + ":" + std::to_string(params.frontend);
}

std::string sharedDvbTuneSignature(const DvbSatelliteParams& p) {
    std::ostringstream out;
    out << p.adapter << ':' << p.frontend
        << ':' << p.frequencyKHz << ':' << p.symbolRateK
        << ':' << p.polarity << ':' << p.deliverySystem
        << ':' << p.modulation << ':' << p.fec
        << ':' << p.diseqcSource << ':' << p.lnbLof1KHz
        << ':' << p.lnbLof2KHz << ':' << p.lnbSlofKHz
        << ':' << p.streamId;
    return out.str();
}

bool addExplicitDvbPids(std::set<uint16_t>& target, const std::string& pids) {
    if (pids.empty() || pids == "8192") return false;
    bool added = false;
    size_t start = 0;
    while (start <= pids.size()) {
        const size_t end = pids.find(':', start);
        const std::string token = pids.substr(
            start, end == std::string::npos ? std::string::npos : end - start);
        if (!token.empty()) {
            try {
                const unsigned long value = std::stoul(token);
                if (value < 8192) {
                    target.insert(static_cast<uint16_t>(value));
                    added = true;
                }
            } catch (...) {
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return added;
}

std::string formatDvbPids(const std::set<uint16_t>& pids) {
    std::ostringstream out;
    bool first = true;
    const auto append = [&](uint16_t pid) {
        // dvbsrc always reserves filters 0 and 1 for PAT and CAT.
        if (pid <= 1) return;
        if (!first) out << ':';
        first = false;
        out << pid;
    };
    for (uint16_t pid : pids) {
        // Keep service media and CA filters ahead of optional DVB SI. Some
        // adapters expose fewer hardware demux slots than dvbsrc's limit.
        if (pid == 0x0011) continue;
        append(pid);
    }
    if (pids.count(0x0011)) append(0x0011);
    return out.str();
}

std::set<uint16_t> requestedDvbPids(const DvbSatelliteParams& requested) {
    std::set<uint16_t> pids;
    if (!addExplicitDvbPids(pids, requested.pids)) return pids;
    // PAT/CAT are added by dvbsrc. SDT is retained for service metadata; NIT,
    // EIT and TDT are not needed by the single-program relay and waste scarce
    // hardware demux filters before PCR/video/audio on some DVB adapters.
    pids.erase(0x0010);
    pids.erase(0x0012);
    pids.erase(0x0014);
    for (uint16_t pid : {uint16_t(0x0000), uint16_t(0x0001), uint16_t(0x0011)}) {
        pids.insert(pid);
    }
    return pids;
}

bool buildSharedDvbPids(
    const std::map<std::string, std::set<uint16_t>>& consumers,
    std::string& requestedPids,
    size_t& filterCount,
    std::string& error,
    bool forceFullTransportStream = false) {
    // v167: a shared DVB frontend always captures the complete transponder.
    // Individual services are filtered in software by their service-relay
    // pipelines. This mirrors Astra's full-MPTS acquisition model and avoids
    // dvbsrc's finite per-PID filter budget as more services are enabled.
    (void)consumers;
    (void)error;
    (void)forceFullTransportStream;
    requestedPids = "8192";
    filterCount = 1;
    return true;
}

bool applySharedDvbPids(
    SharedDvbFrontendState& shared,
    const std::map<std::string, std::set<uint16_t>>& consumers,
    size_t& filterCount,
    std::string& error,
    bool restartSource = false,
    bool forceFullTransportStream = false) {
    std::string requestedPids;
    if (!buildSharedDvbPids(
            consumers, requestedPids, filterCount, error,
            forceFullTransportStream)) {
        return false;
    }
    if (shared.source && requestedPids != shared.requestedPids) {
        if (restartSource && shared.pipeline) {
            if (gst_element_set_state(shared.pipeline, GST_STATE_READY) ==
                GST_STATE_CHANGE_FAILURE) {
                error = "failed to pause shared DVB frontend for PID filter update";
                return false;
            }
            gst_element_get_state(
                shared.pipeline, nullptr, nullptr, 2 * GST_SECOND);
        }
        g_object_set(shared.source, "pids", requestedPids.c_str(), nullptr);
        if (restartSource && shared.pipeline &&
            gst_element_set_state(shared.pipeline, GST_STATE_PLAYING) ==
                GST_STATE_CHANGE_FAILURE) {
            error = "failed to restart shared DVB frontend after PID filter update";
            return false;
        }
    }
    shared.consumerPids = consumers;
    shared.requestedPids = requestedPids;
    return true;
}
std::string sharedDvbMulticastAddress(const DvbSatelliteParams& params) {
    const unsigned slot = static_cast<unsigned>((params.adapter * 16 + params.frontend) % 250);
    return "239.255.250." + std::to_string(slot + 1);
}

uint16_t sharedDvbMulticastPort(const DvbSatelliteParams& params) {
    const unsigned slot = static_cast<unsigned>((params.adapter * 16 + params.frontend) % 1000);
    return static_cast<uint16_t>(45000 + slot);
}

bool configureServicePidFilter(DvbSingleProgramPsiContext& ctx, const std::string& pids) {
    ctx.allowedPids.fill(false);
    // Essential DVB SI tables kept for a standards-compliant service stream.
    for (uint16_t pid : {uint16_t(0x0000), uint16_t(0x0001), uint16_t(0x0010),
                         uint16_t(0x0011), uint16_t(0x0012), uint16_t(0x0014)}) {
        ctx.allowedPids[pid] = true;
    }
    if (pids.empty() || pids == "8192") {
        ctx.filterPids = false;
        return false;
    }
    size_t start = 0;
    bool any = false;
    while (start <= pids.size()) {
        const size_t end = pids.find(':', start);
        const std::string token = pids.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!token.empty()) {
            try {
                const unsigned long value = std::stoul(token);
                if (value < 8192) {
                    ctx.allowedPids[static_cast<size_t>(value)] = true;
                    any = true;
                }
            } catch (...) {
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    ctx.filterPids = any;
    return any;
}

std::string popGstPipelineError(GstBus* bus, const std::string& fallback) {
    if (!bus) return fallback;
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 1500 * GST_MSECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
    if (!msg) return fallback;
    std::string result = fallback;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        if (err && err->message) result = err->message;
        if (dbg && *dbg) result += std::string(" | ") + dbg;
        if (err) g_error_free(err);
        g_free(dbg);
    }
    gst_message_unref(msg);
    return result;
}

bool waitForGstStartup(GstElement* pipeline, GstBus* bus,
                       GstStateChangeReturn stateResult, GstClockTime timeout,
                       const std::string& fallback, std::string& error) {
    error.clear();
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        error = popGstPipelineError(bus, fallback);
        return false;
    }
    if (stateResult != GST_STATE_CHANGE_ASYNC || !pipeline) {
        return true;
    }

    const GstStateChangeReturn waitResult =
        gst_element_get_state(pipeline, nullptr, nullptr, timeout);
    if (waitResult == GST_STATE_CHANGE_FAILURE) {
        error = popGstPipelineError(bus, fallback);
        return false;
    }
    if (waitResult == GST_STATE_CHANGE_ASYNC) {
        error = popGstPipelineError(bus, fallback);
        if (error == fallback) {
            error += ": timed out waiting for PLAYING";
        }
        return false;
    }
    return true;
}
uint32_t mpeg2SectionCrc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint32_t>(data[i]) << 24;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x80000000U)
                ? (crc << 1) ^ 0x04C11DB7U
                : (crc << 1);
        }
    }
    return crc;
}

size_t tsPayloadOffset(const uint8_t* packet) {
    if (!packet || packet[0] != 0x47) return kTsPacketSize;
    const uint8_t adaptationControl = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
    if (adaptationControl == 0 || adaptationControl == 2) return kTsPacketSize;
    size_t offset = 4;
    if (adaptationControl == 3) {
        const size_t adaptationLength = packet[4];
        offset += 1 + adaptationLength;
    }
    return offset < kTsPacketSize ? offset : kTsPacketSize;
}

const uint8_t* tsSectionStart(const uint8_t* packet, size_t& available) {
    available = 0;
    if (!packet || (packet[1] & 0x40) == 0) return nullptr; // payload_unit_start_indicator
    const size_t payload = tsPayloadOffset(packet);
    if (payload >= kTsPacketSize) return nullptr;
    const size_t pointer = packet[payload];
    const size_t start = payload + 1 + pointer;
    if (start >= kTsPacketSize) return nullptr;
    available = kTsPacketSize - start;
    return packet + start;
}
bool appendPsiSectionFromPacket(const uint8_t* packet,
                                uint8_t expectedTableId,
                                std::vector<uint8_t>& buffer,
                                size_t& expectedSize) {
    if (!packet || packet[0] != 0x47) return false;
    const size_t payload = tsPayloadOffset(packet);
    if (payload >= kTsPacketSize) return false;
    size_t start = payload;
    if ((packet[1] & 0x40) != 0) {
        const size_t pointer = packet[payload];
        if (payload + 1 + pointer >= kTsPacketSize) return false;
        start = payload + 1 + pointer;
        buffer.clear();
        expectedSize = 0;
    } else if (buffer.empty()) {
        return false;
    }

    buffer.insert(buffer.end(), packet + start, packet + kTsPacketSize);
    if (expectedSize == 0 && buffer.size() >= 3) {
        if (buffer[0] != expectedTableId) {
            buffer.clear();
            return false;
        }
        const size_t sectionLength = static_cast<size_t>(((buffer[1] & 0x0F) << 8) | buffer[2]);
        const size_t total = 3 + sectionLength;
        if (sectionLength < 5 || total > 4096) {
            buffer.clear();
            expectedSize = 0;
            return false;
        }
        expectedSize = total;
    }
    return expectedSize > 0 && buffer.size() >= expectedSize;
}

void parsePatForSelectedPmt(const uint8_t* section, size_t total, DvbSingleProgramPsiContext* ctx) {
    if (!section || !ctx || total < 12 || section[0] != 0x00) return;
    const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
    if (sectionLength < 9 || 3 + sectionLength > total) return;
    ctx->transportStreamId = static_cast<uint16_t>((section[3] << 8) | section[4]);
    ctx->patVersion = static_cast<uint8_t>((section[5] >> 1) & 0x1F);
    const size_t entriesEnd = 3 + sectionLength - 4;
    for (size_t pos = 8; pos + 4 <= entriesEnd; pos += 4) {
        const uint16_t program = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
        const uint16_t mappedPid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
        if (program == ctx->serviceId && mappedPid > 0 && mappedPid < 0x1FFF) {
            if (ctx->pmtPid != mappedPid) {
                ctx->pmtPid = mappedPid;
                ctx->inputVideoPid = 0;
                ctx->inputAudioPid = 0;
                ctx->remapPmtRewritten = false;
                ctx->remapAnnounced = false;
                ctx->allowedPids[mappedPid] = true;
                ctx->pmtSectionBuffer.clear();
                ctx->pmtSectionExpected = 0;
            }
            return;
        }
    }
}

void discoverSelectedPmtFromPacket(const uint8_t* packet, DvbSingleProgramPsiContext* ctx) {
    if (!packet || !ctx || packet[0] != 0x47) return;
    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);

    if (pid == 0x0000 && appendPsiSectionFromPacket(packet, 0x00, ctx->patSectionBuffer, ctx->patSectionExpected)) {
        parsePatForSelectedPmt(ctx->patSectionBuffer.data(), ctx->patSectionExpected, ctx);
        ctx->patSectionBuffer.clear();
        ctx->patSectionExpected = 0;
        return;
    }

    size_t available = 0;
    const uint8_t* section = tsSectionStart(packet, available);
    if (!section || available < 8) return;

    if (pid == 0x0011 && (section[0] == 0x42 || section[0] == 0x46) && available >= 11) {
        const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
        const size_t total = 3 + sectionLength;
        if (sectionLength >= 12 && total <= available) {
            ctx->sdtVersion = static_cast<uint8_t>((section[5] >> 1) & 0x1F);
            ctx->originalNetworkId = static_cast<uint16_t>((section[8] << 8) | section[9]);
        }
    }

    // Fallback: identify the PMT directly. This works when the PMT PID is
    // present in saved pids even if PAT was too large or delayed.
    if (pid != 0x0000 && section[0] == 0x02 && available >= 8) {
        const uint16_t program = static_cast<uint16_t>((section[3] << 8) | section[4]);
        if (program == ctx->serviceId && ctx->pmtPid != pid) {
            ctx->pmtPid = pid;
            ctx->inputVideoPid = 0;
            ctx->inputAudioPid = 0;
            ctx->remapPmtRewritten = false;
            ctx->remapAnnounced = false;
        }
    }
}

size_t allowCaDescriptorPids(DvbSingleProgramPsiContext* ctx, const uint8_t* descriptors, size_t size) {
    if (!ctx || !descriptors) return 0;
    size_t added = 0;
    size_t pos = 0;
    while (pos + 2 <= size) {
        const uint8_t tag = descriptors[pos];
        const uint8_t length = descriptors[pos + 1];
        pos += 2;
        if (pos + length > size) break;
        if (tag == 0x09 && length >= 4) {
            const uint16_t caPid = static_cast<uint16_t>(((descriptors[pos + 2] & 0x1F) << 8) | descriptors[pos + 3]);
            if (caPid > 0 && caPid < ctx->allowedPids.size() && !ctx->allowedPids[caPid]) {
                ctx->allowedPids[caPid] = true;
                ++added;
            }
        }
        pos += length;
    }
    return added;
}

void healAllowedPidsFromSelectedPmtSection(DvbSingleProgramPsiContext* ctx, const uint8_t* section, size_t total) {
    if (!ctx || !ctx->filterPids || !section || total < 16 || section[0] != 0x02) return;
    const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
    if (sectionLength < 13 || 3 + sectionLength > total) return;
    const uint16_t program = static_cast<uint16_t>((section[3] << 8) | section[4]);
    const uint16_t advertised = ctx->remapEnabled && ctx->outputServiceId > 0 ? ctx->outputServiceId : ctx->serviceId;
    if (program != ctx->serviceId && program != advertised) return;

    const size_t end = 3 + sectionLength - 4;
    size_t added = 0;
    const uint16_t pcrPid = static_cast<uint16_t>(((section[8] & 0x1F) << 8) | section[9]);
    if (pcrPid < ctx->allowedPids.size() && !ctx->allowedPids[pcrPid]) {
        ctx->allowedPids[pcrPid] = true;
        ++added;
    }

    const size_t programInfoLength = static_cast<size_t>(((section[10] & 0x0F) << 8) | section[11]);
    size_t pos = 12;
    if (pos + programInfoLength > end) return;
    added += allowCaDescriptorPids(ctx, section + pos, programInfoLength);
    pos += programInfoLength;

    while (pos + 5 <= end) {
        const uint16_t elementaryPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
        const size_t esInfoLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
        pos += 5;
        if (pos + esInfoLength > end) break;
        if (elementaryPid > 0 && elementaryPid < ctx->allowedPids.size() && !ctx->allowedPids[elementaryPid]) {
            ctx->allowedPids[elementaryPid] = true;
            ++added;
        }
        added += allowCaDescriptorPids(ctx, section + pos, esInfoLength);
        pos += esInfoLength;
    }

    if (!ctx->pidSelfHealAnnounced) {
        if (added > 0) {
            std::cerr << "DVB SPTS PID filter self-heal: SID=" << ctx->serviceId
                      << " PMT_PID=" << ctx->pmtPid
                      << " added_pids=" << added
                      << " source=selected-pmt" << std::endl;
        } else {
            std::cerr << "DVB SPTS PID filter ready: SID=" << ctx->serviceId
                      << " PMT_PID=" << ctx->pmtPid
                      << " source=selected-pmt saved-pids-complete" << std::endl;
        }
        ctx->pidSelfHealAnnounced = true;
    }
}

void healAllowedPidsFromSelectedPmt(uint8_t* packet, DvbSingleProgramPsiContext* ctx) {
    if (!packet || !ctx || !ctx->filterPids || packet[0] != 0x47) return;
    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    if (pid != ctx->pmtPid || pid >= 0x1FFF) return;

    if (appendPsiSectionFromPacket(packet, 0x02, ctx->pmtSectionBuffer, ctx->pmtSectionExpected)) {
        healAllowedPidsFromSelectedPmtSection(ctx, ctx->pmtSectionBuffer.data(), ctx->pmtSectionExpected);
        ctx->pmtSectionBuffer.clear();
        ctx->pmtSectionExpected = 0;
    }
}
uint16_t advertisedDvbServiceId(const DvbSingleProgramPsiContext& ctx) {
    if (!ctx.remapEnabled || ctx.outputServiceId == 0) return ctx.serviceId;
    return (ctx.remapPmtRewritten || ctx.outputServiceId == ctx.serviceId)
        ? ctx.outputServiceId
        : ctx.serviceId;
}

uint16_t requestedDvbServiceId(const DvbSingleProgramPsiContext& ctx) {
    return ctx.remapEnabled && ctx.outputServiceId > 0 ? ctx.outputServiceId : ctx.serviceId;
}

bool isValidDvbElementaryPid(uint32_t pid) {
    return pid >= 0x20 && pid < 0x1FFF;
}

bool isDvbVideoStreamType(uint8_t streamType) {
    switch (streamType) {
        case 0x01: // MPEG-1 video
        case 0x02: // MPEG-2 video
        case 0x10: // MPEG-4 Visual
        case 0x1B: // H.264/AVC
        case 0x24: // H.265/HEVC
        case 0x42: // AVS video
            return true;
        default:
            return false;
    }
}

bool isDvbAudioStreamType(uint8_t streamType) {
    switch (streamType) {
        case 0x03: // MPEG-1 audio
        case 0x04: // MPEG-2 audio
        case 0x0F: // AAC ADTS
        case 0x11: // AAC LATM
        case 0x81: // AC-3 (common private registration)
        case 0x87: // E-AC-3 (common private registration)
            return true;
        default:
            return false;
    }
}

void rewriteDvbRemapPmt(uint8_t* packet, DvbSingleProgramPsiContext* ctx) {
    if (!packet || !ctx || !ctx->remapEnabled || packet[0] != 0x47) return;
    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    if (pid != ctx->pmtPid) return;

    size_t available = 0;
    uint8_t* section = const_cast<uint8_t*>(tsSectionStart(packet, available));
    if (!section || available < 16 || section[0] != 0x02) return;
    const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
    const size_t total = 3 + sectionLength;
    if (sectionLength < 13 || total > available) return;

    const uint16_t inputProgram = static_cast<uint16_t>((section[3] << 8) | section[4]);
    if (inputProgram != ctx->serviceId && inputProgram != advertisedDvbServiceId(*ctx)) return;

    const size_t end = total - 4; // CRC excluded
    const size_t programInfoLength = static_cast<size_t>(((section[10] & 0x0F) << 8) | section[11]);
    if (12 + programInfoLength > end) return;

    // Discover the first video and audio ES exactly as the previous tsdemux
    // remap path did.  Other ES/CA/teletext/subtitle PIDs remain untouched.
    size_t pos = 12 + programInfoLength;
    while (pos + 5 <= end) {
        const uint8_t streamType = section[pos];
        const uint16_t esPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
        const size_t esInfoLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
        if (pos + 5 + esInfoLength > end) return;
        if (ctx->inputVideoPid == 0 && isDvbVideoStreamType(streamType)) ctx->inputVideoPid = esPid;
        if (ctx->inputAudioPid == 0 && isDvbAudioStreamType(streamType)) ctx->inputAudioPid = esPid;
        pos += 5 + esInfoLength;
    }

    const uint16_t outputSid = requestedDvbServiceId(*ctx);
    section[3] = static_cast<uint8_t>(outputSid >> 8);
    section[4] = static_cast<uint8_t>(outputSid & 0xFF);

    uint16_t pcrPid = static_cast<uint16_t>(((section[8] & 0x1F) << 8) | section[9]);
    if (ctx->inputVideoPid && pcrPid == ctx->inputVideoPid && ctx->requestedVideoPid) pcrPid = ctx->requestedVideoPid;
    if (ctx->inputAudioPid && pcrPid == ctx->inputAudioPid && ctx->requestedAudioPid) pcrPid = ctx->requestedAudioPid;
    section[8] = static_cast<uint8_t>(0xE0 | ((pcrPid >> 8) & 0x1F));
    section[9] = static_cast<uint8_t>(pcrPid & 0xFF);

    pos = 12 + programInfoLength;
    while (pos + 5 <= end) {
        const uint8_t streamType = section[pos];
        uint16_t esPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
        const size_t esInfoLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
        if (pos + 5 + esInfoLength > end) return;
        if (ctx->inputVideoPid && esPid == ctx->inputVideoPid && isDvbVideoStreamType(streamType) && ctx->requestedVideoPid) {
            esPid = ctx->requestedVideoPid;
        } else if (ctx->inputAudioPid && esPid == ctx->inputAudioPid && isDvbAudioStreamType(streamType) && ctx->requestedAudioPid) {
            esPid = ctx->requestedAudioPid;
        }
        section[pos + 1] = static_cast<uint8_t>(0xE0 | ((esPid >> 8) & 0x1F));
        section[pos + 2] = static_cast<uint8_t>(esPid & 0xFF);
        pos += 5 + esInfoLength;
    }

    const uint32_t crc = mpeg2SectionCrc32(section, total - 4);
    section[total - 4] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    section[total - 3] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    section[total - 2] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    section[total - 1] = static_cast<uint8_t>(crc & 0xFF);
    ctx->remapPmtRewritten = true;

    if (!ctx->remapAnnounced) {
        const bool pidRemap = ctx->requestedVideoPid && ctx->requestedAudioPid;
        std::cerr << "DVB TS remap: SID=" << ctx->serviceId << "->" << outputSid
                  << " mode=" << (pidRemap ? "packet-av-pid-rewrite-no-demux-no-remux"
                                             : "packet-sid-only-no-demux-no-remux");
        if (pidRemap) {
            std::cerr << " video=" << ctx->inputVideoPid << "->" << ctx->requestedVideoPid
                      << " audio=" << ctx->inputAudioPid << "->" << ctx->requestedAudioPid;
        }
        std::cerr << std::endl;
        ctx->remapAnnounced = true;
    }
}

void rewriteDvbRemapPacketPid(uint8_t* packet, const DvbSingleProgramPsiContext& ctx) {
    if (!packet || !ctx.remapEnabled || packet[0] != 0x47) return;
    if (!ctx.remapPmtRewritten || !ctx.requestedVideoPid || !ctx.requestedAudioPid) return;
    uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    uint16_t mapped = pid;
    if (ctx.inputVideoPid && pid == ctx.inputVideoPid && ctx.requestedVideoPid) mapped = ctx.requestedVideoPid;
    if (ctx.inputAudioPid && pid == ctx.inputAudioPid && ctx.requestedAudioPid) mapped = ctx.requestedAudioPid;
    if (mapped != pid) {
        packet[1] = static_cast<uint8_t>((packet[1] & 0xE0) | ((mapped >> 8) & 0x1F));
        packet[2] = static_cast<uint8_t>(mapped & 0xFF);
    }
}

void normalizeDvbRemapContinuity(uint8_t* packet, DvbSingleProgramPsiContext* ctx) {
    if (!packet || !ctx || !ctx->remapEnabled || packet[0] != 0x47) return;

    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    if (pid >= 0x1FFF) return;

    const uint8_t adaptationControl = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
    if (adaptationControl == 0) {
        ctx->remapContinuityValid[pid] = false;
        return;
    }

    bool discontinuity = false;
    if ((adaptationControl == 2 || adaptationControl == 3) &&
        packet[4] > 0 && packet[4] <= 183) {
        discontinuity = (packet[5] & 0x80) != 0;
    }

    const uint8_t sourceCc = static_cast<uint8_t>(packet[3] & 0x0F);
    if (discontinuity) {
        ctx->remapContinuity[pid] = sourceCc;
        ctx->remapContinuityValid[pid] = true;
        return;
    }

    const bool hasPayload = adaptationControl == 1 || adaptationControl == 3;
    uint8_t outputCc = sourceCc;
    if (hasPayload) {
        if (ctx->remapContinuityValid[pid]) {
            outputCc = static_cast<uint8_t>((ctx->remapContinuity[pid] + 1) & 0x0F);
        }
        ctx->remapContinuity[pid] = outputCc;
        ctx->remapContinuityValid[pid] = true;
    } else if (ctx->remapContinuityValid[pid]) {
        outputCc = ctx->remapContinuity[pid];
    } else {
        ctx->remapContinuity[pid] = outputCc;
        ctx->remapContinuityValid[pid] = true;
    }

    packet[3] = static_cast<uint8_t>((packet[3] & 0xF0) | (outputCc & 0x0F));

    if (!ctx->remapContinuityAnnounced) {
        std::cerr << "DVB remap continuity: SID=" << ctx->serviceId
                  << " output_pid_cc=normalized payload-aware adaptation-only=no-increment"
                  << std::endl;
        ctx->remapContinuityAnnounced = true;
    }
}

void writeSingleProgramPat(uint8_t* packet, const DvbSingleProgramPsiContext& ctx) {
    if (!packet || ctx.pmtPid == 0 || ctx.pmtPid >= 0x1FFF) return;
    const uint8_t continuity = static_cast<uint8_t>(packet[3] & 0x0F);
    std::memset(packet, 0xFF, kTsPacketSize);
    packet[0] = 0x47;
    packet[1] = 0x40; // PUSI + PID 0
    packet[2] = 0x00;
    packet[3] = static_cast<uint8_t>(0x10 | continuity); // payload only
    packet[4] = 0x00; // pointer_field

    uint8_t* section = packet + 5;
    constexpr uint16_t sectionLength = 13; // through CRC32
    section[0] = 0x00; // PAT
    section[1] = static_cast<uint8_t>(0xB0 | ((sectionLength >> 8) & 0x0F));
    section[2] = static_cast<uint8_t>(sectionLength & 0xFF);
    section[3] = static_cast<uint8_t>(ctx.transportStreamId >> 8);
    section[4] = static_cast<uint8_t>(ctx.transportStreamId & 0xFF);
    section[5] = static_cast<uint8_t>(0xC1 | ((ctx.patVersion & 0x1F) << 1));
    section[6] = 0x00; // section_number
    section[7] = 0x00; // last_section_number
    const uint16_t advertisedSid = advertisedDvbServiceId(ctx);
    section[8] = static_cast<uint8_t>(advertisedSid >> 8);
    section[9] = static_cast<uint8_t>(advertisedSid & 0xFF);
    section[10] = static_cast<uint8_t>(0xE0 | ((ctx.pmtPid >> 8) & 0x1F));
    section[11] = static_cast<uint8_t>(ctx.pmtPid & 0xFF);
    const uint32_t crc = mpeg2SectionCrc32(section, 12);
    section[12] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    section[13] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    section[14] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    section[15] = static_cast<uint8_t>(crc & 0xFF);
}

std::vector<uint8_t> dvbUtf8ServiceText(const std::string& value, size_t maxBytes) {
    std::vector<uint8_t> out;
    if (value.empty() || maxBytes < 2) return out;
    out.push_back(0x15); // DVB UTF-8 selector
    const size_t copy = std::min(maxBytes - 1, value.size());
    out.insert(out.end(), value.begin(), value.begin() + static_cast<std::ptrdiff_t>(copy));
    return out;
}

void writeSingleProgramSdt(uint8_t* packet, const DvbSingleProgramPsiContext& ctx) {
    if (!packet || ctx.serviceId == 0) return;
    const uint8_t continuity = static_cast<uint8_t>(packet[3] & 0x0F);
    std::memset(packet, 0xFF, kTsPacketSize);
    packet[0] = 0x47;
    packet[1] = 0x40; // PUSI + PID 0x11
    packet[2] = 0x11;
    packet[3] = static_cast<uint8_t>(0x10 | continuity);
    packet[4] = 0x00;

    const std::string nameText = ctx.serviceName.empty()
        ? ("Service " + std::to_string(advertisedDvbServiceId(ctx)))
        : ctx.serviceName;
    auto provider = dvbUtf8ServiceText(ctx.serviceProvider, 48);
    auto name = dvbUtf8ServiceText(nameText, 80);
    while (provider.size() + name.size() > 140) {
        if (name.size() > 2) name.pop_back();
        else if (provider.size() > 2) provider.pop_back();
        else break;
    }

    const size_t descriptorPayloadLength = 3 + provider.size() + name.size();
    const size_t descriptorTotalLength = 2 + descriptorPayloadLength;
    const uint16_t sectionLength = static_cast<uint16_t>(8 + 5 + descriptorTotalLength + 4);
    uint8_t* section = packet + 5;
    section[0] = 0x42; // actual TS SDT
    section[1] = static_cast<uint8_t>(0xF0 | ((sectionLength >> 8) & 0x0F));
    section[2] = static_cast<uint8_t>(sectionLength & 0xFF);
    section[3] = static_cast<uint8_t>(ctx.transportStreamId >> 8);
    section[4] = static_cast<uint8_t>(ctx.transportStreamId & 0xFF);
    section[5] = static_cast<uint8_t>(0xC1 | ((ctx.sdtVersion & 0x1F) << 1));
    section[6] = 0x00;
    section[7] = 0x00;
    section[8] = static_cast<uint8_t>(ctx.originalNetworkId >> 8);
    section[9] = static_cast<uint8_t>(ctx.originalNetworkId & 0xFF);
    section[10] = 0xFF;

    size_t pos = 11;
    const uint16_t advertisedSid = advertisedDvbServiceId(ctx);
    section[pos++] = static_cast<uint8_t>(advertisedSid >> 8);
    section[pos++] = static_cast<uint8_t>(advertisedSid & 0xFF);
    section[pos++] = 0xFC; // EIT flags off
    const uint16_t loopLength = static_cast<uint16_t>(descriptorTotalLength);
    section[pos++] = static_cast<uint8_t>(0x80 | ((loopLength >> 8) & 0x0F)); // running_status=4
    section[pos++] = static_cast<uint8_t>(loopLength & 0xFF);
    section[pos++] = 0x48; // service_descriptor
    section[pos++] = static_cast<uint8_t>(descriptorPayloadLength);
    section[pos++] = 0x01; // digital television service
    section[pos++] = static_cast<uint8_t>(provider.size());
    for (uint8_t b : provider) section[pos++] = b;
    section[pos++] = static_cast<uint8_t>(name.size());
    for (uint8_t b : name) section[pos++] = b;

    const uint32_t crc = mpeg2SectionCrc32(section, pos);
    section[pos++] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    section[pos++] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    section[pos++] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    section[pos++] = static_cast<uint8_t>(crc & 0xFF);
}

void suppressPatPacketUntilPmtKnown(uint8_t* packet) {
    if (!packet) return;
    const uint8_t continuity = static_cast<uint8_t>(packet[3] & 0x0F);
    std::memset(packet, 0xFF, kTsPacketSize);
    packet[0] = 0x47;
    packet[1] = 0x1F;
    packet[2] = 0xFF;
    packet[3] = static_cast<uint8_t>(0x10 | continuity);
}

GstPadProbeReturn dvbSingleProgramPsiProbe(GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* ctx = static_cast<DvbSingleProgramPsiContext*>(userData);
    if (!ctx || ctx->serviceId == 0 || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }

    GstBuffer* original = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!original) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = gst_buffer_make_writable(original);
    if (!buffer) return GST_PAD_PROBE_OK;
    if (buffer != original) GST_PAD_PROBE_INFO_DATA(info) = buffer;

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) return GST_PAD_PROBE_OK;

    size_t packetStart = 0;
    const size_t maxSyncOffset = std::min<size_t>(kTsPacketSize, map.size);
    bool aligned = map.size >= kTsPacketSize && map.data[0] == 0x47;
    if (aligned && map.size >= kTsPacketSize * 2 && map.data[kTsPacketSize] != 0x47) {
        aligned = false;
    }
    if (!aligned) {
        bool found = false;
        for (size_t candidate = 0; candidate < maxSyncOffset; ++candidate) {
            if (map.data[candidate] != 0x47) continue;
            if (candidate + kTsPacketSize >= map.size || map.data[candidate + kTsPacketSize] == 0x47) {
                packetStart = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            gst_buffer_unmap(buffer, &map);
            return GST_PAD_PROBE_OK;
        }
    }

    size_t writeOffset = 0;
    for (size_t offset = packetStart; offset + kTsPacketSize <= map.size; offset += kTsPacketSize) {
        uint8_t* packet = map.data + offset;
        if (packet[0] != 0x47) continue;
        discoverSelectedPmtFromPacket(packet, ctx);
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        if (ctx->pmtPid > 0 && ctx->pmtPid < 0x1FFF) {
            ctx->allowedPids[ctx->pmtPid] = true;
        }

        healAllowedPidsFromSelectedPmt(packet, ctx);

        const bool remapPidMappingPending = ctx->remapEnabled &&
            (ctx->requestedVideoPid || ctx->requestedAudioPid) &&
            !ctx->remapPmtRewritten;
        const bool filterReady = !ctx->filterPids ||
            (ctx->pidSelfHealAnnounced && !remapPidMappingPending);
        if (ctx->filterPids && !filterReady && !ctx->pidFilterWarmupAnnounced) {
            std::cerr << "DVB SPTS PID filter warmup: SID=" << ctx->serviceId
                      << " saved_pid_filter=pending-pmt full-ts-pass-until-healed" << std::endl;
            ctx->pidFilterWarmupAnnounced = true;
        }
        const bool keepPacket = !filterReady || (pid < ctx->allowedPids.size() && ctx->allowedPids[pid]);
        if (!keepPacket) continue;

        // For DVB remap, rewrite only TS headers + PMT/PAT/SDT.  PES payload,
        // timestamps, codec private data and CA packets stay untouched.
        if (ctx->remapEnabled && pid == ctx->pmtPid) {
            rewriteDvbRemapPmt(packet, ctx);
        }

        if (pid == 0x0000) {
            if (ctx->pmtPid > 0 && ctx->pmtPid < 0x1FFF) {
                writeSingleProgramPat(packet, *ctx);
                if (!ctx->announced) {
                    std::cerr << "DVB SPTS PSI filter: SID=" << ctx->serviceId
                              << " PMT_PID=" << ctx->pmtPid
                              << " PAT=single-program SDT=single-service media=passthrough"
                              << " pid_filter=" << (ctx->filterPids ? "service" : "off")
                              << std::endl;
                    ctx->announced = true;
                }
            } else {
                suppressPatPacketUntilPmtKnown(packet);
            }
        } else if (pid == 0x0011) {
            // SDT is also copied from the full transponder by Linux DVB demux.
            // Replace it so VLC advertises only the selected service.
            writeSingleProgramSdt(packet, *ctx);
        }

        if (pid != 0x0000 && pid != 0x0011 && pid != ctx->pmtPid) {
            rewriteDvbRemapPacketPid(packet, *ctx);
        }

        // Remap creates a new logical SPTS. Normalize continuity only after
        // all PID/SID/PSI rewriting, using the final output PID. Remap OFF
        // remains byte-for-byte unchanged.
        if (ctx->remapEnabled) {
            normalizeDvbRemapContinuity(packet, ctx);
        }

        if (writeOffset != offset) {
            std::memmove(map.data + writeOffset, packet, kTsPacketSize);
        }
        writeOffset += kTsPacketSize;
    }

    gst_buffer_unmap(buffer, &map);
    if (ctx->filterPids) {
        if (writeOffset == 0) return GST_PAD_PROBE_DROP;
        if (writeOffset < gst_buffer_get_size(buffer)) {
            gst_buffer_resize(buffer, 0, static_cast<gssize>(writeOffset));
        }
    }
    return GST_PAD_PROBE_OK;
}


// v178 single-pass DVB service dispatcher.
//
// The v167-v177 relay topology delivered the complete transponder to a
// separate GStreamer service pipeline for every selected channel.  That made
// each shared_service_* thread scan and compact the same MPTS again.  v178
// performs TS framing once on the shared frontend, routes each PID to the
// interested services, applies the existing packet-level SPTS rewrite and CA
// backend per service, then emits 7x188-byte localhost UDP datagrams directly
// to the normal stream input pipeline.  No per-service full-MPTS udpsrc/queue
// pipeline is created.
struct SharedDvbDispatchConsumer {
    std::string streamId;
    std::string frontendKey;
    DvbSingleProgramPsiContext psi;
    int socketFd = -1;
    sockaddr_in destination{};
    bool caEnabled = false;
    unsigned slot = 0;
    std::array<uint8_t, kTsPacketSize * kTsPacketsPerUdpBuffer> datagram{};
    size_t datagramBytes = 0;
    uint64_t inputPackets = 0;
    uint64_t outputPackets = 0;
    uint64_t udpDatagrams = 0;
    uint64_t udpErrors = 0;

    ~SharedDvbDispatchConsumer() {
        if (socketFd >= 0) {
            ::close(socketFd);
            socketFd = -1;
        }
    }
};

struct SharedDvbDispatcherState {
    std::mutex mutex;
    std::array<std::shared_ptr<SharedDvbDispatchConsumer>, 64> slots{};
    std::map<std::string, unsigned> streamSlots;
    std::array<uint64_t, 8192> pidRoutes{};
    std::vector<uint8_t> remainder;
    uint64_t inputPackets = 0;
    uint64_t routedPackets = 0;
    uint64_t droppedPackets = 0;
    uint64_t resyncs = 0;
    bool announced = false;
    std::chrono::steady_clock::time_point statsStarted = std::chrono::steady_clock::now();
};

size_t countAllowedDvbPids(const DvbSingleProgramPsiContext& ctx) {
    return static_cast<size_t>(std::count(ctx.allowedPids.begin(), ctx.allowedPids.end(), true));
}

void rebuildSharedDvbPidRoutes(SharedDvbDispatcherState& dispatcher) {
    dispatcher.pidRoutes.fill(0);
    for (unsigned slot = 0; slot < dispatcher.slots.size(); ++slot) {
        const auto& consumer = dispatcher.slots[slot];
        if (!consumer) continue;
        const uint64_t bit = uint64_t{1} << slot;
        for (size_t pid = 0; pid < consumer->psi.allowedPids.size(); ++pid) {
            if (consumer->psi.allowedPids[pid]) dispatcher.pidRoutes[pid] |= bit;
        }
        // PAT and SDT must always reach every service because they are rewritten
        // to a single-program view and are also used to discover PMT metadata.
        dispatcher.pidRoutes[0x0000] |= bit;
        dispatcher.pidRoutes[0x0011] |= bit;
        if (consumer->psi.pmtPid > 0 && consumer->psi.pmtPid < 0x1FFF) {
            dispatcher.pidRoutes[consumer->psi.pmtPid] |= bit;
        }
    }
}

bool flushSharedDvbConsumerDatagram(SharedDvbDispatchConsumer& consumer, bool force) {
    if (consumer.datagramBytes == 0) return true;
    const size_t fullDatagram = kTsPacketSize * kTsPacketsPerUdpBuffer;
    if (!force && consumer.datagramBytes < fullDatagram) return true;

    // The existing CA backend remains exactly per service and sees only the
    // selected SPTS.  The v176 Irdeto CHID autoselect/Newcamd implementation is
    // untouched; this merely changes where its TS callback is invoked.
    if (consumer.caEnabled) {
        (void)CaBackendManager::instance().processTransport(
            consumer.streamId, consumer.datagram.data(), consumer.datagramBytes);
    }

    const ssize_t sent = ::sendto(
        consumer.socketFd,
        consumer.datagram.data(),
        consumer.datagramBytes,
        MSG_NOSIGNAL,
        reinterpret_cast<const sockaddr*>(&consumer.destination),
        sizeof(consumer.destination));
    if (sent != static_cast<ssize_t>(consumer.datagramBytes)) {
        ++consumer.udpErrors;
        if (consumer.udpErrors <= 5 || (consumer.udpErrors % 100) == 0) {
            std::cerr << "Shared DVB dispatcher UDP warning: stream=" << consumer.streamId
                      << " sent=" << sent
                      << " expected=" << consumer.datagramBytes
                      << " errno=" << errno
                      << " errors=" << consumer.udpErrors << std::endl;
        }
        consumer.datagramBytes = 0;
        return false;
    }
    ++consumer.udpDatagrams;
    consumer.datagramBytes = 0;
    return true;
}

void appendSharedDvbConsumerPacket(
    SharedDvbDispatchConsumer& consumer, const uint8_t* packet) {
    if (!packet || packet[0] != 0x47) return;
    const size_t fullDatagram = kTsPacketSize * kTsPacketsPerUdpBuffer;
    if (consumer.datagramBytes + kTsPacketSize > fullDatagram) {
        flushSharedDvbConsumerDatagram(consumer, true);
    }
    std::memcpy(
        consumer.datagram.data() + consumer.datagramBytes,
        packet,
        kTsPacketSize);
    consumer.datagramBytes += kTsPacketSize;
    ++consumer.outputPackets;
    if (consumer.datagramBytes == fullDatagram) {
        flushSharedDvbConsumerDatagram(consumer, false);
    }
}

bool prepareSharedDvbServicePacket(
    const uint8_t* inputPacket,
    SharedDvbDispatchConsumer& consumer,
    std::array<uint8_t, kTsPacketSize>& packetCopy,
    bool& routesChanged) {
    if (!inputPacket || inputPacket[0] != 0x47) return false;
    ++consumer.inputPackets;
    std::memcpy(packetCopy.data(), inputPacket, kTsPacketSize);
    uint8_t* packet = packetCopy.data();
    auto& ctx = consumer.psi;

    const uint16_t inputPid = static_cast<uint16_t>(
        ((packet[1] & 0x1F) << 8) | packet[2]);
    const uint16_t previousPmtPid = ctx.pmtPid;
    const size_t allowedBefore =
        (inputPid == 0x0000 || inputPid == ctx.pmtPid)
            ? countAllowedDvbPids(ctx)
            : 0;

    // PSI parsing is intentionally limited to packets which can change routing.
    // Ordinary video/audio/PCR packets never enter the section assembler here.
    if (inputPid == 0x0000 || inputPid == 0x0011 || inputPid == ctx.pmtPid) {
        discoverSelectedPmtFromPacket(packet, &ctx);
        if (ctx.pmtPid > 0 && ctx.pmtPid < 0x1FFF) {
            ctx.allowedPids[ctx.pmtPid] = true;
        }
        if (inputPid == ctx.pmtPid) {
            healAllowedPidsFromSelectedPmt(packet, &ctx);
        }
    }

    if (previousPmtPid != ctx.pmtPid) routesChanged = true;
    if (inputPid == 0x0000 || inputPid == ctx.pmtPid) {
        if (countAllowedDvbPids(ctx) != allowedBefore) routesChanged = true;
    }

    // Unlike the old per-service filter, never pass the full transponder while
    // PMT discovery is warming up.  Saved scan PIDs flow immediately; if a scan
    // list is absent, PAT -> selected PMT dynamically teaches the route table.
    const bool keepPacket = inputPid < ctx.allowedPids.size() && ctx.allowedPids[inputPid];
    if (!keepPacket) return false;

    if (ctx.remapEnabled && inputPid == ctx.pmtPid) {
        rewriteDvbRemapPmt(packet, &ctx);
    }

    if (inputPid == 0x0000) {
        if (ctx.pmtPid > 0 && ctx.pmtPid < 0x1FFF) {
            writeSingleProgramPat(packet, ctx);
            if (!ctx.announced) {
                std::cerr << "DVB SPTS PSI filter: SID=" << ctx.serviceId
                          << " PMT_PID=" << ctx.pmtPid
                          << " PAT=single-program SDT=single-service media=passthrough"
                          << " pid_filter=single-pass-dispatcher" << std::endl;
                ctx.announced = true;
            }
        } else {
            suppressPatPacketUntilPmtKnown(packet);
        }
    } else if (inputPid == 0x0011) {
        writeSingleProgramSdt(packet, ctx);
    }

    if (inputPid != 0x0000 && inputPid != 0x0011 && inputPid != ctx.pmtPid) {
        rewriteDvbRemapPacketPid(packet, ctx);
    }
    if (ctx.remapEnabled) normalizeDvbRemapContinuity(packet, &ctx);
    return true;
}

void dispatchSharedDvbPacket(
    SharedDvbDispatcherState& dispatcher, const uint8_t* packet) {
    if (!packet || packet[0] != 0x47) return;
    ++dispatcher.inputPackets;
    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    if (pid >= dispatcher.pidRoutes.size()) return;

    uint64_t routes = dispatcher.pidRoutes[pid];
    if (routes == 0) {
        ++dispatcher.droppedPackets;
        return;
    }

    bool routesChanged = false;
    std::array<uint8_t, kTsPacketSize> packetCopy{};
    while (routes) {
        const unsigned slot = static_cast<unsigned>(__builtin_ctzll(routes));
        routes &= routes - 1;
        if (slot >= dispatcher.slots.size()) continue;
        const auto& consumer = dispatcher.slots[slot];
        if (!consumer) continue;
        if (prepareSharedDvbServicePacket(packet, *consumer, packetCopy, routesChanged)) {
            appendSharedDvbConsumerPacket(*consumer, packetCopy.data());
            ++dispatcher.routedPackets;
        }
    }
    if (routesChanged) rebuildSharedDvbPidRoutes(dispatcher);
}

GstPadProbeReturn sharedDvbDispatcherProbe(
    GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* dispatcher = static_cast<SharedDvbDispatcherState*>(userData);
    if (!dispatcher || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) {
        return GST_PAD_PROBE_OK;
    }
    GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buffer) return GST_PAD_PROBE_OK;
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;

    std::lock_guard<std::mutex> lock(dispatcher->mutex);
    size_t offset = 0;

    // Complete at most one packet carried over from the previous GstBuffer.
    if (!dispatcher->remainder.empty()) {
        const size_t need = kTsPacketSize - dispatcher->remainder.size();
        const size_t take = std::min(need, map.size);
        dispatcher->remainder.insert(
            dispatcher->remainder.end(), map.data, map.data + take);
        offset += take;
        if (dispatcher->remainder.size() == kTsPacketSize) {
            if (dispatcher->remainder[0] == 0x47) {
                dispatchSharedDvbPacket(*dispatcher, dispatcher->remainder.data());
            } else {
                ++dispatcher->resyncs;
            }
            dispatcher->remainder.clear();
        }
    }

    while (offset + kTsPacketSize <= map.size) {
        if (map.data[offset] != 0x47) {
            bool found = false;
            const size_t searchEnd = std::min(map.size, offset + kTsPacketSize);
            for (size_t candidate = offset + 1; candidate < searchEnd; ++candidate) {
                if (map.data[candidate] != 0x47) continue;
                if (candidate + kTsPacketSize >= map.size ||
                    map.data[candidate + kTsPacketSize] == 0x47) {
                    offset = candidate;
                    found = true;
                    ++dispatcher->resyncs;
                    break;
                }
            }
            if (!found) break;
        }
        dispatchSharedDvbPacket(*dispatcher, map.data + offset);
        offset += kTsPacketSize;
    }

    if (offset < map.size) {
        dispatcher->remainder.assign(map.data + offset, map.data + map.size);
        if (dispatcher->remainder.size() >= kTsPacketSize) {
            dispatcher->remainder.erase(
                dispatcher->remainder.begin(),
                dispatcher->remainder.end() - (kTsPacketSize - 1));
        }
    }

    if (!dispatcher->announced) {
        std::cerr << "Shared DVB dispatcher active: mode=single-pass-pid-routing"
                  << " per-service-full-mpts=off"
                  << " internal-multicast=off"
                  << " udp_packet_group=7x188"
                  << " ca_stage=selected-spts-before-local-udp" << std::endl;
        dispatcher->announced = true;
    }

    if (dvbDiagnosticsEnabled()) {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - dispatcher->statsStarted).count();
        if (seconds >= 10.0) {
            std::cerr << "Shared DVB dispatcher stats: input_packets=" << dispatcher->inputPackets
                      << " routed_packets=" << dispatcher->routedPackets
                      << " dropped_packets=" << dispatcher->droppedPackets
                      << " resyncs=" << dispatcher->resyncs
                      << " services=" << dispatcher->streamSlots.size() << std::endl;
            dispatcher->statsStarted = now;
        }
    }

    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
}


struct TsCcStageProbeContext {
    explicit TsCcStageProbeContext(std::string streamId, std::string stage)
        : trace(std::move(streamId), std::move(stage)) {}
    TsCcStageTrace trace;
};

GstPadProbeReturn tsCcStageProbe(GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* ctx = static_cast<TsCcStageProbeContext*>(userData);
    if (!ctx || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
    if (!buffer) return GST_PAD_PROBE_OK;
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return GST_PAD_PROBE_OK;
    ctx->trace.inspect(map.data, map.size);
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
}

struct CaBackendTsProbeContext {
    std::string streamId;
};

GstPadProbeReturn caBackendTsProbe(GstPad*, GstPadProbeInfo* info, gpointer userData) {
    auto* ctx = static_cast<CaBackendTsProbeContext*>(userData);
    if (!ctx || !(info->type & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    GstBuffer* original = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!original) return GST_PAD_PROBE_OK;
    GstBuffer* buffer = gst_buffer_make_writable(original);
    if (!buffer) return GST_PAD_PROBE_OK;
    if (buffer != original) GST_PAD_PROBE_INFO_DATA(info) = buffer;

    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READWRITE)) return GST_PAD_PROBE_OK;
    (void)CaBackendManager::instance().processTransport(ctx->streamId, map.data, map.size);
    gst_buffer_unmap(buffer, &map);
    return GST_PAD_PROBE_OK;
}

bool hasProperty(GstElement* element, const char* propertyName) {
    return element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), propertyName) != nullptr;
}

void setBooleanPropertyIfPresent(GstElement* element, const char* propertyName, gboolean value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setIntPropertyIfPresent(GstElement* element, const char* propertyName, gint value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setUIntPropertyIfPresent(GstElement* element, const char* propertyName, guint value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setUInt64PropertyIfPresent(GstElement* element, const char* propertyName, guint64 value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setInt64PropertyIfPresent(GstElement* element, const char* propertyName, gint64 value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setStringPropertyIfPresent(GstElement* element, const char* propertyName, const std::string& value) {
    if (hasProperty(element, propertyName) && !value.empty()) {
        g_object_set(element, propertyName, value.c_str(), nullptr);
    }
}

std::string appendHlsAccessQuery(const std::string& uri, const StreamConfig& cfg) {
    if (cfg.hlsAccessKeyMode != "query" || cfg.hlsAccessKeyName.empty() || cfg.hlsAccessKeyValue.empty()) {
        return uri;
    }
    gchar* escapedName = g_uri_escape_string(cfg.hlsAccessKeyName.c_str(), nullptr, TRUE);
    gchar* escapedValue = g_uri_escape_string(cfg.hlsAccessKeyValue.c_str(), nullptr, TRUE);
    if (!escapedName || !escapedValue) {
        if (escapedName) g_free(escapedName);
        if (escapedValue) g_free(escapedValue);
        return uri;
    }
    const std::string keyPrefix = std::string(escapedName) + "=";
    // Do not duplicate a key already present in a provider URL.
    const auto queryPos = uri.find('?');
    if (queryPos != std::string::npos) {
        const std::string query = uri.substr(queryPos + 1);
        if (query.rfind(keyPrefix, 0) == 0 || query.find("&" + keyPrefix) != std::string::npos) {
            g_free(escapedName);
            g_free(escapedValue);
            return uri;
        }
    }
    const std::string result = uri + (queryPos == std::string::npos ? "?" : "&") +
        escapedName + "=" + escapedValue;
    g_free(escapedName);
    g_free(escapedValue);
    return result;
}

void configureHlsHttpSource(GstElement* element, const StreamConfig& cfg);

void onHlsSourceLocationChanged(GObject* object, GParamSpec*, gpointer userData) {
    auto* cfg = static_cast<StreamConfig*>(userData);
    if (!cfg || cfg->hlsAccessKeyMode != "query" || cfg->hlsAccessKeyValue.empty()) return;
    if (g_object_get_data(object, "tvs-hls-query-update")) return;
    gchar* current = nullptr;
    g_object_get(object, "location", &current, nullptr);
    if (!current || !*current) {
        if (current) g_free(current);
        return;
    }
    const std::string updated = appendHlsAccessQuery(current, *cfg);
    if (updated != current) {
        g_object_set_data(object, "tvs-hls-query-update", GINT_TO_POINTER(1));
        g_object_set(object, "location", updated.c_str(), nullptr);
        g_object_set_data(object, "tvs-hls-query-update", nullptr);
    }
    g_free(current);
}

void configureHlsHttpSource(GstElement* element, const StreamConfig& cfg) {
    if (!element) return;
    GstElementFactory* factory = gst_element_get_factory(element);
    const gchar* factoryName = factory
        ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
        : nullptr;
    if (!factoryName ||
        (g_strcmp0(factoryName, "souphttpsrc") != 0 && g_strcmp0(factoryName, "curlhttpsrc") != 0)) {
        return;
    }

    setStringPropertyIfPresent(element, "user-agent", cfg.hlsUserAgent);
    setBooleanPropertyIfPresent(element, "keep-alive", TRUE);
    setBooleanPropertyIfPresent(element, "compress", TRUE);
    setIntPropertyIfPresent(element, "timeout", 15);

    if (cfg.hlsAccessKeyMode == "header" && !cfg.hlsAccessKeyName.empty() &&
        !cfg.hlsAccessKeyValue.empty() && hasProperty(element, "extra-headers")) {
        GstStructure* headers = gst_structure_new_empty("extra-headers");
        gst_structure_set(headers,
            cfg.hlsAccessKeyName.c_str(), G_TYPE_STRING, cfg.hlsAccessKeyValue.c_str(),
            nullptr);
        g_object_set(element, "extra-headers", headers, nullptr);
        gst_structure_free(headers);
    }

    if (cfg.hlsAccessKeyMode == "query" && hasProperty(element, "location")) {
        if (!g_object_get_data(G_OBJECT(element), "tvs-hls-location-watch")) {
            // RemapContext owns cfg for the complete pipeline lifetime. Internal
            // hlsdemux download sources set their location after being added to
            // the bin, so watch the property and append the per-stream token to
            // manifests, segments and EXT-X-KEY requests alike.
            g_signal_connect(element, "notify::location", G_CALLBACK(onHlsSourceLocationChanged),
                const_cast<StreamConfig*>(&cfg));
            g_object_set_data(G_OBJECT(element), "tvs-hls-location-watch", GINT_TO_POINTER(1));
        }
        onHlsSourceLocationChanged(G_OBJECT(element), nullptr, const_cast<StreamConfig*>(&cfg));
    }
}

void onHlsDeepElementAdded(GstBin*, GstBin*, GstElement* element, gpointer userData) {
    auto* ctx = static_cast<RemapContext*>(userData);
    if (!ctx) return;
    configureHlsHttpSource(element, ctx->config);
}


struct HttpMpegTsCurlInputState {
    std::atomic<bool> stopping{false};
    std::atomic<uint64_t> bytesReceived{0};
    GstElement* appsrc = nullptr;
    std::thread worker;
    StreamConfig config;
    std::string location;

    HttpMpegTsCurlInputState(GstElement* source, const StreamConfig& cfg, std::string uri)
        : appsrc(source ? GST_ELEMENT(gst_object_ref(source)) : nullptr),
          config(cfg),
          location(std::move(uri)) {}

    ~HttpMpegTsCurlInputState() {
        stop();
        if (appsrc) {
            gst_object_unref(appsrc);
            appsrc = nullptr;
        }
    }

    void stop() {
        stopping.store(true, std::memory_order_relaxed);
        if (appsrc && GST_IS_APP_SRC(appsrc)) {
            gst_app_src_end_of_stream(GST_APP_SRC(appsrc));
        }
        if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) {
            worker.join();
        }
    }
};

size_t httpMpegTsCurlWrite(char* ptr, size_t size, size_t nmemb, void* userData) {
    auto* state = static_cast<HttpMpegTsCurlInputState*>(userData);
    const size_t bytes = size * nmemb;
    if (!state || !ptr || bytes == 0) return bytes;
    if (state->stopping.load(std::memory_order_relaxed) || !state->appsrc) return 0;

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, bytes, nullptr);
    if (!buffer) return 0;
    GstMapInfo map{};
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        return 0;
    }
    std::memcpy(map.data, ptr, bytes);
    gst_buffer_unmap(buffer, &map);

    const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(state->appsrc), buffer);
    if (flow != GST_FLOW_OK) {
        state->stopping.store(true, std::memory_order_relaxed);
        return 0;
    }
    state->bytesReceived.fetch_add(bytes, std::memory_order_relaxed);
    return bytes;
}

int httpMpegTsCurlProgress(void* userData, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* state = static_cast<HttpMpegTsCurlInputState*>(userData);
    return state && state->stopping.load(std::memory_order_relaxed) ? 1 : 0;
}

void postHttpMpegTsError(HttpMpegTsCurlInputState* state, const std::string& message) {
    if (!state || !state->appsrc) return;
    GError* error = g_error_new_literal(
        g_quark_from_static_string("tvs-http-mpegts"), 1, message.c_str());
    GstMessage* gstMessage = gst_message_new_error(
        GST_OBJECT(state->appsrc), error, message.c_str());
    g_error_free(error);
    gst_element_post_message(state->appsrc, gstMessage);
}

void runHttpMpegTsCurlInput(const std::shared_ptr<HttpMpegTsCurlInputState>& state) {
    if (!state || !state->appsrc) return;
    CURL* curl = curl_easy_init();
    if (!curl) {
        postHttpMpegTsError(state.get(), "HTTP MPEG-TS: curl_easy_init failed");
        return;
    }

    struct curl_slist* headers = nullptr;
    if (state->config.hlsAccessKeyMode == "header" &&
        !state->config.hlsAccessKeyName.empty() &&
        !state->config.hlsAccessKeyValue.empty()) {
        const std::string header = state->config.hlsAccessKeyName + ": " + state->config.hlsAccessKeyValue;
        headers = curl_slist_append(headers, header.c_str());
    }
    headers = curl_slist_append(headers, "Accept: */*");
    headers = curl_slist_append(headers, "Accept-Encoding: identity");
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_URL, state->location.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, httpMpegTsCurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, state.get());
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, httpMpegTsCurlProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, state.get());
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        state->config.hlsUserAgent.empty()
            ? "Mozilla/5.0 TVStreammerSAT5"
            : state->config.hlsUserAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    std::cerr << "HTTP MPEG-TS curl input: single_request=1 uri=" << state->location
              << " access=" << (state->config.hlsAccessKeyMode.empty() ? "none" : state->config.hlsAccessKeyMode)
              << " source=libcurl-appsrc" << std::endl;

    const CURLcode result = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    const uint64_t received = state->bytesReceived.load(std::memory_order_relaxed);

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (!state->stopping.load(std::memory_order_relaxed) && result != CURLE_OK) {
        std::ostringstream message;
        message << "HTTP MPEG-TS input failed: " << curl_easy_strerror(result)
                << " http=" << httpCode << " bytes=" << received;
        std::cerr << message.str() << std::endl;
        postHttpMpegTsError(state.get(), message.str());
    }
    if (state->appsrc && GST_IS_APP_SRC(state->appsrc)) {
        gst_app_src_end_of_stream(GST_APP_SRC(state->appsrc));
    }
}

void stopHttpMpegTsInput(StreamState* state) {
    if (!state || !state->httpMpegTsInputState) return;
    auto httpState = std::static_pointer_cast<HttpMpegTsCurlInputState>(state->httpMpegTsInputState);
    if (httpState) httpState->stop();
    state->httpMpegTsInputState.reset();
}

void startHttpMpegTsInput(StreamState* state) {
    if (!state || !state->httpMpegTsInputState) return;
    auto httpState = std::static_pointer_cast<HttpMpegTsCurlInputState>(state->httpMpegTsInputState);
    if (!httpState || httpState->worker.joinable() ||
        httpState->stopping.load(std::memory_order_relaxed)) return;
    try {
        httpState->worker = std::thread(runHttpMpegTsCurlInput, httpState);
    } catch (const std::exception& ex) {
        httpState->stopping.store(true, std::memory_order_relaxed);
        const std::string message = std::string("HTTP MPEG-TS worker thread creation failed: ") + ex.what();
        std::cerr << "Resource guard: " << message << std::endl;
        postHttpMpegTsError(httpState.get(), message);
    }
}

size_t hlsProbeWrite(char* ptr, size_t size, size_t nmemb, void* userData) {
    auto* body = static_cast<std::string*>(userData);
    const size_t bytes = size * nmemb;
    if (!body || !ptr || bytes == 0) return bytes;
    constexpr size_t kProbeLimit = 128 * 1024;
    const size_t remaining = body->size() < kProbeLimit ? kProbeLimit - body->size() : 0;
    const size_t copy = std::min(bytes, remaining);
    body->append(ptr, copy);
    // Playlists are small. Abort large media downloads once the sniff buffer is full.
    return remaining == 0 ? 0 : bytes;
}

bool probeHttpHlsManifest(const StreamConfig& cfg, const std::string& rawUri) {
    const std::string lower = toLower(rawUri);
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) return false;
    if (lower.find(".m3u8") != std::string::npos || toLower(cfg.inputMode) == "hls") return true;

    CURL* curl = curl_easy_init();
    if (!curl) return false;
    const std::string uri = appendHlsAccessQuery(rawUri, cfg);
    std::string body;
    struct curl_slist* headers = nullptr;
    if (cfg.hlsAccessKeyMode == "header" && !cfg.hlsAccessKeyName.empty() && !cfg.hlsAccessKeyValue.empty()) {
        const std::string header = cfg.hlsAccessKeyName + ": " + cfg.hlsAccessKeyValue;
        headers = curl_slist_append(headers, header.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }
    curl_easy_setopt(curl, CURLOPT_URL, uri.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2500L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, hlsProbeWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        cfg.hlsUserAgent.empty() ? "Mozilla/5.0 TVStreammerSAT5" : cfg.hlsUserAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    (void)curl_easy_perform(curl);
    char* contentType = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &contentType);
    const std::string ct = contentType ? toLower(contentType) : "";
    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    const auto first = body.find_first_not_of(" \t\r\n");
    const bool bodyHls = first != std::string::npos && body.compare(first, 7, "#EXTM3U") == 0;
    const bool typeHls = ct.find("mpegurl") != std::string::npos || ct.find("x-mpegurl") != std::string::npos;
    return bodyHls || typeHls;
}

void onStableUdpAudioReservoirRunning(GstElement* queue, gpointer userData) {
    (void)userData;
    if (!queue) {
        return;
    }

    // queue::running fires once the configured startup threshold is satisfied.
    // Drop min-threshold-time to zero at that point so the reservoir is a
    // startup prebuffer only. Leaving min-threshold-time enabled permanently
    // makes queue re-block every time its level later falls below the threshold,
    // which matches the observed rare audio stalls after 30-40 seconds.
    if (g_object_get_data(G_OBJECT(queue), "tvs-audio-reservoir-started")) {
        return;
    }

    g_object_set_data(
        G_OBJECT(queue), "tvs-audio-reservoir-started", GINT_TO_POINTER(1));
    setUInt64PropertyIfPresent(queue, "min-threshold-time", 0);

    std::cerr << "Stable UDP audio reservoir startup complete: "
              << "startup_reservoir_ms=1500 min_threshold_ms=0 "
              << "steady_state=clocksync-only" << std::endl;
}

std::string outputType(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type == "udp_vbr" || type == "udpvbr") {
        type = "udp-vbr";
    } else if (type == "udp_cbr" || type == "udpcbr") {
        type = "udp-cbr";
    }

    if (type != "udp" && type != "udp-vbr" && type != "udp-cbr" && type != "rtp" &&
        type != "srt" && type != "http" && type != "hls" && type != "rtsp" && type != "rtmp" && type != "youtube") {
        type = "udp";
    }
    return type;
}

bool isUdpOutputType(const std::string& type) {
    return type == "udp" || type == "udp-vbr" || type == "udp-cbr";
}

bool usesStableUdpShaper(const StreamConfig& cfg) {
    return isUdpOutputType(outputType(cfg));
}

bool allOutputsUseStableUdp(const StreamConfig& cfg) {
    const auto outputs = tvs::protocols::outputConfigs(cfg);
    return !outputs.empty() && std::all_of(outputs.begin(), outputs.end(), [](const StreamConfig& output) {
        return usesStableUdpShaper(output);
    });
}

bool isUdpOutput(const StreamConfig& cfg) {
    const std::string type = outputType(cfg);
    return isUdpOutputType(type) || type == "rtp";
}

bool udpCbrOutputEnabled(const StreamConfig& cfg) {
    const std::string type = outputType(cfg);
    if (type == "udp-cbr") {
        return true;
    }
    if (type == "udp-vbr") {
        return false;
    }
    return type == "udp" && cfg.cbr && cfg.targetBitrate > 0;
}

bool cbrMuxEnabled(const StreamConfig& cfg) {
    const std::string type = outputType(cfg);
    if (type == "udp-vbr") {
        return false;
    }
    if (type == "udp-cbr") {
        return cfg.targetBitrate > 0;
    }
    return cfg.cbr && cfg.targetBitrate > 0;
}

std::string srtOutputMode(const StreamConfig& cfg);

StreamOutputConfig primaryOutputConfig(const StreamConfig& cfg) {
    StreamOutputConfig output;
    output.outputType = cfg.outputType;
    output.outputMode = cfg.outputMode;
    output.outputHost = cfg.outputHost;
    output.outputPort = cfg.outputPort;
    return output;
}

StreamConfig configForOutput(const StreamConfig& base, const StreamOutputConfig& output) {
    StreamConfig cfg = base;
    cfg.outputType = output.outputType;
    cfg.outputMode = output.outputMode;
    cfg.outputHost = output.outputHost;
    cfg.outputPort = output.outputPort;
    cfg.additionalOutputs.clear();

    const std::string type = outputType(cfg);
    if (type == "udp-cbr") {
        cfg.cbr = true;
    } else if (type == "udp-vbr") {
        cfg.cbr = false;
    }
    return cfg;
}

std::vector<StreamConfig> outputConfigs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    outputs.push_back(configForOutput(cfg, primaryOutputConfig(cfg)));
    for (const auto& output : cfg.additionalOutputs) {
        outputs.push_back(configForOutput(cfg, output));
    }
    return outputs;
}

std::vector<StreamConfig> pipelineOutputConfigs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    bool httpAdded = false;
    bool hlsAdded = false;
    for (const auto& output : outputConfigs(cfg)) {
        const std::string type = outputType(output);
        if (type == "http") {
            if (httpAdded) {
                continue;
            }
            httpAdded = true;
        } else if (type == "hls") {
            if (hlsAdded) {
                continue;
            }
            hlsAdded = true;
        }
        outputs.push_back(output);
    }
    return outputs;
}


bool hasTranscodedHttpOutput(const StreamConfig& cfg) {
    for (const auto& output : outputConfigs(cfg)) {
        if (outputType(output) == "http") {
            return true;
        }
    }
    return false;
}

int connectLocalTcpWithRetry(uint16_t port, std::string& error) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            error = std::string("socket failed: ") + std::strerror(errno);
            return -1;
        }

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
            ::close(fd);
            error = "inet_pton failed for 127.0.0.1";
            return -1;
        }

        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }

        error = std::string("connect to transcoded HTTP relay tcp://127.0.0.1:") +
            std::to_string(port) + " failed: " + std::strerror(errno);
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return -1;
}

bool writeAllToFd(int fd, const char* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = ::write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::string branchName(const std::string& base, size_t branchIndex) {
    return base + "_" + std::to_string(branchIndex);
}

bool hasSrtListenerOutput(const StreamConfig& cfg) {
    for (const auto& output : outputConfigs(cfg)) {
        if (outputType(output) == "srt" && srtOutputMode(output) == "listener") {
            return true;
        }
    }
    return false;
}

uint64_t transcodeVideoBitrateForStats(const StreamConfig& cfg) {
    return std::max<uint64_t>(cfg.transcodeVideoBitrate, 500000);
}

uint64_t transcodeAudioBitrateForStats(const StreamConfig& cfg) {
    return std::clamp<uint64_t>(cfg.transcodeAudioBitrate, 64000, 320000);
}

uint64_t transcodeInputBitrateForStats(const StreamConfig& cfg) {
    // The external GStreamer transcoder owns the input socket, so StreamManager cannot
    // attach its normal source pad probe. Keep the UI graph alive with a conservative
    // estimate derived from the configured encode rate until the transcoder is moved
    // fully in-process.
    return transcodeVideoBitrateForStats(cfg) + transcodeAudioBitrateForStats(cfg) + 300000;
}

uint64_t transcodeMuxBitrateForStats(const StreamConfig& cfg) {
    return tvs::protocols::muxBitrate(cfg);
}

uint16_t transcodeRelayPort(const StreamConfig& cfg) {
    const std::string key = cfg.id.empty() ? cfg.name : cfg.id;
    const size_t hash = std::hash<std::string>{}(key);
    return static_cast<uint16_t>(30000 + (hash % 20000));
}

StreamConfig transcodeRelayOutputConfig(const StreamConfig& cfg) {
    StreamConfig relay = cfg;
    relay.outputType = "fifo";
    relay.outputMode.clear();
    relay.outputHost = tvs::protocols::transcodedFifoRelayPath(cfg);
    relay.outputPort = 0;
    relay.additionalOutputs.clear();
    // The FIFO is an unpaced hand-off between the external encoder and the
    // normal StableUdpOutput stage.  StableUdpOutput owns the final CBR/VBR
    // clock, NULL stuffing, startup reservoir and UDP packetization.
    relay.cbr = false;
    relay.targetBitrate = 0;
    return relay;
}

StreamConfig transcodeRelayPipelineConfig(const StreamConfig& cfg) {
    StreamConfig relay = cfg;
    relay.inputUri = "file://" + tvs::protocols::transcodedFifoRelayPath(cfg);
    relay.inputMode = "file";
    relay.inputInterfaceAddress.clear();
    relay.inputInterfaceAddressConfigured = true;
    relay.testPattern = false;
    relay.transcodeEnabled = false;
    relay.remapEnabled = false;
    return relay;
}

uint64_t initialConfiguredOutputBitrate(const StreamConfig& cfg) {
    if (cfg.transcodeEnabled) {
        return transcodeMuxBitrateForStats(cfg);
    }

    uint64_t total = 0;
    for (const auto& output : pipelineOutputConfigs(cfg)) {
        if (cbrMuxEnabled(output) || udpCbrOutputEnabled(output)) {
            total += output.targetBitrate;
        }
    }
    return total;
}

std::vector<GstElement*> findSinksByFactory(GstElement* pipeline, const char* expectedFactory) {
    std::vector<GstElement*> result;
    if (!pipeline || !expectedFactory) {
        return result;
    }

    GstIterator* iterator = gst_bin_iterate_elements(GST_BIN(pipeline));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
        GstElement* element = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory* factory = gst_element_get_factory(element);
        const gchar* factoryName = factory
            ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
            : nullptr;
        if (factoryName && g_strcmp0(factoryName, expectedFactory) == 0) {
            result.push_back(GST_ELEMENT(gst_object_ref(element)));
        }
        g_value_unset(&item);
    }
    gst_iterator_free(iterator);
    return result;
}

std::string srtOutputMode(const StreamConfig& cfg) {
    const std::string mode = toLower(cfg.outputMode);
    return mode == "caller" ? "caller" : "listener";
}

bool isRtmpUri(const std::string& uriLower) {
    return uriLower.rfind("rtmp://", 0) == 0 ||
           uriLower.rfind("rtmps://", 0) == 0 ||
           uriLower.rfind("rtmpt://", 0) == 0 ||
           uriLower.rfind("rtmpe://", 0) == 0 ||
           uriLower.rfind("rtmpte://", 0) == 0 ||
           uriLower.rfind("rtmpts://", 0) == 0;
}

bool isLocalFileInput(const std::string& input) {
    const std::string inputLower = toLower(input);
    return inputLower.rfind("file://", 0) == 0 || input.find("://") == std::string::npos;
}

bool isBackupFileInput(const StreamConfig& cfg, const std::string& input) {
    return toLower(cfg.backupInputType) == "file" || isLocalFileInput(input);
}

std::string fileLocationFromInput(const std::string& input) {
    if (toLower(input).rfind("file://", 0) != 0) {
        return input;
    }

    gchar* location = gst_uri_get_location(input.c_str());
    if (!location) {
        return input.substr(7);
    }
    std::string result(location);
    g_free(location);
    return result;
}

bool isMpegTsFile(const std::string& input) {
    const std::filesystem::path path(fileLocationFromInput(input));
    const std::string extension = toLower(path.extension().string());
    return extension == ".ts" || extension == ".mts" || extension == ".m2ts" || extension == ".mpegts";
}

std::string hlsDirectory(const StreamConfig& cfg) {
    return "/tmp/tvstreammersat5-hls/" + cfg.id;
}

std::string telegramEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool telegramUsesEnglish(const ConfigManager& manager) {
    return toLower(manager.config.language) == "en";
}

std::string telegramText(const ConfigManager& manager, const char* ru, const char* en) {
    return telegramUsesEnglish(manager) ? en : ru;
}

std::string displayName(const StreamConfig& cfg) {
    return cfg.name.empty() ? cfg.id : cfg.name;
}

std::string configuredInputInterfaceAddress(const StreamConfig& cfg) {
    return cfg.inputInterfaceAddressConfigured
        ? cfg.inputInterfaceAddress
        : cfg.interfaceAddress;
}

uint64_t bufferListSize(GstBufferList* list) {
    if (!list) {
        return 0;
    }

    uint64_t total = 0;
    const guint length = gst_buffer_list_length(list);
    for (guint i = 0; i < length; ++i) {
        GstBuffer* buffer = gst_buffer_list_get(list, i);
        if (buffer) {
            total += gst_buffer_get_size(buffer);
        }
    }
    return total;
}

std::size_t findTsAlignment(const guint8* data, std::size_t size) {
    if (!data || size < kTsPacketSize * 2) {
        return std::string::npos;
    }

    const std::size_t maxOffset = std::min<std::size_t>(kTsPacketSize, size);
    for (std::size_t offset = 0; offset < maxOffset; ++offset) {
        if (data[offset] != 0x47) {
            continue;
        }

        std::size_t confirmed = 1;
        for (std::size_t next = offset + kTsPacketSize;
             next < size && confirmed < 4;
             next += kTsPacketSize) {
            if (data[next] != 0x47) {
                confirmed = 0;
                break;
            }
            ++confirmed;
        }

        // Require at least two consecutive sync bytes. Three or four are
        // automatically checked when enough data is available.
        if (confirmed >= 2) {
            return offset;
        }
    }
    return std::string::npos;
}

uint64_t countContinuityErrors(
    const guint8* data,
    std::size_t size,
    std::array<uint8_t, 8192>& continuity,
    std::array<bool, 8192>& continuityValid,
    std::vector<uint8_t>& remainder,
    std::mutex& continuityMutex) {
    if (!data || size == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(continuityMutex);

    // HTTP and HLS buffers are not guaranteed to begin or end on a 188-byte
    // MPEG-TS boundary. Preserve the incomplete tail and prepend it to the
    // next buffer instead of treating skipped fragments as packet loss.
    std::vector<uint8_t> bytes;
    bytes.reserve(remainder.size() + size);
    bytes.insert(bytes.end(), remainder.begin(), remainder.end());
    bytes.insert(bytes.end(), data, data + size);
    remainder.clear();

    const std::size_t start = findTsAlignment(bytes.data(), bytes.size());
    if (start == std::string::npos) {
        // Retain only enough bytes to detect a sync sequence in the next call.
        const std::size_t keep = std::min<std::size_t>(bytes.size(), kTsPacketSize * 4 - 1);
        remainder.assign(bytes.end() - keep, bytes.end());
        return 0;
    }

    uint64_t errors = 0;
    std::size_t offset = start;
    for (; offset + kTsPacketSize <= bytes.size(); offset += kTsPacketSize) {
        const guint8* packet = bytes.data() + offset;
        if (packet[0] != 0x47) {
            // Lost alignment. Keep the remaining bytes and re-synchronize on
            // the next invocation without reporting synthetic CC errors.
            break;
        }

        if ((packet[1] & 0x80) != 0) {
            ++errors; // transport_error_indicator
        }

        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1f) << 8) | packet[2]);
        if (pid == 0x1fff) {
            continue;
        }

        const guint8 adaptationControl = static_cast<guint8>((packet[3] >> 4) & 0x03);
        if (adaptationControl == 0) {
            ++errors;
            continuityValid[pid] = false;
            continue;
        }

        bool discontinuity = false;
        if ((adaptationControl == 2 || adaptationControl == 3) && packet[4] > 0 && packet[4] <= 183) {
            discontinuity = (packet[5] & 0x80) != 0;
        }
        if (discontinuity) {
            continuityValid[pid] = false;
        }

        const bool hasPayload = adaptationControl == 1 || adaptationControl == 3;
        if (!hasPayload) {
            continue;
        }

        const guint8 continuityCounter = static_cast<guint8>(packet[3] & 0x0f);
        if (continuityValid[pid]) {
            const guint8 expected = static_cast<guint8>((continuity[pid] + 1) & 0x0f);
            if (continuityCounter != expected) {
                ++errors;
            }
        }
        continuity[pid] = continuityCounter;
        continuityValid[pid] = true;
    }

    if (offset < bytes.size()) {
        remainder.assign(bytes.begin() + offset, bytes.end());
        if (remainder.size() > kTsPacketSize * 4) {
            remainder.erase(remainder.begin(), remainder.end() - (kTsPacketSize * 4));
        }
    }
    return errors;
}

struct TransportScramblingCount {
    uint64_t payloadPackets = 0;
    uint64_t scrambledPackets = 0;
    uint64_t clearPesStarts = 0;
};

bool isVideoStreamType(uint8_t streamType) {
    switch (streamType) {
        case 0x01: case 0x02: case 0x10: case 0x1B: case 0x24: case 0x42:
            return true;
        default:
            return false;
    }
}

bool descriptorLoopLooksAudio(const uint8_t* descriptors, size_t length) {
    if (!descriptors) return false;
    size_t pos = 0;
    while (pos + 2 <= length) {
        const uint8_t tag = descriptors[pos];
        const size_t len = descriptors[pos + 1];
        if (pos + 2 + len > length) break;
        if (tag == 0x6A || tag == 0x7A || tag == 0x7B || tag == 0x7C) return true;
        pos += 2 + len;
    }
    return false;
}

bool isAudioStreamType(uint8_t streamType, const uint8_t* descriptors, size_t descriptorLength) {
    switch (streamType) {
        case 0x03: case 0x04: case 0x0F: case 0x11: case 0x81: case 0x87:
            return true;
        case 0x06:
            return descriptorLoopLooksAudio(descriptors, descriptorLength);
        default:
            return false;
    }
}

void seedConfiguredMediaPids(StreamState* state) {
    if (!state || state->outputTelemetryMediaPidsKnown) return;
    if (state->config.videoPid > 0 && state->config.videoPid < 0x1FFF)
        state->outputTelemetryMediaPids[state->config.videoPid] = true;
    if (state->config.audioPid > 0 && state->config.audioPid < 0x1FFF)
        state->outputTelemetryMediaPids[state->config.audioPid] = true;
}

void discoverOutputMediaPids(const uint8_t* packet, StreamState* state) {
    if (!packet || !state || packet[0] != 0x47) return;
    const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
    size_t available = 0;
    const uint8_t* section = tsSectionStart(packet, available);
    if (!section || available < 8) return;

    if (pid == 0x0000 && section[0] == 0x00) {
        const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
        const size_t total = 3 + sectionLength;
        if (sectionLength < 9 || total > available) return;
        const size_t entriesEnd = total - 4;
        for (size_t pos = 8; pos + 4 <= entriesEnd; pos += 4) {
            const uint16_t program = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
            const uint16_t mappedPid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
            if (program != 0 && mappedPid > 0 && mappedPid < 0x1FFF) {
                state->outputTelemetryPmtPid = mappedPid;
                break;
            }
        }
        return;
    }

    if (pid != state->outputTelemetryPmtPid || section[0] != 0x02) return;
    const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
    const size_t total = 3 + sectionLength;
    if (sectionLength < 13 || total > available) return;
    const size_t end = total - 4;
    const size_t programInfoLength = static_cast<size_t>(((section[10] & 0x0F) << 8) | section[11]);
    if (12 + programInfoLength > end) return;

    std::array<bool, 8192> discovered {};
    size_t mediaCount = 0;
    size_t pos = 12 + programInfoLength;
    while (pos + 5 <= end) {
        const uint8_t streamType = section[pos];
        const uint16_t elementaryPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
        const size_t esInfoLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
        if (pos + 5 + esInfoLength > end) break;
        const uint8_t* descriptors = section + pos + 5;
        if (elementaryPid < 0x1FFF &&
            (isVideoStreamType(streamType) || isAudioStreamType(streamType, descriptors, esInfoLength))) {
            discovered[elementaryPid] = true;
            ++mediaCount;
        }
        pos += 5 + esInfoLength;
    }
    if (mediaCount > 0) {
        state->outputTelemetryMediaPids = discovered;
        state->outputTelemetryMediaPidsKnown = true;
    }
}

TransportScramblingCount countTransportScrambling(const guint8* data, std::size_t size, StreamState* state) {
    TransportScramblingCount count;
    if (!data || size == 0 || !state) return count;
    std::lock_guard<std::mutex> lock(state->outputScramblingMutex);
    seedConfiguredMediaPids(state);

    std::vector<uint8_t> bytes;
    bytes.reserve(state->outputScramblingRemainder.size() + size);
    bytes.insert(bytes.end(), state->outputScramblingRemainder.begin(), state->outputScramblingRemainder.end());
    bytes.insert(bytes.end(), data, data + size);
    state->outputScramblingRemainder.clear();

    const std::size_t start = findTsAlignment(bytes.data(), bytes.size());
    if (start == std::string::npos) {
        const std::size_t keep = std::min<std::size_t>(bytes.size(), kTsPacketSize * 4 - 1);
        state->outputScramblingRemainder.assign(bytes.end() - keep, bytes.end());
        return count;
    }

    std::size_t offset = start;
    for (; offset + kTsPacketSize <= bytes.size(); offset += kTsPacketSize) {
        const guint8* packet = bytes.data() + offset;
        if (packet[0] != 0x47) break;
        if ((packet[1] & 0x80) != 0) continue;
        discoverOutputMediaPids(packet, state);
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        if (pid >= state->outputTelemetryMediaPids.size() || !state->outputTelemetryMediaPids[pid]) continue;
        const guint8 adaptationControl = static_cast<guint8>((packet[3] >> 4) & 0x03);
        if (adaptationControl != 1 && adaptationControl != 3) continue;
        ++count.payloadPackets;
        const guint8 scramblingControl = static_cast<guint8>((packet[3] >> 6) & 0x03);
        if (scramblingControl != 0) {
            ++count.scrambledPackets;
        } else if ((packet[1] & 0x40) != 0) {
            const size_t payloadOffset = tsPayloadOffset(packet);
            if (payloadOffset + 3 <= kTsPacketSize &&
                packet[payloadOffset] == 0x00 && packet[payloadOffset + 1] == 0x00 && packet[payloadOffset + 2] == 0x01) {
                ++count.clearPesStarts;
            }
        }
    }

    if (offset < bytes.size()) {
        state->outputScramblingRemainder.assign(bytes.begin() + offset, bytes.end());
        if (state->outputScramblingRemainder.size() > kTsPacketSize * 4) {
            state->outputScramblingRemainder.erase(
                state->outputScramblingRemainder.begin(),
                state->outputScramblingRemainder.end() - (kTsPacketSize * 4));
        }
    }
    return count;
}

void updateOutputScramblingStats(StreamState* state, GstBuffer* buffer) {
    if (!state || !buffer) return;
    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    const auto count = countTransportScrambling(map.data, map.size, state);
    gst_buffer_unmap(buffer, &map);
    if (count.payloadPackets) state->outputTsPayloadPackets.fetch_add(count.payloadPackets, std::memory_order_relaxed);
    if (count.scrambledPackets) state->outputTsScrambledPackets.fetch_add(count.scrambledPackets, std::memory_order_relaxed);
    if (count.clearPesStarts) state->outputTsClearPesStarts.fetch_add(count.clearPesStarts, std::memory_order_relaxed);
}

void updateOutputScramblingStats(StreamState* state, GstBufferList* list) {
    if (!state || !list) return;
    for (guint i = 0; i < gst_buffer_list_length(list); ++i) {
        updateOutputScramblingStats(state, gst_buffer_list_get(list, i));
    }
}

void updateInputContinuityErrors(StreamState* state, GstBuffer* buffer) {
    if (!state || !buffer) return;
    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    const uint64_t errors = countContinuityErrors(
        map.data, map.size, state->inputContinuity, state->inputContinuityValid,
        state->inputTsRemainder, state->inputContinuityMutex);
    gst_buffer_unmap(buffer, &map);
    if (errors > 0) state->inputCcErrors.fetch_add(errors, std::memory_order_relaxed);
}

void updateOutputContinuityErrors(StreamState* state, GstBuffer* buffer) {
    if (!state || !buffer) return;
    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    const uint64_t errors = countContinuityErrors(
        map.data, map.size, state->outputContinuity, state->outputContinuityValid,
        state->outputTsRemainder, state->outputContinuityMutex);
    gst_buffer_unmap(buffer, &map);
    if (errors > 0) state->outputCcErrors.fetch_add(errors, std::memory_order_relaxed);
}

void updateInputContinuityErrors(StreamState* state, GstBufferList* list) {
    if (!state || !list) return;
    for (guint i = 0; i < gst_buffer_list_length(list); ++i) {
        updateInputContinuityErrors(state, gst_buffer_list_get(list, i));
    }
}

void updateOutputContinuityErrors(StreamState* state, GstBufferList* list) {
    if (!state || !list) return;
    for (guint i = 0; i < gst_buffer_list_length(list); ++i) {
        updateOutputContinuityErrors(state, gst_buffer_list_get(list, i));
    }
}

void configureSrtSink(GstElement* sink, const StreamConfig& cfg, bool accessFilteringEnabled) {
    const std::string mode = srtOutputMode(cfg);
    const bool caller = mode == "caller";
    const bool transcoded = cfg.transcodeEnabled;
    const std::string targetHost = cfg.outputHost.empty() || cfg.outputHost == "0.0.0.0" || cfg.outputHost == "::"
        ? "127.0.0.1"
        : cfg.outputHost;
    const std::string bindHost = cfg.interfaceAddress.empty() ? "0.0.0.0" : cfg.interfaceAddress;
    const int effectivePort = (cfg.outputPort > 0 && cfg.outputPort <= 65535) ? cfg.outputPort : 7001;
    const std::string uri = "srt://" + (caller ? targetHost : bindHost) + ":" +
        std::to_string(effectivePort) + "?mode=" + mode;

    g_object_set(sink,
        "uri", uri.c_str(),
        "sync", FALSE,
        "async", FALSE,
        "blocksize", static_cast<guint>(kTsPacketsPerUdpBuffer * 188),
        nullptr);

    setIntPropertyIfPresent(sink, "mode", caller ? 1 : 2);
    setBooleanPropertyIfPresent(sink, "authentication", accessFilteringEnabled ? TRUE : FALSE);
    setBooleanPropertyIfPresent(sink, "wait-for-connection", FALSE);
    if (!caller) {
        setBooleanPropertyIfPresent(sink, "keep-listening", TRUE);
        setUIntPropertyIfPresent(sink, "localport", static_cast<guint>(effectivePort));
    }
    setBooleanPropertyIfPresent(sink, "auto-reconnect", TRUE);
    setBooleanPropertyIfPresent(sink, "qos", FALSE);
    setIntPropertyIfPresent(sink, "latency", transcoded ? 2500 : 250);
    setInt64PropertyIfPresent(sink, "max-lateness", -1);
    setStringPropertyIfPresent(sink, "localaddress", cfg.interfaceAddress);
    if (caller) {
        setUIntPropertyIfPresent(sink, "localport", 0);
    }

    if (transcoded) {
        // The external transcoder already produces and clock-paces a CBR TS.
        // Do not apply GstBaseSink max-bitrate here: targetBitrate may still
        // contain the UI default (for example 2 Mbit/s) while the transcoder
        // is producing 6+ Mbit/s. Throttling the public SRT sink below the
        // real mux bitrate creates periodic queue build-up and A/V stutter.
        setUInt64PropertyIfPresent(sink, "max-bitrate", 0);
    } else if (cfg.targetBitrate > 0) {
        setUInt64PropertyIfPresent(sink, "max-bitrate", static_cast<guint64>(cfg.targetBitrate * 12 / 10));
    }

    std::cerr << "SRT output: mode=" << mode
              << " uri=" << uri
              << " advertised=" << (cfg.outputHost.empty() ? "auto" : cfg.outputHost)
              << ":" << effectivePort
              << " iface=" << (cfg.interfaceAddress.empty() ? "auto" : cfg.interfaceAddress)
              << " auth=" << (accessFilteringEnabled ? "on" : "off")
              << " transcode=" << (transcoded ? "yes" : "no")
              << " latency-ms=" << (transcoded ? 2500 : 250)
              << std::endl;
}

std::string rtmpOutputLocation(const StreamConfig& cfg) {
    const std::string type = outputType(cfg);
    const std::string host = cfg.outputHost;
    const std::string hostLower = toLower(host);

    if (isRtmpUri(hostLower)) {
        return host;
    }

    if (type == "youtube") {
        return "rtmp://a.rtmp.youtube.com/live2/" + host;
    }

    const std::string targetHost = host.empty() ? "127.0.0.1" : host;
    return "rtmp://" + targetHost + ":" + std::to_string(cfg.outputPort) + "/live/" + cfg.id;
}

void configureRtmpSink(GstElement* sink, const StreamConfig& cfg) {
    const std::string location = rtmpOutputLocation(cfg);
    g_object_set(sink,
        "location", location.c_str(),
        "sync", FALSE,
        "async", FALSE,
        "qos", FALSE,
        nullptr);
    setInt64PropertyIfPresent(sink, "max-lateness", -1);
    if (cfg.targetBitrate > 0) {
        setUInt64PropertyIfPresent(sink, "max-bitrate", static_cast<guint64>(cfg.targetBitrate * 12 / 10));
    }
}

void configureHttpSink(GstElement* sink, const StreamConfig& cfg) {
    (void)cfg;
    // Continuous HTTP MPEG-TS must not use GstBaseSink clock scheduling here.
    // The transport is already paced by its source / optional CBR pacer, while
    // GstBuffer timestamps on passthrough MPEG-TS are not guaranteed to form a
    // monotonic network clock.  sync=TRUE can therefore stop multifdsink after
    // the first few seconds even though TS bytes are still arriving upstream.
    // SRT uses the same finished TS with sync=FALSE and is the reference path.
    g_object_set(sink,
        "sync", FALSE,
        "async", FALSE,
        "qos", FALSE,
        nullptr);
    setInt64PropertyIfPresent(sink, "max-lateness", -1);

    // Bound per-client lag so one slow HTTP receiver cannot grow memory without
    // limit.  Resynchronise lagging clients to recent data instead of blocking
    // or destabilising the stream pipeline.
    setIntPropertyIfPresent(sink, "buffers-soft-max", 256);
    setIntPropertyIfPresent(sink, "buffers-max", 512);
    setIntPropertyIfPresent(sink, "recover-policy", 1); // latest
    setUInt64PropertyIfPresent(sink, "timeout", 0);
}

void configureHlsSink(GstElement* sink, const StreamConfig& cfg) {
    const std::filesystem::path directory(hlsDirectory(cfg));
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    // A restarted HLS stream must never expose the previous run's playlist or
    // fragments while the new mux is waiting for its first keyframe.
    if (!ec) {
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const std::string name = entry.path().filename().string();
            if (name == "video.m3u8" ||
                (name.rfind("segment", 0) == 0 && entry.path().extension() == ".ts")) {
                std::error_code removeEc;
                std::filesystem::remove(entry.path(), removeEc);
            }
        }
    }
    const std::string playlist = hlsDirectory(cfg) + "/video.m3u8";
    const std::string location = hlsDirectory(cfg) + "/segment%05d.ts";
    g_object_set(sink,
        "playlist-location", playlist.c_str(),
        "location", location.c_str(),
        "target-duration", 4,
        "max-files", 8,
        nullptr);
    setUIntPropertyIfPresent(sink, "playlist-length", 5);
    setBooleanPropertyIfPresent(sink, "send-keyframe-requests", TRUE);
}

GstElement* createRtpMpegTsSink(const StreamConfig& cfg, const std::string& sinkName) {
    if (!hasElementFactory("rtpmp2tpay") || !hasElementFactory("udpsink")) {
        std::cerr << "missing RTP output elements: rtpmp2tpay or udpsink" << std::endl;
        return nullptr;
    }

    GstElement* bin = gst_bin_new(sinkName.c_str());
    GstElement* pay = gst_element_factory_make("rtpmp2tpay", "rtp_mpegts_pay");
    GstElement* udp = gst_element_factory_make("udpsink", "rtp_udp_sink");
    if (!bin || !pay || !udp) {
        if (bin) gst_object_unref(bin);
        if (pay) gst_object_unref(pay);
        if (udp) gst_object_unref(udp);
        return nullptr;
    }

    gst_bin_add_many(GST_BIN(bin), pay, udp, nullptr);
    setUIntPropertyIfPresent(pay, "mtu", 1400);

    const std::string host = cfg.outputHost.empty() ? "127.0.0.1" : cfg.outputHost;
    g_object_set(udp,
        "host", host.c_str(),
        "port", static_cast<gint>(cfg.outputPort),
        "sync", TRUE,
        "async", FALSE,
        "qos", FALSE,
        "auto-multicast", TRUE,
        "ttl-mc", 32,
        "buffer-size", 8 * 1024 * 1024,
        nullptr);
    setInt64PropertyIfPresent(udp, "max-lateness", -1);
    if (!cfg.interfaceAddress.empty()) {
        setStringPropertyIfPresent(udp, "bind-address", cfg.interfaceAddress);
    }

    if (!gst_element_link(pay, udp)) {
        gst_object_unref(bin);
        return nullptr;
    }

    GstPad* paySink = gst_element_get_static_pad(pay, "sink");
    if (!paySink) {
        gst_object_unref(bin);
        return nullptr;
    }
    GstPad* ghostSink = gst_ghost_pad_new("sink", paySink);
    gst_object_unref(paySink);
    if (!ghostSink || !gst_element_add_pad(bin, ghostSink)) {
        if (ghostSink) gst_object_unref(ghostSink);
        gst_object_unref(bin);
        return nullptr;
    }

    std::cerr << "RTP MPEG-TS output: rtp://" << host << ":" << cfg.outputPort
              << " packetization=7x188 mtu=1400"
              << " iface=" << (cfg.interfaceAddress.empty() ? "auto" : cfg.interfaceAddress)
              << std::endl;
    return bin;
}

void configureQueue(GstElement* queue, guint64 maxSizeTime = 3000000000ULL) {
    if (!queue) {
        return;
    }

    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", maxSizeTime,
        nullptr);
}

void configureOutputQueue(GstElement* queue, const StreamConfig& cfg) {
    configureQueue(queue, isUdpOutput(cfg) ? kUdpQueueLatency : 3000000000ULL);
}

void configureTsPacketAlignment(GstElement* element) {
    setIntPropertyIfPresent(element, "alignment", static_cast<gint>(kTsPacketsPerUdpBuffer));
}

void configureCbrPacer(GstElement* pacer, const StreamConfig& cfg) {
    if (!pacer || !cfg.cbr || cfg.targetBitrate == 0) {
        return;
    }

    g_object_set(pacer,
        "sync", TRUE,
        "silent", TRUE,
        "single-segment", TRUE,
        nullptr);

    const uint64_t bytesPerSecond = cfg.targetBitrate / 8;
    if (bytesPerSecond > 0 && bytesPerSecond <= static_cast<uint64_t>(G_MAXINT)) {
        setIntPropertyIfPresent(pacer, "datarate", static_cast<gint>(bytesPerSecond));
    }
}

void linkDemuxPadToQueue(GstElement* demux, GstPad* pad, gpointer userData) {
    (void)demux;
    auto* queue = static_cast<GstElement*>(userData);
    if (!queue) {
        return;
    }

    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    if (!sinkPad) {
        return;
    }

    if (!gst_pad_is_linked(sinkPad)) {
        GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
        if (ret != GST_PAD_LINK_OK) {
            std::cerr << "HLS demux pad link failed: " << gst_pad_link_get_name(ret) << std::endl;
        }
    }

    gst_object_unref(sinkPad);
}

void configureTsMux(GstElement* mux, const StreamConfig& cfg) {
    g_object_set(mux,
        "alignment", static_cast<gint>(kTsPacketsPerUdpBuffer),
        "pcr-interval", 1800U,
        "pat-interval", 9000U,
        "pmt-interval", 9000U,
        "si-interval", 9000U,
        nullptr);
    const bool externalUdpShaper = usesStableUdpShaper(cfg);
    if (externalUdpShaper) {
        // All UDP MPEG-TS outputs now use the same reservoir/shaper path. Keep
        // mpegtsmux unpadded so the sender can build either strict CBR or
        // source-rate-following VBR from one clean SPTS timeline.
        setUInt64PropertyIfPresent(mux, "bitrate", 0);
    } else if (cbrMuxEnabled(cfg)) {
        setUInt64PropertyIfPresent(mux, "bitrate", static_cast<guint64>(cfg.targetBitrate));
    }
}

void sendServiceDescription(GstElement* mux, const StreamConfig& cfg) {
    if (!mux || (cfg.serviceName.empty() && cfg.serviceProvider.empty())) {
        return;
    }

    GstMpegtsSDT* sdt = gst_mpegts_sdt_new();
    GstMpegtsSDTService* service = gst_mpegts_sdt_service_new();
    if (!sdt || !service) {
        return;
    }

    sdt->actual_ts = TRUE;
    sdt->transport_stream_id = 1;
    sdt->original_network_id = 1;

    service->service_id = static_cast<guint16>(cfg.serviceId ? cfg.serviceId : 1);
    service->EIT_schedule_flag = FALSE;
    service->EIT_present_following_flag = FALSE;
    service->running_status = GST_MPEGTS_RUNNING_STATUS_RUNNING;
    service->free_CA_mode = FALSE;

    GstMpegtsDescriptor* descriptor = gst_mpegts_descriptor_from_dvb_service(
        GST_DVB_SERVICE_DIGITAL_TELEVISION,
        cfg.serviceName.empty() ? cfg.name.c_str() : cfg.serviceName.c_str(),
        cfg.serviceProvider.empty() ? "TVStreammerSAT5" : cfg.serviceProvider.c_str());
    if (descriptor) {
        g_ptr_array_add(service->descriptors, descriptor);
    }
    g_ptr_array_add(sdt->services, service);

    GstMpegtsSection* section = gst_mpegts_section_from_sdt(sdt);
    if (section) {
        gst_mpegts_section_send_event(section, mux);
        gst_mpegts_section_unref(section);
    }
}

GstPad* requestMuxSinkPad(GstElement* mux, uint32_t requestedPid) {
    GstPad* pad = nullptr;
    if (requestedPid > 0) {
        std::string requestedName = "sink_" + std::to_string(requestedPid);
        pad = gst_element_request_pad_simple(mux, requestedName.c_str());
    }
    if (!pad) {
        pad = gst_element_request_pad_simple(mux, "sink_%d");
    }
    return pad;
}

GstPad* requestFlvMuxSinkPad(GstElement* mux, bool isVideo) {
    return gst_element_request_pad_simple(mux, isVideo ? "video" : "audio");
}

uint32_t pidFromDemuxPadName(GstPad* pad) {
    const gchar* padName = pad ? GST_PAD_NAME(pad) : nullptr;
    if (!padName) {
        return 0;
    }

    std::string name(padName);
    const std::size_t separator = name.rfind('_');
    if (separator == std::string::npos || separator + 1 >= name.size()) {
        return 0;
    }

    try {
        const unsigned long pid = std::stoul(name.substr(separator + 1), nullptr, 16);
        return pid <= 0x1FFF ? static_cast<uint32_t>(pid) : 0;
    } catch (...) {
        return 0;
    }
}

void updateMuxProgramMap(RemapContext* ctx) {
    if (!ctx || !ctx->mux || ctx->flvMux || ctx->rtspPush || ctx->hlsSink2 || ctx->programMapApplied) {
        return;
    }

    // mpegtsmux does not reliably accept repeated live prog-map replacements while
    // pads are still being added. Applying a video-only map first and replacing it
    // when audio appears can leave the PMT without a stable audio entry. Wait until
    // both elementary streams are linked, then publish one immutable program map.
    if (ctx->videoPadName.empty() || ctx->audioPadName.empty()) {
        return;
    }

    // Do not install a live prog-map here. Applying it after request pads have
    // started receiving data can split video and audio into separate MPEG-TS
    // programs. The default mpegtsmux mapping places all requested elementary
    // streams in the same program, which is the desired behaviour for one channel.
    ctx->programMapApplied = true;
    std::cerr << "remap using default single-program mapping: video="
              << ctx->videoPadName << " audio=" << ctx->audioPadName << std::endl;
}

std::string parserForCaps(GstCaps* caps, const std::string& capsString) {
    const GstStructure* structure = (caps && gst_caps_get_size(caps) > 0)
        ? gst_caps_get_structure(caps, 0)
        : nullptr;
    const char* mediaType = structure ? gst_structure_get_name(structure) : nullptr;

    if (g_strcmp0(mediaType, "video/x-h264") == 0 || capsString.find("video/x-h264") != std::string::npos) {
        return "h264parse";
    }
    if (g_strcmp0(mediaType, "video/x-h265") == 0 || capsString.find("video/x-h265") != std::string::npos) {
        return "h265parse";
    }
    if (g_strcmp0(mediaType, "video/mpeg") == 0 ||
        capsString.find("video/mpeg,") != std::string::npos) {
        return "mpegvideoparse";
    }
    if (g_strcmp0(mediaType, "audio/mpeg") == 0 || capsString.find("audio/mpeg") != std::string::npos) {
        gint mpegVersion = 0;
        gint layer = 0;
        if (structure) {
            gst_structure_get_int(structure, "mpegversion", &mpegVersion);
            gst_structure_get_int(structure, "layer", &layer);
        }
        if (mpegVersion == 4 || capsString.find("mpegversion=4") != std::string::npos ||
            capsString.find("mpegversion=(int)4") != std::string::npos) {
            return "aacparse";
        }
        if (mpegVersion == 1 || layer == 3 || capsString.find("mpegversion=1") != std::string::npos ||
            capsString.find("mpegversion=(int)1") != std::string::npos) {
            return "mpegaudioparse";
        }
    }
    if (g_strcmp0(mediaType, "audio/x-ac3") == 0 || capsString.find("audio/x-ac3") != std::string::npos) {
        return "ac3parse";
    }
    if (g_strcmp0(mediaType, "audio/x-dts") == 0 || capsString.find("audio/x-dts") != std::string::npos) {
        return "dtsparse";
    }
    return "";
}

GstElement* capsFilterForMux(
    bool flvMux,
    bool isVideo,
    bool isAudio,
    const std::string& capsString,
    const std::string& parserFactory) {
    std::string capsDescription;
    if (flvMux && isVideo && capsString.find("video/x-h264") != std::string::npos) {
        capsDescription = "video/x-h264,stream-format=(string)avc";
    } else if (flvMux && isAudio && parserFactory == "aacparse") {
        capsDescription = "audio/mpeg,mpegversion=(int)4,stream-format=(string)raw";
    } else if (!flvMux && isVideo && capsString.find("video/x-h264") != std::string::npos) {
        capsDescription = "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
    } else if (!flvMux && isAudio && parserFactory == "aacparse") {
        // Do not force raw AAC to ADTS by changing caps. A capsfilter does not add
        // ADTS headers and can produce an AAC PID that is detected but undecodable.
        // Let aacparse negotiate its real output format directly with mpegtsmux.
        capsDescription.clear();
    }

    if (capsDescription.empty()) {
        return nullptr;
    }

    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    if (!filter) {
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string(capsDescription.c_str());
    if (!caps) {
        gst_object_unref(filter);
        return nullptr;
    }
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    return filter;
}

struct RtspPayloadFactories {
    const char* depay = nullptr;
    const char* parser = nullptr;
    const char* muxCaps = nullptr;
    bool isVideo = false;
    bool isAudio = false;
};

RtspPayloadFactories rtspPayloadFactories(const std::string& capsString) {
    const std::string capsLower = toLower(capsString);
    if (capsLower.find("media=(string)video") != std::string::npos &&
        capsLower.find("encoding-name=(string)h264") != std::string::npos) {
        return {"rtph264depay", "h264parse", "video/x-h264,stream-format=(string)byte-stream", true, false};
    }
    if (capsLower.find("media=(string)video") != std::string::npos &&
        (capsLower.find("encoding-name=(string)h265") != std::string::npos ||
         capsLower.find("encoding-name=(string)hevc") != std::string::npos)) {
        return {"rtph265depay", "h265parse", "video/x-h265,stream-format=(string)byte-stream", true, false};
    }
    if (capsLower.find("media=(string)audio") != std::string::npos &&
        (capsLower.find("encoding-name=(string)mpeg4-generic") != std::string::npos ||
         capsLower.find("encoding-name=(string)mp4a-latm") != std::string::npos)) {
        return {"rtpmp4gdepay", "aacparse", nullptr, false, true};
    }
    if (capsLower.find("media=(string)audio") != std::string::npos &&
        capsLower.find("encoding-name=(string)mpa") != std::string::npos) {
        return {"rtpmpadepay", "mpegaudioparse", nullptr, false, true};
    }
    return {};
}

GstElement* makeCapsFilter(const char* capsDescription) {
    if (!capsDescription) {
        return nullptr;
    }

    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    GstCaps* caps = gst_caps_from_string(capsDescription);
    if (!filter || !caps) {
        if (filter) gst_object_unref(filter);
        if (caps) gst_caps_unref(caps);
        return nullptr;
    }

    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    return filter;
}


bool isExternalSrtListenerOutput(const StreamConfig& outputConfig) {
    return tvs::protocols::outputKind(outputConfig) == tvs::protocols::OutputKind::Srt &&
           tvs::protocols::srtOutputMode(outputConfig) != "caller";
}

} // namespace

StreamManager::StreamManager(ConfigManager& cfg, TelegramNotifier& notifier)
    : configManager(cfg), telegramNotifier(notifier), gstreamerInitialized(gst_is_initialized()) {
    std::cerr << "StreamManager constructed" << std::endl;
}

StreamManager::~StreamManager() {
    stopAll();
}

uint16_t StreamManager::allocateDvbServiceRelayPort(const std::string& streamId) {
    constexpr uint16_t kFirstPort = 47000;
    constexpr uint16_t kPortCount = 12000;
    const size_t hash = std::hash<std::string>{}(streamId);
    std::lock_guard<std::mutex> lock(managerMutex);
    for (uint16_t offset = 0; offset < kPortCount; ++offset) {
        const uint16_t port = static_cast<uint16_t>(kFirstPort + ((hash + offset) % kPortCount));
        if (dvbServiceRelayPorts.insert(port).second) return port;
    }
    return 0;
}

void StreamManager::releaseDvbServiceRelayPort(uint16_t port) {
    if (!port) return;
    std::lock_guard<std::mutex> lock(managerMutex);
    dvbServiceRelayPorts.erase(port);
}

bool StreamManager::acquireSharedDvbFrontend(StreamState* state, std::string& error) {
    if (!state) {
        error = "invalid stream state";
        return false;
    }

    DvbSatelliteParams params;
    if (!DvbSatellite::parseUri(state->runtimeConfig.inputUri, params, error)) {
        if (error.empty()) error = "invalid DVB URI";
        return false;
    }

    const std::string frontendKey = sharedDvbFrontendKey(params);
    const std::string tuningSignature = sharedDvbTuneSignature(params);
    const std::string consumerId = state->config.id.empty()
        ? state->runtimeConfig.id : state->config.id;
    const std::set<uint16_t> servicePids = requestedDvbPids(params);
    std::map<std::string, std::set<uint16_t>> initialConsumers;
    initialConsumers[consumerId] = servicePids;
    std::string frontendPids;
    size_t pidFilterCount = 0;
    if (!buildSharedDvbPids(
            initialConsumers, frontendPids, pidFilterCount, error)) {
        return false;
    }
    // v167 shared frontends are permanently full-MPTS. Keep servicePids only
    // for the downstream software relay; never apply them to dvbsrc itself.
    frontendPids = "8192";
    pidFilterCount = 1;
    const std::string device = "/dev/dvb/adapter" + std::to_string(params.adapter) +
                               "/frontend" + std::to_string(params.frontend);
    if (!std::filesystem::exists(device)) {
        error = "DVB frontend device not found: " + device +
                "; check the adapter number and tuner driver";
        return false;
    }
    if (access(device.c_str(), R_OK | W_OK) != 0) {
        error = "DVB frontend device is not accessible: " + device +
                " (" + std::strerror(errno) + ")";
        return false;
    }

    // Use the same per-frontend tune gate as /api/dvb-signal and /api/dvb-scan.
    // This closes the race where the Add Channel modal successfully scans a
    // tuner, then its background signal probe still owns dvbsrc while the newly
    // created tile immediately tries to open the same frontend.
    const auto frontendTuneWaitStarted = std::chrono::steady_clock::now();
    auto frontendTuneGuard = DvbSatellite::acquireFrontendTuneGuard(params);
    const auto frontendTuneWaitMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - frontendTuneWaitStarted).count();
    if (frontendTuneWaitMs >= 10) {
        std::cerr << "DVB stream startup waited for scan/signal frontend gate: "
                  << frontendKey << " wait_ms=" << frontendTuneWaitMs << std::endl;
    }

    {
        std::unique_lock<std::mutex> lock(managerMutex);
        if (releasingDvbFrontends.count(frontendKey) != 0) {
            std::cerr << "DVB frontend release barrier: waiting for " << frontendKey
                      << " before tuning a new stream" << std::endl;
            const bool released = dvbReleaseCondition.wait_for(
                lock,
                std::chrono::milliseconds(2500),
                [&]() { return releasingDvbFrontends.count(frontendKey) == 0; });
            if (!released) {
                std::ostringstream ss;
                ss << "DVB adapter " << params.adapter << " frontend " << params.frontend
                   << " is still being released by the previous stream; retry start. "
                   << "A different transponder/satellite requires the previous tune to be fully stopped "
                   << "or another physical frontend";
                error = ss.str();
                return false;
            }
            std::cerr << "DVB frontend release barrier cleared: " << frontendKey << std::endl;
        }
        auto existing = sharedDvbFrontends.find(frontendKey);
        if (existing != sharedDvbFrontends.end()) {
            if (existing->second->tuningSignature != tuningSignature) {
                std::ostringstream ss;
                ss << "DVB adapter " << params.adapter << " frontend " << params.frontend
                   << " is already used by another transponder; stop those channels or select another adapter";
                error = ss.str();
                return false;
            }
            auto activeConsumers = existing->second->consumerPids;
            activeConsumers[consumerId] = servicePids;
            size_t activeFilterCount = 0;
            if (!applySharedDvbPids(
                    *existing->second, activeConsumers, activeFilterCount, error,
                    false, existing->second->fullTsCapture)) {
                return false;
            }
            ++existing->second->consumers;
            state->sharedDvbInput = true;
            state->sharedDvbFrontendKey = frontendKey;
            state->sharedDvbMulticastAddress = existing->second->multicastAddress;
            state->sharedDvbMulticastPort = existing->second->multicastPort;
            std::cerr << "Shared DVB frontend reused: " << frontendKey
                      << " consumers=" << existing->second->consumers
                      << " pid_services=" << existing->second->consumerPids.size()
                      << " pid_filters=" << activeFilterCount
                      << " pids=" << existing->second->requestedPids
                      << " dispatcher=single-pass-in-process" << std::endl;
            return true;
        }
    }

    auto shared = std::make_unique<SharedDvbFrontendState>();
    shared->tuningSignature = tuningSignature;
    shared->multicastAddress = sharedDvbMulticastAddress(params);
    shared->multicastPort = sharedDvbMulticastPort(params);
    shared->consumers = 1;
    shared->consumerPids = initialConsumers;
    shared->requestedPids = frontendPids;

    GstElement* pipeline = gst_pipeline_new(("dvb_shared_" + std::to_string(params.adapter) + "_" + std::to_string(params.frontend)).c_str());
    GstElement* source = gst_element_factory_make("dvbsrc", "shared_dvb_src");
    GstElement* queue = gst_element_factory_make("queue", "shared_dvb_queue");
    GstElement* sink = gst_element_factory_make("fakesink", "shared_dvb_dispatch_sink");
    if (!pipeline || !source || !queue || !sink ||
        !addElementOrFail(pipeline, source) ||
        !addElementOrFail(pipeline, queue) || !addElementOrFail(pipeline, sink)) {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
        error = "failed to create shared DVB frontend pipeline";
        return false;
    }

    DvbSatelliteParams sharedInputParams = params;
    sharedInputParams.pids = frontendPids;
    if (!DvbSatellite::configureSource(source, sharedInputParams, error)) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        if (error.empty()) error = "failed to configure shared DVB frontend";
        return false;
    }

    // Heavy DVB packet diagnostics are intentionally disabled in the normal
    // hot path.  Enable them only when troubleshooting with
    // TVS_DVB_DIAGNOSTICS=1.
    if (dvbDiagnosticsEnabled()) {
        // v155 byte-for-byte diagnostics at the DVB source. This probe publishes
        // recent exact TS packet fingerprints for the corresponding service relay
        // receiver to correlate against. It is read-only.
        {
            GstPad* auditPad = gst_element_get_static_pad(source, "src");
            if (auditPad) {
                auto* auditContext = new DvbByteAuditContext();
                auditContext->frontendKey = frontendKey;
                auditContext->stage = "dvbsrc-src";
                auditContext->publishSourceHistory = true;
                gst_pad_add_probe(
                    auditPad, GST_PAD_PROBE_TYPE_BUFFER,
                    dvbByteAuditProbe, auditContext,
                    [](gpointer data) { delete static_cast<DvbByteAuditContext*>(data); });
                gst_object_unref(auditPad);
            }
        }

        GstPad* statsPad = gst_element_get_static_pad(source, "src");
        if (statsPad) {
            auto* statsContext = new SharedDvbPidStatsContext();
            statsContext->stage = "dvbsrc-src";
            statsContext->label = frontendKey;
            gst_pad_add_probe(
                statsPad,
                GST_PAD_PROBE_TYPE_BUFFER,
                sharedDvbPidStatsProbe,
                statsContext,
                [](gpointer data) { delete static_cast<SharedDvbPidStatsContext*>(data); });
            gst_object_unref(statsPad);
        }

        // Same cumulative fingerprint immediately before the v178 in-process
        // dispatcher. At equal packet counts its hash must match dvbsrc-src.
        {
            GstPad* auditPad = gst_element_get_static_pad(queue, "src");
            if (auditPad) {
                auto* auditContext = new DvbByteAuditContext();
                auditContext->frontendKey = frontendKey;
                auditContext->stage = "pre-single-pass-dispatch";
                gst_pad_add_probe(
                    auditPad, GST_PAD_PROBE_TYPE_BUFFER,
                    dvbByteAuditProbe, auditContext,
                    [](gpointer data) { delete static_cast<DvbByteAuditContext*>(data); });
                gst_object_unref(auditPad);
            }
        }
    }

    configureQueue(queue, 12000000000ULL);
    g_object_set(sink,
        "sync", FALSE,
        "async", FALSE,
        nullptr);
    setBooleanPropertyIfPresent(sink, "qos", FALSE);

    // v178: the shared frontend is consumed in-process.  A single pad probe
    // frames the MPTS once and dispatches only selected SPTS packets to each
    // service.  The fakesink merely drains the GStreamer source; no full-MPTS
    // localhost multicast copy is generated.
    auto dispatcher = std::make_shared<SharedDvbDispatcherState>();
    shared->dispatcherState = dispatcher;
    GstPad* dispatchPad = gst_element_get_static_pad(queue, "src");
    if (!dispatchPad) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        error = "failed to attach shared DVB single-pass dispatcher";
        return false;
    }
    gst_pad_add_probe(
        dispatchPad, GST_PAD_PROBE_TYPE_BUFFER,
        sharedDvbDispatcherProbe, dispatcher.get(), nullptr);
    gst_object_unref(dispatchPad);

    if (!gst_element_link_many(source, queue, sink, nullptr)) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        error = "failed to link shared DVB frontend pipeline";
        return false;
    }

    shared->pipeline = pipeline;
    shared->source = source;
    shared->bus = gst_element_get_bus(pipeline);

    const auto appendStartupContext = [&]() {
        std::ostringstream context;
        context << " [device=/dev/dvb/adapter" << params.adapter
                << "/frontend" << params.frontend
                << " frequency_khz=" << params.frequencyKHz
                << " symbol_rate=" << params.symbolRateK
                << " polarity=" << params.polarity
                << "; check device existence, permissions, adapter use and signal]";
        return context.str();
    };
    const auto resetFailedStart = [&]() {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND);
    };
    const auto startWithCurrentPids = [&]() {
        const GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        std::string startupError;
        const bool ok = waitForGstStartup(
            pipeline, shared->bus, stateResult, 8 * GST_SECOND,
            "failed to start shared DVB frontend", startupError);
        return std::pair<bool, std::string>(ok, startupError);
    };
    const auto startWithCurrentPidsRetry = [&](const char* mode) {
        std::string lastError;
        for (int attempt = 1; attempt <= 3; ++attempt) {
            auto result = startWithCurrentPids();
            if (result.first) {
                if (attempt > 1) {
                    std::cerr << "Shared DVB frontend startup retry succeeded: stream="
                              << state->config.id << " mode=" << mode
                              << " attempt=" << attempt << std::endl;
                }
                return result;
            }
            lastError = result.second;
            if (attempt < 3) {
                std::cerr << "Shared DVB frontend startup retry: stream=" << state->config.id
                          << " mode=" << mode
                          << " attempt=" << attempt
                          << " error=" << lastError << std::endl;
                resetFailedStart();
                std::this_thread::sleep_for(std::chrono::milliseconds(700));
            }
        }
        return std::pair<bool, std::string>(false, lastError);
    };

    auto [started, startupError] = startWithCurrentPidsRetry("full-ts");
    if (!started) {
        if (startupError.find("Device or resource busy") != std::string::npos ||
            startupError.find("resource busy") != std::string::npos) {
            startupError +=
                "; frontend is occupied. One physical DVB frontend can share channels only "
                "from the same transponder. For another transponder/satellite stop the current "
                "consumer first or select another adapter/frontend";
        }
        error = startupError + appendStartupContext();
        resetFailedStart();
        if (shared->bus) {
            gst_object_unref(shared->bus);
            shared->bus = nullptr;
        }
        gst_object_unref(pipeline);
        shared->pipeline = nullptr;
        return false;
    }

    shared->requestedPids = frontendPids;
    shared->fullTsCapture = true;

    state->sharedDvbInput = true;
    state->sharedDvbFrontendKey = frontendKey;
    state->sharedDvbMulticastAddress = shared->multicastAddress;
    state->sharedDvbMulticastPort = shared->multicastPort;

    {
        std::lock_guard<std::mutex> lock(managerMutex);
        // Another concurrent start may have won the race while this pipeline was
        // transitioning. Do not keep two owners of one frontend.
        auto existing = sharedDvbFrontends.find(frontendKey);
        if (existing != sharedDvbFrontends.end()) {
            if (existing->second->tuningSignature != tuningSignature) {
                gst_element_set_state(shared->pipeline, GST_STATE_NULL);
                gst_element_get_state(shared->pipeline, nullptr, nullptr, GST_SECOND);
                if (shared->bus) gst_object_unref(shared->bus);
                gst_object_unref(shared->pipeline);
                state->sharedDvbInput = false;
                state->sharedDvbFrontendKey.clear();
                error = "DVB frontend became busy with a different transponder";
                return false;
            }
            auto activeConsumers = existing->second->consumerPids;
            activeConsumers[consumerId] = servicePids;
            size_t activeFilterCount = 0;
            if (!applySharedDvbPids(
                    *existing->second, activeConsumers, activeFilterCount, error,
                    false, existing->second->fullTsCapture)) {
                gst_element_set_state(shared->pipeline, GST_STATE_NULL);
                gst_element_get_state(shared->pipeline, nullptr, nullptr, GST_SECOND);
                if (shared->bus) gst_object_unref(shared->bus);
                gst_object_unref(shared->pipeline);
                state->sharedDvbInput = false;
                state->sharedDvbFrontendKey.clear();
                return false;
            }
            ++existing->second->consumers;
            state->sharedDvbMulticastAddress = existing->second->multicastAddress;
            state->sharedDvbMulticastPort = existing->second->multicastPort;
            gst_element_set_state(shared->pipeline, GST_STATE_NULL);
            gst_element_get_state(shared->pipeline, nullptr, nullptr, GST_SECOND);
            if (shared->bus) gst_object_unref(shared->bus);
            gst_object_unref(shared->pipeline);
            std::cerr << "Shared DVB frontend reused after concurrent start: " << frontendKey
                      << " consumers=" << existing->second->consumers << std::endl;
            return true;
        }
        sharedDvbFrontends[frontendKey] = std::move(shared);
    }

    std::cerr << "Shared DVB frontend started: " << frontendKey
              << " frequency_khz=" << params.frequencyKHz
              << " symbol_rate=" << params.symbolRateK
              << " polarity=" << params.polarity
              << " pid_mode=full-mpts-software-service-filter"
              << " pid_services=" << initialConsumers.size()
              << " pid_filters=" << pidFilterCount
              << " pids=" << frontendPids
              << " dispatcher=single-pass-in-process"
              << " internal_multicast=off" << std::endl;
    return true;
}

void StreamManager::releaseSharedDvbFrontend(StreamState* state) {
    if (!state || !state->sharedDvbInput || state->sharedDvbFrontendKey.empty()) return;
    std::unique_ptr<SharedDvbFrontendState> released;
    const std::string key = state->sharedDvbFrontendKey;
    const std::string consumerId = state->config.id.empty()
        ? state->runtimeConfig.id : state->config.id;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto found = sharedDvbFrontends.find(key);
        if (found != sharedDvbFrontends.end()) {
            if (found->second->consumers > 1) {
                auto activeConsumers = found->second->consumerPids;
                activeConsumers.erase(consumerId);
                size_t activeFilterCount = 0;
                std::string pidError;
                if (!applySharedDvbPids(
                        *found->second, activeConsumers, activeFilterCount,
                        pidError, false, found->second->fullTsCapture)) {
                    std::cerr << "Shared DVB PID filter shrink failed: " << key
                              << " error=" << pidError << std::endl;
                }
                --found->second->consumers;
                std::cerr << "Shared DVB frontend retained: " << key
                          << " consumers=" << found->second->consumers
                          << " pid_services=" << found->second->consumerPids.size()
                          << " pid_filters=" << activeFilterCount
                          << " pids=" << found->second->requestedPids
                          << " pid_update=live-no-ready-cycle" << std::endl;
            } else {
                released = std::move(found->second);
                sharedDvbFrontends.erase(found);
                releasingDvbFrontends.insert(key);
            }
        }
    }
    if (released) {
        const auto releaseStarted = std::chrono::steady_clock::now();
        if (released->pipeline) {
            gst_element_set_state(released->pipeline, GST_STATE_NULL);
            gst_element_get_state(
                released->pipeline, nullptr, nullptr, kFastDvbReleaseWait);
        }
        if (released->bus) gst_object_unref(released->bus);
        if (released->pipeline) gst_object_unref(released->pipeline);
        const auto releaseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - releaseStarted).count();
        std::cerr << "Shared DVB frontend stopped: " << key
                  << " adapter_release_ms=" << releaseMs << std::endl;
        {
            std::lock_guard<std::mutex> lock(managerMutex);
            releasingDvbFrontends.erase(key);
        }
        dvbReleaseCondition.notify_all();
    }
    state->sharedDvbInput = false;
    state->sharedDvbFrontendKey.clear();
    state->sharedDvbMulticastAddress.clear();
    state->sharedDvbMulticastPort = 0;
    state->sharedDvbServicePids.clear();
    state->sharedDvbPreferFullTsCapture = false;
}

bool StreamManager::startDvbServiceRelay(StreamState* state, std::string& error) {
    if (!state || !state->sharedDvbInput || state->sharedDvbFrontendKey.empty()) {
        error = "shared DVB frontend is not ready";
        return false;
    }
    if (state->config.inputServiceId == 0) {
        error = "shared DVB single-pass dispatcher requires an input service id";
        return false;
    }

    DvbSatelliteParams params;
    if (!DvbSatellite::parseUri(state->runtimeConfig.inputUri, params, error)) {
        if (error.empty()) error = "invalid DVB URI for service dispatcher";
        return false;
    }
    const std::string servicePidFilter = state->sharedDvbServicePids.empty()
        ? params.pids
        : state->sharedDvbServicePids;

    const uint32_t remapOutputSid = state->config.serviceId
        ? state->config.serviceId
        : state->config.inputServiceId;
    const bool remapVideoPidValid = isValidDvbElementaryPid(state->config.videoPid);
    const bool remapAudioPidValid = isValidDvbElementaryPid(state->config.audioPid);
    const bool remapPidFieldsConfigured = state->config.videoPid != 0 || state->config.audioPid != 0;
    const bool dvbPacketPidRemap = state->config.remapEnabled &&
        remapVideoPidValid && remapAudioPidValid &&
        state->config.videoPid != state->config.audioPid;

    if (state->config.remapEnabled) {
        if (remapOutputSid == 0 || remapOutputSid > 0xFFFF) {
            error = "DVB remap requires a valid output SID or input SID (1..65535)";
            return false;
        }
        if (remapPidFieldsConfigured && !dvbPacketPidRemap) {
            error = "DVB remap PID mode requires distinct V-PID/A-PID (32..8190); clear both for SID-only remap";
            return false;
        }
    }

    const uint16_t outputPort = allocateDvbServiceRelayPort(state->config.id);
    if (!outputPort) {
        error = "no free internal UDP port for DVB single-pass service output";
        return false;
    }

    auto consumer = std::make_shared<SharedDvbDispatchConsumer>();
    consumer->streamId = state->config.id;
    consumer->frontendKey = state->sharedDvbFrontendKey;
    consumer->caEnabled = !state->config.conditionalAccessClient.empty();
    consumer->psi.serviceId = static_cast<uint16_t>(state->config.inputServiceId & 0xFFFFU);
    consumer->psi.serviceName = state->config.serviceName;
    consumer->psi.serviceProvider = state->config.serviceProvider;
    consumer->psi.remapEnabled = state->config.remapEnabled;
    consumer->psi.outputServiceId = static_cast<uint16_t>(remapOutputSid & 0xFFFFU);
    consumer->psi.requestedVideoPid = dvbPacketPidRemap
        ? static_cast<uint16_t>(state->config.videoPid & 0x1FFFU)
        : 0;
    consumer->psi.requestedAudioPid = dvbPacketPidRemap
        ? static_cast<uint16_t>(state->config.audioPid & 0x1FFFU)
        : 0;

    const bool savedPidFilter = configureServicePidFilter(consumer->psi, servicePidFilter);
    if (!savedPidFilter) {
        // No scan PID list is available.  Keep the dispatcher selective anyway:
        // PAT discovers this SID's PMT and the PMT self-heals PCR/ES/ECM PIDs.
        // Passing the whole MPTS here would recreate the CPU regression v178 is
        // intended to remove.
        consumer->psi.filterPids = true;
        std::cerr << "Shared DVB dispatcher dynamic PID discovery: stream=" << state->config.id
                  << " SID=" << state->config.inputServiceId
                  << " source=PAT/PMT saved_pids=none" << std::endl;
    }

    consumer->socketFd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (consumer->socketFd < 0) {
        releaseDvbServiceRelayPort(outputPort);
        error = "failed to create localhost UDP socket for DVB dispatcher";
        return false;
    }
    int sendBuffer = 8 * 1024 * 1024;
    (void)::setsockopt(
        consumer->socketFd, SOL_SOCKET, SO_SNDBUF,
        &sendBuffer, static_cast<socklen_t>(sizeof(sendBuffer)));
    std::memset(&consumer->destination, 0, sizeof(consumer->destination));
    consumer->destination.sin_family = AF_INET;
    consumer->destination.sin_port = htons(outputPort);
    consumer->destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::shared_ptr<SharedDvbDispatcherState> dispatcher;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto found = sharedDvbFrontends.find(state->sharedDvbFrontendKey);
        if (found == sharedDvbFrontends.end() || !found->second || !found->second->dispatcherState) {
            releaseDvbServiceRelayPort(outputPort);
            error = "shared DVB single-pass dispatcher disappeared before service attach";
            return false;
        }
        dispatcher = std::static_pointer_cast<SharedDvbDispatcherState>(found->second->dispatcherState);
    }

    {
        std::lock_guard<std::mutex> dispatchLock(dispatcher->mutex);
        unsigned slot = 64;
        for (unsigned i = 0; i < dispatcher->slots.size(); ++i) {
            if (!dispatcher->slots[i]) {
                slot = i;
                break;
            }
        }
        if (slot >= dispatcher->slots.size()) {
            releaseDvbServiceRelayPort(outputPort);
            error = "shared DVB dispatcher service limit reached (64)";
            return false;
        }
        consumer->slot = slot;
        dispatcher->slots[slot] = consumer;
        dispatcher->streamSlots[state->config.id] = slot;
        rebuildSharedDvbPidRoutes(*dispatcher);
    }

    auto relay = std::make_unique<DvbServiceRelayState>();
    relay->outputPort = outputPort;
    relay->dispatcherConsumer = consumer;
    state->dvbServiceRelay = std::move(relay);
    state->sharedDvbServiceRelayUri = "udp://127.0.0.1:" + std::to_string(outputPort);
    state->dvbTsRemapApplied = state->config.remapEnabled;

    if (state->dvbTsRemapApplied) {
        std::cerr << "DVB remap metadata configured: SID=" << remapOutputSid
                  << " service=\"" << (state->config.serviceName.empty() ? state->config.name : state->config.serviceName) << "\""
                  << " provider=\"" << state->config.serviceProvider << "\""
                  << " target_bitrate=" << state->config.targetBitrate
                  << " output_type=" << state->config.outputType << std::endl;
    }

    std::cerr << "Shared DVB single-pass service attached: stream=" << state->config.id
              << " SID=" << state->config.inputServiceId
              << " frontend=" << state->sharedDvbFrontendKey
              << " slot=" << consumer->slot
              << " service_pids=" << (servicePidFilter.empty() ? "dynamic" : servicePidFilter)
              << " ca=" << (consumer->caEnabled ? "selected-spts" : "off")
              << " source=" << state->sharedDvbServiceRelayUri
              << " per_service_gstreamer_relay=off"
              << " full_mpts_copy=off" << std::endl;
    return true;
}

void StreamManager::stopDvbServiceRelay(StreamState* state) {
    if (!state || !state->dvbServiceRelay) return;
    auto relay = std::move(state->dvbServiceRelay);
    auto consumer = std::static_pointer_cast<SharedDvbDispatchConsumer>(relay->dispatcherConsumer);

    std::shared_ptr<SharedDvbDispatcherState> dispatcher;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto found = sharedDvbFrontends.find(state->sharedDvbFrontendKey);
        if (found != sharedDvbFrontends.end() && found->second && found->second->dispatcherState) {
            dispatcher = std::static_pointer_cast<SharedDvbDispatcherState>(found->second->dispatcherState);
        }
    }

    if (dispatcher && consumer) {
        std::lock_guard<std::mutex> dispatchLock(dispatcher->mutex);
        auto it = dispatcher->streamSlots.find(state->config.id);
        if (it != dispatcher->streamSlots.end()) {
            const unsigned slot = it->second;
            if (slot < dispatcher->slots.size() && dispatcher->slots[slot] == consumer) {
                flushSharedDvbConsumerDatagram(*consumer, true);
                dispatcher->slots[slot].reset();
            }
            dispatcher->streamSlots.erase(it);
            rebuildSharedDvbPidRoutes(*dispatcher);
        }
    }

    releaseDvbServiceRelayPort(relay->outputPort);
    state->sharedDvbServiceRelayUri.clear();
    state->dvbTsRemapApplied = false;
}

bool StreamManager::prepareSharedDvbInput(StreamState* state, std::string& error) {
    if (!state) {
        error = "invalid stream state";
        return false;
    }
    state->runtimeConfig = state->config;
    state->sharedDvbServicePids.clear();
    state->sharedDvbPreferFullTsCapture = false;
    if (state->config.testPattern || !DvbSatellite::isDvbUri(state->config.inputUri)) return true;

    DvbSatelliteParams params;
    if (!DvbSatellite::parseUri(state->runtimeConfig.inputUri, params, error)) return false;

    // Older configurations may not have a saved service PID list. Resolve it
    // before the shared frontend is opened so the resolver is not blocked by
    // our own exclusive tuner session.
    if (state->config.inputServiceId > 0 && (params.pids.empty() || params.pids == "8192")) {
        std::string resolvedPids;
        bool scrambled = false;
        std::string resolveError;
        if (DvbSatellite::resolveServicePids(params, state->config.inputServiceId, resolvedPids, scrambled, resolveError)) {
            params.pids = resolvedPids;
            state->runtimeConfig.inputUri = DvbSatellite::buildUri(params);
            std::cerr << "Shared DVB service PID auto-resolve: SID=" << state->config.inputServiceId
                      << " pids=" << params.pids << std::endl;
        } else {
            std::cerr << "Shared DVB service PID auto-resolve failed for SID=" << state->config.inputServiceId
                      << ": " << resolveError << "; continuing without PID compaction" << std::endl;
        }
    }

    state->sharedDvbServicePids = params.pids;
    state->sharedDvbPreferFullTsCapture = !state->config.conditionalAccessClient.empty() &&
        state->config.inputServiceId > 0 &&
        !params.pids.empty() && params.pids != "8192";
    if (state->sharedDvbPreferFullTsCapture) {
        std::cerr << "Shared DVB CA frontend full-TS preferred: stream=" << state->config.id
                  << " SID=" << state->config.inputServiceId
                  << " software_service_pids=" << state->sharedDvbServicePids
                  << " frontend_mode=full-mpts" << std::endl;
    }

    if (!acquireSharedDvbFrontend(state, error)) return false;
    if (!startDvbServiceRelay(state, error)) {
        releaseSharedDvbFrontend(state);
        return false;
    }

    state->runtimeConfig = state->config;
    state->runtimeConfig.inputUri = state->sharedDvbServiceRelayUri;
    state->runtimeConfig.inputMode = "udp";
    state->runtimeConfig.inputInterfaceAddress.clear();
    state->runtimeConfig.inputInterfaceAddressConfigured = true;
    // The local relay is already a selected SPTS. Keep the original SID in
    // state->config for UI/telemetry, but do not ask generic UDP paths to select
    // the same SID again.
    state->runtimeConfig.inputServiceId = 0;
    std::cerr << "Shared DVB input attached: stream=" << state->config.id
              << " frontend=" << state->sharedDvbFrontendKey
              << " SID=" << state->config.inputServiceId
              << " source=" << state->sharedDvbServiceRelayUri << std::endl;
    return true;
}

void StreamManager::releaseSharedDvbInput(StreamState* state) {
    if (!state) return;
    // v178: unregister the in-process dispatcher consumer first.  The physical
    // frontend owns the dispatcher, so destroying the last frontend before the
    // consumer is detached would race the streaming pad probe.
    stopDvbServiceRelay(state);
    releaseSharedDvbFrontend(state);
    state->runtimeConfig = state->config;
}

void StreamManager::attachSrtConnectionMonitoring(GstElement* sink, const StreamConfig& cfg) {
    if (!sink) {
        return;
    }

    auto* ctx = new SrtAccessContext{this, cfg.id};
    g_object_set_data_full(G_OBJECT(sink), "srt-access-context", ctx, freeSrtAccessContext);

    if (configManager.subscribers.filteringEnabled) {
        g_signal_connect_data(
            sink, "caller-connecting", G_CALLBACK(StreamManager::onSrtCallerConnecting),
            ctx, nullptr, static_cast<GConnectFlags>(0));
        g_signal_connect_data(
            sink, "caller-rejected", G_CALLBACK(StreamManager::onSrtCallerRejected),
            ctx, nullptr, static_cast<GConnectFlags>(0));
    }

    g_signal_connect_data(
        sink, "caller-added", G_CALLBACK(StreamManager::onSrtCallerAdded),
        ctx, nullptr, static_cast<GConnectFlags>(0));
    g_signal_connect_data(
        sink, "caller-removed", G_CALLBACK(StreamManager::onSrtCallerRemoved),
        ctx, nullptr, static_cast<GConnectFlags>(0));

    std::cerr << "SRT connection monitoring attached for stream " << cfg.id
              << " port=" << ((cfg.outputPort > 0 && cfg.outputPort <= 65535) ? cfg.outputPort : 7001)
              << " filtering=" << (configManager.subscribers.filteringEnabled ? "on" : "off")
              << std::endl;
}

GstElement* StreamManager::createExternalSrtOutputPipeline(const StreamConfig& cfg, std::string& error) {
    error.clear();
    const uint16_t relayPort = tvs::protocols::transcodedSrtInternalPort(cfg);

    for (const char* factory : {"udpsrc", "queue", "tsparse"}) {
        if (!hasElementFactory(factory)) {
            error = missingElementStatus(factory);
            return nullptr;
        }
    }

    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* src = gst_element_factory_make("udpsrc", nullptr);
    GstElement* inputQueue = gst_element_factory_make("queue", nullptr);
    GstElement* tsparse = gst_element_factory_make("tsparse", nullptr);
    GstElement* outputQueue = gst_element_factory_make("queue", nullptr);

    if (!pipeline || !src || !inputQueue || !tsparse || !outputQueue) {
        error = "failed to create transcoded SRT relay elements";
        if (pipeline) gst_object_unref(pipeline);
        if (src) gst_object_unref(src);
        if (inputQueue) gst_object_unref(inputQueue);
        if (tsparse) gst_object_unref(tsparse);
        if (outputQueue) gst_object_unref(outputQueue);
        return nullptr;
    }

    gst_bin_add_many(GST_BIN(pipeline), src, inputQueue, tsparse, outputQueue, nullptr);

    // Use the exact same SRT sink constructor as a non-transcoded stream.
    // This is important for subscriber monitoring: caller-connecting,
    // caller-added, caller-removed and caller-rejected are connected in one
    // common place and feed the same active-session table.
    GstElement* sink = createOutputSink(nullptr, cfg, pipeline, "transcoded_srt_sink");
    if (!sink) {
        error = "failed to create monitored SRT output sink";
        gst_object_unref(pipeline);
        return nullptr;
    }

    g_object_set(src,
        "address", "127.0.0.1",
        "port", static_cast<gint>(relayPort),
        "reuse", TRUE,
        "auto-multicast", FALSE,
        // The external CBR pacer controls packet timing. Timestamp buffers at
        // their loopback arrival time instead of rebuilding PCR timestamps a
        // second time in the in-process SRT relay.
        "do-timestamp", TRUE,
        "buffer-size", 16 * 1024 * 1024,
        nullptr);
    GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true");
    if (caps) {
        g_object_set(src, "caps", caps, nullptr);
        gst_caps_unref(caps);
    }

    g_object_set(inputQueue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", static_cast<guint64>(5000000000ULL),
        nullptr);
    // The external path already performs PCR smoothing and CBR pacing. A
    // second set-timestamps/smoothing stage here periodically corrects the
    // clock against loopback arrival time and can produce regular SRT pauses.
    setBooleanPropertyIfPresent(tsparse, "set-timestamps", FALSE);
    setIntPropertyIfPresent(tsparse, "alignment", 7);
    g_object_set(outputQueue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", static_cast<guint64>(5000000000ULL),
        nullptr);

    if (!gst_element_link_many(src, inputQueue, tsparse, outputQueue, sink, nullptr)) {
        error = "failed to link transcoded SRT relay pipeline";
        gst_object_unref(pipeline);
        return nullptr;
    }

    std::cerr << "Transcoded SRT output relay: stream=" << cfg.id
              << " udp=127.0.0.1:" << relayPort
              << " -> srt=" << (cfg.outputHost.empty() ? "auto" : cfg.outputHost)
              << ":" << ((cfg.outputPort > 0 && cfg.outputPort <= 65535) ? cfg.outputPort : 7001)
              << " mode=" << tvs::protocols::srtOutputMode(cfg)
              << " monitoring=direct-callbacks"
              << std::endl;
    return pipeline;
}

bool StreamManager::startExternalSrtOutputs(StreamState* state, std::string& error) {
    if (!state) {
        return true;
    }
    error.clear();

    for (const auto& outputConfig : tvs::protocols::outputConfigs(state->config)) {
        if (!isExternalSrtListenerOutput(outputConfig)) {
            continue;
        }

        auto output = std::make_unique<ExternalSrtOutputState>();
        output->config = outputConfig;
        output->pipeline = createExternalSrtOutputPipeline(outputConfig, error);
        if (!output->pipeline) {
            stopExternalSrtOutputs(state);
            if (error.empty()) error = "failed to create transcoded SRT output relay";
            return false;
        }

        output->bus = gst_element_get_bus(output->pipeline);
        const GstStateChangeReturn stateChange = gst_element_set_state(output->pipeline, GST_STATE_PLAYING);
        if (stateChange == GST_STATE_CHANGE_FAILURE) {
            if (output->bus) {
                GstMessage* msg = gst_bus_timed_pop_filtered(
                    output->bus,
                    2 * GST_SECOND,
                    static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
                if (msg) {
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                        GError* err = nullptr;
                        gchar* dbg = nullptr;
                        gst_message_parse_error(msg, &err, &dbg);
                        if (err && err->message) error = err->message;
                        if (dbg && *dbg) error += std::string(" | ") + dbg;
                        if (err) g_error_free(err);
                        g_free(dbg);
                    }
                    gst_message_unref(msg);
                }
            }
            if (error.empty()) error = "failed to start transcoded SRT output relay";
            if (output->pipeline) {
                gst_element_set_state(output->pipeline, GST_STATE_NULL);
                gst_object_unref(output->pipeline);
                output->pipeline = nullptr;
            }
            if (output->bus) {
                gst_object_unref(output->bus);
                output->bus = nullptr;
            }
            stopExternalSrtOutputs(state);
            return false;
        }

        state->externalSrtOutputs.push_back(std::move(output));
    }
    return true;
}

void StreamManager::stopExternalSrtOutputs(StreamState* state) {
    if (!state) {
        return;
    }
    for (auto& output : state->externalSrtOutputs) {
        if (!output) continue;
        if (output->pipeline) {
            gst_element_set_state(output->pipeline, GST_STATE_NULL);
        }
        if (output->busThread.joinable()) {
            output->busThread.join();
        }
        if (output->bus) {
            gst_object_unref(output->bus);
            output->bus = nullptr;
        }
        if (output->pipeline) {
            gst_object_unref(output->pipeline);
            output->pipeline = nullptr;
        }
    }
    state->externalSrtOutputs.clear();
}

void StreamManager::monitorExternalSrtBus(const std::string& id, size_t outputIndex) {
    (void)id;
    (void)outputIndex;
}

GstElement* StreamManager::createTranscodedUdpRelayPipeline(StreamState* state, std::string& error) {
    if (!state) {
        error = "transcoded UDP relay state is null";
        return nullptr;
    }

    const std::string fifoPath = tvs::protocols::transcodedFifoRelayPath(state->config);
    GstElement* pipeline = gst_pipeline_new((state->config.id + "_transcoded_udp_relay").c_str());
    GstElement* src = gst_element_factory_make("filesrc", "transcoded_udp_fifo_src");
    GstElement* queue = gst_element_factory_make("queue", "transcoded_udp_fifo_queue");
    if (!pipeline || !src || !queue ||
        !addElementOrFail(pipeline, src) || !addElementOrFail(pipeline, queue)) {
        error = "failed to create transcoded UDP FIFO relay elements";
        if (pipeline) gst_object_unref(pipeline);
        return nullptr;
    }

    g_object_set(src, "location", fifoPath.c_str(), nullptr);
    configureQueue(queue, 10000000000ULL);
    if (!gst_element_link(src, queue)) {
        error = "failed to link transcoded UDP FIFO relay source";
        gst_object_unref(pipeline);
        return nullptr;
    }

    // state->config intentionally keeps transcodeEnabled=true.  The output
    // branch therefore treats the FIFO content as an already finished SPTS and
    // sends it directly to StableUdpOutput without another demux/remux pass.
    if (!buildOutputBranches(state, pipeline, queue)) {
        error = "failed to build normal UDP output after transcoding";
        gst_object_unref(pipeline);
        return nullptr;
    }

    std::cerr << "Transcoded UDP relay: external GStreamer encoder -> fifo://"
              << fifoPath
              << " -> StableUdpOutput (default UDP path)" << std::endl;
    return pipeline;
}

bool StreamManager::startStream(const StreamConfig& streamConfig, std::string* error) {
    if (error) error->clear();

    // A stream can remain in the runtime table after its bus thread reports a
    // terminal ERROR/EOS: the UI correctly shows it as OFFLINE, but a later
    // Start used to fail with "stream is already active" simply because the
    // stale StreamState object still existed.  Clean an inactive state before
    // building the replacement pipeline. cleanupStreamState() performs the complete
    // teardown (bus-thread join, pipeline NULL/unref, shared-DVB consumer
    // release, service-relay port release and CA slot release), which is
    // especially important for shared DVB frontends.
    bool staleInactiveStream = false;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto existing = streams.find(streamConfig.id);
        if (existing != streams.end()) {
            if (existing->second && existing->second->active.load()) {
                if (error) *error = "stream is already active: " + streamConfig.id;
                return false;
            }
            staleInactiveStream = true;
        }
    }
    if (staleInactiveStream) {
        std::cerr << "Cleaning inactive stream state before restart: " << streamConfig.id << std::endl;
        if (!cleanupStreamState(streamConfig.id, false)) {
            if (error) *error = "failed to clean inactive stream state: " + streamConfig.id;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> lock(managerMutex);
        if (!gstreamerInitialized) {
            gst_init(nullptr, nullptr);
            gstreamerInitialized = true;
        }
    }

    CardReservationGuard cardReservation(streamConfig, error);
    if (!cardReservation.ok()) {
        if (error && error->empty()) *error = "failed to reserve conditional-access service slot";
        std::cerr << "Stream start failed before pipeline: id=" << streamConfig.id
                  << " stage=cam-reserve error=" << (error ? *error : std::string()) << std::endl;
        return false;
    }
    std::cerr << "Stream start requested: id=" << streamConfig.id
              << " name=" << (streamConfig.name.empty() ? streamConfig.id : streamConfig.name)
              << " input=" << streamConfig.inputUri
              << " cam=" << (streamConfig.conditionalAccessClient.empty() ? "none" : streamConfig.conditionalAccessClient)
              << std::endl;

    auto state = std::make_unique<StreamState>();

    StreamConfig effectiveConfig = streamConfig;
    // input_service_id == 0 is true AUTO mode. Do not open/probe the live
    // input in a second temporary pipeline before the real stream starts.
    // A preflight PAT probe can consume/exclusively occupy SRT sources and can
    // disturb UDP startup. The real demux/decode pipeline performs automatic
    // program selection when SID is zero. A non-zero SID is still selected
    // explicitly with tsdemux program-number.
    if (effectiveConfig.inputServiceId == 0) {
        std::cerr << "Input SID mode: AUTO (single live input, no preflight probe)"
                  << " uri=" << effectiveConfig.inputUri << std::endl;
    }

    state->config = effectiveConfig;
    state->runtimeConfig = effectiveConfig;
    state->primaryInputUri = effectiveConfig.inputUri;
    state->activeInputUri = effectiveConfig.testPattern ? kTestPatternUri : effectiveConfig.inputUri;

    std::string sharedDvbError;
    if (!prepareSharedDvbInput(state.get(), sharedDvbError)) {
        state->statusMessage = "shared DVB input failed: " + sharedDvbError;
        if (error) *error = sharedDvbError;
        std::cerr << "Stream start failed: id=" << streamConfig.id
                  << " stage=shared-dvb-input error=" << sharedDvbError << std::endl;
        return false;
    }

    state->sourceContext = std::make_unique<RemapContext>();
    state->sourceContext->config = state->runtimeConfig;

    if (effectiveConfig.transcodeEnabled && allOutputsUseStableUdp(effectiveConfig) &&
        GstTranscoderProcess::isAvailable()) {
        std::string relayError;
        if (!tvs::protocols::prepareFifoRelay(effectiveConfig, relayError)) {
            state->statusMessage = "transcoded UDP relay setup failed: " + relayError;
            releaseSharedDvbInput(state.get());
            if (error) *error = relayError;
            return false;
        }

        auto gstTranscoder = std::make_unique<GstTranscoderProcess>();
        const StreamConfig relayConfig = transcodeRelayOutputConfig(state->runtimeConfig);
        std::string gstError;
        if (!gstTranscoder->start(relayConfig, gstError)) {
            tvs::protocols::removeFifoRelay(effectiveConfig);
            state->statusMessage = "gstreamer transcoder relay failed: " + gstError;
            releaseSharedDvbInput(state.get());
            if (error) *error = gstError.empty() ? "GStreamer transcoder relay failed to start" : gstError;
            return false;
        }
        state->gstTranscoder = std::move(gstTranscoder);

        GstElement* relayPipeline = createTranscodedUdpRelayPipeline(state.get(), relayError);
        if (!relayPipeline) {
            state->gstTranscoder->stop();
            state->gstTranscoder.reset();
            tvs::protocols::removeFifoRelay(effectiveConfig);
            state->statusMessage = "transcoded UDP output failed: " + relayError;
            releaseSharedDvbInput(state.get());
            if (error) *error = relayError.empty() ? "failed to create transcoded UDP output" : relayError;
            return false;
        }

        state->pipeline = relayPipeline;
        state->bus = gst_element_get_bus(relayPipeline);
        state->running = true;
        state->active = true;
        state->statusMessage = "starting transcoded UDP";
        state->outputBitrate = initialConfiguredOutputBitrate(effectiveConfig);
        state->inputBitrate = transcodeInputBitrateForStats(effectiveConfig);
        state->lastInputActivity = std::chrono::steady_clock::now();
        state->lastPrimaryRetry = state->lastInputActivity;
        state->lastBitrateSample = state->lastInputActivity;
        attachBitrateProbes(state.get());

        const GstStateChangeReturn stateChange = gst_element_set_state(relayPipeline, GST_STATE_PLAYING);
        if (stateChange == GST_STATE_CHANGE_FAILURE) {
            state->running = false;
            state->active = false;
            if (state->bus) {
                gst_object_unref(state->bus);
                state->bus = nullptr;
            }
            gst_element_set_state(relayPipeline, GST_STATE_NULL);
            gst_element_get_state(relayPipeline, nullptr, nullptr, GST_SECOND);
            gst_object_unref(relayPipeline);
            state->pipeline = nullptr;
            state->gstTranscoder->stop();
            state->gstTranscoder.reset();
            tvs::protocols::removeFifoRelay(effectiveConfig);
            state->statusMessage = "transcoded UDP relay playback failed";
            releaseSharedDvbInput(state.get());
            if (error) *error = "failed to start post-transcode StableUdpOutput pipeline";
            return false;
        }

        std::cerr << "Pipeline for stream '" << streamConfig.name
                  << "': external-transcoder -> fifo -> default-udp"
                  << " transcode=" << streamConfig.transcodeResolution
                  << "@" << streamConfig.transcodeVideoBitrate
                  << " outputs=" << buildPipelineDescription(streamConfig) << std::endl;

        bool duplicateStart = false;
        {
            std::lock_guard<std::mutex> lock(managerMutex);
            if (streams.count(streamConfig.id)) {
                duplicateStart = true;
            } else {
                streams[streamConfig.id] = std::move(state);
                try {
                    streams[streamConfig.id]->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);
                } catch (const std::exception& ex) {
                    std::cerr << "Resource guard: bus monitor thread unavailable for stream="
                              << streamConfig.id << " error=" << ex.what()
                              << " action=keep-stream-running-without-bus-monitor" << std::endl;
                }
            }
        }
        if (duplicateStart) {
            if (state) {
                state->running = false;
                if (state->pipeline) gst_element_set_state(state->pipeline, GST_STATE_NULL);
                if (state->bus) gst_object_unref(state->bus);
                if (state->pipeline) gst_object_unref(state->pipeline);
                if (state->gstTranscoder) state->gstTranscoder->stop();
            }
            tvs::protocols::removeFifoRelay(effectiveConfig);
            if (state) releaseSharedDvbInput(state.get());
            if (error) *error = "duplicate stream start detected: " + streamConfig.id;
            return false;
        }

        notifyStreamState(
            streamConfig,
            "🟢",
            telegramText(configManager, "Поток запущен", "Stream started"),
            telegramText(configManager, "Транскодинг -> стандартный UDP", "Transcode -> default UDP") +
                "\nURL: " + streamConfig.inputUri);
        cardReservation.commit();
        return true;
    }

    if (effectiveConfig.transcodeEnabled && GstTranscoderProcess::isAvailable()) {
        std::string srtRelayError;
        if (!startExternalSrtOutputs(state.get(), srtRelayError)) {
            std::cerr << "Transcoded SRT output setup failed for " << streamConfig.id
                      << ": " << srtRelayError << std::endl;
            state->statusMessage = "transcoded srt output failed: " + srtRelayError;
            releaseSharedDvbInput(state.get());
            if (error) *error = srtRelayError.empty() ? "failed to start transcoded SRT output" : srtRelayError;
            return false;
        }

        auto gstTranscoder = std::make_unique<GstTranscoderProcess>();
        std::string gstError;
        if (!gstTranscoder->start(state->runtimeConfig, gstError)) {
            std::cerr << "GStreamer transcoder setup failed for " << streamConfig.id
                      << ": " << gstError << std::endl;
            state->statusMessage = "gstreamer transcoder failed: " + gstError;
            stopExternalSrtOutputs(state.get());
            releaseSharedDvbInput(state.get());
            if (error) *error = gstError.empty() ? "GStreamer transcoder failed to start" : gstError;
            return false;
        }

        std::cerr << "Pipeline for stream '" << streamConfig.name
                  << "': gstreamer-transcoder input=" << streamConfig.inputUri
                  << " transcode=" << streamConfig.transcodeResolution
                  << "@" << streamConfig.transcodeVideoBitrate
                  << " outputs=" << gstTranscoder->description() << std::endl;

        state->gstTranscoder = std::move(gstTranscoder);
        state->running = true;
        state->active = true;
        state->statusMessage = "running via gstreamer";
        state->outputBitrate = initialConfiguredOutputBitrate(effectiveConfig);
        state->inputBitrate = transcodeInputBitrateForStats(effectiveConfig);
        state->lastInputActivity = std::chrono::steady_clock::now();
        state->lastPrimaryRetry = state->lastInputActivity;
        state->lastBitrateSample = state->lastInputActivity;

        bool duplicateStart = false;
        {
            std::lock_guard<std::mutex> lock(managerMutex);
            if (streams.count(streamConfig.id)) {
                duplicateStart = true;
            } else {
                streams[streamConfig.id] = std::move(state);
                try {
                    streams[streamConfig.id]->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);
                } catch (const std::exception& ex) {
                    std::cerr << "Resource guard: bus monitor thread unavailable for stream="
                              << streamConfig.id << " error=" << ex.what()
                              << " action=keep-stream-running-without-bus-monitor" << std::endl;
                }
            }
        }
        if (duplicateStart) {
            if (state) {
                stopExternalSrtOutputs(state.get());
                if (state->gstTranscoder) {
                    state->gstTranscoder->stop();
                }
            }
            if (state) releaseSharedDvbInput(state.get());
            if (error) *error = "duplicate stream start detected: " + streamConfig.id;
            return false;
        }
        notifyStreamState(
            streamConfig,
            "🟢",
            telegramText(configManager, "Поток запущен", "Stream started"),
            telegramText(configManager, "GStreamer-транскодер", "GStreamer transcoder") + "\nURL: " + streamConfig.inputUri);
        cardReservation.commit();
        return true;
    }

    GstElement* pipeline = createPipeline(state.get());
    if (!pipeline) {
        if (state->statusMessage.empty()) state->statusMessage = "pipeline build failed";
        const std::string buildError = state->statusMessage + " for stream: " + streamConfig.name;
        stopHttpMpegTsInput(state.get());
        releaseSharedDvbInput(state.get());
        if (error) *error = buildError;
        return false;
    }

    std::cerr << "Pipeline for stream '" << streamConfig.name
              << "': " << buildPipelineDescription(streamConfig) << std::endl;

    state->pipeline = pipeline;
    state->bus = gst_element_get_bus(pipeline);
    state->running = true;
    state->active = true;
    state->statusMessage = "starting";
    state->outputBitrate = initialConfiguredOutputBitrate(effectiveConfig);
    state->lastInputActivity = std::chrono::steady_clock::now();
    state->lastPrimaryRetry = state->lastInputActivity;
    state->lastBitrateSample = state->lastInputActivity;
    attachBitrateProbes(state.get());

    GstStateChangeReturn stateChange = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateChange == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to set pipeline to PLAYING for stream: " << streamConfig.name << std::endl;
        std::string playingError = "failed to set pipeline to PLAYING for stream: " + streamConfig.name;
        if (state->bus) {
            GstMessage* msg = gst_bus_timed_pop_filtered(
                state->bus,
                2 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
            if (msg) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    GError* err = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_error(msg, &err, &dbg);
                    std::cerr << "PLAYING error: " << (err ? err->message : "unknown")
                              << " debug=" << (dbg ? dbg : "") << std::endl;
                    if (err && err->message) playingError = err->message;
                    if (dbg && *dbg) playingError += std::string(" | ") + dbg;
                    if (err) g_error_free(err);
                    g_free(dbg);
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
                    GError* err = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_warning(msg, &err, &dbg);
                    std::cerr << "PLAYING warning: " << (err ? err->message : "unknown")
                              << " debug=" << (dbg ? dbg : "") << std::endl;
                    if (err) g_error_free(err);
                    g_free(dbg);
                }
                gst_message_unref(msg);
            }
            gst_object_unref(state->bus);
            state->bus = nullptr;
        }
        if (state->gstTranscoder) {
            state->gstTranscoder->stop();
            state->gstTranscoder.reset();
        }
        // A failed DVB transition can leave dvbsrc in READY. Always drive the
        // complete pipeline to NULL before the last reference is released so
        // the frontend fd is closed deterministically.
        gst_element_set_state(pipeline, GST_STATE_NULL);
        stopHttpMpegTsInput(state.get());
        gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND);
        state->pipeline = nullptr;
        gst_object_unref(pipeline);
        releaseSharedDvbInput(state.get());
        if (error) *error = playingError;
        return false;
    }

    startHttpMpegTsInput(state.get());
    state->statusMessage = (stateChange == GST_STATE_CHANGE_ASYNC) ? "starting" : "running";
    bool duplicateStart = false;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        if (streams.count(streamConfig.id)) {
            duplicateStart = true;
        } else {
            streams[streamConfig.id] = std::move(state);
            try {
                    streams[streamConfig.id]->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);
                } catch (const std::exception& ex) {
                    std::cerr << "Resource guard: bus monitor thread unavailable for stream="
                              << streamConfig.id << " error=" << ex.what()
                              << " action=keep-stream-running-without-bus-monitor" << std::endl;
                }
        }
    }
    if (duplicateStart) {
        state->running = false;
        if (state->pipeline) {
            gst_element_set_state(state->pipeline, GST_STATE_NULL);
        }
        stopHttpMpegTsInput(state.get());
        if (state->bus) {
            gst_object_unref(state->bus);
            state->bus = nullptr;
        }
        if (state->pipeline) {
            gst_object_unref(state->pipeline);
            state->pipeline = nullptr;
        }
        releaseSharedDvbInput(state.get());
        if (error) *error = "duplicate stream start detected: " + streamConfig.id;
        return false;
    }
    notifyStreamState(
        streamConfig,
        "🟢",
        telegramText(configManager, "Поток запущен", "Stream started"),
        telegramText(configManager, "Источник: основной", "Source: primary") + "\nURL: " + streamConfig.inputUri);
    cardReservation.commit();
    return true;
}

bool StreamManager::cleanupStreamState(const std::string& id, bool notifyManualStop) {
    std::unique_ptr<StreamState> statePtr;
    StreamConfig stoppedConfig;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto found = streams.find(id);
        if (found == streams.end()) {
            return false;
        }

        statePtr = std::move(found->second);
        streams.erase(found);
        stoppedConfig = statePtr->config;
        statePtr->running = false;
        statePtr->active = false;
        statePtr->statusMessage = notifyManualStop ? "stopped" : "cleaning inactive state";

        for (auto it = httpClients.begin(); it != httpClients.end();) {
            if (it->second.streamId == id) {
                it = httpClients.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
            if (it->second.streamId == id) {
                it = adHocSessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto& state = *statePtr;
    if (state.pipeline) {
        gst_element_set_state(state.pipeline, GST_STATE_NULL);
    }
    stopHttpMpegTsInput(&state);
    // Release DVB immediately after shutdown starts; do not wait for output
    // threads/transcoders before returning the adapter to the kernel.
    releaseSharedDvbInput(&state);
    stopExternalSrtOutputs(&state);
    if (state.busThread.joinable()) {
        state.busThread.join();
    }
    if (state.gstTranscoder) {
        state.gstTranscoder->stop();
        state.gstTranscoder.reset();
    }
    if (state.bus) {
        gst_object_unref(state.bus);
        state.bus = nullptr;
    }
    if (state.pipeline) {
        gst_element_get_state(state.pipeline, nullptr, nullptr, GST_SECOND);
        gst_object_unref(state.pipeline);
        state.pipeline = nullptr;
    }
    state.outputContexts.clear();
    state.sourceContext.reset();
    tvs::protocols::removeFifoRelay(stoppedConfig);
    CardManager::instance().releaseService(id);

    if (notifyManualStop) {
        notifyStreamState(
            stoppedConfig,
            "stop",
            "Stream stopped",
            "Stopped manually");
    } else {
        std::cerr << "Inactive stream state cleaned: " << id << std::endl;
    }
    return true;
}

bool StreamManager::stopStream(const std::string& id) {
    return cleanupStreamState(id, true);
}

bool StreamManager::stopStreamAsync(const std::string& id) {
    std::unique_ptr<StreamState> statePtr;
    StreamConfig stoppedConfig;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto found = streams.find(id);
        if (found == streams.end()) {
            return false;
        }

        statePtr = std::move(found->second);
        streams.erase(found);
        stoppedConfig = statePtr->config;
        statePtr->running = false;
        statePtr->active = false;
        statePtr->statusMessage = "stopping";

        for (auto it = httpClients.begin(); it != httpClients.end();) {
            if (it->second.streamId == id) {
                it = httpClients.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
            if (it->second.streamId == id) {
                it = adHocSessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::thread([this, id, statePtr = std::move(statePtr), stoppedConfig]() mutable {
        auto& state = *statePtr;
        if (state.pipeline) {
            gst_element_set_state(state.pipeline, GST_STATE_NULL);
        }
        // Fast stop path: the adapter is released before the detached cleanup
        // waits for the main bus thread, external outputs or transcoder.
        releaseSharedDvbInput(&state);
        stopExternalSrtOutputs(&state);
        if (state.busThread.joinable()) {
            state.busThread.join();
        }
        if (state.gstTranscoder) {
            state.gstTranscoder->stop();
            state.gstTranscoder.reset();
        }
        if (state.bus) {
            gst_object_unref(state.bus);
            state.bus = nullptr;
        }
        if (state.pipeline) {
            gst_element_get_state(state.pipeline, nullptr, nullptr, GST_SECOND);
            gst_object_unref(state.pipeline);
            state.pipeline = nullptr;
        }
        state.outputContexts.clear();
        state.sourceContext.reset();
        tvs::protocols::removeFifoRelay(stoppedConfig);
        CardManager::instance().releaseService(id);
        notifyStreamState(
            stoppedConfig,
            "stop",
            "Stream stopped",
            "Stopped manually");
    }).detach();
    return true;
}

bool StreamManager::restartStream(const StreamConfig& streamConfig, std::string* error) {
    if (error) error->clear();
    std::cerr << "Hard restarting stream: " << streamConfig.id << std::endl;
    const bool stopped = stopStream(streamConfig.id);
    if (stopped) {
        std::this_thread::sleep_for(kSrtRestartRetryDelay);
    }
    return startStream(streamConfig, error);
}

void StreamManager::stopAll() {
    std::vector<std::unique_ptr<StreamState>> stoppedStreams;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients.clear();
        adHocSessions.clear();
        for (auto& [id, statePtr] : streams) {
            (void)id;
            statePtr->running = false;
            statePtr->active = false;
            stoppedStreams.push_back(std::move(statePtr));
        }
        streams.clear();
    }
    CardManager::instance().releaseAll();

    for (auto& statePtr : stoppedStreams) {
        auto& state = *statePtr;
        if (state.pipeline) {
            gst_element_set_state(state.pipeline, GST_STATE_NULL);
        }
        releaseSharedDvbInput(&state);
        stopExternalSrtOutputs(&state);
        if (state.busThread.joinable()) {
            state.busThread.join();
        }
        if (state.gstTranscoder) {
            state.gstTranscoder->stop();
            state.gstTranscoder.reset();
        }
        if (state.bus) {
            gst_object_unref(state.bus);
        }
        if (state.pipeline) {
            gst_object_unref(state.pipeline);
        }
        state.outputContexts.clear();
        state.sourceContext.reset();
        tvs::protocols::removeFifoRelay(state.config);
    }
}

bool StreamManager::isStreamActive(const std::string& id) {
    std::lock_guard<std::mutex> lock(managerMutex);
    auto found = streams.find(id);
    return found != streams.end() && found->second->active.load();
}

std::vector<std::string> StreamManager::activeStreams() {
    std::lock_guard<std::mutex> lock(managerMutex);
    std::vector<std::string> result;
    for (auto& [id, statePtr] : streams) {
        if (statePtr->active.load()) {
            result.push_back(id);
        }
    }
    return result;
}

std::map<std::string, StreamState*> StreamManager::snapshot() {
    std::lock_guard<std::mutex> lock(managerMutex);
    std::map<std::string, StreamState*> result;
    for (auto& [id, statePtr] : streams) {
        if (statePtr->gstTranscoder && !statePtr->gstTranscoder->isRunning()) {
            statePtr->running = false;
            statePtr->active = false;
            statePtr->statusMessage = "gstreamer transcoder exited";
        }
        if (statePtr->pipeline) {
            updateBitrateEstimates(statePtr.get());
        }
        result[id] = statePtr.get();
    }
    return result;
}

uint64_t StreamManager::queryPipelineBitrate(GstElement* pipeline) {
    (void)pipeline;
    return 0;
}
std::string StreamManager::buildPipelineDescription(const StreamConfig& cfg) {
    const std::string inputInterface = configuredInputInterfaceAddress(cfg);
    std::ostringstream desc;
    desc << "manual-pipeline"
         << " input=" << cfg.inputUri
         << " input_proto=" << tvs::stream_protocols::inputKindName(tvs::stream_protocols::inputKind(cfg))
         << " output_proto=" << tvs::stream_protocols::outputKindName(tvs::stream_protocols::outputKind(cfg))
         << " input_mode=" << cfg.inputMode
         << " input_iface=" << (inputInterface.empty() ? "auto" : inputInterface)
         << " test_pattern=" << (cfg.testPattern ? "on" : "off")
         << " remap=" << (cfg.remapEnabled ? "on" : "off")
         << " udp_shaper=" << (usesStableUdpShaper(cfg) ? "stable" : "n/a")
         << " transcode=" << (cfg.transcodeEnabled ? cfg.transcodeResolution + "@" + std::to_string(cfg.transcodeVideoBitrate) : "off")
         << " outputs=";
    const auto outputs = outputConfigs(cfg);
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (i > 0) {
            desc << ",";
        }
        desc << outputType(outputs[i])
             << "/" << srtOutputMode(outputs[i])
             << "@" << outputs[i].outputHost << ":" << outputs[i].outputPort;
    }
    desc
         << " iface=" << cfg.interfaceAddress
         << " input_service_id=" << cfg.inputServiceId
         << " service_id=" << cfg.serviceId
         << " vpid=" << cfg.videoPid
         << " apid=" << cfg.audioPid;
    return desc.str();
}

bool StreamManager::addHttpClient(const std::string& id, int fd, const std::string& clientIp) {
    std::unique_lock<std::mutex> lock(managerMutex);
    auto found = streams.find(id);
    if (found == streams.end()) {
        close(fd);
        return false;
    }

    if (!found->second->pipeline && found->second->gstTranscoder && hasTranscodedHttpOutput(found->second->config)) {
        const uint16_t relayPort = tvs::protocols::transcodedHttpInternalPort(found->second->config);
        std::string relayError;
        int upstreamFd = connectLocalTcpWithRetry(relayPort, relayError);
        if (upstreamFd < 0) {
            std::cerr << "Transcoded HTTP relay failed for stream " << id
                      << ": " << relayError << std::endl;
            close(fd);
            return false;
        }

        httpClients[fd] = {id, normalizeIpAddress(clientIp), "mpegts"};
        std::thread([this, id, fd, upstreamFd]() {
            std::array<char, 65536> buffer {};
            while (true) {
                ssize_t readBytes = ::read(upstreamFd, buffer.data(), buffer.size());
                if (readBytes < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (readBytes == 0) break;
                if (!writeAllToFd(fd, buffer.data(), static_cast<size_t>(readBytes))) break;
            }
            ::close(upstreamFd);
            ::close(fd);
            std::lock_guard<std::mutex> relayLock(managerMutex);
            httpClients.erase(fd);
        }).detach();
        return true;
    }

    if (!found->second->pipeline) {
        close(fd);
        return false;
    }

    auto sinks = findSinksByFactory(found->second->pipeline, "multifdsink");
    if (sinks.empty()) {
        close(fd);
        return false;
    }

    GstElement* sink = sinks.front();
    for (size_t i = 1; i < sinks.size(); ++i) {
        gst_object_unref(sinks[i]);
    }

    // Register ownership before handing the descriptor to multifdsink so an
    // immediate removal callback cannot leave a stale closed fd in the map.
    httpClients[fd] = {id, normalizeIpAddress(clientIp), "mpegts"};
    lock.unlock();
    g_signal_emit_by_name(sink, "add", fd);
    gst_object_unref(sink);
    return true;
}

bool StreamManager::addStreamSession(const std::string& streamId, const std::string& clientIp, const std::string& protocol) {
    if (streamId.empty() || clientIp.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    const auto now = std::chrono::steady_clock::now();
    const std::string key = protocol == "hls"
        ? protocol + ":" + streamId + ":" + normalizedClientIp
        : protocol + ":" + streamId + ":" + normalizedClientIp + ":" + std::to_string(nextSessionId.fetch_add(1, std::memory_order_relaxed));
    adHocSessions[key] = {streamId, normalizedClientIp, protocol, now};
    return true;
}

bool StreamManager::removeStreamSession(const std::string& streamId, const std::string& clientIp, const std::string& protocol) {
    if (streamId.empty() || clientIp.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        const auto& session = it->second;
        if (session.streamId == streamId && session.clientIp == normalizedClientIp && session.protocol == protocol) {
            it = adHocSessions.erase(it);
            return true;
        }
        ++it;
    }
    return false;
}

void StreamManager::onHttpClientFdRemoved(GstElement* sink, gint fd, gpointer userData) {
    (void)sink;
    auto* manager = static_cast<StreamManager*>(userData);
    if (!manager) {
        if (fd >= 0) ::close(fd);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(manager->managerMutex);
        manager->httpClients.erase(fd);
    }
    // multifdsink explicitly leaves ownership of the descriptor to the
    // application. client-fd-removed is the safe point at which it can close.
    if (fd >= 0) ::close(fd);
}

void StreamManager::pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        const auto& session = it->second;
        if (session.protocol == "hls" && now - session.lastActivity > kHlsSessionTtl) {
            it = adHocSessions.erase(it);
        } else {
            ++it;
        }
    }
}

size_t StreamManager::activeHttpSessions(const std::string& clientIp) const {
    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    size_t count = 0;
    for (const auto& [fd, session] : httpClients) {
        (void)fd;
        if (normalizeIpAddress(session.clientIp) == normalizedClientIp) ++count;
    }
    for (const auto& [key, session] : adHocSessions) {
        (void)key;
        if (normalizeIpAddress(session.clientIp) == normalizedClientIp) ++count;
    }
    return count;
}

size_t StreamManager::activeSubscriberSessions(const SubscriberConfig& subscriber) {
    if (!subscriber.enabled || subscriber.streamIds.empty()) {
        return 0;
    }

    const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
    const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
    auto ipMatches = [&](const std::string& clientIp) {
        const std::string normalizedClientIp = normalizeIpAddress(clientIp);
        return normalizedClientIp == primaryIp || (!backupIp.empty() && normalizedClientIp == backupIp);
    };
    auto streamMatches = [&](const std::string& streamId) {
        return std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
    };

    std::lock_guard<std::mutex> lock(managerMutex);
    pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
    size_t count = 0;
    for (const auto& [fd, session] : httpClients) {
        (void)fd;
        if (ipMatches(session.clientIp) && streamMatches(session.streamId)) {
            ++count;
        }
    }
    for (const auto& [key, session] : adHocSessions) {
        (void)key;
        if (ipMatches(session.clientIp) && streamMatches(session.streamId)) {
            ++count;
        }
    }
    return count;
}

size_t StreamManager::resetHttpSessions(const std::string& clientIp) {
    std::vector<std::pair<GstElement*, int>> sinks;
    size_t removed = 0;
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
        for (auto it = httpClients.begin(); it != httpClients.end();) {
            if (normalizeIpAddress(it->second.clientIp) != normalizedClientIp) {
                ++it;
                continue;
            }
            auto found = streams.find(it->second.streamId);
            if (found != streams.end() && found->second->pipeline) {
                auto foundSinks = findSinksByFactory(found->second->pipeline, "multifdsink");
                for (auto* sink : foundSinks) {
                    sinks.emplace_back(sink, it->first);
                }
            }
            ++removed;
            it = httpClients.erase(it);
        }
        for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
            if (normalizeIpAddress(it->second.clientIp) == normalizedClientIp) {
                ++removed;
                it = adHocSessions.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& [sink, fd] : sinks) {
        g_signal_emit_by_name(sink, "remove", fd);
        gst_object_unref(sink);
    }
    return removed;
}

size_t StreamManager::enforceSubscriberAccess() {
    std::vector<std::pair<GstElement*, int>> sinks;
    std::vector<std::string> srtStreams;
    size_t removed = 0;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
        for (auto it = httpClients.begin(); it != httpClients.end();) {
            if (isClientAllowedForStream(it->second.streamId, it->second.clientIp)) {
                ++it;
                continue;
            }
            std::cerr << "Disconnecting unauthorized " << it->second.protocol
                      << " session stream=" << it->second.streamId
                      << " ip=" << it->second.clientIp << std::endl;
            auto found = streams.find(it->second.streamId);
            if (found != streams.end() && found->second->pipeline) {
                auto foundSinks = findSinksByFactory(found->second->pipeline, "multifdsink");
                for (auto* sink : foundSinks) {
                    sinks.emplace_back(sink, it->first);
                }
            }
            ++removed;
            it = httpClients.erase(it);
        }

        for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
            if (isClientAllowedForStream(it->second.streamId, it->second.clientIp)) {
                ++it;
                continue;
            }
            std::cerr << "Disconnecting unauthorized " << it->second.protocol
                      << " session stream=" << it->second.streamId
                      << " ip=" << it->second.clientIp << std::endl;
            if (it->second.protocol == "srt") {
                srtStreams.push_back(it->second.streamId);
            }
            ++removed;
            it = adHocSessions.erase(it);
        }
    }

    for (const auto& [sink, fd] : sinks) {
        g_signal_emit_by_name(sink, "remove", fd);
        gst_object_unref(sink);
    }
    if (!srtStreams.empty()) {
        restartSrtOutputsForStreams(srtStreams);
    }
    return removed;
}

size_t StreamManager::restartSrtOutputsForStreams(const std::vector<std::string>& streamIds) {
    std::vector<StreamConfig> configs;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        for (const auto& streamId : streamIds) {
            if (streamId.empty() || std::find_if(configs.begin(), configs.end(), [&streamId](const StreamConfig& config) {
                    return config.id == streamId;
                }) != configs.end()) {
                continue;
            }
            auto found = streams.find(streamId);
            if (found == streams.end() || !hasSrtListenerOutput(found->second->config)) {
                continue;
            }
            if (!found->second->pipeline && found->second->externalSrtOutputs.empty()) {
                continue;
            }
            configs.push_back(found->second->config);
        }

        for (const auto& config : configs) {
            for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
                if (it->second.protocol == "srt" && it->second.streamId == config.id) {
                    it = adHocSessions.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    size_t restarted = 0;
    for (const auto& config : configs) {
        std::cerr << "Rebuilding SRT listener to drop clients for stream " << config.id << std::endl;
        if (!stopStream(config.id)) {
            std::cerr << "Unable to stop SRT listener for stream " << config.id << std::endl;
            continue;
        }

        bool started = false;
        for (int attempt = 1; attempt <= kSrtRestartAttempts; ++attempt) {
            std::this_thread::sleep_for(kSrtRestartRetryDelay);
            if (startStream(config)) {
                started = true;
                break;
            }
            std::cerr << "SRT listener restart attempt " << attempt
                      << " failed for stream " << config.id << std::endl;
        }

        if (started) {
            ++restarted;
        } else {
            std::cerr << "SRT listener remains stopped after restart attempts for stream "
                      << config.id << std::endl;
        }
    }
    return restarted;
}

size_t StreamManager::restartAllSrtOutputs() {
    std::vector<std::string> streamIds;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        for (const auto& [id, state] : streams) {
            if (state && hasSrtListenerOutput(state->config) &&
                (state->pipeline || !state->externalSrtOutputs.empty())) {
                streamIds.push_back(id);
            }
        }
    }
    return restartSrtOutputsForStreams(streamIds);
}

bool StreamManager::isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const {
    if (!configManager.subscribers.filteringEnabled) {
        return true;
    }
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    if (streamId.empty() || normalizedClientIp.empty()) {
        return false;
    }
    for (const auto& subscriber : configManager.subscribers.subscribers) {
        const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
        const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
        const bool ipMatches = subscriber.enabled &&
            (normalizedClientIp == primaryIp || (!backupIp.empty() && normalizedClientIp == backupIp));
        const bool streamMatches = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
        if (ipMatches && streamMatches) {
            return true;
        }
    }
    return false;
}

gboolean StreamManager::onSrtCallerConnecting(GstElement* sink, GSocketAddress* addr, const gchar* streamId, gpointer userData) {
    auto* ctx = static_cast<SrtAccessContext*>(userData);
    if (!ctx || !ctx->manager) {
        return TRUE;
    }

    const std::string clientIp = socketAddressToString(addr);
    const std::string requestedStreamId = streamId ? streamId : "";
    const bool allowed = ctx->manager->isClientAllowedForStream(ctx->streamId, clientIp);
    if (!allowed) {
        std::cerr << "SRT access denied for stream " << ctx->streamId << " from " << clientIp;
        if (!requestedStreamId.empty()) {
            std::cerr << " requested_streamid=" << requestedStreamId;
        }
        std::cerr << std::endl;
    } else {
        std::cerr << "SRT access allowed for stream " << ctx->streamId << " from " << clientIp;
        if (!requestedStreamId.empty()) {
            std::cerr << " requested_streamid=" << requestedStreamId;
        }
        std::cerr << std::endl;
    }
    return allowed ? TRUE : FALSE;
}

void StreamManager::onSrtCallerAdded(GstElement* sink, gint, GSocketAddress* addr, gpointer userData) {
    auto* ctx = static_cast<SrtAccessContext*>(userData);
    if (!ctx || !ctx->manager) {
        return;
    }
    const std::string clientIp = socketAddressToString(addr);
    if (!clientIp.empty()) {
        if (!ctx->manager->isClientAllowedForStream(ctx->streamId, clientIp)) {
            std::cerr << "Rebuilding SRT listener after unauthorized caller for stream " << ctx->streamId
                      << " from " << clientIp << std::endl;
            ctx->manager->restartSrtOutputsForStreams({ctx->streamId});
            return;
        }
        std::cerr << "SRT caller added for stream " << ctx->streamId
                  << " from " << clientIp << std::endl;
        ctx->manager->addStreamSession(ctx->streamId, clientIp, "srt");
    }
    (void)sink;
}

void StreamManager::onSrtCallerRemoved(GstElement* sink, gint, GSocketAddress* addr, gpointer userData) {
    auto* ctx = static_cast<SrtAccessContext*>(userData);
    if (!ctx || !ctx->manager) {
        return;
    }
    const std::string clientIp = socketAddressToString(addr);
    if (!clientIp.empty()) {
        std::cerr << "SRT caller removed for stream " << ctx->streamId
                  << " from " << clientIp << std::endl;
        ctx->manager->removeStreamSession(ctx->streamId, clientIp, "srt");
    }
    (void)sink;
}

void StreamManager::onSrtCallerRejected(GstElement* sink, GSocketAddress* addr, const gchar* streamId, gpointer userData) {
    auto* ctx = static_cast<SrtAccessContext*>(userData);
    if (!ctx) {
        return;
    }
    const std::string clientIp = socketAddressToString(addr);
    const std::string requestedStreamId = streamId ? streamId : "";
    std::cerr << "SRT caller rejected for stream " << ctx->streamId
              << " from " << (clientIp.empty() ? "unknown" : clientIp);
    if (!requestedStreamId.empty()) {
        std::cerr << " requested_streamid=" << requestedStreamId;
    }
    std::cerr << std::endl;
    (void)sink;
}

void StreamManager::notifyStreamState(
    const StreamConfig& cfg,
    const std::string& color,
    const std::string& title,
    const std::string& details) {
    const std::string serverName = configManager.config.serverName.empty()
        ? "TVStreammerSAT5"
        : configManager.config.serverName;
    std::ostringstream message;
    const bool english = telegramUsesEnglish(configManager);
    message << color << " <b>" << telegramEscape(title) << "</b>\n"
            << (english ? "Server" : "Сервер") << ": <b>" << telegramEscape(serverName) << "</b>\n"
            << (english ? "Channel" : "Канал") << ": <b>" << telegramEscape(displayName(cfg)) << "</b>\n"
            << "ID: <code>" << telegramEscape(cfg.id) << "</code>";
    if (!details.empty()) {
        message << "\n" << telegramEscape(details);
    }
    telegramNotifier.sendMessage(message.str());
}

bool StreamManager::probeInputAvailable(
    const StreamConfig& baseConfig,
    const std::string& inputUri,
    std::chrono::milliseconds timeout) {
    if (inputUri.empty()) {
        return false;
    }

    StreamState probeState;
    probeState.config = baseConfig;
    probeState.config.inputUri = inputUri;
    probeState.config.testPattern = false;
    probeState.runtimeConfig = probeState.config;
    probeState.sourceContext = std::make_unique<RemapContext>();
    probeState.sourceContext->config = probeState.runtimeConfig;

    GstElement* pipeline = gst_pipeline_new(nullptr);
    if (!pipeline) {
        return false;
    }

    GstElement* sourceTail = nullptr;
    GstElement* source = createSourceChain(&probeState, pipeline, sourceTail);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (!source || !sourceTail || !sink ||
        !addElementOrFail(pipeline, sink) ||
        !gst_element_link(sourceTail, sink)) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
        return false;
    }

    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    std::atomic<bool> receivedData{false};
    GstPad* probePad = gst_element_get_static_pad(sourceTail, "src");
    if (probePad) {
        gst_pad_add_probe(
            probePad,
            static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
            [](GstPad*, GstPadProbeInfo*, gpointer userData) -> GstPadProbeReturn {
                static_cast<std::atomic<bool>*>(userData)->store(true, std::memory_order_relaxed);
                return GST_PAD_PROBE_OK;
            },
            &receivedData,
            nullptr);
        gst_object_unref(probePad);
    }

    GstBus* bus = gst_element_get_bus(pipeline);
    const GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    bool available = stateResult != GST_STATE_CHANGE_FAILURE;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (available && !receivedData.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
        GstMessage* message = gst_bus_timed_pop_filtered(
            bus,
            100 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!message) {
            continue;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR ||
            GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            available = false;
        }
        gst_message_unref(message);
    }

    available = available && receivedData.load(std::memory_order_relaxed);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) {
        gst_object_unref(bus);
    }
    gst_object_unref(pipeline);
    return available;
}

bool StreamManager::restartPipelineWithInput(StreamState* state, const std::string& inputUri, bool useBackup) {
    if (!state || inputUri.empty()) {
        return false;
    }

    GstElement* oldPipeline = state->pipeline;
    GstBus* oldBus = state->bus;

    stopHttpMpegTsInput(state);
    if (oldPipeline) {
        gst_element_set_state(oldPipeline, GST_STATE_NULL);
    }

    state->runtimeConfig = state->config;
    state->runtimeConfig.testPattern = false;
    if (!useBackup && state->sharedDvbInput && !state->sharedDvbServiceRelayUri.empty()) {
        state->runtimeConfig.inputUri = state->sharedDvbServiceRelayUri;
        state->runtimeConfig.inputMode = "udp";
        state->runtimeConfig.inputInterfaceAddress.clear();
        state->runtimeConfig.inputInterfaceAddressConfigured = true;
        state->runtimeConfig.inputServiceId = 0;
    } else {
        state->runtimeConfig.inputUri = inputUri;
    }
    state->sourceContext = std::make_unique<RemapContext>();
    state->sourceContext->config = state->runtimeConfig;
    state->outputContexts.clear();
    state->stableUdpNetworkTelemetry.store(false, std::memory_order_relaxed);
    state->stableUdpNetworkBytes = 0;
    state->lastStableUdpNetworkBytesSample = 0;

    GstElement* newPipeline = createPipeline(state);
    if (!newPipeline) {
        state->pipeline = oldPipeline;
        state->bus = oldBus;
        state->statusMessage = "error: restart failed";
        if (oldPipeline) {
            gst_element_set_state(oldPipeline, GST_STATE_PLAYING);
        }
        return false;
    }

    GstBus* newBus = gst_element_get_bus(newPipeline);
    state->pipeline = newPipeline;
    state->bus = newBus;
    state->usingBackup = useBackup;
    state->backupAttempted = useBackup;
    state->primaryRetryPending = !useBackup;
    state->inputLossNotified = false;
    state->activeInputUri = useBackup ? inputUri : state->primaryInputUri;
    state->active = true;
    state->statusMessage = useBackup ? "running on backup" : "running on primary";
    state->inputBytes = 0;
    state->outputBytes = 0;
    state->stableUdpNetworkBytes = 0;
    state->inputCcErrors = 0;
    state->inputCcErrorsDelta = 0;
    state->outputCcErrors = 0;
    state->outputCcErrorsDelta = 0;
    state->outputTsPayloadPackets = 0;
    state->outputTsScrambledPackets = 0;
    state->outputTsClearPesStarts = 0;
    state->outputTsPayloadPacketsDelta = 0;
    state->outputTsScrambledPacketsDelta = 0;
    state->outputTsClearPesStartsDelta = 0;
    state->lastOutputTsPayloadPacketsSample = 0;
    state->lastOutputTsScrambledPacketsSample = 0;
    state->lastOutputTsClearPesStartsSample = 0;
    state->inputBitrate = 0;
    state->outputBitrate = initialConfiguredOutputBitrate(state->config);
    state->lastInputBytesSample = 0;
    state->lastOutputBytesSample = 0;
    state->lastStableUdpNetworkBytesSample = 0;
    state->lastInputCcErrorsSample = 0;
    state->lastOutputCcErrorsSample = 0;
    state->lastInputBytesSeen = 0;
    {
        std::lock_guard<std::mutex> lock(state->inputContinuityMutex);
        state->inputContinuityValid.fill(false);
        state->inputTsRemainder.clear();
    }
    {
        std::lock_guard<std::mutex> lock(state->outputContinuityMutex);
        state->outputContinuityValid.fill(false);
        state->outputTsRemainder.clear();
    }
    {
        std::lock_guard<std::mutex> lock(state->outputScramblingMutex);
        state->outputScramblingRemainder.clear();
        state->outputTelemetryPmtPid = 0x1FFF;
        state->outputTelemetryMediaPids.fill(false);
        state->outputTelemetryMediaPidsKnown = false;
    }
    state->lastInputActivity = std::chrono::steady_clock::now();
    state->lastPrimaryRetry = state->lastInputActivity;
    state->lastBitrateSample = state->lastInputActivity;
    attachBitrateProbes(state);

    GstStateChangeReturn ret = gst_element_set_state(newPipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        // A failed source transition can leave elements in READY. Drive the
        // replacement to NULL before dropping it; the previous pipeline was
        // already stopped above, so leave the stream offline for the normal
        // monitor/restart path instead of retaining a half-started pipeline.
        gst_element_set_state(newPipeline, GST_STATE_NULL);
        gst_element_get_state(newPipeline, nullptr, nullptr, GST_SECOND);
        if (newBus) gst_object_unref(newBus);
        gst_object_unref(newPipeline);
        state->pipeline = nullptr;
        state->bus = nullptr;
        state->statusMessage = "error: restart playback failed";
        state->active = false;
        if (oldBus) gst_object_unref(oldBus);
        if (oldPipeline) gst_object_unref(oldPipeline);
        return false;
    }

    startHttpMpegTsInput(state);

    if (oldBus) {
        gst_object_unref(oldBus);
    }
    if (oldPipeline) {
        gst_object_unref(oldPipeline);
    }
    return true;
}


bool StreamManager::restartTranscodedInput(
    StreamState* state,
    const std::string& inputUri,
    bool useBackup) {
    if (!state || !state->gstTranscoder || inputUri.empty()) {
        return false;
    }

    const bool stableUdpRelay = allOutputsUseStableUdp(state->config);
    StreamConfig nextConfig = state->config;
    nextConfig.testPattern = false;
    if (!useBackup && state->sharedDvbInput && !state->sharedDvbServiceRelayUri.empty()) {
        nextConfig.inputUri = state->sharedDvbServiceRelayUri;
        nextConfig.inputMode = "udp";
        nextConfig.inputInterfaceAddress.clear();
        nextConfig.inputInterfaceAddressConfigured = true;
        nextConfig.inputServiceId = 0;
    } else {
        nextConfig.inputUri = inputUri;
    }

    std::cerr << "Transcoded input switch: from=" << state->activeInputUri
              << " to=" << inputUri
              << " backup=" << (useBackup ? "yes" : "no")
              << " mode=" << (stableUdpRelay ? "fifo-stable-udp" : "direct-output")
              << std::endl;

    state->gstTranscoder->stop();

    if (stableUdpRelay) {
        if (state->pipeline) {
            gst_element_set_state(state->pipeline, GST_STATE_NULL);
        }
        if (state->bus) {
            gst_object_unref(state->bus);
            state->bus = nullptr;
        }
        if (state->pipeline) {
            gst_object_unref(state->pipeline);
            state->pipeline = nullptr;
        }
    }

    state->runtimeConfig = nextConfig;

    std::string gstError;
    const StreamConfig transcoderConfig =
        stableUdpRelay ? transcodeRelayOutputConfig(nextConfig) : nextConfig;

    if (!state->gstTranscoder->start(transcoderConfig, gstError)) {
        state->statusMessage = "error: transcoded input restart failed: " + gstError;
        state->active = false;
        std::cerr << state->statusMessage << std::endl;
        return false;
    }

    if (stableUdpRelay) {
        state->stableUdpNetworkTelemetry.store(false, std::memory_order_relaxed);
        state->stableUdpNetworkBytes = 0;
        state->lastStableUdpNetworkBytesSample = 0;

        std::string relayError;
        GstElement* relayPipeline = createTranscodedUdpRelayPipeline(state, relayError);
        if (!relayPipeline) {
            state->gstTranscoder->stop();
            state->statusMessage = "error: transcoded UDP relay restart failed: " + relayError;
            state->active = false;
            return false;
        }

        state->pipeline = relayPipeline;
        state->bus = gst_element_get_bus(relayPipeline);
        attachBitrateProbes(state);

        if (gst_element_set_state(relayPipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            state->gstTranscoder->stop();
            if (state->bus) {
                gst_object_unref(state->bus);
                state->bus = nullptr;
            }
            gst_element_set_state(relayPipeline, GST_STATE_NULL);
            gst_element_get_state(relayPipeline, nullptr, nullptr, GST_SECOND);
            gst_object_unref(relayPipeline);
            state->pipeline = nullptr;
            state->statusMessage = "error: transcoded UDP relay playback restart failed";
            state->active = false;
            return false;
        }
    }

    state->usingBackup = useBackup;
    state->backupAttempted = useBackup;
    state->primaryRetryPending = !useBackup;
    state->inputLossNotified = false;
    state->activeInputUri = useBackup ? inputUri : state->primaryInputUri;
    state->active = true;
    state->statusMessage = useBackup ? "running on backup" : "running on primary";

    state->inputBytes = 0;
    state->outputBytes = 0;
    state->stableUdpNetworkBytes = 0;
    state->inputCcErrors = 0;
    state->inputCcErrorsDelta = 0;
    state->outputCcErrors = 0;
    state->outputCcErrorsDelta = 0;
    state->outputTsPayloadPackets = 0;
    state->outputTsScrambledPackets = 0;
    state->outputTsClearPesStarts = 0;
    state->outputTsPayloadPacketsDelta = 0;
    state->outputTsScrambledPacketsDelta = 0;
    state->outputTsClearPesStartsDelta = 0;
    state->lastOutputTsPayloadPacketsSample = 0;
    state->lastOutputTsScrambledPacketsSample = 0;
    state->lastOutputTsClearPesStartsSample = 0;
    state->inputBitrate = 0;
    state->outputBitrate = initialConfiguredOutputBitrate(state->config);
    state->lastInputBytesSample = 0;
    state->lastOutputBytesSample = 0;
    state->lastStableUdpNetworkBytesSample = 0;
    state->lastInputCcErrorsSample = 0;
    state->lastOutputCcErrorsSample = 0;
    state->lastInputBytesSeen = 0;
    state->lastInputActivity = std::chrono::steady_clock::now();
    state->lastPrimaryRetry = state->lastInputActivity;
    state->lastBitrateSample = state->lastInputActivity;
    {
        std::lock_guard<std::mutex> lock(state->outputScramblingMutex);
        state->outputScramblingRemainder.clear();
        state->outputTelemetryPmtPid = 0x1FFF;
        state->outputTelemetryMediaPids.fill(false);
        state->outputTelemetryMediaPidsKnown = false;
    }

    std::cerr << "Transcoded input switch complete: active="
              << state->activeInputUri
              << " using_backup=" << (state->usingBackup ? "yes" : "no")
              << std::endl;
    return true;
}

bool StreamManager::restartActiveInput(
    StreamState* state,
    const std::string& inputUri,
    bool useBackup) {
    if (state && state->gstTranscoder) {
        return restartTranscodedInput(state, inputUri, useBackup);
    }
    return restartPipelineWithInput(state, inputUri, useBackup);
}


GstElement* StreamManager::createPipeline(StreamState* state) {
    if (!state) {
        return nullptr;
    }
    const StreamConfig& cfg = state->config;
    GstElement* pipeline = gst_pipeline_new(cfg.id.c_str());
    if (!pipeline) {
        return nullptr;
    }

    GstElement* sourceTail = nullptr;
    if (!createSourceChain(state, pipeline, sourceTail) || !sourceTail) {
        state->statusMessage = "pipeline build failed: source-chain";
        std::cerr << "Pipeline build failed: stage=source-chain stream=" << cfg.name
                  << " input=" << cfg.inputUri << " input_mode=" << cfg.inputMode << std::endl;
        gst_object_unref(pipeline);
        return nullptr;
    }

    state->outputContexts.clear();
    if (cfg.transcodeEnabled) {
        std::string transcodeError;
        GstElement* transcoderBin = TranscoderModule::createBin(cfg, transcodeError);
        if (!transcoderBin || !addElementOrFail(pipeline, transcoderBin) ||
            !gst_element_link(sourceTail, transcoderBin)) {
            std::cerr << "Transcoder setup failed for " << cfg.id << ": " << transcodeError << std::endl;
            if (transcoderBin && !GST_OBJECT_PARENT(transcoderBin)) gst_object_unref(transcoderBin);
            gst_object_unref(pipeline);
            return nullptr;
        }
        sourceTail = transcoderBin;
    }
    if (!buildOutputBranches(state, pipeline, sourceTail)) {
        state->statusMessage = "pipeline build failed: output-branches";
        std::cerr << "Pipeline build failed: stage=output-branches stream=" << cfg.name
                  << " input=" << cfg.inputUri << " input_mode=" << cfg.inputMode
                  << " output=" << cfg.outputType << " remap=" << (cfg.remapEnabled ? 1 : 0)
                  << std::endl;
        gst_object_unref(pipeline);
        return nullptr;
    }

    return pipeline;
}

GstElement* StreamManager::createSourceChain(StreamState* state, GstElement* pipeline, GstElement*& terminalElement) {
    terminalElement = nullptr;
    if (!state) {
        return nullptr;
    }
    const StreamConfig& cfg = state->runtimeConfig;
    const std::string input = cfg.testPattern ? kTestPatternUri : cfg.inputUri;
    const std::string inputLower = toLower(input);
    const auto inputProtocol = tvs::stream_protocols::inputKind(cfg);

    auto addQueue = [&](const char* name, guint64 maxSizeTime = 3000000000ULL) -> GstElement* {
        GstElement* queue = gst_element_factory_make("queue", name);
        if (!addElementOrFail(pipeline, queue)) {
            return nullptr;
        }
        configureQueue(queue, maxSizeTime);
        return queue;
    };

    if (tvs::stream_protocols::isTestPatternInput(inputProtocol)) {
        return createTestPatternChain(cfg, pipeline, terminalElement);
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Dvb) {
        if (!hasElementFactory("dvbsrc") || !hasElementFactory("tsparse")) {
            std::cerr << "missing DVB input elements: dvbsrc or tsparse" << std::endl;
            return nullptr;
        }

        DvbSatelliteParams params;
        std::string dvbError;
        if (!DvbSatellite::parseUri(cfg.inputUri, params, dvbError)) {
            std::cerr << "invalid DVB input: " << dvbError << std::endl;
            return nullptr;
        }

        // v121 selects the service at the DVB demux/filter level.  On GStreamer
        // versions used by Ubuntu 24.04 the tsparse program_%u request pad can
        // deliver PAT/PMT/SI while failing to forward the service PES packets
        // (the observed symptom is ~50-100 kbit/s instead of the TV service).
        // Resolve PMT/PCR/ES PIDs and ask dvbsrc to capture exactly those PIDs,
        // then use tsparse's normal src pad. This preserves all codec/private
        // PES packets and avoids the request-pad segment-event warning.
        bool serviceScrambled = false;
        std::string servicePidError;
        if (cfg.inputServiceId > 0 && (params.pids.empty() || params.pids == "8192")) {
            std::string resolvedPids;
            if (DvbSatellite::resolveServicePids(
                    params, cfg.inputServiceId, resolvedPids, serviceScrambled, servicePidError)) {
                params.pids = resolvedPids;
                std::cerr << "DVB service PID auto-resolve: SID=" << cfg.inputServiceId
                          << " pids=" << params.pids
                          << " access=" << (serviceScrambled ? "CA" : "FTA") << std::endl;
            } else {
                // Never fall back to tsparse program_%u: full TS is preferable to
                // a false 93-kbit/s service. New scan-created tiles already carry
                // their service PID list in the dvb:// URI, so this path is mainly
                // for configurations created by v120 and older.
                params.pids = "8192";
                std::cerr << "DVB service PID auto-resolve failed for SID=" << cfg.inputServiceId
                          << ": " << servicePidError
                          << "; falling back to full transport stream" << std::endl;
            }
        }

        GstElement* src = gst_element_factory_make("dvbsrc", "input_dvb_src");
        GstElement* parse = gst_element_factory_make("tsparse", "input_dvb_tsparse");
        GstElement* queue = gst_element_factory_make("queue", "input_selected_queue");
        if (!src || !parse || !queue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, parse) ||
            !addElementOrFail(pipeline, queue)) {
            return nullptr;
        }

        if (!DvbSatellite::configureSource(src, params, dvbError)) {
            std::cerr << "DVB source configuration failed: " << dvbError << std::endl;
            return nullptr;
        }
        configureTsPacketAlignment(parse);
        configureQueue(queue, 3000000000ULL);
        if (!gst_element_link_many(src, parse, queue, nullptr)) {
            std::cerr << "DVB input pipeline link failed: dvbsrc -> tsparse -> queue" << std::endl;
            return nullptr;
        }

        // v127: dvbsrc service PID filtering still carries the transponder's
        // original PAT and SDT. Rewrite both to one selected service while
        // leaving PMT/PCR/PES/teletext/subtitle packets untouched.
        if (cfg.inputServiceId > 0 && params.pids != "8192") {
            GstPad* psiPad = gst_element_get_static_pad(parse, "src");
            if (psiPad) {
                auto* psiContext = new DvbSingleProgramPsiContext();
                psiContext->serviceId = static_cast<uint16_t>(cfg.inputServiceId & 0xFFFFU);
                psiContext->serviceName = cfg.serviceName;
                psiContext->serviceProvider = cfg.serviceProvider;
                gst_pad_add_probe(
                    psiPad,
                    GST_PAD_PROBE_TYPE_BUFFER,
                    dvbSingleProgramPsiProbe,
                    psiContext,
                    [](gpointer data) { delete static_cast<DvbSingleProgramPsiContext*>(data); });
                gst_object_unref(psiPad);
            }
        }

        std::cerr << "DVB-S/S2 input: adapter=" << params.adapter
                  << " frontend=" << params.frontend
                  << " frequency_mhz=" << (static_cast<double>(params.frequencyKHz) / 1000.0)
                  << " symbol_rate=" << params.symbolRateK
                  << " polarity=" << params.polarity
                  << " delsys=" << params.deliverySystem
                  << " input_sid=" << cfg.inputServiceId
                  << " selection=" << ((cfg.inputServiceId > 0 && params.pids != "8192")
                        ? "dvbsrc-service-pids" : "full-ts")
                  << " pids=" << params.pids
                  << std::endl;

        terminalElement = queue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Rtmp) {
        if (!hasElementFactory("rtmpsrc") || !hasElementFactory("flvdemux") || !hasElementFactory("mpegtsmux")) {
            std::cerr << "missing RTMP input elements: rtmpsrc, flvdemux or mpegtsmux" << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("rtmpsrc", "input_src");
        GstElement* inputQueue = addQueue("input_queue", 5000000000ULL);
        GstElement* demux = gst_element_factory_make("flvdemux", "input_flv_demux");
        GstElement* mux = gst_element_factory_make("mpegtsmux", "input_rtmp_ts_mux");
        GstElement* outputQueue = gst_element_factory_make("queue", "input_rtmp_ts_queue");
        if (!src || !inputQueue || !demux || !mux || !outputQueue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, demux) ||
            !addElementOrFail(pipeline, mux) ||
            !addElementOrFail(pipeline, outputQueue)) {
            return nullptr;
        }

        g_object_set(src,
            "location", input.c_str(),
            "do-timestamp", TRUE,
            "timeout", 15,
            nullptr);
        configureQueue(outputQueue);
        configureTsMux(mux, cfg);

        if (!gst_element_link_many(src, inputQueue, demux, nullptr) ||
            !gst_element_link(mux, outputQueue)) {
            return nullptr;
        }

        if (!state->sourceContext) {
            state->sourceContext = std::make_unique<RemapContext>();
        }
        state->sourceContext->mux = mux;
        state->sourceContext->config = cfg;
        state->sourceContext->flvMux = false;
        g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onFlvDemuxPadAdded), state->sourceContext.get());

        terminalElement = outputQueue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Rtsp) {
        if (!hasElementFactory("rtspsrc") || !hasElementFactory("mpegtsmux")) {
            std::cerr << "missing RTSP input elements: rtspsrc or mpegtsmux" << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("rtspsrc", "input_src");
        GstElement* mux = gst_element_factory_make("mpegtsmux", "input_rtsp_ts_mux");
        GstElement* outputQueue = gst_element_factory_make("queue", "input_queue");
        if (!src || !mux || !outputQueue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, mux) ||
            !addElementOrFail(pipeline, outputQueue)) {
            return nullptr;
        }

        g_object_set(src,
            "location", input.c_str(),
            "latency", 300,
            "do-rtsp-keep-alive", TRUE,
            nullptr);
        setUInt64PropertyIfPresent(src, "timeout", 15000000);
        setBooleanPropertyIfPresent(src, "ntp-sync", FALSE);
        configureQueue(outputQueue, 5000000000ULL);
        configureTsMux(mux, cfg);

        if (!gst_element_link(mux, outputQueue)) {
            return nullptr;
        }

        if (!state->sourceContext) {
            state->sourceContext = std::make_unique<RemapContext>();
        }
        state->sourceContext->mux = mux;
        state->sourceContext->config = cfg;
        state->sourceContext->flvMux = false;
        g_signal_connect(src, "pad-added", G_CALLBACK(StreamManager::onRtspPadAdded), state->sourceContext.get());

        terminalElement = outputQueue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Srt) {
        std::string mode = toLower(cfg.inputMode);
        const std::string inputInterface = configuredInputInterfaceAddress(cfg);
        const char* factory = (mode == "listener") ? "srtsrc" : "srtclientsrc";
        if (!hasElementFactory(factory)) {
            std::cerr << missingElementStatus(factory) << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make(factory, "input_src");
        GstElement* queue = addQueue("input_queue");
        if (!src || !queue || !addElementOrFail(pipeline, src)) {
            return nullptr;
        }

        g_object_set(src, "uri", input.c_str(), nullptr);
        setBooleanPropertyIfPresent(src, "do-timestamp", TRUE);
        setBooleanPropertyIfPresent(src, "auto-reconnect", TRUE);
        setIntPropertyIfPresent(src, "latency", 500);
        setStringPropertyIfPresent(src, "localaddress", inputInterface);
        if (mode == "listener") {
            setIntPropertyIfPresent(src, "mode", 2);
            setBooleanPropertyIfPresent(src, "wait-for-connection", TRUE);
            setBooleanPropertyIfPresent(src, "keep-listening", TRUE);
        } else {
            setIntPropertyIfPresent(src, "mode", 1);
            setBooleanPropertyIfPresent(src, "wait-for-connection", FALSE);
            setUIntPropertyIfPresent(src, "localport", 0);
        }

        if (!gst_element_link(src, queue)) {
            return nullptr;
        }

        terminalElement = queue;
        return src;
    }

    const std::string normalizedInputMode = toLower(cfg.inputMode);
    const bool directHttpMpegTs = isDirectHttpMpegTsConfig(cfg);

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Http && directHttpMpegTs) {
        // v187: tokenized IPTV HTTP MPEG-TS is fetched by libcurl and fed to
        // appsrc.  This creates exactly one provider request and avoids a
        // build-time dependency on souphttpsrc/caps negotiation for opaque URLs.
        GstElement* src = gst_element_factory_make("appsrc", "input_src");
        GstElement* queue = addQueue("input_queue");
        if (!src || !queue || !addElementOrFail(pipeline, src)) {
            if (src && !GST_OBJECT_PARENT(src)) gst_object_unref(src);
            std::cerr << "HTTP MPEG-TS input: failed to create appsrc/queue" << std::endl;
            return nullptr;
        }

        GstCaps* caps = gst_caps_from_string(
            "video/mpegts,systemstream=(boolean)true,packetsize=(int)188");
        if (!caps) {
            std::cerr << "HTTP MPEG-TS input: failed to create MPEG-TS caps" << std::endl;
            return nullptr;
        }
        gst_app_src_set_caps(GST_APP_SRC(src), caps);
        gst_caps_unref(caps);
        gst_app_src_set_stream_type(GST_APP_SRC(src), GST_APP_STREAM_TYPE_STREAM);
        gst_app_src_set_max_bytes(GST_APP_SRC(src), 4ULL * 1024ULL * 1024ULL);
        g_object_set(src,
            "is-live", TRUE,
            "format", GST_FORMAT_BYTES,
            "block", FALSE,
            "do-timestamp", FALSE,
            nullptr);

        if (!gst_element_link(src, queue)) {
            std::cerr << "HTTP MPEG-TS input: failed to link appsrc -> input_queue" << std::endl;
            return nullptr;
        }

        const std::string location = appendHlsAccessQuery(input, cfg);
        auto httpState = std::make_shared<HttpMpegTsCurlInputState>(src, cfg, location);
        state->httpMpegTsInputState = httpState;

        std::cerr << "HTTP MPEG-TS input: mode=single-request"
                  << " uri=" << input
                  << " access=" << (cfg.hlsAccessKeyMode == "none" ? "url-only" : cfg.hlsAccessKeyMode)
                  << " input_mode=" << (normalizedInputMode.empty() ? "auto" : normalizedInputMode)
                  << " hls_probe=bypassed transport=libcurl-appsrc start=after-pipeline-playing"
                  << std::endl;
        terminalElement = queue;
        return src;
    }

    // Opaque IPTV HTTP URLs often contain a per-session access token in the
    // path.  Do not pre-open such URLs when the user explicitly selects
    // HTTP MPEG-TS: some providers permit only one active request per token
    // and the old HLS sniff request could consume/lock that session before
    // souphttpsrc opened the real stream.
    // v187: do not pre-open opaque HTTP URLs. .m3u8 and explicit HLS mode are
    // classified as HLS by inputKind(); every other HTTP URL in Auto is treated
    // as a continuous MPEG-TS stream and opened exactly once through libcurl.
    const bool useHlsInput = inputProtocol == tvs::stream_protocols::InputProtocolKind::Hls;

    if (useHlsInput) {
        if (!hasElementFactory("souphttpsrc")) {
            std::cerr << missingElementStatus("souphttpsrc") << std::endl;
            return nullptr;
        }
        if (!hasElementFactory("hlsdemux")) {
            std::cerr << missingElementStatus("hlsdemux") << std::endl;
            return nullptr;
        }
        if (!hasElementFactory("mpegtsmux")) {
            std::cerr << missingElementStatus("mpegtsmux") << std::endl;
            return nullptr;
        }

        std::string location = input;
        if (inputLower.rfind("hls://", 0) == 0) {
            location = "http://" + input.substr(6);
        }
        location = appendHlsAccessQuery(location, cfg);

        const std::string inputInterface = configuredInputInterfaceAddress(cfg);
        GstElement* src = gst_element_factory_make("souphttpsrc", "input_src");
        GstElement* demux = gst_element_factory_make("hlsdemux", "hls_demux");
        GstElement* mux = gst_element_factory_make("mpegtsmux", "input_hls_ts_mux");
        GstElement* queue = addQueue("input_queue", 5000000000ULL);
        if (!src || !demux || !mux || !queue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, demux) ||
            !addElementOrFail(pipeline, mux)) {
            return nullptr;
        }

        g_object_set(src, "location", location.c_str(), "is-live", TRUE, "do-timestamp", TRUE, nullptr);
        setIntPropertyIfPresent(src, "timeout", 15);
        if (!inputInterface.empty()) {
            std::cerr << "HLS input: input_iface=" << inputInterface
                      << " is selected, but souphttpsrc uses the kernel route for HTTP sockets"
                      << std::endl;
        }
        setIntPropertyIfPresent(demux, "connection-speed", static_cast<gint>(std::max<uint64_t>(cfg.targetBitrate / 1000, 1)));
        configureTsMux(mux, cfg);

        if (!gst_element_link(src, demux) || !gst_element_link(mux, queue)) {
            return nullptr;
        }

        if (!state->sourceContext) {
            state->sourceContext = std::make_unique<RemapContext>();
        }
        state->sourceContext->mux = mux;
        state->sourceContext->config = cfg;
        state->sourceContext->flvMux = false;
        configureHlsHttpSource(src, state->sourceContext->config);
        // Legacy hlsdemux creates HTTP child sources for variant playlists,
        // segments and AES key URIs. Apply this stream's access credential to
        // every such child, not just to the first manifest request.
        g_signal_connect(demux, "deep-element-added", G_CALLBACK(onHlsDeepElementAdded), state->sourceContext.get());
        g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), state->sourceContext.get());
        terminalElement = queue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Http) {
        if (!hasElementFactory("souphttpsrc")) {
            std::cerr << missingElementStatus("souphttpsrc") << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("souphttpsrc", "input_src");
        GstElement* capsFilter = gst_element_factory_make("capsfilter", "input_http_ts_caps");
        GstElement* queue = addQueue("input_queue");
        if (!src || !capsFilter || !queue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, capsFilter)) {
            return nullptr;
        }

        const std::string inputInterface = configuredInputInterfaceAddress(cfg);
        // hls_access_* fields are deliberately retained for config backward
        // compatibility, but they are generic HTTP/HLS credentials since v185.
        // A token already embedded in the URL path is left byte-for-byte intact.
        const std::string httpLocation = appendHlsAccessQuery(input, cfg);
        g_object_set(src, "location", httpLocation.c_str(), "is-live", TRUE, "do-timestamp", FALSE, nullptr);
        configureHlsHttpSource(src, cfg);
        // Never request gzip/deflate for a continuous MPEG-TS byte stream.
        setBooleanPropertyIfPresent(src, "compress", FALSE);
        setIntPropertyIfPresent(src, "timeout", 15);
        setUIntPropertyIfPresent(src, "blocksize", kTsPacketsPerUdpBuffer * kTsPacketSize);

        GstCaps* tsCaps = gst_caps_from_string(
            "video/mpegts,systemstream=(boolean)true,packetsize=(int)188");
        if (!tsCaps) return nullptr;
        g_object_set(capsFilter, "caps", tsCaps, nullptr);
        gst_caps_unref(tsCaps);

        if (!inputInterface.empty()) {
            std::cerr << "HTTP MPEG-TS input: input_iface=" << inputInterface
                      << " is selected, but souphttpsrc uses the kernel route for HTTP sockets"
                      << std::endl;
        }

        std::cerr << "HTTP MPEG-TS input: mode="
                  << "legacy-http"
                  << " uri=" << input
                  << " access=" << (cfg.hlsAccessKeyMode == "none" ? "url-only" : cfg.hlsAccessKeyMode)
                  << " hls_probe=disabled"
                  << std::endl;

        if (!gst_element_link_many(src, capsFilter, queue, nullptr)) {
            return nullptr;
        }

        terminalElement = queue;
        return src;
    }

    if (UdpInput::handles(input)) {
        std::string error;
        GstElement* src = UdpInput::build(pipeline, cfg, terminalElement, error);
        if (!src) {
            std::cerr << error << std::endl;
        }
        return src;
    }
    const std::string location = fileLocationFromInput(input);
    GstElement* src = gst_element_factory_make("filesrc", "input_src");
    if (!src || !addElementOrFail(pipeline, src)) {
        return nullptr;
    }
    g_object_set(src, "location", location.c_str(), nullptr);

    // MPEG-TS replacement files can be passed directly to the existing TS output chain.
    if (isMpegTsFile(input)) {
        GstElement* queue = addQueue("input_queue");
        if (!queue || !gst_element_link(src, queue)) {
            return nullptr;
        }
        terminalElement = queue;
        return src;
    }

    // Container files such as MP4/MOV must be demuxed and remuxed to MPEG-TS first.
    if (!hasElementFactory("qtdemux") || !hasElementFactory("mpegtsmux")) {
        std::cerr << "missing MP4 replacement file elements: qtdemux or mpegtsmux" << std::endl;
        return nullptr;
    }

    GstElement* demux = gst_element_factory_make("qtdemux", "input_file_demux");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "input_file_ts_mux");
    GstElement* outputQueue = gst_element_factory_make("queue", "input_queue");
    if (!demux || !mux || !outputQueue ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, outputQueue)) {
        return nullptr;
    }

    configureQueue(outputQueue, 5000000000ULL);
    configureTsMux(mux, cfg);
    if (!gst_element_link(src, demux) || !gst_element_link(mux, outputQueue)) {
        return nullptr;
    }

    if (!state->sourceContext) {
        state->sourceContext = std::make_unique<RemapContext>();
    }
    state->sourceContext->mux = mux;
    state->sourceContext->config = cfg;
    state->sourceContext->flvMux = false;
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), state->sourceContext.get());

    terminalElement = outputQueue;
    return src;
}

GstElement* StreamManager::createTestPatternChain(const StreamConfig& cfg, GstElement* pipeline, GstElement*& terminalElement) {
    terminalElement = nullptr;
    const std::vector<const char*> required = {
        "videotestsrc", "capsfilter", "videoconvert", "x264enc", "h264parse",
        "audiotestsrc", "audioconvert", "audioresample", "avenc_aac", "aacparse",
        "mpegtsmux", "queue"
    };
    for (const char* element : required) {
        if (!hasElementFactory(element)) {
            std::cerr << missingElementStatus(element) << std::endl;
            return nullptr;
        }
    }

    GstElement* src = gst_element_factory_make("videotestsrc", "test_bars_src");
    GstElement* capsfilter = gst_element_factory_make("capsfilter", "test_bars_caps");
    GstElement* convert = gst_element_factory_make("videoconvert", "test_bars_convert");
    GstElement* encoder = gst_element_factory_make("x264enc", "test_bars_encoder");
    GstElement* parser = gst_element_factory_make("h264parse", "test_bars_h264parse");
    GstElement* videoQueue = gst_element_factory_make("queue", "test_bars_video_queue");
    GstElement* audioSrc = gst_element_factory_make("audiotestsrc", "test_tone_src");
    GstElement* audioConvert = gst_element_factory_make("audioconvert", "test_tone_convert");
    GstElement* audioResample = gst_element_factory_make("audioresample", "test_tone_resample");
    GstElement* audioCapsfilter = gst_element_factory_make("capsfilter", "test_tone_caps");
    GstElement* audioEncoder = gst_element_factory_make("avenc_aac", "test_tone_encoder");
    GstElement* audioParser = gst_element_factory_make("aacparse", "test_tone_aacparse");
    GstElement* audioQueue = gst_element_factory_make("queue", "test_tone_queue");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "test_bars_mux");
    // Keep the test source visible to the normal input bitrate probe.
    // The previous name (test_bars_queue) was not searched by attachBitrateProbes(),
    // which made a healthy test pattern show Bitrate In = 0/—.
    GstElement* queue = gst_element_factory_make("queue", "input_queue");

    if (!src || !capsfilter || !convert || !encoder || !parser || !videoQueue ||
        !audioSrc || !audioConvert || !audioResample || !audioCapsfilter || !audioEncoder ||
        !audioParser || !audioQueue || !mux || !queue) {
        return nullptr;
    }

    if (!addElementOrFail(pipeline, src) ||
        !addElementOrFail(pipeline, capsfilter) ||
        !addElementOrFail(pipeline, convert) ||
        !addElementOrFail(pipeline, encoder) ||
        !addElementOrFail(pipeline, parser) ||
        !addElementOrFail(pipeline, videoQueue) ||
        !addElementOrFail(pipeline, audioSrc) ||
        !addElementOrFail(pipeline, audioConvert) ||
        !addElementOrFail(pipeline, audioResample) ||
        !addElementOrFail(pipeline, audioCapsfilter) ||
        !addElementOrFail(pipeline, audioEncoder) ||
        !addElementOrFail(pipeline, audioParser) ||
        !addElementOrFail(pipeline, audioQueue) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, queue)) {
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string(
        "video/x-raw,format=I420,width=1920,height=1080,framerate=25/1,pixel-aspect-ratio=1/1");
    g_object_set(capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    GstCaps* audioCaps = gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2");
    g_object_set(audioCapsfilter, "caps", audioCaps, nullptr);
    gst_caps_unref(audioCaps);

    constexpr guint audioBitrate = 128000;
    const uint64_t availableVideoBitrate = cfg.targetBitrate > 500000
        ? (cfg.targetBitrate * 85 / 100) - audioBitrate
        : 1000000;
    guint bitrateKbps = static_cast<guint>(std::max<uint64_t>(availableVideoBitrate / 1000, 350));
    g_object_set(src,
        "is-live", TRUE,
        "pattern", 0,
        nullptr);
    g_object_set(encoder,
        "bitrate", bitrateKbps,
        "key-int-max", 50,
        "bframes", 0,
        "byte-stream", TRUE,
        nullptr);
    gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
    gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "veryfast");
    g_object_set(parser, "config-interval", 1, nullptr);
    g_object_set(audioSrc, "is-live", TRUE, "wave", 0, "freq", 1000.0, nullptr);
    g_object_set(audioEncoder, "bitrate", static_cast<gint64>(audioBitrate), nullptr);
    configureTsMux(mux, cfg);
    configureQueue(videoQueue);
    configureQueue(audioQueue);
    configureQueue(queue);

    if (!gst_element_link_many(src, capsfilter, convert, encoder, parser, videoQueue, mux, nullptr) ||
        !gst_element_link_many(audioSrc, audioConvert, audioResample, audioCapsfilter,
            audioEncoder, audioParser, audioQueue, mux, nullptr) ||
        !gst_element_link(mux, queue)) {
        return nullptr;
    }

    terminalElement = queue;
    return src;
}

bool StreamManager::buildOutputBranches(StreamState* state, GstElement* pipeline, GstElement* sourceTail) {
    if (!state || !pipeline || !sourceTail) {
        return false;
    }

    const auto outputs = pipelineOutputConfigs(state->config);
    if (outputs.empty()) {
        return false;
    }
    if (outputs.size() == 1) {
        return buildOutputBranch(state, pipeline, sourceTail, outputs.front(), 0);
    }

    if (!hasElementFactory("tee")) {
        std::cerr << missingElementStatus("tee") << std::endl;
        return false;
    }

    GstElement* tee = gst_element_factory_make("tee", "output_tee");
    if (!addElementOrFail(pipeline, tee)) {
        return false;
    }
    if (!gst_element_link(sourceTail, tee)) {
        return false;
    }

    for (size_t i = 0; i < outputs.size(); ++i) {
        GstElement* queue = gst_element_factory_make("queue", branchName("tee_queue", i).c_str());
        if (!addElementOrFail(pipeline, queue)) {
            return false;
        }
        configureQueue(queue);

        GstPad* teeSrcPad = gst_element_request_pad_simple(tee, "src_%u");
        GstPad* queueSinkPad = gst_element_get_static_pad(queue, "sink");
        if (!teeSrcPad || !queueSinkPad) {
            if (teeSrcPad) gst_object_unref(teeSrcPad);
            if (queueSinkPad) gst_object_unref(queueSinkPad);
            return false;
        }
        const bool linked = gst_pad_link(teeSrcPad, queueSinkPad) == GST_PAD_LINK_OK;
        gst_object_unref(teeSrcPad);
        gst_object_unref(queueSinkPad);
        if (!linked) {
            return false;
        }

        if (!buildOutputBranch(state, pipeline, queue, outputs[i], i)) {
            return false;
        }
    }

    return true;
}

bool StreamManager::buildOutputBranch(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    const std::string type = outputType(outputConfig);
    if (type == "rtmp" || type == "youtube") {
        return buildRtmpOutputPipeline(state, pipeline, sourceTail, outputConfig, branchIndex);
    }
    if (type == "rtsp") {
        return buildRtspOutputPipeline(state, pipeline, sourceTail, outputConfig, branchIndex);
    }
    if (type == "hls") {
        return buildHlsOutputPipeline(state, pipeline, sourceTail, outputConfig, branchIndex);
    }

    // A transcoded stream is already a finished single-program MPEG-TS produced by
    // TranscoderModule. Re-demuxing and remuxing it separately for UDP/SRT/HTTP/HLS
    // can drop the copied audio PID or split audio/video into different programs.
    // Feed the same transcoded TS to every TS-capable protocol and only apply remap
    // to non-transcoded passthrough streams.
    const bool transcodedInput = state && state->config.transcodeEnabled;
    const auto sourceProtocol = state
        ? tvs::stream_protocols::inputKind(state->runtimeConfig)
        : tvs::stream_protocols::InputProtocolKind::Unknown;
    const bool sharedDvbSpts = state && state->sharedDvbInput &&
        !state->sharedDvbServiceRelayUri.empty() &&
        state->runtimeConfig.inputUri == state->sharedDvbServiceRelayUri;
    const bool sourceAlreadySingleProgramTs = state && (
        state->runtimeConfig.testPattern || sharedDvbSpts ||
        isDirectHttpMpegTsConfig(state->runtimeConfig) ||
        (tvs::stream_protocols::isDvbInput(sourceProtocol) && state->runtimeConfig.inputServiceId > 0));
    // DVB service selection is done by dvbsrc PID filters resolved from the
    // selected service PMT (PAT/PMT/PCR + all elementary PIDs). Test bars are
    // also already a complete SPTS. Feeding either through another
    // tsdemux/mpegtsmux cycle can drop valid/private streams and is not required
    // by StableUdpOutput/WISI shaping. Remux only for explicit PID/SID remapping
    // or a generic multi-program input.
    const bool stableUdpRemux = usesStableUdpShaper(outputConfig) &&
        !transcodedInput && !sourceAlreadySingleProgramTs;
    const bool remapAlreadyApplied = state && state->dvbTsRemapApplied && sharedDvbSpts;
    const bool needsRemux = ((outputConfig.remapEnabled && !remapAlreadyApplied) || stableUdpRemux) && !transcodedInput;
    if (remapAlreadyApplied && outputConfig.remapEnabled) {
        std::cerr << "DVB remap passthrough: packet-level PID/SID rewrite already applied"
                  << " service_id=" << outputConfig.serviceId
                  << " video_pid=" << outputConfig.videoPid
                  << " audio_pid=" << outputConfig.audioPid
                  << " demux=off remux=off" << std::endl;
    }
    if (needsRemux) {
        if (stableUdpRemux) {
            std::cerr << "Unified UDP: rebuilding passthrough TS as a clean single-program transport"
                      << " mode=" << (udpCbrOutputEnabled(outputConfig) ? "CBR" : "VBR")
                      << " input_service_id=" << outputConfig.inputServiceId
                      << " service_id=" << outputConfig.serviceId
                      << " video_pid=" << outputConfig.videoPid
                      << " audio_pid=" << outputConfig.audioPid << std::endl;
        }
        return buildRemapPipeline(state, pipeline, sourceTail, outputConfig, branchIndex);
    }
    return buildPassthroughPipeline(state, pipeline, sourceTail, outputConfig, branchIndex);
}

bool StreamManager::buildPassthroughPipeline(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    if (!state) {
        return false;
    }
    const StreamConfig& cfg = outputConfig;

    // v122: StableUdpOutput already owns the output clock, five-second WISI
    // reservoir and PCR restamping.  A second tsparse with set-timestamps and
    // smoothing-latency immediately before the appsink can wait indefinitely
    // for an input PCR/timeline even while the selected DVB SPTS is flowing.
    // The observed signature was a healthy ~2 Mbit/s DVB input but only 2632
    // bytes (two 1316-byte chunks) reaching the reservoir, pcr_pid=0x1fff and
    // Bitrate Out=0.  Feed the already-normalised MPEG-TS directly to the
    // reservoir for UDP; all source chains reaching this function expose TS,
    // and DVB/test chains are already packet-aligned upstream.
    const bool directStableUdpTs = usesStableUdpShaper(cfg);
    GstElement* tsparse = directStableUdpTs
        ? nullptr
        : gst_element_factory_make("tsparse", branchName("tsparse", branchIndex).c_str());
    GstElement* queue = gst_element_factory_make("queue", branchName("output_queue", branchIndex).c_str());
    const bool cbrPacingActive = !isUdpOutput(cfg) && cbrMuxEnabled(cfg);
    GstElement* pacer = cbrPacingActive
        ? gst_element_factory_make("identity", branchName("cbr_pacer", branchIndex).c_str())
        : nullptr;
    GstElement* sink = createOutputSink(state, cfg, pipeline, branchName("output_sink", branchIndex));

    if ((!directStableUdpTs && !tsparse) || !queue || !sink || (cbrPacingActive && !pacer)) {
        return false;
    }

    if ((!directStableUdpTs && !addElementOrFail(pipeline, tsparse)) ||
        !addElementOrFail(pipeline, queue) ||
        (pacer && !addElementOrFail(pipeline, pacer))) {
        return false;
    }

    configureOutputQueue(queue, cfg);
    configureCbrPacer(pacer, cfg);

    const bool finalDvbRemapContinuity = directStableUdpTs &&
        state->dvbTsRemapApplied && cfg.remapEnabled;
    if (finalDvbRemapContinuity) {
        GstPad* psiPad = gst_element_get_static_pad(queue, "src");
        if (!psiPad) {
            std::cerr << "DVB final SDT remap: failed to get output queue src pad" << std::endl;
            return false;
        }
        auto* psiContext = new DvbRemapFinalPsiContext();
        psiContext->serviceId = static_cast<uint16_t>(cfg.serviceId & 0xFFFFU);
        psiContext->serviceName = cfg.serviceName.empty() ? cfg.name : cfg.serviceName;
        psiContext->serviceProvider = cfg.serviceProvider;
        gst_pad_add_probe(psiPad, GST_PAD_PROBE_TYPE_BUFFER,
            dvbRemapFinalPsiProbe, psiContext,
            [](gpointer data) { delete static_cast<DvbRemapFinalPsiContext*>(data); });
        gst_object_unref(psiPad);


        GstPad* queueSrcPad = gst_element_get_static_pad(queue, "src");
        if (!queueSrcPad) {
            std::cerr << "DVB remap final continuity guard: failed to get output queue src pad" << std::endl;
            return false;
        }
        auto* continuityContext = new DvbRemapFinalContinuityContext();
        continuityContext->streamId = state->config.id;
        continuityContext->branchIndex = branchIndex;
        gst_pad_add_probe(queueSrcPad, GST_PAD_PROBE_TYPE_BUFFER,
            dvbRemapFinalContinuityProbe, continuityContext,
            [](gpointer data) { delete static_cast<DvbRemapFinalContinuityContext*>(data); });
        gst_object_unref(queueSrcPad);
    }

    if (directStableUdpTs) {
        // PRE_WISI is a heavy passive TS continuity/hash diagnostic. Keep it
        // completely out of the production hot path unless explicitly enabled.
        if (dvbDiagnosticsEnabled()) {
            GstPad* preWisiCcPad = gst_element_get_static_pad(queue, "src");
            if (preWisiCcPad) {
                auto* ccContext = new TsCcStageProbeContext(state->config.id, "PRE_WISI");
                gst_pad_add_probe(preWisiCcPad, GST_PAD_PROBE_TYPE_BUFFER,
                    tsCcStageProbe, ccContext,
                    [](gpointer data) { delete static_cast<TsCcStageProbeContext*>(data); });
                gst_object_unref(preWisiCcPad);
            }
        }
        std::cerr << "Stable UDP passthrough: direct MPEG-TS -> WISI reservoir"
                  << " timestamp_tsparse=off smoothing=off"
                  << " packetization=preserve-upstream"
                  << " ts_diagnostics=" << (dvbDiagnosticsEnabled() ? "on" : "off")
                  << std::endl;
        return gst_element_link_many(sourceTail, queue, sink, nullptr);
    }

    configureTsPacketAlignment(tsparse);
    return pacer
        ? gst_element_link_many(sourceTail, tsparse, queue, pacer, sink, nullptr)
        : gst_element_link_many(sourceTail, tsparse, queue, sink, nullptr);
}

bool StreamManager::buildRemapPipeline(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    if (!state) {
        return false;
    }
    if (!hasElementFactory("tsparse") || !hasElementFactory("tsdemux") || !hasElementFactory("mpegtsmux")) {
        std::cerr << "missing remap elements: tsparse, tsdemux or mpegtsmux" << std::endl;
        return false;
    }

    const StreamConfig& cfg = outputConfig;
    GstElement* tsparse = gst_element_factory_make("tsparse", branchName("remap_tsparse", branchIndex).c_str());
    GstElement* preDemuxQueue = gst_element_factory_make("queue", branchName("remap_pre_demux_queue", branchIndex).c_str());
    GstElement* demux = gst_element_factory_make("tsdemux", branchName("demux", branchIndex).c_str());
    GstElement* mux = gst_element_factory_make("mpegtsmux", branchName("mux", branchIndex).c_str());
    const bool cbrActive = !isUdpOutput(cfg) && cbrMuxEnabled(cfg);
    GstElement* outputQueue = gst_element_factory_make("queue", branchName("output_queue", branchIndex).c_str());
    GstElement* pacer = cbrActive ? gst_element_factory_make("identity", branchName("cbr_pacer", branchIndex).c_str()) : nullptr;
    GstElement* sink = createOutputSink(state, cfg, pipeline, branchName("output_sink", branchIndex));
    if (!tsparse || !preDemuxQueue || !demux || !mux || !outputQueue || !sink ||
        (cbrActive && !pacer)) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, preDemuxQueue) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, outputQueue) ||
        (pacer && !addElementOrFail(pipeline, pacer))) {
        return false;
    }

    configureQueue(preDemuxQueue);
    configureOutputQueue(outputQueue, cfg);
    configureCbrPacer(pacer, cfg);
    configureTsMux(mux, cfg);

    if (usesStableUdpShaper(cfg)) {
        if (udpCbrOutputEnabled(cfg) && cfg.targetBitrate == 0) {
            std::cerr << "UDP CBR requires Target bitrate greater than zero" << std::endl;
            return false;
        }
        // Test pattern and DVB source chains are already a clean single-program TS
        // before they reach the output branch.  The DVB chain has already selected
        // cfg.inputServiceId at its first tsdemux.  Selecting that original SID a
        // second time here is wrong because the intermediate mpegtsmux creates a new
        // single-program transport (normally program 1).  The same applies to the
        // synthetic test transport.  AUTO therefore selects the only program here.
        // Other multi-program inputs (UDP/SRT/File/etc.) still use inputServiceId.
        const bool sourceAlreadySingleProgram =
            cfg.testPattern ||
            tvs::stream_protocols::isDvbInput(tvs::stream_protocols::inputKind(cfg));
        const uint32_t inputServiceId = sourceAlreadySingleProgram ? 0U : cfg.inputServiceId;
        if (inputServiceId > 0) {
            setIntPropertyIfPresent(demux, "program-number", static_cast<gint>(inputServiceId));
        }
        std::cerr << "Unified UDP mux: bitrate=0 mode="
                  << (udpCbrOutputEnabled(cfg) ? "CBR" : "VBR")
                  << " external_shaper=" << (udpCbrOutputEnabled(cfg) ? cfg.targetBitrate : 0)
                  << " input_sid=" << inputServiceId
                  << " output_sid=" << cfg.serviceId
                  << " audio_reservoir_ms=" << (udpCbrOutputEnabled(cfg) ? 1500 : 0)
                  << " audio_reservoir_mode=" << (udpCbrOutputEnabled(cfg) ? "startup-only" : "off")
                  << " audio_pacer=" << (udpCbrOutputEnabled(cfg) ? "clocksync" : "off")
                  << " alignment=" << kTsPacketsPerUdpBuffer
                  << " pcr_interval=1800 pat_pmt_interval=9000" << std::endl;
    }
    sendServiceDescription(mux, cfg);

    if (!gst_element_link_many(sourceTail, tsparse, preDemuxQueue, demux, nullptr)) {
        return false;
    }
    const bool outputLinked = pacer
        ? gst_element_link_many(mux, outputQueue, pacer, sink, nullptr)
        : gst_element_link_many(mux, outputQueue, sink, nullptr);
    if (!outputLinked) {
        return false;
    }

    auto context = std::make_unique<RemapContext>();
    context->mux = mux;
    context->sink = sink;
    context->config = cfg;

    const bool stableUdpRemap = usesStableUdpShaper(cfg) && cfg.remapEnabled;
    if (stableUdpRemap) {
        // Configure the complete output program map before any elementary data is
        // linked into mpegtsmux. This avoids live prog-map replacement and makes
        // UDP remapping deterministic: input SID only selects tsdemux, while
        // serviceId is the new output program number.
        if (cfg.serviceId == 0 || cfg.videoPid == 0 || cfg.audioPid == 0) {
            std::cerr << "UDP remap requires non-zero output SID, V-PID and A-PID" << std::endl;
            return false;
        }

        const std::string videoPadName = "sink_" + std::to_string(cfg.videoPid);
        const std::string audioPadName = "sink_" + std::to_string(cfg.audioPid);
        GstPad* videoPad = gst_element_request_pad_simple(mux, videoPadName.c_str());
        GstPad* audioPad = gst_element_request_pad_simple(mux, audioPadName.c_str());
        if (!videoPad || !audioPad) {
            if (videoPad) gst_object_unref(videoPad);
            if (audioPad) gst_object_unref(audioPad);
            std::cerr << "UDP remap failed to reserve output PID pads: video="
                      << cfg.videoPid << " audio=" << cfg.audioPid << std::endl;
            return false;
        }
        context->preallocatedVideoMuxPad = videoPad;
        context->preallocatedAudioMuxPad = audioPad;

        GstStructure* programMap = gst_structure_new_empty("program_map");
        gst_structure_set(programMap,
            videoPadName.c_str(), G_TYPE_INT, static_cast<gint>(cfg.serviceId),
            audioPadName.c_str(), G_TYPE_INT, static_cast<gint>(cfg.serviceId),
            nullptr);
        g_object_set(mux, "prog-map", programMap, nullptr);
        gst_structure_free(programMap);

        context->programMapApplied = true;
        context->videoPadName = videoPadName;
        context->audioPadName = audioPadName;
        std::cerr << "UDP remap program map: input_sid="
                  << cfg.inputServiceId
                  << " output_sid=" << cfg.serviceId
                  << " video=" << videoPadName
                  << " audio=" << audioPadName << std::endl;
    }

    RemapContext* contextPtr = context.get();
    state->outputContexts.push_back(std::move(context));
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), contextPtr);
    return true;
}

bool StreamManager::buildHlsOutputPipeline(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    if (!state || !pipeline || !sourceTail) return false;
    const StreamConfig& cfg = outputConfig;

    // Explicit PID/SID remapping still needs our mpegtsmux so the configured
    // output mapping is preserved.  Feeding that freshly remuxed TS to the old
    // hlssink is safe because the mux now generates key-unit aware output.
    if (cfg.remapEnabled || !hasElementFactory("hlssink2")) {
        if (!hasElementFactory("hlssink2")) {
            std::cerr << "HLS: hlssink2 unavailable, using mpegtsmux -> hlssink fallback" << std::endl;
        } else {
            std::cerr << "HLS: explicit remap enabled, using mpegtsmux -> hlssink to preserve SID/PIDs" << std::endl;
        }
        return buildRemapPipeline(state, pipeline, sourceTail, cfg, branchIndex);
    }

    for (const char* factory : {"tsparse", "queue", "tsdemux", "hlssink2"}) {
        if (!hasElementFactory(factory)) {
            std::cerr << missingElementStatus(factory) << " required for HLS" << std::endl;
            return false;
        }
    }

    GstElement* tsparse = gst_element_factory_make("tsparse", branchName("hls_tsparse", branchIndex).c_str());
    GstElement* queue = gst_element_factory_make("queue", branchName("hls_pre_demux_queue", branchIndex).c_str());
    GstElement* demux = gst_element_factory_make("tsdemux", branchName("hls_ts_demux", branchIndex).c_str());
    GstElement* sink = gst_element_factory_make("hlssink2", branchName("hls_sink2", branchIndex).c_str());
    if (!tsparse || !queue || !demux || !sink) {
        if (tsparse) gst_object_unref(tsparse);
        if (queue) gst_object_unref(queue);
        if (demux) gst_object_unref(demux);
        if (sink) gst_object_unref(sink);
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, queue) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, sink)) {
        if (tsparse && !GST_OBJECT_PARENT(tsparse)) gst_object_unref(tsparse);
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (demux && !GST_OBJECT_PARENT(demux)) gst_object_unref(demux);
        if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
        return false;
    }

    configureTsPacketAlignment(tsparse);
    configureQueue(queue);
    configureHlsSink(sink, cfg);

    const bool sourceAlreadySingleProgram =
        cfg.testPattern ||
        tvs::stream_protocols::isDvbInput(tvs::stream_protocols::inputKind(cfg));
    if (!sourceAlreadySingleProgram && cfg.inputServiceId > 0) {
        setIntPropertyIfPresent(demux, "program-number", static_cast<gint>(cfg.inputServiceId));
    }

    if (!gst_element_link_many(sourceTail, tsparse, queue, demux, nullptr)) {
        std::cerr << "HLS pipeline link failed before tsdemux" << std::endl;
        return false;
    }

    auto context = std::make_unique<RemapContext>();
    context->mux = sink;
    context->sink = sink;
    context->config = cfg;
    context->hlsSink2 = true;
    RemapContext* contextPtr = context.get();
    state->outputContexts.push_back(std::move(context));
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), contextPtr);

    std::cerr << "HLS output: tsdemux -> elementary parsers -> hlssink2"
              << " target_duration=4 playlist_length=5 max_files=8"
              << " source_keyframes=preserved remux=internal"
              << std::endl;
    return true;
}

bool StreamManager::buildRtspOutputPipeline(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    if (!state || !pipeline || !sourceTail) return false;
    for (const char* factory : {"tsparse", "queue", "tsdemux", "rtspclientsink"}) {
        if (!hasElementFactory(factory)) {
            std::cerr << missingElementStatus(factory) << " required for RTSP Push" << std::endl;
            return false;
        }
    }

    const StreamConfig& cfg = outputConfig;
    GstElement* tsparse = gst_element_factory_make("tsparse", branchName("rtsp_tsparse", branchIndex).c_str());
    GstElement* queue = gst_element_factory_make("queue", branchName("rtsp_pre_demux_queue", branchIndex).c_str());
    GstElement* demux = gst_element_factory_make("tsdemux", branchName("rtsp_ts_demux", branchIndex).c_str());
    GstElement* sink = createOutputSink(state, cfg, pipeline, branchName("rtsp_client_sink", branchIndex));
    if (!tsparse || !queue || !demux || !sink) return false;

    if (!addElementOrFail(pipeline, tsparse) || !addElementOrFail(pipeline, queue) ||
        !addElementOrFail(pipeline, demux)) return false;
    configureTsPacketAlignment(tsparse);
    configureQueue(queue);
    if (!gst_element_link_many(sourceTail, tsparse, queue, demux, nullptr)) {
        std::cerr << "RTSP Push pipeline link failed before tsdemux" << std::endl;
        return false;
    }

    auto context = std::make_unique<RemapContext>();
    context->mux = sink;
    context->sink = sink;
    context->config = cfg;
    context->rtspPush = true;
    RemapContext* contextPtr = context.get();
    state->outputContexts.push_back(std::move(context));
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), contextPtr);

    std::cerr << "RTSP Push passthrough: tsdemux -> elementary parsers -> rtspclientsink"
              << " location=" << tvs::protocols::rtspOutputLocation(cfg)
              << " listener=off" << std::endl;
    return true;
}

bool StreamManager::buildRtmpOutputPipeline(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    if (!state) {
        return false;
    }
    if (!hasElementFactory("tsparse") || !hasElementFactory("tsdemux") || !hasElementFactory("flvmux")) {
        std::cerr << "missing RTMP output elements: tsparse, tsdemux or flvmux" << std::endl;
        return false;
    }

    const StreamConfig& cfg = outputConfig;
    GstElement* tsparse = gst_element_factory_make("tsparse", branchName("rtmp_tsparse", branchIndex).c_str());
    GstElement* preDemuxQueue = gst_element_factory_make("queue", branchName("rtmp_pre_demux_queue", branchIndex).c_str());
    GstElement* demux = gst_element_factory_make("tsdemux", branchName("rtmp_ts_demux", branchIndex).c_str());
    GstElement* mux = gst_element_factory_make("flvmux", branchName("rtmp_flv_mux", branchIndex).c_str());
    GstElement* outputQueue = gst_element_factory_make("queue", branchName("output_queue", branchIndex).c_str());
    GstElement* sink = createOutputSink(state, cfg, pipeline, branchName("output_sink", branchIndex));
    if (!tsparse || !preDemuxQueue || !demux || !mux || !outputQueue || !sink) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, preDemuxQueue) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, outputQueue)) {
        return false;
    }

    configureQueue(preDemuxQueue);
    configureQueue(outputQueue);
    configureTsPacketAlignment(tsparse);
    g_object_set(mux,
        "streamable", TRUE,
        "enforce-increasing-timestamps", TRUE,
        "skip-backwards-streams", TRUE,
        nullptr);

    if (!gst_element_link_many(sourceTail, tsparse, preDemuxQueue, demux, nullptr)) {
        return false;
    }
    if (!gst_element_link_many(mux, outputQueue, sink, nullptr)) {
        return false;
    }

    auto context = std::make_unique<RemapContext>();
    context->mux = mux;
    context->sink = sink;
    context->config = cfg;
    context->flvMux = true;
    RemapContext* contextPtr = context.get();
    state->outputContexts.push_back(std::move(context));
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), contextPtr);
    return true;
}

GstElement* StreamManager::createOutputSink(StreamState* state, const StreamConfig& cfg, GstElement* pipeline, const std::string& sinkName) {
    const std::string type = outputType(cfg);
    const auto outputProtocol = tvs::stream_protocols::outputKind(cfg);
    if (outputProtocol == tvs::stream_protocols::OutputProtocolKind::Unknown) {
        std::cerr << "unknown output protocol module for type: " << type << std::endl;
        return nullptr;
    }
    if (isUdpOutputType(type)) {
        // UDP-CBR and UDP-VBR share one stable MPEG-TS reservoir/shaper. CBR
        // uses Target bitrate with NULL padding; VBR follows the measured source
        // rate with the same startup reservoir, packetization and periodic PCR.
        std::string error;
        const bool claimNetworkTelemetry = state &&
            !state->stableUdpNetworkTelemetry.load(std::memory_order_relaxed);
        std::atomic<uint64_t>* networkBytes = claimNetworkTelemetry
            ? &state->stableUdpNetworkBytes
            : nullptr;
        GstElement* sink = StableUdpOutput::createSink(pipeline, cfg, sinkName, error, networkBytes);
        if (!sink) {
            std::cerr << error << std::endl;
        } else if (claimNetworkTelemetry) {
            state->stableUdpNetworkBytes = 0;
            state->lastStableUdpNetworkBytesSample = 0;
            state->stableUdpNetworkTelemetry.store(true, std::memory_order_relaxed);
        }
        return sink;
    }
    if (type == "rtp") {
        GstElement* sink = createRtpMpegTsSink(cfg, sinkName);
        if (!sink || !addElementOrFail(pipeline, sink)) {
            if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
            return nullptr;
        }
        return sink;
    }

    const char* factory = "srtsink";
    if (type == "http") {
        factory = "multifdsink";
    } else if (type == "hls") {
        factory = "hlssink";
    } else if (type == "rtsp") {
        factory = "rtspclientsink";
    } else if (type == "rtmp" || type == "youtube") {
        factory = "rtmpsink";
    }

    if (!hasElementFactory(factory)) {
        std::cerr << missingElementStatus(factory) << std::endl;
        return nullptr;
    }

    GstElement* sink = gst_element_factory_make(factory, sinkName.c_str());
    if (!sink || !addElementOrFail(pipeline, sink)) {
        return nullptr;
    }

    if (type == "srt") {
        attachSrtConnectionMonitoring(sink, cfg);
        configureSrtSink(sink, cfg, configManager.subscribers.filteringEnabled);
    } else if (type == "http") {
        configureHttpSink(sink, cfg);
        // Connect once per sink.  multifdsink never closes client descriptors
        // itself; onHttpClientFdRemoved owns that final close.
        g_signal_connect(sink, "client-fd-removed", G_CALLBACK(StreamManager::onHttpClientFdRemoved), this);
    } else if (type == "hls") {
        configureHlsSink(sink, cfg);
    } else if (type == "rtsp") {
        const std::string location = tvs::protocols::rtspOutputLocation(cfg);
        g_object_set(sink, "location", location.c_str(), nullptr);
        setIntPropertyIfPresent(sink, "protocols", 4);
        setUIntPropertyIfPresent(sink, "latency", 200);
        std::cerr << "RTSP Push output: location=" << location << " transport=tcp listener=off" << std::endl;
    } else if (type == "rtmp" || type == "youtube") {
        configureRtmpSink(sink, cfg);
    }

    return sink;
}

void StreamManager::onDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data) {
    (void)demux;
    auto* ctx = static_cast<RemapContext*>(user_data);
    if (!ctx || !ctx->mux) {
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    std::string capsString = caps ? gst_caps_to_string(caps) : "unknown";
    const bool isMpegTs = capsString.find("video/mpegts") != std::string::npos ||
        capsString.find("video/mpegts") != std::string::npos ||
        capsString.find("application/x-mpegts") != std::string::npos;
    if (isMpegTs) {
        // Some hlsdemux versions expose complete MPEG-TS fragments instead of
        // elementary audio/video pads. Do not mistake video/mpegts for MPEG-2
        // video. Insert tsdemux and feed its elementary pads to the existing mux.
        GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(ctx->mux));
        GstElement* tsdemux = gst_element_factory_make("tsdemux", nullptr);
        if (pipeline && tsdemux && gst_bin_add(GST_BIN(pipeline), tsdemux)) {
            gst_element_sync_state_with_parent(tsdemux);
            GstPad* sinkPad = gst_element_get_static_pad(tsdemux, "sink");
            if (sinkPad && gst_pad_link(pad, sinkPad) == GST_PAD_LINK_OK) {
                g_signal_connect(tsdemux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), ctx);
                gst_object_unref(sinkPad);
                if (caps) gst_caps_unref(caps);
                gst_object_unref(pipeline);
                return;
            }
            if (sinkPad) gst_object_unref(sinkPad);
        }
        if (tsdemux && !GST_OBJECT_PARENT(tsdemux)) gst_object_unref(tsdemux);
        if (pipeline) gst_object_unref(pipeline);
    }
    bool isAudio = capsString.find("audio/") != std::string::npos;
    bool isVideo = capsString.find("video/") != std::string::npos;
    bool isPrivateTs = capsString.find("private") != std::string::npos || capsString.find("subpicture") != std::string::npos;

    if ((!isAudio && !isVideo) || isPrivateTs) {
        if (caps) gst_caps_unref(caps);
        drainDynamicPad(ctx->mux, pad);
        return;
    }
    if ((isVideo && ctx->videoLinked) || (isAudio && ctx->audioLinked)) {
        if (caps) gst_caps_unref(caps);
        drainDynamicPad(ctx->mux, pad);
        return;
    }

    std::string parserFactory = parserForCaps(caps, capsString);
    if (caps) {
        gst_caps_unref(caps);
        caps = nullptr;
    }
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* parser = parserFactory.empty() ? nullptr : gst_element_factory_make(parserFactory.c_str(), nullptr);
    GstElement* capsfilter = capsFilterForMux(ctx->flvMux, isVideo, isAudio, capsString, parserFactory);

    const bool stableUdpAudioReservoir =
        isAudio && !ctx->flvMux &&
        usesStableUdpShaper(ctx->config) &&
        udpCbrOutputEnabled(ctx->config);

    GstElement* audioReservoirQueue = stableUdpAudioReservoir
        ? gst_element_factory_make("queue", nullptr)
        : nullptr;
    GstElement* audioClockSync = stableUdpAudioReservoir
        ? gst_element_factory_make("clocksync", nullptr)
        : nullptr;

    if (!queue || !parser ||
        (stableUdpAudioReservoir && (!audioReservoirQueue || !audioClockSync))) {
        std::cerr << "remap skipped unsupported elementary stream caps: " << capsString;
        if (stableUdpAudioReservoir && !audioClockSync) {
            std::cerr << " (clocksync unavailable for Stable UDP audio reservoir)";
        }
        std::cerr << std::endl;
        if (queue) gst_object_unref(queue);
        if (parser) gst_object_unref(parser);
        if (capsfilter) gst_object_unref(capsfilter);
        if (audioReservoirQueue) gst_object_unref(audioReservoirQueue);
        if (audioClockSync) gst_object_unref(audioClockSync);
        drainDynamicPad(ctx->mux, pad);
        return;
    }

    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(ctx->mux));
    if (!pipeline) {
        gst_object_unref(queue);
        return;
    }

    if (!gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), parser) ||
        (capsfilter && !gst_bin_add(GST_BIN(pipeline), capsfilter)) ||
        (audioReservoirQueue && !gst_bin_add(GST_BIN(pipeline), audioReservoirQueue)) ||
        (audioClockSync && !gst_bin_add(GST_BIN(pipeline), audioClockSync))) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (parser && !GST_OBJECT_PARENT(parser)) gst_object_unref(parser);
        if (capsfilter && !GST_OBJECT_PARENT(capsfilter)) gst_object_unref(capsfilter);
        if (audioReservoirQueue && !GST_OBJECT_PARENT(audioReservoirQueue)) gst_object_unref(audioReservoirQueue);
        if (audioClockSync && !GST_OBJECT_PARENT(audioClockSync)) gst_object_unref(audioClockSync);
        gst_object_unref(pipeline);
        return;
    }

    configureQueue(queue);

    if (stableUdpAudioReservoir) {
        // Compressed AAC is not decoded or modified. Build a 1500 ms startup
        // reserve, then release parsed AAC buffers according to their original
        // timestamps. The queue threshold is disabled after the initial fill so
        // it cannot periodically re-buffer and create later audio stalls.
        configureQueue(audioReservoirQueue, kStableUdpAudioReservoirMax);
        setUInt64PropertyIfPresent(
            audioReservoirQueue, "min-threshold-time", kStableUdpAudioReservoir);
        setIntPropertyIfPresent(audioReservoirQueue, "leaky", 0);
        g_signal_connect(
            audioReservoirQueue,
            "running",
            G_CALLBACK(onStableUdpAudioReservoirRunning),
            nullptr);

        setBooleanPropertyIfPresent(audioClockSync, "sync", TRUE);
        setBooleanPropertyIfPresent(audioClockSync, "sync-to-first", TRUE);
    }
    if (parserFactory == "h264parse" || parserFactory == "h265parse") {
        g_object_set(parser, "config-interval", 1, nullptr);
    }
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(parser);
    if (capsfilter) {
        gst_element_sync_state_with_parent(capsfilter);
    }
    if (audioReservoirQueue) {
        gst_element_sync_state_with_parent(audioReservoirQueue);
    }
    if (audioClockSync) {
        gst_element_sync_state_with_parent(audioClockSync);
    }

    const bool parserLinked = capsfilter
        ? gst_element_link_many(queue, parser, capsfilter, nullptr)
        : gst_element_link(queue, parser);
    if (!parserLinked) {
        gst_object_unref(pipeline);
        drainDynamicPad(ctx->mux, pad);
        return;
    }

    GstElement* parserTail = capsfilter ? capsfilter : parser;
    if (stableUdpAudioReservoir &&
        !gst_element_link_many(parserTail, audioReservoirQueue, audioClockSync, nullptr)) {
        std::cerr << "Stable UDP audio reservoir link failed" << std::endl;
        gst_object_unref(pipeline);
        drainDynamicPad(ctx->mux, pad);
        return;
    }

    GstPad* queueSinkPad = gst_element_get_static_pad(queue, "sink");
    if (!queueSinkPad) {
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(pad, queueSinkPad) != GST_PAD_LINK_OK) {
        gst_object_unref(queueSinkPad);
        gst_object_unref(pipeline);
        drainDynamicPad(ctx->mux, pad);
        return;
    }
    gst_object_unref(queueSinkPad);

    uint32_t requestedPid = isVideo ? ctx->config.videoPid : ctx->config.audioPid;
    if (requestedPid == 0) {
        requestedPid = pidFromDemuxPadName(pad);
    }

    GstElement* muxSourceElement =
        stableUdpAudioReservoir ? audioClockSync : parserTail;
    GstPad* parserSrcPad = gst_element_get_static_pad(muxSourceElement, "src");
    GstPad* muxSinkPad = nullptr;
    const bool stableUdpPreMapped = !ctx->flvMux && usesStableUdpShaper(ctx->config) &&
                                    ctx->config.remapEnabled && ctx->programMapApplied;
    if (stableUdpPreMapped) {
        GstPad* reservedPad = isVideo ? ctx->preallocatedVideoMuxPad : ctx->preallocatedAudioMuxPad;
        if (reservedPad) {
            muxSinkPad = GST_PAD(gst_object_ref(reservedPad));
        }
    } else if (ctx->hlsSink2) {
        muxSinkPad = gst_element_request_pad_simple(ctx->mux, isVideo ? "video" : "audio");
    } else if (ctx->rtspPush) {
        muxSinkPad = gst_element_request_pad_simple(ctx->mux, "sink_%u");
    } else {
        muxSinkPad = ctx->flvMux
            ? requestFlvMuxSinkPad(ctx->mux, isVideo)
            : requestMuxSinkPad(ctx->mux, requestedPid);
    }
    if (!parserSrcPad || !muxSinkPad) {
        if (parserSrcPad) gst_object_unref(parserSrcPad);
        if (muxSinkPad) gst_object_unref(muxSinkPad);
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(parserSrcPad, muxSinkPad) == GST_PAD_LINK_OK) {
        std::cerr << (ctx->hlsSink2 ? "HLS linked " : (ctx->rtspPush ? "RTSP push linked " : "remap linked ")) << (isAudio ? "audio" : "video")
                  << " caps=" << capsString << " parser=" << parserFactory
                  << " pid=" << requestedPid
                  << (stableUdpPreMapped ? " output_sid=" + std::to_string(ctx->config.serviceId) : "")
                  << (stableUdpAudioReservoir
                      ? " audio_reservoir_ms=1500 audio_reservoir_mode=startup-only audio_pacer=clocksync(sync-to-first)"
                      : "")
                  << std::endl;
        const gchar* padName = GST_PAD_NAME(muxSinkPad);
        if (isVideo) {
            ctx->videoLinked = true;
            ctx->videoPadName = padName ? padName : "";
        }
        if (isAudio) {
            ctx->audioLinked = true;
            ctx->audioPadName = padName ? padName : "";
        }
        if (!ctx->rtspPush && !ctx->hlsSink2) updateMuxProgramMap(ctx);
    }

    gst_object_unref(parserSrcPad);
    gst_object_unref(muxSinkPad);
    gst_object_unref(pipeline);
}

void StreamManager::onFlvDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data) {
    onDemuxPadAdded(demux, pad, user_data);
}

void StreamManager::onRtspPadAdded(GstElement* src, GstPad* pad, gpointer user_data) {
    (void)src;
    auto* ctx = static_cast<RemapContext*>(user_data);
    if (!ctx || !ctx->mux) {
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    std::string capsString = caps ? gst_caps_to_string(caps) : "unknown";
    if (caps) {
        gst_caps_unref(caps);
    }

    RtspPayloadFactories factories = rtspPayloadFactories(capsString);
    if (!factories.depay || !factories.parser) {
        std::cerr << "RTSP skipped unsupported RTP caps: " << capsString << std::endl;
        return;
    }
    if ((factories.isVideo && ctx->videoLinked) || (factories.isAudio && ctx->audioLinked)) {
        return;
    }
    if (!hasElementFactory(factories.depay) || !hasElementFactory(factories.parser)) {
        std::cerr << "missing RTSP payload elements: " << factories.depay
                  << " or " << factories.parser << std::endl;
        return;
    }

    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(ctx->mux));
    if (!pipeline) {
        return;
    }

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* depay = gst_element_factory_make(factories.depay, nullptr);
    GstElement* parser = gst_element_factory_make(factories.parser, nullptr);
    GstElement* capsfilter = makeCapsFilter(factories.muxCaps);
    if (!queue || !depay || !parser) {
        if (queue) gst_object_unref(queue);
        if (depay) gst_object_unref(depay);
        if (parser) gst_object_unref(parser);
        if (capsfilter) gst_object_unref(capsfilter);
        gst_object_unref(pipeline);
        return;
    }

    if (!gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), depay) ||
        !gst_bin_add(GST_BIN(pipeline), parser) ||
        (capsfilter && !gst_bin_add(GST_BIN(pipeline), capsfilter))) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (depay && !GST_OBJECT_PARENT(depay)) gst_object_unref(depay);
        if (parser && !GST_OBJECT_PARENT(parser)) gst_object_unref(parser);
        if (capsfilter && !GST_OBJECT_PARENT(capsfilter)) gst_object_unref(capsfilter);
        gst_object_unref(pipeline);
        return;
    }

    configureQueue(queue, 5000000000ULL);
    if (g_strcmp0(factories.parser, "h264parse") == 0 || g_strcmp0(factories.parser, "h265parse") == 0) {
        g_object_set(parser, "config-interval", 1, nullptr);
    }

    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parser);
    if (capsfilter) {
        gst_element_sync_state_with_parent(capsfilter);
    }

    const bool parserLinked = capsfilter
        ? gst_element_link_many(queue, depay, parser, capsfilter, nullptr)
        : gst_element_link_many(queue, depay, parser, nullptr);
    if (!parserLinked) {
        gst_object_unref(pipeline);
        return;
    }

    GstPad* queueSinkPad = gst_element_get_static_pad(queue, "sink");
    if (!queueSinkPad) {
        gst_object_unref(pipeline);
        return;
    }
    if (gst_pad_link(pad, queueSinkPad) != GST_PAD_LINK_OK) {
        gst_object_unref(queueSinkPad);
        gst_object_unref(pipeline);
        return;
    }
    gst_object_unref(queueSinkPad);

    const uint32_t requestedPid = factories.isVideo ? ctx->config.videoPid : ctx->config.audioPid;
    GstElement* muxSourceElement = capsfilter ? capsfilter : parser;
    GstPad* parserSrcPad = gst_element_get_static_pad(muxSourceElement, "src");
    GstPad* muxSinkPad = requestMuxSinkPad(ctx->mux, requestedPid);
    if (!parserSrcPad || !muxSinkPad) {
        if (parserSrcPad) gst_object_unref(parserSrcPad);
        if (muxSinkPad) gst_object_unref(muxSinkPad);
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(parserSrcPad, muxSinkPad) == GST_PAD_LINK_OK) {
        std::cerr << "RTSP remap linked " << (factories.isAudio ? "audio" : "video")
                  << " pid=" << requestedPid << std::endl;
        const gchar* padName = GST_PAD_NAME(muxSinkPad);
        if (factories.isVideo) {
            ctx->videoLinked = true;
            ctx->videoPadName = padName ? padName : "";
        }
        if (factories.isAudio) {
            ctx->audioLinked = true;
            ctx->audioPadName = padName ? padName : "";
        }
        updateMuxProgramMap(ctx);
    }

    gst_object_unref(parserSrcPad);
    gst_object_unref(muxSinkPad);
    gst_object_unref(pipeline);
}

void StreamManager::attachBitrateProbes(StreamState* state) {
    if (!state || !state->pipeline) {
        return;
    }

    gboolean inputAttached = FALSE;
    gboolean outputAttached = FALSE;

    // DVB uses input_selected_queue after tsdemux + SID selection so the
    // displayed input bitrate is the service bitrate, not the transponder.
    GstElement* inputQueue = gst_bin_get_by_name(GST_BIN(state->pipeline), "input_selected_queue");
    if (!inputQueue) {
        inputQueue = gst_bin_get_by_name(GST_BIN(state->pipeline), "input_queue");
    }
    if (inputQueue) {
        GstPad* sinkPad = gst_element_get_static_pad(inputQueue, "sink");
        if (sinkPad) {
            gst_pad_add_probe(
                sinkPad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                inputPadProbe,
                state,
                nullptr);
            gst_object_unref(sinkPad);
            inputAttached = TRUE;
        }
        gst_object_unref(inputQueue);
    }

    // For streams with multiple outputs, measure and validate the common
    // MPEG-TS only once before output_tee. Attaching the same continuity
    // tracker to every output_queue makes each packet appear repeatedly and
    // produces false output CC errors and an aggregate bitrate multiplied by
    // the number of branches.
    GstElement* outputTee = gst_bin_get_by_name(GST_BIN(state->pipeline), "output_tee");
    if (outputTee) {
        GstPad* sinkPad = gst_element_get_static_pad(outputTee, "sink");
        if (sinkPad) {
            gst_pad_add_probe(
                sinkPad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                outputPadProbe,
                state,
                nullptr);
            gst_object_unref(sinkPad);
            outputAttached = TRUE;
        }
        gst_object_unref(outputTee);
    }

    // A single-output pipeline has no output_tee, so retain the existing
    // output_queue probe in that case. Stop after the first queue to guarantee
    // that one logical TS stream is counted exactly once.
    if (!outputAttached) {
        GstIterator* outputIterator = gst_bin_iterate_elements(GST_BIN(state->pipeline));
        GValue outputItem = G_VALUE_INIT;
        while (gst_iterator_next(outputIterator, &outputItem) == GST_ITERATOR_OK) {
            GstElement* element = GST_ELEMENT(g_value_get_object(&outputItem));
            const gchar* name = GST_ELEMENT_NAME(element);
            if (name && g_str_has_prefix(name, "output_queue")) {
                GstPad* srcPad = gst_element_get_static_pad(element, "src");
                if (srcPad) {
                    gst_pad_add_probe(
                        srcPad,
                        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                        outputPadProbe,
                        state,
                        nullptr);
                    gst_object_unref(srcPad);
                    outputAttached = TRUE;
                }
            }
            g_value_unset(&outputItem);
            if (outputAttached) {
                break;
            }
        }
        gst_iterator_free(outputIterator);
    }

    GstIterator* iterator = gst_bin_iterate_elements(GST_BIN(state->pipeline));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
        GstElement* element = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory* factory = gst_element_get_factory(element);
        const gchar* factoryName = factory
            ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
            : nullptr;

        if (!inputAttached && factoryName &&
            (g_strcmp0(factoryName, "srtsrc") == 0 ||
             g_strcmp0(factoryName, "srtclientsrc") == 0 ||
             g_strcmp0(factoryName, "rtspsrc") == 0 ||
             g_strcmp0(factoryName, "rtmpsrc") == 0 ||
             g_strcmp0(factoryName, "souphttpsrc") == 0 ||
             g_strcmp0(factoryName, "udpsrc") == 0 ||
             g_strcmp0(factoryName, "filesrc") == 0)) {
            GstPad* srcPad = gst_element_get_static_pad(element, "src");
            if (srcPad) {
                gst_pad_add_probe(
                    srcPad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                    inputPadProbe,
                    state,
                    nullptr);
                gst_object_unref(srcPad);
                inputAttached = TRUE;
            }
        }

        if (!outputAttached && factoryName &&
            (g_strcmp0(factoryName, "udpsink") == 0 ||
             g_strcmp0(factoryName, "srtsink") == 0 ||
             g_strcmp0(factoryName, "rtmpsink") == 0 ||
             g_strcmp0(factoryName, "multifdsink") == 0 ||
             g_strcmp0(factoryName, "hlssink") == 0 ||
             g_strcmp0(factoryName, "appsink") == 0)) {
            GstPad* sinkPad = gst_element_get_static_pad(element, "sink");
            if (sinkPad) {
                gst_pad_add_probe(
                    sinkPad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                    outputPadProbe,
                    state,
                    nullptr);
                gst_object_unref(sinkPad);
                outputAttached = TRUE;
            }
        }

        g_value_unset(&item);
        if (inputAttached && outputAttached) {
            break;
        }
    }

    gst_iterator_free(iterator);
}

void StreamManager::updateBitrateEstimates(StreamState* state) {
    if (!state) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state->lastBitrateSample).count();
    if (elapsedMs < 1000) {
        return;
    }

    uint64_t currentInputBytes = state->inputBytes.load();
    uint64_t currentOutputBytes = state->outputBytes.load();
    uint64_t currentStableUdpNetworkBytes = state->stableUdpNetworkBytes.load();
    uint64_t currentInputCcErrors = state->inputCcErrors.load();
    uint64_t currentOutputCcErrors = state->outputCcErrors.load();
    uint64_t currentOutputTsPayloadPackets = state->outputTsPayloadPackets.load();
    uint64_t currentOutputTsScrambledPackets = state->outputTsScrambledPackets.load();
    uint64_t currentOutputTsClearPesStarts = state->outputTsClearPesStarts.load();
    uint64_t inputDelta = currentInputBytes - state->lastInputBytesSample;
    uint64_t outputDelta = currentOutputBytes - state->lastOutputBytesSample;
    uint64_t stableUdpNetworkDelta = currentStableUdpNetworkBytes - state->lastStableUdpNetworkBytesSample;
    uint64_t inputCcDelta = currentInputCcErrors - state->lastInputCcErrorsSample;
    uint64_t outputCcDelta = currentOutputCcErrors - state->lastOutputCcErrorsSample;
    uint64_t outputTsPayloadDelta = currentOutputTsPayloadPackets - state->lastOutputTsPayloadPacketsSample;
    uint64_t outputTsScrambledDelta = currentOutputTsScrambledPackets - state->lastOutputTsScrambledPacketsSample;
    uint64_t outputTsClearPesStartsDelta = currentOutputTsClearPesStarts - state->lastOutputTsClearPesStartsSample;
    double seconds = static_cast<double>(elapsedMs) / 1000.0;

    state->inputBitrate = static_cast<uint64_t>((inputDelta * 8) / seconds);
    const uint64_t measuredOutputBitrate = static_cast<uint64_t>((outputDelta * 8) / seconds);
    const uint64_t measuredStableUdpNetworkBitrate = static_cast<uint64_t>((stableUdpNetworkDelta * 8) / seconds);
    state->outputBitrate = udpCbrOutputEnabled(state->config) && state->config.targetBitrate > 0
        ? state->config.targetBitrate
        : (state->stableUdpNetworkTelemetry.load(std::memory_order_relaxed)
            ? measuredStableUdpNetworkBitrate
            : measuredOutputBitrate);
    state->inputCcErrorsDelta = inputCcDelta;
    state->outputCcErrorsDelta = outputCcDelta;
    state->outputTsPayloadPacketsDelta = outputTsPayloadDelta;
    state->outputTsScrambledPacketsDelta = outputTsScrambledDelta;
    state->outputTsClearPesStartsDelta = outputTsClearPesStartsDelta;

    state->lastInputBytesSample = currentInputBytes;
    state->lastOutputBytesSample = currentOutputBytes;
    state->lastStableUdpNetworkBytesSample = currentStableUdpNetworkBytes;
    state->lastInputCcErrorsSample = currentInputCcErrors;
    state->lastOutputCcErrorsSample = currentOutputCcErrors;
    state->lastOutputTsPayloadPacketsSample = currentOutputTsPayloadPackets;
    state->lastOutputTsScrambledPacketsSample = currentOutputTsScrambledPackets;
    state->lastOutputTsClearPesStartsSample = currentOutputTsClearPesStarts;
    state->lastBitrateSample = now;
}
GstPadProbeReturn StreamManager::inputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    (void)pad;
    auto* state = static_cast<StreamState*>(user_data);
    if (!state) {
        return GST_PAD_PROBE_OK;
    }

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer) {
            state->inputBytes.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
            updateInputContinuityErrors(state, buffer);
        }
    } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList* list = gst_pad_probe_info_get_buffer_list(info);
        state->inputBytes.fetch_add(bufferListSize(list), std::memory_order_relaxed);
        updateInputContinuityErrors(state, list);
    }

    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn StreamManager::outputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    (void)pad;
    auto* state = static_cast<StreamState*>(user_data);
    if (!state) {
        return GST_PAD_PROBE_OK;
    }

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer) {
            state->outputBytes.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
            updateOutputContinuityErrors(state, buffer);
            updateOutputScramblingStats(state, buffer);
        }
    } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList* list = gst_pad_probe_info_get_buffer_list(info);
        state->outputBytes.fetch_add(bufferListSize(list), std::memory_order_relaxed);
        updateOutputContinuityErrors(state, list);
        updateOutputScramblingStats(state, list);
    }

    return GST_PAD_PROBE_OK;
}

void StreamManager::monitorBus(const std::string& id) {
    auto found = streams.find(id);
    if (found == streams.end()) {
        return;
    }

    StreamState* state = found->second.get();
    GstBus* bus = state->bus;

    if (state->gstTranscoder && !state->pipeline) {
        auto lastSyntheticSample = std::chrono::steady_clock::now();

        while (state->running.load()) {
            const auto now = std::chrono::steady_clock::now();

            if (!state->gstTranscoder->isRunning()) {
                if (!state->usingBackup && !state->config.backupInputUri.empty()) {
                    notifyStreamState(
                        state->config,
                        "🟡",
                        telegramText(configManager, "Основной поток пропал", "Primary stream lost"),
                        telegramText(configManager, "Нет медиаданных 5 секунд", "No media data for 5 seconds") +
                            "\n" + telegramText(configManager, "Переключаюсь на резерв", "Switching to backup") +
                            "\nBackup: " + state->config.backupInputUri);

                    if (restartTranscodedInput(state, state->config.backupInputUri, true)) {
                        notifyStreamState(
                            state->config,
                            "🟠",
                            telegramText(configManager, "Работаю с резервного источника", "Running from backup source"),
                            telegramText(configManager, "Активный источник: резерв", "Active source: backup") +
                                "\nURL: " + state->activeInputUri);
                        lastSyntheticSample = std::chrono::steady_clock::now();
                        continue;
                    }
                }

                if (state->usingBackup &&
                    state->config.backupFileLoop &&
                    isBackupFileInput(state->config, state->activeInputUri)) {
                    const std::string loopFile = state->activeInputUri;
                    if (restartTranscodedInput(state, loopFile, true)) {
                        state->statusMessage = "running on backup file loop";
                        lastSyntheticSample = std::chrono::steady_clock::now();
                        continue;
                    }
                }

                state->statusMessage = "error: gstreamer transcoder exited";
                state->active = false;
                state->running = false;
                notifyStreamState(
                    state->config,
                    "🔴",
                    telegramText(configManager, "Ошибка GStreamer-транскодера", "GStreamer transcoder error"),
                    telegramText(configManager, "Процесс gst-launch завершился", "gst-launch process exited"));
                return;
            }

            if (state->usingBackup &&
                now - state->lastPrimaryRetry >= kPrimaryRetryInterval) {
                state->lastPrimaryRetry = now;
                const std::string primaryUri = state->primaryInputUri;
                if (!primaryUri.empty() &&
                    probeInputAvailable(state->config, primaryUri, kInputFailoverDelay)) {
                    notifyStreamState(
                        state->config,
                        "🟢",
                        telegramText(configManager, "Основной поток снова доступен", "Primary stream is available again"),
                        telegramText(configManager, "Переключаюсь на основной источник", "Switching to primary source") +
                            "\nURL: " + primaryUri);
                    if (restartTranscodedInput(state, primaryUri, false)) {
                        lastSyntheticSample = std::chrono::steady_clock::now();
                        continue;
                    }
                }
            }

            const auto elapsedMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - lastSyntheticSample).count();
            if (elapsedMs >= 1000) {
                const double seconds = static_cast<double>(elapsedMs) / 1000.0;
                const uint64_t inputEstimate = transcodeInputBitrateForStats(state->config);
                const uint64_t outputEstimate = transcodeMuxBitrateForStats(state->config);
                state->inputBitrate = inputEstimate;
                state->outputBitrate = outputEstimate;
                state->inputBytes.fetch_add(
                    static_cast<uint64_t>((inputEstimate * seconds) / 8.0),
                    std::memory_order_relaxed);
                state->outputBytes.fetch_add(
                    static_cast<uint64_t>((outputEstimate * seconds) / 8.0),
                    std::memory_order_relaxed);
                state->lastBitrateSample = now;
                state->statusMessage = state->usingBackup
                    ? "running on backup via gstreamer"
                    : "running via gstreamer";
                lastSyntheticSample = now;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        return;
    }

    if (!bus) {
        return;
    }

    while (state->running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (state->gstTranscoder) {
            if (!state->gstTranscoder->isRunning()) {
                if (!state->usingBackup &&
                    !state->config.backupInputUri.empty() &&
                    restartTranscodedInput(state, state->config.backupInputUri, true)) {
                    bus = state->bus;
                    notifyStreamState(
                        state->config,
                        "🟠",
                        telegramText(configManager, "Работаю с резервного источника", "Running from backup source"),
                        telegramText(configManager, "Активный источник: резерв", "Active source: backup") +
                            "\nURL: " + state->activeInputUri);
                    continue;
                }

                if (state->usingBackup &&
                    state->config.backupFileLoop &&
                    isBackupFileInput(state->config, state->activeInputUri) &&
                    restartTranscodedInput(state, state->activeInputUri, true)) {
                    bus = state->bus;
                    state->statusMessage = "running on backup file loop";
                    continue;
                }

                state->statusMessage = "error: gstreamer transcoder exited";
                state->active = false;
                state->running = false;
                notifyStreamState(
                    state->config,
                    "🔴",
                    telegramText(configManager, "Ошибка GStreamer-транскодера", "GStreamer transcoder error"),
                    telegramText(configManager, "Процесс gst-launch завершился", "gst-launch process exited"));
                return;
            }
            // A live gst-launch process is not proof of media activity.
            // SRT/UDP may remain connected with zero media. The external
            // watchdog is the real 5-second no-buffer detector.
        }
        const uint64_t currentInputBytes = state->inputBytes.load();
        if (currentInputBytes != state->lastInputBytesSeen) {
            state->lastInputBytesSeen = currentInputBytes;
            state->lastInputActivity = now;
            if (state->inputLossNotified && !state->usingBackup && !state->primaryRetryPending) {
                state->statusMessage = "running";
                notifyStreamState(
                    state->config,
                    "🟢",
                    telegramText(configManager, "Входной сигнал восстановлен", "Input signal restored"),
                    telegramText(configManager, "Активный источник: основной", "Active source: primary") + "\nURL: " + state->activeInputUri);
            }
            state->inputLossNotified = false;
            if (state->primaryRetryPending && !state->usingBackup) {
                state->primaryRetryPending = false;
                state->backupAttempted = false;
                state->statusMessage = "running on primary";
                notifyStreamState(
                    state->config,
                    "🟢",
                    telegramText(configManager, "Основной поток восстановлен", "Primary stream restored"),
                    telegramText(configManager, "Активный источник: основной", "Active source: primary") + "\nURL: " + state->activeInputUri);
            }
        }

        if (!state->config.testPattern) {
            const bool inputTimedOut = now - state->lastInputActivity >= kInputFailoverDelay;
            if (inputTimedOut && !state->usingBackup && !state->config.backupInputUri.empty()) {
                notifyStreamState(
                    state->config,
                    "🟡",
                    telegramText(configManager, "Основной поток пропал", "Primary stream lost"),
                    telegramText(configManager, "Нет входных данных 5 секунд", "No input data for 5 seconds") +
                        "\n" + telegramText(configManager, "Переключаюсь на резерв", "Switching to backup") +
                        "\nBackup: " + state->config.backupInputUri);
                if (restartActiveInput(state, state->config.backupInputUri, true)) {
                    bus = state->bus;
                    state->inputLossNotified = false;
                    notifyStreamState(
                        state->config,
                        "🟠",
                        telegramText(configManager, "Работаю с резервного источника", "Running from backup source"),
                        telegramText(configManager, "Активный источник: резерв", "Active source: backup") + "\nURL: " + state->activeInputUri);
                } else {
                    state->inputLossNotified = true;
                    notifyStreamState(
                        state->config,
                        "🔴",
                        telegramText(configManager, "Не удалось включить резерв", "Failed to start backup"),
                        telegramText(configManager, "Backup pipeline не стартовал", "Backup pipeline did not start") +
                            "\nBackup: " + state->config.backupInputUri);
                }
            } else if (state->usingBackup && now - state->lastPrimaryRetry >= kPrimaryRetryInterval) {
                const std::string primaryUri = state->primaryInputUri;
                state->lastPrimaryRetry = now;

                if (!primaryUri.empty()) {
                    // Probe the primary input with an independent temporary pipeline.
                    // The active backup-file pipeline keeps playing uninterrupted while
                    // availability is checked. Switch only after real media data arrives.
                    StreamConfig primaryProbeConfig = state->config;
                    std::string primaryProbeUri = primaryUri;
                    if (state->sharedDvbInput && !state->sharedDvbServiceRelayUri.empty()) {
                        primaryProbeConfig.inputUri = state->sharedDvbServiceRelayUri;
                        primaryProbeConfig.inputMode = "udp";
                        primaryProbeConfig.inputInterfaceAddress.clear();
                        primaryProbeConfig.inputInterfaceAddressConfigured = true;
                        primaryProbeConfig.inputServiceId = 0;
                        primaryProbeUri = state->sharedDvbServiceRelayUri;
                    }
                    if (probeInputAvailable(primaryProbeConfig, primaryProbeUri, kInputFailoverDelay)) {
                        notifyStreamState(
                            state->config,
                            "🟢",
                            telegramText(configManager, "Основной поток снова доступен", "Primary stream is available again"),
                            telegramText(configManager, "Переключаюсь с файла подмены на основной источник", "Switching from the replacement file to the primary source") +
                                "\nURL: " + primaryUri);
                        if (restartActiveInput(state, primaryUri, false)) {
                            bus = state->bus;
                            state->inputLossNotified = false;
                        }
                    }
                }
            } else if (inputTimedOut && !state->usingBackup && state->primaryRetryPending && !state->config.backupInputUri.empty()) {
                notifyStreamState(
                    state->config,
                    "🟡",
                    telegramText(configManager, "Основной пока недоступен", "Primary is still unavailable"),
                    telegramText(configManager, "Возвращаюсь на резервный источник", "Returning to backup source") +
                        "\nBackup: " + state->config.backupInputUri);
                if (restartActiveInput(state, state->config.backupInputUri, true)) {
                    bus = state->bus;
                    state->inputLossNotified = false;
                }
            } else if (inputTimedOut && !state->usingBackup && state->config.backupInputUri.empty() && !state->inputLossNotified) {
                state->inputLossNotified = true;
                const bool dvbStartupTimedOut =
                    DvbSatellite::isDvbUri(state->primaryInputUri) &&
                    state->lastInputBytesSeen == 0 &&
                    state->inputBytes.load(std::memory_order_relaxed) == 0;
                const bool isDvb = DvbSatellite::isDvbUri(state->activeInputUri);
                Json::Value dvbStats;
                bool dvbLocked = false;
                if (isDvb) {
                    dvbStats = DvbSatellite::signalFromUri(state->activeInputUri);
                    dvbLocked = dvbStats.get("locked", false).asBool();
                }
                if (dvbLocked) {
                    const auto sid = state->config.inputServiceId;
                    state->statusMessage = "DVB LOCK - no service data";
                    if (sid > 0) state->statusMessage += " (SID " + std::to_string(sid) + ")";
                    notifyStreamState(
                        state->config,
                        "🟠",
                        telegramText(configManager, "DVB сигнал есть, но нет данных сервиса", "DVB locked, but no service data"),
                        telegramText(configManager, "Frontend имеет LOCK, но выбранный сервис не передает данные", "Frontend is locked, but the selected service is not producing data") +
                            (sid > 0 ? "\nSID: " + std::to_string(sid) : "") +
                            "\nURL: " + state->activeInputUri);
                } else {
                    state->statusMessage = "no input signal";
                    notifyStreamState(
                        state->config,
                        "🔴",
                        telegramText(configManager, "Нет входного сигнала", "No input signal"),
                        telegramText(configManager, "Входных данных нет 5 секунд", "No input data for 5 seconds") +
                            "\n" + telegramText(configManager, "Резервная ссылка не задана", "Backup URL is not configured") +
                            "\nURL: " + state->activeInputUri);
                }
                if (dvbStartupTimedOut) {
                    if (dvbLocked) {
                        state->statusMessage = "error: DVB LOCK - no service data";
                        if (state->config.inputServiceId > 0) {
                            state->statusMessage += " (SID " + std::to_string(state->config.inputServiceId) + ")";
                        }
                    } else {
                        state->statusMessage = "error: DVB startup timed out without input signal";
                    }
                    state->active = false;
                    state->running = false;
                    if (state->pipeline) {
                        gst_element_set_state(state->pipeline, GST_STATE_NULL);
                    }
                    releaseSharedDvbInput(state);
                    CardManager::instance().releaseService(id);
                    return;
                }
            }
        }

        GstMessage* msg = gst_bus_timed_pop(bus, 500000000LL);
        if (!msg) {
            continue;
        }

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                std::string message = err ? err->message : "unknown";
                if (err) {
                    g_error_free(err);
                }
                g_free(dbg);

                state->statusMessage = "error: " + message;
                state->active = false;
                state->running = false;
                notifyStreamState(
                    state->config,
                    "🔴",
                    telegramText(configManager, "Ошибка потока", "Stream error"),
                    telegramText(configManager, "Причина", "Reason") + ": " + message);
                gst_message_unref(msg);
                return;
            }
            case GST_MESSAGE_EOS:
                if (state->usingBackup &&
                    state->config.backupFileLoop &&
                    isBackupFileInput(state->config, state->activeInputUri) &&
                    state->pipeline) {
                    // Recreate the file pipeline after EOS instead of seeking the completed
                    // pipeline. Demuxers such as qtdemux may accept a seek after EOS but stay
                    // drained, which leaves the output running with a black frame.
                    const std::string loopFile = state->activeInputUri;
                    gst_message_unref(msg);
                    if (restartActiveInput(state, loopFile, true)) {
                        bus = state->bus;
                        state->statusMessage = "running on backup file loop";
                        state->active = true;
                        continue;
                    }
                    std::cerr << "Failed to restart backup file loop for stream: " << id << std::endl;
                    state->statusMessage = "error: backup file loop restart failed";
                    state->active = false;
                    state->running = false;
                    notifyStreamState(
                        state->config,
                        "🔴",
                        telegramText(configManager, "Ошибка повтора файла подмены", "Replacement file loop failed"),
                        telegramText(configManager, "Не удалось запустить файл подмены с начала", "Failed to restart the replacement file from the beginning") +
                            "\nFile: " + loopFile);
                    return;
                }
                state->statusMessage = "ended";
                state->active = false;
                state->running = false;
                notifyStreamState(
                    state->config,
                    "⚫",
                    telegramText(configManager, "Поток завершился", "Stream ended"),
                    telegramText(configManager, "GStreamer получил EOS", "GStreamer received EOS"));
                gst_message_unref(msg);
                return;
            default:
                gst_message_unref(msg);
                break;
        }
    }
}
