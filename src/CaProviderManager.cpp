// CaProviderManager.cpp
#include "CaProviderManager.h"
#include "NewcamdStatusBackend.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <chrono>
#include <unistd.h>

namespace ca_provider {
namespace {

// ---------------------------------------------------------------------------
// Кэш ключей (CW) в памяти
// ---------------------------------------------------------------------------
struct CachedCW {
    uint8_t cw0[8];
    uint8_t cw1[8];
    std::chrono::steady_clock::time_point timestamp;
    int ttl_seconds;  // время жизни в секундах (обычно 6-8 для CW)
};

// Глобальный кэш: ключ = "caid:provider:sid"
static std::unordered_map<std::string, CachedCW> g_cwCache;
static std::mutex g_cacheMutex;

// Вспомогательная функция для формирования ключа
static std::string buildCacheKey(uint16_t caid, uint16_t provider, uint16_t sid) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%04x:%04x:%04x", caid, provider, sid);
    return std::string(buf);
}

// Проверка валидности CW (не истекло)
static bool isCWValid(const CachedCW& cached) {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - cached.timestamp).count();
    return elapsed < cached.ttl_seconds;
}

// Внутренняя функция очистки кэша (вызывается с захваченным мьютексом)
static void clearCacheInternal() {
    g_cwCache.clear();
}

// ---------------------------------------------------------------------------
// Заглушка для реального получения CW от backend (будет реализована отдельно)
// ---------------------------------------------------------------------------
static CWResponse requestCWFromBackend(const CaProviderConfig& provider,
                                       uint16_t sid, uint16_t caid, uint16_t providerId) {
    // В реальном коде здесь должен быть вызов NewcamdClient::sendECM
    // или аналогичного метода для других backend'ов.
    // Пока возвращаем пустой ответ с успехом=false
    CWResponse response{};
    response.success = false;
    return response;
}

// ---------------------------------------------------------------------------
// Публичные функции для работы с кэшем
// ---------------------------------------------------------------------------
CWResponse getCW(const CaProviderConfig& provider, uint16_t sid,
                 uint16_t caid, uint16_t providerId) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);

    // Проверяем, есть ли в кэше
    std::string key = buildCacheKey(caid, providerId, sid);
    auto it = g_cwCache.find(key);
    if (it != g_cwCache.end() && isCWValid(it->second)) {
        CWResponse response{};
        response.success = true;
        memcpy(response.cw_0, it->second.cw0, 8);
        memcpy(response.cw_1, it->second.cw1, 8);
        response.caid = caid;
        response.provider = providerId;
        response.sid = sid;
        return response;
    }

    // Нет в кэше или истекло – запрашиваем у backend
    CWResponse fresh = requestCWFromBackend(provider, sid, caid, providerId);
    if (fresh.success) {
        // Сохраняем в кэш с TTL = 7 секунд (типично для CW)
        CachedCW cached;
        memcpy(cached.cw0, fresh.cw_0, 8);
        memcpy(cached.cw1, fresh.cw_1, 8);
        cached.timestamp = std::chrono::steady_clock::now();
        cached.ttl_seconds = 7;
        g_cwCache[key] = cached;
    }
    return fresh;
}

void clearCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    clearCacheInternal();
}

void setCacheTTL(int ttlSeconds) {
    // Можно установить глобальный TTL, но пока оставим как заглушку
    (void)ttlSeconds;
}

// ---------------------------------------------------------------------------
// Существующие функции (без изменений)
// ---------------------------------------------------------------------------
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

bool isReaderBackend(const CaProviderConfig& provider) {
    return provider.backendType.empty() || provider.backendType == "reader";
}

std::string cardStatus(const CaProviderConfig& provider, const Json::Value& serialReaders) {
    if (!isReaderBackend(provider)) {
        if (provider.backendType == "newcamd-status") {
            const auto status = NewcamdStatusBackend::probe(provider.endpoint);
            return status.status;
        }
        return "BACKEND_UNKNOWN";
    }
    if (provider.readerById.empty()) return "NO_READER";
    return findSerialReaderById(serialReaders, provider.readerById) ? "READER_ONLINE" : "OFFLINE";
}

std::string managerStatus(const CaProviderConfig& provider, const Json::Value& serialReaders) {
    if (provider.backendType == "newcamd-status") {
        const auto status = NewcamdStatusBackend::probe(provider.endpoint);
        if (!status.configured) return "newcamd status endpoint is not configured";
        return status.online ? "newcamd TCP endpoint online (status-only backend)"
                             : "newcamd TCP endpoint offline: " + status.error;
    }
    if (!isReaderBackend(provider)) return "unknown CA provider backend";
    if (provider.readerById.empty()) return "reader is not assigned";
    return findSerialReaderById(serialReaders, provider.readerById)
        ? "serial reader online; card capability/descrambling interface is not configured"
        : "configured reader is offline";
}

Json::Value backendStatusJson(const CaProviderConfig& provider, const Json::Value& serialReaders) {
    Json::Value root;
    root["type"] = provider.backendType.empty() ? "reader" : provider.backendType;
    if (provider.backendType == "newcamd-status") {
        const auto status = NewcamdStatusBackend::probe(provider.endpoint);
        root["status"] = status.status;
        root["message"] = !status.configured
            ? "newcamd status endpoint is not configured"
            : (status.online ? "newcamd TCP endpoint online (status-only backend)"
                             : "newcamd TCP endpoint offline: " + status.error);
        root["configured"] = status.configured;
        root["online"] = status.online;
        root["host"] = status.host;
        root["port"] = status.port;
        root["error"] = status.error;
        root["capabilities"] = "tcp-status-only";
    } else {
        const bool configured = !provider.readerById.empty();
        const bool online = configured && findSerialReaderById(serialReaders, provider.readerById);
        root["status"] = !configured ? "NO_READER" : (online ? "READER_ONLINE" : "OFFLINE");
        root["message"] = !configured ? "reader is not assigned"
            : (online ? "serial reader online; card capability/descrambling interface is not configured"
                      : "configured reader is offline");
        root["configured"] = configured;
        root["online"] = online;
        root["capabilities"] = "serial-reader-inventory";
    }
    return root;
}

// ---------------------------------------------------------------------------
// Функция для остановки/очистки кэша (вызывается при завершении программы)
// ---------------------------------------------------------------------------
void shutdownCaProvider() {
    clearCache();  // теперь это публичная функция, конфликта нет
}

} // namespace ca_provider