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

struct CaProviderConfig {
    std::string id;
    std::string name;
    std::string backendType = "external";
    std::string endpoint;
    int maxChannels = 8;
    bool enabled = true;

    Json::Value toJson() const;
    static CaProviderConfig fromJson(const Json::Value& root);
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
    std::string inputMode = "auto";
    bool satelliteEnabled = false;
    int satelliteAdapter = 0;
    int satelliteFrontend = 0;
    uint32_t satelliteFrequency = 0;
    uint32_t satelliteSymbolRate = 27500;
    std::string satellitePolarization = "H";
    std::string satelliteDeliverySystem = "dvb-s2";
    std::string satelliteModulation = "auto";
    std::string satelliteFec = "auto";
    std::string satellitePilot = "auto";
    std::string satelliteRolloff = "auto";
    int satelliteDiseqcSource = -1;
    int satelliteStreamId = -1;
    uint32_t satelliteServiceId = 1;
    uint32_t satelliteLnbLof1 = 9750000;
    uint32_t satelliteLnbLof2 = 10600000;
    uint32_t satelliteLnbSlof = 11700000;
    std::string caProviderId;
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
    uint32_t serviceId = 1;
    std::string serviceName;
    std::string serviceProvider;
    std::vector<StreamOutputConfig> additionalOutputs;

    Json::Value toJson() const;
    static StreamConfig fromJson(const Json::Value& root);
};

struct AppConfig {
    std::string login = "admin";
    std::string password = "admin";
    std::string serverName = "TVStreamer5";
    int httpPort = 9000;
    std::string language = "en";
    std::string telegramToken;
    std::string telegramChatId;
    std::vector<CaProviderConfig> caProviders;
    std::vector<StreamConfig> streams;

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
