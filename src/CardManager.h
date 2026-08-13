#pragma once

#include <jsoncpp/json/json.h>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ConfigManager.h"

// Internal Conditional-Access control plane.
//
// This manager deliberately does not expose a network CA protocol and does not
// export key material. It owns reader/service reservations, stable-reader
// identity, per-reader lifecycle/ATR activation state, service limits and
// latency/error telemetry. The lifecycle monitor only performs the same local
// conservative Phoenix ATR probe used by PhoenixManager; it is not a CA key
// backend and does not implement descrambling by itself.
class CardManager {
public:
    static CardManager& instance();
    ~CardManager();

    // Load persistent per-reader policy after ConfigManager has loaded.
    void configure(const std::vector<CaReaderConfig>& readers);

    bool reserveService(const StreamConfig& config, std::string* error = nullptr);
    void activateService(const std::string& streamId);
    void releaseService(const std::string& streamId);
    void releaseAll();

    // Force a safe local reader re-probe. If another process owns the Phoenix,
    // it is never reset or opened and EXTERNAL_OWNER is returned instead.
    Json::Value reactivateReader(const std::string& readerKey);

    // Telemetry hook for an authorised in-process card backend. No payload or
    // key material is accepted here; only timing/result metadata is stored.
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
        std::string backendId = "passthrough";
        std::string backendConfig = "{}";
        bool autoActivate = true;
        bool autoReactivate = true;
        unsigned retrySeconds = 5;
        std::string hardwareStatus;
        std::string hardwareDetail;
        bool externalOwner = false;
    };

    struct ReaderLifecycle {
        std::string status = "UNKNOWN";
        std::string detail;
        std::string atr;
        std::string detectedCardSystem;
        std::string detectedProvider;
        uint64_t attempts = 0;
        bool probing = false;
        std::chrono::system_clock::time_point lastAttempt{};
        std::chrono::system_clock::time_point lastReady{};
        std::chrono::steady_clock::time_point nextAttempt{};
    };

    static std::string isoTime(const std::chrono::system_clock::time_point& value);
    static ReaderProfile applyKnownProfile(ReaderProfile profile);
    ReaderProfile applyConfiguredPolicy(ReaderProfile profile) const;
    CaReaderConfig configuredPolicy(const ReaderProfile& profile) const;
    std::map<std::string, ReaderProfile> inventory(bool forceRefresh = false) const;
    static ReaderProfile resolveReader(const std::string& requested,
                                       const std::map<std::string, ReaderProfile>& readers);
    Json::Value slotToJson(const ServiceSlot& slot) const;

    void ensureMonitorStarted();
    void monitorLoop();
    Json::Value probeReaderNow(const std::string& readerKey, bool manual);
    void updateLifecycleFromHardware(const ReaderProfile& reader);
    void markMissingReaders(const std::map<std::string, ReaderProfile>& readers);
    void updateSlotLifecycleStatus(const std::string& readerKey, const std::string& status);
    ReaderLifecycle lifecycleFor(const std::string& readerKey) const;

    mutable std::mutex mutex_;
    std::map<std::string, ServiceSlot> slotsByStream_;

    mutable std::mutex inventoryMutex_;
    mutable std::map<std::string, ReaderProfile> cachedInventory_;
    mutable std::chrono::steady_clock::time_point lastInventoryRefresh_{};

    mutable std::mutex configMutex_;
    std::vector<CaReaderConfig> readerConfigs_;

    mutable std::mutex lifecycleMutex_;
    std::map<std::string, ReaderLifecycle> lifecycle_;

    std::atomic<bool> stopMonitor_{false};
    std::atomic<bool> monitorStarted_{false};
    std::thread monitorThread_;
};
