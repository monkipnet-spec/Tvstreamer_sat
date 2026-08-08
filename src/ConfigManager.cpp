#include "ConfigManager.h"
#include "utils.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <iomanip>

StreamOutputConfig StreamOutputConfig::fromJson(const Json::Value& root) {
    StreamOutputConfig output;
    output.outputType = root.get("output_type", "udp-cbr").asString();
    output.outputMode = root.get("output_mode", "listener").asString();
    output.outputHost = root.get("output_host", "127.0.0.1").asString();
    output.outputPort = root.get("output_port", 1234).asInt();
    return output;
}


CaProviderConfig CaProviderConfig::fromJson(const Json::Value& root) {
    CaProviderConfig config;
    config.id = root.get("id", "").asString();
    config.name = root.get("name", config.id).asString();
    config.backendType = root.get("backend_type", "external").asString();
    if (config.backendType.empty()) config.backendType = "external";
    config.endpoint = root.get("endpoint", "").asString();
    config.maxChannels = std::clamp(root.get("max_channels", 8).asInt(), 1, 64);
    config.enabled = root.get("enabled", true).asBool();
    return config;
}

Json::Value CaProviderConfig::toJson() const {
    Json::Value root;
    root["id"] = id;
    root["name"] = name;
    root["backend_type"] = backendType;
    root["endpoint"] = endpoint;
    root["max_channels"] = std::clamp(maxChannels, 1, 64);
    root["enabled"] = enabled;
    return root;
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
    config.interfaceAddress = root.get("interface_address", "").asString();
    config.inputInterfaceAddressConfigured = root.isMember("input_interface_address");
    config.inputInterfaceAddress = root.get("input_interface_address", "").asString();
    config.inputMode = root.get("input_mode", "auto").asString();
    config.satelliteEnabled = root.get("satellite_enabled", false).asBool();
    config.satelliteAdapter = std::max(0, root.get("satellite_adapter", 0).asInt());
    config.satelliteFrontend = std::max(0, root.get("satellite_frontend", 0).asInt());
    config.satelliteFrequency = root.get("satellite_frequency", Json::UInt(0)).asUInt();
    config.satelliteSymbolRate = root.get("satellite_symbol_rate", Json::UInt(27500)).asUInt();
    config.satellitePolarization = root.get("satellite_polarization", "H").asString();
    if (config.satellitePolarization != "H" && config.satellitePolarization != "V" &&
        config.satellitePolarization != "h" && config.satellitePolarization != "v") {
        config.satellitePolarization = "H";
    }
    config.satelliteDeliverySystem = root.get("satellite_delivery_system", "dvb-s2").asString();
    if (config.satelliteDeliverySystem != "dvb-s" && config.satelliteDeliverySystem != "dvb-s2") {
        config.satelliteDeliverySystem = "dvb-s2";
    }
    config.satelliteModulation = root.get("satellite_modulation", "auto").asString();
    config.satelliteFec = root.get("satellite_fec", "auto").asString();
    config.satellitePilot = root.get("satellite_pilot", "auto").asString();
    config.satelliteRolloff = root.get("satellite_rolloff", "auto").asString();
    config.satelliteDiseqcSource = std::clamp(root.get("satellite_diseqc_source", -1).asInt(), -1, 7);
    config.satelliteStreamId = std::clamp(root.get("satellite_stream_id", -1).asInt(), -1, 255);
    config.satelliteServiceId = root.get("satellite_service_id", Json::UInt(1)).asUInt();
    config.satelliteLnbLof1 = root.get("satellite_lnb_lof1", Json::UInt(9750000)).asUInt();
    config.satelliteLnbLof2 = root.get("satellite_lnb_lof2", Json::UInt(10600000)).asUInt();
    config.satelliteLnbSlof = root.get("satellite_lnb_slof", Json::UInt(11700000)).asUInt();
    config.caProviderId = root.get("ca_provider_id", "").asString();
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
    config.serviceName = root.get("service_name", "").asString();
    config.serviceProvider = root.get("service_provider", "").asString();
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
    root["satellite_enabled"] = satelliteEnabled;
    root["satellite_adapter"] = satelliteAdapter;
    root["satellite_frontend"] = satelliteFrontend;
    root["satellite_frequency"] = satelliteFrequency;
    root["satellite_symbol_rate"] = satelliteSymbolRate;
    root["satellite_polarization"] = satellitePolarization;
    root["satellite_delivery_system"] = satelliteDeliverySystem;
    root["satellite_modulation"] = satelliteModulation;
    root["satellite_fec"] = satelliteFec;
    root["satellite_pilot"] = satellitePilot;
    root["satellite_rolloff"] = satelliteRolloff;
    root["satellite_diseqc_source"] = satelliteDiseqcSource;
    root["satellite_stream_id"] = satelliteStreamId;
    root["satellite_service_id"] = satelliteServiceId;
    root["satellite_lnb_lof1"] = satelliteLnbLof1;
    root["satellite_lnb_lof2"] = satelliteLnbLof2;
    root["satellite_lnb_slof"] = satelliteLnbSlof;
    root["ca_provider_id"] = caProviderId;
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
    root["service_id"] = serviceId;
    root["service_name"] = serviceName;
    root["service_provider"] = serviceProvider;
    Json::Value extraOutputs(Json::arrayValue);
    for (const auto& output : additionalOutputs) {
        extraOutputs.append(output.toJson());
    }
    root["additional_outputs"] = extraOutputs;
    return root;
}

Json::Value AppConfig::toJson() const {
    Json::Value root;
    root["login"] = login;
    const std::string storedPassword = normalizeMd5Password(password);
    root["password_md5"] = storedPassword.rfind("md5:", 0) == 0 ? storedPassword.substr(4) : storedPassword;
    root["server_name"] = serverName;
    root["http_port"] = httpPort;
    root["language"] = language;
    root["telegram_token"] = telegramToken;
    root["telegram_chat_id"] = telegramChatId;
    Json::Value providers(Json::arrayValue);
    for (const auto& provider : caProviders) {
        providers.append(provider.toJson());
    }
    root["ca_providers"] = providers;
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
    if (root.isMember("password_md5") && !root.get("password_md5", "").asString().empty()) {
        config.password = normalizeMd5Password(root.get("password_md5", "").asString());
    } else {
        const std::string legacyPassword = root.get("password", "admin").asString();
        config.password = legacyPassword.rfind("md5:", 0) == 0
            ? normalizeMd5Password(legacyPassword)
            : "md5:" + md5Hex(legacyPassword);
    }
    config.serverName = root.get("server_name", "TVStreamer5").asString();
    config.httpPort = root.get("http_port", 9000).asInt();
    config.language = root.get("language", "en").asString();
    if (config.language != "ru" && config.language != "en") {
        config.language = "en";
    }
    config.telegramToken = root.get("telegram_token", "").asString();
    config.telegramChatId = root.get("telegram_chat_id", "").asString();
    if (root.isMember("ca_providers") && root["ca_providers"].isArray()) {
        for (const auto& item : root["ca_providers"]) {
            CaProviderConfig provider = CaProviderConfig::fromJson(item);
            if (!provider.id.empty()) config.caProviders.push_back(provider);
        }
    }
    if (root.isMember("streams") && root["streams"].isArray()) {
        for (const auto& item : root["streams"]) {
            config.streams.push_back(StreamConfig::fromJson(item));
        }
    }
    return config;
}

ConfigManager::ConfigManager() {
    configPath = std::filesystem::current_path() / "tvstreamer5-config.json";
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
            defaultConfig.password = normalizeMd5Password(defaultConfig.password);
            config = defaultConfig;
        }
        if (!save()) return false;
        return loadSubscribers();
    }

    bool needsPasswordMigration = false;
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
        needsPasswordMigration = !root.isMember("password_md5");
        config = AppConfig::fromJson(root);
    }

    if (needsPasswordMigration) {
        std::cerr << "Migrating config password to MD5 storage" << std::endl;
        if (!save()) return false;
    }
    return loadSubscribers();
}

bool ConfigManager::loadSubscribers() {
    const auto path = std::filesystem::current_path() / "tvstreamer5-subscribers.json";
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
    config.password = normalizeMd5Password(config.password);
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
    const auto path = std::filesystem::current_path() / "tvstreamer5-subscribers.json";
    std::ofstream output(path);
    if (!output.is_open()) return false;
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "  ";
    output << Json::writeString(writer, subscribers.toJson());
    return true;
}
