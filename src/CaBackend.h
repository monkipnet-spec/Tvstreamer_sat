#pragma once

#include <jsoncpp/json/json.h>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "ConfigManager.h"
#include "ca/CaBackendPluginApi.h"

struct CaBackendReaderBinding {
    std::string key;
    std::string stableDevice;
    std::string device;
    std::string serial;
    std::string displayName;
    std::string cardSystem;
    std::string caid;
    std::string provider;
    unsigned maxServices = 10;
    std::string backendId = "passthrough";
    std::string backendConfig = "{}";
};

class CaBackendManager {
public:
    static CaBackendManager& instance();
    ~CaBackendManager();

    CaBackendManager(const CaBackendManager&) = delete;
    CaBackendManager& operator=(const CaBackendManager&) = delete;

    // Reload discoverable .so plugins and keep persisted CAM-client policy.
    void configure(const std::vector<CamClientConfig>& clients);

    bool startService(const StreamConfig& stream,
                      const CaBackendReaderBinding& reader,
                      std::string* error = nullptr);
    void markServiceActive(const std::string& streamId);
    void stopService(const std::string& streamId);
    void stopAll();

    // MPEG-TS processing hook. The call is a no-op for the built-in
    // passthrough backend. No raw control-word interface exists in this API.
    bool processTransport(const std::string& streamId, uint8_t* data, size_t size);

    Json::Value streamState(const std::string& streamId) const;
    Json::Value snapshot() const;

    static constexpr const char* kDefaultPluginDirectory = "/opt/tvstreammersat5/ca-plugins";

private:
    CaBackendManager();

    struct LoadedBackend {
        std::string id;
        std::string displayName;
        std::string vendor;
        std::string path;
        uint32_t capabilities = 0;
        bool builtin = false;
        bool usable = false;
        std::string loadError;
        void* library = nullptr;
        const tvs_ca_backend_api_v1* api = nullptr;
        void* instance = nullptr;
        std::map<std::string, unsigned> readerRefs;
    };

    struct ServiceSession {
        std::string streamId;
        std::string streamName;
        std::string readerKey;
        std::string backendId;
        bool active = false;
        bool passthrough = true;
        uint64_t calls = 0;
        uint64_t bytes = 0;
        uint64_t errors = 0;
        uint64_t retries = 0;
        uint64_t packetsSeen = 0;
        uint64_t packetsChanged = 0;
        uint64_t packetsClear = 0;
        uint64_t packetsScrambled = 0;
        std::string status = "RESERVED";
        std::string lastError;
    };

    static void hostLog(int level, const char* backendId, const char* message);
    static uint64_t hostMonotonicMs();
    static std::string extractDvbPids(const std::string& uri);
    static std::string safeString(const char* value);

    void unloadPluginsLocked();
    void loadPluginsLocked();
    void loadPluginFileLocked(const std::string& path);
    LoadedBackend* findBackendLocked(const std::string& id);
    const LoadedBackend* findBackendLocked(const std::string& id) const;
    bool ensureReaderOpenLocked(LoadedBackend& backend,
                                const CaBackendReaderBinding& reader,
                                std::string& error);
    void releaseReaderLocked(LoadedBackend& backend, const std::string& readerKey);
    Json::Value backendToJsonLocked(const LoadedBackend& backend) const;

    mutable std::mutex mutex_;
    std::map<std::string, LoadedBackend> backends_;
    std::map<std::string, ServiceSession> sessions_;
    std::vector<CamClientConfig> clientPolicies_;
    tvs_ca_host_api_v1 hostApi_{};
};
