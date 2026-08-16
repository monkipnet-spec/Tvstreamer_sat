#pragma once

#include <jsoncpp/json/json.h>
#include <mutex>
#include <string>
#include <vector>

class OscamMiniManager {
public:
    static OscamMiniManager& instance();

    static constexpr const char* kConfigDir = "/opt/TVStreammerSAT5/oscam-mini/config";
    static constexpr const char* kBinary = "/opt/TVStreammerSAT5/oscam-mini/oscam-mini";
    static constexpr const char* kService = "oscam-mini.service";

    std::string statusJson();
    std::string settingsJson();
    std::string saveSettingsJson(const std::string& body);
    std::string serviceActionJson(const std::string& body);
    std::string renderPage();

private:
    OscamMiniManager() = default;

    struct NewcamdUser {
        std::string user = "tvstreamer";
        std::string password = "tvstreamer";
        int port = 11220;
        std::string caid = "0652";
        std::string provider = "000000";
        std::string groups = "1";
        bool au = true;
    };

    struct Reader {
        std::string label;
        std::string protocol = "mouse";
        std::string device;
        std::string caid;
        std::string detect = "cd";
        std::string ident;
        std::string emmcache = "1,3,2";
        int mhz = 600;
        int cardmhz = 600;
        int group = 1;
        bool enabled = true;
    };

    struct Settings {
        std::string bindIp = "127.0.0.1";
        std::string key = "0102030405060708091011121314";
        bool keepalive = true;
        std::vector<NewcamdUser> users;
        std::vector<Reader> readers;
    };

    Settings loadLocked();
    bool saveLocked(const Settings& settings, std::string& error);
    Json::Value statusLocked();

    static std::string readFile(const std::string& path);
    static bool writeAtomic(const std::string& path, const std::string& content, std::string& error);
    static std::string run(const std::string& command, int* rc = nullptr);
    static std::vector<std::string> ttyDevices();
    static std::string json(const Json::Value& value);
    static Json::Value parse(const std::string& body, std::string& error);
    static std::string trim(std::string value);

    std::mutex mutex_;
};
