#pragma once

#include <jsoncpp/json/json.h>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

struct StreamOutputConfig {
    std::string outputType = "udp-cbr";
    std::string outputMode = "listener";
    std::string outputHost = "127.0.0.1";
    int outputPort = 1234;

    Json::Value toJson() const;
    static StreamOutputConfig fromJson(const Json::Value& root);
};

struct StreamConfig {
    std::string id;
    std::string name;
    std::string inputUri;
    std::string backupInputUri;
    std::string backupInputType = "url";
    bool backupFileLoop = false;
    std::string outputType = "udp-cbr";
    std::string outputMode = "listener";
    std::string outputHost = "127.0.0.1";
    int outputPort = 1234;
    std::string interfaceAddress;
    std::string inputInterfaceAddress;
    bool inputInterfaceAddressConfigured = false;
    // Optional IPv4 source filter for multicast UDP/RTP input (SSM).
    // Empty = accept any source (ASM, backwards-compatible).
    std::string inputSourceAddress;
    std::string inputMode = "auto";
    // Per-stream HTTP access credentials for HLS manifests, segments and EXT-X-KEY URIs.
    // mode: none | header | query. The value is stored per stream and never shared.
    std::string hlsAccessKeyMode = "none";
    std::string hlsAccessKeyName = "Authorization";
    std::string hlsAccessKeyValue;
    std::string hlsUserAgent = "Mozilla/5.0 TVStreammerSAT5";
    bool testPattern = false;
    bool autoStart = false;
    bool remapEnabled = false;
    bool cbr = true;
    uint64_t targetBitrate = 2000000;
    bool transcodeEnabled = false;
    std::string transcodeResolution = "1920x1080";
    uint64_t transcodeVideoBitrate = 6000000;
    std::string transcodeAudioCodec = "aac";
    uint64_t transcodeAudioBitrate = 192000;
    uint32_t audioPid = 0;
    uint32_t videoPid = 0;
    uint32_t inputServiceId = 0;
    uint32_t serviceId = 1;
    std::string serviceName;
    std::string serviceProvider;
    std::string conditionalAccessClient;
    std::vector<StreamOutputConfig> additionalOutputs;

    Json::Value toJson() const;
    static StreamConfig fromJson(const Json::Value& root);
};


struct CamClientConfig {
    std::string id;
    std::string name;
    unsigned maxServices = 10;
    std::string backendId = "newcamd";
    std::string backendConfig = "{}";

    Json::Value toJson() const;
    static CamClientConfig fromJson(const Json::Value& root);
};

struct AppConfig {
    std::string login = "admin";
    std::string password = "admin";
    std::string serverName = "TVStreammerSAT5";
    int httpPort = 9000;
    std::string language = "en";
    std::string telegramToken;
    std::string telegramChatId;
    std::vector<StreamConfig> streams;
    std::vector<CamClientConfig> camClients;

    Json::Value toJson() const;
    static AppConfig fromJson(const Json::Value& root);
};

struct SubscriberConfig {
    std::string name;
    std::string primaryIp;
    std::string backupIp;
    std::string addedAt;
    bool enabled = true;
    std::vector<std::string> streamIds;

    Json::Value toJson() const;
    static SubscriberConfig fromJson(const Json::Value& root);
};

struct SubscriberListConfig {
    bool filteringEnabled = false;
    std::vector<SubscriberConfig> subscribers;

    Json::Value toJson() const;
    static SubscriberListConfig fromJson(const Json::Value& root);
};

class ConfigManager {
public:
    ConfigManager();
    bool load();
    bool save();
    bool loadSubscribers();
    bool saveSubscribers();

    AppConfig config;
    SubscriberListConfig subscribers;

private:
    std::filesystem::path configPath;
    std::mutex fileMutex;
};
