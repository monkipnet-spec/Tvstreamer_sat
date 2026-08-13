#include "CardManager.h"

#include "PhoenixManager.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <utility>

namespace {

std::string readerPath(const Json::Value& item) {
    const std::string stable = item.get("stable_device", "").asString();
    return stable.empty() ? item.get("device", "").asString() : stable;
}

std::string ttySerialFromPath(const std::string& path) {
    const auto marker = path.find("UART_");
    if (marker == std::string::npos) return {};
    const auto begin = marker + 5;
    const auto end = path.find("-if", begin);
    if (end == std::string::npos || end <= begin) return {};
    return path.substr(begin, end - begin);
}

bool lifecycleReady(const std::string& status) {
    return status == "READY" || status == "CARD_UNREADABLE";
}

} // namespace

CardManager& CardManager::instance() {
    static CardManager manager;
    return manager;
}

CardManager::~CardManager() {
    stopMonitor_.store(true);
    if (monitorThread_.joinable()) monitorThread_.join();
}

void CardManager::configure(const std::vector<CaReaderConfig>& readers) {
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        readerConfigs_ = readers;
        for (auto& item : readerConfigs_) {
            item.maxServices = std::clamp(item.maxServices, 1u, kMaxConfigurableServices);
            item.retrySeconds = std::clamp(item.retrySeconds, 2u, 300u);
        }
    }
    {
        std::lock_guard<std::mutex> lock(inventoryMutex_);
        cachedInventory_.clear();
        lastInventoryRefresh_ = {};
    }
    ensureMonitorStarted();
}

std::string CardManager::isoTime(const std::chrono::system_clock::time_point& value) {
    if (value.time_since_epoch().count() == 0) return {};
    const std::time_t time = std::chrono::system_clock::to_time_t(value);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

CardManager::ReaderProfile CardManager::applyKnownProfile(ReaderProfile profile) {
    if (profile.serial == "A104JCGD") {
        profile.displayName = "Voprosy_otvety";
        profile.cardSystem = "Irdeto";
        profile.caid = "0652";
        profile.provider = "0406BE";
        profile.maxServices = 10;
    } else if (profile.serial == "AD023J2Q") {
        profile.displayName = "Pervy1";
        profile.cardSystem = "Irdeto";
        profile.caid = "0652";
        profile.provider = "0400DC";
        profile.maxServices = 10;
    }
    return profile;
}

CaReaderConfig CardManager::configuredPolicy(const ReaderProfile& profile) const {
    CaReaderConfig result;
    result.readerKey = profile.key;
    result.serial = profile.serial;
    result.maxServices = profile.maxServices;
    result.autoActivate = true;
    result.autoReactivate = true;
    result.retrySeconds = 5;

    std::lock_guard<std::mutex> lock(configMutex_);
    for (const auto& config : readerConfigs_) {
        if ((!config.readerKey.empty() && (config.readerKey == profile.key ||
                                           config.readerKey == profile.stableDevice ||
                                           config.readerKey == profile.device)) ||
            (!config.serial.empty() && config.serial == profile.serial)) {
            result = config;
            if (result.readerKey.empty()) result.readerKey = profile.key;
            if (result.serial.empty()) result.serial = profile.serial;
            result.maxServices = std::clamp(result.maxServices, 1u, kMaxConfigurableServices);
            result.retrySeconds = std::clamp(result.retrySeconds, 2u, 300u);
            break;
        }
    }
    return result;
}

CardManager::ReaderProfile CardManager::applyConfiguredPolicy(ReaderProfile profile) const {
    const CaReaderConfig policy = configuredPolicy(profile);
    profile.maxServices = policy.maxServices;
    profile.autoActivate = policy.autoActivate;
    profile.autoReactivate = policy.autoReactivate;
    profile.retrySeconds = policy.retrySeconds;
    return profile;
}

std::map<std::string, CardManager::ReaderProfile> CardManager::inventory(bool forceRefresh) const {
    std::lock_guard<std::mutex> cacheLock(inventoryMutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!forceRefresh && !cachedInventory_.empty() &&
        lastInventoryRefresh_.time_since_epoch().count() != 0 &&
        now - lastInventoryRefresh_ < std::chrono::seconds(2)) {
        return cachedInventory_;
    }

    std::map<std::string, ReaderProfile> result;
    const Json::Value readers = PhoenixManager::readers(false);
    for (const auto& item : readers) {
        ReaderProfile profile;
        profile.stableDevice = readerPath(item);
        profile.device = item.get("device", "").asString();
        profile.serial = item.get("serial", "").asString();
        if (profile.serial.empty()) profile.serial = ttySerialFromPath(profile.stableDevice);
        profile.key = profile.stableDevice.empty() ? profile.device : profile.stableDevice;
        profile.displayName = item.get("name", "Phoenix").asString();
        profile.hardwareStatus = item.get("status", "unknown").asString();
        profile.hardwareDetail = item.get("detail", "").asString();
        profile.externalOwner = profile.hardwareStatus == "busy";
        profile = applyConfiguredPolicy(applyKnownProfile(std::move(profile)));
        if (!profile.key.empty()) result[profile.key] = profile;
    }
    cachedInventory_ = result;
    lastInventoryRefresh_ = now;
    return result;
}

CardManager::ReaderProfile CardManager::resolveReader(
    const std::string& requested,
    const std::map<std::string, ReaderProfile>& readers) {
    if (requested.empty()) return {};
    for (const auto& [key, reader] : readers) {
        if (requested == key || requested == reader.stableDevice || requested == reader.device ||
            requested == reader.serial) {
            return reader;
        }
    }

    ReaderProfile fallback;
    fallback.key = requested;
    fallback.stableDevice = requested;
    fallback.serial = ttySerialFromPath(requested);
    fallback.displayName = "Phoenix";
    fallback.hardwareStatus = "unavailable";
    fallback.hardwareDetail = "configured reader is not currently detected";
    return applyKnownProfile(std::move(fallback));
}

void CardManager::ensureMonitorStarted() {
    bool expected = false;
    if (!monitorStarted_.compare_exchange_strong(expected, true)) return;
    stopMonitor_.store(false);
    monitorThread_ = std::thread([this]() { monitorLoop(); });
}

void CardManager::updateSlotLifecycleStatus(const std::string& readerKey, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, slot] : slotsByStream_) {
        (void)id;
        if (slot.readerKey != readerKey) continue;
        if (status == "READY" || status == "CARD_UNREADABLE") {
            if (slot.lastStatus == "EXTERNAL_OWNER" || slot.lastStatus == "READER_UNAVAILABLE" ||
                slot.lastStatus == "NO_CARD" || slot.lastStatus == "PROBE_FAILED" ||
                slot.lastStatus == "RESERVED") {
                slot.lastStatus = slot.active ? "ACTIVE" : "RESERVED";
            }
        } else if (status == "EXTERNAL_OWNER") slot.lastStatus = "EXTERNAL_OWNER";
        else if (status == "NO_CARD") slot.lastStatus = "NO_CARD";
        else if (status == "UNAVAILABLE") slot.lastStatus = "READER_UNAVAILABLE";
        else if (status == "PERMISSION") slot.lastStatus = "READER_PERMISSION";
        else if (status == "PROBE_FAILED") slot.lastStatus = "PROBE_FAILED";
    }
}

CardManager::ReaderLifecycle CardManager::lifecycleFor(const std::string& readerKey) const {
    std::lock_guard<std::mutex> lock(lifecycleMutex_);
    const auto found = lifecycle_.find(readerKey);
    return found == lifecycle_.end() ? ReaderLifecycle{} : found->second;
}

void CardManager::updateLifecycleFromHardware(const ReaderProfile& reader) {
    std::string slotStatus;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        auto& state = lifecycle_[reader.key];
        if (reader.externalOwner) {
            state.status = "EXTERNAL_OWNER";
            state.detail = reader.hardwareDetail;
            state.nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(reader.retrySeconds);
            slotStatus = state.status;
        } else if (state.status == "UNKNOWN" || state.status == "EXTERNAL_OWNER" || state.status == "UNAVAILABLE") {
            state.status = "DETECTED";
            state.detail = "reader is free; activation probe pending";
            state.nextAttempt = std::chrono::steady_clock::now();
            slotStatus = state.status;
        }
    }
    if (!slotStatus.empty()) updateSlotLifecycleStatus(reader.key, slotStatus);
}

void CardManager::markMissingReaders(const std::map<std::string, ReaderProfile>& readers) {
    std::set<std::string> knownKeys;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        for (const auto& config : readerConfigs_) if (!config.readerKey.empty()) knownKeys.insert(config.readerKey);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, slot] : slotsByStream_) {
            (void)id;
            knownKeys.insert(slot.readerKey);
        }
    }
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        for (const auto& [key, state] : lifecycle_) {
            (void)state;
            knownKeys.insert(key);
        }
    }
    for (const auto& key : knownKeys) {
        if (readers.count(key)) continue;
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            auto& state = lifecycle_[key];
            state.status = "UNAVAILABLE";
            state.detail = "configured reader is not currently detected";
            state.nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
        updateSlotLifecycleStatus(key, "UNAVAILABLE");
    }
}

Json::Value CardManager::probeReaderNow(const std::string& readerKey, bool manual) {
    Json::Value response;
    response["reader"] = readerKey;
    if (readerKey.empty()) {
        response["result"] = "error";
        response["status"] = "INVALID_READER";
        return response;
    }

    const auto readers = inventory(true);
    const ReaderProfile profile = applyConfiguredPolicy(resolveReader(readerKey, readers));
    if (profile.key.empty() || profile.hardwareStatus == "unavailable") {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        auto& state = lifecycle_[readerKey];
        state.status = "UNAVAILABLE";
        state.detail = "reader is not detected";
        state.nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(profile.retrySeconds ? profile.retrySeconds : 5);
        response["result"] = "error";
        response["status"] = state.status;
        response["detail"] = state.detail;
        return response;
    }
    if (profile.externalOwner) {
        {
            std::lock_guard<std::mutex> lock(lifecycleMutex_);
            auto& state = lifecycle_[profile.key];
            state.status = "EXTERNAL_OWNER";
            state.detail = profile.hardwareDetail;
            state.nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(profile.retrySeconds);
        }
        updateSlotLifecycleStatus(profile.key, "EXTERNAL_OWNER");
        response["result"] = "busy";
        response["status"] = "EXTERNAL_OWNER";
        response["detail"] = profile.hardwareDetail;
        return response;
    }

    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        auto& state = lifecycle_[profile.key];
        if (state.probing) {
            response["result"] = "busy";
            response["status"] = "PROBING";
            response["detail"] = "reader activation probe already in progress";
            return response;
        }
        state.probing = true;
        state.status = "PROBING";
        state.detail = manual ? "manual reactivation in progress" : "automatic activation in progress";
    }

    const Json::Value probe = PhoenixManager::reader(profile.key, true);
    const auto nowSystem = std::chrono::system_clock::now();
    std::string lifecycleStatus = "PROBE_FAILED";
    std::string detail = "reader disappeared during activation probe";
    std::string atr;
    std::string detectedSystem;
    std::string detectedProvider;
    if (!probe.isNull()) {
        const std::string status = probe.get("status", "unknown").asString();
        detail = probe.get("detail", "").asString();
        atr = probe.get("atr", "").asString();
        detectedSystem = probe.get("card_system", "").asString();
        detectedProvider = probe.get("provider_name", "").asString();
        if (status == "card") lifecycleStatus = "READY";
        else if (status == "card_unreadable") lifecycleStatus = "CARD_UNREADABLE";
        else if (status == "no_card") lifecycleStatus = "NO_CARD";
        else if (status == "busy") lifecycleStatus = "EXTERNAL_OWNER";
        else if (status == "permission") lifecycleStatus = "PERMISSION";
        else if (status == "unavailable") lifecycleStatus = "UNAVAILABLE";
        else lifecycleStatus = "PROBE_FAILED";
    }

    uint64_t attempts = 0;
    {
        std::lock_guard<std::mutex> lock(lifecycleMutex_);
        auto& state = lifecycle_[profile.key];
        state.probing = false;
        state.status = lifecycleStatus;
        state.detail = detail;
        state.atr = atr;
        state.detectedCardSystem = detectedSystem;
        state.detectedProvider = detectedProvider;
        ++state.attempts;
        attempts = state.attempts;
        state.lastAttempt = nowSystem;
        if (lifecycleReady(lifecycleStatus)) state.lastReady = nowSystem;
        state.nextAttempt = std::chrono::steady_clock::now() + std::chrono::seconds(profile.retrySeconds);
    }
    updateSlotLifecycleStatus(profile.key, lifecycleStatus);

    std::cerr << "CA CardManager: " << (manual ? "manual" : "auto")
              << " reader activation reader=" << profile.key
              << " status=" << lifecycleStatus
              << " attempt=" << attempts
              << " retry=" << profile.retrySeconds << "s" << std::endl;

    response["result"] = lifecycleReady(lifecycleStatus) ? "ok" : "error";
    response["status"] = lifecycleStatus;
    response["detail"] = detail;
    response["attempts"] = Json::UInt64(attempts);
    response["atr"] = atr;
    return response;
}

Json::Value CardManager::reactivateReader(const std::string& readerKey) {
    ensureMonitorStarted();
    return probeReaderNow(readerKey, true);
}

void CardManager::monitorLoop() {
    while (!stopMonitor_.load()) {
        const auto readers = inventory(true);
        markMissingReaders(readers);
        const auto now = std::chrono::steady_clock::now();

        for (const auto& [key, reader] : readers) {
            updateLifecycleFromHardware(reader);
            ReaderLifecycle state = lifecycleFor(key);
            if (reader.externalOwner || state.probing || lifecycleReady(state.status)) continue;

            bool shouldProbe = false;
            if (state.status == "DETECTED") {
                const bool wasReadyBefore = state.lastReady.time_since_epoch().count() != 0;
                shouldProbe = wasReadyBefore ? reader.autoReactivate : reader.autoActivate;
            } else if ((state.status == "NO_CARD" || state.status == "PROBE_FAILED" ||
                      state.status == "PERMISSION" || state.status == "UNAVAILABLE" ||
                      state.status == "EXTERNAL_OWNER") && reader.autoReactivate &&
                     (state.nextAttempt.time_since_epoch().count() == 0 || now >= state.nextAttempt)) {
                shouldProbe = true;
            }
            if (shouldProbe) (void)probeReaderNow(key, false);
            if (stopMonitor_.load()) break;
        }

        for (int i = 0; i < 10 && !stopMonitor_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
}

bool CardManager::reserveService(const StreamConfig& config, std::string* error) {
    if (error) error->clear();
    if (config.conditionalAccessReader.empty()) return true;
    ensureMonitorStarted();

    const auto readers = inventory(true);
    std::lock_guard<std::mutex> lock(mutex_);
    if (slotsByStream_.count(config.id)) return true;

    ReaderProfile reader;
    if (config.conditionalAccessReader == "auto") {
        if (error) *error = "CA AUTO routing is disabled: select the specific Phoenix reader assigned to this channel";
        std::cerr << "CA CardManager: reservation rejected stream=" << config.id
                  << " reason=legacy-auto-routing-disabled" << std::endl;
        return false;
    }
    reader = applyConfiguredPolicy(resolveReader(config.conditionalAccessReader, readers));
    if (reader.key.empty()) {
        if (error) *error = "CA reader path is empty";
        return false;
    }

    const auto used = static_cast<unsigned>(std::count_if(
        slotsByStream_.begin(), slotsByStream_.end(), [&](const auto& entry) {
            return entry.second.readerKey == reader.key;
        }));
    if (used >= reader.maxServices) {
        if (error) {
            *error = "CA service limit reached for " + reader.key + ": " +
                     std::to_string(used) + "/" + std::to_string(reader.maxServices);
        }
        std::cerr << "CA CardManager: reservation rejected reader=" << reader.key
                  << " stream=" << config.id << " slots=" << used << "/"
                  << reader.maxServices << std::endl;
        return false;
    }

    ServiceSlot slot;
    slot.streamId = config.id;
    slot.streamName = config.name;
    slot.serviceId = config.inputServiceId ? config.inputServiceId : config.serviceId;
    slot.serviceProvider = config.serviceProvider;
    slot.requestedReader = config.conditionalAccessReader;
    slot.readerKey = reader.key;
    const ReaderLifecycle lifecycle = lifecycleFor(reader.key);
    slot.lastStatus = reader.externalOwner ? "EXTERNAL_OWNER" :
        (lifecycleReady(lifecycle.status) ? "RESERVED" : lifecycle.status);
    if (slot.lastStatus.empty() || slot.lastStatus == "UNKNOWN" || slot.lastStatus == "DETECTED") {
        slot.lastStatus = "RESERVED";
    }
    slotsByStream_[config.id] = std::move(slot);
    std::cerr << "CA CardManager: reserved reader=" << reader.key
              << " stream=" << config.id
              << " sid=" << (config.inputServiceId ? config.inputServiceId : config.serviceId)
              << " slots=" << (used + 1) << "/" << reader.maxServices
              << " external_owner=" << (reader.externalOwner ? "yes" : "no")
              << " auto_activate=" << (reader.autoActivate ? "yes" : "no")
              << " auto_reactivate=" << (reader.autoReactivate ? "yes" : "no")
              << " native_backend=off" << std::endl;
    return true;
}

void CardManager::activateService(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slotsByStream_.find(streamId);
    if (found == slotsByStream_.end()) return;
    found->second.active = true;
    if (found->second.lastStatus.empty() || found->second.lastStatus == "RESERVED") {
        found->second.lastStatus = "ACTIVE";
    }
    std::cerr << "CA CardManager: active reader=" << found->second.readerKey
              << " stream=" << found->second.streamId
              << " sid=" << found->second.serviceId << std::endl;
}

void CardManager::releaseService(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slotsByStream_.find(streamId);
    if (found == slotsByStream_.end()) return;
    std::cerr << "CA CardManager: released reader=" << found->second.readerKey
              << " stream=" << found->second.streamId
              << " sid=" << found->second.serviceId << std::endl;
    slotsByStream_.erase(found);
}

void CardManager::releaseAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    slotsByStream_.clear();
}

void CardManager::recordTransaction(const std::string& streamId, uint64_t latencyMs,
                                    bool success, const std::string& status) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = slotsByStream_.find(streamId);
    if (found == slotsByStream_.end()) return;
    ServiceSlot& slot = found->second;
    ++slot.transactions;
    if (success) ++slot.successfulTransactions;
    else ++slot.failedTransactions;
    slot.lastLatencyMs = latencyMs;
    slot.maxLatencyMs = std::max(slot.maxLatencyMs, latencyMs);
    slot.averageLatencyMs += (static_cast<double>(latencyMs) - slot.averageLatencyMs) /
                             static_cast<double>(slot.transactions);
    slot.lastTransactionAt = std::chrono::system_clock::now();
    if (!status.empty()) slot.lastStatus = status;
}

Json::Value CardManager::slotToJson(const ServiceSlot& slot) const {
    Json::Value item;
    item["stream_id"] = slot.streamId;
    item["stream_name"] = slot.streamName;
    item["service_id"] = slot.serviceId;
    item["service_provider"] = slot.serviceProvider;
    item["reader"] = slot.readerKey;
    item["requested_reader"] = slot.requestedReader;
    item["auto_assigned"] = slot.requestedReader == "auto";
    item["active"] = slot.active;
    item["status"] = slot.lastStatus;
    item["reserved_at"] = isoTime(slot.reservedAt);
    item["transactions"] = Json::UInt64(slot.transactions);
    item["transactions_ok"] = Json::UInt64(slot.successfulTransactions);
    item["transactions_failed"] = Json::UInt64(slot.failedTransactions);
    item["last_latency_ms"] = Json::UInt64(slot.lastLatencyMs);
    item["max_latency_ms"] = Json::UInt64(slot.maxLatencyMs);
    item["average_latency_ms"] = slot.averageLatencyMs;
    item["last_transaction_at"] = isoTime(slot.lastTransactionAt);
    return item;
}

Json::Value CardManager::snapshot() const {
    const auto readers = inventory();
    std::lock_guard<std::mutex> lock(mutex_);

    Json::Value root;
    root["mode"] = "internal-control-plane";
    root["native_card_backend"] = false;
    root["network_ca_server"] = false;
    root["external_key_export"] = false;
    root["default_max_services"] = kDefaultMaxServices;
    root["max_configurable_services"] = kMaxConfigurableServices;
    root["reserved_services"] = Json::UInt(slotsByStream_.size());

    Json::Value readerList(Json::arrayValue);
    for (const auto& [key, rawReader] : readers) {
        ReaderProfile reader = applyConfiguredPolicy(rawReader);
        const ReaderLifecycle lifecycle = lifecycleFor(key);
        Json::Value item;
        item["key"] = key;
        item["stable_device"] = reader.stableDevice;
        item["device"] = reader.device;
        item["serial"] = reader.serial;
        item["display_name"] = reader.displayName;
        item["card_system"] = reader.cardSystem;
        item["caid"] = reader.caid;
        item["provider"] = reader.provider;
        item["max_services"] = reader.maxServices;
        item["auto_activate"] = reader.autoActivate;
        item["auto_reactivate"] = reader.autoReactivate;
        item["retry_seconds"] = reader.retrySeconds;
        item["hardware_status"] = reader.hardwareStatus;
        item["hardware_detail"] = reader.hardwareDetail;
        item["external_owner"] = reader.externalOwner;
        item["activation_status"] = lifecycle.status;
        item["activation_detail"] = lifecycle.detail;
        item["activation_attempts"] = Json::UInt64(lifecycle.attempts);
        item["activation_probing"] = lifecycle.probing;
        item["last_activation_attempt"] = isoTime(lifecycle.lastAttempt);
        item["last_ready"] = isoTime(lifecycle.lastReady);
        item["atr"] = lifecycle.atr;
        item["detected_card_system"] = lifecycle.detectedCardSystem;
        item["detected_provider"] = lifecycle.detectedProvider;

        Json::Value services(Json::arrayValue);
        unsigned active = 0;
        for (const auto& [streamId, slot] : slotsByStream_) {
            (void)streamId;
            if (slot.readerKey != key) continue;
            services.append(slotToJson(slot));
            if (slot.active) ++active;
        }
        item["services"] = services;
        item["services_used"] = Json::UInt(services.size());
        item["services_active"] = active;
        item["services_free"] = reader.maxServices > services.size()
            ? Json::UInt(reader.maxServices - static_cast<unsigned>(services.size()))
            : Json::UInt(0);
        readerList.append(item);
    }

    // Keep configured/disconnected readers visible even without an active slot.
    std::vector<CaReaderConfig> policies;
    {
        std::lock_guard<std::mutex> configLock(configMutex_);
        policies = readerConfigs_;
    }
    for (const auto& policy : policies) {
        if (policy.readerKey.empty()) continue;
        bool exists = false;
        for (const auto& item : readerList) {
            if (item.get("key", "").asString() == policy.readerKey ||
                (!policy.serial.empty() && item.get("serial", "").asString() == policy.serial)) {
                exists = true;
                break;
            }
        }
        if (exists) continue;
        Json::Value item;
        item["key"] = policy.readerKey;
        item["stable_device"] = policy.readerKey;
        item["serial"] = policy.serial;
        item["display_name"] = "Phoenix";
        item["max_services"] = std::clamp(policy.maxServices, 1u, kMaxConfigurableServices);
        item["auto_activate"] = policy.autoActivate;
        item["auto_reactivate"] = policy.autoReactivate;
        item["retry_seconds"] = std::clamp(policy.retrySeconds, 2u, 300u);
        item["hardware_status"] = "unavailable";
        item["external_owner"] = false;
        const ReaderLifecycle lifecycle = lifecycleFor(policy.readerKey);
        item["activation_status"] = lifecycle.status == "UNKNOWN" ? "UNAVAILABLE" : lifecycle.status;
        item["activation_detail"] = lifecycle.detail;
        item["activation_attempts"] = Json::UInt64(lifecycle.attempts);
        item["last_activation_attempt"] = isoTime(lifecycle.lastAttempt);
        item["last_ready"] = isoTime(lifecycle.lastReady);
        item["services"] = Json::Value(Json::arrayValue);
        item["services_used"] = 0;
        item["services_active"] = 0;
        item["services_free"] = item["max_services"];
        readerList.append(item);
    }

    // Keep configured-but-temporarily-disconnected readers with active slots visible.
    for (const auto& [streamId, slot] : slotsByStream_) {
        (void)streamId;
        Json::Value* existing = nullptr;
        for (auto& item : readerList) {
            if (item.get("key", "").asString() == slot.readerKey) {
                existing = &item;
                break;
            }
        }
        if (!existing) {
            ReaderProfile reader = applyConfiguredPolicy(resolveReader(slot.readerKey, readers));
            Json::Value item;
            item["key"] = reader.key;
            item["stable_device"] = reader.stableDevice;
            item["device"] = reader.device;
            item["serial"] = reader.serial;
            item["display_name"] = reader.displayName;
            item["card_system"] = reader.cardSystem;
            item["caid"] = reader.caid;
            item["provider"] = reader.provider;
            item["max_services"] = reader.maxServices;
            item["auto_activate"] = reader.autoActivate;
            item["auto_reactivate"] = reader.autoReactivate;
            item["retry_seconds"] = reader.retrySeconds;
            item["hardware_status"] = "unavailable";
            item["external_owner"] = false;
            const ReaderLifecycle lifecycle = lifecycleFor(reader.key);
            item["activation_status"] = lifecycle.status;
            item["activation_detail"] = lifecycle.detail;
            item["services"] = Json::Value(Json::arrayValue);
            readerList.append(item);
            existing = &readerList[readerList.size() - 1];
        }
        (*existing)["services"].append(slotToJson(slot));
    }

    for (auto& item : readerList) {
        const unsigned maxServices = item.get("max_services", kDefaultMaxServices).asUInt();
        const unsigned used = item["services"].isArray() ? item["services"].size() : 0;
        unsigned active = 0;
        if (item["services"].isArray()) {
            for (const auto& slot : item["services"]) if (slot.get("active", false).asBool()) ++active;
        }
        item["services_used"] = used;
        item["services_active"] = active;
        item["services_free"] = maxServices > used ? maxServices - used : 0;
    }
    root["readers"] = readerList;
    return root;
}

Json::Value CardManager::streamState(const std::string& streamId) const {
    const auto readers = inventory();
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value result;
    const auto found = slotsByStream_.find(streamId);
    if (found == slotsByStream_.end()) {
        result["managed"] = false;
        result["status"] = "UNBOUND";
        result["native_card_backend"] = false;
        return result;
    }
    result = slotToJson(found->second);
    result["managed"] = true;
    result["native_card_backend"] = false;
    ReaderProfile reader = applyConfiguredPolicy(resolveReader(found->second.readerKey, readers));
    result["reader_display_name"] = reader.displayName;
    result["reader_serial"] = reader.serial;
    result["caid"] = reader.caid;
    result["provider"] = reader.provider;
    result["card_system"] = reader.cardSystem;
    result["hardware_status"] = reader.hardwareStatus;
    result["external_owner"] = reader.externalOwner;
    result["max_services"] = reader.maxServices;
    const ReaderLifecycle lifecycle = lifecycleFor(reader.key);
    result["activation_status"] = lifecycle.status;
    result["activation_detail"] = lifecycle.detail;
    return result;
}
