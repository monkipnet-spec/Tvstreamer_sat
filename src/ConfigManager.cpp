#include "ConfigManager.h"
#include "utils.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>


namespace {

bool usesUdpEndpointFields(const std::string& outputType) {
    const std::string type = toLower(outputType);
    return type == "udp" || type == "udp-cbr" || type == "udp-vbr" || type == "rtp";
}

void normalizeOutputEndpoint(std::string& outputHost, int& outputPort, const std::string& outputType) {
    if (!usesUdpEndpointFields(outputType)) return;
    std::string normalizedHost;
    int normalizedPort = outputPort;
    if (normalizeUdpEndpoint(outputHost, outputPort, normalizedHost, normalizedPort)) {
        outputHost = normalizedHost;
        outputPort = normalizedPort;
    }
}

} // namespace

StreamOutputConfig StreamOutputConfig::fromJson(const Json::Value& root) {
    StreamOutputConfig output;
    output.outputType = root.get("output_type", "udp-cbr").asString();
    output.outputMode = root.get("output_mode", "listener").asString();
    output.outputHost = root.get("output_host", "127.0.0.1").asString();
    output.outputPort = root.get("output_port", 1234).asInt();
    normalizeOutputEndpoint(output.outputHost, output.outputPort, output.outputType);
    if (toLower(output.outputType) == "srt" && (!root.isMember("output_port") || output.outputPort <= 0 || output.outputPort > 65535)) {
        output.outputPort = 7001;
    }
    return output;
}

Json::Value StreamOutputConfig::toJson() const {
    Json::Value root;
    root["output_type"] = outputType;
    root["output_mode"] = outputMode;
    root["output_host"] = outputHost;
    root["output_port"] = outputPort;
    return root;
}

StreamConfig StreamConfig::fromJson(const Json::Value& root) {
    StreamConfig config;
    config.id = root.get("id", "").asString();
    config.name = root.get("name", "").asString();
    config.inputUri = root.get("input_uri", "").asString();
    config.backupInputUri = root.get("backup_input_uri", "").asString();
    config.backupInputType = root.get("backup_input_type", "").asString();
    if (config.backupInputType.empty()) {
        const std::string backupUri = config.backupInputUri;
        config.backupInputType =
            backupUri.empty()
                ? "url"
                : (backupUri.rfind("file://", 0) == 0 || backupUri.find("://") == std::string::npos
                ? "file"
                : "url");
    }
    config.backupFileLoop = root.get("backup_file_loop", false).asBool();
    config.outputType = root.get("output_type", "udp-cbr").asString();
    config.outputMode = root.get("output_mode", "listener").asString();
    config.outputHost = root.get("output_host", "127.0.0.1").asString();
    config.outputPort = root.get("output_port", 1234).asInt();
    normalizeOutputEndpoint(config.outputHost, config.outputPort, config.outputType);
    if (toLower(config.outputType) == "srt" && (!root.isMember("output_port") || config.outputPort <= 0 || config.outputPort > 65535)) {
        config.outputPort = 7001;
    }
    config.interfaceAddress = root.get("interface_address", "").asString();
    config.inputInterfaceAddressConfigured = root.isMember("input_interface_address");
    config.inputInterfaceAddress = root.get("input_interface_address", "").asString();
    config.inputMode = root.get("input_mode", "auto").asString();
    config.hlsAccessKeyMode = root.get("hls_access_key_mode", "none").asString();
    if (config.hlsAccessKeyMode != "header" && config.hlsAccessKeyMode != "query") {
        config.hlsAccessKeyMode = "none";
    }
    config.hlsAccessKeyName = root.get("hls_access_key_name",
        config.hlsAccessKeyMode == "query" ? "token" : "Authorization").asString();
    config.hlsAccessKeyValue = root.get("hls_access_key_value", "").asString();
    config.hlsUserAgent = root.get("hls_user_agent", "Mozilla/5.0 TVStreammerSAT5").asString();
    config.testPattern = root.get("test_pattern", false).asBool();
    config.autoStart = root.get("auto_start", false).asBool();
    config.remapEnabled = root.get("remap_enabled", false).asBool();
    config.cbr = root.get("cbr", true).asBool();
    config.targetBitrate = root.get("target_bitrate", Json::UInt64(2000000)).asUInt64();
    config.transcodeEnabled = root.get("transcode_enabled", false).asBool();
    config.transcodeResolution = root.get("transcode_resolution", "1920x1080").asString();
    config.transcodeVideoBitrate = root.get("transcode_video_bitrate", Json::UInt64(6000000)).asUInt64();
    config.transcodeAudioCodec = root.get("transcode_audio_codec", "aac").asString();
    if (config.transcodeAudioCodec != "aac" && config.transcodeAudioCodec != "mp3" &&
        config.transcodeAudioCodec != "copy") {
        config.transcodeAudioCodec = "aac";
    }
    config.transcodeAudioBitrate = root.get("transcode_audio_bitrate", Json::UInt64(192000)).asUInt64();
    config.transcodeAudioBitrate = std::clamp<uint64_t>(config.transcodeAudioBitrate, 64000, 320000);
    config.audioPid = root.get("audio_pid", 0).asUInt();
    config.videoPid = root.get("video_pid", 0).asUInt();
    config.serviceId = root.get("service_id", 1).asUInt();
    // input_service_id=0 means automatic PAT-based service detection.
    // Existing configurations that predate input_service_id keep their previous
    // explicit behaviour by inheriting service_id; newly created streams use 0.
    config.inputServiceId = root.isMember("input_service_id")
        ? root.get("input_service_id", 0).asUInt()
        : config.serviceId;
    config.serviceName = root.get("service_name", "").asString();
    config.serviceProvider = root.get("service_provider", "").asString();
    config.conditionalAccessClient = root.get("conditional_access_client", "").asString();
    if (root.isMember("outputs") && root["outputs"].isArray() && root["outputs"].size() > 0) {
        const auto primary = StreamOutputConfig::fromJson(root["outputs"][0]);
        config.outputType = primary.outputType;
        config.outputMode = primary.outputMode;
        config.outputHost = primary.outputHost;
        config.outputPort = primary.outputPort;
        for (Json::ArrayIndex i = 1; i < root["outputs"].size(); ++i) {
            config.additionalOutputs.push_back(StreamOutputConfig::fromJson(root["outputs"][i]));
        }
    } else if (root.isMember("additional_outputs") && root["additional_outputs"].isArray()) {
        for (const auto& item : root["additional_outputs"]) {
            config.additionalOutputs.push_back(StreamOutputConfig::fromJson(item));
        }
    }
    return config;
}

Json::Value StreamConfig::toJson() const {
    Json::Value root;
    root["id"] = id;
    root["name"] = name;
    root["input_uri"] = inputUri;
    root["backup_input_uri"] = backupInputUri;
    root["backup_input_type"] = backupInputType;
    root["backup_file_loop"] = backupFileLoop;
    root["output_type"] = outputType;
    root["output_mode"] = outputMode;
    root["output_host"] = outputHost;
    root["output_port"] = outputPort;
    root["interface_address"] = interfaceAddress;
    if (inputInterfaceAddressConfigured) {
        root["input_interface_address"] = inputInterfaceAddress;
    }
    root["input_mode"] = inputMode;
    root["hls_access_key_mode"] = hlsAccessKeyMode;
    root["hls_access_key_name"] = hlsAccessKeyName;
    root["hls_access_key_value"] = hlsAccessKeyValue;
    root["hls_user_agent"] = hlsUserAgent;
    root["test_pattern"] = testPattern;
    root["auto_start"] = autoStart;
    root["remap_enabled"] = remapEnabled;
    root["cbr"] = cbr;
    root["target_bitrate"] = Json::UInt64(targetBitrate);
    root["transcode_enabled"] = transcodeEnabled;
    root["transcode_resolution"] = transcodeResolution;
    root["transcode_video_bitrate"] = Json::UInt64(transcodeVideoBitrate);
    root["transcode_audio_codec"] = transcodeAudioCodec;
    root["transcode_audio_bitrate"] = Json::UInt64(transcodeAudioBitrate);
    root["audio_pid"] = audioPid;
    root["video_pid"] = videoPid;
    root["input_service_id"] = inputServiceId;
    root["service_id"] = serviceId;
    root["service_name"] = serviceName;
    root["service_provider"] = serviceProvider;
    root["conditional_access_client"] = conditionalAccessClient;
    Json::Value extraOutputs(Json::arrayValue);
    for (const auto& output : additionalOutputs) {
        extraOutputs.append(output.toJson());
    }
    root["additional_outputs"] = extraOutputs;
    return root;
}

CamClientConfig CamClientConfig::fromJson(const Json::Value& root) {
    CamClientConfig config;
    config.id = root.get("id", "").asString();
    config.name = root.get("name", config.id).asString();
    config.maxServices = std::clamp(root.get("max_services", 10).asUInt(), 1u, 64u);
    config.backendId = root.get("backend_id", "newcamd").asString();
    if (config.backendId.empty() || config.backendId == "passthrough") config.backendId = "newcamd";
    config.backendConfig = root.get("backend_config", "{}").asString();
    if (config.backendConfig.empty()) config.backendConfig = "{}";
    return config;
}

Json::Value CamClientConfig::toJson() const {
    Json::Value root;
    root["id"] = id;
    root["name"] = name.empty() ? id : name;
    root["max_services"] = std::clamp(maxServices, 1u, 64u);
    root["backend_id"] = backendId.empty() ? "newcamd" : backendId;
    root["backend_config"] = backendConfig.empty() ? "{}" : backendConfig;
    return root;
}
Json::Value AppConfig::toJson() const {
    Json::Value root;
    root["login"] = login;
    root["password"] = password;
    root["server_name"] = serverName;
    root["http_port"] = httpPort;
    root["language"] = language;
    root["telegram_token"] = telegramToken;
    root["telegram_chat_id"] = telegramChatId;
    Json::Value camClientsJson(Json::arrayValue);
    for (const auto& client : camClients) camClientsJson.append(client.toJson());
    root["cam_clients"] = camClientsJson;
    Json::Value list(Json::arrayValue);
    for (const auto& stream : streams) {
        list.append(stream.toJson());
    }
    root["streams"] = list;
    return root;
}

AppConfig AppConfig::fromJson(const Json::Value& root) {
    AppConfig config;
    config.login = root.get("login", "admin").asString();
    config.password = root.get("password", "admin").asString();
    config.serverName = root.get("server_name", "TVStreammerSAT5").asString();
    config.httpPort = root.get("http_port", 9000).asInt();
    config.language = root.get("language", "en").asString();
    if (config.language != "ru" && config.language != "en") {
        config.language = "en";
    }
    config.telegramToken = root.get("telegram_token", "").asString();
    config.telegramChatId = root.get("telegram_chat_id", "").asString();
    if (root.isMember("cam_clients") && root["cam_clients"].isArray()) {
        for (const auto& item : root["cam_clients"]) config.camClients.push_back(CamClientConfig::fromJson(item));
    }
    if (root.isMember("streams") && root["streams"].isArray()) {
        for (const auto& item : root["streams"]) {
            config.streams.push_back(StreamConfig::fromJson(item));
        }
    }
    return config;
}

ConfigManager::ConfigManager() {
    configPath = std::filesystem::current_path() / "tvstreammersat5-config.json";
}

Json::Value SubscriberConfig::toJson() const {
    Json::Value root;
    root["name"] = name;
    root["primary_ip"] = primaryIp;
    root["backup_ip"] = backupIp;
    root["added_at"] = addedAt;
    root["enabled"] = enabled;
    Json::Value streams(Json::arrayValue);
    for (const auto& id : streamIds) streams.append(id);
    root["stream_ids"] = streams;
    return root;
}

SubscriberConfig SubscriberConfig::fromJson(const Json::Value& root) {
    SubscriberConfig subscriber;
    subscriber.name = root.get("name", "").asString();
    subscriber.primaryIp = root.get("primary_ip", "").asString();
    subscriber.backupIp = root.get("backup_ip", "").asString();
    subscriber.addedAt = root.get("added_at", "").asString();
    subscriber.enabled = root.get("enabled", true).asBool();
    if (root["stream_ids"].isArray()) {
        for (const auto& id : root["stream_ids"]) {
            subscriber.streamIds.push_back(id.asString());
        }
    }
    return subscriber;
}

Json::Value SubscriberListConfig::toJson() const {
    Json::Value root;
    root["filtering_enabled"] = filteringEnabled;
    Json::Value list(Json::arrayValue);
    for (const auto& subscriber : subscribers) {
        list.append(subscriber.toJson());
    }
    root["subscribers"] = list;
    return root;
}

SubscriberListConfig SubscriberListConfig::fromJson(const Json::Value& root) {
    SubscriberListConfig config;
    config.filteringEnabled = root.get("filtering_enabled", false).asBool();
    if (root["subscribers"].isArray()) {
        for (const auto& item : root["subscribers"]) {
            config.subscribers.push_back(SubscriberConfig::fromJson(item));
        }
    }
    return config;
}

bool ConfigManager::load() {
    if (!std::filesystem::exists(configPath)) {
        std::cerr << "Config file not found, creating default configuration: " << configPath << std::endl;
        AppConfig defaultConfig;
        {
            std::lock_guard<std::mutex> lock(fileMutex);
            config = defaultConfig;
        }
        if (!save()) return false;
        return loadSubscribers();
    }

    {
        std::lock_guard<std::mutex> lock(fileMutex);
        std::ifstream input(configPath);
        if (!input.is_open()) {
            return false;
        }
        Json::Value root;
        Json::CharReaderBuilder readerBuilder;
        std::string errs;
        bool ok = Json::parseFromStream(readerBuilder, input, &root, &errs);
        if (!ok) {
            std::cerr << "Failed to parse config: " << errs << std::endl;
            return false;
        }
        config = AppConfig::fromJson(root);
    }
    return loadSubscribers();
}

bool ConfigManager::loadSubscribers() {
    const auto path = std::filesystem::current_path() / "tvstreammersat5-subscribers.json";
    if (!std::filesystem::exists(path)) {
        subscribers = SubscriberListConfig{};
        return saveSubscribers();
    }
    std::lock_guard<std::mutex> lock(fileMutex);
    std::ifstream input(path);
    if (!input.is_open()) return false;
    Json::Value root;
    Json::CharReaderBuilder readerBuilder;
    std::string errs;
    if (!Json::parseFromStream(readerBuilder, input, &root, &errs)) {
        std::cerr << "Failed to parse subscribers config: " << errs << std::endl;
        return false;
    }
    subscribers = SubscriberListConfig::fromJson(root);
    return true;
}

bool ConfigManager::save() {
    std::lock_guard<std::mutex> lock(fileMutex);
    std::ofstream output(configPath);
    if (!output.is_open()) {
        std::cerr << "Unable to open config file for writing: " << configPath << std::endl;
        return false;
    }
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    std::string str = Json::writeString(writer, config.toJson());
    output << str;
    return true;
}

bool ConfigManager::saveSubscribers() {
    std::lock_guard<std::mutex> lock(fileMutex);
    const auto path = std::filesystem::current_path() / "tvstreammersat5-subscribers.json";
    std::ofstream output(path);
    if (!output.is_open()) return false;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    output << Json::writeString(writer, subscribers.toJson());
    return true;
}
