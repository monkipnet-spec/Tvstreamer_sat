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

struct EcmDescriptor {
    uint16_t pid = kNullPid;
    uint16_t caid = 0;
    uint32_t provid = 0;
    bool operator<(const EcmDescriptor& other) const {
        if (pid != other.pid) return pid < other.pid;
        if (caid != other.caid) return caid < other.caid;
        return provid < other.provid;
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
    dvbcsa_key_t* key = nullptr;
    bool valid = false;
    uint64_t updates = 0;
    ControlWordSlot() { key = dvbcsa_key_alloc(); }
    ~ControlWordSlot() { if (key) dvbcsa_key_free(key); }
    ControlWordSlot(const ControlWordSlot&) = delete;
    ControlWordSlot& operator=(const ControlWordSlot&) = delete;
};

struct ServiceBinding {
    mutable std::mutex mutex;
    std::string clientKey;
    uint16_t serviceId = 0;
    uint16_t pmtPid = kNullPid;
    uint16_t defaultCaid = 0;
    uint32_t defaultProvid = 0;
    std::set<EcmDescriptor> ecmPids;
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
    uint64_t ecmRequests = 0;
    uint64_t cwUpdates = 0;

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
};

struct NewcamdInstance {
    std::mutex mutex;
    std::map<std::string, ReaderConfig> readersByClient;
    // v166: one independent Newcamd TCP session per active service.  This
    // prevents message ids, receiver state and ECM latency from one service
    // affecting any other service that uses the same OSCam reader/account.
    std::map<std::string, std::shared_ptr<NewcamdSession>> sessionsByStream;
    std::map<std::string, std::shared_ptr<ServiceBinding>> servicesByStream;
};

constexpr size_t kMaxSessionsPerReader = 10;

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
    const size_t queueDepth = session->ecmQueue.size();

    std::cerr << "NEWCAMD ECM SEND: client=" << clientKey
              << " msg=" << messageId
              << " stream=" << item.streamId
              << " sid=" << item.serviceId
              << " ecm_pid=" << item.ecmPid
              << " queue_depth=" << queueDepth
              << " queued_ms=" << (startedMs >= item.queuedMs ? startedMs - item.queuedMs : 0)
              << std::endl;
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
        if (!cw || !inst || messageId == 0) return;
        auto session = weakSession.lock();
        if (!session) return;

        uint16_t expectedMessageId = 0;
        uint64_t startedMs = 0;
        {
            std::lock_guard<std::mutex> queueLock(session->ecmMutex);
            if (!session->ecmInFlight) {
                std::cerr << "NEWCAMD CW DROP: client=" << clientKey
                          << " stream=" << streamId
                          << " msg=" << messageId
                          << " parity=" << ((parity & 1) ? "ODD" : "EVEN")
                          << " reason=no-inflight-ecm" << std::endl;
                return;
            }
            expectedMessageId = session->currentMessageId;
            startedMs = session->currentStartedMs;
        }

        if (messageId != expectedMessageId) {
            std::cerr << "NEWCAMD CW DROP: client=" << clientKey
                      << " stream=" << streamId
                      << " msg=" << messageId
                      << " expected_msg=" << expectedMessageId
                      << " parity=" << ((parity & 1) ? "ODD" : "EVEN")
                      << " reason=transaction-mismatch" << std::endl;
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
            {
                std::lock_guard<std::mutex> serviceLock(target->mutex);
                ControlWordSlot& slot = (parity & 1) ? target->odd : target->even;
                if (!slot.key) return;
                dvbcsa_key_set(cw, slot.key);
                slot.valid = true;
                ++slot.updates;
                ++target->cwUpdates;
                sid = target->serviceId;
            }
            const uint64_t now = monotonic_ms();
            std::cerr << "NEWCAMD CW MAP: client=" << clientKey
                      << " session=per-service"
                      << " stream=" << streamId
                      << " msg=" << messageId
                      << " sid=" << sid
                      << " parity=" << ((parity & 1) ? "ODD" : "EVEN")
                      << " latency_ms=" << (now >= startedMs ? now - startedMs : 0)
                      << std::endl;
        }

        // NewcamdClient emits EVEN then ODD for the same response. ODD completes
        // this service-local transaction and allows the next ECM from this same
        // service to be sent. No other service shares this TCP session.
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

    // Probe credentials/server once when the reader is opened. The probe is
    // immediately closed; actual persistent connections are created per service.
    NewcamdClient probe(readerConfig.host, readerConfig.port, readerConfig.user,
                        readerConfig.pass, readerConfig.des);
    if (!probe.connect() || !probe.login()) {
        const std::string detail = probe.last_error();
        write_error(error, error_size,
                    detail.empty() ? "Newcamd connect/login failed"
                                   : "Newcamd connect/login failed: " + detail);
        return TVS_CA_RESULT_ERROR;
    }
    probe.disconnect();

    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        inst->readersByClient[clientKey] = std::move(readerConfig);
    }
    std::cerr << "NEWCAMD READER READY: client=" << clientKey
              << " session_mode=per-service max_sessions=" << kMaxSessionsPerReader
              << std::endl;
    return TVS_CA_RESULT_OK;
}

static void newcamd_close_reader(void* instance, const char* reader_key) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !reader_key) return;

    const std::string clientKey = reader_key;
    std::vector<std::shared_ptr<NewcamdSession>> removedSessions;
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
        inst->readersByClient.erase(clientKey);
    }
    removedSessions.clear();
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
              << " mode=independent-tcp session=" << (activeForReader + 1)
              << "/" << kMaxSessionsPerReader << std::endl;
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

static int newcamd_process_ts(void* instance, const char* stream_id, uint8_t* data, size_t size, struct tvs_ca_ts_result_v1* result) {
    if (!instance || !stream_id || !data || !result) return TVS_CA_RESULT_PASSTHROUGH;
    auto* inst = static_cast<NewcamdInstance*>(instance);
    std::shared_ptr<ServiceBinding> bindingPtr;
    std::shared_ptr<NewcamdSession> session;
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
    }
    if (!session->connected.load() || !session->client) return TVS_CA_RESULT_PASSTHROUGH;
    std::lock_guard<std::mutex> serviceLock(bindingPtr->mutex);
    ServiceBinding& binding = *bindingPtr;

    bool changed = false;
    bool waitingForKey = false;
    bool sawEcm = false;

    ++binding.diagCalls;
    const size_t bufferRemainder = size % kTsPacketSize;
    if (bufferRemainder != 0) ++binding.diagUnalignedBuffers;

    for (size_t offset = 0; offset + kTsPacketSize <= size; offset += kTsPacketSize) {
        uint8_t* packet = data + offset;
        if (packet[0] != 0x47) {
            ++binding.diagBadSyncPackets;
            continue;
        }

        result->packets_seen++;
        const uint16_t pid = parse_pid(packet);
        const bool tei = (packet[1] & 0x80) != 0;
        const bool payloadStart = (packet[1] & 0x40) != 0;
        const uint8_t scramblingControl = static_cast<uint8_t>((packet[3] >> 6) & 0x03);
        const uint8_t adaptationControl = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
        const uint8_t continuityCounter = static_cast<uint8_t>(packet[3] & 0x0F);

        if (tei) ++binding.diagTeiPackets;
        
        switch (scramblingControl) {
            case 0: result->packets_clear++; break;
            default: result->packets_scrambled++; ++binding.diagScrambledPackets; break;
        }

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

            // Full-MPTS CA path: parse only PAT, the PAT-selected PMT and known
            // ECM PIDs.  Never feed video/audio/other-service payload into the
            // PSI section assembler.  Besides lowering CPU this prevents random
            // PES bytes from being mistaken for CA sections during MPTS warmup.
            const bool parsePsiPid = pid == 0x0000 || pid == binding.pmtPid || knownEcmPid ||
                (binding.serviceId == 0 && binding.pmtPid == kNullPid);
            if (parsePsiPid) {
                auto sections = binding.assemblers[pid].push(payload, payloadSize, payloadStart);
                for (const auto& section : sections) {
                    if (section.empty()) continue;
                    if (pid == 0x0000 && section[0] == 0x00) {
                        parse_pat_section(section, binding);
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

        // The full-MPTS prefilter hook sees scrambled packets from every service
        // on the transponder.  Only descramble elementary/PCR PIDs advertised by
        // the selected service PMT; unrelated services are left byte-for-byte
        // untouched and are dropped by the downstream SPTS filter.
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
        if (!slot.valid || !slot.key) {
            waitingForKey = true;
            continue;
        }
        
        dvbcsa_decrypt(slot.key, scrambledPayload, scrambledPayloadSize);
        packet[3] &= 0x3F;
        result->packets_changed++;
        changed = true;
    }

    const uint64_t diagNowMs = monotonic_ms();
    if (binding.diagLastLogMs == 0 || diagNowMs - binding.diagLastLogMs >= 1000) {
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
    std::vector<std::shared_ptr<ServiceBinding>> services;
    size_t readers = 0;
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        readers = inst->readersByClient.size();
        for (const auto& entry : inst->sessionsByStream) if (entry.second) sessions.push_back(entry.second);
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
