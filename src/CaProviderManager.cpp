#include "CaProviderManager.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

namespace ca_provider {
namespace {

bool safeDeviceNode(const std::string& device) {
    if (device.rfind("/dev/ttyUSB", 0) != 0 && device.rfind("/dev/ttyACM", 0) != 0) {
        return false;
    }
    for (char ch : device) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '/' || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

std::map<std::string, std::string> udevPropertiesForDevice(const std::string& device) {
    std::map<std::string, std::string> properties;
    if (!safeDeviceNode(device)) {
        return properties;
    }

    const std::string command = "udevadm info --query=property --name=" + device + " 2>/dev/null";
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe) {
        return properties;
    }

    char buffer[1024];
    while (std::fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
            line.pop_back();
        }
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        properties[line.substr(0, separator)] = line.substr(separator + 1);
    }
    ::pclose(pipe);
    return properties;
}

std::string firstProperty(const std::map<std::string, std::string>& properties,
                          std::initializer_list<const char*> names) {
    for (const char* name : names) {
        const auto it = properties.find(name);
        if (it != properties.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return "";
}

} // namespace

Json::Value enumerateSerialReadersJson() {
    Json::Value root(Json::arrayValue);
    const std::filesystem::path byIdDirectory("/dev/serial/by-id");
    std::error_code error;
    if (!std::filesystem::exists(byIdDirectory, error) || error) {
        return root;
    }

    std::vector<std::filesystem::path> entries;
    for (std::filesystem::directory_iterator iterator(byIdDirectory, error), end;
         !error && iterator != end; iterator.increment(error)) {
        entries.push_back(iterator->path());
    }
    std::sort(entries.begin(), entries.end());

    for (const auto& byIdPath : entries) {
        std::error_code canonicalError;
        const auto resolved = std::filesystem::canonical(byIdPath, canonicalError);
        if (canonicalError) continue;
        const std::string device = resolved.string();
        if (!safeDeviceNode(device)) continue;

        const auto properties = udevPropertiesForDevice(device);
        Json::Value item;
        item["by_id"] = byIdPath.string();
        item["by_id_name"] = byIdPath.filename().string();
        item["device"] = device;
        item["tty"] = resolved.filename().string();
        item["vendor"] = firstProperty(properties, {"ID_VENDOR_FROM_DATABASE", "ID_VENDOR"});
        item["model"] = firstProperty(properties, {"ID_MODEL_FROM_DATABASE", "ID_MODEL"});
        item["serial"] = firstProperty(properties, {"ID_SERIAL_SHORT", "ID_SERIAL"});
        item["usb_path"] = firstProperty(properties, {"ID_PATH", "ID_PATH_TAG"});
        item["online"] = true;
        root.append(item);
    }
    return root;
}

const Json::Value* findSerialReaderById(const Json::Value& readers, const std::string& byId) {
    if (byId.empty() || !readers.isArray()) return nullptr;
    for (const auto& reader : readers) {
        if (reader.get("by_id", "").asString() == byId) return &reader;
    }
    return nullptr;
}

const CaProviderConfig* findProvider(const AppConfig& config, const std::string& id) {
    if (id.empty()) return nullptr;
    for (const auto& provider : config.caProviders) {
        if (provider.id == id) {
            return &provider;
        }
    }
    return nullptr;
}

int effectiveMaxChannels(const CaProviderConfig& provider) {
    return std::clamp(provider.maxChannels, 1, 1024);
}

std::string cardStatus(const CaProviderConfig& provider, const Json::Value& serialReaders) {
    if (provider.readerById.empty()) return "NO_READER";
    return findSerialReaderById(serialReaders, provider.readerById) ? "READER_ONLINE" : "OFFLINE";
}

std::string managerStatus(const CaProviderConfig& provider, const Json::Value& serialReaders) {
    if (provider.readerById.empty()) return "reader is not assigned";
    return findSerialReaderById(serialReaders, provider.readerById)
        ? "serial reader online; card capability/descrambling interface is not configured"
        : "configured reader is offline";
}

} // namespace ca_provider
