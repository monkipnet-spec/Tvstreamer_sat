#include "CardManager.h"

#include "CaBackend.h"

#include <algorithm>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace {

Json::Value parseBackendConfig(const std::string& value) {
    Json::Value parsed;
    Json::CharReaderBuilder builder;
    std::string errors;
    std::istringstream input(value.empty() ? "{}" : value);
    if (!Json::parseFromStream(builder, input, &parsed, &errors)) return Json::Value(Json::objectValue);
    return parsed;
}

std::string endpointText(const Json::Value& config) {
    const std::string host = config.get("host", "").asString();
    const int port = config.get("port", 0).asInt();
    if (host.empty() || port <= 0) return {};
    return host + ":" + std::to_string(port);
}

} // namespace

CardManager& CardManager::instance() {
    static CardManager manager;
    return manager;
}

void CardManager::configure(const std::vector<CamClientConfig>& clients) {
    std::vector<CamClientConfig> normalized;
    normalized.reserve(clients.size());
    for (auto client : clients) {
        if (client.id.empty()) continue;
        client.maxServices = std::clamp(client.maxServices, 1u, kMaxConfigurableServices);
        if (client.name.empty()) client.name = client.id;
        if (client.backendId.empty() || client.backendId == "passthrough") client.backendId = "newcamd";
        if (client.backendConfig.empty()) client.backendConfig = "{}";
        normalized.push_back(std::move(client));
    }
    std::vector<CamClientConfig> configured;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        clients_ = std::move(normalized);
        configured = clients_;
    }
    CaBackendManager::instance().configure(configured);
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

std::string CardManager::displayName(const CamClientConfig& client) {
    return client.name.empty() ? client.id : client.name;
}

CamClientConfig CardManager::findClient(const std::string& requested) const {
    if (requested.empty()) return {};
    std::lock_guard<std::mutex> lock(configMutex_);
    for (const auto& client : clients_) {
        if (client.id == requested || client.name == requested) return client;
    }
    return {};
}

Json::Value CardManager::clientStatusJson(const CamClientConfig& client) {
    Json::Value item;
    item["id"] = client.id;
    item["name"] = displayName(client);
    item["max_services"] = std::clamp(client.maxServices, 1u, kMaxConfigurableServices);
    item["backend_id"] = client.backendId.empty() ? "newcamd" : client.backendId;
    const Json::Value backendConfig = parseBackendConfig(client.backendConfig);
    item["configured"] = !client.id.empty();
    item["endpoint"] = endpointText(backendConfig);
    item["host"] = backendConfig.get("host", "").asString();
    item["port"] = backendConfig.get("port", 0).asInt();
    item["user"] = backendConfig.get("user", "").asString();
    item["des_configured"] = !backendConfig.get("des", "").asString().empty();
    item["status"] = item["endpoint"].asString().empty() ? "NOT_CONFIGURED" : "CONFIGURED";
    return item;
}

bool CardManager::reserveService(const StreamConfig& config, std::string* error) {
    if (error) error->clear();
    if (config.conditionalAccessClient.empty()) return true;
    if (config.conditionalAccessClient == "auto") {
        if (error) *error = "CAM auto routing is disabled: select a CAM client for this channel";
        return false;
    }

    CamClientConfig client = findClient(config.conditionalAccessClient);
    if (client.id.empty()) {
        if (error) *error = "CAM client is not configured: " + config.conditionalAccessClient;
        return false;
    }
    client.maxServices = std::clamp(client.maxServices, 1u, kMaxConfigurableServices);
    if (client.backendId.empty()) client.backendId = "newcamd";
    if (client.backendConfig.empty()) client.backendConfig = "{}";

    std::lock_guard<std::mutex> lock(mutex_);
    if (slotsByStream_.count(config.id)) return true;

    const auto used = static_cast<unsigned>(std::count_if(
        slotsByStream_.begin(), slotsByStream_.end(), [&](const auto& entry) {
            return entry.second.clientId == client.id;
        }));
    if (used >= client.maxServices) {
        if (error) {
            *error = "CAM client service limit reached for " + client.id + ": " +
                     std::to_string(used) + "/" + std::to_string(client.maxServices);
        }
        return false;
    }

    CaBackendReaderBinding backendClient;
    backendClient.key = client.id;
    backendClient.displayName = displayName(client);
    backendClient.maxServices = client.maxServices;
    backendClient.backendId = client.backendId;
    backendClient.backendConfig = client.backendConfig;

    std::string backendError;
    if (!CaBackendManager::instance().startService(config, backendClient, &backendError)) {
        if (error) *error = backendError.empty() ? "failed to reserve CAM backend service" : backendError;
        std::cerr << "CAM manager: backend reservation rejected client=" << client.id
                  << " stream=" << config.id
                  << " backend=" << client.backendId
                  << " error=" << backendError << std::endl;
        return false;
    }

    ServiceSlot slot;
    slot.streamId = config.id;
    slot.streamName = config.name;
    slot.serviceId = config.inputServiceId ? config.inputServiceId : config.serviceId;
    slot.serviceProvider = config.serviceProvider;
    slot.requestedClient = config.conditionalAccessClient;
    slot.clientId = client.id;
    slot.clientName = displayName(client);
    slot.backendId = client.backendId;
    slot.lastStatus = "RESERVED";
    slotsByStream_[config.id] = std::move(slot);

    std::cerr << "CAM manager: reserved client=" << client.id
              << " stream=" << config.id
              << " sid=" << (config.inputServiceId ? config.inputServiceId : config.serviceId)
              << " slots=" << (used + 1) << "/" << client.maxServices
              << " backend=" << client.backendId << std::endl;
    return true;
}

void CardManager::activateService(const std::string& streamId) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = slotsByStream_.find(streamId);
        if (found == slotsByStream_.end()) return;
        found->second.active = true;
        if (found->second.lastStatus.empty() || found->second.lastStatus == "RESERVED") {
            found->second.lastStatus = "ACTIVE";
        }
        std::cerr << "CAM manager: active client=" << found->second.clientId
                  << " stream=" << found->second.streamId
                  << " sid=" << found->second.serviceId << std::endl;
    }
    CaBackendManager::instance().markServiceActive(streamId);
}

void CardManager::releaseService(const std::string& streamId) {
    bool existed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = slotsByStream_.find(streamId);
        if (found == slotsByStream_.end()) return;
        std::cerr << "CAM manager: released client=" << found->second.clientId
                  << " stream=" << found->second.streamId
                  << " sid=" << found->second.serviceId << std::endl;
        slotsByStream_.erase(found);
        existed = true;
    }
    if (existed) CaBackendManager::instance().stopService(streamId);
}

void CardManager::releaseAll() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        slotsByStream_.clear();
    }
    CaBackendManager::instance().stopAll();
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
    item["client"] = slot.clientId;
    item["client_name"] = slot.clientName;
    item["requested_client"] = slot.requestedClient;
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
    item["backend_id"] = slot.backendId;
    item["backend"] = CaBackendManager::instance().streamState(slot.streamId);
    return item;
}

Json::Value CardManager::snapshot() const {
    std::vector<CamClientConfig> clients;
    {
        std::lock_guard<std::mutex> configLock(configMutex_);
        clients = clients_;
    }
    std::lock_guard<std::mutex> lock(mutex_);

    Json::Value root;
    root["mode"] = "cam-client+plugin-backend";
    root["ca_backend"] = CaBackendManager::instance().snapshot();
    root["network_ca_server"] = false;
    root["external_key_export"] = false;
    root["default_max_services"] = kDefaultMaxServices;
    root["max_configurable_services"] = kMaxConfigurableServices;
    root["reserved_services"] = Json::UInt(slotsByStream_.size());

    Json::Value clientList(Json::arrayValue);
    for (const auto& client : clients) {
        Json::Value item = clientStatusJson(client);
        Json::Value services(Json::arrayValue);
        unsigned active = 0;
        for (const auto& [streamId, slot] : slotsByStream_) {
            (void)streamId;
            if (slot.clientId != client.id) continue;
            services.append(slotToJson(slot));
            if (slot.active) ++active;
        }
        const unsigned maxServices = item.get("max_services", kDefaultMaxServices).asUInt();
        const unsigned used = services.size();
        item["services"] = services;
        item["services_used"] = used;
        item["services_active"] = active;
        item["services_free"] = maxServices > used ? maxServices - used : 0;
        clientList.append(item);
    }
    root["clients"] = clientList;
    return root;
}

Json::Value CardManager::streamState(const std::string& streamId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value result;
    const auto found = slotsByStream_.find(streamId);
    if (found == slotsByStream_.end()) {
        result["managed"] = false;
        result["status"] = "UNBOUND";
        result["backend"] = CaBackendManager::instance().streamState(streamId);
        result["native_card_backend"] = false;
        return result;
    }
    result = slotToJson(found->second);
    result["managed"] = true;
    const Json::Value backendState = CaBackendManager::instance().streamState(streamId);
    result["backend"] = backendState;
    result["native_card_backend"] = backendState.get("native_plugin", false).asBool();
    result["client_display_name"] = found->second.clientName;
    result["backend_id"] = found->second.backendId;
    return result;
}