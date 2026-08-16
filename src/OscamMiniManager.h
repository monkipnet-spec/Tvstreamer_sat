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
    struct Reader {
        std::string label, protocol="mouse", device, caid, detect="cd", ident, emmcache="1,3,2";
        int mhz=600, cardmhz=600, group=1;
        bool enabled=true;
    };
    struct Settings {
        std::string bindIp="127.0.0.1", ports, key="0102030405060708091011121314";
        std::string user="tvstreamer", password="tvstreamer", groups="1,2";
        bool keepalive=true, au=true;
        std::vector<Reader> readers;
    };
    Settings loadLocked();
    bool saveLocked(const Settings&, std::string& error);
    Json::Value statusLocked();
    static std::string readFile(const std::string&);
    static bool writeAtomic(const std::string&, const std::string&, std::string&);
    static std::string run(const std::string&, int* rc=nullptr);
    static std::vector<std::string> ttyDevices();
    static std::string json(const Json::Value&);
    static Json::Value parse(const std::string&, std::string&);
    static std::string trim(std::string);
    std::mutex mutex_;
};
