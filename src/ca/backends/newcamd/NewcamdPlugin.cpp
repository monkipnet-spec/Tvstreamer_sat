#include "../../CaBackendPluginApi.h"
#include "NewcamdClient.h"
extern "C" {
#include <dvbcsa/dvbcsa.h>
}
#include <jsoncpp/json/json.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr size_t kTsPacketSize = 188;
constexpr uint16_t kNullPid = 0x1FFF;

bool caDiagnosticsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TVS_CA_DIAGNOSTICS");
        if (!value || !*value) value = std::getenv("TVS_DVB_DIAGNOSTICS");
        return value && *value && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

struct EcmDescriptor {
    uint16_t pid = kNullPid;
    uint16_t caid = 0;
    uint32_t provid = 0;
    bool operator<(const EcmDescriptor& other) const {
        if (pid != other.pid) return pid < other.pid;
        if (caid != other.caid) return caid < other.caid;
        return provid < other.provid;
    }
    bool operator==(const EcmDescriptor& other) const {
        return pid == other.pid && caid == other.caid && provid == other.provid;
    }
};

struct SectionAssembler {
    std::vector<uint8_t> bytes;
    size_t expected = 0;
    std::vector<std::vector<uint8_t>> push(const uint8_t* payload, size_t size, bool payloadStart) {
        std::vector<std::vector<uint8_t>> sections;
        if (!payload || size == 0) return sections;
        size_t pos = 0;
        if (payloadStart) {
            const uint8_t pointer = payload[0];
            pos = 1;
            if (!bytes.empty() && pointer > 0 && pos + pointer <= size) {
                append(payload + pos, pointer, sections);
            }
            bytes.clear();
            expected = 0;
            pos = std::min(size, static_cast<size_t>(1) + pointer);
        }
        if (pos < size) append(payload + pos, size - pos, sections);
        return sections;
    }
private:
    void append(const uint8_t* data, size_t size, std::vector<std::vector<uint8_t>>& sections) {
        size_t pos = 0;
        while (pos < size) {
            if (bytes.empty() && data[pos] == 0xFF) break;
            const size_t needHeader = expected == 0 && bytes.size() < 3 ? 3 - bytes.size() : 0;
            const size_t chunk = needHeader ? std::min(needHeader, size - pos)
                : (expected ? std::min(expected - bytes.size(), size - pos) : size - pos);
            bytes.insert(bytes.end(), data + pos, data + pos + chunk);
            pos += chunk;
            if (expected == 0 && bytes.size() >= 3) {
                expected = 3 + (((bytes[1] & 0x0F) << 8) | bytes[2]);
                if (expected < 3 || expected > 4096) {
                    bytes.clear();
                    expected = 0;
                    break;
                }
            }
            if (expected > 0 && bytes.size() >= expected) {
                sections.push_back(bytes);
                bytes.clear();
                expected = 0;
            }
        }
    }
};

struct ControlWordSlot {
    dvbcsa_key_t* scalarKey = nullptr;
    struct dvbcsa_bs_key_s* bsKey = nullptr;
    bool valid = false;
    uint64_t updates = 0;
    ControlWordSlot() {
        scalarKey = dvbcsa_key_alloc();
        bsKey = dvbcsa_bs_key_alloc();
    }
    ~ControlWordSlot() {
        if (scalarKey) dvbcsa_key_free(scalarKey);
        if (bsKey) dvbcsa_bs_key_free(bsKey);
    }
    ControlWordSlot(const ControlWordSlot&) = delete;
    ControlWordSlot& operator=(const ControlWordSlot&) = delete;
};

struct DvbcsaBatchScratch {
    std::vector<struct dvbcsa_bs_batch_s> even;
    std::vector<struct dvbcsa_bs_batch_s> odd;
    size_t evenCount = 0;
    size_t oddCount = 0;

    DvbcsaBatchScratch() {
        const size_t capacity = std::max<size_t>(1, dvbcsa_bs_batch_size());
        // One extra sentinel entry is required by dvbcsa_bs_decrypt().
        even.resize(capacity + 1);
        odd.resize(capacity + 1);
    }

    size_t capacity() const { return even.empty() ? 0 : even.size() - 1; }
};

struct ServiceBinding {
    mutable std::mutex mutex;
    std::string clientKey;
    uint16_t serviceId = 0;
    uint16_t pmtPid = kNullPid;
    uint16_t defaultCaid = 0;
    uint32_t defaultProvid = 0;
    std::set<EcmDescriptor> ecmPids;
    std::set<EcmDescriptor> emmPids;
    bool catRoutingAnnounced = false;
    // Elementary/PCR PIDs belonging to this selected service.  In full-MPTS
    // mode the CA hook sees the transponder before software service filtering,
    // so never apply this service's CW to scrambled packets of other services.
    std::set<uint16_t> servicePayloadPids;
    bool pmtRoutingAnnounced = false;
    std::map<uint16_t, SectionAssembler> assemblers;
    std::set<std::string> sentEcms;
    std::set<uint16_t> pendingEcmIds;
    std::map<uint16_t, uint64_t> pendingEcmStartedMs;
    ControlWordSlot even;
    ControlWordSlot odd;
    DvbcsaBatchScratch csaBatch;
    std::string lastSuccessfulEcmSignature;
    uint64_t ecmRequests = 0;
    uint64_t cwUpdates = 0;

    // Irdeto ECMs on one service can rotate through several CHIDs.  OSCam/Astra
    // only keeps using the CHID that actually returns a valid CW.  Track that
    // selection per service and temporarily suppress rejected CHIDs so the
    // client can advance to the subscribed one instead of hammering the first
    // unsubscribed ECM forever.
    bool irdetoChidSelected = false;
    uint16_t selectedIrdetoChid = 0;
    std::map<uint16_t, uint64_t> rejectedIrdetoChidUntilMs;

    uint64_t diagLastLogMs = 0;
    uint64_t diagCalls = 0;
    uint64_t diagUnalignedBuffers = 0;
    uint64_t diagBadSyncPackets = 0;
    uint64_t diagTeiPackets = 0;
    uint64_t diagBadAdaptationPackets = 0;
    uint64_t diagPayloadPackets = 0;
    uint64_t diagNonModulo8Payloads = 0;
    uint64_t diagScrambledPackets = 0;
    uint64_t diagScrambledNonModulo8Payloads = 0;
    uint64_t diagBsFullChunks = 0;
    uint64_t diagBsTailChunks = 0;
    uint64_t diagScalarTailPackets = 0;
    uint16_t diagSamplePid = 0;
    uint8_t diagSampleScrambling = 0;
    uint8_t diagSampleAdaptation = 0;
    uint16_t diagSamplePayload = 0;
    uint8_t diagSampleCc = 0;
    bool diagSamplePusi = false;
    bool diagSampleTei = false;
};

struct EcmQueueItem {
    std::string streamId;
    uint16_t serviceId = 0;
    uint16_t caid = 0;
    uint32_t provid = 0;
    uint16_t ecmPid = 0;
    std::vector<uint8_t> section;
    std::string signature;
    uint64_t queuedMs = 0;
    bool isIrdeto = false;
    bool hasIrdetoChid = false;
    uint16_t irdetoChid = 0;
};

struct EmmQueueItem {
    std::string streamId;
    uint16_t serviceId = 0;
    EcmDescriptor descriptor;
    std::vector<uint8_t> section;
    std::string signature;
    uint64_t queuedMs = 0;
};

struct NewcamdSession {
    std::unique_ptr<NewcamdClient> client;
    std::atomic<bool> connected{false};
    std::string lastError;

    // Astra-style Newcamd scheduler: exactly one ECM transaction is in flight
    // on this TCP connection. Other services wait here. For a service that is
    // already queued, a newer ECM replaces the stale queued one.
    std::mutex ecmMutex;
    std::deque<EcmQueueItem> ecmQueue;
    bool ecmInFlight = false;
    uint16_t currentMessageId = 0;
    std::string currentStreamId;
    uint16_t currentEcmPid = 0;
    uint64_t currentStartedMs = 0;
    std::string currentSignature;
    bool currentIsIrdeto = false;
    bool currentHasIrdetoChid = false;
    uint16_t currentIrdetoChid = 0;

    ~NewcamdSession() {
        if (client) {
            client->set_key_update_callback({});
            client.reset();
        }
    }
};

struct ReaderConfig {
    std::string host = "127.0.0.1";
    int port = 15000;
    std::string user = "user";
    std::string pass = "pass";
    std::string des = "0102030405060708091011121314";
    uint16_t caid = 0;
    uint32_t provid = 0;
};

struct NewcamdAuSession {
    std::unique_ptr<NewcamdClient> client;
    std::atomic<bool> connected{false};
    std::mutex mutex;
    std::deque<EmmQueueItem> emmQueue;
    bool emmInFlight = false;
    uint16_t currentMessageId = 0;
    uint8_t currentTableId = 0;
    std::string currentSignature;
    std::string currentStreamId;
    uint64_t currentStartedMs = 0;
    std::map<std::string, uint64_t> recentEmms;
    uint64_t emmSent = 0;
    uint64_t emmAcked = 0;
    uint64_t emmDuplicateDrops = 0;
    uint64_t emmQueueDrops = 0;
    uint64_t emmFilterDrops = 0;
    uint64_t emmSendErrors = 0;

    ~NewcamdAuSession() {
        if (client) {
            client->set_emm_ack_callback({});
            client.reset();
        }
    }
};

struct NewcamdInstance {
    std::mutex mutex;
    std::map<std::string, ReaderConfig> readersByClient;
    // One dedicated AU/EMM connection per reader.  Service ECM connections
    // remain independent (v166) and never wait behind entitlement updates.
    std::map<std::string, std::shared_ptr<NewcamdAuSession>> auSessionsByClient;
    // v166: one independent Newcamd TCP session per active service.  This
    // prevents message ids, receiver state and ECM latency from one service
    // affecting any other service that uses the same OSCam reader/account.
    std::map<std::string, std::shared_ptr<NewcamdSession>> sessionsByStream;
    std::map<std::string, std::shared_ptr<ServiceBinding>> servicesByStream;
};

constexpr size_t kMaxSessionsPerReader = 10;
constexpr uint64_t kEmmDuplicateTtlMs = 30000;
constexpr size_t kMaxRecentEmms = 512;
constexpr size_t kMaxQueuedEmms = 64;

void write_error(char* error, size_t error_size, const std::string& message) {
    if (!error || error_size == 0) return;
    const size_t count = std::min(error_size - 1, message.size());
    std::memcpy(error, message.data(), count);
    error[count] = '\0';
}

void write_status(tvs_ca_ts_result_v1* result, const std::string& status) {
    if (!result || status.empty()) return;
    const size_t count = std::min(sizeof(result->status) - 1, status.size());
    std::memcpy(result->status, status.data(), count);
    result->status[count] = '\0';
}

Json::Value parse_config(const char* json, std::string& error) {
    Json::Value config;
    Json::CharReaderBuilder builder;
    std::istringstream input(json && *json ? json : "{}");
    if (!Json::parseFromStream(builder, input, &config, &error)) return Json::Value();
    return config;
}

uint16_t parse_pid(const uint8_t* packet) {
    return static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
}

uint64_t monotonic_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t parse_hex_value(const char* text, uint32_t maxValue) {
    if (!text || !*text) return 0;
    try {
        std::string value(text);
        if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) value.erase(0, 2);
        const unsigned long parsed = std::stoul(value, nullptr, 16);
        return parsed <= maxValue ? static_cast<uint32_t>(parsed) : 0;
    } catch (...) {
        return 0;
    }
}

bool is_irdeto_caid(uint16_t caid) {
    return ((caid & 0xFF00u) == 0x0600u) || caid == 0x1702u;
}

bool extract_irdeto_chid(const std::vector<uint8_t>& section, uint16_t& chid) {
    // Astra softcam/cas/irdeto.c reads CHID from ECM bytes 6..7.
    if (section.size() < 8 || (section[0] != 0x80 && section[0] != 0x81)) return false;
    chid = static_cast<uint16_t>((static_cast<uint16_t>(section[6]) << 8) | section[7]);
    return true;
}

const uint8_t* ts_payload(uint8_t* packet, size_t* payloadSize) {
    if (!packet || !payloadSize || packet[0] != 0x47) return nullptr;
    const uint8_t adaptationControl = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
    if (adaptationControl == 0 || adaptationControl == 2) return nullptr;
    size_t offset = 4;
    if (adaptationControl == 3) {
        const uint8_t adaptationLength = packet[4];
        offset += 1 + adaptationLength;
        if (offset >= kTsPacketSize) return nullptr;
    }
    *payloadSize = kTsPacketSize - offset;
    return packet + offset;
}

uint8_t* ts_payload_mut(uint8_t* packet, size_t* payloadSize) {
    return const_cast<uint8_t*>(ts_payload(packet, payloadSize));
}

uint32_t descriptor_provid(const uint8_t* data, size_t size) {
    if (size >= 3) return (static_cast<uint32_t>(data[0]) << 16) |
                         (static_cast<uint32_t>(data[1]) << 8) |
                         static_cast<uint32_t>(data[2]);
    return 0;
}

void collect_ca_descriptors(const uint8_t* data, size_t size, std::set<EcmDescriptor>& out) {
    size_t pos = 0;
    while (pos + 2 <= size) {
        const uint8_t tag = data[pos];
        const uint8_t len = data[pos + 1];
        pos += 2;
        if (pos + len > size) break;
        if (tag == 0x09 && len >= 4) {
            EcmDescriptor ecm;
            ecm.caid = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
            ecm.pid = static_cast<uint16_t>(((data[pos + 2] & 0x1F) << 8) | data[pos + 3]);
            ecm.provid = descriptor_provid(data + pos + 4, len - 4);
            if (ecm.pid > 0 && ecm.pid < kNullPid) out.insert(ecm);
        }
        pos += len;
    }
}

void keep_reader_caid(std::set<EcmDescriptor>& descriptors, uint16_t caid) {
    if (caid == 0) return;
    for (auto it = descriptors.begin(); it != descriptors.end();) {
        if (it->caid != caid) it = descriptors.erase(it);
        else ++it;
    }
}

void parse_cat_section(const std::vector<uint8_t>& section, ServiceBinding& binding) {
    if (section.size() < 12 || section[0] != 0x01) return;
    const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
    const size_t sectionTotal = 3 + sectionLength;
    if (sectionLength < 9 || sectionTotal > section.size()) return;
    const size_t descriptorsStart = 8;
    const size_t crcStart = sectionTotal - 4;
    if (descriptorsStart > crcStart) return;

    std::set<EcmDescriptor> newEmmPids;
    collect_ca_descriptors(section.data() + descriptorsStart,
                           crcStart - descriptorsStart, newEmmPids);
    keep_reader_caid(newEmmPids, binding.defaultCaid);
    if (newEmmPids.empty()) return;

    const bool changed = newEmmPids != binding.emmPids;
    binding.emmPids = std::move(newEmmPids);
    if (changed || !binding.catRoutingAnnounced) {
        std::cerr << "NEWCAMD CAT EMM SELECT: stream_service=" << binding.serviceId
                  << " reader_caid=0x" << std::hex << binding.defaultCaid << std::dec
                  << " emm_pids=" << binding.emmPids.size()
                  << " au=astra-irdeto-filter" << std::endl;
        binding.catRoutingAnnounced = true;
    }
}

void parse_pat_section(const std::vector<uint8_t>& section, ServiceBinding& binding) {
    if (section.size() < 12 || section[0] != 0x00 || binding.serviceId == 0) return;
    const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
    const size_t sectionTotal = 3 + sectionLength;
    if (sectionLength < 9 || sectionTotal > section.size()) return;
    const size_t entriesEnd = sectionTotal - 4;
    for (size_t pos = 8; pos + 4 <= entriesEnd; pos += 4) {
        const uint16_t program = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
        const uint16_t pmtPid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
        if (program == binding.serviceId && pmtPid > 0 && pmtPid < kNullPid) {
            if (binding.pmtPid != pmtPid) {
                binding.pmtPid = pmtPid;
                binding.ecmPids.clear();
                binding.servicePayloadPids.clear();
                binding.pmtRoutingAnnounced = false;
                // Drop stale PSI assemblers except PAT and the newly selected PMT.
                auto patIt = binding.assemblers.find(0);
                SectionAssembler patCopy;
                if (patIt != binding.assemblers.end()) patCopy = patIt->second;
                binding.assemblers.clear();
                binding.assemblers.emplace(0, std::move(patCopy));
            }
            return;
        }
    }
}

void parse_pmt_section(const std::vector<uint8_t>& section, ServiceBinding& binding) {
    if (section.size() < 12 || section[0] != 0x02) return;
    const uint16_t serviceId = static_cast<uint16_t>((section[3] << 8) | section[4]);
    if (binding.serviceId && serviceId != binding.serviceId) return;

    const size_t sectionLength = static_cast<size_t>(((section[1] & 0x0F) << 8) | section[2]);
    const size_t sectionTotal = 3 + sectionLength;
    if (sectionLength < 13 || sectionTotal > section.size()) return;

    binding.serviceId = serviceId;
    std::set<EcmDescriptor> newEcmPids;
    std::set<uint16_t> newPayloadPids;

    const uint16_t pcrPid = static_cast<uint16_t>(((section[8] & 0x1F) << 8) | section[9]);
    if (pcrPid > 0 && pcrPid < kNullPid) newPayloadPids.insert(pcrPid);

    const uint16_t programInfoLength = static_cast<uint16_t>(((section[10] & 0x0F) << 8) | section[11]);
    size_t pos = 12;
    const size_t crcStart = sectionTotal >= 4 ? sectionTotal - 4 : sectionTotal;
    if (pos + programInfoLength > crcStart) return;
    collect_ca_descriptors(section.data() + pos, programInfoLength, newEcmPids);
    pos += programInfoLength;

    while (pos + 5 <= crcStart) {
        const uint16_t elementaryPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
        const uint16_t esInfoLength = static_cast<uint16_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
        pos += 5;
        if (pos + esInfoLength > crcStart) break;
        if (elementaryPid > 0 && elementaryPid < kNullPid) newPayloadPids.insert(elementaryPid);
        collect_ca_descriptors(section.data() + pos, esInfoLength, newEcmPids);
        pos += esInfoLength;
    }

    // A PMT can advertise several CAIDs for the same service.  This reader
    // has a fixed CAID (e.g. Pervy1=0652), so do not send unrelated 0662 ECMs
    // to OSCam only to have them rejected on every rotation.
    keep_reader_caid(newEcmPids, binding.defaultCaid);
    binding.ecmPids = std::move(newEcmPids);
    binding.servicePayloadPids = std::move(newPayloadPids);
    if (!binding.pmtRoutingAnnounced) {
        std::cerr << "NEWCAMD PMT SELECT: stream_service=" << binding.serviceId
                  << " pmt_pid=" << binding.pmtPid
                  << " ecm_pids=" << binding.ecmPids.size()
                  << " payload_pids=" << binding.servicePayloadPids.size()
                  << " source=full-mpts-pat-selected" << std::endl;
        binding.pmtRoutingAnnounced = true;
    }
}

std::string ecm_signature(const std::vector<uint8_t>& section) {
    std::string key;
    key.reserve(section.size() * 2);
    static constexpr char hex[] = "0123456789abcdef";
    for (uint8_t b : section) {
        key.push_back(hex[b >> 4]);
        key.push_back(hex[b & 0x0F]);
    }
    return key;
}

EcmDescriptor descriptor_for_pid(const ServiceBinding& binding, uint16_t pid) {
    for (const auto& item : binding.ecmPids) {
        if (item.pid == pid) {
            EcmDescriptor result = item;
            if (result.caid == 0) result.caid = binding.defaultCaid;
            if (result.provid == 0) result.provid = binding.defaultProvid;
            return result;
        }
    }
    EcmDescriptor fallback;
    fallback.pid = pid;
    fallback.caid = binding.defaultCaid;
    fallback.provid = binding.defaultProvid;
    return fallback;
}

EcmDescriptor descriptor_for_emm_pid(const ServiceBinding& binding, uint16_t pid) {
    for (const auto& item : binding.emmPids) {
        if (item.pid == pid) {
            EcmDescriptor result = item;
            if (result.caid == 0) result.caid = binding.defaultCaid;
            if (result.provid == 0) result.provid = binding.defaultProvid;
            return result;
        }
    }
    EcmDescriptor fallback;
    fallback.pid = pid;
    fallback.caid = binding.defaultCaid;
    fallback.provid = binding.defaultProvid;
    return fallback;
}

void pump_ecm_queue(NewcamdInstance* inst, const std::string& clientKey,
                    const std::shared_ptr<NewcamdSession>& session) {
    if (!inst || !session || !session->client || !session->connected.load()) return;

    // Hold ecmMutex across send_ecm and ownership publication. If an extremely
    // fast reply reaches the receiver thread, its callback waits here until the
    // current transaction metadata is fully installed.
    std::unique_lock<std::mutex> queueLock(session->ecmMutex);
    if (session->ecmInFlight || session->ecmQueue.empty()) return;

    EcmQueueItem item = std::move(session->ecmQueue.front());
    session->ecmQueue.pop_front();

    uint16_t messageId = 0;
    const uint64_t startedMs = monotonic_ms();
    if (!session->client->send_ecm(item.serviceId, item.caid, item.provid, item.section, &messageId)) {
        session->lastError = session->client->last_error();
        std::cerr << "NEWCAMD ECM DROP: client=" << clientKey
                  << " stream=" << item.streamId
                  << " sid=" << item.serviceId
                  << " ecm_pid=" << item.ecmPid
                  << " reason=send-failed detail=" << session->lastError << std::endl;
        return;
    }

    session->ecmInFlight = true;
    session->currentMessageId = messageId;
    session->currentStreamId = item.streamId;
    session->currentEcmPid = item.ecmPid;
    session->currentStartedMs = startedMs;
    session->currentSignature = item.signature;
    session->currentIsIrdeto = item.isIrdeto;
    session->currentHasIrdetoChid = item.hasIrdetoChid;
    session->currentIrdetoChid = item.irdetoChid;
    const size_t queueDepth = session->ecmQueue.size();

    if (caDiagnosticsEnabled()) {
        std::cerr << "NEWCAMD ECM SEND: client=" << clientKey
                  << " msg=" << messageId
                  << " stream=" << item.streamId
                  << " sid=" << item.serviceId
                  << " ecm_pid=" << item.ecmPid
                  << " queue_depth=" << queueDepth
                  << " queued_ms=" << (startedMs >= item.queuedMs ? startedMs - item.queuedMs : 0);
        if (item.isIrdeto && item.hasIrdetoChid) {
            std::cerr << " irdeto_chid=0x" << std::hex << item.irdetoChid << std::dec;
        }
        std::cerr << std::endl;
    }
}

void enqueue_ecm(NewcamdInstance* inst, const std::string& clientKey,
                 const std::shared_ptr<NewcamdSession>& session, EcmQueueItem item) {
    if (!inst || !session || !session->client || !session->connected.load()) return;

    bool shouldPump = false;
    {
        std::lock_guard<std::mutex> queueLock(session->ecmMutex);

        // Repeated transmission of the currently active ECM is normal DVB SI
        // behaviour. Do not queue the same transaction again.
        if (session->ecmInFlight && session->currentStreamId == item.streamId &&
            session->currentSignature == item.signature) {
            return;
        }

        // Keep at most one not-yet-sent ECM per service. A newly observed ECM is
        // more useful than an older queued one for that same service.
        bool replaced = false;
        for (auto& queued : session->ecmQueue) {
            if (queued.streamId == item.streamId) {
                if (queued.signature == item.signature) return;
                queued = std::move(item);
                replaced = true;
                break;
            }
        }
        if (!replaced) session->ecmQueue.push_back(std::move(item));
        shouldPump = !session->ecmInFlight;
    }
    if (shouldPump) pump_ecm_queue(inst, clientKey, session);
}

void pump_emm_queue(const std::string& clientKey,
                    const std::shared_ptr<NewcamdAuSession>& auSession) {
    if (!auSession || !auSession->connected.load() || !auSession->client) return;

    while (true) {
        EmmQueueItem item;
        uint16_t messageId = 0;
        uint64_t sentCount = 0;
        std::string sendError;
        {
            // Hold the AU mutex across send_emm and ownership publication, just
            // like the ECM scheduler.  A localhost OSCam reply can be extremely
            // fast; its callback waits until currentMessageId is installed.
            std::unique_lock<std::mutex> lock(auSession->mutex);
            if (auSession->emmInFlight || auSession->emmQueue.empty()) return;
            item = std::move(auSession->emmQueue.front());
            auSession->emmQueue.pop_front();

            if (!auSession->client->send_emm(item.serviceId, item.descriptor.caid,
                                             item.descriptor.provid, item.section,
                                             &messageId)) {
                sendError = auSession->client->last_error();
                ++auSession->emmSendErrors;
                auSession->recentEmms.erase(item.signature);
            } else {
                auSession->emmInFlight = true;
                auSession->currentMessageId = messageId;
                auSession->currentTableId = item.section.empty() ? 0 : item.section[0];
                auSession->currentSignature = item.signature;
                auSession->currentStreamId = item.streamId;
                auSession->currentStartedMs = monotonic_ms();
                sentCount = ++auSession->emmSent;
            }
        }

        if (!sendError.empty()) {
            std::cerr << "NEWCAMD EMM DROP: client=" << clientKey
                      << " stream=" << item.streamId
                      << " pid=" << item.descriptor.pid
                      << " caid=0x" << std::hex << item.descriptor.caid << std::dec
                      << " type=0x" << std::hex
                      << static_cast<unsigned>(item.section.empty() ? 0 : item.section[0])
                      << std::dec
                      << " reason=send-failed detail=" << sendError << std::endl;
            // Try the next queued update; failed signatures are no longer in the
            // dedup cache and will also be eligible on the next broadcast copy.
            continue;
        }

        if (sentCount <= 20 || (sentCount % 100) == 0 || caDiagnosticsEnabled()) {
            std::cerr << "NEWCAMD EMM SEND: client=" << clientKey
                      << " stream=" << item.streamId
                      << " sid=" << item.serviceId
                      << " msg=" << messageId
                      << " pid=" << item.descriptor.pid
                      << " caid=0x" << std::hex << item.descriptor.caid << std::dec
                      << " provid=0x" << std::hex << item.descriptor.provid << std::dec
                      << " type=0x" << std::hex
                      << static_cast<unsigned>(item.section.empty() ? 0 : item.section[0])
                      << std::dec
                      << " bytes=" << item.section.size()
                      << " total=" << sentCount
                      << " au=dedicated-reader serial=one-in-flight" << std::endl;
        }
        return;
    }
}

bool forward_irdeto_emm(const std::string& clientKey, const std::string& streamId,
                        uint16_t serviceId, const EcmDescriptor& descriptor,
                        const std::vector<uint8_t>& section,
                        const std::shared_ptr<NewcamdAuSession>& auSession) {
    if (!auSession || !auSession->connected.load() || !auSession->client) return false;
    if (!auSession->client->au_enabled() || !auSession->client->matches_irdeto_emm(section)) {
        std::lock_guard<std::mutex> lock(auSession->mutex);
        ++auSession->emmFilterDrops;
        return false;
    }

    const uint64_t now = monotonic_ms();
    const std::string signature = ecm_signature(section);
    bool shouldPump = false;
    {
        std::lock_guard<std::mutex> lock(auSession->mutex);
        for (auto it = auSession->recentEmms.begin(); it != auSession->recentEmms.end();) {
            if (now >= it->second && now - it->second >= kEmmDuplicateTtlMs)
                it = auSession->recentEmms.erase(it);
            else
                ++it;
        }
        auto duplicate = auSession->recentEmms.find(signature);
        if (duplicate != auSession->recentEmms.end() &&
            now >= duplicate->second && now - duplicate->second < kEmmDuplicateTtlMs) {
            ++auSession->emmDuplicateDrops;
            return false;
        }
        if (auSession->recentEmms.size() >= kMaxRecentEmms) {
            auSession->recentEmms.erase(auSession->recentEmms.begin());
        }
        auSession->recentEmms[signature] = now;

        if (auSession->emmQueue.size() >= kMaxQueuedEmms) {
            // Do not ever block the TS/CSA hot path on a slow card.  Drop the
            // oldest queued (not in-flight) EMM and let its next broadcast copy
            // become eligible again.
            auSession->recentEmms.erase(auSession->emmQueue.front().signature);
            auSession->emmQueue.pop_front();
            ++auSession->emmQueueDrops;
        }

        EmmQueueItem item;
        item.streamId = streamId;
        item.serviceId = serviceId;
        item.descriptor = descriptor;
        item.section = section;
        item.signature = signature;
        item.queuedMs = now;
        auSession->emmQueue.push_back(std::move(item));
        shouldPump = !auSession->emmInFlight;
    }
    if (shouldPump) pump_emm_queue(clientKey, auSession);
    return true;
}

void configure_service_callback(NewcamdInstance* inst,
                                const std::string& clientKey,
                                const std::string& streamId,
                                const std::shared_ptr<ServiceBinding>& binding,
                                const std::shared_ptr<NewcamdSession>& session) {
    if (!session || !session->client) return;
    session->client->set_key_update_callback(
        [inst, clientKey, streamId,
         weakBinding = std::weak_ptr<ServiceBinding>(binding),
         weakSession = std::weak_ptr<NewcamdSession>(session)]
        (uint16_t messageId, uint8_t parity, const uint8_t* cw) {
        if (!inst || messageId == 0) return;
        auto session = weakSession.lock();
        if (!session) return;

        uint16_t expectedMessageId = 0;
        uint64_t startedMs = 0;
        bool currentIsIrdeto = false;
        bool currentHasIrdetoChid = false;
        uint16_t currentIrdetoChid = 0;
        std::string currentSignature;
        {
            std::lock_guard<std::mutex> queueLock(session->ecmMutex);
            if (!session->ecmInFlight) {
                std::cerr << "NEWCAMD CW DROP: client=" << clientKey
                          << " stream=" << streamId
                          << " msg=" << messageId
                          << " reason=no-inflight-ecm" << std::endl;
                return;
            }
            expectedMessageId = session->currentMessageId;
            startedMs = session->currentStartedMs;
            currentIsIrdeto = session->currentIsIrdeto;
            currentHasIrdetoChid = session->currentHasIrdetoChid;
            currentIrdetoChid = session->currentIrdetoChid;
            currentSignature = session->currentSignature;
        }

        if (messageId != expectedMessageId) {
            std::cerr << "NEWCAMD CW DROP: client=" << clientKey
                      << " stream=" << streamId
                      << " msg=" << messageId
                      << " expected_msg=" << expectedMessageId
                      << " reason=transaction-mismatch" << std::endl;
            return;
        }

        // NewcamdClient uses parity=0xFF with cw=nullptr for a valid ECM reply
        // that did not contain a CW (OSCam 'not found').  v175 left the session
        // permanently ecmInFlight in this case, so the first unsubscribed Irdeto
        // CHID blocked all later ECMs.  Complete the transaction and allow the
        // next CHID/ECM to be tried.
        if (!cw || parity == 0xFF) {
            {
                std::lock_guard<std::mutex> queueLock(session->ecmMutex);
                if (session->ecmInFlight && session->currentMessageId == messageId) {
                    session->ecmInFlight = false;
                    session->currentMessageId = 0;
                    session->currentStreamId.clear();
                    session->currentEcmPid = 0;
                    session->currentStartedMs = 0;
                    session->currentSignature.clear();
                    session->currentIsIrdeto = false;
                    session->currentHasIrdetoChid = false;
                    session->currentIrdetoChid = 0;
                }
            }

            auto target = weakBinding.lock();
            if (target && currentIsIrdeto && currentHasIrdetoChid) {
                std::lock_guard<std::mutex> serviceLock(target->mutex);
                const uint64_t now = monotonic_ms();
                target->rejectedIrdetoChidUntilMs[currentIrdetoChid] = now + 15000;
                if (target->irdetoChidSelected && target->selectedIrdetoChid == currentIrdetoChid) {
                    target->irdetoChidSelected = false;
                }
                std::cerr << "NEWCAMD IRDETO CHID REJECT: client=" << clientKey
                          << " stream=" << streamId
                          << " sid=" << target->serviceId
                          << " chid=0x" << std::hex << currentIrdetoChid << std::dec
                          << " retry_after_ms=15000" << std::endl;
            } else {
                std::cerr << "NEWCAMD ECM REJECT: client=" << clientKey
                          << " stream=" << streamId
                          << " msg=" << messageId
                          << " reason=no-control-word" << std::endl;
            }
            pump_ecm_queue(inst, clientKey, session);
            return;
        }

        auto target = weakBinding.lock();
        if (!target) {
            std::cerr << "NEWCAMD CW DROP: client=" << clientKey
                      << " stream=" << streamId
                      << " msg=" << messageId
                      << " parity=" << ((parity & 1) ? "ODD" : "EVEN")
                      << " reason=stream-gone" << std::endl;
        } else {
            uint16_t sid = 0;
            bool newlySelectedChid = false;
            {
                std::lock_guard<std::mutex> serviceLock(target->mutex);
                ControlWordSlot& slot = (parity & 1) ? target->odd : target->even;
                if (!slot.scalarKey || !slot.bsKey) return;
                dvbcsa_key_set(cw, slot.scalarKey);
                dvbcsa_bs_key_set(cw, slot.bsKey);
                slot.valid = true;
                ++slot.updates;
                ++target->cwUpdates;
                sid = target->serviceId;
                if ((parity & 1) && !currentSignature.empty()) {
                    target->lastSuccessfulEcmSignature = currentSignature;
                }

                if (currentIsIrdeto && currentHasIrdetoChid && !target->irdetoChidSelected) {
                    target->irdetoChidSelected = true;
                    target->selectedIrdetoChid = currentIrdetoChid;
                    target->rejectedIrdetoChidUntilMs.clear();
                    newlySelectedChid = true;
                }
            }
            const uint64_t now = monotonic_ms();
            if (newlySelectedChid) {
                std::cerr << "NEWCAMD IRDETO CHID SELECT: client=" << clientKey
                          << " stream=" << streamId
                          << " sid=" << sid
                          << " chid=0x" << std::hex << currentIrdetoChid << std::dec
                          << std::endl;
            }
            if (caDiagnosticsEnabled()) {
                std::cerr << "NEWCAMD CW MAP: client=" << clientKey
                          << " session=per-service"
                          << " stream=" << streamId
                          << " msg=" << messageId
                          << " sid=" << sid
                          << " parity=" << ((parity & 1) ? "ODD" : "EVEN")
                          << " latency_ms=" << (now >= startedMs ? now - startedMs : 0)
                          << std::endl;
            }
        }

        // NewcamdClient emits EVEN then ODD for the same successful response.
        // ODD completes the service-local transaction and allows the next ECM.
        if (parity & 1) {
            {
                std::lock_guard<std::mutex> queueLock(session->ecmMutex);
                if (session->ecmInFlight && session->currentMessageId == messageId) {
                    session->ecmInFlight = false;
                    session->currentMessageId = 0;
                    session->currentStreamId.clear();
                    session->currentEcmPid = 0;
                    session->currentStartedMs = 0;
                    session->currentSignature.clear();
                    session->currentIsIrdeto = false;
                    session->currentHasIrdetoChid = false;
                    session->currentIrdetoChid = 0;
                }
            }
            pump_ecm_queue(inst, clientKey, session);
        }
    });
}

} // namespace

extern "C" {

static void* newcamd_create(const struct tvs_ca_host_api_v1* host) {
    (void)host;
    return new NewcamdInstance();
}

static void newcamd_destroy(void* instance) {
    delete static_cast<NewcamdInstance*>(instance);
}

static int newcamd_open_reader(void* instance, const struct tvs_ca_reader_info_v1* reader, char* error, size_t error_size) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !reader || !reader->reader_key || !*reader->reader_key) {
        write_error(error, error_size, "invalid Newcamd CAM client instance");
        return TVS_CA_RESULT_ERROR;
    }

    const std::string clientKey = reader->reader_key;
    std::string parseError;
    Json::Value config = parse_config(reader->backend_config_json, parseError);
    if (!parseError.empty()) {
        write_error(error, error_size, "invalid Newcamd backend_config JSON: " + parseError);
        return TVS_CA_RESULT_ERROR;
    }

    ReaderConfig readerConfig;
    readerConfig.host = config.get("host", "127.0.0.1").asString();
    readerConfig.port = config.get("port", 15000).asInt();
    readerConfig.user = config.get("user", "user").asString();
    readerConfig.pass = config.get("pass", "pass").asString();
    readerConfig.des = config.get("des", "0102030405060708091011121314").asString();
    readerConfig.caid = static_cast<uint16_t>(parse_hex_value(reader->caid, 0xFFFFu));
    readerConfig.provid = parse_hex_value(reader->provider, 0xFFFFFFu);

    // v196: keep the reader probe alive as a dedicated AU/EMM Newcamd
    // connection.  Astra forwards addressed Irdeto EMMs over its CAM session;
    // the old TVStreammerSAT5 probe disconnected immediately and therefore the
    // card never received entitlement updates while our service sessions ran.
    auto auSession = std::make_shared<NewcamdAuSession>();
    auSession->client = std::make_unique<NewcamdClient>(
        readerConfig.host, readerConfig.port, readerConfig.user,
        readerConfig.pass, readerConfig.des);
    if (!auSession->client->connect() || !auSession->client->login()) {
        const std::string detail = auSession->client->last_error();
        write_error(error, error_size,
                    detail.empty() ? "Newcamd connect/login failed"
                                   : "Newcamd connect/login failed: " + detail);
        return TVS_CA_RESULT_ERROR;
    }

    const uint16_t cardCaid = auSession->client->card_caid();
    if (cardCaid != 0) readerConfig.caid = cardCaid;
    auSession->client->set_emm_ack_callback(
        [clientKey, weakAu = std::weak_ptr<NewcamdAuSession>(auSession)]
        (uint16_t messageId, uint8_t tableId) {
            auto au = weakAu.lock();
            if (!au) return;

            uint64_t acked = 0;
            uint64_t latencyMs = 0;
            std::string streamId;
            bool matched = false;
            {
                std::lock_guard<std::mutex> lock(au->mutex);
                if (au->emmInFlight && au->currentMessageId == messageId) {
                    const uint64_t now = monotonic_ms();
                    latencyMs = now >= au->currentStartedMs ? now - au->currentStartedMs : 0;
                    streamId = au->currentStreamId;
                    au->emmInFlight = false;
                    au->currentMessageId = 0;
                    au->currentTableId = 0;
                    au->currentSignature.clear();
                    au->currentStreamId.clear();
                    au->currentStartedMs = 0;
                    acked = ++au->emmAcked;
                    matched = true;
                }
            }
            if (!matched) {
                std::cerr << "NEWCAMD EMM ACK DROP: client=" << clientKey
                          << " msg=" << messageId
                          << " type=0x" << std::hex << static_cast<unsigned>(tableId) << std::dec
                          << " reason=transaction-mismatch" << std::endl;
                return;
            }
            if (acked <= 20 || (acked % 100) == 0 || caDiagnosticsEnabled()) {
                std::cerr << "NEWCAMD EMM ACK: client=" << clientKey
                          << " stream=" << streamId
                          << " msg=" << messageId
                          << " type=0x" << std::hex << static_cast<unsigned>(tableId) << std::dec
                          << " latency_ms=" << latencyMs
                          << " total=" << acked << std::endl;
            }
            pump_emm_queue(clientKey, au);
        });
    auSession->client->start_receiver();
    auSession->connected = true;

    std::shared_ptr<NewcamdAuSession> replacedAuSession;
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        auto old = inst->auSessionsByClient.find(clientKey);
        if (old != inst->auSessionsByClient.end()) replacedAuSession = old->second;
        inst->readersByClient[clientKey] = readerConfig;
        inst->auSessionsByClient[clientKey] = auSession;
    }
    replacedAuSession.reset();

    std::cerr << "NEWCAMD READER READY: client=" << clientKey
              << " session_mode=per-service max_sessions=" << kMaxSessionsPerReader
              << " reader_caid=0x" << std::hex << readerConfig.caid << std::dec
              << std::endl;
    std::cerr << "NEWCAMD AU SESSION: client=" << clientKey
              << " mode=dedicated-reader-tcp"
              << " au=" << (auSession->client->au_enabled() ? "enabled" : "disabled")
              << " card_caid=0x" << std::hex << auSession->client->card_caid() << std::dec
              << " providers=" << auSession->client->providers().size()
              << " emm_filter=astra-irdeto-address"
              << " emm_dedup_ttl_ms=" << kEmmDuplicateTtlMs
              << std::endl;
    if (!auSession->client->au_enabled()) {
        std::cerr << "NEWCAMD AU WARNING: client=" << clientKey
                  << " server reported AU disabled; ECM decoding remains available but EMM will not be sent"
                  << std::endl;
    }
    return TVS_CA_RESULT_OK;
}

static void newcamd_close_reader(void* instance, const char* reader_key) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !reader_key) return;

    const std::string clientKey = reader_key;
    std::vector<std::shared_ptr<NewcamdSession>> removedSessions;
    std::shared_ptr<NewcamdAuSession> removedAuSession;
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        for (auto it = inst->servicesByStream.begin(); it != inst->servicesByStream.end();) {
            if (it->second && it->second->clientKey == clientKey) {
                auto sit = inst->sessionsByStream.find(it->first);
                if (sit != inst->sessionsByStream.end()) {
                    removedSessions.push_back(sit->second);
                    inst->sessionsByStream.erase(sit);
                }
                it = inst->servicesByStream.erase(it);
            } else {
                ++it;
            }
        }
        auto auIt = inst->auSessionsByClient.find(clientKey);
        if (auIt != inst->auSessionsByClient.end()) {
            removedAuSession = auIt->second;
            inst->auSessionsByClient.erase(auIt);
        }
        inst->readersByClient.erase(clientKey);
    }
    removedSessions.clear();
    removedAuSession.reset();
}

static int newcamd_start_service(void* instance, const char* reader_key,
                                 const struct tvs_ca_service_info_v1* service,
                                 char* error, size_t error_size) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !reader_key || !service || !service->stream_id) {
        write_error(error, error_size, "invalid Newcamd service binding");
        return TVS_CA_RESULT_ERROR;
    }

    const std::string clientKey = reader_key;
    const std::string streamId = service->stream_id;
    ReaderConfig config;
    size_t activeForReader = 0;
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        auto readerIt = inst->readersByClient.find(clientKey);
        if (readerIt == inst->readersByClient.end()) {
            write_error(error, error_size, "Newcamd CAM reader is not configured");
            return TVS_CA_RESULT_ERROR;
        }
        config = readerIt->second;
        if (inst->sessionsByStream.count(streamId)) return TVS_CA_RESULT_OK;
        for (const auto& entry : inst->servicesByStream) {
            if (entry.second && entry.second->clientKey == clientKey) ++activeForReader;
        }
        if (activeForReader >= kMaxSessionsPerReader) {
            write_error(error, error_size, "Newcamd per-reader service session limit reached (10)");
            return TVS_CA_RESULT_ERROR;
        }
    }

    auto binding = std::make_shared<ServiceBinding>();
    binding->clientKey = clientKey;
    binding->serviceId = service->service_id <= 0xFFFF
        ? static_cast<uint16_t>(service->service_id) : 0;
    binding->defaultCaid = config.caid;
    binding->defaultProvid = config.provid;

    auto session = std::make_shared<NewcamdSession>();
    session->client = std::make_unique<NewcamdClient>(
        config.host, config.port, config.user, config.pass, config.des);
    configure_service_callback(inst, clientKey, streamId, binding, session);

    if (!session->client->connect() || !session->client->login()) {
        const std::string detail = session->client->last_error();
        write_error(error, error_size,
                    detail.empty() ? "Newcamd service session connect/login failed"
                                   : "Newcamd service session connect/login failed: " + detail);
        return TVS_CA_RESULT_ERROR;
    }
    session->client->start_receiver();
    session->connected = true;
    session->lastError = session->client->last_error();

    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        // Recheck the limit under the commit lock in case two services started
        // at the same time.
        activeForReader = 0;
        for (const auto& entry : inst->servicesByStream) {
            if (entry.second && entry.second->clientKey == clientKey) ++activeForReader;
        }
        if (activeForReader >= kMaxSessionsPerReader) {
            session->connected = false;
            session.reset();
            write_error(error, error_size, "Newcamd per-reader service session limit reached (10)");
            return TVS_CA_RESULT_ERROR;
        }
        inst->servicesByStream[streamId] = binding;
        inst->sessionsByStream[streamId] = session;
    }

    std::cerr << "NEWCAMD SERVICE SESSION: client=" << clientKey
              << " stream=" << streamId
              << " sid=" << binding->serviceId
              << " caid=0x" << std::hex << binding->defaultCaid << std::dec
              << " mode=independent-tcp session=" << (activeForReader + 1)
              << "/" << kMaxSessionsPerReader
              << " csa=native-bitslice-chunked"
              << " bs_batch_size=" << dvbcsa_bs_batch_size()
              << " full_native_batch=on"
              << " ecm_duplicate_suppression=on"
              << " ca_diagnostics=" << (caDiagnosticsEnabled() ? "on" : "off")
              << std::endl;
    return TVS_CA_RESULT_OK;
}

static void newcamd_stop_service(void* instance, const char* stream_id) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !stream_id) return;
    const std::string streamId = stream_id;
    std::shared_ptr<NewcamdSession> removedSession;
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        inst->servicesByStream.erase(streamId);
        auto sit = inst->sessionsByStream.find(streamId);
        if (sit != inst->sessionsByStream.end()) {
            removedSession = sit->second;
            inst->sessionsByStream.erase(sit);
        }
    }
    removedSession.reset();
}

static void decrypt_csa_batch_chunk(ControlWordSlot& slot,
                                    std::vector<struct dvbcsa_bs_batch_s>& batch,
                                    size_t& count,
                                    bool fullNativeChunk,
                                    ServiceBinding& binding,
                                    bool diagnostics) {
    if (!slot.valid || !slot.scalarKey || !slot.bsKey || count == 0) return;

    const size_t nativeBatch = batch.empty() ? 0 : (batch.size() - 1);
    if (nativeBatch == 0) return;
    const size_t bitsliceThreshold = std::min<size_t>(nativeBatch, 8);

    // dvbcsa_bs_decrypt() expects a null sentinel after the last valid lane.
    // DvbcsaBatchScratch reserves nativeBatch+1 entries specifically for it.
    batch[count].data = nullptr;
    batch[count].len = 0;

    if (count >= bitsliceThreshold) {
        dvbcsa_bs_decrypt(
            slot.bsKey, batch.data(), static_cast<unsigned int>(kTsPacketSize - 4));
        if (diagnostics) {
            if (fullNativeChunk && count == nativeBatch) ++binding.diagBsFullChunks;
            else ++binding.diagBsTailChunks;
        }
    } else {
        for (size_t i = 0; i < count; ++i) {
            if (batch[i].data && batch[i].len) {
                dvbcsa_decrypt(slot.scalarKey, batch[i].data, batch[i].len);
                if (diagnostics) ++binding.diagScalarTailPackets;
            }
        }
    }
    count = 0;
}

static int newcamd_process_ts(void* instance, const char* stream_id, uint8_t* data, size_t size, struct tvs_ca_ts_result_v1* result) {
    if (!instance || !stream_id || !data || !result) return TVS_CA_RESULT_PASSTHROUGH;
    auto* inst = static_cast<NewcamdInstance*>(instance);
    std::shared_ptr<ServiceBinding> bindingPtr;
    std::shared_ptr<NewcamdSession> session;
    std::shared_ptr<NewcamdAuSession> auSession;
    {
        // Resolve stable shared objects under the global lock, then release it.
        // Each service has its own lock so multiple channels can descramble and
        // submit ECMs concurrently without blocking CW delivery to other streams.
        std::lock_guard<std::mutex> instanceLock(inst->mutex);
        auto serviceIt = inst->servicesByStream.find(stream_id);
        if (serviceIt == inst->servicesByStream.end() || !serviceIt->second) return TVS_CA_RESULT_PASSTHROUGH;
        bindingPtr = serviceIt->second;
        auto sessionIt = inst->sessionsByStream.find(stream_id);
        if (sessionIt == inst->sessionsByStream.end() || !sessionIt->second) return TVS_CA_RESULT_PASSTHROUGH;
        session = sessionIt->second;
        auto auIt = inst->auSessionsByClient.find(bindingPtr->clientKey);
        if (auIt != inst->auSessionsByClient.end()) auSession = auIt->second;
    }
    if (!session->connected.load() || !session->client) return TVS_CA_RESULT_PASSTHROUGH;
    std::lock_guard<std::mutex> serviceLock(bindingPtr->mutex);
    ServiceBinding& binding = *bindingPtr;

    bool changed = false;
    bool waitingForKey = false;
    bool sawEcm = false;
    const bool diagnostics = caDiagnosticsEnabled();

    if (diagnostics) ++binding.diagCalls;
    const size_t bufferRemainder = size % kTsPacketSize;
    if (diagnostics && bufferRemainder != 0) ++binding.diagUnalignedBuffers;

    for (size_t offset = 0; offset + kTsPacketSize <= size; offset += kTsPacketSize) {
        uint8_t* packet = data + offset;
        if (packet[0] != 0x47) {
            if (diagnostics) ++binding.diagBadSyncPackets;
            continue;
        }

        result->packets_seen++;
        const uint16_t pid = parse_pid(packet);
        const bool payloadStart = (packet[1] & 0x40) != 0;
        const uint8_t scramblingControl = static_cast<uint8_t>((packet[3] >> 6) & 0x03);

        switch (scramblingControl) {
            case 0: result->packets_clear++; break;
            default: result->packets_scrambled++; break;
        }

        if (diagnostics) {
            const bool tei = (packet[1] & 0x80) != 0;
            const uint8_t adaptationControl = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
            const uint8_t continuityCounter = static_cast<uint8_t>(packet[3] & 0x0F);
            if (tei) ++binding.diagTeiPackets;
            if (scramblingControl != 0) ++binding.diagScrambledPackets;

            size_t diagnosticPayloadSize = 0;
            bool validAdaptation = true;
            if (adaptationControl == 0) {
                validAdaptation = false;
            } else if (adaptationControl == 1) {
                diagnosticPayloadSize = kTsPacketSize - 4;
            } else if (adaptationControl == 3) {
                const size_t adaptationLength = packet[4];
                if (5 + adaptationLength > kTsPacketSize) {
                    validAdaptation = false;
                } else {
                    const size_t diagnosticPayloadOffset = 5 + adaptationLength;
                    if (diagnosticPayloadOffset < kTsPacketSize) {
                        diagnosticPayloadSize = kTsPacketSize - diagnosticPayloadOffset;
                    }
                }
            }

            if (!validAdaptation) {
                ++binding.diagBadAdaptationPackets;
            } else if (diagnosticPayloadSize > 0) {
                ++binding.diagPayloadPackets;
                if ((diagnosticPayloadSize % 8) != 0) {
                    ++binding.diagNonModulo8Payloads;
                    if (scramblingControl == 2 || scramblingControl == 3) {
                        ++binding.diagScrambledNonModulo8Payloads;
                        binding.diagSamplePid = pid;
                        binding.diagSampleScrambling = scramblingControl;
                        binding.diagSampleAdaptation = adaptationControl;
                        binding.diagSamplePayload = static_cast<uint16_t>(diagnosticPayloadSize);
                        binding.diagSampleCc = continuityCounter;
                        binding.diagSamplePusi = payloadStart;
                        binding.diagSampleTei = tei;
                    }
                }
            }
        }

        size_t payloadSize = 0;
        const uint8_t* payload = ts_payload(packet, &payloadSize);

        if (payload && payloadSize > 0) {
            bool knownEcmPid = false;
            for (const auto& ecm : binding.ecmPids) {
                if (ecm.pid == pid) {
                    knownEcmPid = true;
                    break;
                }
            }
            bool knownEmmPid = false;
            for (const auto& emm : binding.emmPids) {
                if (emm.pid == pid) {
                    knownEmmPid = true;
                    break;
                }
            }

            // v196 adds CAT (PID 1) and the CAT-selected EMM PIDs to the same
            // low-volume PSI path. Video/audio packets still never enter the
            // section assembler, preserving the v180 CA hot path.
            const bool parsePsiPid = pid == 0x0000 || pid == 0x0001 ||
                pid == binding.pmtPid || knownEcmPid || knownEmmPid ||
                (binding.serviceId == 0 && binding.pmtPid == kNullPid);
            if (parsePsiPid) {
                auto sections = binding.assemblers[pid].push(payload, payloadSize, payloadStart);
                for (const auto& section : sections) {
                    if (section.empty()) continue;
                    if (pid == 0x0000 && section[0] == 0x00) {
                        parse_pat_section(section, binding);
                    } else if (pid == 0x0001 && section[0] == 0x01) {
                        parse_cat_section(section, binding);
                    } else if (knownEmmPid && section[0] >= 0x82 && section[0] <= 0x8F) {
                        const EcmDescriptor emm = descriptor_for_emm_pid(binding, pid);
                        if (is_irdeto_caid(emm.caid)) {
                            (void)forward_irdeto_emm(binding.clientKey, stream_id,
                                                     binding.serviceId, emm, section, auSession);
                        }
                    } else if (section[0] == 0x02) {
                        const uint16_t sectionServiceId = section.size() >= 5
                            ? static_cast<uint16_t>((section[3] << 8) | section[4]) : 0;
                        if (!binding.serviceId || sectionServiceId == binding.serviceId) {
                            binding.pmtPid = pid;
                            parse_pmt_section(section, binding);
                        }
                    } else if ((section[0] == 0x80 || section[0] == 0x81) && knownEcmPid) {
                        sawEcm = true;
                        const std::string signature = ecm_signature(section);
                        // Repeated copies of an ECM section are normal. Once that exact
                        // ECM has already produced a valid CW there is no reason to send
                        // it to OSCam again; wait for a genuinely new ECM signature.
                        if (!binding.lastSuccessfulEcmSignature.empty() &&
                            signature == binding.lastSuccessfulEcmSignature) {
                            continue;
                        }
                        const EcmDescriptor ecm = descriptor_for_pid(binding, pid);
                        EcmQueueItem item;
                        item.streamId = stream_id;
                        item.serviceId = binding.serviceId;
                        item.caid = ecm.caid;
                        item.provid = ecm.provid;
                        item.ecmPid = ecm.pid;
                        item.section = section;
                        item.signature = signature;
                        item.queuedMs = monotonic_ms();
                        item.isIrdeto = is_irdeto_caid(item.caid);
                        if (item.isIrdeto) {
                            item.hasIrdetoChid = extract_irdeto_chid(section, item.irdetoChid);
                            if (item.hasIrdetoChid) {
                                if (binding.irdetoChidSelected &&
                                    binding.selectedIrdetoChid != item.irdetoChid) {
                                    continue;
                                }
                                const auto rejected = binding.rejectedIrdetoChidUntilMs.find(item.irdetoChid);
                                if (!binding.irdetoChidSelected && rejected != binding.rejectedIrdetoChidUntilMs.end()) {
                                    if (item.queuedMs < rejected->second) continue;
                                    binding.rejectedIrdetoChidUntilMs.erase(rejected);
                                }
                            }
                        }
                        ++binding.ecmRequests;
                        enqueue_ecm(inst, binding.clientKey, session, std::move(item));
                    }
                }
            }
        }

        if (scramblingControl == 0 || scramblingControl == 1) {
            if (scramblingControl == 1) waitingForKey = true;
            continue;
        }

        // The v177+ CA hook receives the selected SPTS. Keep the PMT-derived
        // payload guard as an additional safety check so only elementary/PCR PIDs
        // belonging to this service can ever be descrambled with its CW.
        if (!binding.servicePayloadPids.empty() &&
            binding.servicePayloadPids.count(pid) == 0) {
            continue;
        }

        size_t scrambledPayloadSize = 0;
        uint8_t* scrambledPayload = ts_payload_mut(packet, &scrambledPayloadSize);
        if (!scrambledPayload || scrambledPayloadSize == 0) {
            waitingForKey = true;
            continue;
        }

        ControlWordSlot& slot = scramblingControl == 3 ? binding.odd : binding.even;
        if (!slot.valid || !slot.scalarKey || !slot.bsKey) {
            waitingForKey = true;
            continue;
        }

        auto& batch = scramblingControl == 3 ? binding.csaBatch.odd : binding.csaBatch.even;
        size_t& batchCount = scramblingControl == 3 ? binding.csaBatch.oddCount : binding.csaBatch.evenCount;
        const size_t batchCapacity = binding.csaBatch.capacity();
        if (batchCapacity == 0) {
            waitingForKey = true;
            continue;
        }

        // Persistent scratch only: pointers reference the caller-owned TS buffer
        // and remain valid until process_ts returns. No allocations occur here.
        // v199 immediately decrypts a *full native* bitslice chunk, resets the
        // scratch counter, then keeps scanning the same caller buffer.  This is
        // safe for arbitrary process_ts sizes and eliminates v198's overflow
        // guard that could leave packets untouched once one parity hit capacity.
        batch[batchCount].data = scrambledPayload;
        batch[batchCount].len = static_cast<unsigned int>(scrambledPayloadSize);
        ++batchCount;

        packet[3] &= 0x3F;
        result->packets_changed++;
        changed = true;

        if (batchCount == batchCapacity) {
            decrypt_csa_batch_chunk(
                slot, batch, batchCount, true, binding, diagnostics);
        }
    }

    // Flush only the residual parity tails here. Full native chunks were
    // already decrypted in-place while scanning the wide CA input batch.
    decrypt_csa_batch_chunk(
        binding.even, binding.csaBatch.even, binding.csaBatch.evenCount,
        false, binding, diagnostics);
    decrypt_csa_batch_chunk(
        binding.odd, binding.csaBatch.odd, binding.csaBatch.oddCount,
        false, binding, diagnostics);

    const uint64_t diagNowMs = diagnostics ? monotonic_ms() : 0;
    if (diagnostics && (binding.diagLastLogMs == 0 || diagNowMs - binding.diagLastLogMs >= 1000)) {
        binding.diagLastLogMs = diagNowMs;
        std::cerr
            << "CA TS DIAG: stream=" << stream_id
            << " calls=" << binding.diagCalls
            << " buffer_size=" << size
            << " remainder=" << bufferRemainder
            << " unaligned_buffers=" << binding.diagUnalignedBuffers
            << " bad_sync=" << binding.diagBadSyncPackets
            << " tei=" << binding.diagTeiPackets
            << " bad_afc=" << binding.diagBadAdaptationPackets
            << " payload_packets=" << binding.diagPayloadPackets
            << " nonmod8=" << binding.diagNonModulo8Payloads
            << " scrambled=" << binding.diagScrambledPackets
            << " scrambled_nonmod8=" << binding.diagScrambledNonModulo8Payloads
            << " bs_native=" << dvbcsa_bs_batch_size()
            << " bs_full_chunks=" << binding.diagBsFullChunks
            << " bs_tail_chunks=" << binding.diagBsTailChunks
            << " scalar_tail_packets=" << binding.diagScalarTailPackets
            << " sample_pid=" << binding.diagSamplePid
            << " sample_sc=" << static_cast<unsigned>(binding.diagSampleScrambling)
            << " sample_afc=" << static_cast<unsigned>(binding.diagSampleAdaptation)
            << " sample_payload=" << binding.diagSamplePayload
            << " sample_mod8=" << (binding.diagSamplePayload % 8)
            << " sample_cc=" << static_cast<unsigned>(binding.diagSampleCc)
            << " sample_pusi=" << (binding.diagSamplePusi ? 1 : 0)
            << " sample_tei=" << (binding.diagSampleTei ? 1 : 0)
            << std::endl;
    }

    if (changed) {
        write_status(result, "BACKEND_ACTIVE");
        return TVS_CA_RESULT_OK;
    }
    if (waitingForKey) {
        const std::string detail = session->lastError.empty() ? "WAITING_FOR_CW" : "WAITING_FOR_CW: " + session->lastError;
        write_status(result, detail);
        return TVS_CA_RESULT_RETRY;
    }
    if (sawEcm) {
        write_status(result, "ECM_SEEN_NO_CW");
        return TVS_CA_RESULT_RETRY;
    }
    write_status(result, "NO_SCRAMBLED_PAYLOAD");
    return TVS_CA_RESULT_PASSTHROUGH;
}

static const char* newcamd_status_json(void* instance) {
    static thread_local std::string status;
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst) {
        status = "{\"status\":\"disconnected\",\"clients\":0}";
        return status.c_str();
    }

    std::vector<std::shared_ptr<NewcamdSession>> sessions;
    std::vector<std::shared_ptr<NewcamdAuSession>> auSessions;
    std::vector<std::shared_ptr<ServiceBinding>> services;
    size_t readers = 0;
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        readers = inst->readersByClient.size();
        for (const auto& entry : inst->sessionsByStream) if (entry.second) sessions.push_back(entry.second);
        for (const auto& entry : inst->auSessionsByClient) if (entry.second) auSessions.push_back(entry.second);
        for (const auto& entry : inst->servicesByStream) if (entry.second) services.push_back(entry.second);
    }

    size_t connected = 0;
    size_t authenticated = 0;
    size_t evenKeys = 0;
    size_t oddKeys = 0;
    size_t pendingEcms = 0;
    for (const auto& session : sessions) {
        if (!session || !session->connected.load()) continue;
        ++connected;
        if (session->client && session->client->authenticated()) ++authenticated;
        std::lock_guard<std::mutex> queueLock(session->ecmMutex);
        pendingEcms += session->ecmQueue.size() + (session->ecmInFlight ? 1 : 0);
    }
    size_t auConnected = 0;
    size_t auEnabled = 0;
    uint64_t emmSent = 0;
    uint64_t emmAcked = 0;
    uint64_t emmQueued = 0;
    uint64_t emmDuplicateDrops = 0;
    uint64_t emmQueueDrops = 0;
    uint64_t emmFilterDrops = 0;
    uint64_t emmSendErrors = 0;
    for (const auto& au : auSessions) {
        if (!au) continue;
        if (au->connected.load()) ++auConnected;
        if (au->client && au->client->au_enabled()) ++auEnabled;
        std::lock_guard<std::mutex> auLock(au->mutex);
        emmSent += au->emmSent;
        emmAcked += au->emmAcked;
        emmQueued += au->emmQueue.size() + (au->emmInFlight ? 1 : 0);
        emmDuplicateDrops += au->emmDuplicateDrops;
        emmQueueDrops += au->emmQueueDrops;
        emmFilterDrops += au->emmFilterDrops;
        emmSendErrors += au->emmSendErrors;
    }
    for (const auto& binding : services) {
        std::lock_guard<std::mutex> serviceLock(binding->mutex);
        if (binding->even.valid) ++evenKeys;
        if (binding->odd.valid) ++oddKeys;
    }
    const std::string state = connected ? "connected" : (readers ? "ready" : "disconnected");
    status = "{\"status\":\"" + state +
             "\",\"clients\":" + std::to_string(connected) +
             ",\"readers\":" + std::to_string(readers) +
             ",\"service_sessions\":" + std::to_string(connected) +
             ",\"max_sessions_per_reader\":" + std::to_string(kMaxSessionsPerReader) +
             ",\"session_mode\":\"per-service-tcp\"" +
             ",\"au_sessions\":" + std::to_string(auConnected) +
             ",\"au_enabled\":" + std::to_string(auEnabled) +
             ",\"emm_sent\":" + std::to_string(emmSent) +
             ",\"emm_acked\":" + std::to_string(emmAcked) +
             ",\"emm_queued\":" + std::to_string(emmQueued) +
             ",\"emm_duplicate_drops\":" + std::to_string(emmDuplicateDrops) +
             ",\"emm_queue_drops\":" + std::to_string(emmQueueDrops) +
             ",\"emm_filter_drops\":" + std::to_string(emmFilterDrops) +
             ",\"emm_send_errors\":" + std::to_string(emmSendErrors) +
             ",\"authenticated_clients\":" + std::to_string(authenticated) +
             ",\"streams\":" + std::to_string(services.size()) +
             ",\"pending_ecms\":" + std::to_string(pendingEcms) +
             ",\"even_keys\":" + std::to_string(evenKeys) +
             ",\"odd_keys\":" + std::to_string(oddKeys) +
             ",\"newcamd_authenticated\":" + std::string(authenticated ? "true" : "false") +
             "}";
    return status.c_str();
}

static const tvs_ca_backend_api_v1 api = {
    TVS_CA_BACKEND_ABI_V1,
    "newcamd",
    "Newcamd OSCAM Client",
    "Monk",
    TVS_CA_CAP_TS_INPLACE | TVS_CA_CAP_MULTI_SERVICE,
    newcamd_create,
    newcamd_destroy,
    newcamd_open_reader,
    newcamd_close_reader,
    newcamd_start_service,
    newcamd_stop_service,
    newcamd_process_ts,
    newcamd_status_json
};

const tvs_ca_backend_api_v1* tvstreammersat5_ca_backend_get_api_v1(void) {
    return &api;
}

}
