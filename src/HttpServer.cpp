#include "HttpServer.h"
#include "AppVersion.h"

#include "utils.h"
#include "TranscoderModule.h"
#include "DvbSatellite.h"
#include "CardManager.h"
#include "OscamMiniManager.h"
#include "protocols/GstProtocolTypes.h"

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <set>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {

constexpr const char* kProgramRelease = tvs::app::kProgramRelease;
constexpr const char* kProgramVersion = tvs::app::kProgramVersion;

struct CaDecodeUiMemory {
    std::string state;
    std::chrono::steady_clock::time_point holdUntil{};
};

std::mutex& caDecodeUiMutex() {
    static std::mutex mutex;
    return mutex;
}

std::map<std::string, CaDecodeUiMemory>& caDecodeUiMemory() {
    static std::map<std::string, CaDecodeUiMemory> memory;
    return memory;
}

std::string smoothCaDecodeState(const std::string& id, const std::string& rawState, bool active) {
    if (id.empty() || rawState == "not_applicable" || !active) {
        std::lock_guard<std::mutex> lock(caDecodeUiMutex());
        caDecodeUiMemory().erase(id);
        return active ? rawState : "offline";
    }

    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(caDecodeUiMutex());
    auto& item = caDecodeUiMemory()[id];
    if (rawState == "clear") {
        item.state = "clear";
        item.holdUntil = now + std::chrono::seconds(6);
        return rawState;
    }
    if (item.state == "clear" && now < item.holdUntil &&
        (rawState == "waiting" || rawState == "invalid" || rawState == "scrambled")) {
        return "clear";
    }
    item.state = rawState;
    item.holdUntil = now + std::chrono::seconds(2);
    return rawState;
}
std::string queryValue(const std::string& target, const std::string& key) {
    const auto queryPos = target.find('?');
    if (queryPos == std::string::npos) {
        return "";
    }

    std::string query = target.substr(queryPos + 1);
    std::istringstream stream(query);
    std::string part;
    while (std::getline(stream, part, '&')) {
        const auto eq = part.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        if (part.substr(0, eq) == key) {
            return part.substr(eq + 1);
        }
    }
    return "";
}

std::string urlDecode(const std::string& value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const std::string hex = value.substr(i + 1, 2);
            char* end = nullptr;
            const long ch = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                decoded.push_back(static_cast<char>(ch));
                i += 2;
                continue;
            }
        }
        decoded.push_back(value[i] == '+' ? ' ' : value[i]);
    }
    return decoded;
}

int64_t unixNowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string cleanPathToken(const std::string& value, bool allowDot = false) {
    std::string cleaned;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || (allowDot && ch == '.')) {
            cleaned.push_back(ch);
        }
    }
    return cleaned;
}

std::string cleanFileName(const std::string& value) {
    std::string name;
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') {
            name.push_back(ch);
        } else if (std::isspace(static_cast<unsigned char>(ch))) {
            name.push_back('_');
        }
    }
    if (name.empty() || name == "." || name == "..") {
        name = "backup-file.ts";
    }
    if (!name.empty() && name.front() == '.') {
        name.insert(name.begin(), '_');
    }
    if (name.size() > 96) {
        name = name.substr(name.size() - 96);
    }
    return name;
}

std::string base64Decode(const std::string& value) {
    static const std::string alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string decoded;
    int buffer = 0;
    int bits = -8;

    for (unsigned char ch : value) {
        if (std::isspace(ch)) {
            continue;
        }
        if (ch == '=') {
            break;
        }
        const auto pos = alphabet.find(static_cast<char>(ch));
        if (pos == std::string::npos) {
            return "";
        }
        buffer = (buffer << 6) + static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return decoded;
}

bool constantTimeEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }

    unsigned char diff = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

bool isLoopbackHost(const std::string& host) {
    const std::string lower = toLower(host);
    return lower == "localhost" || lower == "127.0.0.1" || lower == "::1";
}

std::string advertisedHost(const StreamConfig& cfg, bool replaceLoopback = false) {
    if (cfg.outputHost.empty() || cfg.outputHost == "0.0.0.0" || cfg.outputHost == "::" ||
        (replaceLoopback && isLoopbackHost(cfg.outputHost))) {
        if (!cfg.interfaceAddress.empty()) {
            return cfg.interfaceAddress;
        }
        const auto interfaces = enumerateNetworkInterfaces();
        if (!interfaces.empty()) {
            return interfaces.front().address;
        }
        return "127.0.0.1";
    }
    return cfg.outputHost;
}

int validPortOrDefault(int port, int defaultPort) {
    return port > 0 && port <= 65535 ? port : defaultPort;
}

std::string normalizedOutputType(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type == "udp_vbr" || type == "udpvbr") {
        type = "udp-vbr";
    } else if (type == "udp_cbr" || type == "udpcbr") {
        type = "udp-cbr";
    }
    if (type == "udp") {
        return cfg.cbr ? "udp-cbr" : "udp-vbr";
    }
    if (type != "udp-vbr" && type != "udp-cbr" && type != "rtp" &&
        type != "srt" && type != "http" && type != "hls" &&
        type != "rtsp" && type != "rtmp" && type != "youtube") {
        return "udp-vbr";
    }
    return type;
}

StreamOutputConfig primaryOutputConfig(const StreamConfig& cfg) {
    StreamOutputConfig output;
    output.outputType = cfg.outputType;
    output.outputMode = cfg.outputMode;
    output.outputHost = cfg.outputHost;
    output.outputPort = cfg.outputPort;
    return output;
}

StreamConfig configForOutput(const StreamConfig& base, const StreamOutputConfig& output) {
    StreamConfig cfg = base;
    cfg.outputType = output.outputType;
    cfg.outputMode = output.outputMode;
    cfg.outputHost = output.outputHost;
    cfg.outputPort = output.outputPort;
    cfg.additionalOutputs.clear();
    const std::string type = normalizedOutputType(cfg);
    if (type == "udp-cbr") {
        cfg.cbr = true;
    } else if (type == "udp-vbr") {
        cfg.cbr = false;
    }
    return cfg;
}

std::vector<StreamConfig> streamOutputs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    outputs.push_back(configForOutput(cfg, primaryOutputConfig(cfg)));
    for (const auto& output : cfg.additionalOutputs) {
        outputs.push_back(configForOutput(cfg, output));
    }
    return outputs;
}

int streamHttpPort(const StreamConfig& cfg, int defaultPort) {
    const std::string type = normalizedOutputType(cfg);
    if (type == "http" || type == "hls") {
        return validPortOrDefault(cfg.outputPort, defaultPort);
    }
    return defaultPort;
}

std::map<std::string, StreamConfig> streamConfigById(const std::vector<StreamConfig>& streams) {
    std::map<std::string, StreamConfig> result;
    for (const auto& stream : streams) {
        if (!stream.id.empty()) {
            result[stream.id] = stream;
        }
    }
    return result;
}

bool sameStreamConfig(const StreamConfig& left, const StreamConfig& right) {
    return left.toJson() == right.toJson();
}

std::vector<std::string> currentProcessArgs() {
    std::vector<std::string> args;
    std::ifstream input("/proc/self/cmdline", std::ios::binary);
    std::string arg;
    while (std::getline(input, arg, '\0')) {
        args.push_back(arg);
    }
    if (args.empty()) {
        args.push_back("TVStreammerSAT5");
    }
    return args;
}

void closeInheritedFileDescriptors() {
    long maxFd = sysconf(_SC_OPEN_MAX);
    if (maxFd < 0 || maxFd > 65536) {
        maxFd = 65536;
    }
    for (int fd = 3; fd < maxFd; ++fd) {
        close(fd);
    }
}

[[noreturn]] void execCurrentProcess(const std::vector<std::string>& args) {
    std::vector<std::string> argvStorage = args;
    std::vector<char*> argv;
    argv.reserve(argvStorage.size() + 1);
    for (auto& arg : argvStorage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    execv("/proc/self/exe", argv.data());
    std::cerr << "Program restart failed: " << std::strerror(errno) << std::endl;
    std::_Exit(1);
}

std::string streamLink(const StreamConfig& cfg, int httpPort) {
    const std::string type = normalizedOutputType(cfg);
    if (type == "rtp") {
        return "rtp://" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
    }
    if (type == "srt") {
        const std::string mode = toLower(cfg.outputMode) == "caller" ? "listener" : "caller";
        const bool listener = toLower(cfg.outputMode) != "caller";
        return "srt://" + advertisedHost(cfg, listener) + ":" + std::to_string(cfg.outputPort) + "?mode=" + mode;
    }
    if (type == "youtube") {
        const std::string hostLower = toLower(cfg.outputHost);
        return hostLower.rfind("rtmp", 0) == 0
            ? cfg.outputHost
            : "rtmp://a.rtmp.youtube.com/live2/" + cfg.outputHost;
    }
    if (type == "rtmp") {
        const std::string hostLower = toLower(cfg.outputHost);
        return hostLower.rfind("rtmp", 0) == 0
            ? cfg.outputHost
            : "rtmp://" + advertisedHost(cfg) + ":" + std::to_string(cfg.outputPort) + "/live/" + cfg.id;
    }
    if (type == "http") {
        return "http://" + advertisedHost(cfg, true) + ":" + std::to_string(streamHttpPort(cfg, httpPort)) + "/stream/" + cfg.id + ".ts";
    }
    if (type == "rtsp") {
        const std::string hostLower = toLower(cfg.outputHost);
        return hostLower.rfind("rtsp://", 0) == 0 || hostLower.rfind("rtsps://", 0) == 0
            ? cfg.outputHost
            : "rtsp://" + advertisedHost(cfg) + ":" + std::to_string(cfg.outputPort > 0 ? cfg.outputPort : 8554) + "/" + cfg.id;
    }
    if (type == "hls") {
        return "http://" + advertisedHost(cfg, true) + ":" + std::to_string(streamHttpPort(cfg, httpPort)) + "/hls/" + cfg.id + "/playlist.m3u8";
    }
    return "udp://@" + cfg.outputHost + ":" + std::to_string(cfg.outputPort);
}

} // namespace

std::string extractStreamIdFromTarget(const std::string& target) {
    const std::string tsPrefix = "/stream/";
    if (target.rfind(tsPrefix, 0) == 0) {
        const auto start = tsPrefix.size();
        if (target.size() <= start + 3 || target.substr(target.size() - 3) != ".ts") {
            return "";
        }
        const auto end = target.find('.', start);
        return cleanPathToken(target.substr(start, end == std::string::npos ? std::string::npos : end - start));
    }

    const std::string hlsPrefix = "/hls/";
    if (target.rfind(hlsPrefix, 0) == 0) {
        const auto slash = target.find('/', hlsPrefix.size());
        if (slash == std::string::npos) {
            return "";
        }
        return cleanPathToken(target.substr(hlsPrefix.size(), slash - hlsPrefix.size()));
    }

    return "";
}

HttpServer::HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm)
    : ioContext(ioc), configManager(cfg), streamManager(sm) {
}

bool HttpServer::start() {
    return bindHttpPorts(configuredHttpPorts());
}

void HttpServer::doAccept(std::shared_ptr<tcp::acceptor> listener, int port, uint64_t generation) {
    if (!listener || !listener->is_open()) {
        return;
    }

    listener->async_accept([this, listener, port, generation](boost::system::error_code ec, tcp::socket socket) {
        if (generation != acceptGeneration.load()) {
            return;
        }
        if (!ec) {
            std::thread(&HttpServer::handleSession, this, std::move(socket)).detach();
        } else if (ec != boost::asio::error::operation_aborted) {
            std::cerr << "HTTP accept failed on port " << port << ": " << ec.message() << std::endl;
        }
        if (listener->is_open()) {
            doAccept(listener, port, generation);
        }
    });
}

void HttpServer::handleSession(tcp::socket socket) {
    try {
        boost::beast::flat_buffer buffer;
        http::request_parser<http::string_body> parser;
        parser.body_limit(512ULL * 1024ULL * 1024ULL);
        http::read(socket, buffer, parser);
        http::request<http::string_body> req = parser.release();
        http::response<http::string_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "TVStreammerSAT5");
        res.set(http::field::content_type, "text/html; charset=UTF-8");
        res.set(http::field::cache_control, "no-store");
        res.set(http::field::pragma, "no-cache");
        res.keep_alive(req.keep_alive());

        const std::string target(req.target());
        if (requiresAuthentication(target) && !isAuthorized(req)) {
            writeUnauthorized(res);
            res.content_length(res.body().size());
            http::write(socket, res);
            return;
        }

        if (req.method() == http::verb::get) {
          if (target.rfind("/stream/", 0) == 0) {
            if (!isStreamClientAllowed(socket, target)) {
              res.result(http::status::forbidden);
              res.set(http::field::content_type, "text/plain");
              res.body() = "Stream access denied";
            } else if (handleHttpStream(socket, target)) {
              return;
            }
            } else if (target.rfind("/hls/", 0) == 0) {
                if (!isStreamClientAllowed(socket, target)) {
                    res.result(http::status::forbidden);
                    res.set(http::field::content_type, "text/plain");
                    res.body() = "Stream access denied";
                } else if (serveHlsFile(socket, target, res)) {
                    // serveHlsFile filled the response.
                }
            } else if (target == "/" || target == "/index.html") {
                res.body() = renderIndexPage();
            } else if (target == "/oscam-mini") {
                res.set(http::field::content_type, "text/html; charset=UTF-8");
                res.body() = OscamMiniManager::instance().renderPage();
            } else if (target == "/api/oscam-mini/status") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().statusJson();
            } else if (target == "/api/oscam-mini/settings") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().settingsJson();
            } else if (target == "/api/interfaces") {
                res.set(http::field::content_type, "application/json");
                res.body() = listInterfaces();
            } else if (target == "/api/system-metrics") {
              res.set(http::field::content_type, "application/json");
              res.body() = systemMetrics();
            } else if (target == "/api/backup-files") {
                res.set(http::field::content_type, "application/json");
                res.body() = listBackupFiles();
            } else if (target == "/api/state") {
                res.set(http::field::content_type, "application/json");
                res.body() = currentState();
            } else if (target == "/api/dvb-adapters") {
                res.set(http::field::content_type, "application/json");
                res.body() = dvbAdapters();
            } else if (target == "/api/ca-manager") {
                res.set(http::field::content_type, "application/json");
                res.body() = caManagerStatus();
            } else if (target.rfind("/api/quality-history", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                res.body() = qualityHistory(target);
            } else if (target == "/health") {
                res.set(http::field::content_type, "text/plain");
                res.body() = "Healthy";
            } else {
                res.result(http::status::not_found);
                res.body() = "Not Found";
            }
        } else if (req.method() == http::verb::post) {
            if (target == "/api/oscam-mini/save") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().saveSettingsJson(req.body());
            } else if (target == "/api/oscam-mini/action") {
                res.set(http::field::content_type, "application/json");
                res.body() = OscamMiniManager::instance().serviceActionJson(req.body());
            } else if (target == "/api/save-config") {
                handleSaveConfig(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/start-stream") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleStartStream(req.body());
            } else if (target == "/api/stop-stream") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleStopStream(req.body());
            } else if (target == "/api/restart-program") {
                handleRestartProgram();
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"restarting\"}";
            } else if (target == "/api/delete-stream") {
              handleDeleteStream(req.body());
              res.set(http::field::content_type, "application/json");
              res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/save-subscribers") {
                handleSaveSubscribers(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else if (target.rfind("/api/upload-backup-file", 0) == 0) {
                res.set(http::field::content_type, "application/json");
                res.body() = handleUploadBackupFile(target, req.body());
            } else if (target == "/api/delete-backup-file") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDeleteBackupFile(req.body());
            } else if (target == "/api/reset-subscriber") {
              handleResetSubscriber(req.body());
              res.set(http::field::content_type, "application/json");
              res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/cam-client-settings") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleCamClientSettings(req.body());
            } else if (target == "/api/dvb-signal") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbTune(req.body(), false);
            } else if (target == "/api/dvb-scan") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbTune(req.body(), true);
            } else if (target == "/api/dvb-add-channels") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleDvbAddChannels(req.body());
            } else {
                res.result(http::status::not_found);
                res.body() = "Not Found";
            }
        } else {
            res.result(http::status::method_not_allowed);
            res.body() = "Method Not Allowed";
        }

        res.content_length(res.body().size());
        http::write(socket, res);
    } catch (const std::exception& ex) {
        const std::string message = ex.what();
        if (message.find("bad method") != std::string::npos) {
            std::cerr << "HTTP non-HTTP request ignored on web port: " << message << std::endl;
        } else {
            std::cerr << "HTTP session failed: " << message << std::endl;
        }
    }
}

bool HttpServer::requiresAuthentication(const std::string& target) const {
    return target != "/health" &&
           target.rfind("/stream/", 0) != 0 &&
           target.rfind("/hls/", 0) != 0;
}

bool HttpServer::isAuthorized(const http::request<http::string_body>& req) const {
    const auto auth = req.find(http::field::authorization);
    if (auth == req.end()) {
        return false;
    }

    const std::string header(auth->value());
    const std::string prefix = "Basic ";
    if (header.size() <= prefix.size() || !boost::algorithm::istarts_with(header, prefix)) {
        return false;
    }

    const std::string decoded = base64Decode(header.substr(prefix.size()));
    const auto separator = decoded.find(':');
    if (separator == std::string::npos) {
        return false;
    }

    const std::string login = decoded.substr(0, separator);
    const std::string password = decoded.substr(separator + 1);
    return constantTimeEquals(login, configManager.config.login) &&
           constantTimeEquals(password, configManager.config.password);
}

bool HttpServer::isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const {
  if (!configManager.subscribers.filteringEnabled) {
    return true;
  }
  const std::string normalizedClientIp = normalizeIpAddress(clientIp);
  if (streamId.empty() || normalizedClientIp.empty()) {
    return false;
  }
  for (const auto& subscriber : configManager.subscribers.subscribers) {
    const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
    const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
    const bool ipMatches = subscriber.enabled &&
      (normalizedClientIp == primaryIp || (!backupIp.empty() && normalizedClientIp == backupIp));
    const bool streamMatches = std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
    if (ipMatches && streamMatches) {
      return true;
    }
  }
  return false;
}

bool HttpServer::isStreamClientAllowed(const tcp::socket& socket, const std::string& target) const {
  boost::system::error_code ec;
  const std::string clientIp = socket.remote_endpoint(ec).address().to_string();
  if (ec) {
    return false;
  }
  const std::string streamId = extractStreamIdFromTarget(target);
  return isClientAllowedForStream(streamId, clientIp);
}

void HttpServer::writeUnauthorized(http::response<http::string_body>& res) const {
    res.result(http::status::unauthorized);
    res.set(http::field::www_authenticate, "Basic realm=\"TVStreammerSAT5\"");
    res.set(http::field::content_type, "text/plain; charset=UTF-8");
    res.body() = "Unauthorized";
}

std::set<int> HttpServer::configuredHttpPorts() const {
    std::set<int> ports;
    if (configManager.config.httpPort > 0 && configManager.config.httpPort <= 65535) {
        ports.insert(configManager.config.httpPort);
    }
    const int defaultPort = configManager.config.httpPort;
    for (const auto& stream : configManager.config.streams) {
        for (const auto& output : streamOutputs(stream)) {
            const std::string type = normalizedOutputType(output);
            if ((type == "http" || type == "hls") && output.outputPort > 0 && output.outputPort <= 65535) {
                ports.insert(streamHttpPort(output, defaultPort));
            }
        }
    }
    if (ports.empty()) {
        ports.insert(9000);
    }
    return ports;
}

bool HttpServer::bindHttpPorts(const std::set<int>& ports) {
    boost::system::error_code ec;
    const uint64_t generation = acceptGeneration.fetch_add(1) + 1;

    for (auto& [port, listener] : acceptors) {
        (void)port;
        if (listener && listener->is_open()) {
            listener->cancel(ec);
            listener->close(ec);
        }
    }

    std::unordered_map<int, std::shared_ptr<tcp::acceptor>> nextAcceptors;
    for (int port : ports) {
        if (port <= 0 || port > 65535) {
            std::cerr << "Invalid HTTP port: " << port << std::endl;
            continue;
        }

        auto listener = std::make_shared<tcp::acceptor>(ioContext);
        tcp::endpoint endpoint(tcp::v4(), static_cast<unsigned short>(port));
        listener->open(endpoint.protocol(), ec);
        if (ec) {
            std::cerr << "HTTP server failed to open port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->set_option(boost::asio::socket_base::reuse_address(true), ec);
        if (ec) {
            std::cerr << "HTTP server failed to set reuse_address on port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->bind(endpoint, ec);
        if (ec) {
            std::cerr << "HTTP server failed to bind port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }
        listener->listen(boost::asio::socket_base::max_listen_connections, ec);
        if (ec) {
            std::cerr << "HTTP server failed to listen on port " << port << ": " << ec.message() << std::endl;
            ec.clear();
            continue;
        }

        doAccept(listener, port, generation);
        nextAcceptors[port] = std::move(listener);
        std::cerr << "HTTP server listening on port " << port << std::endl;
    }

    const bool primaryBound = nextAcceptors.count(configManager.config.httpPort) > 0;
    acceptors = std::move(nextAcceptors);
    if (!primaryBound) {
        std::cerr << "HTTP server primary port is not listening: " << configManager.config.httpPort << std::endl;
    }
    return primaryBound;
}

void HttpServer::refreshHttpPorts() {
    boost::asio::post(ioContext, [this]() {
        bindHttpPorts(configuredHttpPorts());
    });
}

std::string HttpServer::listInterfaces() {
    Json::Value root;
    auto interfaces = enumerateNetworkInterfaces();
    for (auto& iface : interfaces) {
        Json::Value item;
        item["name"] = iface.name;
        item["address"] = iface.address;
        root.append(item);
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

  std::string HttpServer::systemMetrics() {
    uint64_t cpuTotal = 0;
    uint64_t cpuIdle = 0;
    {
      std::ifstream statFile("/proc/stat");
      std::string label;
      uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
      if (statFile >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal && label == "cpu") {
        cpuIdle = idle + iowait;
        cpuTotal = user + nice + system + idle + iowait + irq + softirq + steal;
      }
    }

    uint64_t memoryTotal = 0;
    uint64_t memoryAvailable = 0;
    {
      std::ifstream meminfo("/proc/meminfo");
      std::string label;
      uint64_t value = 0;
      std::string unit;
      while (meminfo >> label >> value >> unit) {
        if (label == "MemTotal:") memoryTotal = value;
        if (label == "MemAvailable:") memoryAvailable = value;
      }
    }

    Json::Value root;
    std::lock_guard<std::mutex> lock(metricsMutex);
    double cpuPercent = 0.0;
    const auto now = std::chrono::steady_clock::now();
    if (previousCpuTotal > 0 && cpuTotal > previousCpuTotal && cpuIdle >= previousCpuIdle) {
      const uint64_t totalDelta = cpuTotal - previousCpuTotal;
      const uint64_t idleDelta = cpuIdle - previousCpuIdle;
      cpuPercent = 100.0 * static_cast<double>(totalDelta - std::min(totalDelta, idleDelta)) / totalDelta;
    }
    previousCpuTotal = cpuTotal;
    previousCpuIdle = cpuIdle;
    const double elapsedSeconds = previousMetricsSample.time_since_epoch().count() == 0
      ? 0.0
      : std::chrono::duration<double>(now - previousMetricsSample).count();
    previousMetricsSample = now;

    root["cpu_percent"] = cpuPercent;
    root["ram_percent"] = memoryTotal == 0
      ? 0.0
      : 100.0 * static_cast<double>(memoryTotal - std::min(memoryTotal, memoryAvailable)) / memoryTotal;
    Json::Value interfaces(Json::arrayValue);
    const auto interfaceAddresses = enumerateNetworkInterfaces();
    std::ifstream netdev("/proc/net/dev");
    std::string line;
    while (std::getline(netdev, line)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string name = line.substr(0, colon);
      name.erase(0, name.find_first_not_of(" \t"));
      name.erase(name.find_last_not_of(" \t") + 1);
      if (name == "lo" || name.empty()) continue;
      std::istringstream values(line.substr(colon + 1));
      uint64_t rxBytes = 0, txBytes = 0;
      if (!(values >> rxBytes)) continue;
      for (int i = 0; i < 7; ++i) {
        uint64_t ignored = 0;
        values >> ignored;
      }
      if (!(values >> txBytes)) continue;
      double rxMbps = 0.0;
      double txMbps = 0.0;
      const auto previous = previousNetworkBytes.find(name);
      if (elapsedSeconds > 0.0 && previous != previousNetworkBytes.end()) {
        rxMbps = static_cast<double>(rxBytes - std::min(rxBytes, previous->second.first)) * 8.0 / elapsedSeconds / 1000000.0;
        txMbps = static_cast<double>(txBytes - std::min(txBytes, previous->second.second)) * 8.0 / elapsedSeconds / 1000000.0;
      }
      previousNetworkBytes[name] = {rxBytes, txBytes};
      Json::Value item;
      item["name"] = name;
      const auto address = std::find_if(interfaceAddresses.begin(), interfaceAddresses.end(), [&name](const NetworkInterface& iface) {
        return iface.name == name;
      });
      item["address"] = address == interfaceAddresses.end() ? "" : address->address;
      item["rx_mbps"] = rxMbps;
      item["tx_mbps"] = txMbps;
      interfaces.append(item);
    }
    root["interfaces"] = interfaces;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
  }

std::string HttpServer::currentState() {
    Json::Value root;
    root["login"] = configManager.config.login;
    root["server_name"] = configManager.config.serverName;
    root["http_port"] = configManager.config.httpPort;
    root["language"] = configManager.config.language;
    root["telegram_token"] = configManager.config.telegramToken;
    root["telegram_chat_id"] = configManager.config.telegramChatId;
    Json::Value camClients(Json::arrayValue);
    for (const auto& client : configManager.config.camClients) camClients.append(client.toJson());
    root["cam_clients"] = camClients;
    root["program_release"] = kProgramRelease;
    root["program_version"] = kProgramVersion;
    Json::Value interfaces(Json::arrayValue);
    for (const auto& iface : enumerateNetworkInterfaces()) {
        Json::Value item;
        item["name"] = iface.name;
        item["address"] = iface.address;
        interfaces.append(item);
    }
    root["interfaces"] = interfaces;
    root["stream_count"] = Json::UInt(configManager.config.streams.size());
    root["active_count"] = Json::UInt(streamManager.activeStreams().size());
    root["ca_manager"] = CardManager::instance().snapshot();
    root["subscriber_filtering_enabled"] = configManager.subscribers.filteringEnabled;
    static const auto transcoderCapabilities = TranscoderModule::inspectCapabilities();
    Json::Value transcoder;
    transcoder["available"] = transcoderCapabilities.available;
    transcoder["video_encoder"] = transcoderCapabilities.videoEncoder;
    transcoder["audio_encoder"] = transcoderCapabilities.audioEncoder;
    transcoder["aac_encoder"] = transcoderCapabilities.aacEncoder;
    transcoder["mp3_encoder"] = transcoderCapabilities.mp3Encoder;
    transcoder["deinterlace"] = transcoderCapabilities.deinterlaceAvailable;
    transcoder["message"] = transcoderCapabilities.message;
    Json::Value missing(Json::arrayValue);
    for (const auto& element : transcoderCapabilities.missingElements) missing.append(element);
    transcoder["missing_elements"] = missing;
    root["transcoder"] = transcoder;
    Json::Value subscribers(Json::arrayValue);
    for (const auto& subscriber : configManager.subscribers.subscribers) {
      Json::Value item = subscriber.toJson();
      const size_t activeSessions = streamManager.activeSubscriberSessions(subscriber);
      item["active_sessions"] = Json::UInt64(activeSessions);
      item["session_active"] = activeSessions > 0;
      subscribers.append(item);
    }
    root["subscribers"] = subscribers;
    Json::Value streams(Json::arrayValue);
    auto snap = streamManager.snapshot();
    // Reading FE status is cheap and does not retune the frontend, but several
    // channel tiles may share the exact same DVB input/transponder. Cache one
    // ioctl snapshot per live DVB URI for each /api/state response.
    std::map<std::string, Json::Value> dvbSignalCache;
    for (const auto& cfg : configManager.config.streams) {
        Json::Value item = cfg.toJson();
        if (snap.count(cfg.id)) {
            auto* streamState = snap.at(cfg.id);
            item["active"] = streamState->active.load();
            item["status"] = streamState->statusMessage;
            item["using_backup"] = streamState->usingBackup;
            item["active_input_uri"] = cfg.testPattern
                ? "test://bars"
                : (streamState->activeInputUri.empty() ? cfg.inputUri : streamState->activeInputUri);
            item["active_input_label"] = cfg.testPattern
                ? "Тест"
                : (streamState->usingBackup
                    ? (toLower(cfg.backupInputType) == "file" ? "Файл замены" : "Резерв")
                    : "Основной");
            item["bitrate_in_kbps"] = Json::UInt64(streamState->inputBitrate.load() / 1000);
            item["bitrate_out_kbps"] = Json::UInt64(streamState->outputBitrate.load() / 1000);
            item["input_cc_errors"] = Json::UInt64(streamState->inputCcErrorsDelta.load());
            item["output_cc_errors"] = Json::UInt64(streamState->outputCcErrorsDelta.load());
            item["input_cc_errors_total"] = Json::UInt64(streamState->inputCcErrors.load());
            item["output_cc_errors_total"] = Json::UInt64(streamState->outputCcErrors.load());
            const uint64_t caPayloadPackets = streamState->outputTsPayloadPacketsDelta.load();
            const uint64_t caScrambledPackets = streamState->outputTsScrambledPacketsDelta.load();
            const uint64_t caClearPesStarts = streamState->outputTsClearPesStartsDelta.load();
            item["ca_output_payload_packets"] = Json::UInt64(caPayloadPackets);
            item["ca_output_scrambled_packets"] = Json::UInt64(caScrambledPackets);
            item["ca_output_clear_pes_starts"] = Json::UInt64(caClearPesStarts);
            item["ca_output_scrambled_percent"] = caPayloadPackets > 0
                ? (100.0 * static_cast<double>(caScrambledPackets) / static_cast<double>(caPayloadPackets))
                : 0.0;
            std::string rawCaDecodeState;
            const bool streamActive = streamState->active.load();
            if (cfg.conditionalAccessClient.empty()) {
                rawCaDecodeState = "not_applicable";
            } else if (!streamActive) {
                rawCaDecodeState = "offline";
            } else if (streamState->outputBitrate.load() == 0 || caPayloadPackets < 50) {
                rawCaDecodeState = "waiting";
            } else if (caScrambledPackets > 0) {
                rawCaDecodeState = "scrambled";
            } else if (caClearPesStarts == 0) {
                rawCaDecodeState = "invalid";
            } else {
                rawCaDecodeState = "clear";
            }
            item["ca_decode_state"] = smoothCaDecodeState(cfg.id, rawCaDecodeState, streamActive);
            item["cc_errors"] = item["input_cc_errors"];
            item["cc_errors_total"] = item["input_cc_errors_total"];
        } else {
            item["active"] = false;
            item["status"] = "stopped";
            item["using_backup"] = false;
            item["active_input_uri"] = cfg.testPattern ? "test://bars" : cfg.inputUri;
            item["active_input_label"] = cfg.testPattern ? "Тест" : "Основной";
            item["bitrate_in_kbps"] = Json::UInt64(0);
            item["bitrate_out_kbps"] = Json::UInt64(0);
            item["input_cc_errors"] = Json::UInt64(0);
            item["output_cc_errors"] = Json::UInt64(0);
            item["input_cc_errors_total"] = Json::UInt64(0);
            item["output_cc_errors_total"] = Json::UInt64(0);
            item["ca_output_payload_packets"] = Json::UInt64(0);
            item["ca_output_scrambled_packets"] = Json::UInt64(0);
            item["ca_output_clear_pes_starts"] = Json::UInt64(0);
            item["ca_output_scrambled_percent"] = 0.0;
            item["ca_decode_state"] = cfg.conditionalAccessClient.empty() ? "not_applicable" : "offline";
            item["cc_errors"] = Json::UInt64(0);
            item["cc_errors_total"] = Json::UInt64(0);
        }
        const bool configuredDvb = DvbSatellite::isDvbUri(cfg.inputUri);
        item["dvb_input"] = configuredDvb;
        item["dvb_signal_available"] = false;
        item["dvb_locked"] = false;
        item["dvb_signal"] = 0;
        item["dvb_quality"] = 0;
        if (configuredDvb && item.get("active", false).asBool() &&
            !item.get("using_backup", false).asBool() && !cfg.testPattern) {
            const std::string liveDvbUri = item.get("active_input_uri", cfg.inputUri).asString();
            if (DvbSatellite::isDvbUri(liveDvbUri)) {
                auto cached = dvbSignalCache.find(liveDvbUri);
                if (cached == dvbSignalCache.end()) {
                    cached = dvbSignalCache.emplace(
                        liveDvbUri, DvbSatellite::signalFromUri(liveDvbUri)).first;
                }
                const Json::Value& dvbStats = cached->second;
                item["dvb_signal_available"] = dvbStats.get("available", false).asBool();
                item["dvb_locked"] = dvbStats.get("locked", false).asBool();
                item["dvb_signal"] = dvbStats.get("signal", 0).asInt();
                item["dvb_quality"] = dvbStats.get("quality", 0).asInt();
                if (dvbStats.isMember("signal_db")) item["dvb_signal_db"] = dvbStats["signal_db"];
                if (dvbStats.isMember("cnr_db")) item["dvb_cnr_db"] = dvbStats["cnr_db"];
            }
        }

        item["ca"] = CardManager::instance().streamState(cfg.id);

        Json::Value links(Json::arrayValue);
        for (const auto& output : streamOutputs(cfg)) {
            Json::Value link;
            link["output_type"] = normalizedOutputType(output);
            link["output_mode"] = output.outputMode;
            link["output_host"] = output.outputHost;
            link["output_port"] = output.outputPort;
            link["url"] = streamLink(output, configManager.config.httpPort);
            links.append(link);
        }
        item["vlc_links"] = links;
        item["vlc_link"] = links.size() == 0 ? "" : links[0].get("url", "").asString();
        recordQualitySample(cfg, item);
        streams.append(item);
    }
    root["streams"] = streams;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::dvbAdapters() {
    Json::Value root = DvbSatellite::adapters();
    std::map<std::pair<int, int>, Json::Value> usage;
    const auto snap = streamManager.snapshot();
    for (const auto& [id, state] : snap) {
        if (!state || !state->running.load()) continue;
        DvbSatelliteParams params;
        std::string parseError;
        if (!DvbSatellite::parseUri(state->config.inputUri, params, parseError)) continue;
        const auto key = std::make_pair(params.adapter, params.frontend);
        auto& item = usage[key];
        item["consumers"] = item.get("consumers", 0).asUInt() + 1;
        item["frequency_khz"] = params.frequencyKHz;
        item["symbol_rate"] = params.symbolRateK;
        item["polarity"] = params.polarity;
        item["delivery_system"] = params.deliverySystem;
        if (!item.isMember("streams")) item["streams"] = Json::Value(Json::arrayValue);
        Json::Value stream;
        stream["id"] = id;
        stream["name"] = state->config.name;
        stream["sid"] = state->config.inputServiceId;
        item["streams"].append(stream);
    }
    if (root.isMember("adapters") && root["adapters"].isArray()) {
        for (Json::ArrayIndex i = 0; i < root["adapters"].size(); ++i) {
            auto& adapter = root["adapters"][i];
            const auto key = std::make_pair(adapter.get("adapter", 0).asInt(), adapter.get("frontend", 0).asInt());
            const auto found = usage.find(key);
            if (found == usage.end()) {
                adapter["in_use"] = false;
                adapter["consumers"] = 0;
            } else {
                adapter["in_use"] = true;
                adapter["consumers"] = found->second.get("consumers", 0);
                adapter["frequency_khz"] = found->second.get("frequency_khz", 0);
                adapter["symbol_rate"] = found->second.get("symbol_rate", 0);
                adapter["polarity"] = found->second.get("polarity", "");
                adapter["delivery_system"] = found->second.get("delivery_system", "");
                adapter["streams"] = found->second["streams"];
            }
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::caManagerStatus() {
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, CardManager::instance().snapshot());
}

std::string HttpServer::handleCamClientSettings(const std::string& body) {
    Json::Value response;
    Json::Value request;
    Json::CharReaderBuilder readerBuilder;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["result"] = "error";
        response["error"] = "invalid CAM client settings: " + errors;
    } else {
        auto configToString = [](const Json::Value& value) {
            Json::StreamWriterBuilder writer;
            writer["indentation"] = "";
            return Json::writeString(writer, value.isNull() ? Json::Value(Json::objectValue) : value);
        };
        auto parseClient = [&](const Json::Value& item) {
            CamClientConfig updated;
            updated.id = item.get("id", item.get("client_id", "").asString()).asString();
            updated.name = item.get("name", updated.id).asString();
            updated.maxServices = std::clamp(item.get("max_services", 10).asUInt(), 1u,
                                             CardManager::kMaxConfigurableServices);
            updated.backendId = item.get("backend_id", "newcamd").asString();
            if (updated.backendId.empty() || updated.backendId == "passthrough") updated.backendId = "newcamd";
            if (item.isMember("backend_config")) {
                updated.backendConfig = item["backend_config"].isString()
                    ? item["backend_config"].asString()
                    : configToString(item["backend_config"]);
            } else {
                Json::Value backendConfig;
                backendConfig["host"] = item.get("host", "127.0.0.1").asString();
                backendConfig["port"] = item.get("port", 15000).asInt();
                backendConfig["user"] = item.get("user", "user").asString();
                backendConfig["pass"] = item.get("pass", "pass").asString();
                backendConfig["des"] = item.get("des", "0102030405060708091011121314").asString();
                updated.backendConfig = configToString(backendConfig);
            }
            if (updated.backendConfig.empty()) updated.backendConfig = "{}";
            return updated;
        };

        std::vector<CamClientConfig> nextClients;
        if (request["clients"].isArray()) {
            for (const auto& item : request["clients"]) {
                CamClientConfig client = parseClient(item);
                if (!client.id.empty()) nextClients.push_back(client);
            }
            configManager.config.camClients = nextClients;
        } else {
            CamClientConfig updated = parseClient(request);
            if (updated.id.empty()) {
                response["result"] = "error";
                response["error"] = "client id is required";
                Json::StreamWriterBuilder writer;
                return Json::writeString(writer, response);
            }
            bool replaced = false;
            for (auto& existing : configManager.config.camClients) {
                if (existing.id == updated.id) {
                    existing = updated;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) configManager.config.camClients.push_back(updated);
        }

        if (!configManager.save()) {
            response["result"] = "error";
            response["error"] = "failed to save CAM client settings";
        } else {
            CardManager::instance().configure(configManager.config.camClients);
            response = CardManager::instance().snapshot();
            response["result"] = "ok";
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleDvbTune(const std::string& body, bool scan) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["error"] = "invalid DVB request: " + errors;
    } else {
        response = scan ? DvbSatellite::scan(request) : DvbSatellite::signal(request);
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleDvbAddChannels(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["error"] = "invalid channel selection: " + errors;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    if (!request["channels"].isArray() || request["channels"].size() == 0) {
        response["error"] = "No satellite channels selected";
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    std::string outputType = toLower(request.get("output_type", "udp-vbr").asString());
    if (outputType != "udp-vbr" && outputType != "udp-cbr") outputType = "udp-vbr";
    std::string outputHost = request.get("output_host", "239.255.10.1").asString();
    if (outputHost.empty()) outputHost = "239.255.10.1";
    int basePort = std::clamp(request.get("base_port", 5000).asInt(), 1, 65535);
    const std::string interfaceAddress = request.get("interface_address", "").asString();
    const bool autoStart = request.get("auto_start", false).asBool();
    const std::string conditionalAccessClient = request.get("conditional_access_client", "").asString();
    bool hasScrambledSelection = false;
    for (const auto& selected : request["channels"]) {
        if (selected.get("scrambled", false).asBool()) { hasScrambledSelection = true; break; }
    }
    if (hasScrambledSelection && (conditionalAccessClient.empty() || conditionalAccessClient == "auto")) {
        response["error"] = "Select a CAM client for scrambled channels";
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }
    const uint64_t targetBitrate = std::clamp<uint64_t>(
        request.get("target_bitrate_kbps", Json::UInt64(12000)).asUInt64(), 500, 100000) * 1000ULL;

    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::set<int> occupiedPorts;
    for (const auto& existing : configManager.config.streams) {
        if (existing.outputHost == outputHost) occupiedPorts.insert(existing.outputPort);
        for (const auto& extra : existing.additionalOutputs) {
            if (extra.outputHost == outputHost) occupiedPorts.insert(extra.outputPort);
        }
    }
    int nextPort = basePort;
    size_t created = 0;
    size_t skipped = 0;
    Json::Value createdItems(Json::arrayValue);

    for (Json::ArrayIndex index = 0; index < request["channels"].size(); ++index) {
        const Json::Value& item = request["channels"][index];
        const uint32_t sid = item.get("service_id", 0).asUInt();
        const std::string inputUri = item.get("input_uri", "").asString();
        if (sid == 0 || inputUri.empty()) {
            ++skipped;
            continue;
        }

        DvbSatelliteParams params;
        std::string dvbError;
        if (!DvbSatellite::parseUri(inputUri, params, dvbError)) {
            ++skipped;
            continue;
        }

        const bool duplicate = std::any_of(
            configManager.config.streams.begin(), configManager.config.streams.end(),
            [&](const StreamConfig& existing) {
                return existing.inputUri == inputUri && existing.inputServiceId == sid;
            });
        if (duplicate) {
            ++skipped;
            continue;
        }

        StreamConfig config;
        config.id = "sat-" + std::to_string(now) + "-" + std::to_string(sid) + "-" + std::to_string(index);
        config.name = item.get("name", "").asString();
        if (config.name.empty()) config.name = "Satellite " + std::to_string(sid);
        config.inputUri = inputUri;
        config.inputMode = "auto";
        config.testPattern = false;
        config.autoStart = autoStart;
        config.outputType = outputType;
        config.outputMode = "listener";
        config.outputHost = outputHost;
        while (nextPort <= 65535 && occupiedPorts.count(nextPort)) ++nextPort;
        if (nextPort > 65535) {
            ++skipped;
            continue;
        }
        config.outputPort = nextPort;
        occupiedPorts.insert(nextPort);
        ++nextPort;
        config.interfaceAddress = interfaceAddress;
        config.cbr = outputType == "udp-cbr";
        config.targetBitrate = targetBitrate;
        config.remapEnabled = false;
        config.transcodeEnabled = false;
        config.inputServiceId = sid;
        config.serviceId = sid;
        config.serviceName = config.name;
        config.serviceProvider = item.get("provider", "").asString();
        config.conditionalAccessClient = item.get("scrambled", false).asBool()
            ? conditionalAccessClient
            : "";
        config.videoPid = 0;
        config.audioPid = 0;

        configManager.config.streams.push_back(config);
        Json::Value createdItem = config.toJson();
        createdItems.append(createdItem);
        ++created;
    }

    if (created > 0) configManager.save();
    response["result"] = "ok";
    response["created"] = Json::UInt64(created);
    response["skipped"] = Json::UInt64(skipped);
    response["channels"] = createdItems;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

bool HttpServer::handleHttpStream(tcp::socket& socket, const std::string& target) {
    const std::string prefix = "/stream/";
    if (target.size() <= prefix.size() + 3 || target.substr(target.size() - 3) != ".ts") {
        return false;
    }

    const std::string id = cleanPathToken(target.substr(prefix.size(), target.size() - prefix.size() - 3));
    if (id.empty()) {
        return false;
    }

    const std::string header =
        "HTTP/1.1 200 OK\r\n"
        "Server: TVStreammerSAT5\r\n"
        "Content-Type: video/MP2T\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "\r\n";
    boost::asio::write(socket, boost::asio::buffer(header));
    boost::system::error_code endpointError;
    const std::string clientIp = socket.remote_endpoint(endpointError).address().to_string();
    int fd = socket.release();
    streamManager.addHttpClient(id, fd, endpointError ? "" : clientIp);
    return true;
}

bool HttpServer::serveHlsFile(const tcp::socket& socket, const std::string& target, http::response<http::string_body>& res) {
    const std::string prefix = "/hls/";
    const auto slash = target.find('/', prefix.size());
    if (slash == std::string::npos) {
        return false;
    }

    const std::string id = cleanPathToken(target.substr(prefix.size(), slash - prefix.size()));
    const std::string rawFileName = target.substr(slash + 1);
    const std::string fileName = cleanPathToken(rawFileName, true);
    if (id.empty() || fileName.empty() || fileName != rawFileName || fileName.find("..") != std::string::npos) {
        return false;
    }

    const std::filesystem::path filePath =
        std::filesystem::path("/tmp/tvstreammersat5-hls") / id / fileName;
    if (!std::filesystem::exists(filePath) || !std::filesystem::is_regular_file(filePath)) {
        res.result(http::status::not_found);
        res.set(http::field::content_type, "text/plain");
        res.body() = "Not Found";
        return true;
    }

    std::ifstream input(filePath, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    res.body() = buffer.str();
    boost::system::error_code endpointError;
    const std::string clientIp = socket.remote_endpoint(endpointError).address().to_string();
    if (!endpointError && !clientIp.empty()) {
        streamManager.addStreamSession(id, clientIp, "hls");
    }
    if (filePath.extension() == ".m3u8") {
        res.set(http::field::content_type, "application/vnd.apple.mpegurl");
        res.set(http::field::cache_control, "no-cache");
    } else {
        res.set(http::field::content_type, "video/MP2T");
        res.set(http::field::cache_control, "no-cache");
    }
    return true;
}


std::string HttpServer::qualityHistory(const std::string& target) {
    const std::string id = queryValue(target, "id");
    uint64_t periodSeconds = 3600;
    try {
        const std::string period = queryValue(target, "period");
        if (!period.empty()) {
            periodSeconds = std::stoull(period);
        }
    } catch (const std::exception&) {
        periodSeconds = 3600;
    }
    periodSeconds = std::clamp<uint64_t>(periodSeconds, 60, 30ULL * 24ULL * 60ULL * 60ULL);

    Json::Value root;
    root["id"] = id;
    root["period_seconds"] = Json::UInt64(periodSeconds);
    root["generated_at"] = Json::Int64(unixNowSeconds());
    Json::Value samples(Json::arrayValue);

    const int64_t cutoff = unixNowSeconds() - static_cast<int64_t>(periodSeconds);
    std::map<std::string, unsigned int> totals = {
        {"ok", 0}, {"warn", 0}, {"error", 0}, {"offline", 0}
    };

    {
        std::lock_guard<std::mutex> lock(qualityMutex);
        auto found = qualitySamples.find(id);
        if (found != qualitySamples.end()) {
            for (const auto& sample : found->second) {
                if (sample.timestamp < cutoff) {
                    continue;
                }
                Json::Value item;
                item["ts"] = Json::Int64(sample.timestamp);
                item["active"] = sample.active;
                item["input_kbps"] = Json::UInt64(sample.inputKbps);
                item["output_kbps"] = Json::UInt64(sample.outputKbps);
                item["target_kbps"] = Json::UInt64(sample.targetKbps);
                item["input_cc_errors"] = Json::UInt64(sample.inputCcErrors);
                item["output_cc_errors"] = Json::UInt64(sample.outputCcErrors);
                item["cc_errors"] = Json::UInt64(sample.inputCcErrors);
                item["status"] = sample.status;
                item["level"] = sample.level;
                item["message"] = sample.message;
                samples.append(item);
                totals[sample.level]++;
            }
        }
    }

    root["samples"] = samples;
    Json::Value summary;
    summary["ok"] = Json::UInt(totals["ok"]);
    summary["warn"] = Json::UInt(totals["warn"]);
    summary["error"] = Json::UInt(totals["error"]);
    summary["offline"] = Json::UInt(totals["offline"]);
    uint64_t inputCcErrorsTotal = 0;
    uint64_t outputCcErrorsTotal = 0;
    for (const auto& sample : samples) {
        inputCcErrorsTotal += sample.get("input_cc_errors", Json::UInt64(0)).asUInt64();
        outputCcErrorsTotal += sample.get("output_cc_errors", Json::UInt64(0)).asUInt64();
    }
    summary["input_cc_errors"] = Json::UInt64(inputCcErrorsTotal);
    summary["output_cc_errors"] = Json::UInt64(outputCcErrorsTotal);
    summary["cc_errors"] = Json::UInt64(inputCcErrorsTotal);
    root["summary"] = summary;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

void HttpServer::recordQualitySample(const StreamConfig& cfg, const Json::Value& state) {
    const int64_t now = unixNowSeconds();
    QualitySample sample;
    sample.timestamp = now;
    sample.active = state.get("active", false).asBool();
    sample.inputKbps = state.get("bitrate_in_kbps", Json::UInt64(0)).asUInt64();
    sample.outputKbps = state.get("bitrate_out_kbps", Json::UInt64(0)).asUInt64();
    sample.targetKbps = (cfg.transcodeEnabled ? tvs::protocols::muxBitrate(cfg) : cfg.targetBitrate) / 1000;
    sample.inputCcErrors = state.get("input_cc_errors", state.get("cc_errors", Json::UInt64(0))).asUInt64();
    sample.outputCcErrors = state.get("output_cc_errors", Json::UInt64(0)).asUInt64();
    sample.status = state.get("status", "").asString();

    const std::string statusLower = toLower(sample.status);
    if (!sample.active) {
        sample.level = "offline";
        sample.message = sample.status == "stopped" ? "Поток остановлен" : "Поток не активен: " + sample.status;
    } else if (statusLower.find("error") != std::string::npos ||
               statusLower.find("failed") != std::string::npos ||
               statusLower.find("ended") != std::string::npos) {
        sample.level = "error";
        sample.message = "Ошибка GStreamer: " + sample.status;
    } else if (sample.inputKbps == 0) {
        sample.level = "warn";
        sample.message = "Нет входного битрейта при активном потоке";
    } else if (sample.inputCcErrors > 0 || sample.outputCcErrors > 0) {
        sample.level = "error";
        sample.message = "CC-errors MPEG-TS: вход=" + std::to_string(sample.inputCcErrors) +
            ", выход=" + std::to_string(sample.outputCcErrors);
    } else if (sample.targetKbps > 0 && sample.outputKbps > 0) {
        const double diff = std::abs(static_cast<double>(sample.outputKbps) - static_cast<double>(sample.targetKbps));
        const double deviation = diff / static_cast<double>(sample.targetKbps);
        if (deviation > 0.20) {
            sample.level = "warn";
            sample.message = "Выходной битрейт отклоняется от цели больше чем на 20%";
        } else {
            sample.level = "ok";
            sample.message = "Качество в норме";
        }
    } else {
        sample.level = "ok";
        sample.message = "Качество в норме";
    }

    std::lock_guard<std::mutex> lock(qualityMutex);
    auto& samples = qualitySamples[cfg.id];
    if (!samples.empty() && samples.back().timestamp == sample.timestamp) {
        samples.back() = sample;
    } else {
        samples.push_back(sample);
    }

    const int64_t cutoff = now - 30LL * 24LL * 60LL * 60LL;
    while (!samples.empty() && samples.front().timestamp < cutoff) {
        samples.pop_front();
    }
}

void HttpServer::handleSaveConfig(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        std::cerr << "Invalid config payload: " << errs << std::endl;
        return;
    }
    const AppConfig previousConfig = configManager.config;
    const auto previousStreams = streamConfigById(previousConfig.streams);
    const auto managedSnapshot = streamManager.snapshot();
    AppConfig nextConfig = AppConfig::fromJson(root);
    if (!root.isMember("login") || root.get("login", "").asString().empty()) {
        nextConfig.login = configManager.config.login;
    }
    if (!root.isMember("password") || root.get("password", "").asString().empty()) {
        nextConfig.password = configManager.config.password;
    }
    if (!root.isMember("server_name") || root.get("server_name", "").asString().empty()) {
        nextConfig.serverName = configManager.config.serverName;
    }
    if (!root.isMember("http_port") || nextConfig.httpPort <= 0 || nextConfig.httpPort > 65535) {
        nextConfig.httpPort = configManager.config.httpPort;
    }
    if (!root.isMember("language")) {
        nextConfig.language = configManager.config.language;
    }
    if (!root.isMember("cam_clients")) {
        nextConfig.camClients = configManager.config.camClients;
    }
    const bool hasStreamArray = root.isMember("streams") && root["streams"].isArray();
    const bool explicitlyAllowEmptyStreams = root.get("allow_empty_streams", false).asBool();
    if ((!hasStreamArray || nextConfig.streams.empty()) &&
        !previousConfig.streams.empty() && !explicitlyAllowEmptyStreams) {
        nextConfig.streams = previousConfig.streams;
        std::cerr << "Config save preserved " << previousConfig.streams.size()
                  << " existing stream(s): request contained no streams or an empty stream list"
                  << std::endl;
    }
    const auto nextStreams = streamConfigById(nextConfig.streams);
    std::vector<std::string> streamsToStop;
    std::vector<StreamConfig> streamsToRestart;
    for (const auto& [id, state] : managedSnapshot) {
        (void)state;
        const auto next = nextStreams.find(id);
        if (next == nextStreams.end()) {
            streamsToStop.push_back(id);
            continue;
        }
        const auto previous = previousStreams.find(id);
        if (previous == previousStreams.end() || !sameStreamConfig(previous->second, next->second)) {
            streamsToRestart.push_back(next->second);
        }
    }

    configManager.config = nextConfig;
    configManager.save();
    CardManager::instance().configure(configManager.config.camClients);
    refreshHttpPorts();

    for (const auto& id : streamsToStop) {
        std::cerr << "Stopping removed stream after config save: " << id << std::endl;
        streamManager.stopStream(id);
    }
    for (const auto& stream : streamsToRestart) {
        std::cerr << "Hard restarting stream after config change: " << stream.id << std::endl;
        if (!streamManager.restartStream(stream)) {
            std::cerr << "Hard restart failed for stream: " << stream.id << std::endl;
        }
    }
}

std::string HttpServer::listBackupFiles() {
    Json::Value root;
    Json::Value files(Json::arrayValue);
    const auto directory = std::filesystem::current_path() / "backup-files";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        root["error"] = "failed to create backup-files directory";
    } else {
        struct BackupFileEntry {
            std::filesystem::path path;
            std::filesystem::file_time_type modified;
            uintmax_t size = 0;
        };
        std::vector<BackupFileEntry> entries;
        for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
            if (ec) break;
            if (!entry.is_regular_file(ec) || ec) {
                ec.clear();
                continue;
            }
            BackupFileEntry item;
            item.path = entry.path();
            item.modified = entry.last_write_time(ec);
            if (ec) {
                ec.clear();
                item.modified = std::filesystem::file_time_type::min();
            }
            item.size = entry.file_size(ec);
            if (ec) {
                ec.clear();
                item.size = 0;
            }
            entries.push_back(std::move(item));
        }
        std::sort(entries.begin(), entries.end(), [](const BackupFileEntry& left, const BackupFileEntry& right) {
            return left.modified > right.modified;
        });
        for (const auto& entry : entries) {
            Json::Value item;
            item["name"] = entry.path.filename().string();
            item["path"] = entry.path.string();
            item["size"] = Json::UInt64(entry.size);
            files.append(item);
        }
    }
    root["files"] = files;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::handleUploadBackupFile(const std::string& target, const std::string& body) {
    Json::Value root;
    if (body.empty()) {
        root["error"] = "empty file";
    } else {
        const std::string streamId = cleanPathToken(urlDecode(queryValue(target, "stream_id")));
        const std::string requestedName = cleanFileName(urlDecode(queryValue(target, "filename")));
        const std::string prefix = streamId.empty() ? "stream" : streamId;
        const auto directory = std::filesystem::current_path() / "backup-files";
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        if (ec) {
            root["error"] = "failed to create backup-files directory";
        } else {
            const auto destination = directory /
                (prefix + "-" + std::to_string(unixNowSeconds()) + "-" + requestedName);
            std::ofstream output(destination, std::ios::binary);
            if (!output.is_open()) {
                root["error"] = "failed to open destination file";
            } else {
                output.write(body.data(), static_cast<std::streamsize>(body.size()));
                output.close();
                root["path"] = destination.string();
                root["filename"] = requestedName;
                root["size"] = Json::UInt64(body.size());
            }
        }
    }

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::handleDeleteBackupFile(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["error"] = "invalid request";
    } else {
        const std::string requestedName = request.get("name", "").asString();
        const std::string safeName = cleanFileName(requestedName);
        if (requestedName.empty() || safeName != requestedName) {
            response["error"] = "invalid filename";
        } else {
            const auto directory = std::filesystem::current_path() / "backup-files";
            const auto filePath = directory / safeName;
            std::error_code ec;
            if (!std::filesystem::exists(filePath, ec) || ec || !std::filesystem::is_regular_file(filePath, ec)) {
                response["error"] = "file not found";
            } else if (!std::filesystem::remove(filePath, ec) || ec) {
                response["error"] = "failed to delete file";
            } else {
                response["result"] = "ok";
                response["name"] = safeName;
                response["path"] = filePath.string();
            }
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleStartStream(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        const std::string message = "Invalid start-stream payload: " + errs;
        std::cerr << message << std::endl;
        response["result"] = "error";
        response["error"] = message;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    const std::string streamId = root.get("id", "").asString();
    const auto configuredStream = std::find_if(
        configManager.config.streams.begin(), configManager.config.streams.end(),
        [&streamId](const StreamConfig& stream) { return stream.id == streamId; });
    if (streamId.empty() || configuredStream == configManager.config.streams.end()) {
        const std::string message = streamId.empty()
            ? "missing stream id"
            : ("stream is not present in saved configuration: " + streamId);
        std::cerr << "Start stream rejected: " << message << std::endl;
        response["result"] = "error";
        response["error"] = message;
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, response);
    }

    const StreamConfig cfg = *configuredStream;
    std::string startError;
    bool started = streamManager.startStream(cfg, &startError);
    if (!started && streamManager.isStreamActive(cfg.id)) started = true;

    response["stream_id"] = cfg.id;
    if (started) {
        response["result"] = "ok";
        if (!cfg.conditionalAccessClient.empty()) {
            response["ca"] = CardManager::instance().streamState(cfg.id);
        }
    } else {
        const std::string message = startError.empty()
            ? ("Failed to start stream: " + (cfg.name.empty() ? cfg.id : cfg.name))
            : startError;
        std::cerr << "Start stream failed: id=" << cfg.id
                  << " name=" << (cfg.name.empty() ? cfg.id : cfg.name)
                  << " error=" << message << std::endl;
        response["result"] = "error";
        response["error"] = message;
    }

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

std::string HttpServer::handleStopStream(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        const std::string message = "Invalid stop-stream payload: " + errs;
        std::cerr << message << std::endl;
        response["result"] = "error";
        response["error"] = message;
    } else {
        const std::string id = root.get("id", "").asString();
        if (id.empty()) {
            response["result"] = "error";
            response["error"] = "missing stream id";
        } else {
            const bool stopped = streamManager.stopStreamAsync(id);
            const bool configured = std::any_of(
                configManager.config.streams.begin(), configManager.config.streams.end(),
                [&id](const StreamConfig& stream) { return stream.id == id; });
            if (stopped || configured) {
                response["result"] = "ok";
                response["already_stopped"] = !stopped;
                response["status"] = "stopped";
            } else {
                response["result"] = "error";
                response["error"] = "stream is not configured: " + id;
            }
            response["stream_id"] = id;
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

void HttpServer::handleRestartProgram() {
    const auto args = currentProcessArgs();
    std::thread([this, args]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cerr << "Restarting TVStreammerSAT5 process" << std::endl;
        streamManager.stopAll();
        closeInheritedFileDescriptors();
        execCurrentProcess(args);
    }).detach();
}

  void HttpServer::handleDeleteStream(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
      std::cerr << "Invalid delete-stream payload: " << errs << std::endl;
      return;
    }
    const std::string id = root.get("id", "").asString();
    if (id.empty()) return;
    streamManager.stopStream(id);
    auto& streams = configManager.config.streams;
    streams.erase(std::remove_if(streams.begin(), streams.end(), [&id](const StreamConfig& stream) {
      return stream.id == id;
    }), streams.end());
    configManager.save();
  }

  void HttpServer::handleSaveSubscribers(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
      std::cerr << "Invalid subscribers payload: " << errs << std::endl;
      return;
    }
    SubscriberListConfig next;
    next.filteringEnabled = root.get("filtering_enabled", false).asBool();
    if (root["subscribers"].isArray()) {
      for (const auto& item : root["subscribers"]) {
        auto subscriber = SubscriberConfig::fromJson(item);
        if (!subscriber.name.empty() && (!subscriber.primaryIp.empty() || !subscriber.backupIp.empty())) {
          next.subscribers.push_back(std::move(subscriber));
        }
      }
    }
    const bool filteringModeChanged = configManager.subscribers.filteringEnabled != next.filteringEnabled;
    if (configManager.subscribers.toJson() == next.toJson()) {
      std::cerr << "Subscriber update unchanged; skipping session reset" << std::endl;
      return;
    }

    configManager.subscribers = std::move(next);
    configManager.saveSubscribers();
    const size_t reset = streamManager.enforceSubscriberAccess();
    if (reset > 0) {
      std::cerr << "Reset unauthorized stream sessions after subscriber update: " << reset << std::endl;
    }
    if (filteringModeChanged) {
      const size_t restarted = streamManager.restartAllSrtOutputs();
      std::cerr << "Restarted SRT outputs after filtering mode change: " << restarted << std::endl;
    }
  }

  void HttpServer::handleResetSubscriber(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) return;
    const std::string name = root.get("name", "").asString();
    for (const auto& subscriber : configManager.subscribers.subscribers) {
      if (subscriber.name != name) continue;
      size_t reset = 0;
      if (!subscriber.primaryIp.empty()) reset += streamManager.resetHttpSessions(subscriber.primaryIp);
      if (!subscriber.backupIp.empty() && subscriber.backupIp != subscriber.primaryIp) {
        reset += streamManager.resetHttpSessions(subscriber.backupIp);
      }
      const size_t restarted = streamManager.restartSrtOutputsForStreams(subscriber.streamIds);
      std::cerr << "Reset subscriber sessions: " << name << " (" << reset
                << "), restarted_srt_outputs=" << restarted << std::endl;
      return;
    }
  }

void HttpServer::addEndpoint(const std::string& path, std::function<void(const boost::asio::ip::tcp::socket&)> handler) {
    // Store endpoint handler for future use
    // This is a simple implementation - in a real server you'd want proper routing
    endpointHandlers[path] = handler;
}


std::string HttpServer::renderIndexPage() {
    static const std::string html = R"HTML(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="X-UA-Compatible" content="IE=edge">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>TVStreammerSAT5</title>
<style>
html{font-size:14px}
body{font-family:Arial,Helvetica,sans-serif;background:#0f1218;color:#EEE;margin:0;padding:0;min-height:100vh}
body:before{content:'';position:fixed;inset:0;background:radial-gradient(circle at top left,rgba(40,160,255,.18),transparent 28%),radial-gradient(circle at top right,rgba(120,90,255,.15),transparent 22%),linear-gradient(180deg,#10131a 0%,#090c12 100%);pointer-events:none;z-index:-1}
header{position:relative;z-index:100000;overflow:visible;display:flex;align-items:center;justify-content:space-between;padding:8px 10px;background:rgba(19,23,31,.95);backdrop-filter:blur(10px);border-bottom:1px solid rgba(255,255,255,.06);gap:12px;flex-wrap:wrap}
.header-left{display:flex;align-items:flex-start;gap:10px}
.header-left .title{font-size:1.05rem;font-weight:700;letter-spacing:.02em;color:#fff}
.header-left .subtitle{font-size:.78rem;color:#9aa3b1;margin-top:2px}
.header-right{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.header-center{display:flex;align-items:center;justify-content:center;gap:12px}
.system-load{display:flex;flex-direction:column;align-items:stretch;justify-content:center;gap:2px;min-width:86px;padding:6px 10px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08);border-radius:12px;color:#d1d9ed;font-size:.76rem;white-space:nowrap}
.system-load strong{color:#fff;font-size:.76rem}
.system-load .metric{display:flex;align-items:center;justify-content:space-between;gap:10px;width:100%;line-height:1.2}
.system-load .metric span{color:#7dd1ff;font-weight:700;min-width:38px;text-align:right}
.network-button{padding:7px 10px;border:1px solid rgba(57,189,248,.28);border-radius:10px;color:#bdefff;background:rgba(57,189,248,.12);cursor:pointer;font-size:.78rem;white-space:nowrap}
.network-button:hover{background:rgba(57,189,248,.24)}
.restart-button{border-color:rgba(255,184,77,.28);color:#ffe0a3;background:rgba(255,184,77,.1)}
.restart-button:hover{background:rgba(255,184,77,.2);border-color:rgba(255,184,77,.38)}
.restart-button:disabled{opacity:.65;cursor:wait}
.system-menu{position:relative;z-index:100001}
.system-menu summary{list-style:none;display:flex;align-items:center;gap:8px}
.system-menu summary::-webkit-details-marker{display:none}
.system-menu summary:after{content:'';width:0;height:0;border-left:4px solid transparent;border-right:4px solid transparent;border-top:5px solid currentColor;opacity:.8}
.system-menu[open] summary{background:rgba(255,255,255,.1);border-color:rgba(255,255,255,.24)}
.system-menu-list{position:absolute;right:0;top:calc(100% + 6px);z-index:100001;min-width:178px;padding:6px;background:#121825;border:1px solid rgba(255,255,255,.12);border-radius:10px;box-shadow:0 18px 42px rgba(0,0,0,.28)}
.system-menu-item{display:block;width:100%;padding:8px 10px;border:0;border-radius:7px;background:transparent;color:#e7edf8;text-align:left;font-size:.82rem;cursor:pointer}
.system-menu-item:hover{background:rgba(255,255,255,.08)}
.system-menu-item.restart-button{color:#ffe0a3;background:transparent;border:0}
.system-menu-item.restart-button:hover{background:rgba(255,184,77,.12)}
.button-primary{padding:8px 14px;border:none;border-radius:999px;color:#FFF;background:#1f8bff;cursor:pointer;font-size:0.88rem;transition:background .2s ease,color .2s ease,box-shadow .2s ease,opacity .2s ease}
.button-secondary{padding:7px 12px;border:1px solid rgba(255,255,255,.14);border-radius:999px;color:#EEE;background:rgba(255,255,255,.05);cursor:pointer;font-size:0.82rem;transition:background .2s ease,border-color .2s ease}
.button-primary:hover{background:#0f7ce7}
.button-primary.save-clean{background:rgba(255,255,255,.08);color:#9aa3b1;box-shadow:inset 0 0 0 1px rgba(255,255,255,.12)}
.button-primary.save-clean:hover{background:rgba(255,255,255,.08)}
.button-primary.save-dirty{background:#ffbd4a;color:#161b25;box-shadow:0 0 0 2px rgba(255,189,74,.18)}
.button-primary.save-dirty:hover{background:#ffc968}
.button-secondary:hover{background:rgba(255,255,255,.1);border-color:rgba(255,255,255,.24)}
.container{padding:10px 12px 12px;max-width:1180px;margin:0 auto}
.stats-panel{display:grid;grid-template-columns:repeat(2,minmax(100px,1fr));gap:10px;padding:8px 12px;background:rgba(255,255,255,.04);border:1px solid rgba(255,255,255,.08);border-radius:16px}
.stats-panel .status{display:flex;flex-direction:column;gap:3px;font-size:.78rem;color:#d1d9ed}
.stats-panel .status strong{color:#fff;font-size:.78rem}
.stats-panel .status span{font-size:1rem;font-weight:700;color:#fff}
.tile-grid{display:grid;grid-template-columns:repeat(auto-fill, minmax(calc(180px * 1.15), 1fr));gap:12px 1ch;justify-content:start}
.tile{position:relative;background:rgba(22,27,37,.94);padding:8px 10px 8px 16px;border-radius:18px;border:1px solid rgba(255,255,255,.06);display:flex;flex-direction:column;gap:4px;height:252px;min-height:252px;width:100%;max-width:none;box-sizing:border-box;box-shadow:0 18px 42px rgba(0,0,0,.14);transition:transform .2s ease,border-color .2s ease;font-size:11px}
.tile:before{content:'';position:absolute;left:0;top:12px;bottom:12px;width:4px;border-radius:999px;background:linear-gradient(180deg,#3fc8ff,#1d69ff)}
.tile:hover{transform:translateY(-1px);border-color:rgba(31,136,255,.3)}
.tile.active{border-color:#17c261}
.tile.error{border-color:#fb5f5f}
.tile .top{display:flex;align-items:flex-start;justify-content:space-between;gap:4px;padding-right:64px;min-height:27px}
.tile .tile-actions{position:absolute;top:8px;right:8px;display:flex;align-items:center;gap:5px;z-index:2}
.tile .delete-button{display:inline-flex;align-items:center;justify-content:center;position:static;width:18px;height:18px;padding:0;border:0;border-radius:50%;background:#d9363e;color:#fff;font-family:Arial,Helvetica,sans-serif;font-size:15px;font-weight:700;line-height:1;cursor:pointer;box-shadow:0 3px 8px rgba(0,0,0,.24)}
.tile .delete-button:hover{background:#f0444d;transform:scale(1.08)}
.tile .title{font-size:11px;font-weight:700;line-height:1.2;color:#fff;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.tile .badge{position:static;transform:none;padding:2px 5px;background:rgba(20,161,255,.14);color:#7dd1ff;border-radius:999px;font-size:9px;line-height:12px;text-transform:uppercase;letter-spacing:.06em;white-space:nowrap}
.tile .dvb-meters{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:5px;width:100%}
.tile .dvb-meters.placeholder{visibility:hidden}
.tile .dvb-meter{position:relative;height:13px;overflow:hidden;border-radius:999px;background:rgba(255,255,255,.07);box-shadow:inset 0 0 0 1px rgba(255,255,255,.08)}
.tile .dvb-meter-fill{position:absolute;left:0;top:0;bottom:0;width:0%;border-radius:inherit;transition:width .25s ease,background .25s ease;opacity:.9}
.tile .dvb-meter-label{position:relative;z-index:1;display:flex;align-items:center;justify-content:center;height:100%;font-size:8px;font-weight:800;line-height:13px;color:#fff;text-shadow:0 1px 2px rgba(0,0,0,.9);letter-spacing:.02em;white-space:nowrap}
.tile .status-line{display:flex;align-items:center;gap:5px;min-width:0;margin-top:2px;line-height:12px}.tile .status-pill{flex:0 0 auto;padding:1px 5px;background:rgba(255,255,255,.06);color:#c9d2e4;border-radius:999px;font-size:9px;text-transform:uppercase;letter-spacing:.06em}.tile .runtime-status{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#9ca8bb;font-size:9px}
.tile .status-pill.active{background:rgba(23,194,97,.15);color:#b6f7c2}
.tile .status-pill.stopped{background:rgba(255,95,95,.14);color:#ffb3b3}
.tile .decode-pill{display:inline-flex;align-items:center;justify-content:center;min-width:64px;padding:1px 5px;border-radius:999px;font-size:8px;font-weight:800;line-height:10px;letter-spacing:.035em;white-space:nowrap}
.tile .info-row.decode-row strong{font-size:9px;line-height:10px}
.tile .decode-pill.clear{background:rgba(23,194,97,.18);color:#bdf8cb;box-shadow:inset 0 0 0 1px rgba(23,194,97,.28)}
.tile .decode-pill.scrambled{background:rgba(255,95,95,.18);color:#ffc2c2;box-shadow:inset 0 0 0 1px rgba(255,95,95,.3)}
.tile .decode-pill.waiting{background:rgba(255,184,77,.16);color:#ffe0a3;box-shadow:inset 0 0 0 1px rgba(255,184,77,.24)}
.tile .decode-pill.offline{background:rgba(255,255,255,.07);color:#aeb7c8;box-shadow:inset 0 0 0 1px rgba(255,255,255,.1)}
.tile .info{display:grid;grid-template-columns:1fr;gap:3px;font-size:11px;color:#b3b8c6}
.tile .info-row{display:flex;justify-content:space-between;gap:8px;align-items:center}
.tile .info-row strong{color:#fff;font-size:11px}
.tile .info-row span{max-width:140px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;text-align:right}
.tile .info-row.placeholder{visibility:hidden}
.tile .controls{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:6px;margin-top:0}
.tile .controls button{padding:5px 8px;border:none;border-radius:10px;background:rgba(255,255,255,.06);color:#EEE;font-size:9px;cursor:pointer;transition:background .2s ease,transform .08s ease,box-shadow .2s ease}
.tile .controls button:hover{background:rgba(255,255,255,.12)}
.tile .controls button:active{transform:translateY(1px) scale(.98)}
.tile .controls .start-button{background:rgba(23,194,97,.18);color:#bdf8cb;box-shadow:inset 0 0 0 1px rgba(23,194,97,.26)}
.tile .controls .start-button:hover{background:rgba(23,194,97,.28)}
.tile .controls .stop-button{background:rgba(255,95,95,.18);color:#ffc2c2;box-shadow:inset 0 0 0 1px rgba(255,95,95,.28)}
.tile .controls .stop-button:hover{background:rgba(255,95,95,.28)}
.tile .controls .copy-button.copied{background:rgba(23,194,97,.38);color:#fff;box-shadow:0 0 0 2px rgba(23,194,97,.28)}
.tile .controls .copy-button.copy-error{background:rgba(255,184,77,.24);color:#ffe0a3;box-shadow:0 0 0 2px rgba(255,184,77,.22)}
.tile .controls .quality-button{background:rgba(57,189,248,.14);color:#bdefff;box-shadow:inset 0 0 0 1px rgba(57,189,248,.2)}
.tile .controls .quality-button:hover{background:rgba(57,189,248,.24)}
.modal{position:fixed;top:var(--header-height,58px);left:0;right:0;bottom:0;background:rgba(8,10,15,.78);display:none;align-items:flex-start;justify-content:center;padding:12px;overflow:auto;z-index:20;box-sizing:border-box}
.modal.quality-open,.modal.stream-open{top:var(--header-height,58px);height:auto;align-items:flex-start;overflow:auto;padding-top:12px}
.modal.active{display:flex}
.modal-content{position:relative;background:rgba(11,15,22,.985);padding:18px 18px;border-radius:22px;width:min(520px,100%);max-height:calc(100vh - var(--header-height,58px) - 24px);overflow:auto;box-shadow:0 28px 70px rgba(0,0,0,.24);border:1px solid rgba(255,255,255,.08);box-sizing:border-box;margin:0 auto}
.modal-close{display:inline-flex;align-items:center;justify-content:center;position:absolute;top:8px;right:8px;width:20px;height:20px;padding:0;border:0;border-radius:50%;background:#d9363e;color:#fff;font-family:Arial,Helvetica,sans-serif;font-size:16px;font-weight:700;line-height:1;cursor:pointer;z-index:20;box-shadow:0 2px 7px rgba(0,0,0,.28)}
.modal-close:hover{background:#f0444d;transform:scale(1.08)}
.modal-content.stream-modal{width:min(680px,100%);max-height:calc(100% - 12px);margin:0 auto}
.modal-content.quality-modal{width:min(1240px,100%);background:rgba(9,13,20,.99);max-height:calc(100% - 12px);margin:0 auto}
.modal-content.network-modal{width:min(620px,100%)}
.modal-content.subscriber-modal{width:min(1280px,100%);max-height:98%}
.modal-content h2{margin-top:0;font-size:1.25rem;margin-bottom:14px;color:#fff}
.quality-head{display:flex;justify-content:space-between;gap:12px;align-items:flex-start;flex-wrap:wrap;margin-bottom:10px}
.quality-title{display:flex;flex-direction:column;gap:4px}
.quality-title small{color:#9aa3b1}
.quality-toolbar{display:flex;justify-content:flex-end;align-items:flex-start;gap:8px;flex-wrap:wrap}
.period-tabs{display:flex;gap:6px;flex-wrap:wrap}
.period-tabs button{padding:6px 8px;border:1px solid rgba(255,255,255,.1);background:rgba(255,255,255,.05);color:#d7deec;border-radius:8px;cursor:pointer;font-size:.72rem}
.period-tabs button.active{background:#1f8bff;color:#fff;border-color:#1f8bff}
.quality-refresh{display:flex;align-items:center;gap:6px;color:#9aa3b1;font-size:.72rem;white-space:nowrap}
.quality-refresh select{padding:6px 8px;border:1px solid rgba(255,255,255,.1);background:#121825;color:#d7deec;border-radius:8px;font-size:.72rem}
.quality-board{position:relative;border:1px solid rgba(255,255,255,.1);background:#0f1622;border-radius:10px;padding:10px 10px 8px}
.quality-board canvas{display:block;width:100%;height:320px;cursor:copy}
.quality-legend{display:grid;grid-template-columns:1fr auto auto auto auto;gap:8px 18px;align-items:center;margin:10px 0 0;color:#cfd8ea;font-size:.78rem}
.quality-legend .legend-head{color:#8f9bad;font-weight:700;text-align:right}
.quality-legend .legend-name{display:flex;align-items:center;gap:6px;min-width:0}
.quality-legend span{white-space:nowrap}
.quality-dot{width:9px;height:9px;border-radius:50%;display:inline-block}
.quality-ok{background:#17c261}.quality-warn{background:#ffbd4a}.quality-error{background:#ff5f5f}.quality-offline{background:#7c879b}
.quality-line{width:10px;height:10px;border-radius:2px;display:inline-block;box-shadow:0 0 0 1px rgba(255,255,255,.18)}
.quality-input{background:#26ef46}.quality-output{background:#36a3ff}.quality-input-cc{background:#ff9f1a}.quality-output-cc{background:#ff4f9a}
.quality-decode{display:grid;gap:5px;margin:8px 0 10px;padding:8px 10px;background:rgba(255,255,255,.045);border:1px solid rgba(255,255,255,.07);border-radius:8px;color:#cfd8ea;font-size:.78rem;line-height:1.35}
.quality-decode strong{color:#fff}
.quality-details{margin-top:10px}
.quality-stats{display:grid;grid-template-columns:minmax(240px,1fr) 52px repeat(4,70px);gap:4px 10px;align-items:center;color:#cfd8ea;font-size:.78rem}
.quality-stats .metric-name{display:flex;align-items:center;gap:6px;min-width:0;overflow-wrap:anywhere}
.quality-stats .unit,.quality-stats .value,.quality-stats .head{text-align:right}
.quality-stats .head{color:#8f9bad;font-weight:700}
.quality-card{background:rgba(255,255,255,.045);border:1px solid rgba(255,255,255,.07);border-radius:8px;padding:8px;color:#cfd8ea;font-size:.78rem}
.quality-card strong{display:block;color:#fff;margin-bottom:4px}
.quality-errors{margin-top:10px;max-height:150px;overflow:auto;border-top:1px solid rgba(255,255,255,.08);padding-top:8px;color:#cfd8ea;font-size:.78rem}
.quality-errors div{display:flex;gap:8px;padding:3px 0}
.quality-empty{padding:30px;text-align:center;color:#9aa3b1}
.quality-copy{color:#7dd1ff;font-size:.78rem;min-height:18px;margin-top:-4px}
@media (max-width:760px){.quality-toolbar{justify-content:flex-start}.quality-board canvas{height:260px}.quality-legend{grid-template-columns:1fr 48px repeat(4,56px);overflow-x:auto}.quality-stats{grid-template-columns:minmax(190px,1fr) 48px repeat(4,56px);overflow-x:auto}}
.form-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.form-grid.full{grid-template-columns:1fr}
.form-row.full{grid-column:1/-1}
.form-row{display:flex;flex-direction:column;gap:8px;align-items:flex-start}
.form-row label{font-size:.78rem;color:#9aa3b1}
.form-row input,.form-row select{width:100%;max-width:210px;padding:7px 9px;background:#121825;border:1px solid rgba(255,255,255,.08);border-radius:8px;color:#EEE;font-size:.8rem}
.form-row input.compact,.form-row select.compact{max-width:150px}
.input-main-row{display:grid;grid-template-columns:minmax(260px,1fr) minmax(150px,190px) minmax(130px,150px);gap:8px;width:100%;align-items:end}
.input-main-row input,.input-main-row select{box-sizing:border-box;max-width:none}
.input-main-row .form-row{min-width:0}
.row-inline{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.row-inline.compact-row input{width:100%;padding:7px 9px}
.output-list{display:grid;gap:8px;width:100%}
.output-row{display:grid;grid-template-columns:minmax(120px,1.1fr) minmax(106px,.8fr) minmax(130px,1fr) 86px 30px;gap:6px;align-items:end;padding:8px;background:rgba(255,255,255,.035);border:1px solid rgba(255,255,255,.07);border-radius:8px}
.output-row .form-row{gap:5px}
.output-row input,.output-row select{box-sizing:border-box;max-width:none}
.output-row .remove-output{width:30px;height:30px;padding:0;border:0;border-radius:8px;background:rgba(255,95,95,.18);color:#ffc2c2;cursor:pointer}
.output-row .remove-output:disabled{opacity:.35;cursor:not-allowed}
@media (max-width:760px){.output-row{grid-template-columns:1fr 1fr}.output-row .remove-output{align-self:end}}
.backup-source{display:grid;grid-template-columns:140px minmax(220px,1fr);gap:8px;width:100%}
.backup-source input,.backup-source select{box-sizing:border-box;max-width:none}
.backup-file-row{grid-column:1/-1;display:flex;align-items:center;gap:10px;min-height:32px}
.backup-file-row input{padding:0;background:transparent;border:0;color:#cfd8ea}
.backup-file-row span{color:#7dd1ff;font-size:.78rem;overflow-wrap:anywhere}
.backup-library{grid-column:1/-1;position:relative;width:100%}
.backup-library-button{width:100%;padding:8px 10px;border:1px solid rgba(255,255,255,.1);border-radius:8px;background:#121825;color:#e7edf8;text-align:left;cursor:pointer}
.backup-library-menu{display:none;position:absolute;left:0;right:0;top:calc(100% + 4px);z-index:10020;max-height:230px;overflow:auto;padding:5px;background:#121825;border:1px solid rgba(255,255,255,.14);border-radius:9px;box-shadow:0 16px 34px rgba(0,0,0,.4)}
.backup-library.open .backup-library-menu{display:block}
.backup-library-item{display:grid;grid-template-columns:minmax(0,1fr) 26px;gap:6px;align-items:center;padding:3px}
.backup-library-select{min-width:0;padding:7px 8px;border:0;border-radius:6px;background:transparent;color:#e7edf8;text-align:left;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;cursor:pointer}
.backup-library-select:hover{background:rgba(255,255,255,.08)}
.backup-library-delete{width:26px;height:26px;padding:0;border:0;border-radius:50%;background:#d9363e;color:#fff;font-size:15px;line-height:26px;cursor:pointer}
.backup-library-delete:hover{background:#f0444d}
.backup-library-empty{padding:9px;color:#9aa3b1;font-size:.78rem}
@media (max-width:760px){.input-main-row{grid-template-columns:1fr}.backup-source{grid-template-columns:1fr}.backup-file-row{flex-direction:column;align-items:flex-start}}
.form-row-inline small-field input{width:calc(100% - 8px)}
.form-row .checkbox-inline{display:flex;align-items:center;gap:10px;margin-top:8px}
.form-row .checkbox-inline input{width:16px;height:16px}
.modal-actions{display:flex;justify-content:flex-end;gap:10px;margin-top:16px}
.modal-actions button{min-width:100px;padding:8px 12px}
.about-list{display:grid;gap:10px;margin:4px 0 0}
.about-row{display:grid;grid-template-columns:120px 1fr;gap:12px;padding:9px 0;border-bottom:1px solid rgba(255,255,255,.08);font-size:.9rem}
.about-row:last-child{border-bottom:none}
.about-row strong{color:#9aa3b1;font-weight:600}
.about-row span,.about-row a{color:#fff;text-decoration:none;overflow-wrap:anywhere}
.about-row a:hover{color:#7dd1ff}
.about-donate{align-items:start}
.about-donate-content{display:grid;grid-template-columns:148px minmax(0,1fr);gap:12px;align-items:center}
.about-qr{width:148px;height:148px;display:block;background:#fff;border-radius:8px;padding:8px;box-sizing:border-box}
.about-donate-wallet{display:flex;flex-direction:column;gap:6px;min-width:0}
.about-donate-wallet-label{font-size:.82rem;font-weight:700;color:#9aa3b1}
.about-donate-address{font-family:monospace;font-size:.82rem;line-height:1.45}
@media (max-width:560px){.about-donate-content{grid-template-columns:1fr}.about-qr{width:132px;height:132px}}
.network-table{width:100%;border-collapse:collapse;color:#d7deec;font-size:.85rem}
.network-table th,.network-table td{padding:9px 6px;text-align:right;border-bottom:1px solid rgba(255,255,255,.08)}
.network-table th:first-child,.network-table td:first-child{text-align:left}
.network-table th{color:#9aa3b1;font-weight:600}
.network-empty{padding:22px 0;text-align:center;color:#9aa3b1}
.subscriber-list{display:grid;gap:8px;min-width:1020px}
.subscriber-row{display:grid;grid-template-columns:minmax(190px,1.8fr) minmax(180px,1fr) minmax(180px,1fr) minmax(120px,auto) 86px minmax(145px,auto) 34px;gap:6px;align-items:center}
.subscriber-row input{width:100%;box-sizing:border-box;padding:7px 8px;background:#121825;border:1px solid rgba(255,255,255,.08);border-radius:8px;color:#EEE;font-size:.78rem}
.subscriber-row .remove-subscriber{width:30px;height:30px;padding:0;border:0;border-radius:8px;background:rgba(255,95,95,.18);color:#ffc2c2;cursor:pointer}
.subscriber-head{display:grid;grid-template-columns:minmax(190px,1.8fr) minmax(180px,1fr) minmax(180px,1fr) minmax(120px,auto) 86px minmax(145px,auto) 34px;gap:6px;color:#9aa3b1;font-size:.72rem;margin-bottom:4px;min-width:1020px}
.subscriber-streams{grid-column:1/-1;display:flex;gap:5px;flex-wrap:wrap;padding:4px 0 8px 4px;border-bottom:1px solid rgba(255,255,255,.08)}
.subscriber-streams label{display:flex;align-items:center;gap:4px;color:#cfd8ea;font-size:.72rem}
.subscriber-stream-picker{position:relative;min-width:0}
.subscriber-stream-picker summary{display:inline-flex;align-items:center;gap:6px;padding:6px 9px;border:1px solid rgba(255,255,255,.1);border-radius:8px;background:rgba(255,255,255,.05);color:#d7deec;font-size:.76rem;cursor:pointer;list-style:none}
.subscriber-stream-picker summary::-webkit-details-marker{display:none}
.subscriber-stream-picker summary:after{content:'▾';color:#9aa3b1}
.subscriber-stream-options{position:absolute;left:0;right:auto;top:calc(100% + 4px);z-index:30;display:grid;gap:5px;min-width:180px;margin-top:0;padding:8px;background:#121825;border:1px solid rgba(255,255,255,.1);border-radius:8px;box-shadow:0 14px 30px rgba(0,0,0,.28)}
.subscriber-stream-options label{display:flex;align-items:center;justify-content:flex-start;gap:8px;width:100%;color:#cfd8ea;font-size:.76rem;text-align:left;white-space:nowrap;cursor:pointer}
.subscriber-stream-options input[type="checkbox"]{flex:0 0 15px;width:15px;height:15px;margin:0}
.subscriber-enabled{display:flex;align-items:center;justify-content:center;gap:4px;color:#b6f7c2;font-size:.72rem;white-space:nowrap}
.subscriber-enabled input{width:15px;height:15px}
.subscriber-session{display:flex;align-items:center;gap:6px;white-space:nowrap;color:#9aa3b1;font-size:.72rem}
.subscriber-session.active{color:#b6f7c2}
.reset-session{padding:5px 7px;border:1px solid rgba(255,184,77,.25);border-radius:7px;background:rgba(255,184,77,.12);color:#ffe0a3;cursor:pointer;font-size:.7rem}

.modal.satellite-open{top:var(--header-height,58px);height:auto;align-items:flex-start;overflow:auto;padding-top:12px}
.modal-content.satellite-modal{width:min(920px,100%);max-height:calc(100% - 12px);margin:0 auto}
.sat-signal-panel{display:grid;grid-template-columns:repeat(2,minmax(0,1fr)) auto;gap:10px;align-items:center;margin-bottom:14px;padding:12px;background:rgba(31,139,255,.08);border:1px solid rgba(57,189,248,.18);border-radius:14px}
.sat-meter{display:grid;gap:5px}.sat-meter-head{display:flex;justify-content:space-between;gap:8px;color:#cfd8ea;font-size:.78rem}.sat-meter-head strong{color:#fff}.sat-bar{height:9px;background:rgba(255,255,255,.07);border-radius:999px;overflow:hidden}.sat-bar>span{display:block;height:100%;width:0;background:linear-gradient(90deg,#fb5f5f,#ffbd4a,#17c261);transition:width .25s ease}.sat-lock{padding:6px 9px;border-radius:999px;background:rgba(255,95,95,.14);color:#ffb3b3;font-size:.72rem;white-space:nowrap}.sat-lock.locked{background:rgba(23,194,97,.15);color:#b6f7c2}
.sat-form{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:9px}.sat-field{display:flex;flex-direction:column;gap:5px}.sat-field label{color:#9aa3b1;font-size:.72rem}.sat-field input,.sat-field select{width:100%;box-sizing:border-box;padding:8px 9px;background:#121825;border:1px solid rgba(255,255,255,.1);border-radius:8px;color:#eee}.sat-field.wide{grid-column:span 2}.sat-actions{display:flex;align-items:center;gap:8px;flex-wrap:wrap;margin:12px 0}.sat-scan-status{color:#9aa3b1;font-size:.78rem}.sat-services{max-height:320px;overflow:auto;border:1px solid rgba(255,255,255,.08);border-radius:12px}.sat-service-head,.sat-service-row{display:grid;grid-template-columns:34px minmax(170px,1.8fr) minmax(110px,1fr) 92px 72px 72px;gap:8px;align-items:center;padding:8px 10px}.sat-service-head{position:sticky;top:0;background:#121825;color:#9aa3b1;font-size:.7rem;z-index:1}.sat-service-row{border-top:1px solid rgba(255,255,255,.06);font-size:.78rem}.sat-service-row:hover{background:rgba(255,255,255,.035)}.sat-service-row input[type=checkbox]{width:16px;height:16px}.sat-service-name{color:#fff;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.sat-service-provider{color:#c5cada;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.sat-access{display:inline-flex;align-items:center;justify-content:center;min-width:58px;padding:3px 7px;border-radius:999px;font-size:.68rem;font-weight:800;letter-spacing:.02em}.sat-access.fta{color:#8ff0b5;background:rgba(34,197,94,.14);border:1px solid rgba(34,197,94,.38)}.sat-access.ca{color:#ff9da5;background:rgba(239,68,68,.14);border:1px solid rgba(239,68,68,.38)}.sat-access.unknown{color:#ffd78a;background:rgba(245,158,11,.12);border:1px solid rgba(245,158,11,.34)}.sat-empty{padding:28px 12px;text-align:center;color:#9aa3b1}.sat-output{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:9px;margin-top:12px;padding-top:12px;border-top:1px solid rgba(255,255,255,.08)}
.cam-panel{margin:10px 0 4px;padding:10px 12px;border:1px solid rgba(168,85,247,.24);border-radius:12px;background:rgba(126,34,206,.07)}
.cam-head{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:8px}.cam-head strong{color:#e9ddff}.cam-list{display:grid;gap:6px}.cam-row{display:grid;grid-template-columns:86px minmax(160px,1fr) auto;gap:8px;align-items:center;padding:7px 8px;border-radius:9px;background:rgba(255,255,255,.035);font-size:.75rem}.cam-row .cam-name{font-weight:800;color:#fff}.cam-device{min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;color:#aeb8ca}.cam-state{display:inline-flex;align-items:center;justify-content:center;padding:3px 7px;border-radius:999px;font-size:.66rem;font-weight:800;white-space:nowrap}.cam-state.card{color:#9ef3bd;background:rgba(34,197,94,.14);border:1px solid rgba(34,197,94,.4)}.cam-state.no-card{color:#ffb3b8;background:rgba(239,68,68,.13);border:1px solid rgba(239,68,68,.36)}.cam-state.busy{color:#a8dcff;background:rgba(56,189,248,.12);border:1px solid rgba(56,189,248,.32)}.cam-state.detected{color:#d7c9ff;background:rgba(168,85,247,.12);border:1px solid rgba(168,85,247,.32)}.cam-state.warn{color:#ffd88c;background:rgba(245,158,11,.12);border:1px solid rgba(245,158,11,.32)}.cam-controls{grid-column:1/-1;display:flex;align-items:center;gap:10px;flex-wrap:wrap;padding-top:4px;border-top:1px solid rgba(255,255,255,.06);font-size:.68rem;color:#aeb8ca}.cam-controls label{display:inline-flex;align-items:center;gap:4px;white-space:nowrap}.cam-controls input[type=number]{width:58px;padding:3px 5px;border-radius:6px;border:1px solid rgba(255,255,255,.14);background:#111723;color:#fff}.cam-controls select{max-width:210px;padding:3px 5px;border-radius:6px;border:1px solid rgba(255,255,255,.14);background:#111723;color:#fff}.cam-controls input[type=checkbox]{margin:0}.cam-controls button{padding:4px 8px;font-size:.68rem}.cam-activation-detail{min-width:160px;flex:1;color:#8f99aa;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}.cam-empty{color:#8f99aa;font-size:.75rem;padding:5px 0}.cam-select{display:grid;grid-template-columns:minmax(150px,.8fr) minmax(230px,1.8fr);gap:8px;align-items:end;margin-top:8px}
@media (max-width:760px){.sat-signal-panel{grid-template-columns:1fr}.sat-form,.sat-output{grid-template-columns:repeat(2,minmax(0,1fr))}.cam-row{grid-template-columns:78px minmax(120px,1fr)}.cam-row .cam-state{grid-column:1/-1;justify-self:start}.cam-controls{gap:7px}.cam-activation-detail{flex-basis:100%}.cam-select{grid-template-columns:1fr}.sat-service-head,.sat-service-row{grid-template-columns:30px minmax(150px,1fr) 82px 62px}.sat-service-head>*:nth-child(3),.sat-service-head>*:nth-child(6),.sat-service-row>*:nth-child(3),.sat-service-row>*:nth-child(6){display:none}}
@media (max-width:480px){.sat-form,.sat-output{grid-template-columns:1fr}.sat-field.wide{grid-column:span 1}}.ui-toast-stack{position:fixed;right:18px;bottom:18px;z-index:30000;display:grid;gap:8px;width:min(520px,calc(100vw - 36px));pointer-events:none}
.ui-toast{position:relative;padding:11px 38px 11px 14px;border:1px solid rgba(255,95,95,.42);border-radius:9px;background:#2a1820;color:#ffd9dd;box-shadow:0 12px 30px rgba(0,0,0,.32);font-size:.82rem;line-height:1.4;pointer-events:auto;animation:ui-toast-in .16s ease-out}
.ui-toast.ok{border-color:rgba(23,194,97,.42);background:#14251d;color:#c8f7d7}
.ui-toast-close{display:inline-flex;align-items:center;justify-content:center;position:absolute;top:6px;right:6px;width:18px;height:18px;padding:0;border:0;border-radius:50%;background:#d9363e;color:#fff;font-family:Arial,Helvetica,sans-serif;font-size:14px;font-weight:700;line-height:1;cursor:pointer}
.ui-toast-close:hover{background:#f0444d;transform:scale(1.08)}
@keyframes ui-toast-in{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}

</style>
</head>
<body>
<header>
<div class="header-left">
<div>
<div class="title">Control Panel</div>
<div class="subtitle" data-i18n="subtitle">Broadcast monitoring and stream control</div>
</div>
</div>
<div class="header-center">
<div class="system-load">
<span class="metric"><strong>CPU</strong> <span id="cpuLoad">—%</span></span>
<span class="metric"><strong>RAM</strong> <span id="ramLoad">—%</span></span>
</div>
<div class="stats-panel">
<div class="status"><strong data-i18n="total">Total:</strong> <span id="totalCount">0</span></div>
<div class="status"><strong data-i18n="active">Active:</strong> <span id="activeCount">0</span></div>
</div>
<button class="network-button" onclick="openNetworkModal()" data-i18n="network">Network</button>
</div>
<div class="header-right">
<button class="button-secondary" onclick="toggleLanguage()" id="languageButton">RU</button>
<details class="system-menu" id="systemMenu">
<summary class="button-secondary" data-i18n="system">System</summary>
<div class="system-menu-list">
<button class="system-menu-item" onclick="openLoginModal();closeSystemMenu()" data-i18n="user">User</button>
<button class="system-menu-item" onclick="openTelegramModal();closeSystemMenu()" data-i18n="telegram">Telegram API</button>
<button class="system-menu-item" onclick="openNewcamdModal();closeSystemMenu()">Newcamd</button>
<button class="system-menu-item" onclick="window.location.href='/oscam-mini';closeSystemMenu()">OSCam-mini</button>
<button class="system-menu-item" onclick="openAboutModal();closeSystemMenu()" data-i18n="about">About</button>
<button class="system-menu-item restart-button" onclick="closeSystemMenu();restartProgram()" data-i18n="restartProgram">Restart</button>
</div>
</details>
<button class="button-secondary" onclick="downloadVlcPlaylist()" data-i18n="playlist">VLC playlist</button>
<button class="button-secondary" onclick="openSubscribersModal()" data-i18n="subscribers">Subscribers</button>
<button class="button-secondary" onclick="openAddChannelModal()" data-i18n="addChannel">+ Add channel</button>
<button class="button-primary" onclick="openStreamModal()" data-i18n="addStream">+ Add stream</button>
</div>
</header>
<div class="container">
<div id="tiles" class="tile-grid"></div>
<div id="modal" class="modal">
<div class="modal-content" id="modalContent"></div>
</div>
<script>
const translations = {
  en: {
    subtitle:'Broadcast monitoring and stream control', total:'Total:', active:'Active:', network:'Network', system:'System', user:'User', addStream:'+ Add stream', addChannel:'+ Add channel',
    interfacesNotFound:'No interfaces found', output:'Output', activeInput:'Active input', primary:'Primary', backup:'Backup', sid:'SID', bitrateIn:'Bitrate In', bitrateOut:'Bitrate Out', status:'Status',
    online:'Online', backupOnline:'Backup', offline:'Offline', start:'Start', stop:'Stop', edit:'Edit', chart:'Chart', delete:'Delete stream', removeConfirm:'Delete stream',
    restartProgram:'Restart', restartConfirm:'Restart TVStreammerSAT5 now?', restarting:'Restarting...',
    networkLoad:'Network interface load', interface:'Interface', incoming:'Incoming', outgoing:'Outgoing', close:'Close',
    about:'About', product:'Product', version:'Version', name:'Name', country:'Country', donate:'Donate', donateQr:'Donate QR code', donateWallet:'Telegram Wallet', cancel:'Cancel', save:'Save', userTitle:'User', telegram:'Telegram API', quality:'Stream quality', playlist:'VLC playlist', subscribers:'Subscribers', streams:'Streams', filtering:'Enable IP filtering', addSubscriber:'Add subscriber', primaryIp:'Primary IP', backupIp:'Backup IP', addedAt:'Added at', subscriberName:'Subscriber name', noSubscribers:'No subscribers added', noStreams:'No streams configured', enabled:'Enabled', disabled:'Disabled', exportSubscribers:'Export TXT', session:'Session', activeSession:'Online', offlineSession:'Offline', resetSession:'Reset'
  },
  ru: {
    subtitle:'Мониторинг трансляций и управление потоками', total:'Всего:', active:'Активно:', network:'Сеть', system:'Система', user:'Пользователь', addStream:'+ Добавить поток', addChannel:'+ Добавить канал',
    interfacesNotFound:'Интерфейсы не найдены', output:'Вывод', activeInput:'Активный вход', primary:'Основной', backup:'Резерв', sid:'SID', bitrateIn:'Bitrate In', bitrateOut:'Bitrate Out', status:'Статус',
    online:'Онлайн', backupOnline:'Резерв', offline:'Офлайн', start:'Старт', stop:'Стоп', edit:'Ред.', chart:'График', delete:'Удалить поток', removeConfirm:'Удалить поток',
    restartProgram:'Перезапуск', restartConfirm:'Перезапустить TVStreammerSAT5 сейчас?', restarting:'Перезапуск...',
    networkLoad:'Загрузка сетевых интерфейсов', interface:'Интерфейс', incoming:'Входящий', outgoing:'Исходящий', close:'Закрыть',
    about:'О программе', product:'Программа', version:'Версия', name:'Имя', country:'Страна', donate:'Донат', donateQr:'QR-код доната', donateWallet:'Telegram-кошелёк', cancel:'Отмена', save:'Сохранить', userTitle:'Пользователь', telegram:'Telegram API', quality:'Качество потока', playlist:'Плейлист VLC', subscribers:'Абоненты', streams:'Потоки', filtering:'Включить фильтрацию по IP', addSubscriber:'Добавить абонента', primaryIp:'Основной IP', backupIp:'Резервный IP', addedAt:'Дата добавления', subscriberName:'Наименование абонента', noSubscribers:'Абоненты не добавлены', noStreams:'Потоки не настроены', enabled:'Включен', disabled:'Отключен', exportSubscribers:'Экспорт TXT', session:'Сессия', activeSession:'Онлайн', offlineSession:'Офлайн', resetSession:'Сбросить'
  }
};
function normalizeLanguage(value) {
  return value === 'ru' ? 'ru' : 'en';
}
let language = normalizeLanguage(localStorage.getItem('tvstreammersat5-language') || 'en');
const donateAddress = 'UQD1uQn5WxhzKLXjL0KOVuJDcRU65pYzgt6pm_gzJM-vT-cN';
const donateQrPath = 'M4 4h7v1H4zM12 4h1v1H12zM14 4h3v1H14zM25 4h3v1H25zM30 4h7v1H30zM4 5h1v1H4zM10 5h1v1H10zM13 5h1v1H13zM15 5h1v1H15zM17 5h2v1H17zM20 5h3v1H20zM26 5h1v1H26zM28 5h1v1H28zM30 5h1v1H30zM36 5h1v1H36zM4 6h1v1H4zM6 6h3v1H6zM10 6h1v1H10zM12 6h3v1H12zM16 6h1v1H16zM18 6h2v1H18zM22 6h1v1H22zM25 6h2v1H25zM28 6h1v1H28zM30 6h1v1H30zM32 6h3v1H32zM36 6h1v1H36zM4 7h1v1H4zM6 7h3v1H6zM10 7h1v1H10zM14 7h1v1H14zM16 7h1v1H16zM18 7h1v1H18zM20 7h1v1H20zM22 7h1v1H22zM24 7h2v1H24zM27 7h2v1H27zM30 7h1v1H30zM32 7h3v1H32zM36 7h1v1H36zM4 8h1v1H4zM6 8h3v1H6zM10 8h1v1H10zM14 8h11v1H14zM26 8h2v1H26zM30 8h1v1H30zM32 8h3v1H32zM36 8h1v1H36zM4 9h1v1H4zM10 9h1v1H10zM12 9h1v1H12zM15 9h1v1H15zM20 9h1v1H20zM22 9h1v1H22zM24 9h1v1H24zM26 9h1v1H26zM30 9h1v1H30zM36 9h1v1H36zM4 10h7v1H4zM12 10h1v1H12zM14 10h1v1H14zM16 10h1v1H16zM18 10h1v1H18zM20 10h1v1H20zM22 10h1v1H22zM24 10h1v1H24zM26 10h1v1H26zM28 10h1v1H28zM30 10h7v1H30zM13 11h1v1H13zM16 11h1v1H16zM22 11h1v1H22zM27 11h2v1H27zM4 12h1v1H4zM6 12h1v1H6zM10 12h2v1H10zM14 12h2v1H14zM17 12h1v1H17zM19 12h1v1H19zM21 12h1v1H21zM23 12h1v1H23zM25 12h1v1H25zM27 12h2v1H27zM31 12h1v1H31zM34 12h1v1H34zM36 12h1v1H36zM4 13h2v1H4zM9 13h1v1H9zM11 13h3v1H11zM16 13h1v1H16zM18 13h2v1H18zM24 13h3v1H24zM28 13h3v1H28zM35 13h2v1H35zM5 14h2v1H5zM10 14h1v1H10zM15 14h1v1H15zM17 14h7v1H17zM25 14h1v1H25zM27 14h2v1H27zM30 14h2v1H30zM34 14h1v1H34zM36 14h1v1H36zM6 15h3v1H6zM11 15h1v1H11zM16 15h1v1H16zM18 15h3v1H18zM22 15h3v1H22zM26 15h2v1H26zM29 15h5v1H29zM35 15h2v1H35zM6 16h1v1H6zM8 16h1v1H8zM10 16h1v1H10zM17 16h6v1H17zM24 16h1v1H24zM30 16h2v1H30zM33 16h2v1H33zM36 16h1v1H36zM4 17h3v1H4zM8 17h2v1H8zM13 17h1v1H13zM15 17h1v1H15zM18 17h1v1H18zM20 17h3v1H20zM24 17h2v1H24zM27 17h5v1H27zM33 17h1v1H33zM35 17h1v1H35zM4 18h2v1H4zM8 18h3v1H8zM12 18h2v1H12zM15 18h2v1H15zM18 18h1v1H18zM20 18h1v1H20zM22 18h1v1H22zM24 18h4v1H24zM30 18h1v1H30zM33 18h2v1H33zM36 18h1v1H36zM5 19h1v1H5zM8 19h2v1H8zM11 19h1v1H11zM13 19h1v1H13zM18 19h3v1H18zM23 19h2v1H23zM28 19h2v1H28zM31 19h2v1H31zM35 19h1v1H35zM6 20h1v1H6zM9 20h2v1H9zM13 20h1v1H13zM15 20h3v1H15zM21 20h1v1H21zM24 20h1v1H24zM30 20h2v1H30zM36 20h1v1H36zM4 21h1v1H4zM7 21h3v1H7zM14 21h1v1H14zM16 21h2v1H16zM19 21h1v1H19zM21 21h1v1H21zM24 21h1v1H24zM27 21h3v1H27zM31 21h1v1H31zM33 21h1v1H33zM36 21h1v1H36zM8 22h1v1H8zM10 22h1v1H10zM13 22h1v1H13zM15 22h1v1H15zM17 22h1v1H17zM19 22h1v1H19zM23 22h1v1H23zM26 22h4v1H26zM32 22h3v1H32zM36 22h1v1H36zM6 23h1v1H6zM8 23h2v1H8zM14 23h2v1H14zM18 23h1v1H18zM20 23h2v1H20zM23 23h1v1H23zM26 23h1v1H26zM30 23h1v1H30zM36 23h1v1H36zM5 24h2v1H5zM8 24h1v1H8zM10 24h2v1H10zM13 24h2v1H13zM16 24h2v1H16zM19 24h1v1H19zM21 24h1v1H21zM25 24h1v1H25zM28 24h1v1H28zM30 24h1v1H30zM32 24h1v1H32zM35 24h1v1H35zM5 25h2v1H5zM8 25h1v1H8zM17 25h2v1H17zM25 25h1v1H25zM27 25h1v1H27zM31 25h1v1H31zM33 25h1v1H33zM35 25h1v1H35zM4 26h2v1H4zM9 26h2v1H9zM12 26h1v1H12zM14 26h1v1H14zM16 26h1v1H16zM18 26h2v1H18zM21 26h1v1H21zM23 26h1v1H23zM26 26h2v1H26zM30 26h1v1H30zM32 26h2v1H32zM36 26h1v1H36zM7 27h1v1H7zM11 27h1v1H11zM13 27h1v1H13zM15 27h2v1H15zM18 27h1v1H18zM28 27h6v1H28zM4 28h4v1H4zM10 28h2v1H10zM13 28h1v1H13zM17 28h3v1H17zM23 28h3v1H23zM28 28h5v1H28zM35 28h2v1H35zM12 29h3v1H12zM18 29h1v1H18zM20 29h1v1H20zM24 29h2v1H24zM28 29h1v1H28zM32 29h1v1H32zM35 29h2v1H35zM4 30h7v1H4zM12 30h1v1H12zM14 30h1v1H14zM16 30h2v1H16zM19 30h6v1H19zM28 30h1v1H28zM30 30h1v1H30zM32 30h1v1H32zM34 30h1v1H34zM36 30h1v1H36zM4 31h1v1H4zM10 31h1v1H10zM16 31h4v1H16zM22 31h1v1H22zM27 31h2v1H27zM32 31h2v1H32zM35 31h1v1H35zM4 32h1v1H4zM6 32h3v1H6zM10 32h1v1H10zM13 32h2v1H13zM16 32h1v1H16zM19 32h14v1H19zM35 32h1v1H35zM4 33h1v1H4zM6 33h3v1H6zM10 33h1v1H10zM15 33h1v1H15zM18 33h1v1H18zM20 33h1v1H20zM22 33h1v1H22zM24 33h2v1H24zM29 33h1v1H29zM31 33h1v1H31zM35 33h2v1H35zM4 34h1v1H4zM6 34h3v1H6zM10 34h1v1H10zM12 34h1v1H12zM15 34h3v1H15zM20 34h1v1H20zM22 34h1v1H22zM24 34h2v1H24zM27 34h1v1H27zM32 34h5v1H32zM4 35h1v1H4zM10 35h1v1H10zM13 35h1v1H13zM16 35h1v1H16zM19 35h2v1H19zM22 35h3v1H22zM30 35h2v1H30zM33 35h1v1H33zM4 36h7v1H4zM12 36h2v1H12zM15 36h1v1H15zM17 36h2v1H17zM21 36h1v1H21zM26 36h1v1H26zM28 36h2v1H28zM32 36h2v1H32zM36 36h1v1H36z';
function t(key, values={}) {
  let value = translations[language]?.[key] || translations.en[key] || key;
  Object.entries(values).forEach(([name, replacement]) => { value = value.replace(`{${name}}`, replacement); });
  return value;
}
function applyLanguage() {
  document.querySelectorAll('[data-i18n]').forEach(element => { element.textContent = t(element.dataset.i18n); });
  const button = document.getElementById('languageButton');
  if (button) button.textContent = language === 'en' ? 'RU' : 'EN';
}
function toggleLanguage() {
  language = language === 'en' ? 'ru' : 'en';
  localStorage.setItem('tvstreammersat5-language', language);
  applyLanguage();
  render(true);
  saveLanguagePreference();
}
function closeSystemMenu() {
  document.getElementById('systemMenu')?.removeAttribute('open');
}
document.addEventListener('click', event => {
  const menu = document.getElementById('systemMenu');
  if (menu && !menu.contains(event.target)) closeSystemMenu();
});
document.addEventListener('click', event => {
  const button = event.target.closest?.('[data-stream-action]');
  if (!button) return;
  const id = button.dataset.streamId || button.closest('.tile[data-stream-id]')?.dataset.streamId || '';
  if (!id) return;
  event.preventDefault();
  event.stopPropagation();
  try {
    const action = button.dataset.streamAction;
    if (action === 'toggle') {
      const stream = streamById(id);
      toggleStream(id, stream ? !!stream.active : button.dataset.streamActive === '1', button);
    } else if (action === 'edit') {
      editStream(id);
    } else if (action === 'delete') {
      deleteStream(id);
    } else if (action === 'quality') {
      openQualityModal(id);
    } else if (action === 'copy') {
      copyStreamLinks(id, button);
    }
  } catch (error) {
    uiError(error?.message || error);
  }
});
let state = {};
let statePollTimer = null;
let metricsPollTimer = null;
let stateFetchPromise = null;
let metricsFetchPromise = null;
let lastTileStructureSignature = '';
let lastKnownStreams = [];
let allowEmptyStreamStateOnce = false;
let subscribersModalOpen = false;
let subscriberFormBaseline = '';
let satelliteSignalTimer = null;
let satelliteSignalPending = false;
let satelliteSignalController = null;
let satelliteScanning = false;
let satelliteTuneGeneration = 0;
let satelliteServices = [];
let satelliteLastScan = null;
let dvbAdapters = [];
let camClientsLoaded = false;
let caManagerState = {clients:[]};
let streamActionBusy = new Set();
function uiError(message) {
  console.error(message);
  const text = String(message || 'UI error');
  let stack = document.getElementById('uiToastStack');
  if (!stack) {
    stack = document.createElement('div');
    stack.id = 'uiToastStack';
    stack.className = 'ui-toast-stack';
    document.body.appendChild(stack);
  }
  const toast = document.createElement('div');
  toast.className = 'ui-toast';
  const messageEl = document.createElement('span');
  messageEl.textContent = text;
  const close = document.createElement('button');
  close.type = 'button';
  close.className = 'ui-toast-close';
  close.setAttribute('aria-label', t('close'));
  close.title = t('close');
  close.textContent = '×';
  close.addEventListener('click', () => {
    toast.remove();
    if (!stack.children.length) stack.remove();
  });
  toast.appendChild(messageEl);
  toast.appendChild(close);
  stack.appendChild(toast);
  window.setTimeout(() => {
    toast.remove();
    if (!stack.children.length) stack.remove();
  }, 9000);
}
async function fetchJson(url, options={}, timeoutMs=30000) {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const response = await fetch(url, {...options, signal: controller.signal});
    let data = {};
    try { data = await response.json(); } catch (_) {}
    if (!response.ok) throw new Error(data.error || `HTTP ${response.status}`);
    return data;
  } catch (error) {
    if (error?.name === 'AbortError') throw new Error(`Request timeout: ${url}`);
    throw error;
  } finally {
    clearTimeout(timer);
  }
}
function streamById(id) {
  return (state.streams || []).find(stream => String(stream.id) === String(id));
}
function setStreamStatusMessage(id, message, level='info') {
  const esc = window.CSS?.escape ? CSS.escape(String(id)) : String(id).replace(/["\\]/g, '\\$&');
  const tile = document.querySelector(`.tile[data-stream-id="${esc}"]`);
  const target = tile?.querySelector('[data-role="runtime-status"]');
  if (!target) return;
  target.textContent = message || '';
  target.title = message || '';
  target.dataset.level = level;
}
function markStreamStoppedInUi(id, message='stopped') {
  const stream = streamById(id);
  if (!stream) return;
  stream.active = false;
  stream.status = message;
  stream.using_backup = false;
  stream.bitrate_in_kbps = 0;
  stream.bitrate_out_kbps = 0;
  if (stream.conditional_access_client) stream.ca_decode_state = 'offline';
  render(false);
}
function restoreStreamButton(button, text) {
  if (!button) return;
  button.disabled = false;
  if (text) button.textContent = text;
}
function saveLanguagePreference() {
  fetch('/api/save-config', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({language})
  }).catch(()=>{});
}
function fetchState() {
  if (stateFetchPromise) return stateFetchPromise;
  stateFetchPromise = fetch('/api/state', {cache:'no-store'})
    .then(response => {
      if (!response.ok) throw new Error(`state HTTP ${response.status}`);
      return response.json();
    })
    .then(data => {
      if (!Array.isArray(data.streams)) {
        throw new Error('state response does not contain a streams array');
      }
      if (data.streams.length > 0) {
        lastKnownStreams = data.streams;
      } else if (lastKnownStreams.length > 0 && !allowEmptyStreamStateOnce) {
        console.warn('TVStreammerSAT5 ignored an unexpected empty stream list');
        data.streams = lastKnownStreams;
        data.stream_count = lastKnownStreams.length;
        data.active_count = 0;
      }
      allowEmptyStreamStateOnce = false;
      const storedLanguage = localStorage.getItem('tvstreammersat5-language');
      const serverLanguage = normalizeLanguage(data.language);
      language = normalizeLanguage(storedLanguage || language);
      localStorage.setItem('tvstreammersat5-language', language);
      data.language = language;
      const cachedInterfaces = Array.isArray(state.interfaces) ? state.interfaces : [];
      if (!Array.isArray(data.interfaces) || !data.interfaces.length) data.interfaces = cachedInterfaces;
      state = data;
      applyLanguage();
      try {
        render(false);
        refreshSubscriberSessions();
      } catch (error) {
        console.error('TVStreammerSAT5 render failed:', error);
      }
      if (serverLanguage !== language) saveLanguagePreference(data);
      return data;
    })
    .catch(error => {
      console.warn('TVStreammerSAT5 state refresh failed:', error);
      return null;
    })
    .finally(() => { stateFetchPromise = null; });
  return stateFetchPromise;
}
async function statePollLoop() {
  await fetchState();
  clearTimeout(statePollTimer);
  statePollTimer = setTimeout(statePollLoop, 2000);
}
function updateSystemLoad(metrics) {
  document.getElementById('cpuLoad').textContent = `${Number(metrics.cpu_percent || 0).toFixed(1)}%`;
  document.getElementById('ramLoad').textContent = `${Number(metrics.ram_percent || 0).toFixed(1)}%`;
  const table = document.getElementById('networkTableBody');
  if (!table) return;
  const interfaces = metrics.interfaces || [];
  table.innerHTML = interfaces.length ? interfaces.map(iface => `
    <tr><td>${iface.name}${iface.address ? ` (${iface.address})` : ''}</td><td>${Number(iface.rx_mbps || 0).toFixed(2)} Mbps</td><td>${Number(iface.tx_mbps || 0).toFixed(2)} Mbps</td></tr>
  `).join('') : `<tr><td colspan="3" class="network-empty">${t('interfacesNotFound')}</td></tr>`;
}
function fetchSystemMetrics() {
  if (metricsFetchPromise) return metricsFetchPromise;
  metricsFetchPromise = fetch('/api/system-metrics', {cache:'no-store'})
    .then(response => {
      if (!response.ok) throw new Error(`metrics HTTP ${response.status}`);
      return response.json();
    })
    .then(metrics => {
      state.system_metrics = metrics;
      updateSystemLoad(metrics);
      return metrics;
    })
    .catch(error => {
      console.warn('TVStreammerSAT5 metrics refresh failed:', error);
      return null;
    })
    .finally(() => { metricsFetchPromise = null; });
  return metricsFetchPromise;
}
async function metricsPollLoop() {
  await fetchSystemMetrics();
  clearTimeout(metricsPollTimer);
  metricsPollTimer = setTimeout(metricsPollLoop, 3000);
}
function downloadVlcPlaylist() {
  const entries = (state.streams || [])
    .flatMap(stream => {
      const links = streamLinks(stream);
      return links.map(link => {
        const suffix = links.length > 1 && link.output_type ? ` ${String(link.output_type).toUpperCase()}` : '';
        const name = String((stream.name || stream.id) + suffix).replace(/[\r\n]/g, ' ').trim();
        return `#EXTINF:-1,${name}\n${link.url}`;
      });
    });
  const content = `#EXTM3U\n${entries.join('\n')}\n`;
  const blob = new Blob([content], {type:'audio/x-mpegurl;charset=utf-8'});
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = 'tvstreammersat5-playlist.m3u';
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}
function updateHeaderHeight() {
  const header = document.querySelector('header');
  const height = header ? Math.ceil(header.getBoundingClientRect().height) : 58;
  document.documentElement.style.setProperty('--header-height', `${height}px`);
}
window.addEventListener('resize', updateHeaderHeight);
updateHeaderHeight();
function modalCloseButton() {
  return `<button class="modal-close" onclick="closeModal()" aria-label="${t('close')}">×</button>`;
}
function openModal(html) {
  subscribersModalOpen = false;
  document.getElementById('modalContent').innerHTML = modalCloseButton() + html;
  document.getElementById('modalContent').className = 'modal-content';
  document.getElementById('modal').classList.remove('quality-open', 'stream-open', 'satellite-open');
  document.getElementById('modal').classList.add('active');
}
function closeModal() {
  subscribersModalOpen = false;
  stopQualityAutoRefresh();
  stopSatelliteSignalPolling();
  document.getElementById('modal').classList.remove('active', 'quality-open', 'stream-open', 'satellite-open');
}
function escapeHtmlValue(value) {
  return String(value ?? '').replace(/[&<>"']/g, ch => ({
    '&':'&amp;',
    '<':'&lt;',
    '>':'&gt;',
    '"':'&quot;',
    "'":'&#39;'
  })[ch]);
}
function normalizedOutputType(stream) {
  const raw = String(stream.output_type || 'udp').toLowerCase();
  if (raw === 'udp') return stream.cbr ? 'udp-cbr' : 'udp-vbr';
  if (raw === 'udp_cbr' || raw === 'udpcbr') return 'udp-cbr';
  if (raw === 'udp_vbr' || raw === 'udpvbr') return 'udp-vbr';
  return raw;
}
function outputConfigsForStream(stream) {
  const makeOutput = output => ({
    output_type: output.output_type || 'udp-cbr',
    output_mode: output.output_mode || 'listener',
    output_host: output.output_host || '127.0.0.1',
    output_port: Number(output.output_port || 1234),
    cbr: stream.cbr
  });
  if (Array.isArray(stream.outputs) && stream.outputs.length) {
    return stream.outputs.map(makeOutput);
  }
  return [
    makeOutput(stream),
    ...(Array.isArray(stream.additional_outputs) ? stream.additional_outputs.map(makeOutput) : [])
  ];
}
function streamLinks(stream) {
  if (Array.isArray(stream.vlc_links) && stream.vlc_links.length) {
    return stream.vlc_links.filter(link => link.url);
  }
  return stream.vlc_link ? [{output_type: normalizedOutputType(stream), url: stream.vlc_link}] : [];
}
function outputBadgeText(stream) {
  const outputs = outputConfigsForStream(stream);
  return outputs.length > 1 ? `${outputs.length} OUT` : normalizedOutputType(outputs[0] || stream).toUpperCase();
}
function streamBitrateMode(stream) {
  const type = normalizedOutputType(stream);
  if (type === 'udp-cbr') return 'CBR';
  if (type === 'udp-vbr') return 'VBR';
  return stream.cbr ? 'CBR' : 'VBR';
}
function streamTileStructureSignature(stream) {
  return {
    id: stream.id,
    name: stream.name,
    input_uri: stream.input_uri,
    backup_input_uri: stream.backup_input_uri,
    backup_input_type: stream.backup_input_type,
    backup_file_loop: stream.backup_file_loop,
    input_service_id: stream.input_service_id,
    service_id: stream.service_id,
    conditional_access_client: stream.conditional_access_client,
    cbr: stream.cbr,
    outputs: outputConfigsForStream(stream),
    links: streamLinks(stream)
  };
}
function tilesStructureSignature() {
  return JSON.stringify({
    language,
    streams: (state.streams || []).map(streamTileStructureSignature)
  });
}
function dvbMeterColor(value, locked) {
  if (!locked) return '#d9363e';
  if (value >= 65) return '#17c261';
  if (value >= 35) return '#ffb84d';
  return '#fb5f5f';
}
function updateDvbMeter(tile, kind, value, available, locked) {
  const fill = tile.querySelector(`[data-role="dvb-${kind}-fill"]`);
  const label = tile.querySelector(`[data-role="dvb-${kind}-label"]`);
  if (!fill || !label) return;
  const numeric = Math.max(0, Math.min(100, Number(value || 0)));
  const shown = available ? Math.round(numeric) : 0;
  fill.style.width = `${shown}%`;
  fill.style.background = available ? dvbMeterColor(numeric, locked) : 'rgba(255,255,255,.12)';
  label.textContent = `${kind === 'signal' ? 'S' : 'Q'} ${available ? `${shown}%` : '—'}`;
}
function caDecodeInfo(stream) {
  if (!stream?.conditional_access_client) return {cls:'offline', text:'FTA', detail:'Не требуется'};
  const mode = String(stream.ca_decode_state || (stream.active ? 'waiting' : 'offline'));
  const scrambled = Number(stream.ca_output_scrambled_packets || 0);
  const payload = Number(stream.ca_output_payload_packets || 0);
  const clearPes = Number(stream.ca_output_clear_pes_starts || 0);
  const percent = Number(stream.ca_output_scrambled_percent || 0);
  if (mode === 'clear') return {cls:'clear', text:'ДЕКОД: ОК', detail:`ОТКРЫТ A/V · PES ${clearPes} · scrambled 0/${payload}`};
  if (mode === 'scrambled') return {cls:'scrambled', text:'ДЕКОД: НЕТ', detail:`ЗАКОДИРОВАН A/V · ${percent.toFixed(1)}% (${scrambled}/${payload})`};
  if (mode === 'invalid') return {cls:'scrambled', text:'ДЕКОД: НЕТ', detail:`A/V пакеты идут, но валидный открытый PES не найден (${payload} пак.)`};
  if (mode === 'offline') return {cls:'offline', text:'ДЕКОД: OFF', detail:'Канал остановлен'};
  return {cls:'waiting', text:'ДЕКОД: …', detail:'ОЖИДАНИЕ A/V ПАКЕТОВ'};
}

function updateStreamTile(tile, stream) {
  if (!tile || !stream) return;
  tile.classList.toggle('active', !!stream.active);

  const statusPill = tile.querySelector('[data-role="status-pill"]');
  if (statusPill) {
    statusPill.className = `status-pill ${stream.active ? 'active' : 'stopped'}`;
    statusPill.textContent = stream.active ? (stream.using_backup ? 'Backup' : 'Online') : 'Offline';
  }
  const runtimeStatus = tile.querySelector('[data-role="runtime-status"]');
  if (runtimeStatus) {
    const value = String(stream.status || '').trim();
    runtimeStatus.textContent = value;
    runtimeStatus.title = value;
  }

  if (stream.dvb_input) {
    const available = !!stream.dvb_signal_available;
    const locked = !!stream.dvb_locked;
    updateDvbMeter(tile, 'signal', stream.dvb_signal, available, locked);
    updateDvbMeter(tile, 'quality', stream.dvb_quality, available, locked);
  }

  const activeInput = tile.querySelector('[data-role="active-input"]');
  if (activeInput) {
    activeInput.textContent = `${stream.active_input_label || t('primary')} · ${stream.active_input_uri || stream.input_uri || '—'}`;
  }

  const bitrateIn = tile.querySelector('[data-role="bitrate-in"]');
  if (bitrateIn) bitrateIn.textContent = stream.bitrate_in_kbps ? `${stream.bitrate_in_kbps} kbps` : '—';

  const bitrateOut = tile.querySelector('[data-role="bitrate-out"]');
  if (bitrateOut) bitrateOut.textContent = stream.bitrate_out_kbps ? `${stream.bitrate_out_kbps} kbps` : '—';

  const caStatus = tile.querySelector('[data-role="ca-status"]');
  if (caStatus) caStatus.textContent = caStreamStatusText(stream);

  const decodeInfo = caDecodeInfo(stream);
  const decodePill = tile.querySelector('[data-role="decode-status"]');
  if (decodePill) {
    decodePill.className = `decode-pill ${decodeInfo.cls}`;
    decodePill.textContent = decodeInfo.text;
    decodePill.title = `${decodeInfo.detail}. Контроль по A/V PID, scrambling_control и валидному PES на выходе.`;
  }

  const toggleButton = tile.querySelector('[data-role="stream-toggle"]');
  if (toggleButton) {
    toggleButton.className = stream.active ? 'stop-button' : 'start-button';
    toggleButton.textContent = stream.active ? t('stop') : t('start');
    toggleButton.dataset.streamId = String(stream.id);
    toggleButton.dataset.streamActive = stream.active ? '1' : '0';
    toggleButton.onclick = null;
  }
}
function updateLiveTiles() {
  const totalCount = document.getElementById('totalCount');
  const activeCount = document.getElementById('activeCount');
  if (totalCount) totalCount.textContent = state.stream_count ?? (state.streams || []).length;
  if (activeCount) activeCount.textContent = state.active_count ?? (state.streams || []).filter(stream => stream.active).length;

  const tiles = document.getElementById('tiles');
  if (!tiles) return;
  const tileElements = Array.from(tiles.querySelectorAll('.tile[data-stream-id]'));
  const byId = new Map(tileElements.map(tile => [tile.dataset.streamId, tile]));
  (state.streams || []).forEach(stream => updateStreamTile(byId.get(String(stream.id)), stream));
}
function render(force=false) {
  const tiles = document.getElementById('tiles');
  if (!tiles) return;

  const signature = tilesStructureSignature();
  const streams = state.streams || [];
  if (!force && signature === lastTileStructureSignature && tiles.children.length === streams.length) {
    updateLiveTiles();
    return;
  }

  tiles.innerHTML = '';
  streams.forEach(stream => {
    const outputs = outputConfigsForStream(stream);
    const outputType = normalizedOutputType(outputs[0] || stream);
    const bitrateMode = streamBitrateMode(stream);
    const links = streamLinks(stream);
    const primaryLink = links[0]?.url || stream.vlc_link || `${stream.output_host || ''}:${stream.output_port || ''}`;
    const tile = document.createElement('div');
    tile.className = 'tile' + (stream.active ? ' active' : '');
    tile.dataset.streamId = String(stream.id);
    tile.innerHTML = `
      <div class="top">
        <div>
          <div class="title">${escapeHtmlValue(stream.name || stream.id)}</div>
          <div class="status-line">
            <div data-role="status-pill" class="status-pill ${stream.active ? 'active' : 'stopped'}">${stream.active ? (stream.using_backup ? 'Backup' : 'Online') : 'Offline'}</div>
            <span data-role="runtime-status" class="runtime-status" title="${escapeHtmlValue(stream.status || '')}">${escapeHtmlValue(stream.status || '')}</span>
          </div>
        </div>
      </div>
      <div class="tile-actions">
        <div class="badge">${outputs.length > 1 ? outputBadgeText(stream) : bitrateMode}</div>
        <button class="delete-button" title="${t('delete')}" aria-label="${t('delete')}" data-stream-action="delete" data-stream-id="${escapeHtmlValue(stream.id)}">×</button>
      </div>
      <div class="dvb-meters${stream.dvb_input ? '' : ' placeholder'}" title="${stream.dvb_input ? 'DVB Signal / Quality' : ''}">
        <div class="dvb-meter"><span data-role="dvb-signal-fill" class="dvb-meter-fill"></span><b data-role="dvb-signal-label" class="dvb-meter-label">S —</b></div>
        <div class="dvb-meter"><span data-role="dvb-quality-fill" class="dvb-meter-fill"></span><b data-role="dvb-quality-label" class="dvb-meter-label">Q —</b></div>
      </div>
      <div class="info">
        <div class="info-row"><strong>${t('output')}</strong><span>${outputs.length > 1 ? outputBadgeText(stream) : outputType.toUpperCase()} · ${primaryLink}</span></div>
        <div class="info-row"><strong>${t('activeInput')}</strong><span data-role="active-input">${stream.active_input_label || t('primary')} · ${stream.active_input_uri || stream.input_uri || '—'}</span></div>
        <div class="info-row"><strong>${t('primary')}</strong><span>${stream.input_uri || '—'}</span></div>
        <div class="info-row"><strong>${t('backup')}</strong><span>${stream.backup_input_uri || '—'}${stream.backup_input_type === 'file' && stream.backup_file_loop ? ' · loop' : ''}</span></div>
        <div class="info-row"><strong>${t('sid')}</strong><span>${stream.service_id || '—'}</span></div>
        ${stream.conditional_access_client ? `<div class="info-row"><strong>CA</strong><span data-role="ca-status">${caStreamStatusText(stream)}</span></div>
        <div class="info-row decode-row"><strong>Декодирование</strong><span data-role="decode-status" class="decode-pill ${caDecodeInfo(stream).cls}" title="Контроль по A/V PID, scrambling_control и валидному PES">${caDecodeInfo(stream).text}</span></div>` : `<div class="info-row placeholder"><strong>CA</strong><span>—</span></div>
        <div class="info-row placeholder decode-row"><strong>Декодирование</strong><span>—</span></div>`}
        <div class="info-row"><strong>${t('bitrateIn')}</strong><span data-role="bitrate-in">${stream.bitrate_in_kbps ? stream.bitrate_in_kbps + ' kbps' : '—'}</span></div>
        <div class="info-row"><strong>${t('bitrateOut')}</strong><span data-role="bitrate-out">${stream.bitrate_out_kbps ? stream.bitrate_out_kbps + ' kbps' : '—'}</span></div>
      </div>
      <div class="controls">
        <button data-role="stream-toggle" data-stream-action="toggle" data-stream-id="${escapeHtmlValue(stream.id)}" data-stream-active="${stream.active ? '1' : '0'}" class="${stream.active ? 'stop-button' : 'start-button'}">${stream.active ? t('stop') : t('start')}</button>
        <button data-stream-action="edit" data-stream-id="${escapeHtmlValue(stream.id)}">${t('edit')}</button>
        <button class="quality-button" data-stream-action="quality" data-stream-id="${escapeHtmlValue(stream.id)}">${t('chart')}</button>
        <button class="copy-button" data-stream-action="copy" data-stream-id="${escapeHtmlValue(stream.id)}">${links.length > 1 ? 'URLs' : 'URL'}</button>
      </div>`;
    tiles.appendChild(tile);
    updateStreamTile(tile, stream);
  });
  lastTileStructureSignature = signature;
  updateLiveTiles();
}
async function toggleStream(id, active, button=null) {
  const stream = streamById(id);
  const body = active ? {id} : stream;
  if (!body) {
    uiError(`Stream not found in UI state: ${id}`);
    fetchState();
    return;
  }
  if (streamActionBusy.has(id)) return;
  streamActionBusy.add(id);
  const url = active ? '/api/stop-stream' : '/api/start-stream';
  const originalText = button?.textContent;
  if (button) {
    button.disabled = true;
    button.textContent = active ? 'Stopping...' : 'Starting...';
    setStreamStatusMessage(id, active ? 'Stopping stream...' : 'Starting stream...', 'pending');
  }
  const restoreTimer = setTimeout(() => {
    streamActionBusy.delete(id);
    restoreStreamButton(button, originalText);
    setStreamStatusMessage(id, active ? 'Stop request timed out' : 'Start request timed out', 'error');
  }, active ? 17000 : 47000);
  try {
    const data = await fetchJson(url, {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(body)}, active ? 15000 : 45000);
    if (data.result === 'error') throw new Error(data.error || (active ? 'Stop failed' : 'Start failed'));
    if (active) markStreamStoppedInUi(id, data.already_stopped ? 'stopped' : 'stopping');
    setStreamStatusMessage(id, active ? 'Stop command accepted' : 'Start command accepted', 'ok');
  } catch (error) {
    const message = error?.message || String(error);
    if (active && message.includes('stream is not active')) {
      markStreamStoppedInUi(id, 'stopped');
      setStreamStatusMessage(id, 'Already stopped', 'ok');
    } else {
      setStreamStatusMessage(id, message, 'error');
      uiError(message);
    }
  } finally {
    clearTimeout(restoreTimer);
    streamActionBusy.delete(id);
    restoreStreamButton(button, originalText);
    setTimeout(fetchState, 300);
    setTimeout(fetchState, 1500);
  }
}
function deleteStream(id) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream || !window.confirm(`${t('removeConfirm')} «${stream.name || stream.id}»?`)) return;
  allowEmptyStreamStateOnce = state.streams.length === 1;
  fetch('/api/delete-stream', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})})
    .then(()=>{
      lastKnownStreams = state.streams.filter(item => item.id !== id);
      closeModal();
      setTimeout(fetchState, 300);
    })
    .catch(error => {
      allowEmptyStreamStateOnce = false;
      uiError(error?.message || error);
    });
}
function restartProgram() {
  if (!window.confirm(t('restartConfirm'))) return;
  const button = document.querySelector('.restart-button');
  if (button) {
    button.disabled = true;
    button.textContent = t('restarting');
  }
  fetch('/api/restart-program', {method:'POST'})
    .catch(()=>{})
    .finally(()=>{
      setTimeout(()=>window.location.reload(), 3500);
    });
}
function openNetworkModal() {
  document.getElementById('modalContent').className = 'modal-content network-modal';
  document.getElementById('modalContent').innerHTML = modalCloseButton() + `
    <h2>${t('networkLoad')}</h2>
    <table class="network-table"><thead><tr><th>${t('interface')}</th><th>${t('incoming')}</th><th>${t('outgoing')}</th></tr></thead><tbody id="networkTableBody"></tbody></table>
    <div class="modal-actions"><button class="button-secondary" onclick="closeNetworkModal()">${t('close')}</button></div>
  `;
  document.getElementById('modal').classList.add('active');
  fetchSystemMetrics();
}
function closeNetworkModal() {
  closeModal();
}
function openSubscribersModal() {
  const renderSubscribers = () => {
    const subscribers = state.subscribers || [];
    const rows = subscribers.length ? subscribers.map((subscriber, index) => `
      <div class="subscriber-row" data-index="${index}" data-added-at="${subscriber.added_at || ''}">
        <input data-field="name" value="${subscriber.name || ''}" placeholder="${t('subscriberName')}" />
        <input data-field="primary_ip" value="${subscriber.primary_ip || ''}" placeholder="${t('primaryIp')}" />
        <input data-field="backup_ip" value="${subscriber.backup_ip || ''}" placeholder="${t('backupIp')}" />
        <details class="subscriber-stream-picker">
          <summary id="subscriberStreamsSummary-${index}">${t('streams')} (${(subscriber.stream_ids || []).length})</summary>
          <div class="subscriber-stream-options">
            ${(state.streams || []).map(stream => `<label><input type="checkbox" data-stream-id="${stream.id}" onchange="updateSubscriberStreamSummary(${index})" ${(subscriber.stream_ids || []).includes(stream.id) ? 'checked' : ''} />${stream.name || stream.id}</label>`).join('') || `<span>${t('noStreams')}</span>`}
          </div>
        </details>
        <label class="subscriber-enabled"><input data-field="enabled" type="checkbox" onchange="updateSubscriberStatus(this)" ${subscriber.enabled !== false ? 'checked' : ''} /><span>${subscriber.enabled !== false ? t('enabled') : t('disabled')}</span></label>
        <div class="subscriber-session ${subscriber.session_active ? 'active' : ''}"><span>${subscriber.session_active ? `${t('activeSession')} (${subscriber.active_sessions || 0})` : t('offlineSession')}</span><button class="reset-session" onclick="resetSubscriberSession('${String(subscriber.name || '').replace(/'/g, "\\'")}')">${t('resetSession')}</button></div>
        <button class="remove-subscriber" onclick="removeSubscriber(${index})" aria-label="Remove">×</button>
      </div>
    `).join('') : `<div class="network-empty">${t('noSubscribers')}</div>`;
    openModal(`
      <h2>${t('subscribers')}</h2>
      <div class="checkbox-inline"><input id="subscriberFiltering" type="checkbox" ${state.subscriber_filtering_enabled ? 'checked' : ''} /><span>${t('filtering')}</span></div>
      <div class="subscriber-head"><span>${t('subscriberName')}</span><span>${t('primaryIp')}</span><span>${t('backupIp')}</span><span>${t('streams')}</span><span>${t('enabled')}</span><span>${t('session')}</span><span></span></div>
      <div id="subscriberList" class="subscriber-list">${rows}</div>
      <div class="modal-actions">
        <button class="button-secondary" onclick="addSubscriber()">+ ${t('addSubscriber')}</button>
        <button class="button-secondary" onclick="exportSubscribers()">${t('exportSubscribers')}</button>
        <button class="button-secondary" onclick="closeModal()">${t('cancel')}</button>
        <button id="saveSubscribersButton" class="button-primary save-clean" onclick="saveSubscribers()">${t('save')}</button>
      </div>
    `);
    document.getElementById('modalContent').className = 'modal-content subscriber-modal';
    subscribersModalOpen = true;
    if (!subscriberFormBaseline) {
      subscriberFormBaseline = serializeSubscriberPayload(collectSubscriberPayload());
    }
    wireSubscriberDirtyTracking();
    updateSubscribersSaveButton();
    refreshSubscriberSessions();
  };
  subscriberFormBaseline = '';
  renderSubscribers();
}
function serializeSubscriberPayload(payload) {
  return JSON.stringify(payload);
}
function collectSubscriberPayload() {
  const rows = [...document.querySelectorAll('.subscriber-row')];
  const subscribers = rows.map(row => {
    const value = field => row.querySelector(`[data-field="${field}"]`)?.value.trim() || '';
    return {
      name:value('name'), primary_ip:value('primary_ip'), backup_ip:value('backup_ip'), added_at:row.dataset.addedAt || '',
      enabled:row.querySelector('[data-field="enabled"]')?.checked !== false,
      stream_ids:[...row.querySelectorAll('[data-stream-id]:checked')].map(input=>input.dataset.streamId)
    };
  });
  return {filtering_enabled:document.getElementById('subscriberFiltering')?.checked === true, subscribers};
}
function wireSubscriberDirtyTracking() {
  const list = document.getElementById('subscriberList');
  list?.addEventListener('input', updateSubscribersSaveButton);
  list?.addEventListener('change', updateSubscribersSaveButton);
  document.getElementById('subscriberFiltering')?.addEventListener('change', updateSubscribersSaveButton);
}
function updateSubscribersSaveButton() {
  const button = document.getElementById('saveSubscribersButton');
  if (!button) return;
  const dirty = serializeSubscriberPayload(collectSubscriberPayload()) !== subscriberFormBaseline;
  button.classList.toggle('save-dirty', dirty);
  button.classList.toggle('save-clean', !dirty);
}
function refreshSubscriberSessions() {
  if (!subscribersModalOpen) return;
  const list = document.getElementById('subscriberList');
  if (!list) return;
  (state.subscribers || []).forEach((subscriber, index) => {
    const row = list.querySelector(`.subscriber-row[data-index="${index}"]`);
    const session = row?.querySelector('.subscriber-session');
    const text = session?.querySelector('span');
    if (!session || !text) return;
    const active = !!subscriber.session_active;
    session.classList.toggle('active', active);
    text.textContent = active ? `${t('activeSession')} (${subscriber.active_sessions || 0})` : t('offlineSession');
  });
}
function addSubscriber() {
  const payload = collectSubscriberPayload();
  payload.subscribers.push({name:'', primary_ip:'', backup_ip:'', added_at:new Date().toISOString().slice(0, 10), enabled:true, stream_ids:[]});
  state.subscribers = payload.subscribers;
  state.subscriber_filtering_enabled = payload.filtering_enabled;
  const baseline = subscriberFormBaseline;
  openSubscribersModal();
  subscriberFormBaseline = baseline;
  updateSubscribersSaveButton();
}
function removeSubscriber(index) {
  const payload = collectSubscriberPayload();
  payload.subscribers.splice(index, 1);
  state.subscribers = payload.subscribers;
  state.subscriber_filtering_enabled = payload.filtering_enabled;
  const baseline = subscriberFormBaseline;
  openSubscribersModal();
  subscriberFormBaseline = baseline;
  updateSubscribersSaveButton();
}
function updateSubscriberStreamSummary(index) {
  const row = document.querySelector(`.subscriber-row[data-index="${index}"]`);
  const summary = document.getElementById(`subscriberStreamsSummary-${index}`);
  if (!row || !summary) return;
  summary.textContent = `${t('streams')} (${row.querySelectorAll('[data-stream-id]:checked').length})`;
  updateSubscribersSaveButton();
}
function updateSubscriberStatus(input) {
  const label = input.closest('.subscriber-enabled');
  const text = label?.querySelector('span');
  if (text) text.textContent = input.checked ? t('enabled') : t('disabled');
  updateSubscribersSaveButton();
}
function resetSubscriberSession(name) {
  fetch('/api/reset-subscriber', {
    method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({name})
  }).then(()=>{ state.subscribers = (state.subscribers || []).map(subscriber => subscriber.name === name ? {...subscriber, session_active:false, active_sessions:0} : subscriber); refreshSubscriberSessions(); fetchState(); });
}
function saveSubscribers() {
  const payload = collectSubscriberPayload();
  const serialized = serializeSubscriberPayload(payload);
  if (serialized === subscriberFormBaseline) return;
  fetch('/api/save-subscribers', {
    method:'POST', headers:{'Content-Type':'application/json'},
    body:JSON.stringify(payload)
  }).then(()=>{
    state.subscribers=payload.subscribers;
    state.subscriber_filtering_enabled=payload.filtering_enabled;
    subscriberFormBaseline = serialized;
    updateSubscribersSaveButton();
    refreshSubscriberSessions();
    fetchState();
  });
}
function exportSubscribers() {
  const rows = [...document.querySelectorAll('.subscriber-row')];
  const lines = [t('subscribers')];
  rows.forEach((row, index) => {
    const value = field => row.querySelector(`[data-field="${field}"]`)?.value.trim() || '—';
    const enabled = row.querySelector('[data-field="enabled"]')?.checked !== false;
    const streams = [...row.querySelectorAll('[data-stream-id]:checked')].map(input => {
      const label = input.closest('label');
      return label ? label.textContent.trim() : input.dataset.streamId;
    });
    lines.push('', `${index + 1}. ${value('name')}`,
      `IP: ${value('primary_ip')}`,
      `Backup IP: ${value('backup_ip')}`,
      `${t('addedAt')}: ${row.dataset.addedAt || '—'}`,
      `${t('status')}: ${enabled ? t('enabled') : t('disabled')}`,
      `${t('streams')}: ${streams.length ? streams.join(', ') : '—'}`);
  });
  const blob = new Blob([lines.join('\n') + '\n'], {type:'text/plain;charset=utf-8'});
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = 'tvstreammersat5-subscribers.txt';
  document.body.appendChild(link);
  link.click();
  link.remove();
  URL.revokeObjectURL(url);
}
function editStream(id) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream) return;
  openStreamForm(stream);
}
function openAboutModal() {
  openModal(`
    <h2>${t('about')}</h2>
    <div class="about-list">
      <div class="about-row"><strong>${t('product')}</strong><span>TVStreammerSAT5</span></div>
      <div class="about-row"><strong>${t('version')}</strong><span>${state.program_release||'Release 49'} / ${state.program_version||'v178'}</span></div>
      <div class="about-row"><strong>${t('name')}</strong><span>Лукомский Виталий</span></div>
      <div class="about-row"><strong>${t('country')}</strong><span>Беларусь, г. Борисов</span></div>
      <div class="about-row"><strong>Email</strong><a href="mailto:monkipnet@gmail.com">monkipnet@gmail.com</a></div>
      <div class="about-row about-donate"><strong>${t('donate')}</strong><div class="about-donate-content">
        <svg class="about-qr" viewBox="0 0 41 41" role="img" aria-label="${t('donateQr')}" shape-rendering="crispEdges">
          <rect width="41" height="41" fill="#fff"></rect>
          <path d="${donateQrPath}" fill="#111"></path>
        </svg>
        <div class="about-donate-wallet">
          <span class="about-donate-wallet-label">${t('donateWallet')}</span>
          <span class="about-donate-address">${donateAddress}</span>
        </div>
      </div></div>
    </div>
    <div class="modal-actions">
      <button class="button-primary" onclick="closeModal()">${t('close')}</button>
    </div>
  `);
}
function openLoginModal() {
  openModal(`
    <h2>${t('userTitle')}</h2>
    <div class="form-grid full">
      <div class="form-row"><label>Login</label><input id="login" value="${state.login||''}" /></div>
      <div class="form-row"><label>Новый пароль</label><input id="password" type="password" placeholder="Оставьте пустым, чтобы не менять" /></div>
      <div class="form-row"><label>Имя сервера</label><input id="serverName" value="${state.server_name||''}" /></div>
      <div class="form-row"><label>Порт web-интерфейса</label><input id="httpPort" type="number" min="1" max="65535" value="${state.http_port||9000}" /></div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">${t('cancel')}</button>
      <button class="button-primary" onclick="saveSettings()">${t('save')}</button>
    </div>
  `);
}
function openTelegramModal() {
  openModal(`
    <h2>Telegram API</h2>
    <div class="form-grid full">
      <div class="form-row"><label>Token</label><input id="telegramToken" value="${state.telegram_token||''}" /></div>
      <div class="form-row"><label>Chat ID</label><input id="telegramChatId" value="${state.telegram_chat_id||''}" /></div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">Отмена</button>
      <button class="button-primary" onclick="saveSettings()">Сохранить</button>
    </div>
  `);
}
function satEscape(value) {
  return String(value ?? '').replace(/[&<>"']/g, char => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[char]));
}
function camClientsForUi() {
  const live = Array.isArray(caManagerState?.clients) ? caManagerState.clients : [];
  return live.length ? live : (Array.isArray(state.cam_clients) ? state.cam_clients.map(client => ({...client, ...parseCamBackendConfig(client)})) : []);
}
function parseCamBackendConfig(client={}) {
  const raw = client.backend_config;
  if (raw && typeof raw === 'object') return raw;
  if (typeof raw === 'string' && raw.trim()) {
    try { return JSON.parse(raw); } catch (_) { return {}; }
  }
  return {};
}
function camClientSummary(id) {
  const wanted = String(id || '');
  const client = camClientsForUi().find(item => String(item.id || '') === wanted);
  if (!client) return '';
  const config = parseCamBackendConfig(client);
  const endpoint = client.endpoint || ((client.host || config.host) ? `${client.host || config.host}:${client.port || config.port || 0}` : 'not configured');
  const slots = `${Number(client.services_used||0)}/${Number(client.max_services||10)}`;
  return `${client.name || client.id} / ${endpoint} / ${client.backend_id || 'newcamd'} / ${slots}`;
}
function caStreamStatusText(stream) {
  const ca = stream?.ca || {};
  if (!stream?.conditional_access_client) return 'FTA / no CAM';
  if (!ca.managed) return camClientSummary(stream.conditional_access_client) || 'CAM client configured / slot free';
  const client = ca.client_name || ca.client_display_name || ca.client || stream.conditional_access_client;
  const backendId = String(ca.backend_id || ca.backend?.backend_id || 'newcamd');
  const backendState = String(ca.backend?.status || ca.status || '');
  const stateText = ca.active ? 'active' : 'reserved';
  return `${client} / backend ${backendId}${backendState?` / ${backendState}`:''} / ${stateText}`;
}
async function loadCaManager() {
  try {
    caManagerState = await fetchJson('/api/ca-manager', {cache:'no-store'}, 10000);
  } catch (_) {
    caManagerState = {clients:[]};
  }
  return caManagerState;
}
function caBackendOptions(selected='newcamd') {
  const current = String(selected || 'newcamd');
  const backends = Array.isArray(caManagerState?.ca_backend?.backends) ? caManagerState.ca_backend.backends : [];
  const options = backends.map(item => {
    const id = String(item.id || '');
    if (!id || id === 'passthrough') return '';
    const label = String(item.display_name || id);
    const suffix = item.usable === false ? ' / load error' : ' / plugin';
    return `<option value="${satEscape(id)}" ${id===current?'selected':''}>${satEscape(label + suffix)}</option>`;
  }).filter(Boolean);
  if (!backends.some(item => String(item.id || '') === current)) {
    options.push(`<option value="${satEscape(current)}" selected>${satEscape(current)} / not loaded</option>`);
  }
  return options.join('');
}
function camClientOptions(selected='') {
  const current = String(selected || '');
  const options = [`<option value="" ${(!current || current==='auto')?'selected':''}>Do not use CAM / FTA</option>`];
  camClientsForUi().forEach(client => {
    const id = String(client.id || '');
    if (!id) return;
    options.push(`<option value="${satEscape(id)}" ${id===current?'selected':''}>${satEscape(camClientSummary(id) || id)}</option>`);
  });
  if (current && current !== 'auto' && !camClientsForUi().some(client => String(client.id || '') === current)) {
    options.push(`<option value="${satEscape(current)}" selected>${satEscape(current)} / missing</option>`);
  }
  return options.join('');
}
function renderCamClients() {
  const list = document.getElementById('satCamClients');
  const select = document.getElementById('satCamClientSelect');
  const clients = camClientsForUi();
  if (list) {
    list.innerHTML = clients.length ? clients.map(client => {
      const id = String(client.id || '');
      const status = String(client.status || (client.endpoint ? 'CONFIGURED' : 'NOT_CONFIGURED'));
      return `<div class="cam-row"><span class="cam-name">${satEscape(client.name || id)}</span><span class="cam-device">${satEscape(camClientSummary(id) || id)}</span><span class="cam-state ${status==='CONFIGURED'?'card':'warn'}">${satEscape(status)}</span></div>`;
    }).join('') : '<div class="cam-empty">No CAM clients configured.</div>';
  }
  if (select) {
    const previous = select.value || '';
    select.innerHTML = camClientOptions(previous);
    select.value = previous === 'auto' ? '' : previous;
  }
}
async function loadCamClients() {
  await loadCaManager();
  camClientsLoaded = true;
  renderCamClients();
  return caManagerState;
}
async function refreshCamClients() {
  const button = document.getElementById('satCamRefresh');
  if (button) button.disabled = true;
  await loadCamClients();
  if (button) button.disabled = false;
}
function nextNewcamdIndex() {
  const used = new Set([...document.querySelectorAll('.newcamd-row [data-cam-field="id"]')].map(input => String(input.value || '')));
  let index = document.querySelectorAll('.newcamd-row').length + 1;
  while (used.has(`newcamd-${index}`)) index += 1;
  return index;
}
function defaultNewcamdClient(index=1) {
  return {id:`newcamd-${index}`, name:`OSCam ${index}`, backend_id:'newcamd', max_services:10, backend_config:{host:'127.0.0.1', port:15000, user:'user', pass:'pass', des:'0102030405060708091011121314'}};
}
function newcamdSettingsClients() {
  const configured = Array.isArray(state.cam_clients) ? state.cam_clients : [];
  return configured.length ? configured : [defaultNewcamdClient(1)];
}
function newcamdRowHtml(client={}, index=0) {
  const cfg = parseCamBackendConfig(client);
  return `<div class="newcamd-row" data-index="${index}">
    <div class="form-row"><label>ID</label><input data-cam-field="id" value="${satEscape(client.id || '')}" /></div>
    <div class="form-row"><label>Name</label><input data-cam-field="name" value="${satEscape(client.name || client.id || '')}" /></div>
    <div class="form-row"><label>OSCam host</label><input data-cam-field="host" value="${satEscape(cfg.host || client.host || '127.0.0.1')}" /></div>
    <div class="form-row"><label>Port</label><input data-cam-field="port" type="number" min="1" max="65535" value="${Number(cfg.port || client.port || 15000)}" /></div>
    <div class="form-row"><label>User</label><input data-cam-field="user" value="${satEscape(cfg.user || client.user || 'user')}" /></div>
    <div class="form-row"><label>Password</label><input data-cam-field="pass" type="password" value="${satEscape(cfg.pass || client.pass || '')}" /></div>
    <div class="form-row full"><label>DES key</label><input data-cam-field="des" value="${satEscape(cfg.des || client.des || '0102030405060708091011121314')}" /></div>
    <div class="form-row"><label>Max channels</label><input data-cam-field="max_services" type="number" min="1" max="64" value="${Number(client.max_services || 10)}" /></div>
    <div class="form-row"><label>Backend</label><select data-cam-field="backend_id">${caBackendOptions(client.backend_id || 'newcamd')}</select></div>
    <div class="form-row"><label>&nbsp;</label><button class="button-secondary" type="button" onclick="removeNewcamdRow(this)">Remove</button></div>
  </div>`;
}
function addNewcamdRow(client=null) {
  const rows = document.getElementById('newcamdRows');
  if (!rows) return;
  const index = client ? rows.querySelectorAll('.newcamd-row').length + 1 : nextNewcamdIndex();
  rows.insertAdjacentHTML('beforeend', newcamdRowHtml(client || defaultNewcamdClient(index), index));
}
function removeNewcamdRow(button) {
  button?.closest('.newcamd-row')?.remove();
}
function collectNewcamdClients() {
  return [...document.querySelectorAll('.newcamd-row')].map((row, index) => {
    const value = field => row.querySelector(`[data-cam-field="${field}"]`)?.value || '';
    const id = value('id') || `newcamd-${index+1}`;
    return {
      id,
      name: value('name') || id,
      max_services: Math.max(1, Math.min(64, Number(value('max_services') || 10))),
      backend_id: value('backend_id') || 'newcamd',
      backend_config: {host:value('host') || '127.0.0.1', port:Number(value('port') || 15000), user:value('user') || 'user', pass:value('pass'), des:value('des') || '0102030405060708091011121314'}
    };
  }).filter(client => client.id);
}
async function saveNewcamdSettings() {
  const clients = collectNewcamdClients();
  try {
    const data = await fetchJson('/api/cam-client-settings', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({clients})}, 15000);
    if (data.result === 'error') throw new Error(data.error || 'Failed to save CAM clients');
    caManagerState = data;
    state.cam_clients = clients;
    closeModal();
    fetchState();
  } catch (error) {
    uiError(error?.message || error);
  }
}
function renderNewcamdRows(clients) {
  const rows = document.getElementById('newcamdRows');
  if (!rows) return;
  rows.innerHTML = clients.map((client,index)=>newcamdRowHtml(client,index)).join('');
}
async function openNewcamdModal() {
  const clients = newcamdSettingsClients();
  openModal(`
    <h2>Newcamd / OSCam CAM clients</h2>
    <div class="cam-empty" id="newcamdSettingsStatus">Add one OSCam/Newcamd client per server or satellite package, then select the client when adding DVB channels.</div>
    <div id="newcamdRows" class="form-grid full">${clients.map((client,index)=>newcamdRowHtml(client,index)).join('')}</div>
    <div class="modal-actions">
      <button class="button-secondary" type="button" onclick="addNewcamdRow()">Add CAM client</button>
      <button class="button-secondary" onclick="closeModal()">Cancel</button>
      <button class="button-primary" onclick="saveNewcamdSettings()">Save</button>
    </div>
  `);
  loadCaManager().then(snapshot => {
    const status = document.getElementById('newcamdSettingsStatus');
    if (status) status.textContent = `${(snapshot.clients || []).length} CAM client(s) configured. Use Add CAM client for another OSCam/Newcamd server.`;
  }).catch(error => {
    const status = document.getElementById('newcamdSettingsStatus');
    if (status) status.textContent = error?.message || 'CAM manager refresh failed';
  });
}
function refreshSatelliteFrontendOptions(preferredFrontend=null) {
  const adapterSelect = document.getElementById('satAdapter');
  const frontendSelect = document.getElementById('satFrontend');
  if (!adapterSelect || !frontendSelect) return;
  const adapter = Number(adapterSelect.value || 0);
  const frontends = dvbAdapters
    .filter(item => Number(item.adapter) === adapter)
    .sort((a,b) => Number(a.frontend) - Number(b.frontend));
  const previous = preferredFrontend === null ? Number(frontendSelect.value || 0) : Number(preferredFrontend);
  if (!frontends.length) {
    frontendSelect.innerHTML = '<option value="0">Frontend 0</option>';
    frontendSelect.value = '0';
    return;
  }
  frontendSelect.innerHTML = frontends.map(item => {
    const f = Number(item.frontend || 0);
    const device = String(item.device || `/dev/dvb/adapter${adapter}/frontend${f}`);
    const consumers = Number(item.consumers || 0);
    const freq = Number(item.frequency_khz || 0);
    const tune = consumers > 0 ? ` · SHARED ${consumers}${freq ? ` · ${(freq/1000).toFixed(0)} MHz ${satEscape(String(item.polarity||''))}` : ''}` : ' · свободен';
    return `<option value="${f}">Frontend ${f}${tune} · ${satEscape(device)}</option>`;
  }).join('');
  const selected = frontends.some(item => Number(item.frontend) === previous)
    ? previous : Number(frontends[0].frontend || 0);
  frontendSelect.value = String(selected);
}

function refreshSatelliteAdapterOptions(preferredAdapter=null, preferredFrontend=null) {
  const adapterSelect = document.getElementById('satAdapter');
  if (!adapterSelect) return;
  const previous = preferredAdapter === null ? Number(adapterSelect.value || 0) : Number(preferredAdapter);
  const adapters = [...new Set(dvbAdapters.map(item => Number(item.adapter)).filter(Number.isFinite))].sort((a,b)=>a-b);
  if (!adapters.length) {
    adapterSelect.innerHTML = '<option value="0">Adapter 0</option>';
    adapterSelect.value = '0';
    refreshSatelliteFrontendOptions(preferredFrontend);
    return;
  }
  adapterSelect.innerHTML = adapters.map(adapter => {
    const adapterItems = dvbAdapters.filter(item => Number(item.adapter) === adapter);
    const count = adapterItems.length;
    const used = adapterItems.reduce((sum,item)=>sum + (Number(item.consumers||0)>0 ? 1 : 0), 0);
    return `<option value="${adapter}">Adapter ${adapter}${count > 1 ? ` · ${count} frontend` : ''}${used ? ` · занято ${used}` : ''}</option>`;
  }).join('');
  const selected = adapters.includes(previous) ? previous : adapters[0];
  adapterSelect.value = String(selected);
  refreshSatelliteFrontendOptions(preferredFrontend);
}

function satelliteTunePayload() {
  const adapter = Number(document.getElementById('satAdapter')?.value || 0);
  const frontend = Number(document.getElementById('satFrontend')?.value || 0);
  return {
    adapter,
    frontend,
    frequency_mhz:Number(document.getElementById('satFrequency')?.value || 0),
    symbol_rate:Number(document.getElementById('satSymbolRate')?.value || 0),
    polarity:document.getElementById('satPolarity')?.value || 'H',
    delivery_system:document.getElementById('satDeliverySystem')?.value || 'dvb-s2',
    modulation:document.getElementById('satModulation')?.value || 'auto',
    fec:document.getElementById('satFec')?.value || 'auto',
    diseqc_source:Number(document.getElementById('satDiseqc')?.value ?? -1),
    lnb_lof1_mhz:Number(document.getElementById('satLof1')?.value || 9750),
    lnb_lof2_mhz:Number(document.getElementById('satLof2')?.value || 10600),
    lnb_slof_mhz:Number(document.getElementById('satSlof')?.value || 11700),
    stream_id:Number(document.getElementById('satStreamId')?.value ?? -1),
    hold_lock:!!document.getElementById('satHoldLock')?.checked
  };
}
function updateSatelliteMeters(data={}) {
  const signal = Math.max(0, Math.min(100, Number(data.signal || 0)));
  const quality = Math.max(0, Math.min(100, Number(data.quality || 0)));
  const signalValue = document.getElementById('satSignalValue');
  const qualityValue = document.getElementById('satQualityValue');
  const signalBar = document.getElementById('satSignalBar');
  const qualityBar = document.getElementById('satQualityBar');
  const lock = document.getElementById('satLock');
  if (signalValue) signalValue.textContent = `${Math.round(signal)}%${Number.isFinite(Number(data.signal_db)) ? ` / ${Number(data.signal_db).toFixed(1)} dB` : ''}`;
  if (qualityValue) qualityValue.textContent = `${Math.round(quality)}%${Number.isFinite(Number(data.cnr_db)) ? ` / C/N ${Number(data.cnr_db).toFixed(1)} dB` : ''}`;
  if (signalBar) signalBar.style.width = `${signal}%`;
  if (qualityBar) qualityBar.style.width = `${quality}%`;
  if (lock) {
    lock.textContent = data.locked ? 'LOCK' : 'NO LOCK';
    lock.classList.toggle('locked', !!data.locked);
  }
}
function stopSatelliteSignalPolling() {
  clearInterval(satelliteSignalTimer);
  satelliteSignalTimer = null;
  ++satelliteTuneGeneration;
  if (satelliteSignalController) satelliteSignalController.abort();
  satelliteSignalController = null;
  satelliteSignalPending = false;
  satelliteScanning = false;
}
function resetSatelliteServicesForTuneChange() {
  satelliteServices = [];
  satelliteLastScan = null;
  const services = document.getElementById('satServices');
  const count = document.getElementById('satFoundCount');
  if (count) count.textContent = '0';
  if (services) services.innerHTML = '<div class="sat-empty">Параметры тюнера изменены. Выполните сканирование заново.</div>';
}
function satelliteAdapterChanged() {
  ++satelliteTuneGeneration;
  if (satelliteSignalController) satelliteSignalController.abort();
  satelliteSignalController = null;
  satelliteSignalPending = false;
  refreshSatelliteFrontendOptions();
  resetSatelliteServicesForTuneChange();
  const adapter = Number(document.getElementById('satAdapter')?.value || 0);
  const frontend = Number(document.getElementById('satFrontend')?.value || 0);
  const info = document.getElementById('satDeviceInfo');
  if (info) info.textContent = `Выбран /dev/dvb/adapter${adapter}/frontend${frontend}`;
  updateSatelliteSignal();
}
function satelliteFrontendChanged() {
  ++satelliteTuneGeneration;
  if (satelliteSignalController) satelliteSignalController.abort();
  satelliteSignalController = null;
  satelliteSignalPending = false;
  resetSatelliteServicesForTuneChange();
  const adapter = Number(document.getElementById('satAdapter')?.value || 0);
  const frontend = Number(document.getElementById('satFrontend')?.value || 0);
  const info = document.getElementById('satDeviceInfo');
  if (info) info.textContent = `Выбран /dev/dvb/adapter${adapter}/frontend${frontend}`;
  updateSatelliteSignal();
}
async function updateSatelliteSignal() {
  if (satelliteScanning || satelliteSignalPending || !document.getElementById('satFrequency')) return;
  const generation = satelliteTuneGeneration;
  const payload = satelliteTunePayload();
  const controller = new AbortController();
  satelliteSignalController = controller;
  satelliteSignalPending = true;
  try {
    const response = await fetch('/api/dvb-signal', {
      method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload), signal:controller.signal
    });
    const data = await response.json();
    if (generation !== satelliteTuneGeneration || controller !== satelliteSignalController) return;
    updateSatelliteMeters(data);
    const info = document.getElementById('satDeviceInfo');
    if (info) {
      const actualAdapter = Number(data.adapter ?? payload.adapter);
      const actualFrontend = Number(data.frontend ?? payload.frontend);
      const device = data.device || `/dev/dvb/adapter${actualAdapter}/frontend${actualFrontend}`;
      info.textContent = data.error && !data.locked
        ? `${device}: ${data.error}`
        : `${device}${data.locked ? ' · LOCK' : ''}`;
    }
  } catch (error) {
    if (error?.name === 'AbortError') return;
    const info = document.getElementById('satDeviceInfo');
    if (info && generation === satelliteTuneGeneration) info.textContent = 'Ошибка чтения DVB frontend';
  } finally {
    if (controller === satelliteSignalController) {
      satelliteSignalController = null;
      satelliteSignalPending = false;
    }
  }
}
function scheduleSatelliteSignalPolling() {
  clearInterval(satelliteSignalTimer);
  satelliteSignalTimer = setInterval(updateSatelliteSignal, 2500);
  updateSatelliteSignal();
}
async function loadSatelliteAdapters() {
  const info = document.getElementById('satDeviceInfo');
  try {
    const response = await fetch('/api/dvb-adapters', {cache:'no-store'});
    const data = await response.json();
    await loadCamClients();
    const adapterBeforeLoad = Number(document.getElementById('satAdapter')?.value || 0);
    const frontendBeforeLoad = Number(document.getElementById('satFrontend')?.value || 0);
    dvbAdapters = Array.isArray(data.adapters) ? data.adapters : [];
    if (!data.dvbsrc_available) {
      refreshSatelliteAdapterOptions(adapterBeforeLoad, frontendBeforeLoad);
      if (info) info.textContent = 'GStreamer dvbsrc не найден. Установите gstreamer1.0-plugins-bad.';
      return;
    }
    if (!dvbAdapters.length) {
      refreshSatelliteAdapterOptions(adapterBeforeLoad, frontendBeforeLoad);
      if (info) info.textContent = 'DVB frontend не обнаружен в /dev/dvb.';
      return;
    }
    const adapters = [...new Set(dvbAdapters.map(item => Number(item.adapter)).filter(Number.isFinite))];
    const preferredAdapter = adapters.includes(adapterBeforeLoad)
      ? adapterBeforeLoad
      : Number(dvbAdapters[0].adapter || 0);
    const preferredFrontend = dvbAdapters.some(item => Number(item.adapter) === preferredAdapter && Number(item.frontend) === frontendBeforeLoad)
      ? frontendBeforeLoad
      : Number((dvbAdapters.find(item => Number(item.adapter) === preferredAdapter) || dvbAdapters[0]).frontend || 0);
    refreshSatelliteAdapterOptions(preferredAdapter, preferredFrontend);
    if (info) info.textContent = `Выбран /dev/dvb/adapter${preferredAdapter}/frontend${preferredFrontend} · доступно: ${dvbAdapters.map(item=>item.device).join(', ')}`;
    updateSatelliteSignal();
  } catch (error) {
    dvbAdapters = [];
    refreshSatelliteAdapterOptions();
    if (info) info.textContent = 'Не удалось получить список DVB frontend';
  }
}
function renderSatelliteServices() {
  const container = document.getElementById('satServices');
  const count = document.getElementById('satFoundCount');
  if (count) count.textContent = String(satelliteServices.length);
  if (!container) return;
  if (!satelliteServices.length) {
    container.innerHTML = '<div class="sat-empty">Каналы не найдены. Проверьте частоту, Symbol Rate, поляризацию и уровень сигнала.</div>';
    return;
  }
  container.innerHTML = `
    <div class="sat-service-head"><span><input id="satSelectAll" type="checkbox" checked onchange="toggleAllSatelliteServices(this.checked)" /></span><span>Канал</span><span>Провайдер</span><span>Доступ</span><span>SID</span><span>PMT PID</span></div>
    ${satelliteServices.map((service,index)=>`
      <label class="sat-service-row">
        <input class="sat-service-check" type="checkbox" data-index="${index}" ${service.pmt_ready===false ? 'disabled' : 'checked'} />
        <span class="sat-service-name" title="${satEscape(service.name)}">${satEscape(service.name || ('Service ' + service.service_id))}</span>
        <span class="sat-service-provider" title="${satEscape(service.provider)}">${satEscape(service.provider || '—')}</span>
        <span><span class="sat-access ${service.scrambled ? 'ca' : (service.pmt_ready===false ? 'unknown' : 'fta')}" title="${service.scrambled ? 'Кодированный канал (CA)' : (service.pmt_ready===false ? 'PMT/PID ещё не получены' : 'Открытый канал (FTA)')}">${service.scrambled ? 'КОД.' : (service.pmt_ready===false ? 'ПРОВ.' : 'FTA')}</span></span>
        <span>${Number(service.service_id || 0)}</span>
        <span title="PCR PID: ${Number(service.pcr_pid || 0)}; PIDs: ${(Array.isArray(service.stream_pids) ? service.stream_pids : []).join(', ')}">${Number(service.pmt_pid || 0)}</span>
      </label>`).join('')}`;
}
function toggleAllSatelliteServices(checked) {
  document.querySelectorAll('.sat-service-check:not(:disabled)').forEach(input => { input.checked = checked; });
}
async function startSatelliteScan() {
  const button = document.getElementById('satScanButton');
  const status = document.getElementById('satScanStatus');
  satelliteScanning = true;
  clearInterval(satelliteSignalTimer);
  satelliteSignalTimer = null;
  ++satelliteTuneGeneration;
  const generation = satelliteTuneGeneration;
  if (satelliteSignalController) satelliteSignalController.abort();
  satelliteSignalController = null;
  satelliteSignalPending = false;
  const payload = satelliteTunePayload();
  const holdLock = !!payload.hold_lock;
  if (button) button.disabled = true;
  if (status) status.textContent = `Сканирование /dev/dvb/adapter${payload.adapter}/frontend${payload.frontend}${holdLock ? ' · удержание LOCK' : ''}...`;
  try {
    const response = await fetch('/api/dvb-scan', {
      method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)
    });
    const data = await response.json();
    if (generation !== satelliteTuneGeneration) return;
    const actualAdapter = Number(data.adapter ?? payload.adapter);
    const actualFrontend = Number(data.frontend ?? payload.frontend);
    if (actualAdapter !== payload.adapter || actualFrontend !== payload.frontend) {
      throw new Error(`DVB scan adapter mismatch: requested ${payload.adapter}:${payload.frontend}, server used ${actualAdapter}:${actualFrontend}`);
    }
    updateSatelliteMeters(data);
    satelliteServices = Array.isArray(data.services) ? data.services : [];
    satelliteLastScan = {adapter:actualAdapter, frontend:actualFrontend, device:data.device || `/dev/dvb/adapter${actualAdapter}/frontend${actualFrontend}`};
    renderSatelliteServices();
    if (status) {
      const caCount = satelliteServices.filter(service=>service.scrambled === true).length;
      const pendingCount = satelliteServices.filter(service=>service.pmt_ready === false).length;
      const ftaCount = satelliteServices.filter(service=>service.scrambled !== true && service.pmt_ready !== false).length;
      const scanDevice = satelliteLastScan?.device || `/dev/dvb/adapter${payload.adapter}/frontend${payload.frontend}`;
      status.textContent = data.error && !satelliteServices.length
        ? `${scanDevice}: ${data.error}`
        : `${scanDevice} · Найдено: ${satelliteServices.length} · FTA: ${ftaCount} · Код.: ${caCount}${pendingCount ? ` · PMT: ${pendingCount} не готово` : ''}${data.error ? ` (${data.error})` : ''}`;
    }
  } catch (error) {
    satelliteServices = [];
    renderSatelliteServices();
    if (status) status.textContent = 'Ошибка сканирования DVB-S/S2';
  } finally {
    satelliteScanning = false;
    if (button) button.disabled = false;
    // Do not restart background /api/dvb-signal polling after a scan. The scan
    // already produced the final signal/quality snapshot. Reopening dvbsrc here
    // can race the Start button of a newly saved tile and make the real stream
    // fail with gst_base_src_start()/EBUSY. Changing tuner parameters still
    // performs an explicit one-shot signal update.
    clearInterval(satelliteSignalTimer);
    satelliteSignalTimer = null;
  }
}
async function saveSelectedSatelliteChannels() {
  // Stop and abort modal probing before saving. The server-side stream startup
  // uses the same frontend gate, so even an already-dispatched probe must finish
  // and release dvbsrc before the tile can acquire the tuner.
  stopSatelliteSignalPolling();
  const selected = [...document.querySelectorAll('.sat-service-check:checked')]
    .map(input => satelliteServices[Number(input.dataset.index)])
    .filter(Boolean);
  if (!selected.length) {
    alert('Выберите хотя бы один канал.');
    return;
  }
  const saveButton = document.getElementById('satSaveButton');
  if (saveButton) saveButton.disabled = true;
  const payload = {
    channels:selected,
    output_type:document.getElementById('satOutputType')?.value || 'udp-vbr',
    output_host:document.getElementById('satOutputHost')?.value || '239.255.10.1',
    base_port:Number(document.getElementById('satBasePort')?.value || 5000),
    target_bitrate_kbps:Number(document.getElementById('satTargetBitrate')?.value || 12000),
    interface_address:document.getElementById('satOutputInterface')?.value || '',
    conditional_access_client:document.getElementById('satCamClientSelect')?.value || '',
    auto_start:document.getElementById('satAutoStart')?.checked === true
  };
  try {
    const response = await fetch('/api/dvb-add-channels', {
      method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload)
    });
    const result = await response.json();
    if (result.error) throw new Error(result.error);
    closeModal();
    fetchState();
  } catch (error) {
    alert(error.message || 'Не удалось сохранить спутниковые каналы');
    if (saveButton) saveButton.disabled = false;
  }
}
function openAddChannelModal() {
  satelliteServices = [];
  openModal(`
    <h2>Добавить канал — DVB-S/S2</h2>
    <div class="sat-signal-panel">
      <div class="sat-meter"><div class="sat-meter-head"><span>Сигнал</span><strong id="satSignalValue">0%</strong></div><div class="sat-bar"><span id="satSignalBar"></span></div></div>
      <div class="sat-meter"><div class="sat-meter-head"><span>Качество</span><strong id="satQualityValue">0%</strong></div><div class="sat-bar"><span id="satQualityBar"></span></div></div>
      <span id="satLock" class="sat-lock">NO LOCK</span>
    </div>
    <div class="sat-form">
      <div class="sat-field"><label>Adapter</label><select id="satAdapter" onchange="satelliteAdapterChanged()"><option value="0">Adapter 0</option></select></div>
      <div class="sat-field"><label>Frontend</label><select id="satFrontend" onchange="satelliteFrontendChanged()"><option value="0">Frontend 0</option></select></div>
      <div class="sat-field"><label>Частота, MHz</label><input id="satFrequency" type="number" min="900" max="14000" step="0.001" value="11727" onchange="updateSatelliteSignal()" /></div>
      <div class="sat-field"><label>Symbol Rate, kSym/s</label><input id="satSymbolRate" type="number" min="100" max="60000" step="1" value="27500" onchange="updateSatelliteSignal()" /></div>
      <div class="sat-field"><label>Поляризация</label><select id="satPolarity" onchange="updateSatelliteSignal()"><option value="H">H — Horizontal</option><option value="V">V — Vertical</option></select></div>
      <div class="sat-field"><label>Стандарт</label><select id="satDeliverySystem" onchange="updateSatelliteSignal()"><option value="dvb-s2">DVB-S2</option><option value="dvb-s">DVB-S</option></select></div>
      <div class="sat-field"><label>Модуляция</label><select id="satModulation" onchange="updateSatelliteSignal()"><option value="auto">Auto</option><option value="qpsk">QPSK</option><option value="8psk">8PSK</option><option value="16apsk">16APSK</option><option value="32apsk">32APSK</option></select></div>
      <div class="sat-field"><label>FEC</label><select id="satFec" onchange="updateSatelliteSignal()"><option value="auto">Auto</option><option>1/2</option><option>2/3</option><option>3/4</option><option>4/5</option><option>5/6</option><option>7/8</option><option>8/9</option><option>9/10</option><option>3/5</option></select></div>
      <div class="sat-field"><label>DiSEqC source</label><input id="satDiseqc" type="number" min="-1" max="7" value="-1" onchange="updateSatelliteSignal()" /></div>
      <div class="sat-field"><label>LNB LOF1, MHz</label><input id="satLof1" type="number" value="9750" onchange="updateSatelliteSignal()" /></div>
      <div class="sat-field"><label>LNB LOF2, MHz</label><input id="satLof2" type="number" value="10600" onchange="updateSatelliteSignal()" /></div>
      <div class="sat-field"><label>LNB SLOF, MHz</label><input id="satSlof" type="number" value="11700" onchange="updateSatelliteSignal()" /></div>
      <div class="sat-field"><label>ISI / Stream ID</label><input id="satStreamId" type="number" min="-1" max="255" value="-1" onchange="updateSatelliteSignal()" /></div>
      <div class="sat-field wide"><label>Сканирование</label><div class="checkbox-inline"><input id="satHoldLock" type="checkbox" checked /><span>Удерживать LOCK</span></div></div>
    </div>
    <div id="satDeviceInfo" class="sat-scan-status" style="margin-top:8px">Поиск DVB frontend...</div>
    <div class="cam-panel">
      <div class="cam-head"><strong>CAM clients / Newcamd</strong><button id="satCamRefresh" class="button-secondary" type="button" onclick="refreshCamClients()">Refresh</button></div>
      <div id="satCamClients" class="cam-list"><div class="cam-empty">Loading CAM clients...</div></div>
      <div class="cam-select"><div class="sat-field"><label>CAM for scrambled channels</label><select id="satCamClientSelect"><option value="">Do not use CAM / FTA</option></select></div><small>Select the CAM client used to descramble saved encrypted services. Configure Newcamd/OSCam in System / Newcamd.</small></div>
    </div>
    <div class="sat-actions">
      <button id="satScanButton" class="button-primary" onclick="startSatelliteScan()">Сканировать каналы</button>
      <span id="satScanStatus" class="sat-scan-status">Найдено: <span id="satFoundCount">0</span></span>
    </div>
    <div id="satServices" class="sat-services"><div class="sat-empty">Нажмите «Сканировать каналы».</div></div>
    <div class="sat-output">
      <div class="sat-field"><label>Выход</label><select id="satOutputType"><option value="udp-vbr">UDP VBR</option><option value="udp-cbr">UDP CBR</option></select></div>
      <div class="sat-field"><label>Multicast / IP</label><input id="satOutputHost" value="239.255.10.1" /></div>
      <div class="sat-field"><label>Первый UDP порт</label><input id="satBasePort" type="number" min="1" max="65535" value="5000" /></div>
      <div class="sat-field"><label>CBR bitrate, кбит/с</label><input id="satTargetBitrate" type="number" min="500" max="100000" value="12000" /></div>
      <div class="sat-field"><label>Выходной интерфейс</label><select id="satOutputInterface"><option value="">Авто (системный маршрут)</option>${(state.interfaces||[]).map(i=>`<option value="${satEscape(i.address)}">${satEscape(i.name)} (${satEscape(i.address)})</option>`).join('')}</select></div>
      <div class="sat-field wide"><label>Автозапуск</label><div class="checkbox-inline"><input id="satAutoStart" type="checkbox" /><span>Запускать созданные каналы после перезапуска</span></div></div>
    </div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">Отмена</button>
      <button id="satSaveButton" class="button-primary" onclick="saveSelectedSatelliteChannels()">Сохранить выбранные</button>
    </div>
  `);
  document.getElementById('modalContent').classList.add('satellite-modal');
  updateHeaderHeight();
  document.getElementById('modal').classList.add('satellite-open');
  loadSatelliteAdapters();
  scheduleSatelliteSignalPolling();
}

function openStreamModal() {
  openStreamForm({
    id: 'stream-' + Date.now(),
    name:'', input_uri:'', backup_input_uri:'', backup_input_type:'url', backup_file_loop:false, output_type:'udp-cbr', output_mode:'listener', output_host:'127.0.0.1', output_port:1234,
    interface_address:'', input_interface_address:'', input_mode:'auto', conditional_access_client:'', test_pattern:false, auto_start:false, remap_enabled:false, cbr:true, target_bitrate:2000000, transcode_enabled:false, transcode_resolution:'1920x1080', transcode_video_bitrate:6000000, transcode_audio_codec:'aac', transcode_audio_bitrate:192000,
    audio_pid:0, video_pid:0, input_service_id:0, service_id:1, service_name:'', service_provider:'', additional_outputs:[]
  });
}
function outputTypeOptions(selected) {
  const options = [
    ['udp-vbr', 'UDP MPEG-TS VBR'],
    ['udp-cbr', 'UDP MPEG-TS CBR'],
    ['rtp', 'RTP MPEG-TS'],
    ['srt', 'SRT'],
    ['http', 'HTTP TS'],
    ['hls', 'HLS'],
    ['rtsp', 'RTSP Push'],
    ['rtmp', 'RTMP Push'],
    ['youtube', 'YouTube']
  ];
  return options.map(([value, label]) => `<option value="${value}" ${selected===value?'selected':''}>${label}</option>`).join('');
}
function renderOutputRows(outputs, links=[], startIndex=0) {
  return outputs.map((output, offset) => {
    const index = startIndex + offset;
    const type = normalizedOutputType(output);
    const link = links[index]?.url || '';
    return `
      <div class="output-row" data-output-index="${index}">
        <div class="form-row"><label>${index === 0 ? 'Основной формат' : 'Доп. формат'}</label><select data-output-field="output_type" onchange="updateOutputHints()">${outputTypeOptions(type)}</select></div>
        <div class="form-row"><label>SRT режим</label><select data-output-field="output_mode" onchange="updateOutputHints()"><option value="listener" ${(!output.output_mode || output.output_mode==='listener')?'selected':''}>Listener</option><option value="caller" ${output.output_mode==='caller'?'selected':''}>Caller</option></select></div>
        <div class="form-row"><label data-output-host-label>Адрес выхода</label><input data-output-field="output_host" value="${output.output_host||'239.0.0.1'}" placeholder="239.0.0.1" /></div>
        <div class="form-row"><label data-output-port-label>Порт</label><input data-output-field="output_port" type="number" min="1" max="65535" value="${output.output_port||1234}" placeholder="1234" /></div>
        <button class="remove-output" type="button" onclick="removeStreamOutput(this)" ${index === 0 ? 'disabled' : ''}>×</button>
        <div class="form-row full" style="grid-column:1/-1"><label>URL для плеера</label><input readonly value="${link}" placeholder="Ссылка появится после сохранения" /></div>
      </div>
    `;
  }).join('');
}
function renumberOutputRows() {
  document.querySelectorAll('.output-row').forEach((row, index) => {
    row.dataset.outputIndex = index;
    const label = row.querySelector('label');
    if (label) label.textContent = index === 0 ? 'Основной формат' : 'Доп. формат';
    const remove = row.querySelector('.remove-output');
    if (remove) remove.disabled = index === 0;
  });
}
function addStreamOutput() {
  const list = document.getElementById('streamOutputs');
  if (!list) return;
  const index = list.querySelectorAll('.output-row').length;
  const iface = document.getElementById('streamInterface')?.value || '127.0.0.1';
  list.insertAdjacentHTML('beforeend', renderOutputRows([{output_type:'hls', output_mode:'listener', output_host:iface, output_port:state.http_port || 9000, cbr:document.getElementById('streamCbr')?.checked}], [], index));
  updateOutputHints();
}
function removeStreamOutput(button) {
  const row = button.closest('.output-row');
  if (!row || Number(row.dataset.outputIndex || 0) === 0) return;
  row.remove();
  renumberOutputRows();
  updateOutputHints();
}
function collectOutputRows() {
  const rows = [...document.querySelectorAll('.output-row')];
  return rows.map(row => {
    const value = field => row.querySelector(`[data-output-field="${field}"]`)?.value || '';
    return {
      output_type: value('output_type') || 'udp-cbr',
      output_mode: value('output_mode') || 'listener',
      output_host: value('output_host') || '127.0.0.1',
      output_port: Number(value('output_port') || 1234)
    };
  });
}
function formatBackupFileSize(bytes) {
  const value = Number(bytes || 0);
  if (value < 1024) return `${value} B`;
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`;
  if (value < 1024 * 1024 * 1024) return `${(value / 1024 / 1024).toFixed(1)} MB`;
  return `${(value / 1024 / 1024 / 1024).toFixed(2)} GB`;
}
function toggleBackupFileLibrary() {
  document.getElementById('streamBackupLibrary')?.classList.toggle('open');
}
function selectUploadedBackupFile(path, name) {
  const backupInput = document.getElementById('streamBackupInput');
  const typeSelect = document.getElementById('streamBackupInputType');
  const status = document.getElementById('streamBackupUploadStatus');
  if (typeSelect) typeSelect.value = 'file';
  if (backupInput) backupInput.value = path || '';
  if (status) status.textContent = name ? `Выбран файл: ${name}` : '';
  document.getElementById('streamBackupLibrary')?.classList.remove('open');
  updateBackupInputMode();
}
function loadUploadedBackupFiles() {
  const menu = document.getElementById('streamBackupLibraryMenu');
  const button = document.getElementById('streamBackupLibraryButton');
  if (!menu) return;
  menu.textContent = '';
  const loading = document.createElement('div');
  loading.className = 'backup-library-empty';
  loading.textContent = 'Загрузка списка...';
  menu.appendChild(loading);
  fetch('/api/backup-files').then(r=>r.json()).then(result=>{
    menu.textContent = '';
    const files = Array.isArray(result.files) ? result.files : [];
    if (!files.length) {
      const empty = document.createElement('div');
      empty.className = 'backup-library-empty';
      empty.textContent = result.error || 'Загруженных файлов нет';
      menu.appendChild(empty);
      if (button) button.textContent = 'Выбрать ранее загруженный файл';
      return;
    }
    files.forEach(file=>{
      const row = document.createElement('div');
      row.className = 'backup-library-item';
      const select = document.createElement('button');
      select.type = 'button';
      select.className = 'backup-library-select';
      select.textContent = `${file.name} (${formatBackupFileSize(file.size)})`;
      select.title = file.path || file.name;
      select.onclick = ()=>selectUploadedBackupFile(file.path, file.name);
      const remove = document.createElement('button');
      remove.type = 'button';
      remove.className = 'backup-library-delete';
      remove.textContent = '×';
      remove.title = `Удалить ${file.name}`;
      remove.onclick = event=>{event.stopPropagation(); deleteUploadedBackupFile(file);};
      row.append(select, remove);
      menu.appendChild(row);
    });
  }).catch(()=>{
    menu.textContent = '';
    const error = document.createElement('div');
    error.className = 'backup-library-empty';
    error.textContent = 'Не удалось загрузить список файлов';
    menu.appendChild(error);
  });
}
function deleteUploadedBackupFile(file) {
  if (!file?.name || !confirm(`Удалить файл «${file.name}»?`)) return;
  fetch('/api/delete-backup-file', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({name:file.name})
  }).then(r=>r.json()).then(result=>{
    if (result.result !== 'ok') throw new Error(result.error || 'Не удалось удалить файл');
    const backupInput = document.getElementById('streamBackupInput');
    const status = document.getElementById('streamBackupUploadStatus');
    if (backupInput && (backupInput.value === file.path || backupInput.value.endsWith('/' + file.name) || backupInput.value.endsWith('\\\\' + file.name))) {
      backupInput.value = '';
      if (status) status.textContent = 'Выбранный файл удалён';
    }
    loadUploadedBackupFiles();
  }).catch(error=>alert(error.message || 'Ошибка удаления файла'));
}
function updateBackupInputMode() {
  const type = document.getElementById('streamBackupInputType')?.value || 'url';
  const pathInput = document.getElementById('streamBackupInput');
  const fileRow = document.getElementById('streamBackupFileRow');
  const library = document.getElementById('streamBackupLibrary');
  const loopRow = document.getElementById('streamBackupFileLoopRow');
  if (pathInput) {
    pathInput.placeholder = type === 'file'
      ? '/path/to/replacement.ts или загрузите файл ниже'
      : 'http://192.168.1.2/...';
  }
  if (fileRow) fileRow.style.display = type === 'file' ? '' : 'none';
  if (library) library.style.display = type === 'file' ? '' : 'none';
  if (loopRow) loopRow.style.display = type === 'file' ? '' : 'none';
}
function uploadBackupReplacementFile(streamId, input) {
  const file = input?.files?.[0];
  const status = document.getElementById('streamBackupUploadStatus');
  const backupInput = document.getElementById('streamBackupInput');
  const typeSelect = document.getElementById('streamBackupInputType');
  if (!file || !backupInput) return;
  if (typeSelect) typeSelect.value = 'file';
  updateBackupInputMode();
  if (status) status.textContent = 'Загрузка...';
  fetch(`/api/upload-backup-file?stream_id=${encodeURIComponent(streamId)}&filename=${encodeURIComponent(file.name)}`, {
    method:'POST',
    headers:{'Content-Type':'application/octet-stream'},
    body:file
  }).then(r=>r.json()).then(result=>{
    if (result.path) {
      backupInput.value = result.path;
      if (status) status.textContent = `Файл загружен: ${result.filename || file.name}`;
      loadUploadedBackupFiles();
    } else {
      if (status) status.textContent = result.error || 'Не удалось загрузить файл';
    }
  }).catch(()=>{
    if (status) status.textContent = 'Ошибка загрузки файла';
  });
}
function openStreamForm(stream) {
  const renderStreamForm = () => {
    const ifaceOptions = state.interfaces || [];
    const outputOptions = ifaceOptions.map(i=>`<option value="${i.address}" ${i.address===stream.interface_address?'selected':''}>${i.name} (${i.address})</option>`).join('');
    const hasInputInterface = Object.prototype.hasOwnProperty.call(stream, 'input_interface_address');
    const selectedInputInterface = hasInputInterface ? (stream.input_interface_address || '') : (stream.interface_address || '');
    const inputOptions = ifaceOptions.map(i=>`<option value="${i.address}" ${i.address===selectedInputInterface?'selected':''}>${i.name} (${i.address})</option>`).join('');
    const outputs = outputConfigsForStream(stream);
    const outputType = normalizedOutputType(outputs[0] || stream);
    const links = Array.isArray(stream.vlc_links) ? stream.vlc_links : [];
    const transcoderInfo = state.transcoder || {};
    const transcoderAvailable = transcoderInfo.available === true;
    const transcoderMissing = Array.isArray(transcoderInfo.missing_elements) ? transcoderInfo.missing_elements.join(', ') : '';
    const camOptions = camClientOptions(stream.conditional_access_client || '');
    const transcoderStatus = transcoderAvailable
      ? `Доступно: H.264 ${transcoderInfo.video_encoder || 'encoder'}, AAC ${transcoderInfo.aac_encoder || 'нет'}, MP3 ${transcoderInfo.mp3_encoder || 'нет'}, deinterlace ${transcoderInfo.deinterlace ? 'да' : 'нет'}`
      : `Недоступно: ${transcoderMissing || transcoderInfo.message || 'не установлены необходимые GStreamer-плагины'}`;
    openModal(`
      <h2>${stream.name ? 'Редактирование трансляции' : 'Настройка трансляции'}</h2>
      <div class="form-grid">
        <div class="form-row full"><label>Имя плитки</label><input class="compact" id="streamName" value="${stream.name||''}" placeholder="Belarus 5" /></div>
        <div class="form-row full"><div class="input-main-row"><div class="form-row"><label>Входной URL (Основной)</label><input id="streamInput" value="${stream.input_uri||''}" placeholder="rtsp://camera/live, udp://@:9087, udp://239.1.1.1:1234 или https://host/live.m3u8" /></div><div class="form-row"><label>Интерфейс входа</label><select id="streamInputInterface"><option value="">Auto / все интерфейсы</option>${inputOptions}</select></div><div class="form-row"><label>Режим входа</label><select id="streamInputMode"><option value="auto" ${(!stream.input_mode || stream.input_mode==='auto')?'selected':''}>Auto</option><option value="hls" ${stream.input_mode==='hls'?'selected':''}>HLS</option><option value="caller" ${stream.input_mode==='caller'?'selected':''}>SRT Caller</option><option value="listener" ${stream.input_mode==='listener'?'selected':''}>SRT Listener</option></select></div></div></div>
        <div class="form-row full" id="streamCamRow" style="display:${String(stream.input_uri||'').startsWith('dvb://')?'': 'none'}"><label>CAM client (scrambled DVB)</label><select id="streamConditionalAccessClient">${camOptions}</select><small>Select a CAM/Newcamd client for encrypted DVB services. FTA streams do not use this setting.</small></div>
        <div class="form-row full"><label>Резерв / файл замены</label><div class="backup-source"><select id="streamBackupInputType" onchange="updateBackupInputMode()"><option value="url" ${(!stream.backup_input_type || stream.backup_input_type==='url')?'selected':''}>URL резерва</option><option value="file" ${stream.backup_input_type==='file'?'selected':''}>Файл замены</option></select><input id="streamBackupInput" value="${stream.backup_input_uri||''}" placeholder="http://192.168.1.2/..." /><div class="backup-library" id="streamBackupLibrary"><button class="backup-library-button" id="streamBackupLibraryButton" type="button" onclick="toggleBackupFileLibrary()">Выбрать ранее загруженный файл</button><div class="backup-library-menu" id="streamBackupLibraryMenu"></div></div><div class="backup-file-row" id="streamBackupFileRow"><input id="streamBackupFilePicker" type="file" accept="video/*,.ts,.mts,.m2ts,.mp4,.mov,.m4v" onchange="uploadBackupReplacementFile('${stream.id}', this)" /><span id="streamBackupUploadStatus"></span></div></div></div>
        <div class="form-row full" id="streamBackupFileLoopRow"><label>Зациклить файл замены</label><div class="checkbox-inline"><input id="streamBackupFileLoop" type="checkbox" ${stream.backup_file_loop ? 'checked' : ''} /><span>Повторять до появления основного потока</span></div></div>
        <div class="form-row full"><label>Тестовая таблица</label><div class="checkbox-inline"><input id="streamTestPattern" type="checkbox" ${stream.test_pattern ? 'checked' : ''} /><span>Использовать вместо входных потоков</span></div></div>
        <div class="form-row full"><label>Интерфейс вывода</label><select class="compact" id="streamInterface" onchange="syncOutputHostWithInterface()"><option value="">Auto / все интерфейсы</option>${outputOptions}</select></div>
        <div class="form-row full"><label>Выходные форматы</label><div id="streamOutputs" class="output-list">${renderOutputRows(outputs, links)}</div><button class="button-secondary" type="button" onclick="addStreamOutput()">+ Добавить формат</button></div>
        <div class="form-row full"><label>V-PID / A-PID</label><div class="row-inline compact-row"><input class="compact" id="streamVideoPid" type="number" min="16" max="8190" value="${stream.video_pid||258}" placeholder="V-PID 258" /><input class="compact" id="streamAudioPid" type="number" min="16" max="8190" value="${stream.audio_pid||257}" placeholder="A-PID 257" /></div></div>
        <div class="form-row"><label>SID входа</label><input class="compact" id="streamInputServiceId" type="number" min="0" max="65535" value="${stream.input_service_id ?? 0}" placeholder="0 = Auto" /><small>0 = автоопределение SID из PAT; значение 1–65535 = выбрать конкретный входной канал.</small></div>
        <div class="form-row"><label>SID выхода</label><input class="compact" id="streamServiceId" type="number" min="1" max="65535" value="${stream.service_id||1}" placeholder="1" /></div>
        <div class="form-row full"><label>Имя Канала и Провайдер</label><div class="row-inline compact-row"><input class="compact" id="streamServiceName" value="${stream.service_name||''}" placeholder="Belarus 5" /><input class="compact" id="streamProvider" value="${stream.service_provider||''}" placeholder="BTRC" /></div></div>
        <div class="form-row full"><label>Target bitrate (кбит/с, для CBR)</label><input id="streamBitrate" type="number" value="${Math.round((stream.target_bitrate||2000000)/1000)}" placeholder="2000" /></div>
        <div class="form-row full"><label>Транскодирование</label><div class="checkbox-inline"><input id="streamTranscodeEnabled" type="checkbox" ${(stream.transcode_enabled && transcoderAvailable) ? 'checked' : ''} ${transcoderAvailable ? '' : 'disabled'} onchange="updateTranscodeControls()" /><span>Транскодировать видео в H.264 CBR, устранить черезстрочность и перекодировать звук</span></div><small style="color:${transcoderAvailable ? '#7ee2a8' : '#ff9f9f'}">${transcoderStatus}</small></div>
        <div class="form-row full" id="streamTranscodeControls" style="display:${(stream.transcode_enabled && transcoderAvailable)?'block':'none'}"><label>Параметры транскодирования</label><div class="row-inline compact-row"><select id="streamTranscodeResolution" onchange="applyRecommendedTranscodeBitrate()"><option value="3840x2160" ${stream.transcode_resolution==='3840x2160'?'selected':''}>3840×2160 (4K UHD)</option><option value="3200x1800" ${stream.transcode_resolution==='3200x1800'?'selected':''}>3200×1800 (3K)</option><option value="2560x1440" ${stream.transcode_resolution==='2560x1440'?'selected':''}>2560×1440 (2K QHD)</option><option value="1920x1080" ${(!stream.transcode_resolution||stream.transcode_resolution==='1920x1080')?'selected':''}>1920×1080 (Full HD)</option><option value="1280x720" ${stream.transcode_resolution==='1280x720'?'selected':''}>1280×720 (HD)</option><option value="720x576" ${stream.transcode_resolution==='720x576'?'selected':''}>720×576 (PAL SD)</option></select><input id="streamTranscodeBitrate" type="number" min="500" max="100000" step="100" value="${Math.round((stream.transcode_video_bitrate||6000000)/1000)}" placeholder="6000" /><span>кбит/с CBR</span></div><div class="row-inline compact-row" style="margin-top:8px"><select id="streamTranscodeAudioCodec" onchange="updateTranscodeAudioControls()"><option value="copy" ${stream.transcode_audio_codec==='copy'?'selected':''}>Проброс оригинальной дорожки</option><option value="aac" ${(stream.transcode_audio_codec||'aac')==='aac'?'selected':''} ${transcoderInfo.aac_encoder?'':'disabled'}>AAC-LC${transcoderInfo.aac_encoder?'':' (недоступен)'}</option><option value="mp3" ${stream.transcode_audio_codec==='mp3'?'selected':''} ${transcoderInfo.mp3_encoder?'':'disabled'}>MP3${transcoderInfo.mp3_encoder?'':' (недоступен)'}</option></select><select id="streamTranscodeAudioBitrate" ${stream.transcode_audio_codec==='copy'?'disabled':''}><option value="96000" ${(stream.transcode_audio_bitrate||192000)===96000?'selected':''}>96 кбит/с</option><option value="128000" ${(stream.transcode_audio_bitrate||192000)===128000?'selected':''}>128 кбит/с</option><option value="160000" ${(stream.transcode_audio_bitrate||192000)===160000?'selected':''}>160 кбит/с</option><option value="192000" ${(stream.transcode_audio_bitrate||192000)===192000?'selected':''}>192 кбит/с</option><option value="256000" ${(stream.transcode_audio_bitrate||192000)===256000?'selected':''}>256 кбит/с</option><option value="320000" ${(stream.transcode_audio_bitrate||192000)===320000?'selected':''}>320 кбит/с</option></select><span>аудио</span></div><small>Видео всегда преобразуется в прогрессивный режим 25p. По умолчанию: Full HD — 6000 кбит/с, звук AAC 192 кбит/с. В режиме проброса исходная аудиодорожка не перекодируется.</small></div>
        <div class="form-row full"><label>Автозапуск</label><div class="checkbox-inline"><input id="streamAutoStart" type="checkbox" ${stream.auto_start ? 'checked' : ''} /><span>Запускать после перезапуска программы</span></div></div>
        <div class="form-row full" id="streamCbrRow"><label>Включить CBR</label><div class="checkbox-inline"><input id="streamCbr" type="checkbox" ${stream.cbr ? 'checked' : ''} onchange="syncUdpCbrModeFromCheckbox()" /><span>CBR</span></div></div>
        <div class="form-row full"><label>Включить Remap</label><div class="checkbox-inline"><input id="streamRemapEnabled" type="checkbox" ${stream.remap_enabled ? 'checked' : ''} /><span>Remap PID / Service</span></div><small>Для MPEG-TS: SID входа 0 = автоопределение программы из PAT; ненулевой SID выбирает конкретный входной канал. SID выхода всегда задаётся отдельно и используется для Remap в PAT/PMT/SDT. V-PID и A-PID задают выходные PID.</small></div>
      </div>
      <div class="modal-actions">
        <button class="button-secondary" onclick="closeModal()">Отмена</button>
        <button class="button-primary" onclick="saveStream('${stream.id}')">Сохранить</button>
      </div>
    `);
    document.getElementById('modalContent').classList.add('stream-modal');
    updateHeaderHeight();
    document.getElementById('modal').classList.add('stream-open');
    document.getElementById('streamCbr').checked = outputType === 'udp-cbr' || (outputType !== 'udp-vbr' && stream.cbr);
    updateBackupInputMode();
    updateTranscodeControls();
    loadUploadedBackupFiles();
    updateOutputHints();
  };

  const formLoaders = [];
  if (!state.interfaces || !state.interfaces.length) formLoaders.push(loadInterfaces());
  if (String(stream.input_uri || '').startsWith('dvb://') && !camClientsLoaded) formLoaders.push(loadCamClients().then(()=>{ camClientsLoaded = true; }));
  if (formLoaders.length) Promise.all(formLoaders).then(renderStreamForm);
  else renderStreamForm();
}
function updateTranscodeControls() {
  const enabled = document.getElementById('streamTranscodeEnabled');
  const controls = document.getElementById('streamTranscodeControls');
  const available = state.transcoder?.available === true;
  if (enabled && !available) {
    enabled.checked = false;
    enabled.disabled = true;
  }
  if (controls) controls.style.display = available && enabled && enabled.checked ? 'block' : 'none';
  updateOutputHints();
}
function updateTranscodeAudioControls() {
  const codec = document.getElementById('streamTranscodeAudioCodec');
  const bitrate = document.getElementById('streamTranscodeAudioBitrate');
  if (codec && bitrate) bitrate.disabled = codec.value === 'copy';
}

function applyRecommendedTranscodeBitrate() {
  const resolution = document.getElementById('streamTranscodeResolution');
  const bitrate = document.getElementById('streamTranscodeBitrate');
  if (!resolution || !bitrate) return;
  const defaults = {'3840x2160':25000,'3200x1800':18000,'2560x1440':12000,'1920x1080':6000,'1280x720':3500,'720x576':2000};
  bitrate.value = defaults[resolution.value] || 6000;
}
function syncUdpCbrModeFromCheckbox() {
  const cbrInput = document.getElementById('streamCbr');
  const primaryTypeSelect = document.querySelector('.output-row [data-output-field="output_type"]');
  if (!cbrInput || !primaryTypeSelect) return;
  const type = primaryTypeSelect.value || 'udp-vbr';
  if (type === 'udp-cbr' || type === 'udp-vbr') {
    primaryTypeSelect.value = cbrInput.checked ? 'udp-cbr' : 'udp-vbr';
  }
  updateOutputHints();
}

function updateOutputHints() {
  const rows = [...document.querySelectorAll('.output-row')];
  const cbrRow = document.getElementById('streamCbrRow');
  const cbrInput = document.getElementById('streamCbr');
  rows.forEach(row => {
    const type = row.querySelector('[data-output-field="output_type"]')?.value || 'udp-cbr';
    const outputMode = row.querySelector('[data-output-field="output_mode"]')?.value || 'listener';
    const hostLabel = row.querySelector('[data-output-host-label]');
    const portLabel = row.querySelector('[data-output-port-label]');
    const host = row.querySelector('[data-output-field="output_host"]');
    const port = row.querySelector('[data-output-field="output_port"]');
    const modeRow = row.querySelector('[data-output-field="output_mode"]')?.closest('.form-row');
    if (!hostLabel || !portLabel || !host || !port) return;
    if (modeRow) modeRow.style.display = type === 'srt' ? '' : 'none';
    if (type === 'http' || type === 'hls') {
      hostLabel.textContent = 'Адрес для ссылки';
      portLabel.textContent = type === 'hls' ? 'HLS порт' : 'HTTP порт';
      port.disabled = false;
      port.placeholder = String(state.http_port || 9000);
      host.placeholder = 'IP интерфейса или DNS';
    } else if (type === 'rtp') {
      hostLabel.textContent = 'RTP IP / мультикаст';
      portLabel.textContent = 'RTP порт';
      port.disabled = false;
      host.placeholder = '239.0.0.1';
    } else if (type === 'srt') {
      hostLabel.textContent = outputMode === 'caller' ? 'SRT сервер' : 'SRT host для ссылки';
      portLabel.textContent = 'SRT порт';
      port.disabled = false;
      host.placeholder = outputMode === 'caller' ? 'server.example.com или IP' : '0.0.0.0 для listener';
      if (outputMode === 'listener' && (!host.value || host.value === '127.0.0.1' || host.value === '239.0.0.1')) {
        host.value = '0.0.0.0';
      } else if (outputMode === 'caller' && (!host.value || host.value === '0.0.0.0' || host.value === '239.0.0.1')) {
        host.value = '127.0.0.1';
      }
    } else if (type === 'rtsp') {
      hostLabel.textContent = 'RTSP сервер';
      portLabel.textContent = 'RTSP порт';
      port.disabled = false;
      host.placeholder = 'rtsp://server/app/name или IP сервера';
      if (!host.value || host.value === '0.0.0.0' || host.value === '239.0.0.1') host.value = '127.0.0.1';
    } else if (type === 'youtube') {
      hostLabel.textContent = 'YouTube key / URL';
      portLabel.textContent = 'Порт';
      port.disabled = true;
      host.placeholder = 'xxxx-xxxx-xxxx-xxxx или rtmp://a.rtmp.youtube.com/live2/...';
    } else if (type === 'rtmp') {
      hostLabel.textContent = 'RTMP URL / host';
      portLabel.textContent = 'RTMP порт';
      port.disabled = false;
      host.placeholder = 'rtmp://server/app/key или server.example.com';
    } else {
      hostLabel.textContent = 'Мультикаст / UDP IP';
      portLabel.textContent = 'UDP порт';
      port.disabled = false;
      host.placeholder = '239.0.0.1';
    }
  });
  if (cbrInput && cbrRow) {
    const primaryType = rows[0]?.querySelector('[data-output-field="output_type"]')?.value || 'udp-cbr';
    const udpMode = primaryType === 'udp-cbr' || primaryType === 'udp-vbr';
    if (udpMode) cbrInput.checked = primaryType === 'udp-cbr';
    cbrInput.disabled = false;
    cbrRow.style.display = '';
    const bitrateInput = document.getElementById('streamBitrate');
    if (bitrateInput) bitrateInput.disabled = udpMode && !cbrInput.checked;
  }
  syncOutputHostWithInterface();
}
function syncOutputHostWithInterface() {
  const iface = document.getElementById('streamInterface')?.value || '';
  if (!iface) return;
  document.querySelectorAll('.output-row').forEach(row => {
    const type = row.querySelector('[data-output-field="output_type"]')?.value || 'udp-cbr';
    const outputMode = row.querySelector('[data-output-field="output_mode"]')?.value || 'listener';
    const host = row.querySelector('[data-output-field="output_host"]');
    if (!host || (type !== 'http' && type !== 'hls' && !(type === 'srt' && outputMode === 'listener'))) return;
    if (!host.value || host.value === '0.0.0.0' || host.value === '127.0.0.1') {
      host.value = iface;
    }
  });
}
function saveSettings() {
  const httpPortInput = document.getElementById('httpPort');
  const httpPort = httpPortInput ? Number(httpPortInput.value || 9000) : state.http_port;
  const previousHttpPort = Number(state.http_port || window.location.port || 9000);
  const payload = {
    login: document.getElementById('login')?.value || state.login,
    server_name: document.getElementById('serverName')?.value || state.server_name,
    telegram_token: document.getElementById('telegramToken')?.value || state.telegram_token,
    telegram_chat_id: document.getElementById('telegramChatId')?.value || state.telegram_chat_id,
    http_port: httpPort,
    language,
    streams: state.streams
  };
  const password = document.getElementById('password')?.value;
  if (password) payload.password = password;
  fetch('/api/save-config', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(()=>{
      if (httpPortInput && httpPort && httpPort !== previousHttpPort) {
        const nextUrl = new URL(window.location.href);
        nextUrl.port = String(httpPort);
        setTimeout(() => { window.location.href = nextUrl.toString(); }, 400);
        return;
      }
      closeModal();
      fetchState();
    });
}
function saveStream(id) {
  const outputs = collectOutputRows();
  const primaryOutput = outputs[0] || {output_type:'udp-cbr', output_mode:'listener', output_host:'127.0.0.1', output_port:1234};
  const cbrCheckbox = document.getElementById('streamCbr');
  let selectedOutputType = primaryOutput.output_type;
  const primaryUdp = selectedOutputType === 'udp-cbr' || selectedOutputType === 'udp-vbr';
  if (primaryUdp && cbrCheckbox) {
    selectedOutputType = cbrCheckbox.checked ? 'udp-cbr' : 'udp-vbr';
    primaryOutput.output_type = selectedOutputType;
    if (outputs.length) outputs[0].output_type = selectedOutputType;
  }
  const selectedCbr = selectedOutputType === 'udp-cbr'
    ? true
    : (selectedOutputType === 'udp-vbr' ? false : Boolean(cbrCheckbox?.checked));
  const payload = {
    id: id,
    name: document.getElementById('streamName').value,
    input_uri: document.getElementById('streamInput').value,
    output_type: selectedOutputType,
    output_mode: primaryOutput.output_mode,
    output_host: primaryOutput.output_host,
    output_port: primaryOutput.output_port,
    additional_outputs: outputs.slice(1),
    backup_input_uri: document.getElementById('streamBackupInput').value,
    backup_input_type: document.getElementById('streamBackupInputType').value,
    backup_file_loop: document.getElementById('streamBackupInputType').value === 'file' && document.getElementById('streamBackupFileLoop').checked,
    interface_address: document.getElementById('streamInterface').value,
    input_interface_address: document.getElementById('streamInputInterface').value,
    input_mode: document.getElementById('streamInputMode').value,
    test_pattern: document.getElementById('streamTestPattern').checked,
    auto_start: document.getElementById('streamAutoStart').checked,
    remap_enabled: document.getElementById('streamRemapEnabled').checked,
    cbr: selectedCbr,
    target_bitrate: Number(document.getElementById('streamBitrate').value) * 1000,
    transcode_enabled: state.transcoder?.available === true && document.getElementById('streamTranscodeEnabled').checked,
    transcode_resolution: document.getElementById('streamTranscodeResolution').value,
    transcode_video_bitrate: Number(document.getElementById('streamTranscodeBitrate').value) * 1000,
    transcode_audio_codec: document.getElementById('streamTranscodeAudioCodec').value,
    transcode_audio_bitrate: Number(document.getElementById('streamTranscodeAudioBitrate').value),
    audio_pid: Number(document.getElementById('streamAudioPid').value),
    video_pid: Number(document.getElementById('streamVideoPid').value),
    input_service_id: Number(document.getElementById('streamInputServiceId').value),
    service_id: Number(document.getElementById('streamServiceId').value),
    service_name: document.getElementById('streamServiceName').value,
    service_provider: document.getElementById('streamProvider').value,
    conditional_access_client: document.getElementById('streamConditionalAccessClient')?.value || ''
  };
  const existingIndex = state.streams.findIndex(s=>s.id===id);
  if (existingIndex >= 0) {
    state.streams[existingIndex] = payload;
  } else {
    state.streams.push(payload);
  }
  const savePayload = {
    login: state.login,
    server_name: state.server_name,
    telegram_token: state.telegram_token,
    telegram_chat_id: state.telegram_chat_id,
    http_port: state.http_port,
    language,
    streams: state.streams
  };
  fetch('/api/save-config', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(savePayload)})
    .then(()=>{closeModal();fetchState();});
}
function setCopyButtonState(button, className) {
  if (!button) return;
  button.classList.remove('copied', 'copy-error');
  button.classList.add(className);
  clearTimeout(button.copyStateTimer);
  button.copyStateTimer = setTimeout(() => {
    button.classList.remove('copied', 'copy-error');
  }, 1400);
}
function fallbackCopyText(text) {
  const input = document.createElement('textarea');
  input.value = text;
  input.setAttribute('readonly', '');
  input.style.position = 'fixed';
  input.style.left = '-9999px';
  input.style.top = '0';
  document.body.appendChild(input);
  input.focus();
  input.select();
  let ok = false;
  try {
    ok = document.execCommand('copy');
  } finally {
    document.body.removeChild(input);
  }
  return ok;
}
function copyLink(text, button) {
  const onSuccess = () => setCopyButtonState(button, 'copied');
  const onError = () => {
    if (fallbackCopyText(text)) {
      onSuccess();
    } else {
      setCopyButtonState(button, 'copy-error');
    }
  };

  if (navigator.clipboard && window.isSecureContext) {
    navigator.clipboard.writeText(text).then(onSuccess).catch(onError);
  } else {
    onError();
  }
}
function copyStreamLinks(id, button) {
  const stream = (state.streams || []).find(item => item.id === id);
  if (!stream) return;
  const text = streamLinks(stream).map(link => link.url).join('\n') || stream.vlc_link || '';
  copyLink(text, button);
}
const qualityPeriods = [
  {label:'Месяц', seconds:2592000},
  {label:'Неделя', seconds:604800},
  {label:'День', seconds:86400},
  {label:'Пол дня', seconds:43200},
  {label:'5 часов', seconds:18000},
  {label:'1 час', seconds:3600},
  {label:'30 минут', seconds:1800},
  {label:'10 минут', seconds:600},
  {label:'Минута', seconds:60}
];
const qualityRefreshOptions = [
  {label:'Выкл', ms:0},
  {label:'2 сек', ms:2000},
  {label:'5 сек', ms:5000},
  {label:'10 сек', ms:10000},
  {label:'30 сек', ms:30000}
];
function storedQualityRefreshMs() {
  const stored = localStorage.getItem('tvstreammersat5-quality-refresh-ms');
  const value = stored === null ? 2000 : Number(stored);
  return qualityRefreshOptions.some(option => option.ms === value) ? value : 2000;
}
let qualityChart = {streamId:'', period:3600, samples:[], points:[], refreshMs:storedQualityRefreshMs(), timer:null};
function stopQualityAutoRefresh() {
  clearInterval(qualityChart.timer);
  qualityChart.timer = null;
}
function restartQualityAutoRefresh() {
  stopQualityAutoRefresh();
  if (!qualityChart.streamId || !qualityChart.refreshMs) return;
  qualityChart.timer = setInterval(() => {
    loadQualityHistory(qualityChart.streamId, qualityChart.period);
  }, qualityChart.refreshMs);
}
function setQualityAutoRefresh(ms) {
  qualityChart.refreshMs = qualityRefreshOptions.some(option => option.ms === ms) ? ms : 0;
  localStorage.setItem('tvstreammersat5-quality-refresh-ms', String(qualityChart.refreshMs));
  restartQualityAutoRefresh();
}
function qualityColor(level) {
  return {ok:'#17c261', warn:'#ffbd4a', error:'#ff5f5f', offline:'#7c879b'}[level] || '#9aa3b1';
}
function formatTime(ts, period) {
  const date = new Date(ts * 1000);
  if (period >= 86400) {
    return date.toLocaleDateString([], {day:'2-digit', month:'2-digit'}) + ' ' +
      date.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'});
  }
  return date.toLocaleTimeString([], {hour:'2-digit', minute:'2-digit', second: period <= 600 ? '2-digit' : undefined});
}
function openQualityModal(id, periodSeconds=3600) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream) return;
  const outputs = outputConfigsForStream(stream);
  stopQualityAutoRefresh();
  qualityChart.streamId = id;
  qualityChart.period = periodSeconds;
  document.getElementById('modalContent').className = 'modal-content quality-modal';
  const tabs = qualityPeriods.map(p=>`<button class="${p.seconds===periodSeconds?'active':''}" onclick="loadQualityHistory('${id}', ${p.seconds})">${p.label}</button>`).join('');
  const refreshOptions = qualityRefreshOptions.map(option => `<option value="${option.ms}" ${option.ms===qualityChart.refreshMs?'selected':''}>${option.label}</option>`).join('');
  document.getElementById('modalContent').innerHTML = modalCloseButton() + `
    <div class="quality-head">
      <div class="quality-title">
        <h2>Качество потока</h2>
        <small>${stream.name || stream.id} · ${outputs.map(output => `${normalizedOutputType(output).toUpperCase()} ${output.output_host}:${output.output_port}`).join(' · ')}</small>
      </div>
      <div class="quality-toolbar">
        <div class="period-tabs">${tabs}</div>
        <label class="quality-refresh"><span>Автообновление</span><select onchange="setQualityAutoRefresh(Number(this.value))">${refreshOptions}</select></label>
      </div>
    </div>
    <div class="quality-board">
      <canvas id="qualityCanvas" width="1160" height="320"></canvas>
    </div>
    <div class="quality-decode">
      <strong>Расшифровка</strong>
      <span><i class="quality-line quality-input"></i> Зеленый — входной bitrate по левой шкале Mbit/s.</span>
      <span><i class="quality-line quality-output"></i> Синий — выходной bitrate по левой шкале Mbit/s.</span>
      <span><i class="quality-line quality-input-cc"></i> Оранжевый — CC-errors входного MPEG-TS по правой шкале.</span>
      <span><i class="quality-line quality-output-cc"></i> Розовый — CC-errors выходного MPEG-TS по правой шкале.</span>
      <span>Всплески CC-errors обычно означают потерю, дублирование или перестановку TS-пакетов.</span>
      <span>Клик по графику копирует картинку графика.</span>
    </div>
    <div id="qualityCopyNotice" class="quality-copy"></div>
    <div id="qualityDetails" class="quality-details"></div>
    <div id="qualityErrors" class="quality-errors"></div>
    <div class="modal-actions">
      <button class="button-secondary" onclick="closeModal()">Закрыть</button>
    </div>
  `;
  updateHeaderHeight();
  document.getElementById('modal').classList.add('active', 'quality-open');
  loadQualityHistory(id, periodSeconds);
  restartQualityAutoRefresh();
}
function loadQualityHistory(id, periodSeconds) {
  qualityChart.period = periodSeconds;
  fetch(`/api/quality-history?id=${encodeURIComponent(id)}&period=${periodSeconds}`)
    .then(r=>r.json())
    .then(data=>{
      qualityChart.samples = data.samples || [];
      renderQualityTabs(periodSeconds);
      drawQualityChart(data);
    });
}
function renderQualityTabs(periodSeconds) {
  document.querySelectorAll('.period-tabs button').forEach((button, index) => {
    button.classList.toggle('active', qualityPeriods[index]?.seconds === periodSeconds);
  });
}
function drawQualityChart(data) {
  const canvas = document.getElementById('qualityCanvas');
  const details = document.getElementById('qualityDetails');
  const errors = document.getElementById('qualityErrors');
  if (!canvas || !details || !errors) return;
  const setupCanvas = (target, height) => {
    const targetRect = target.getBoundingClientRect();
    const targetRatio = window.devicePixelRatio || 1;
    target.width = Math.max(640, Math.floor(targetRect.width * targetRatio));
    target.height = Math.floor(height * targetRatio);
    const targetContext = target.getContext('2d');
    targetContext.setTransform(targetRatio, 0, 0, targetRatio, 0, 0);
    return {ctx: targetContext, width: target.width / targetRatio, height};
  };
  const chart = setupCanvas(canvas, 320);
  const ctx = chart.ctx;
  const width = chart.width;
  const height = chart.height;
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = '#0f1622';
  ctx.fillRect(0, 0, width, height);

  const samples = data.samples || [];
  const stream = (state.streams || []).find(item => item.id === qualityChart.streamId) || {};
  const streamName = stream.name || data.id || 'Поток';
  const streamLinkText = (stream.vlc_links && stream.vlc_links[0]?.url) || stream.vlc_link || stream.input_uri || '';
  const title = `${state.server_name || 'TVStreammerSAT5'}: Поток: ${streamName}${streamLinkText ? ` (${streamLinkText})` : ''}`;
  const edgeTime = ts => {
    const date = new Date(ts * 1000);
    const day = String(date.getDate()).padStart(2, '0');
    const month = String(date.getMonth() + 1).padStart(2, '0');
    const hours = String(date.getHours()).padStart(2, '0');
    const minutes = String(date.getMinutes()).padStart(2, '0');
    return `${day}-${month} ${hours}:${minutes}`;
  };
  const formatMbitValue = kbps => {
    if (!kbps) return '0 bit/s';
    const mbps = kbps / 1000;
    const digits = mbps >= 10 ? 2 : 3;
    return `${Number(mbps.toFixed(digits))} Mbit/s`;
  };
  const formatMetricNumber = value => Number(value || 0).toLocaleString('ru-RU', {maximumFractionDigits: 3});
  const statsFor = values => {
    const normalized = values.map(value => Number(value || 0));
    if (!normalized.length) return {last:0, min:0, avg:0, max:0};
    const sum = normalized.reduce((total, value) => total + value, 0);
    return {
      last: normalized[normalized.length - 1],
      min: Math.min(...normalized),
      avg: sum / normalized.length,
      max: Math.max(...normalized)
    };
  };
  const statsRow = (dotClass, name, unit, stats, formatter) => `
    <div class="metric-name"><i class="quality-line ${dotClass}"></i>${name}</div>
    <div class="unit">${unit}</div>
    <div class="value">${formatter(stats.last)}</div>
    <div class="value">${formatter(stats.min)}</div>
    <div class="value">${formatter(stats.avg)}</div>
    <div class="value">${formatter(stats.max)}</div>
  `;

  if (!samples.length) {
    ctx.fillStyle = '#cfd8ea';
    ctx.font = '700 13px Arial';
    ctx.textAlign = 'center';
    ctx.fillText('История пока пустая. Данные появятся после нескольких обновлений состояния.', width / 2, height / 2);
    details.innerHTML = '<div class="quality-card"><strong>Нет данных</strong>История собирается в памяти во время работы приложения.</div>';
    errors.innerHTML = '';
    qualityChart.points = [];
    canvas.onclick = () => copyQualityChartImage(canvas);
    return;
  }

  const left = 74, right = 64, top = 36, bottom = 70;
  const plotW = width - left - right;
  const plotH = height - top - bottom;
  const plotRight = left + plotW;
  const plotBottom = top + plotH;
  const endTs = data.generated_at || Math.floor(Date.now()/1000);
  const startTs = endTs - (data.period_seconds || qualityChart.period);
  const inputBitrateValues = samples.map(s => s.input_kbps || 0);
  const outputBitrateValues = samples.map(s => s.output_kbps || 0);
  const inputCcValues = samples.map(s => s.input_cc_errors ?? s.cc_errors ?? 0);
  const outputCcValues = samples.map(s => s.output_cc_errors || 0);
  const bitrateValues = [...inputBitrateValues, ...outputBitrateValues];
  const ccValues = [...inputCcValues, ...outputCcValues];
  const maxBitrateMbit = Math.max(1, ...bitrateValues.map(value => value / 1000));
  const bitrateStep = maxBitrateMbit <= 20 ? 2 : (maxBitrateMbit <= 50 ? 5 : 10);
  const leftMaxMbit = Math.max(bitrateStep, Math.ceil(maxBitrateMbit * 1.12 / bitrateStep) * bitrateStep);
  const maxCcErrors = Math.max(1, ...ccValues);
  const niceAxis = (maxValue, targetTicks) => {
    const rawMax = Math.max(1, Number(maxValue || 0));
    const roughStep = rawMax / Math.max(1, targetTicks);
    const exponent = Math.floor(Math.log10(roughStep));
    const base = Math.pow(10, exponent);
    const fraction = roughStep / base;
    const niceFraction = fraction <= 1 ? 1 : (fraction <= 2 ? 2 : (fraction <= 5 ? 5 : 10));
    const step = niceFraction * base;
    return {max: Math.ceil(rawMax / step) * step, step};
  };
  const rightTickCount = Math.max(4, Math.min(8, Math.floor(plotH / 38)));
  const rightAxis = niceAxis(Math.max(5, maxCcErrors * 1.25), rightTickCount);
  const rightMax = Math.max(5, rightAxis.max);
  const rightStep = Math.max(1, rightAxis.step);

  ctx.fillStyle = '#cfd8ea';
  ctx.font = '700 13px Arial';
  ctx.textAlign = 'center';
  let titleText = title;
  while (ctx.measureText(titleText).width > plotW && titleText.length > 24) {
    titleText = titleText.slice(0, -4) + '...';
  }
  ctx.fillText(titleText, width / 2, 18);

  ctx.lineWidth = 1;
  ctx.strokeStyle = 'rgba(255,255,255,.24)';
  ctx.beginPath();
  ctx.moveTo(left, top - 8);
  ctx.lineTo(left, plotBottom);
  ctx.lineTo(plotRight, plotBottom);
  ctx.moveTo(plotRight, top - 8);
  ctx.lineTo(plotRight, plotBottom);
  ctx.stroke();

  ctx.strokeStyle = 'rgba(255,255,255,.13)';
  ctx.setLineDash([2, 2]);
  ctx.fillStyle = '#c8d0dc';
  ctx.font = '11px Arial';
  ctx.textAlign = 'right';
  for (let value=0; value<=leftMaxMbit; value+=bitrateStep) {
    const y = plotBottom - (value / leftMaxMbit) * plotH;
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(plotRight, y);
    ctx.stroke();
    ctx.fillText(value === 0 ? '0 bit/s' : `${value} Mbit/s`, left - 8, y + 4);
  }
  ctx.textAlign = 'right';
  for (let value=0; value<=rightMax + rightStep * .5; value+=rightStep) {
    const y = plotBottom - (value / rightMax) * plotH;
    ctx.fillText(String(Math.round(value)), width - 8, y + 4);
  }

  ctx.textAlign = 'center';
  const tickCount = Math.max(4, Math.min(20, Math.floor(plotW / 64)));
  for (let i=0;i<=tickCount;i++) {
    const correctedX = left + plotW * i / tickCount;
    const ts = startTs + (endTs - startTs) * i / tickCount;
    ctx.beginPath();
    ctx.moveTo(correctedX, top);
    ctx.lineTo(correctedX, plotBottom);
    ctx.stroke();
    const edge = i === 0 || i === tickCount;
    ctx.save();
    ctx.translate(correctedX, height - 10);
    ctx.rotate(-Math.PI / 2);
    ctx.fillStyle = edge ? '#ff7f7f' : '#c8d0dc';
    ctx.textAlign = 'left';
    ctx.fillText(edge ? edgeTime(ts) : formatTime(ts, data.period_seconds), 0, 4);
    ctx.restore();
  }
  ctx.setLineDash([]);

  const xFor = ts => left + ((ts - startTs) / Math.max(1, endTs - startTs)) * plotW;
  const bitrateYFor = kbps => plotBottom - Math.min(kbps / 1000, leftMaxMbit) / leftMaxMbit * plotH;
  const ccYFor = value => plotBottom - Math.min(value, rightMax) / rightMax * plotH;
  const drawSeries = (values, color, yFor, widthPx=2) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = widthPx;
    ctx.beginPath();
    let started = false;
    samples.forEach((s, index) => {
      const value = values[index] || 0;
      const x = xFor(s.ts);
      const y = yFor(value);
      if (!started) { ctx.moveTo(x, y); started = true; } else { ctx.lineTo(x, y); }
    });
    ctx.stroke();
  };

  const drawCcBars = (values, color, offset) => {
    ctx.fillStyle = color;
    samples.forEach((s, index) => {
      const value = values[index] || 0;
      if (!value) return;
      const x = xFor(s.ts) + offset;
      const y = ccYFor(value);
      ctx.fillRect(x - 1.5, y, 3, plotBottom - y);
    });
  };
  drawCcBars(inputCcValues, 'rgba(255,159,26,.30)', -2);
  drawCcBars(outputCcValues, 'rgba(255,79,154,.30)', 2);
  drawSeries(inputCcValues, '#ff9f1a', ccYFor, 1.8);
  drawSeries(outputCcValues, '#ff4f9a', ccYFor, 1.8);
  drawSeries(inputBitrateValues, '#26ef46', bitrateYFor, 2.5);
  drawSeries(outputBitrateValues, '#36a3ff', bitrateYFor, 2.5);

  qualityChart.points = [];
  samples.forEach(s => {
    const x = xFor(s.ts);
    const inputY = bitrateYFor(s.input_kbps || 0);
    const outputY = bitrateYFor(s.output_kbps || 0);
    ctx.fillStyle = '#26ef46';
    ctx.beginPath();
    ctx.arc(x, inputY, 2.5, 0, Math.PI * 2);
    ctx.fill();
    ctx.fillStyle = '#36a3ff';
    ctx.beginPath();
    ctx.arc(x, outputY, 2.5, 0, Math.PI * 2);
    ctx.fill();
    qualityChart.points.push({x, y:(inputY + outputY) / 2, sample:s});
  });

  const inputBitrateStats = statsFor(inputBitrateValues);
  const outputBitrateStats = statsFor(outputBitrateValues);
  const inputCcStats = statsFor(inputCcValues);
  const outputCcStats = statsFor(outputCcValues);
  details.innerHTML = `
    <div class="quality-stats">
      <div></div><div></div><div class="head">посл</div><div class="head">мин</div><div class="head">сред</div><div class="head">макс</div>
      ${statsRow('quality-input', `${streamName} — входной bitrate`, '[input]', inputBitrateStats, formatMbitValue)}
      ${statsRow('quality-output', `${streamName} — выходной bitrate`, '[output]', outputBitrateStats, formatMbitValue)}
      ${statsRow('quality-input-cc', `${streamName} — входные CC-errors`, '[input]', inputCcStats, value => formatMetricNumber(value))}
      ${statsRow('quality-output-cc', `${streamName} — выходные CC-errors`, '[output]', outputCcStats, value => formatMetricNumber(value))}
    </div>
  `;
  const bad = samples.filter(s => s.level !== 'ok' || (s.input_cc_errors ?? s.cc_errors ?? 0) > 0 || (s.output_cc_errors || 0) > 0).slice(-30).reverse();
  errors.innerHTML = bad.length
    ? bad.map(s => {
        const inputCc = s.input_cc_errors ?? s.cc_errors ?? 0;
        const outputCc = s.output_cc_errors || 0;
        const markerColor = outputCc > 0 ? '#ff4f9a' : (inputCc > 0 ? '#ff9f1a' : qualityColor(s.level));
        return `<div><span style="color:${markerColor}">●</span><span>${formatTime(s.ts, data.period_seconds)}</span><span>${s.message} · CC input: ${inputCc} · CC output: ${outputCc}</span></div>`;
      }).join('')
    : '<div><span style="color:#17c261">●</span><span>За выбранный период входных и выходных CC-errors и других ошибок нет</span></div>';
  canvas.onclick = () => copyQualityChartImage(canvas);
}
function copyQualityChartImage(canvas) {
  const notice = document.getElementById('qualityCopyNotice');
  const show = message => {
    if (!notice) return;
    notice.textContent = message;
    clearTimeout(notice.copyTimer);
    notice.copyTimer = setTimeout(()=>{ notice.textContent = ''; }, 1800);
  };
  if (!canvas || !navigator.clipboard || !window.ClipboardItem || !window.isSecureContext) {
    show('Браузер не разрешил копировать картинку графика');
    return;
  }
  let item;
  try {
    const png = new Promise((resolve, reject) => {
      canvas.toBlob(blob => blob ? resolve(blob) : reject(new Error('empty canvas image')), 'image/png');
    });
    item = new ClipboardItem({'image/png': png});
  } catch (error) {
    show('Браузер не поддерживает копирование картинки графика');
    return;
  }
  navigator.clipboard.write([item])
    .then(() => show('Картинка графика скопирована в буфер обмена'))
    .catch(() => show('Не удалось скопировать картинку графика'));
}
function loadInterfaces() {
  return fetch('/api/interfaces')
    .then(r=>r.json())
    .then(data=>{ state.interfaces=data; return data; })
    .catch(() => { state.interfaces=[]; return []; });
}
window.onload = () => {
  applyLanguage();
  loadInterfaces();
  statePollLoop();
  metricsPollLoop();
};
window.addEventListener('beforeunload', () => {
  clearTimeout(statePollTimer);
  clearTimeout(metricsPollTimer);
});
</script>
</body>
</html>
)HTML";
    return html;
}
