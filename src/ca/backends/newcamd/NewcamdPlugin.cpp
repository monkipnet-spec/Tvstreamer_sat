#include "../../CaBackendPluginApi.h"
#include "NewcamdClient.h"

extern "C" {
#include <dvbcsa/dvbcsa.h>
}
#include <jsoncpp/json/json.h>

#include <algorithm>
#include <chrono>
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
    std::string clientKey;
    uint16_t serviceId = 0;
    uint16_t pmtPid = kNullPid;
    uint16_t defaultCaid = 0;
    uint32_t defaultProvid = 0;
    std::set<EcmDescriptor> ecmPids;
    std::map<uint16_t, SectionAssembler> assemblers;
    std::set<std::string> sentEcms;
    std::set<uint16_t> pendingEcmIds;
    ControlWordSlot even;
    ControlWordSlot odd;
    uint64_t ecmRequests = 0;
    uint64_t cwUpdates = 0;
    uint64_t generation = 0;

    // Diagnostic-only MPEG-TS framing counters. These counters never modify
    // TS payload or CA state; they are used to correlate PES corruption with
    // input buffer alignment, TS sync/adaptation errors and payload lengths.
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

struct NewcamdSession {
    std::unique_ptr<NewcamdClient> client;
    std::mutex ioMutex;
    bool connected = false;
    std::string lastError;

    ~NewcamdSession() {
        if (client) {
            client->set_key_update_callback({});
            client.reset();
        }
    }
};

struct NewcamdInstance {
    std::mutex mutex;
    std::map<std::string, std::shared_ptr<NewcamdSession>> sessionsByClient;
    std::map<std::string, ServiceBinding> servicesByStream;
    uint64_t nextServiceGeneration = 0;
};

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

void parse_pmt_section(const std::vector<uint8_t>& section, ServiceBinding& binding) {
    if (section.size() < 12 || section[0] != 0x02) return;
    const uint16_t serviceId = static_cast<uint16_t>((section[3] << 8) | section[4]);
    if (binding.serviceId && serviceId != binding.serviceId) return;
    binding.serviceId = serviceId;
    const uint16_t programInfoLength = static_cast<uint16_t>(((section[10] & 0x0F) << 8) | section[11]);
    size_t pos = 12;
    if (pos + programInfoLength > section.size()) return;
    collect_ca_descriptors(section.data() + pos, programInfoLength, binding.ecmPids);
    pos += programInfoLength;
    const size_t crcStart = section.size() >= 4 ? section.size() - 4 : section.size();
    while (pos + 5 <= crcStart) {
        const uint16_t esInfoLength = static_cast<uint16_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
        pos += 5;
        if (pos + esInfoLength > crcStart) break;
        collect_ca_descriptors(section.data() + pos, esInfoLength, binding.ecmPids);
        pos += esInfoLength;
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
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        const auto existing = inst->sessionsByClient.find(clientKey);
        if (existing != inst->sessionsByClient.end() && existing->second && existing->second->connected) {
            return TVS_CA_RESULT_OK;
        }
    }

    std::string parseError;
    Json::Value config = parse_config(reader->backend_config_json, parseError);
    if (!parseError.empty()) {
        write_error(error, error_size, "invalid Newcamd backend_config JSON: " + parseError);
        return TVS_CA_RESULT_ERROR;
    }

    auto session = std::make_shared<NewcamdSession>();
    session->client = std::make_unique<NewcamdClient>(
        config.get("host", "127.0.0.1").asString(),
        config.get("port", 15000).asInt(),
        config.get("user", "user").asString(),
        config.get("pass", "pass").asString(),
        config.get("des", "0102030405060708091011121314").asString());

    session->client->set_key_update_callback([inst, clientKey](uint16_t messageId, uint8_t parity, const uint8_t* cw) {
        if (!cw || !inst || messageId == 0) return;
        std::lock_guard<std::mutex> lock(inst->mutex);
        ServiceBinding* target = nullptr;
        ServiceBinding* singleCandidate = nullptr;
        unsigned candidates = 0;
        for (auto& [streamId, binding] : inst->servicesByStream) {
            (void)streamId;
            if (binding.clientKey != clientKey) continue;
            ++candidates;
            singleCandidate = &binding;
            if (binding.pendingEcmIds.erase(messageId) > 0) {
                target = &binding;
                break;
            }
        }
        if (!target && candidates == 1) target = singleCandidate;
        if (!target) return;
        ControlWordSlot& slot = (parity & 1) ? target->odd : target->even;
        if (!slot.key) return;
        dvbcsa_key_set(cw, slot.key);
        slot.valid = true;
        ++slot.updates;
        ++target->cwUpdates;
    });

    if (session->client->connect() && session->client->login()) {
        session->client->start_receiver();
        session->connected = true;
        session->lastError = session->client->last_error();
        std::lock_guard<std::mutex> lock(inst->mutex);
        inst->sessionsByClient[clientKey] = session;
        return TVS_CA_RESULT_OK;
    }

    const std::string detail = session->client ? session->client->last_error() : std::string();
    write_error(error, error_size, detail.empty() ? "Newcamd connect/login failed" : "Newcamd connect/login failed: " + detail);
    return TVS_CA_RESULT_ERROR;
}

static void newcamd_close_reader(void* instance, const char* reader_key) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !reader_key) return;

    std::shared_ptr<NewcamdSession> removedSession;
    const std::string clientKey = reader_key;
    {
        std::lock_guard<std::mutex> lock(inst->mutex);
        for (auto it = inst->servicesByStream.begin(); it != inst->servicesByStream.end();) {
            if (it->second.clientKey == clientKey) it = inst->servicesByStream.erase(it);
            else ++it;
        }
        auto sessionIt = inst->sessionsByClient.find(clientKey);
        if (sessionIt != inst->sessionsByClient.end()) {
            removedSession = std::move(sessionIt->second);
            inst->sessionsByClient.erase(sessionIt);
        }
    }

    // Destruction disconnects the TCP client and joins its receiver. The
    // receiver callback also takes inst->mutex, so destruction/join must occur
    // after the plugin mutex is released.
    removedSession.reset();
}

static int newcamd_start_service(void* instance, const char* reader_key, const struct tvs_ca_service_info_v1* service, char* error, size_t error_size) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !reader_key || !service || !service->stream_id) {
        write_error(error, error_size, "invalid Newcamd service binding");
        return TVS_CA_RESULT_ERROR;
    }
    std::lock_guard<std::mutex> lock(inst->mutex);
    if (!inst->sessionsByClient.count(reader_key)) {
        write_error(error, error_size, "Newcamd CAM client is not connected");
        return TVS_CA_RESULT_ERROR;
    }
    inst->servicesByStream.erase(service->stream_id);
    auto& binding = inst->servicesByStream.try_emplace(service->stream_id).first->second;
    binding.clientKey = reader_key;
    binding.serviceId = service->service_id <= 0xFFFF ? static_cast<uint16_t>(service->service_id) : 0;
    binding.generation = ++inst->nextServiceGeneration;
    if (binding.generation == 0) binding.generation = ++inst->nextServiceGeneration;
    return TVS_CA_RESULT_OK;
}

static void newcamd_stop_service(void* instance, const char* stream_id) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !stream_id) return;
    std::lock_guard<std::mutex> lock(inst->mutex);
    inst->servicesByStream.erase(stream_id);
}

static int newcamd_process_ts(void* instance, const char* stream_id, uint8_t* data, size_t size, struct tvs_ca_ts_result_v1* result) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !stream_id || !data || !result) return TVS_CA_RESULT_PASSTHROUGH;

    struct EcmTask {
        EcmDescriptor descriptor;
        std::vector<uint8_t> section;
    };
    struct EcmResult {
        bool success = false;
        uint16_t messageId = 0;
        std::string error;
    };

    std::unique_lock<std::mutex> instanceLock(inst->mutex);
    auto serviceIt = inst->servicesByStream.find(stream_id);
    if (serviceIt == inst->servicesByStream.end()) return TVS_CA_RESULT_PASSTHROUGH;
    ServiceBinding& binding = serviceIt->second;
    const uint64_t bindingGeneration = binding.generation;
    const std::string clientKey = binding.clientKey;
    const uint16_t serviceId = binding.serviceId;

    auto sessionIt = inst->sessionsByClient.find(clientKey);
    if (sessionIt == inst->sessionsByClient.end() || !sessionIt->second) return TVS_CA_RESULT_PASSTHROUGH;
    std::shared_ptr<NewcamdSession> session = sessionIt->second;
    if (!session->connected || !session->client) return TVS_CA_RESULT_PASSTHROUGH;

    bool changed = false;
    bool waitingForKey = false;
    bool sawEcm = false;
    std::vector<EcmTask> pendingEcms;

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
        const bool tei = (packet[1] & 0x80) != 0;
        if (tei) {
            ++binding.diagTeiPackets;
            continue;
        }

        const uint16_t pid = parse_pid(packet);
        const bool payloadStart = (packet[1] & 0x40) != 0;
        const uint8_t scramblingControl = static_cast<uint8_t>((packet[3] >> 6) & 0x03);
        const uint8_t adaptationControl = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
        const uint8_t continuityCounter = static_cast<uint8_t>(packet[3] & 0x0F);

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
                    binding.diagSampleTei = false;
                }
            }
        }

        size_t payloadSize = 0;
        const uint8_t* payload = ts_payload(packet, &payloadSize);

        if (payload && payloadSize > 0) {
            auto sections = binding.assemblers[pid].push(payload, payloadSize, payloadStart);
            for (const auto& section : sections) {
                if (section.empty()) continue;
                if (section[0] == 0x02) {
                    binding.pmtPid = pid;
                    parse_pmt_section(section, binding);
                } else if (section[0] == 0x80 || section[0] == 0x81) {
                    if (binding.ecmPids.empty()) continue;
                    bool knownEcmPid = false;
                    for (const auto& ecm : binding.ecmPids) if (ecm.pid == pid) knownEcmPid = true;
                    if (knownEcmPid) {
                        sawEcm = true;
                        const std::string signature = ecm_signature(section);
                        if (binding.sentEcms.insert(signature).second) {
                            pendingEcms.push_back({descriptor_for_pid(binding, pid), section});
                            if (binding.sentEcms.size() > 128) binding.sentEcms.clear();
                        }
                    }
                }
            }
        }

        if (scramblingControl == 0) {
            result->packets_clear++;
            continue;
        }
        result->packets_scrambled++;
        if (scramblingControl == 1) {
            waitingForKey = true;
            continue;
        }

        size_t scrambledPayloadSize = 0;
        uint8_t* scrambledPayload = ts_payload_mut(packet, &scrambledPayloadSize);
        if (!scrambledPayload || scrambledPayloadSize < 8) {
            waitingForKey = true;
            continue;
        }

        ControlWordSlot& slot = scramblingControl == 3 ? binding.odd : binding.even;
        if (!slot.valid || !slot.key) {
            waitingForKey = true;
            continue;
        }

        // Keep the existing CA payload transform unchanged in this reliability-only release.
        const size_t decryptSize = scrambledPayloadSize & ~static_cast<size_t>(7);
        if (decryptSize == 0) {
            waitingForKey = true;
            continue;
        }
        dvbcsa_decrypt(slot.key, scrambledPayload, decryptSize);
        packet[3] &= 0x3F;
        result->packets_changed++;
        changed = true;
    }

    if (!pendingEcms.empty()) {
        instanceLock.unlock();

        std::vector<EcmResult> ecmResults;
        ecmResults.reserve(pendingEcms.size());
        {
            // Network requests may block. Serialize them per session, but never
            // hold the global plugin mutex while waiting on external I/O.
            std::lock_guard<std::mutex> ioLock(session->ioMutex);
            for (const auto& task : pendingEcms) {
                EcmResult ecmResult;
                if (session->client && session->client->send_ecm(
                        serviceId,
                        task.descriptor.caid,
                        task.descriptor.provid,
                        task.section,
                        &ecmResult.messageId)) {
                    ecmResult.success = true;
                } else if (session->client) {
                    ecmResult.error = session->client->last_error();
                } else {
                    ecmResult.error = "Newcamd session closed";
                }
                ecmResults.push_back(std::move(ecmResult));
            }
        }

        instanceLock.lock();

        // References obtained before unlock are no longer trusted. Re-resolve
        // the binding and session and only apply results to the same generation.
        serviceIt = inst->servicesByStream.find(stream_id);
        if (serviceIt == inst->servicesByStream.end() ||
            serviceIt->second.generation != bindingGeneration ||
            serviceIt->second.clientKey != clientKey) {
            instanceLock.unlock();
            return changed ? TVS_CA_RESULT_OK : TVS_CA_RESULT_PASSTHROUGH;
        }

        auto currentSessionIt = inst->sessionsByClient.find(clientKey);
        if (currentSessionIt == inst->sessionsByClient.end() ||
            currentSessionIt->second != session) {
            instanceLock.unlock();
            return changed ? TVS_CA_RESULT_OK : TVS_CA_RESULT_PASSTHROUGH;
        }

        ServiceBinding& currentBinding = serviceIt->second;
        for (const auto& ecmResult : ecmResults) {
            if (ecmResult.success) {
                ++currentBinding.ecmRequests;
                if (ecmResult.messageId != 0) currentBinding.pendingEcmIds.insert(ecmResult.messageId);
            } else if (!ecmResult.error.empty()) {
                session->lastError = ecmResult.error;
            }
        }
    }

    // Re-resolve the binding even when no network I/O was queued, so the rest
    // of the function uses one explicit current reference.
    serviceIt = inst->servicesByStream.find(stream_id);
    if (serviceIt == inst->servicesByStream.end() ||
        serviceIt->second.generation != bindingGeneration ||
        serviceIt->second.clientKey != clientKey) {
        instanceLock.unlock();
        return changed ? TVS_CA_RESULT_OK : TVS_CA_RESULT_PASSTHROUGH;
    }
    ServiceBinding& currentBinding = serviceIt->second;

    const uint64_t diagNowMs = monotonic_ms();
    if (currentBinding.diagLastLogMs == 0 || diagNowMs - currentBinding.diagLastLogMs >= 1000) {
        currentBinding.diagLastLogMs = diagNowMs;
        std::cerr
            << "CA TS DIAG: stream=" << stream_id
            << " calls=" << currentBinding.diagCalls
            << " buffer_size=" << size
            << " remainder=" << bufferRemainder
            << " unaligned_buffers=" << currentBinding.diagUnalignedBuffers
            << " bad_sync=" << currentBinding.diagBadSyncPackets
            << " tei=" << currentBinding.diagTeiPackets
            << " bad_afc=" << currentBinding.diagBadAdaptationPackets
            << " payload_packets=" << currentBinding.diagPayloadPackets
            << " nonmod8=" << currentBinding.diagNonModulo8Payloads
            << " scrambled=" << currentBinding.diagScrambledPackets
            << " scrambled_nonmod8=" << currentBinding.diagScrambledNonModulo8Payloads
            << " sample_pid=" << currentBinding.diagSamplePid
            << " sample_sc=" << static_cast<unsigned>(currentBinding.diagSampleScrambling)
            << " sample_afc=" << static_cast<unsigned>(currentBinding.diagSampleAdaptation)
            << " sample_payload=" << currentBinding.diagSamplePayload
            << " sample_mod8=" << (currentBinding.diagSamplePayload % 8)
            << " sample_cc=" << static_cast<unsigned>(currentBinding.diagSampleCc)
            << " sample_pusi=" << (currentBinding.diagSamplePusi ? 1 : 0)
            << " sample_tei=" << (currentBinding.diagSampleTei ? 1 : 0)
            << std::endl;
    }

    int returnCode = TVS_CA_RESULT_PASSTHROUGH;
    if (changed) {
        write_status(result, "BACKEND_ACTIVE");
        returnCode = TVS_CA_RESULT_OK;
    } else if (waitingForKey) {
        const std::string detail = session->lastError.empty() ? "WAITING_FOR_CW" : "WAITING_FOR_CW: " + session->lastError;
        write_status(result, detail);
        returnCode = TVS_CA_RESULT_RETRY;
    } else if (sawEcm) {
        write_status(result, "ECM_SEEN_NO_CW");
        returnCode = TVS_CA_RESULT_RETRY;
    } else {
        write_status(result, "NO_SCRAMBLED_PAYLOAD");
    }

    // Never allow the last session reference to be destroyed while holding the
    // global mutex: destruction may stop/join the receiver thread.
    instanceLock.unlock();
    return returnCode;
}
static const char* newcamd_status_json(void* instance) {
    static thread_local std::string status;
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst) {
        status = "{\"status\":\"disconnected\",\"clients\":0}";
        return status.c_str();
    }
    std::lock_guard<std::mutex> lock(inst->mutex);
    size_t connected = 0;
    size_t authenticated = 0;
    size_t evenKeys = 0;
    size_t oddKeys = 0;
    for (const auto& entry : inst->sessionsByClient) {
        if (!entry.second || !entry.second->connected) continue;
        ++connected;
        if (entry.second->client && entry.second->client->authenticated()) ++authenticated;
    }
    for (const auto& entry : inst->servicesByStream) {
        if (entry.second.even.valid) ++evenKeys;
        if (entry.second.odd.valid) ++oddKeys;
    }
    status = "{\"status\":\"" + std::string(connected ? "connected" : "disconnected") +
             "\",\"clients\":" + std::to_string(connected) +
             ",\"authenticated_clients\":" + std::to_string(authenticated) +
             ",\"streams\":" + std::to_string(inst->servicesByStream.size()) +
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
