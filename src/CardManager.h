#pragma once

#include <jsoncpp/json/json.h>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "ConfigManager.h"

// Internal Conditional-Access control plane for CAM clients.
//
// The manager owns CAM-client service reservations and plugin telemetry. It has
// no local serial-reader inventory and no provider-card lifecycle path.
class CardManager {
public:
    static CardManager& instance();

    void configure(const std::vector<CamClientConfig>& clients);

    bool reserveService(const StreamConfig& config, std::string* error = nullptr);
    void activateService(const std::string& streamId);
    void releaseService(const std::string& streamId);
    void releaseAll();

    void recordTransaction(const std::string& streamId, uint64_t latencyMs, bool success,
                           const std::string& status = {});

    Json::Value snapshot() const;
    Json::Value streamState(const std::string& streamId) const;

    static constexpr unsigned kDefaultMaxServices = 10;
    static constexpr unsigned kMaxConfigurableServices = 64;

private:
    CardManager() = default;
    CardManager(const CardManager&) = delete;
    CardManager& operator=(const CardManager&) = delete;

    struct ServiceSlot {
        std::string streamId;
        std::string streamName;
        uint32_t serviceId = 0;
        std::string serviceProvider;
        std::string requestedClient;
        std::string clientId;
        std::string clientName;
        std::string backendId;
        bool active = false;
        uint64_t transactions = 0;
        uint64_t successfulTransactions = 0;
        uint64_t failedTransactions = 0;
        uint64_t lastLatencyMs = 0;
        uint64_t maxLatencyMs = 0;
        double averageLatencyMs = 0.0;
        std::string lastStatus;
        std::chrono::system_clock::time_point reservedAt = std::chrono::system_clock::now();
        std::chrono::system_clock::time_point lastTransactionAt{};
    };

    static std::string isoTime(const std::chrono::system_clock::time_point& value);
    static Json::Value clientStatusJson(const CamClientConfig& client);
    static std::string displayName(const CamClientConfig& client);
    CamClientConfig findClient(const std::string& requested) const;
    Json::Value slotToJson(const ServiceSlot& slot) const;

    mutable std::mutex mutex_;
    std::map<std::string, ServiceSlot> slotsByStream_;

    mutable std::mutex configMutex_;
    std::vector<CamClientConfig> clients_;
};