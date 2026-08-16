#pragma once
#include <jsoncpp/json/json.h>
#include <mutex>
#include <string>
#include <vector>

class OscamMiniManager {
public:
    static OscamMiniManager& instance();

    static constexpr const char* kRootDir   = "/opt/TVStreammerSAT5/oscam-mini";
    static constexpr const char* kConfigDir = "/opt/TVStreammerSAT5/oscam-mini/config";
    static constexpr const char* kBinary    = "/opt/TVStreammerSAT5/oscam-mini/oscam-mini";
    static constexpr const char* kService   = "oscam-mini.service";

    std::string statusJson();
    std::string settingsJson();
    std::string saveSettingsJson(const std::string& body);
    std::string serviceActionJson(const std::string& body);
    std::string renderPage();

private:
    OscamMiniManager() = default;

    struct Reader {
        std::string label;
        std::string protocol = "mouse";
        std::string device;
        std::string caid;
        std::string detect = "cd";
        int mhz = 600;
        int cardmhz = 600;
        int group = 1;
        std::string ident;
        std::string emmcache = "1,3,2";
        bool enabled = true;
    };

    struct Settings {
        std::string bindIp = "127.0.0.1";
        std::string newcamdPorts;
        std::string newcamdKey = "0102030405060708091011121314";
        bool keepalive = true;
        std::string user = "tvstreamer";
        std::string password = "tvstreamer";
        std::string userGroups = "1,2";
        bool au = true;
        std::vector<Reader> readers;
    };

    Settings loadSettingsLocked();
    bool saveSettingsLocked(const Settings&, std::string& error);
    Json::Value statusLocked();

    static Json::Value parseJson(const std::string&, std::string& error);
    static std::string jsonString(const Json::Value&);
    static std::string runCommand(const std::string&, int* exitCode = nullptr);
    static std::string readFile(const std::string&);
    static bool writeAtomic(const std::string&, const std::string&, std::string& error);
    static std::vector<std::string> ttyDevices();
    static std::string trim(const std::string&);
    static bool validHex(const std::string&, size_t exactLength = 0);
    static bool validToken(const std::string&, size_t maxLen = 64);
    static bool validDevice(const std::string&);

    mutable std::mutex mutex_;
};
