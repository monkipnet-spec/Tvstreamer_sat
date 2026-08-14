#include "CaBackend.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace {

void writeError(char* buffer, size_t size, const std::string& text) {
    if (!buffer || size == 0) return;
    const size_t count = std::min(size - 1, text.size());
    std::memcpy(buffer, text.data(), count);
    buffer[count] = '\0';
}

bool validPluginApi(const tvs_ca_backend_api_v1* api, std::string& error) {
    if (!api) {
        error = "entry point returned null API";
        return false;
    }
    if (api->abi_version != TVS_CA_BACKEND_ABI_V1) {
        error = "unsupported CA backend ABI version";
        return false;
    }
    if (!api->backend_id || !*api->backend_id) {
        error = "backend_id is empty";
        return false;
    }
    if (!api->create || !api->destroy || !api->open_reader || !api->close_reader ||
        !api->start_service || !api->stop_service || !api->process_ts) {
        error = "required CA backend function is missing";
        return false;
    }
    return true;
}

} // namespace

CaBackendManager& CaBackendManager::instance() {
    static CaBackendManager manager;
    return manager;
}

CaBackendManager::CaBackendManager() {
    hostApi_.abi_version = TVS_CA_BACKEND_ABI_V1;
    hostApi_.log = &CaBackendManager::hostLog;
    hostApi_.monotonic_ms = &CaBackendManager::hostMonotonicMs;

    LoadedBackend passthrough;
    passthrough.id = "passthrough";
    passthrough.displayName = "Passthrough (без декодирования)";
    passthrough.vendor = "TVStreammerSAT5";
    passthrough.path = "builtin";
    passthrough.builtin = true;
    passthrough.usable = true;
    backends_.emplace(passthrough.id, std::move(passthrough));
}

CaBackendManager::~CaBackendManager() {
    stopAll();
    std::lock_guard<std::mutex> lock(mutex_);
    unloadPluginsLocked();
}

void CaBackendManager::hostLog(int level, const char* backendId, const char* message) {
    const char* levelText = "INFO";
    if (level == TVS_CA_LOG_DEBUG) levelText = "DEBUG";
    else if (level == TVS_CA_LOG_WARN) levelText = "WARN";
    else if (level == TVS_CA_LOG_ERROR) levelText = "ERROR";
    std::cerr << "CA backend[" << safeString(backendId) << "] " << levelText
              << ": " << safeString(message) << std::endl;
}

uint64_t CaBackendManager::hostMonotonicMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string CaBackendManager::safeString(const char* value) {
    return value ? std::string(value) : std::string();
}

std::string CaBackendManager::extractDvbPids(const std::string& uri) {
    const auto query = uri.find('?');
    if (query == std::string::npos) return {};
    const std::string needle = "pids=";
    size_t start = uri.find(needle, query + 1);
    if (start == std::string::npos) return {};
    start += needle.size();
    size_t end = uri.find('&', start);
    std::string value = uri.substr(start, end == std::string::npos ? std::string::npos : end - start);
    // The application serializes ':' as %3A in DVB URIs.
    for (size_t pos = 0; (pos = value.find("%3A", pos)) != std::string::npos;) {
        value.replace(pos, 3, ":");
        ++pos;
    }
    for (size_t pos = 0; (pos = value.find("%3a", pos)) != std::string::npos;) {
        value.replace(pos, 3, ":");
        ++pos;
    }
    return value;
}

void CaBackendManager::configure(const std::vector<CamClientConfig>& clients) {
    std::lock_guard<std::mutex> lock(mutex_);
    clientPolicies_ = clients;
    if (!sessions_.empty()) {
        // Running streams keep their current plugin instances. Discovery is
        // refreshed when no CA sessions are active to avoid invalidating ABI
        // pointers while a transport probe is executing.
        return;
    }
    unloadPluginsLocked();
    loadPluginsLocked();
}

void CaBackendManager::unloadPluginsLocked() {
    for (auto it = backends_.begin(); it != backends_.end();) {
        LoadedBackend& backend = it->second;
        if (backend.builtin) {
            ++it;
            continue;
        }
        if (backend.instance && backend.api && backend.api->destroy) {
            backend.api->destroy(backend.instance);
            backend.instance = nullptr;
        }
        if (backend.library) {
            dlclose(backend.library);
            backend.library = nullptr;
        }
        it = backends_.erase(it);
    }
}

void CaBackendManager::loadPluginsLocked() {
    std::vector<std::filesystem::path> directories;
    if (const char* env = std::getenv("TVSTREAMMERSAT5_CA_PLUGIN_DIR")) {
        if (*env) directories.emplace_back(env);
    }

    std::error_code ec;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec && !executable.empty()) {
        const auto executableDir = executable.parent_path();
        directories.push_back(executableDir / "ca-plugins");
        directories.push_back(executableDir);
    }
    directories.emplace_back(kDefaultPluginDirectory);

    std::vector<std::string> files;
    std::set<std::string> seenDirectories;
    for (const auto& directory : directories) {
        const auto normalized = directory.lexically_normal().string();
        if (normalized.empty() || !seenDirectories.insert(normalized).second) continue;
        if (!std::filesystem::exists(directory, ec) || ec) {
            ec.clear();
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file()) continue;
            const auto path = entry.path();
            if (path.extension() == ".so") files.push_back(path.string());
        }
        ec.clear();
    }

    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    for (const auto& path : files) loadPluginFileLocked(path);
}

void CaBackendManager::loadPluginFileLocked(const std::string& path) {
    void* library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::cerr << "CA backend plugin load failed: " << path << ": " << dlerror() << std::endl;
        return;
    }

    dlerror();
    auto entry = reinterpret_cast<tvs_ca_backend_get_api_v1_fn>(dlsym(library, TVS_CA_BACKEND_ENTRY_V1));
    const char* symbolError = dlerror();
    if (symbolError || !entry) {
        std::cerr << "CA backend plugin missing entry point: " << path << std::endl;
        dlclose(library);
        return;
    }

    const tvs_ca_backend_api_v1* api = entry();
    std::string error;
    if (!validPluginApi(api, error)) {
        std::cerr << "CA backend plugin rejected: " << path << ": " << error << std::endl;
        dlclose(library);
        return;
    }
    const std::string id = safeString(api->backend_id);
    if (backends_.count(id)) {
        std::cerr << "CA backend plugin duplicate id ignored: " << id << " path=" << path << std::endl;
        dlclose(library);
        return;
    }

    void* instance = api->create(&hostApi_);
    if (!instance) {
        std::cerr << "CA backend plugin create failed: " << id << " path=" << path << std::endl;
        dlclose(library);
        return;
    }

    LoadedBackend backend;
    backend.id = id;
    backend.displayName = safeString(api->display_name);
    if (backend.displayName.empty()) backend.displayName = id;
    backend.vendor = safeString(api->vendor);
    backend.path = path;
    backend.capabilities = api->capabilities;
    backend.usable = true;
    backend.library = library;
    backend.api = api;
    backend.instance = instance;
    backends_.emplace(id, std::move(backend));

    std::cerr << "CA backend plugin loaded: id=" << id
              << " path=" << path
              << " capabilities=0x" << std::hex << api->capabilities << std::dec << std::endl;
}

CaBackendManager::LoadedBackend* CaBackendManager::findBackendLocked(const std::string& id) {
    auto found = backends_.find(id.empty() ? "passthrough" : id);
    return found == backends_.end() ? nullptr : &found->second;
}

const CaBackendManager::LoadedBackend* CaBackendManager::findBackendLocked(const std::string& id) const {
    auto found = backends_.find(id.empty() ? "passthrough" : id);
    return found == backends_.end() ? nullptr : &found->second;
}

bool CaBackendManager::ensureReaderOpenLocked(LoadedBackend& backend,
                                               const CaBackendReaderBinding& reader,
                                               std::string& error) {
    if (backend.builtin) {
        ++backend.readerRefs[reader.key];
        return true;
    }
    auto refs = backend.readerRefs.find(reader.key);
    if (refs != backend.readerRefs.end()) {
        ++refs->second;
        return true;
    }

    tvs_ca_reader_info_v1 info{};
    info.reader_key = reader.key.c_str();
    info.device = reader.device.c_str();
    info.serial = reader.serial.c_str();
    info.display_name = reader.displayName.c_str();
    info.card_system = reader.cardSystem.c_str();
    info.caid = reader.caid.c_str();
    info.provider = reader.provider.c_str();
    info.backend_config_json = reader.backendConfig.c_str();
    info.max_services = reader.maxServices;

    char errorBuffer[512]{};
    const int result = backend.api->open_reader(backend.instance, &info, errorBuffer, sizeof(errorBuffer));
    if (result != TVS_CA_RESULT_OK && result != TVS_CA_RESULT_PASSTHROUGH) {
        error = errorBuffer[0] ? errorBuffer : "CA backend failed to open reader";
        return false;
    }
    backend.readerRefs[reader.key] = 1;
    return true;
}

void CaBackendManager::releaseReaderLocked(LoadedBackend& backend, const std::string& readerKey) {
    auto found = backend.readerRefs.find(readerKey);
    if (found == backend.readerRefs.end()) return;
    if (found->second > 1) {
        --found->second;
        return;
    }
    if (!backend.builtin && backend.api && backend.api->close_reader) {
        backend.api->close_reader(backend.instance, readerKey.c_str());
    }
    backend.readerRefs.erase(found);
}

bool CaBackendManager::startService(const StreamConfig& stream,
                                    const CaBackendReaderBinding& reader,
                                    std::string* error) {
    if (error) error->clear();
    std::lock_guard<std::mutex> lock(mutex_);
    if (sessions_.count(stream.id)) return true;

    const std::string backendId = reader.backendId.empty() ? "passthrough" : reader.backendId;
    LoadedBackend* backend = findBackendLocked(backendId);
    if (!backend || !backend->usable) {
        if (error) *error = "CA backend is not loaded: " + backendId;
        return false;
    }

    std::string openError;
    if (!ensureReaderOpenLocked(*backend, reader, openError)) {
        if (error) *error = openError;
        return false;
    }

    ServiceSession session;
    session.streamId = stream.id;
    session.streamName = stream.name;
    session.readerKey = reader.key;
    session.backendId = backend->id;
    session.passthrough = backend->builtin || !(backend->capabilities & TVS_CA_CAP_TS_INPLACE);
    session.status = session.passthrough ? "PASSTHROUGH_NO_DECODE" : "BACKEND_RESERVED";

    if (!backend->builtin) {
        const std::string pids = extractDvbPids(stream.inputUri);
        tvs_ca_service_info_v1 info{};
        info.stream_id = stream.id.c_str();
        info.stream_name = stream.name.c_str();
        info.service_id = stream.inputServiceId ? stream.inputServiceId : stream.serviceId;
        info.service_name = stream.serviceName.c_str();
        info.service_provider = stream.serviceProvider.c_str();
        info.input_uri = stream.inputUri.c_str();
        info.service_pids = pids.c_str();

        char errorBuffer[512]{};
        const int result = backend->api->start_service(
            backend->instance, reader.key.c_str(), &info, errorBuffer, sizeof(errorBuffer));
        if (result != TVS_CA_RESULT_OK && result != TVS_CA_RESULT_PASSTHROUGH) {
            releaseReaderLocked(*backend, reader.key);
            if (error) *error = errorBuffer[0] ? errorBuffer : "CA backend failed to start service";
            return false;
        }
        session.passthrough = result == TVS_CA_RESULT_PASSTHROUGH ||
                              !(backend->capabilities & TVS_CA_CAP_TS_INPLACE);
        session.status = session.passthrough ? "PASSTHROUGH_NO_DECODE" : "BACKEND_RESERVED";
    }

    sessions_.emplace(stream.id, std::move(session));
    std::cerr << "CA backend service reserved: stream=" << stream.id
              << " client=" << reader.key
              << " backend=" << backend->id
              << " mode=" << (backend->builtin ? "passthrough" : "plugin") << std::endl;
    return true;
}

void CaBackendManager::markServiceActive(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(streamId);
    if (found == sessions_.end()) return;
    found->second.active = true;
    if (found->second.status == "BACKEND_RESERVED") found->second.status = "BACKEND_ACTIVE";
}

void CaBackendManager::stopService(const std::string& streamId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = sessions_.find(streamId);
    if (found == sessions_.end()) return;
    ServiceSession session = found->second;
    sessions_.erase(found);

    LoadedBackend* backend = findBackendLocked(session.backendId);
    if (!backend) return;
    if (!backend->builtin && backend->api && backend->api->stop_service) {
        backend->api->stop_service(backend->instance, streamId.c_str());
    }
    releaseReaderLocked(*backend, session.readerKey);
    std::cerr << "CA backend service stopped: stream=" << streamId
              << " client=" << session.readerKey
              << " backend=" << session.backendId << std::endl;
}

void CaBackendManager::stopAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<ServiceSession> sessions;
    sessions.reserve(sessions_.size());
    for (const auto& [id, session] : sessions_) {
        (void)id;
        sessions.push_back(session);
    }
    sessions_.clear();

    for (const auto& session : sessions) {
        LoadedBackend* backend = findBackendLocked(session.backendId);
        if (!backend) continue;
        if (!backend->builtin && backend->api && backend->api->stop_service) {
            backend->api->stop_service(backend->instance, session.streamId.c_str());
        }
        releaseReaderLocked(*backend, session.readerKey);
    }
}

bool CaBackendManager::processTransport(const std::string& streamId, uint8_t* data, size_t size) {
    if (!data || size == 0) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    auto sessionIt = sessions_.find(streamId);
    if (sessionIt == sessions_.end()) return true;
    ServiceSession& session = sessionIt->second;
    ++session.calls;
    session.bytes += size;
    if ((size % 188u) != 0u) {
        ++session.errors;
        session.status = "TS_ALIGNMENT_ERROR";
        session.lastError = "CA backend received a non-188-byte-aligned transport buffer";
        return false;
    }

    LoadedBackend* backend = findBackendLocked(session.backendId);
    if (!backend || backend->builtin || !backend->api || !backend->api->process_ts) {
        session.status = "PASSTHROUGH_NO_DECODE";
        return true;
    }

    tvs_ca_ts_result_v1 result{};
    const int rc = backend->api->process_ts(backend->instance, streamId.c_str(), data, size, &result);
    session.packetsSeen += result.packets_seen;
    session.packetsChanged += result.packets_changed;
    session.packetsClear += result.packets_clear;
    session.packetsScrambled += result.packets_scrambled;
    if (result.status[0]) session.status = result.status;

    if (rc == TVS_CA_RESULT_OK) {
        if (!result.status[0]) session.status = "BACKEND_ACTIVE";
        return true;
    }
    if (rc == TVS_CA_RESULT_PASSTHROUGH) {
        session.passthrough = true;
        if (!result.status[0]) session.status = "PASSTHROUGH_NO_DECODE";
        return true;
    }
    if (rc == TVS_CA_RESULT_RETRY) {
        ++session.retries;
        if (!result.status[0]) session.status = "BACKEND_RETRY";
        return true; // preserve input TS while backend recovers
    }

    ++session.errors;
    session.lastError = result.status[0] ? result.status : "CA backend process_ts failed";
    session.status = "BACKEND_ERROR";
    // Fail open at the transport boundary: never destroy or truncate a valid TS
    // merely because an optional CA plugin failed. Decode telemetry will still
    // show scrambled packets to the UI.
    return false;
}

Json::Value CaBackendManager::backendToJsonLocked(const LoadedBackend& backend) const {
    Json::Value item;
    item["id"] = backend.id;
    item["display_name"] = backend.displayName;
    item["vendor"] = backend.vendor;
    item["path"] = backend.path;
    item["builtin"] = backend.builtin;
    item["usable"] = backend.usable;
    item["capabilities"] = Json::UInt(backend.capabilities);
    item["ts_inplace"] = (backend.capabilities & TVS_CA_CAP_TS_INPLACE) != 0;
    item["multi_service"] = (backend.capabilities & TVS_CA_CAP_MULTI_SERVICE) != 0;
    item["emm_managed"] = (backend.capabilities & TVS_CA_CAP_EMM_MANAGED) != 0;
    item["entitlement_status"] = (backend.capabilities & TVS_CA_CAP_ENTITLEMENT_STATUS) != 0;
    item["reader_reconnect"] = (backend.capabilities & TVS_CA_CAP_READER_RECONNECT) != 0;
    item["load_error"] = backend.loadError;
    item["open_clients"] = Json::UInt(backend.readerRefs.size());
    if (!backend.builtin && backend.api && backend.api->status_json) {
        const char* status = backend.api->status_json(backend.instance);
        if (status && *status) {
            Json::Value parsed;
            Json::CharReaderBuilder builder;
            std::string errors;
            std::istringstream input(status);
            if (Json::parseFromStream(builder, input, &parsed, &errors)) item["plugin_status"] = parsed;
            else item["plugin_status_text"] = status;
        }
    }
    return item;
}

Json::Value CaBackendManager::streamState(const std::string& streamId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value result;
    auto found = sessions_.find(streamId);
    if (found == sessions_.end()) {
        result["managed"] = false;
        result["backend_id"] = "";
        result["status"] = "NO_BACKEND_SESSION";
        return result;
    }
    const ServiceSession& session = found->second;
    result["managed"] = true;
    result["backend_id"] = session.backendId;
    result["client"] = session.readerKey;
    result["active"] = session.active;
    result["passthrough"] = session.passthrough;
    result["status"] = session.status;
    result["calls"] = Json::UInt64(session.calls);
    result["bytes"] = Json::UInt64(session.bytes);
    result["errors"] = Json::UInt64(session.errors);
    result["retries"] = Json::UInt64(session.retries);
    result["packets_seen"] = Json::UInt64(session.packetsSeen);
    result["packets_changed"] = Json::UInt64(session.packetsChanged);
    result["packets_clear"] = Json::UInt64(session.packetsClear);
    result["packets_scrambled"] = Json::UInt64(session.packetsScrambled);
    result["last_error"] = session.lastError;
    const LoadedBackend* backend = findBackendLocked(session.backendId);
    result["native_plugin"] = backend && !backend->builtin &&
                              (backend->capabilities & TVS_CA_CAP_TS_INPLACE) != 0 &&
                              !session.passthrough;
    if (backend) {
        result["display_name"] = backend->displayName;
        result["capabilities"] = Json::UInt(backend->capabilities);
    }
    return result;
}

Json::Value CaBackendManager::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Json::Value root;
    std::string directory = kDefaultPluginDirectory;
    if (const char* env = std::getenv("TVSTREAMMERSAT5_CA_PLUGIN_DIR")) if (*env) directory = env;
    root["abi_version"] = Json::UInt(TVS_CA_BACKEND_ABI_V1);
    root["plugin_directory"] = directory;
    root["network_ca_server"] = false;
    root["external_key_export"] = false;
    root["raw_control_word_api"] = false;
    root["sessions"] = Json::UInt(sessions_.size());

    Json::Value backends(Json::arrayValue);
    for (const auto& [id, backend] : backends_) {
        (void)id;
        backends.append(backendToJsonLocked(backend));
    }
    root["backends"] = backends;
    return root;
}
