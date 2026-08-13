#pragma once

#include <jsoncpp/json/json.h>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include "ConfigManager.h"

// Internal Conditional-Access control plane.
//
// This manager deliberately does not expose a network CA protocol and does not
// export key material.  It owns only reader/service reservations, stable-reader
// identity, per-service state and latency/error telemetry.  A future authorised
// local card backend can use these reservations without changing the DVB/WISI
// transport path.
class CardManager {
public:
    static CardManager& instance();

    bool reserveService(const StreamConfig& config, std::string* error = nullptr);
    void activateService(const std::string& streamId);
    void releaseService(const std::string& streamId);
    void releaseAll();

    // Telemetry hook for an authorised in-process card backend. No payload or
    // key material is accepted here; only timing/result metadata is stored.
    void recordTransaction(const std::string& streamId, uint64_t latencyMs, bool success,
                           const std::string& status = {});

    Json::Value snapshot() const;
    Json::Value streamState(const std::string& streamId) const;

    static constexpr unsigned kDefaultMaxServices = 10;

private:
    CardManager() = default;

    struct ServiceSlot {
        std::string streamId;
        std::string streamName;
        uint32_t serviceId = 0;
        std::string serviceProvider;
        std::string requestedReader;
        std::string readerKey;
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

    struct ReaderProfile {
        std::string key;
        std::string stableDevice;
        std::string device;
        std::string serial;
        std::string displayName;
        std::string cardSystem;
        std::string caid;
        std::string provider;
        unsigned maxServices = kDefaultMaxServices;
        std::string hardwareStatus;
        std::string hardwareDetail;
        bool externalOwner = false;
    };

    static std::string isoTime(const std::chrono::system_clock::time_point& value);
    static ReaderProfile applyKnownProfile(ReaderProfile profile);
    std::map<std::string, ReaderProfile> inventory(bool forceRefresh = false) const;
    static ReaderProfile resolveReader(const std::string& requested,
                                       const std::map<std::string, ReaderProfile>& readers);
    Json::Value slotToJson(const ServiceSlot& slot) const;

    mutable std::mutex mutex_;
    std::map<std::string, ServiceSlot> slotsByStream_;
    mutable std::mutex inventoryMutex_;
    mutable std::map<std::string, ReaderProfile> cachedInventory_;
    mutable std::chrono::steady_clock::time_point lastInventoryRefresh_{};
};
