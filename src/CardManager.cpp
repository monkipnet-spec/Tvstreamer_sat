#include "CardManager.h"

#include "PhoenixManager.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace {

std::string readerPath(const Json::Value& item) {
    const std::string stable = item.get("stable_device", "").asString();
    return stable.empty() ? item.get("device", "").asString() : stable;
}

std::string ttySerialFromPath(const std::string& path) {
    // Stable FTDI names used by /dev/serial/by-id normally contain the serial
    // between "UART_" and "-if".  This is only a display/profile fallback;
    // sysfs serial from PhoenixManager remains authoritative when available.
    const auto marker = path.find("UART_");
    if (marker == std::string::npos) return {};
    const auto begin = marker + 5;
    const auto end = path.find("-if", begin);
    if (end == std::string::npos || end <= begin) return {};
    return path.substr(begin, end - begin);
}

} // namespace

CardManager& CardManager::instance() {
    static CardManager manager;
    return manager;
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
    // Deployment profiles learned from the two current FTDI readers.  Matching
    // is by immutable FTDI serial, never ttyUSB index, so reboot enumeration
    // cannot swap profiles.
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

std::map<std::string, CardManager::ReaderProfile> CardManager::inventory(bool forceRefresh) const {
    std::lock_guard<std::mutex> cacheLock(inventoryMutex_);
    const auto now = std::chrono::steady_clock::now();
    if (!forceRefresh && !cachedInventory_.empty() &&
        lastInventoryRefresh_.time_since_epoch().count() != 0 &&
        now - lastInventoryRefresh_ < std::chrono::seconds(5)) {
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
        profile = applyKnownProfile(std::move(profile));
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
        if (requested == key || requested == reader.stableDevice || requested == reader.device) {
            return reader;
        }
    }

    // Keep a disconnected reader reservation so a running stream does not have
    // to be rebuilt merely because USB is temporarily absent.  The UI reports
    // the reader as unavailable until it returns.
    ReaderProfile fallback;
    fallback.key = requested;
    fallback.stableDevice = requested;
    fallback.serial = ttySerialFromPath(requested);
    fallback.displayName = "Phoenix";
    fallback.hardwareStatus = "unavailable";
    fallback.hardwareDetail = "configured reader is not currently detected";
    return applyKnownProfile(std::move(fallback));
}

bool CardManager::reserveService(const StreamConfig& config, std::string* error) {
    if (error) error->clear();
    if (config.conditionalAccessReader.empty()) return true;

    const auto readers = inventory(true);
    const ReaderProfile reader = resolveReader(config.conditionalAccessReader, readers);
    if (reader.key.empty()) {
        if (error) *error = "CA reader path is empty";
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (slotsByStream_.count(config.id)) return true;

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
    slot.lastStatus = reader.externalOwner ? "EXTERNAL_OWNER" : "RESERVED";
    slotsByStream_[config.id] = std::move(slot);
    std::cerr << "CA CardManager: reserved reader=" << reader.key
              << " stream=" << config.id
              << " sid=" << (config.inputServiceId ? config.inputServiceId : config.serviceId)
              << " slots=" << (used + 1) << "/" << reader.maxServices
              << " external_owner=" << (reader.externalOwner ? "yes" : "no")
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
    root["reserved_services"] = Json::UInt(slotsByStream_.size());

    Json::Value readerList(Json::arrayValue);
    for (const auto& [key, reader] : readers) {
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
        item["hardware_status"] = reader.hardwareStatus;
        item["hardware_detail"] = reader.hardwareDetail;
        item["external_owner"] = reader.externalOwner;

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

    // Keep configured-but-temporarily-disconnected readers visible.
    for (const auto& [streamId, slot] : slotsByStream_) {
        (void)streamId;
        if (readers.count(slot.readerKey)) continue;
        Json::Value* existing = nullptr;
        for (auto& item : readerList) {
            if (item.get("key", "").asString() == slot.readerKey) {
                existing = &item;
                break;
            }
        }
        if (!existing) {
            ReaderProfile reader = resolveReader(slot.readerKey, readers);
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
            item["hardware_status"] = "unavailable";
            item["hardware_detail"] = reader.hardwareDetail;
            item["external_owner"] = false;
            item["services"] = Json::Value(Json::arrayValue);
            readerList.append(item);
            existing = &readerList[readerList.size() - 1];
        }
        (*existing)["services"].append(slotToJson(slot));
    }

    // Recalculate counts for disconnected entries too.
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
    const ReaderProfile reader = resolveReader(found->second.readerKey, readers);
    result["reader_display_name"] = reader.displayName;
    result["reader_serial"] = reader.serial;
    result["caid"] = reader.caid;
    result["provider"] = reader.provider;
    result["card_system"] = reader.cardSystem;
    result["hardware_status"] = reader.hardwareStatus;
    result["external_owner"] = reader.externalOwner;
    return result;
}
