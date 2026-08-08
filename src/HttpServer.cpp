#include "HttpServer.h"

#include "DvbManager.h"

#include "utils.h"
#include "TranscoderModule.h"
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
#include <cstdio>
#include <filesystem>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>
#include <vector>
#include <unistd.h>

namespace {

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
    if (type != "udp-vbr" && type != "udp-cbr" &&
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

std::string primaryInputDisplay(const StreamConfig& cfg) {
    if (!cfg.satelliteEnabled) {
        return cfg.inputUri;
    }
    std::ostringstream ss;
    ss << (cfg.satelliteDeliverySystem == "dvb-s" ? "DVB-S" : "DVB-S2")
       << " adapter=" << cfg.satelliteAdapter
       << " frontend=" << cfg.satelliteFrontend
       << " " << (static_cast<double>(cfg.satelliteFrequency) / 1000.0) << "MHz"
       << " SR=" << cfg.satelliteSymbolRate << "kBd"
       << " " << cfg.satellitePolarization;
    if (cfg.satelliteServiceId > 0) {
        ss << " SID=" << cfg.satelliteServiceId;
    }
    return ss.str();
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
        args.push_back("TVStreamer");
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
        res.set(http::field::server, "TVStreamer5");
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
            } else if (target == "/api/interfaces") {
                res.set(http::field::content_type, "application/json");
                res.body() = listInterfaces();
            } else if (target == "/api/dvb-devices") {
                res.set(http::field::content_type, "application/json");
                res.body() = listDvbDevices();
            } else if (target == "/api/serial-readers") {
                res.set(http::field::content_type, "application/json");
                res.body() = listSerialReaders();
            } else if (target == "/api/system-metrics") {
              res.set(http::field::content_type, "application/json");
              res.body() = systemMetrics();
            } else if (target == "/api/backup-files") {
                res.set(http::field::content_type, "application/json");
                res.body() = listBackupFiles();
            } else if (target == "/api/state") {
                res.set(http::field::content_type, "application/json");
                res.body() = currentState();
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
            if (target == "/api/save-config") {
                handleSaveConfig(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
            } else if (target == "/api/start-stream") {
                res.set(http::field::content_type, "application/json");
                res.body() = handleStartStream(req.body());
            } else if (target == "/api/stop-stream") {
                handleStopStream(req.body());
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"result\": \"ok\"}";
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
            } else if (target == "/api/scan-satellite") {
              res.set(http::field::content_type, "application/json");
              res.body() = handleScanSatellite(req.body());
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
        std::cerr << "HTTP session failed: " << ex.what() << std::endl;
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
           verifyMd5Password(password, configManager.config.password);
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
    res.set(http::field::www_authenticate, "Basic realm=\"TVStreamer5\"");
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

std::string HttpServer::listDvbDevices() {
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, tvs::dvb::enumerateDevices());
}

std::string HttpServer::listSerialReaders() {
    Json::Value root(Json::arrayValue);
    const std::filesystem::path byIdDirectory("/dev/serial/by-id");
    std::error_code error;

    if (!std::filesystem::exists(byIdDirectory, error) || error) {
        Json::StreamWriterBuilder writer;
        return Json::writeString(writer, root);
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
        if (canonicalError) {
            continue;
        }
        const std::string device = resolved.string();
        if (!safeDeviceNode(device)) {
            continue;
        }

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
        root.append(item);
    }

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, root);
}

std::string HttpServer::handleScanSatellite(const std::string& body) {
    Json::Value response;
    Json::CharReaderBuilder readerBuilder;
    Json::Value request;
    std::string errors;
    std::istringstream input(body);
    if (!Json::parseFromStream(readerBuilder, input, &request, &errors)) {
        response["result"] = "error";
        response["error"] = "invalid satellite scan request: " + errors;
    } else {
        StreamConfig scanConfig = StreamConfig::fromJson(request);
        scanConfig.satelliteEnabled = true;

        // Tuning a frontend for a scan would retune a running channel that uses
        // the same hardware. Refuse it instead of interrupting an active stream.
        const auto snapshot = streamManager.snapshot();
        bool busy = false;
        std::string busyStream;
        for (const auto& [id, state] : snapshot) {
            if (!state || !state->active.load() || !state->config.satelliteEnabled) continue;
            if (state->config.satelliteAdapter == scanConfig.satelliteAdapter &&
                state->config.satelliteFrontend == scanConfig.satelliteFrontend) {
                busy = true;
                busyStream = id;
                break;
            }
        }
        if (busy) {
            response["result"] = "error";
            response["error"] = "DVB frontend is in use by active stream: " + busyStream;
            response["busy_stream_id"] = busyStream;
        } else {
            response = tvs::dvb::scanTransponder(scanConfig, 7000);
        }
    }
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
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
    root["stream_count"] = Json::UInt(configManager.config.streams.size());
    root["active_count"] = Json::UInt(streamManager.activeStreams().size());
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
    auto snap = streamManager.snapshot();
    Json::Value caProviders(Json::arrayValue);
    for (const auto& provider : configManager.config.caProviders) {
      Json::Value item = provider.toJson();
      unsigned assignedChannels = 0;
      unsigned activeChannels = 0;
      for (const auto& stream : configManager.config.streams) {
        if (stream.caProviderId != provider.id) continue;
        ++assignedChannels;
        auto found = snap.find(stream.id);
        if (found != snap.end() && found->second && found->second->active.load() && !found->second->usingBackup) {
          ++activeChannels;
        }
      }
      item["assigned_channels"] = assignedChannels;
      item["active_channels"] = activeChannels;
      item["available_channels"] = provider.maxChannels > static_cast<int>(activeChannels)
        ? provider.maxChannels - static_cast<int>(activeChannels) : 0;
      item["capacity_ok"] = activeChannels <= static_cast<unsigned>(provider.maxChannels);
      const std::string backendType = toLower(provider.backendType);
      const bool authorizedTs = backendType == "authorized-ts" || backendType == "predecoded-ts" || backendType == "decrypted-ts";
      item["backend_connected"] = authorizedTs && activeChannels > 0;
      item["backend_status"] = authorizedTs
        ? (provider.endpoint.empty()
            ? "authorized TS transport: endpoint is not configured"
            : (activeChannels > 0 ? "authorized TS transport active" : "authorized TS transport configured"))
        : "external integration point; TVStreamer does not handle CA keys";
      caProviders.append(item);
    }
    root["ca_providers"] = caProviders;
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
    for (const auto& cfg : configManager.config.streams) {
        Json::Value item = cfg.toJson();
        if (snap.count(cfg.id)) {
            auto* streamState = snap.at(cfg.id);
            item["active"] = streamState->active.load();
            item["status"] = streamState->statusMessage;
            item["using_backup"] = streamState->usingBackup;
            item["active_input_uri"] = cfg.testPattern
                ? "test://bars"
                : (streamState->activeInputUri.empty() ? primaryInputDisplay(cfg) : streamState->activeInputUri);
            item["active_input_label"] = cfg.testPattern
                ? "Тест"
                : (streamState->usingBackup
                    ? (toLower(cfg.backupInputType) == "file" ? "Файл замены" : "Резерв")
                    : (cfg.satelliteEnabled ? "Спутник" : "Основной"));
            item["bitrate_in_kbps"] = Json::UInt64(streamState->inputBitrate.load() / 1000);
            item["bitrate_out_kbps"] = Json::UInt64(streamState->outputBitrate.load() / 1000);
            item["input_cc_errors"] = Json::UInt64(streamState->inputCcErrorsDelta.load());
            item["output_cc_errors"] = Json::UInt64(streamState->outputCcErrorsDelta.load());
            item["input_cc_errors_total"] = Json::UInt64(streamState->inputCcErrors.load());
            item["output_cc_errors_total"] = Json::UInt64(streamState->outputCcErrors.load());
            item["cc_errors"] = item["input_cc_errors"];
            item["cc_errors_total"] = item["input_cc_errors_total"];
        } else {
            item["active"] = false;
            item["status"] = "stopped";
            item["using_backup"] = false;
            item["active_input_uri"] = cfg.testPattern ? "test://bars" : primaryInputDisplay(cfg);
            item["active_input_label"] = cfg.testPattern ? "Тест" : (cfg.satelliteEnabled ? "Спутник" : "Основной");
            item["bitrate_in_kbps"] = Json::UInt64(0);
            item["bitrate_out_kbps"] = Json::UInt64(0);
            item["input_cc_errors"] = Json::UInt64(0);
            item["output_cc_errors"] = Json::UInt64(0);
            item["input_cc_errors_total"] = Json::UInt64(0);
            item["output_cc_errors_total"] = Json::UInt64(0);
            item["cc_errors"] = Json::UInt64(0);
            item["cc_errors_total"] = Json::UInt64(0);
        }
        if (cfg.satelliteEnabled) {
            bool dvbActive = false;
            auto activeIt = snap.find(cfg.id);
            if (activeIt != snap.end() && activeIt->second) {
                dvbActive = activeIt->second->active.load() && !activeIt->second->usingBackup;
            }

            Json::Value frontend;
            if (dvbActive) {
                frontend = tvs::dvb::frontendStatus(cfg.satelliteAdapter, cfg.satelliteFrontend);
            }
            const uint64_t signalRaw = frontend.get("signal_strength_raw", Json::UInt(0)).asUInt64();
            const uint64_t snrRaw = frontend.get("snr_raw", Json::UInt(0)).asUInt64();
            const auto percentFromRaw = [](uint64_t value) -> unsigned {
                if (value >= 65535ULL) return 100U;
                return static_cast<unsigned>((value * 100ULL + 32767ULL) / 65535ULL);
            };
            item["dvb_signal_percent"] = percentFromRaw(signalRaw);
            item["dvb_quality_percent"] = percentFromRaw(snrRaw);
            item["dvb_has_lock"] = dvbActive && frontend.get("has_lock", false).asBool();
        }

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
        "Server: TVStreamer5\r\n"
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
        std::filesystem::path("/tmp/tvstreamer5-hls") / id / fileName;
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
    const bool passwordProvided =
        (root.isMember("password") && !root.get("password", "").asString().empty()) ||
        (root.isMember("password_md5") && !root.get("password_md5", "").asString().empty());
    if (!passwordProvided) {
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
    if (!root.isMember("ca_providers")) {
        nextConfig.caProviders = previousConfig.caProviders;
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

    auto cfg = StreamConfig::fromJson(root);
    const CaProviderConfig* selectedCaProvider = nullptr;
    if (!cfg.caProviderId.empty()) {
        for (const auto& provider : configManager.config.caProviders) {
            if (provider.id == cfg.caProviderId) {
                selectedCaProvider = &provider;
                break;
            }
        }
        if (!selectedCaProvider) {
            response["result"] = "error";
            response["stream_id"] = cfg.id;
            response["error"] = "CA provider '" + cfg.caProviderId + "' not found";
            Json::StreamWriterBuilder writer;
            return Json::writeString(writer, response);
        }
        if (!selectedCaProvider->enabled) {
            response["result"] = "error";
            response["stream_id"] = cfg.id;
            response["error"] = "CA provider '" + selectedCaProvider->name + "' is disabled";
            Json::StreamWriterBuilder writer;
            return Json::writeString(writer, response);
        }
        unsigned activeOnProvider = 0;
        const auto managed = streamManager.snapshot();
        for (const auto& stream : configManager.config.streams) {
            if (stream.id == cfg.id || stream.caProviderId != cfg.caProviderId) continue;
            auto active = managed.find(stream.id);
            if (active != managed.end() && active->second && active->second->active.load() && !active->second->usingBackup) {
                ++activeOnProvider;
            }
        }
        if (activeOnProvider >= static_cast<unsigned>(selectedCaProvider->maxChannels)) {
            response["result"] = "error";
            response["stream_id"] = cfg.id;
            response["error"] = "CA provider '" + selectedCaProvider->name + "' reached max_channels=" +
                std::to_string(selectedCaProvider->maxChannels);
            Json::StreamWriterBuilder writer;
            return Json::writeString(writer, response);
        }
    }

    std::string startError;
    bool started = streamManager.startStream(cfg, &startError);

    if (!started &&
        !streamManager.isStreamActive(cfg.id) &&
        streamManager.snapshot().count(cfg.id) > 0) {
        std::string restartError;
        started = streamManager.restartStream(cfg, &restartError);
        if (!started && !restartError.empty()) {
            startError = restartError;
        }
    }

    if (started) {
        response["result"] = "ok";
        response["stream_id"] = cfg.id;
        if (cfg.satelliteEnabled && selectedCaProvider) {
            Json::Value warnings(Json::arrayValue);
            const std::string backendType = toLower(selectedCaProvider->backendType);
            if (backendType == "authorized-ts" || backendType == "predecoded-ts" || backendType == "decrypted-ts") {
                warnings.append(
                    "Канал использует authorized pre-decoded TS transport provider '" + selectedCaProvider->name +
                    "' (max_channels=" + std::to_string(selectedCaProvider->maxChannels) +
                    "). TVStreamer получает уже расшифрованный MPEG-TS и не получает/не хранит CW/ECM.");
            } else {
                warnings.append(
                    "Канал назначен CA provider '" + selectedCaProvider->name +
                    "'. Для этого backend типа TVStreamer не выполняет обмен ключами или descrambling.");
            }
            response["warnings"] = warnings;
        }
    } else {
        response["result"] = "error";
        response["stream_id"] = cfg.id;
        response["error"] = startError.empty()
            ? ("Failed to start stream: " + (cfg.name.empty() ? cfg.id : cfg.name))
            : startError;
    }

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, response);
}

void HttpServer::handleStopStream(const std::string& body) {
    Json::CharReaderBuilder readerBuilder;
    Json::Value root;
    std::string errs;
    std::istringstream ss(body);
    if (!Json::parseFromStream(readerBuilder, ss, &root, &errs)) {
        std::cerr << "Invalid stop-stream payload: " << errs << std::endl;
        return;
    }
    std::string id = root.get("id", "").asString();
    streamManager.stopStream(id);
}

void HttpServer::handleRestartProgram() {
    const auto args = currentProcessArgs();
    std::thread([this, args]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        std::cerr << "Restarting TVStreamer5 process" << std::endl;
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
<title>TVStreamer5</title>
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
.tile{position:relative;background:rgba(22,27,37,.94);padding:10px 10px 10px 16px;border-radius:18px;border:1px solid rgba(255,255,255,.06);display:flex;flex-direction:column;gap:6px;min-height:240px;height:240px;width:100%;max-width:none;box-sizing:border-box;box-shadow:0 18px 42px rgba(0,0,0,.14);transition:transform .2s ease,border-color .2s ease;font-size:11px;overflow:hidden}
.tile:before{content:'';position:absolute;left:0;top:12px;bottom:12px;width:4px;border-radius:999px;background:linear-gradient(180deg,#3fc8ff,#1d69ff)}
.tile:hover{transform:translateY(-1px);border-color:rgba(31,136,255,.3)}
.tile.active{border-color:#17c261}
.tile.error{border-color:#fb5f5f}
.tile .top{display:flex;align-items:center;justify-content:space-between;gap:6px;padding-right:82px}
.tile .delete-button{position:absolute;top:8px;right:8px;width:16px;height:16px;padding:0;border:0;border-radius:50%;background:#d9363e;color:#fff;font-size:12px;line-height:16px;cursor:pointer;box-shadow:0 3px 8px rgba(0,0,0,.24)}
.tile .delete-button:hover{background:#f0444d;transform:scale(1.08)}
.tile .title{font-size:11px;font-weight:700;line-height:1.2;color:#fff}
.tile .badge{position:absolute;right:30px;top:8px;left:auto;transform:none;padding:2px 5px;background:rgba(20,161,255,.14);color:#7dd1ff;border-radius:999px;font-size:11px;text-transform:uppercase;letter-spacing:.08em}
.tile .status-pill{padding:2px 6px;background:rgba(255,255,255,.06);color:#c9d2e4;border-radius:999px;font-size:11px;text-transform:uppercase;letter-spacing:.08em}
.tile .status-pill.active{background:rgba(23,194,97,.15);color:#b6f7c2}
.tile .status-pill.stopped{background:rgba(255,95,95,.14);color:#ffb3b3}
.tile .info{display:grid;grid-template-columns:1fr;gap:5px;font-size:11px;color:#b3b8c6;min-height:0;overflow:hidden}
.tile .info-row{display:flex;justify-content:space-between;gap:8px;align-items:center}
.tile .info-row strong{color:#fff;font-size:11px}
.tile .info-row span{max-width:140px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;text-align:right}
.tile .dvb-meters{display:grid;gap:5px;margin:1px 0 2px;padding:6px 7px;border:1px solid rgba(255,255,255,.07);border-radius:9px;background:rgba(255,255,255,.025)}
.tile .dvb-meter-head{display:flex;align-items:center;justify-content:space-between;gap:8px;color:#d8deea;font-size:10px}
.tile .dvb-meter-head strong{font-size:10px;color:#fff;font-weight:600}
.tile .dvb-meter-value{font-variant-numeric:tabular-nums;color:#fff}
.tile .dvb-meter-track{height:7px;overflow:hidden;border-radius:999px;background:rgba(255,255,255,.09);box-shadow:inset 0 0 0 1px rgba(255,255,255,.04)}
.tile .dvb-meter-fill{height:100%;width:0;border-radius:999px;transition:width .35s ease,background-color .35s ease}
.tile .dvb-meter-fill.bad{background:#ff5f5f}
.tile .dvb-meter-fill.warn{background:#ffbd4a}
.tile .dvb-meter-fill.good{background:#17c261}
.tile .dvb-lock{font-size:9px;color:#ffb3b3}
.tile .dvb-lock.locked{color:#b6f7c2}
.tile .controls{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:6px;margin-top:auto}
.tile .controls button{padding:7px 8px;border:none;border-radius:10px;background:rgba(255,255,255,.06);color:#EEE;font-size:9px;cursor:pointer;transition:background .2s ease,transform .08s ease,box-shadow .2s ease}
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
.modal{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(8,10,15,.78);display:none;align-items:center;justify-content:center;padding:12px;z-index:20;box-sizing:border-box}
.modal.quality-open,.modal.stream-open{top:var(--header-height,58px);height:auto;align-items:flex-start;overflow:auto;padding-top:12px}
.modal.active{display:flex}
.modal-content{position:relative;background:rgba(11,15,22,.985);padding:18px 18px;border-radius:22px;width:min(520px,100%);max-height:92%;overflow:auto;box-shadow:0 28px 70px rgba(0,0,0,.24);border:1px solid rgba(255,255,255,.08)}
.modal-content.error-modal{width:min(760px,100%);border-color:rgba(255,95,95,.32)}
.error-modal-head{display:flex;align-items:center;gap:10px;margin-bottom:10px;color:#ffb8b8}
.error-modal-icon{display:grid;place-items:center;flex:0 0 32px;width:32px;height:32px;border-radius:50%;background:rgba(255,95,95,.16);font-weight:800}
.error-modal-message{padding:10px 12px;border-radius:10px;background:rgba(255,95,95,.08);border:1px solid rgba(255,95,95,.18);color:#ffd1d1;white-space:pre-wrap;overflow-wrap:anywhere}
.error-modal-details{margin-top:10px;padding:10px 12px;border-radius:10px;background:#0b1018;border:1px solid rgba(255,255,255,.08);color:#b8c2d4;font:12px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;white-space:pre-wrap;overflow-wrap:anywhere;max-height:320px;overflow:auto}
.modal-close{position:absolute;top:10px;right:10px;width:28px;height:28px;padding:0;border:0;border-radius:8px;background:rgba(255,95,95,.18);color:#ffc2c2;font-size:18px;line-height:28px;cursor:pointer;z-index:2}
.modal-close:hover{background:rgba(255,95,95,.3);color:#fff}
.modal-content.stream-modal{width:min(680px,100%);max-height:calc(100% - 12px);margin:0 auto}
.modal-content.satellite-modal{width:min(980px,100%);max-height:calc(100% - 12px);margin:0 auto;padding:14px 16px}
.modal-content.satellite-modal h2{margin-bottom:9px}
.sat-setup-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:7px 9px}
.sat-setup-grid .form-row{gap:4px;min-width:0}
.sat-setup-grid .form-row input,.sat-setup-grid .form-row select{box-sizing:border-box;width:100%;max-width:none;padding:6px 8px}
.sat-section{grid-column:1/-1;padding:8px 10px;border:1px solid rgba(255,255,255,.07);border-radius:11px;background:rgba(255,255,255,.02)}
.sat-section-title{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:7px;color:#dce5f3;font-size:.78rem;font-weight:700}
.sat-advanced{grid-column:1/-1;border:1px solid rgba(255,255,255,.07);border-radius:10px;background:rgba(255,255,255,.018);padding:0 9px}
.sat-advanced summary{cursor:pointer;padding:7px 0;color:#b9c6d8;font-size:.76rem;font-weight:700;user-select:none}
.sat-advanced[open] summary{margin-bottom:6px;border-bottom:1px solid rgba(255,255,255,.06)}
.sat-advanced-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:7px 9px;padding-bottom:9px}
.sat-advanced-grid .form-row{gap:4px;min-width:0}
.sat-advanced-grid .form-row input,.sat-advanced-grid .form-row select{box-sizing:border-box;width:100%;max-width:none;padding:6px 8px}
.sat-toolbar{display:flex;align-items:center;justify-content:space-between;gap:8px;flex-wrap:wrap}
.sat-toolbar .button-primary{padding:7px 11px}
.sat-scan-status{display:flex;align-items:center;gap:6px;flex:1;min-width:220px;color:#cbd4e4;font-size:.76rem}
.sat-signal-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:7px;margin-top:7px}
.sat-signal-box{padding:6px 8px;border:1px solid rgba(255,255,255,.08);border-radius:9px;background:rgba(255,255,255,.025)}
.sat-signal-head{display:flex;justify-content:space-between;gap:8px;margin-bottom:4px;font-size:.73rem;color:#dbe3ef}
.sat-result-tools{display:grid;grid-template-columns:minmax(180px,1fr) auto;align-items:center;gap:7px;margin:6px 0}
.sat-result-tools input[type="search"]{min-width:0;max-width:none}
.sat-channel-list{display:grid;gap:5px;max-height:250px;overflow:auto;padding-right:2px}
.sat-channel-row{display:grid;grid-template-columns:auto minmax(180px,1.5fr) minmax(70px,.55fr) minmax(80px,.7fr) minmax(110px,.9fr);gap:8px;align-items:center;padding:8px 9px;border:1px solid rgba(255,255,255,.07);border-radius:10px;background:rgba(255,255,255,.025);font-size:.8rem}
.sat-channel-row:hover{border-color:rgba(31,139,255,.3);background:rgba(31,139,255,.05)}
.sat-channel-row.hidden{display:none}
.sat-channel-name{min-width:0}
.sat-channel-name strong{display:block;color:#fff;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sat-channel-name small{display:block;color:#8f9aab;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.sat-channel-meta{color:#c9d2e2;white-space:nowrap}
.sat-ca{display:inline-block;padding:2px 6px;border-radius:999px;font-size:.7rem;background:rgba(23,194,97,.14);color:#b6f7c2}
.sat-ca.scrambled{background:rgba(255,189,74,.14);color:#ffe0a3}
@media(max-width:900px){.sat-setup-grid,.sat-advanced-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.sat-result-tools{grid-template-columns:1fr auto}}
@media(max-width:700px){.sat-setup-grid,.sat-advanced-grid,.sat-signal-grid{grid-template-columns:1fr}.sat-result-tools{grid-template-columns:1fr}.sat-channel-row{grid-template-columns:auto minmax(0,1fr);}.sat-channel-meta{grid-column:2}.sat-channel-codecs{grid-column:2}}
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
<button class="system-menu-item" onclick="openCaProvidersModal();closeSystemMenu()">CA Providers</button>
<button class="system-menu-item" onclick="openAboutModal();closeSystemMenu()" data-i18n="about">About</button>
<button class="system-menu-item restart-button" onclick="closeSystemMenu();restartProgram()" data-i18n="restartProgram">Restart</button>
</div>
</details>
<button class="button-secondary" onclick="downloadVlcPlaylist()" data-i18n="playlist">VLC playlist</button>
<button class="button-secondary" onclick="openSubscribersModal()" data-i18n="subscribers">Subscribers</button>
<button class="button-secondary" onclick="openSatelliteChannelModal()" data-i18n="addChannel">+ Add channel</button>
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
    interfacesNotFound:'No interfaces found', output:'Output', activeInput:'Active input', primary:'Primary', backup:'Backup', sid:'SID', bitrateIn:'Bitrate In', bitrateOut:'Bitrate Out', status:'Status', signalLevel:'Signal level', signalQuality:'Signal quality', lock:'LOCK', noLock:'NO LOCK',
    online:'Online', backupOnline:'Backup', offline:'Offline', start:'Start', stop:'Stop', edit:'Edit', chart:'Chart', delete:'Delete stream', removeConfirm:'Delete stream',
    restartProgram:'Restart', restartConfirm:'Restart TVStreamer5 now?', restarting:'Restarting...',
    networkLoad:'Network interface load', interface:'Interface', incoming:'Incoming', outgoing:'Outgoing', close:'Close',
    about:'About', product:'Product', version:'Version', name:'Name', country:'Country', donate:'Donate', donateQr:'Donate QR code', cancel:'Cancel', save:'Save', userTitle:'User', telegram:'Telegram API', quality:'Stream quality', playlist:'VLC playlist', subscribers:'Subscribers', streams:'Streams', filtering:'Enable IP filtering', addSubscriber:'Add subscriber', primaryIp:'Primary IP', backupIp:'Backup IP', addedAt:'Added at', subscriberName:'Subscriber name', noSubscribers:'No subscribers added', noStreams:'No streams configured', enabled:'Enabled', disabled:'Disabled', exportSubscribers:'Export TXT', session:'Session', activeSession:'Online', offlineSession:'Offline', resetSession:'Reset'
  },
  ru: {
    subtitle:'Мониторинг трансляций и управление потоками', total:'Всего:', active:'Активно:', network:'Сеть', system:'Система', user:'Пользователь', addStream:'+ Добавить поток', addChannel:'+ Добавить канал',
    interfacesNotFound:'Интерфейсы не найдены', output:'Вывод', activeInput:'Активный вход', primary:'Основной', backup:'Резерв', sid:'SID', bitrateIn:'Bitrate In', bitrateOut:'Bitrate Out', status:'Статус', signalLevel:'Уровень сигнала', signalQuality:'Качество сигнала', lock:'LOCK', noLock:'НЕТ LOCK',
    online:'Онлайн', backupOnline:'Резерв', offline:'Офлайн', start:'Старт', stop:'Стоп', edit:'Ред.', chart:'График', delete:'Удалить поток', removeConfirm:'Удалить поток',
    restartProgram:'Перезапуск', restartConfirm:'Перезапустить TVStreamer5 сейчас?', restarting:'Перезапуск...',
    networkLoad:'Загрузка сетевых интерфейсов', interface:'Интерфейс', incoming:'Входящий', outgoing:'Исходящий', close:'Закрыть',
    about:'О программе', product:'Программа', version:'Версия', name:'Имя', country:'Страна', donate:'Донат', donateQr:'QR-код доната', cancel:'Отмена', save:'Сохранить', userTitle:'Пользователь', telegram:'Telegram API', quality:'Качество потока', playlist:'Плейлист VLC', subscribers:'Абоненты', streams:'Потоки', filtering:'Включить фильтрацию по IP', addSubscriber:'Добавить абонента', primaryIp:'Основной IP', backupIp:'Резервный IP', addedAt:'Дата добавления', subscriberName:'Наименование абонента', noSubscribers:'Абоненты не добавлены', noStreams:'Потоки не настроены', enabled:'Включен', disabled:'Отключен', exportSubscribers:'Экспорт TXT', session:'Сессия', activeSession:'Онлайн', offlineSession:'Офлайн', resetSession:'Сбросить'
  }
};
function normalizeLanguage(value) {
  return value === 'ru' ? 'ru' : 'en';
}
let language = normalizeLanguage(localStorage.getItem('tvstreamer-language') || 'en');
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
  localStorage.setItem('tvstreamer-language', language);
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
let state = {};
let statePollTimer = null;
let metricsPollTimer = null;
let stateFetchPromise = null;
let metricsFetchPromise = null;
let lastTileStructureSignature = '';
let subscribersModalOpen = false;
let subscriberFormBaseline = '';
let satelliteScanServices = [];
function saveLanguagePreference(sourceState=state) {
  if (!Array.isArray(sourceState.streams)) return;
  fetch('/api/save-config', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({
      login: sourceState.login,
      server_name: sourceState.server_name,
      telegram_token: sourceState.telegram_token,
      telegram_chat_id: sourceState.telegram_chat_id,
      http_port: sourceState.http_port,
      language,
      ca_providers: sourceState.ca_providers || [],
      streams: sourceState.streams
    })
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
      const cachedInterfaces = state.interfaces;
      const cachedDvbDevices = state.dvb_devices;
      const storedLanguage = localStorage.getItem('tvstreamer-language');
      const serverLanguage = normalizeLanguage(data.language);
      language = normalizeLanguage(storedLanguage || language);
      localStorage.setItem('tvstreamer-language', language);
      data.language = language;
      state = data;
      if (cachedInterfaces) state.interfaces = cachedInterfaces;
      if (cachedDvbDevices) state.dvb_devices = cachedDvbDevices;
      applyLanguage();
      render(false);
      refreshSubscriberSessions();
      if (serverLanguage !== language) saveLanguagePreference(data);
      return data;
    })
    .catch(error => {
      console.warn('TVStreamer5 state refresh failed:', error);
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
      console.warn('TVStreamer5 metrics refresh failed:', error);
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
  link.download = 'tvstreamer5-playlist.m3u';
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
  document.getElementById('modal').classList.remove('quality-open', 'stream-open');
  document.getElementById('modal').classList.add('active');
}
function closeModal() {
  subscribersModalOpen = false;
  stopQualityAutoRefresh();
  document.getElementById('modal').classList.remove('active', 'quality-open', 'stream-open');
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
function satelliteFrequencyKhzToMhz(value) {
  const khz = Number(value || 0);
  if (!Number.isFinite(khz) || khz <= 0) return 0;
  return khz / 1000;
}
function satelliteFrequencyMhzToKhz(value) {
  const mhz = Number(value || 0);
  if (!Number.isFinite(mhz) || mhz <= 0) return 0;
  return Math.round(mhz * 1000);
}
function formatSatelliteFrequencyMhz(value) {
  const mhz = satelliteFrequencyKhzToMhz(value);
  if (!mhz) return '—';
  return Number.isInteger(mhz) ? String(mhz) : mhz.toFixed(3).replace(/0+$/, '').replace(/\.$/, '');
}
function satelliteInputSummary(stream) {
  const system = stream.satellite_delivery_system === 'dvb-s' ? 'DVB-S' : 'DVB-S2';
  const freq = formatSatelliteFrequencyMhz(stream.satellite_frequency);
  const sr = Number(stream.satellite_symbol_rate || 0);
  const pol = String(stream.satellite_polarization || 'H').toUpperCase();
  const sid = Number(stream.satellite_service_id || 0);
  return `${system} · ${freq} MHz · SR ${sr || '—'} kBd · ${pol}${sid ? ` · SID ${sid}` : ''}`;
}
function primaryInputSummary(stream) {
  return stream.satellite_enabled ? satelliteInputSummary(stream) : (stream.input_uri || '—');
}
function caProviderTileSummary(stream) {
  if (!stream?.satellite_enabled) return '';
  const provider = (state.ca_providers || []).find(item => String(item.id || '') === String(stream.ca_provider_id || ''));
  if (provider) {
    return `${provider.name || provider.id} ${Number(provider.active_channels || 0)}/${Number(provider.max_channels || 8)}`;
  }
  return stream.ca_provider_id ? `${stream.ca_provider_id} (не найден)` : 'без CA';
}
function satelliteTilePrimarySummary(stream) {
  const system = stream.satellite_delivery_system === 'dvb-s' ? 'DVB-S' : 'DVB-S2';
  const freq = formatSatelliteFrequencyMhz(stream.satellite_frequency);
  const sr = Number(stream.satellite_symbol_rate || 0);
  const pol = String(stream.satellite_polarization || 'H').toUpperCase();
  return `${system} · ${freq} MHz · ${pol}${sr ? ` · ${sr}k` : ''} · CA ${caProviderTileSummary(stream)}`;
}
function satelliteTileActiveInputSummary(stream) {
  const source = stream.active_input_label || t('primary');
  if (stream.using_backup) return `${source} · ${stream.active_input_uri || stream.backup_input_uri || '—'}`;
  const signal = clampPercent(stream.dvb_signal_percent);
  const quality = clampPercent(stream.dvb_quality_percent);
  return `${source} · S ${signal}% · Q ${quality}% · ${stream.dvb_has_lock ? t('lock') : t('noLock')}`;
}
function clampPercent(value) {
  const number = Number(value || 0);
  return Math.max(0, Math.min(100, Number.isFinite(number) ? number : 0));
}
function dvbMeterClass(value) {
  const percent = clampPercent(value);
  if (percent >= 65) return 'good';
  if (percent >= 35) return 'warn';
  return 'bad';
}
function updateDvbMeter(tile, rolePrefix, value) {
  const percent = clampPercent(value);
  const fill = tile.querySelector(`[data-role="${rolePrefix}-fill"]`);
  const label = tile.querySelector(`[data-role="${rolePrefix}-value"]`);
  if (fill) {
    fill.style.width = `${percent}%`;
    fill.className = `dvb-meter-fill ${dvbMeterClass(percent)}`;
  }
  if (label) label.textContent = `${percent}%`;
}
function streamTileStructureSignature(stream) {
  return {
    id: stream.id,
    name: stream.name,
    input_uri: stream.input_uri,
    satellite_enabled: stream.satellite_enabled,
    satellite_adapter: stream.satellite_adapter,
    satellite_frontend: stream.satellite_frontend,
    satellite_frequency: stream.satellite_frequency,
    satellite_symbol_rate: stream.satellite_symbol_rate,
    satellite_polarization: stream.satellite_polarization,
    satellite_delivery_system: stream.satellite_delivery_system,
    satellite_modulation: stream.satellite_modulation,
    satellite_fec: stream.satellite_fec,
    satellite_diseqc_source: stream.satellite_diseqc_source,
    satellite_stream_id: stream.satellite_stream_id,
    satellite_service_id: stream.satellite_service_id,
    ca_provider_id: stream.ca_provider_id,
    backup_input_uri: stream.backup_input_uri,
    backup_input_type: stream.backup_input_type,
    backup_file_loop: stream.backup_file_loop,
    service_id: stream.service_id,
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
function updateStreamTile(tile, stream) {
  if (!tile || !stream) return;
  tile.classList.toggle('active', !!stream.active);

  const statusPill = tile.querySelector('[data-role="status-pill"]');
  if (statusPill) {
    statusPill.className = `status-pill ${stream.active ? 'active' : 'stopped'}`;
    statusPill.textContent = stream.active ? (stream.using_backup ? 'Backup' : 'Online') : 'Offline';
  }

  const activeInput = tile.querySelector('[data-role="active-input"]');
  if (activeInput) {
    activeInput.textContent = stream.satellite_enabled
      ? satelliteTileActiveInputSummary(stream)
      : `${stream.active_input_label || t('primary')} · ${stream.active_input_uri || primaryInputSummary(stream)}`;
  }
  const primarySummary = tile.querySelector('[data-role="primary-summary"]');
  if (primarySummary) {
    primarySummary.textContent = stream.satellite_enabled ? satelliteTilePrimarySummary(stream) : primaryInputSummary(stream);
  }

  const bitrateIn = tile.querySelector('[data-role="bitrate-in"]');
  if (bitrateIn) bitrateIn.textContent = stream.bitrate_in_kbps ? `${stream.bitrate_in_kbps} kbps` : '—';

  const bitrateOut = tile.querySelector('[data-role="bitrate-out"]');
  if (bitrateOut) bitrateOut.textContent = stream.bitrate_out_kbps ? `${stream.bitrate_out_kbps} kbps` : '—';

  const status = tile.querySelector('[data-role="stream-status"]');
  if (status) status.textContent = stream.status || '';


  const toggleButton = tile.querySelector('[data-role="stream-toggle"]');
  if (toggleButton) {
    toggleButton.className = stream.active ? 'stop-button' : 'start-button';
    toggleButton.textContent = stream.active ? t('stop') : t('start');
    toggleButton.onclick = () => toggleStream(stream.id, !!stream.active);
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
    tile.className = 'tile' + (stream.satellite_enabled ? ' satellite' : '') + (stream.active ? ' active' : '');
    tile.dataset.streamId = String(stream.id);
    tile.innerHTML = `
      <div class="top">
        <div>
          <div class="title">${stream.name || stream.id}</div>
          <div data-role="status-pill" class="status-pill ${stream.active ? 'active' : 'stopped'}">${stream.active ? (stream.using_backup ? 'Backup' : 'Online') : 'Offline'}</div>
        </div>
        <div class="badge">${outputs.length > 1 ? outputBadgeText(stream) : bitrateMode}</div>
      </div>
      <button class="delete-button" title="Удалить поток" aria-label="Удалить поток" onclick="deleteStream('${stream.id}')">×</button>
      <div class="info">
        <div class="info-row"><strong>${t('activeInput')}</strong><span data-role="active-input">${stream.satellite_enabled ? satelliteTileActiveInputSummary(stream) : `${stream.active_input_label || t('primary')} · ${stream.active_input_uri || primaryInputSummary(stream)}`}</span></div>
        <div class="info-row"><strong>${t('primary')}</strong><span data-role="primary-summary">${stream.satellite_enabled ? satelliteTilePrimarySummary(stream) : primaryInputSummary(stream)}</span></div>
        <div class="info-row"><strong>${t('backup')}</strong><span>${stream.backup_input_uri || '—'}${stream.backup_input_type === 'file' && stream.backup_file_loop ? ' · loop' : ''}</span></div>
        <div class="info-row"><strong>${t('sid')}</strong><span>${stream.service_id || '—'}</span></div>
        <div class="info-row"><strong>${t('bitrateIn')}</strong><span data-role="bitrate-in">${stream.bitrate_in_kbps ? stream.bitrate_in_kbps + ' kbps' : '—'}</span></div>
        <div class="info-row"><strong>${t('bitrateOut')}</strong><span data-role="bitrate-out">${stream.bitrate_out_kbps ? stream.bitrate_out_kbps + ' kbps' : '—'}</span></div>
        <div class="info-row"><strong>${t('status')}</strong><span data-role="stream-status">${stream.status || ''}</span></div>
      </div>
      <div class="controls">
        <button data-role="stream-toggle" class="${stream.active ? 'stop-button' : 'start-button'}">${stream.active ? t('stop') : t('start')}</button>
        <button onclick="editStream('${stream.id}')">${t('edit')}</button>
        <button class="quality-button" onclick="openQualityModal('${stream.id}')">${t('chart')}</button>
        <button class="copy-button" onclick="copyStreamLinks('${stream.id}', this)">${links.length > 1 ? 'URLs' : 'URL'}</button>
      </div>`;
    tiles.appendChild(tile);
    updateStreamTile(tile, stream);
  });
  lastTileStructureSignature = signature;
  updateLiveTiles();
}
function openStreamError(title, message, details='') {
  subscribersModalOpen = false;
  const content = document.getElementById('modalContent');
  content.className = 'modal-content error-modal';
  content.innerHTML = modalCloseButton() + `
    <div class="error-modal-head"><span class="error-modal-icon">!</span><h2 style="margin:0">${escapeHtmlValue(title || 'Ошибка')}</h2></div>
    <div class="error-modal-message">${escapeHtmlValue(message || 'Неизвестная ошибка')}</div>
    ${details ? `<div class="error-modal-details">${escapeHtmlValue(details)}</div>` : ''}
    <div class="modal-actions"><button class="button-primary" onclick="closeModal()">${t('close')}</button></div>`;
  document.getElementById('modal').classList.remove('quality-open', 'stream-open');
  document.getElementById('modal').classList.add('active');
}
async function checkStreamStartError(id, name) {
  try {
    const response = await fetch('/api/state', {cache:'no-store'});
    const latest = await response.json();
    const current = (latest.streams || []).find(stream => String(stream.id) === String(id));
    if (!current) return;
    const status = String(current.status || '').trim();
    const lowered = status.toLowerCase();
    const looksLikeError = lowered.startsWith('error:') ||
      lowered.includes('failed') ||
      lowered.includes('no input signal');
    if (!current.active && looksLikeError) {
      openStreamError('Ошибка запуска потока', `${name || id}: ${status}`);
    }
  } catch (_) {
    // The primary start request already reports transport/JSON failures. A
    // follow-up status check is best-effort and must not replace that error.
  }
}
function toggleStream(id, active) {
  const url = active ? '/api/stop-stream' : '/api/start-stream';
  const stream = state.streams.find(s=>s.id===id);
  const body = active ? {id} : stream;
  fetch(url, {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(async response=>{
      let data = {};
      const text = await response.text();
      try { data = text ? JSON.parse(text) : {}; } catch (_) { data = {result:'error', error:text || `HTTP ${response.status}`}; }
      if (!response.ok || data.result === 'error') {
        const message = data.error || `HTTP ${response.status} ${response.statusText}`;
        openStreamError(
          active ? 'Ошибка остановки потока' : 'Ошибка запуска потока',
          `${stream?.name || id}: ${message}`,
          data.details || ''
        );
        throw new Error(message);
      }
      if (!active && Array.isArray(data.warnings) && data.warnings.length) {
        openStreamError('Предупреждение запуска', `${stream?.name || id}: поток запущен с предупреждением`, data.warnings.join('\n'));
      }
      if (!active) {
        // Some source errors arrive asynchronously after PLAYING was accepted.
        // Re-check the live stream status so Start-button failures are visible
        // instead of being left only in the tile/journal.
        setTimeout(() => checkStreamStartError(id, stream?.name || id), 2500);
        setTimeout(() => checkStreamStartError(id, stream?.name || id), 5500);
      }
      return data;
    })
    .catch(error=>{
      if (!document.getElementById('modal')?.classList.contains('active')) {
        openStreamError(
          active ? 'Ошибка остановки потока' : 'Ошибка запуска потока',
          `${stream?.name || id}: ${error.message || error}`
        );
      }
    })
    .finally(()=>{
      setTimeout(fetchState,300);
      setTimeout(fetchState,1200);
    });
}
function deleteStream(id) {
  const stream = state.streams.find(s=>s.id===id);
  if (!stream || !window.confirm(`${t('removeConfirm')} «${stream.name || stream.id}»?`)) return;
  fetch('/api/delete-stream', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})})
    .then(()=>{ closeModal(); setTimeout(fetchState, 300); });
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
  link.download = 'tvstreamer5-subscribers.txt';
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
      <div class="about-row"><strong>${t('product')}</strong><span>TVStreamer5</span></div>
      <div class="about-row"><strong>${t('version')}</strong><span>v82</span></div>
      <div class="about-row"><strong>${t('name')}</strong><span>Лукомский Виталий</span></div>
      <div class="about-row"><strong>${t('country')}</strong><span>Беларусь, г. Борисов</span></div>
      <div class="about-row"><strong>Email</strong><a href="mailto:monkipnet@gmail.com">monkipnet@gmail.com</a></div>
      <div class="about-row about-donate"><strong>${t('donate')}</strong><div class="about-donate-content">
        <svg class="about-qr" viewBox="0 0 41 41" role="img" aria-label="${t('donateQr')}" shape-rendering="crispEdges">
          <rect width="41" height="41" fill="#fff"></rect>
          <path d="${donateQrPath}" fill="#111"></path>
        </svg>
        <span class="about-donate-address">${donateAddress}</span>
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
      <div class="form-row"><label>Новый пароль</label><input id="password" type="password" placeholder="Оставьте пустым, чтобы не менять" /><small>В конфигурации сохраняется только MD5-хэш пароля.</small></div>
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
function nextCaProviderId() {
  const used = new Set((state.ca_providers || []).map(provider => String(provider.id || '')));
  document.querySelectorAll?.('.ca-provider-row [data-ca-field="id"]').forEach(input => used.add(String(input.value || '')));
  let index = 1;
  while (used.has(`ca-card-${index}`)) index += 1;
  return `ca-card-${index}`;
}
function caProviderRowHtml(provider={}) {
  const id = String(provider.id || nextCaProviderId());
  const assigned = Number(provider.assigned_channels || (state.streams || []).filter(stream => stream.ca_provider_id === id).length || 0);
  const active = Number(provider.active_channels || 0);
  const maxChannels = Math.max(1, Number(provider.max_channels || 8));
  return `<div class="ca-provider-row" data-provider-id="${escapeHtmlValue(id)}" style="border:1px solid #344155;border-radius:10px;padding:12px;margin-bottom:10px">
    <div class="form-grid">
      <div class="form-row"><label>ID</label><input data-ca-field="id" value="${escapeHtmlValue(id)}" /><small>Например ca-card-1. При переименовании ссылки каналов обновятся при сохранении.</small></div>
      <div class="form-row"><label>Название</label><input data-ca-field="name" value="${escapeHtmlValue(provider.name || '')}" placeholder="Основная карта / CAM" /></div>
      <div class="form-row"><label>Backend</label><select data-ca-field="backend_type"><option value="authorized-ts" ${provider.backend_type==='authorized-ts'?'selected':''}>Authorized pre-decoded TS</option><option value="external" ${(provider.backend_type||'external')==='external'?'selected':''}>External authorized backend</option><option value="cam-service" ${provider.backend_type==='cam-service'?'selected':''}>CAM service</option><option value="custom" ${provider.backend_type==='custom'?'selected':''}>Custom integration</option></select></div>
      <div class="form-row"><label>Endpoint</label><input data-ca-field="endpoint" value="${escapeHtmlValue(provider.endpoint || '')}" placeholder="udp://127.0.0.1:9000 или srt://host:port?streamid={service_id}" /></div>
      <div class="form-row"><label>Max active channels</label><input data-ca-field="max_channels" type="number" min="1" max="64" value="${maxChannels}" /></div>
      <div class="form-row"><label>Состояние</label><div class="checkbox-inline"><input data-ca-field="enabled" type="checkbox" ${provider.enabled===false?'':'checked'} /><span>Provider включён</span></div><small>${active}/${maxChannels} active · ${assigned} assigned</small></div>
    </div>
    <div style="display:flex;justify-content:flex-end;margin-top:8px"><button class="button-secondary" type="button" onclick="removeCaProviderRow(this)">Удалить</button></div>
  </div>`;
}
function serialReaderTableHtml(readers=[]) {
  if (!Array.isArray(readers) || !readers.length) {
    return '<div style="color:#aeb8ca;padding:10px 0">Устройства /dev/serial/by-id/* не обнаружены.</div>';
  }
  return `<div style="overflow-x:auto"><table style="width:100%;border-collapse:collapse;font-size:13px">
    <thead><tr style="text-align:left;color:#aeb8ca"><th style="padding:7px;border-bottom:1px solid #344155">By-ID</th><th style="padding:7px;border-bottom:1px solid #344155">Device</th><th style="padding:7px;border-bottom:1px solid #344155">Производитель</th><th style="padding:7px;border-bottom:1px solid #344155">Модель</th><th style="padding:7px;border-bottom:1px solid #344155">Serial</th></tr></thead>
    <tbody>${readers.map(reader => `<tr><td style="padding:7px;border-bottom:1px solid #253044"><code title="${escapeHtmlValue(reader.by_id || '')}">${escapeHtmlValue(reader.by_id_name || reader.by_id || '')}</code></td><td style="padding:7px;border-bottom:1px solid #253044"><strong>${escapeHtmlValue(reader.device || reader.tty || '')}</strong></td><td style="padding:7px;border-bottom:1px solid #253044">${escapeHtmlValue(reader.vendor || '—')}</td><td style="padding:7px;border-bottom:1px solid #253044">${escapeHtmlValue(reader.model || '—')}</td><td style="padding:7px;border-bottom:1px solid #253044"><code>${escapeHtmlValue(reader.serial || '—')}</code></td></tr>`).join('')}</tbody>
  </table></div>`;
}
async function refreshSerialReaders() {
  const target = document.getElementById('serialReadersTable');
  const status = document.getElementById('serialReadersStatus');
  if (!target) return;
  if (status) status.textContent = 'Поиск serial-reader устройств…';
  try {
    const response = await fetch('/api/serial-readers', {cache:'no-store'});
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const readers = await response.json();
    target.innerHTML = serialReaderTableHtml(readers);
    if (status) status.textContent = `Обнаружено: ${Array.isArray(readers) ? readers.length : 0}`;
  } catch (error) {
    target.innerHTML = '<div style="color:#ff8f8f;padding:10px 0">Не удалось получить список serial-reader устройств.</div>';
    if (status) status.textContent = error.message || String(error);
  }
}
function openCaProvidersModal() {
  const providers = Array.isArray(state.ca_providers) ? state.ca_providers : [];
  openModal(`
    <h2>CA Providers</h2>
    <div class="sat-signal-box" style="margin-bottom:12px"><strong>Логический CA provider</strong><div style="margin-top:4px;color:#aeb8ca">Один provider назначается нескольким спутниковым каналам и ограничивает число одновременно активных каналов. Физические /dev/ttyUSB* и /dev/ttyACM* к отдельным каналам не привязываются.</div><small>Provider ID — внутренний идентификатор TVStreamer, а не ID карты. Для новых provider автоматически предлагаются понятные ID вида ca-card-1, ca-card-2. Backend «Authorized pre-decoded TS» принимает уже расшифрованный MPEG-TS от авторизованного CAM/CA backend.</small></div>
    <div style="border:1px solid #344155;border-radius:10px;padding:12px;margin-bottom:14px">
      <div style="display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap"><div><strong>Обнаруженные serial-reader устройства</strong><div id="serialReadersStatus" style="font-size:12px;color:#aeb8ca;margin-top:3px">Поиск…</div></div><button class="button-secondary" type="button" onclick="refreshSerialReaders()">Обновить</button></div>
      <div id="serialReadersTable" style="margin-top:8px"><div style="color:#aeb8ca;padding:10px 0">Загрузка…</div></div>
      <small style="display:block;margin-top:8px">By-ID стабилен и предпочтительнее /dev/ttyUSBN. Производитель, модель и serial относятся к USB/serial-reader. Какая карта вставлена в ридер, должен сообщать внешний авторизованный backend.</small>
    </div>
    <div id="caProviderRows">${providers.map(caProviderRowHtml).join('') || '<div id="caProvidersEmpty" style="color:#aeb8ca;margin:10px 0">CA Providers пока не созданы.</div>'}</div>
    <button class="button-secondary" type="button" onclick="addCaProviderRow()">+ Добавить CA Provider</button>
    <div id="caProviderSaveStatus" style="margin-top:10px;color:#ffb36b"></div>
    <div class="modal-actions"><button class="button-secondary" onclick="closeModal()">Отмена</button><button class="button-primary" onclick="saveCaProviders()">Сохранить</button></div>
  `);
  refreshSerialReaders();
}
function addCaProviderRow() {
  document.getElementById('caProvidersEmpty')?.remove();
  const rows = document.getElementById('caProviderRows');
  if (!rows) return;
  const wrapper = document.createElement('div');
  const nextId = nextCaProviderId();
  wrapper.innerHTML = caProviderRowHtml({id:nextId,name:`Карта ${nextId.replace('ca-card-','')}`,max_channels:8,enabled:true});
  rows.appendChild(wrapper.firstElementChild);
}
function removeCaProviderRow(button) {
  const row = button?.closest('.ca-provider-row');
  if (!row) return;
  const id = row.querySelector('[data-ca-field="id"]')?.value || row.dataset.providerId || '';
  const assigned = (state.streams || []).filter(stream => stream.ca_provider_id === id).length;
  if (assigned > 0) {
    const status = document.getElementById('caProviderSaveStatus');
    if (status) status.textContent = `Provider ${id} назначен ${assigned} каналам. Сначала переключите эти каналы на другой provider или «Без CA provider».`;
    return;
  }
  row.remove();
}
function collectCaProviders() {
  const providers = [];
  const ids = new Set();
  for (const row of document.querySelectorAll('.ca-provider-row')) {
    const id = String(row.querySelector('[data-ca-field="id"]')?.value || '').trim();
    const name = String(row.querySelector('[data-ca-field="name"]')?.value || '').trim();
    if (!id) throw new Error('У каждого CA Provider должен быть ID.');
    if (!/^[A-Za-z0-9._-]+$/.test(id)) throw new Error(`Недопустимый ID provider: ${id}`);
    if (ids.has(id)) throw new Error(`Повторяющийся ID provider: ${id}`);
    ids.add(id);
    providers.push({
      id,
      name: name || id,
      backend_type: row.querySelector('[data-ca-field="backend_type"]')?.value || 'external',
      endpoint: String(row.querySelector('[data-ca-field="endpoint"]')?.value || '').trim(),
      max_channels: Math.min(64, Math.max(1, Number(row.querySelector('[data-ca-field="max_channels"]')?.value || 8))),
      enabled: row.querySelector('[data-ca-field="enabled"]')?.checked !== false
    });
  }
  return providers;
}
function saveCaProviders() {
  const status = document.getElementById('caProviderSaveStatus');
  let providers;
  try { providers = collectCaProviders(); }
  catch (error) { if (status) status.textContent = error.message || String(error); return; }

  const renameMap = new Map();
  for (const row of document.querySelectorAll('.ca-provider-row')) {
    const originalId = String(row.dataset.providerId || '').trim();
    const nextId = String(row.querySelector('[data-ca-field="id"]')?.value || '').trim();
    if (originalId && nextId && originalId !== nextId) renameMap.set(originalId, nextId);
  }
  const updatedStreams = (state.streams || []).map(stream => {
    const nextProviderId = renameMap.get(String(stream.ca_provider_id || ''));
    return nextProviderId ? {...stream, ca_provider_id: nextProviderId} : {...stream};
  });
  const validIds = new Set(providers.map(provider => provider.id));
  const missing = updatedStreams.filter(stream => stream.ca_provider_id && !validIds.has(stream.ca_provider_id));
  if (missing.length) {
    if (status) status.textContent = `Нельзя удалить provider: ${missing.length} канал(ов) всё ещё ссылаются на удалённый ID.`;
    return;
  }
  const payload = {
    login: state.login,
    server_name: state.server_name,
    telegram_token: state.telegram_token,
    telegram_chat_id: state.telegram_chat_id,
    http_port: state.http_port,
    language,
    ca_providers: providers,
    streams: updatedStreams
  };
  fetch('/api/save-config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(payload)})
    .then(response=>{ if(!response.ok) throw new Error(`HTTP ${response.status}`); return response.json(); })
    .then(()=>{ state.ca_providers = providers; state.streams = updatedStreams; closeModal(); fetchState(); })
    .catch(error=>{ if(status) status.textContent = `Ошибка сохранения: ${error.message || error}`; });
}
function openStreamModal() {
  openStreamForm({
    id: 'stream-' + Date.now(),
    name:'', input_uri:'', backup_input_uri:'', backup_input_type:'url', backup_file_loop:false, output_type:'udp-cbr', output_mode:'listener', output_host:'127.0.0.1', output_port:1234,
    interface_address:'', input_interface_address:'', input_mode:'auto', satellite_enabled:false, satellite_adapter:0, satellite_frontend:0, satellite_frequency:0, satellite_symbol_rate:27500, satellite_polarization:'H', satellite_delivery_system:'dvb-s2', satellite_modulation:'auto', satellite_fec:'auto', satellite_pilot:'auto', satellite_rolloff:'auto', satellite_diseqc_source:-1, satellite_stream_id:-1, satellite_service_id:1, satellite_lnb_lof1:9750000, satellite_lnb_lof2:10600000, satellite_lnb_slof:11700000, ca_provider_id:'', test_pattern:false, auto_start:false, remap_enabled:false, cbr:true, target_bitrate:2000000, transcode_enabled:false, transcode_resolution:'1920x1080', transcode_video_bitrate:6000000, transcode_audio_codec:'aac', transcode_audio_bitrate:192000,
    audio_pid:0, video_pid:0, service_id:1, service_name:'', service_provider:'', additional_outputs:[]
  });
}
function outputTypeOptions(selected) {
  const options = [
    ['udp-vbr', 'UDP MPEG-TS VBR'],
    ['udp-cbr', 'UDP MPEG-TS CBR'],
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
function escapeHtmlValue(value) {
  return String(value ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;');
}
function dvbAdapters() {
  return Array.isArray(state.dvb_devices?.adapters) ? state.dvb_devices.adapters : [];
}
function satelliteAdapterOptions(selected) {
  const selectedNumber = Number(selected || 0);
  const adapters = dvbAdapters();
  if (!adapters.length) {
    return `<option value="${selectedNumber}" selected>Adapter ${selectedNumber} (не найден в /dev/dvb)</option>`;
  }
  let found = false;
  const options = adapters.map(adapter => {
    const number = Number(adapter.adapter || 0);
    if (number === selectedNumber) found = true;
    const frontends = Array.isArray(adapter.frontends) ? adapter.frontends.length : 0;
    return `<option value="${number}" ${number===selectedNumber?'selected':''}>Adapter ${number} · ${frontends} frontend</option>`;
  });
  if (!found) options.unshift(`<option value="${selectedNumber}" selected>Adapter ${selectedNumber} (сохранён, сейчас не найден)</option>`);
  return options.join('');
}
function satelliteFrontendOptions(adapterNumber, selected) {
  const selectedNumber = Number(selected || 0);
  const adapter = dvbAdapters().find(item => Number(item.adapter) === Number(adapterNumber));
  const frontends = Array.isArray(adapter?.frontends) ? adapter.frontends : [];
  if (!frontends.length) {
    return `<option value="${selectedNumber}" selected>Frontend ${selectedNumber} (не найден)</option>`;
  }
  let found = false;
  const options = frontends.map(frontend => {
    const number = Number(frontend.frontend || 0);
    if (number === selectedNumber) found = true;
    const systems = Array.isArray(frontend.delivery_systems) && frontend.delivery_systems.length
      ? ` · ${frontend.delivery_systems.join('/')}` : '';
    const name = frontend.name ? ` · ${frontend.name}` : '';
    return `<option value="${number}" ${number===selectedNumber?'selected':''}>Frontend ${number}${escapeHtmlValue(name)}${escapeHtmlValue(systems)}</option>`;
  });
  if (!found) options.unshift(`<option value="${selectedNumber}" selected>Frontend ${selectedNumber} (сохранён, сейчас не найден)</option>`);
  return options.join('');
}
function caProviderOptions(selected) {
  const selectedValue = String(selected || '');
  const providers = Array.isArray(state.ca_providers) ? state.ca_providers : [];
  const options = [`<option value="" ${selectedValue?'':'selected'}>Без CA provider</option>`];
  providers.forEach(provider => {
    const id = String(provider.id || '');
    if (!id) return;
    const name = provider.name || id;
    const active = Number(provider.active_channels || 0);
    const max = Math.max(1, Number(provider.max_channels || 8));
    const disabled = provider.enabled === false;
    const status = disabled ? ' · отключён' : ` · ${active}/${max} active`;
    options.push(`<option value="${escapeHtmlValue(id)}" ${id===selectedValue?'selected':''} ${disabled?'disabled':''}>${escapeHtmlValue(name)}${escapeHtmlValue(status)}</option>`);
  });
  if (selectedValue && !providers.some(provider => String(provider.id || '') === selectedValue)) {
    options.push(`<option value="${escapeHtmlValue(selectedValue)}" selected>${escapeHtmlValue(selectedValue)} (provider не найден)</option>`);
  }
  return options.join('');
}
function updateChannelSatelliteFrontendOptions() {
  const adapter = document.getElementById('channelSatelliteAdapter');
  const frontend = document.getElementById('channelSatelliteFrontend');
  if (!adapter || !frontend) return;
  const previous = Number(frontend.value || 0);
  frontend.innerHTML = satelliteFrontendOptions(Number(adapter.value || 0), previous);
  if (![...frontend.options].some(option => Number(option.value) === previous)) frontend.selectedIndex = 0;
}
function satelliteChannelScanPayload() {
  return {
    id: 'satellite-channel-scan',
    name: 'Satellite channel scan',
    satellite_enabled: true,
    satellite_adapter: Number(document.getElementById('channelSatelliteAdapter')?.value || 0),
    satellite_frontend: Number(document.getElementById('channelSatelliteFrontend')?.value || 0),
    // UI uses MHz for convenience; the existing config/GStreamer contract remains kHz.
    satellite_frequency: satelliteFrequencyMhzToKhz(document.getElementById('channelSatelliteFrequency')?.value),
    satellite_symbol_rate: Number(document.getElementById('channelSatelliteSymbolRate')?.value || 0),
    satellite_polarization: document.getElementById('channelSatellitePolarization')?.value || 'H',
    satellite_delivery_system: document.getElementById('channelSatelliteDeliverySystem')?.value || 'dvb-s2',
    satellite_modulation: document.getElementById('channelSatelliteModulation')?.value || 'auto',
    satellite_fec: document.getElementById('channelSatelliteFec')?.value || 'auto',
    satellite_pilot: document.getElementById('channelSatellitePilot')?.value || 'auto',
    satellite_rolloff: document.getElementById('channelSatelliteRolloff')?.value || 'auto',
    satellite_diseqc_source: Number(document.getElementById('channelSatelliteDiseqcSource')?.value ?? -1),
    satellite_stream_id: Number(document.getElementById('channelSatelliteStreamId')?.value ?? -1),
    satellite_service_id: 0,
    satellite_lnb_lof1: Number(document.getElementById('channelSatelliteLnbLof1')?.value || 9750000),
    satellite_lnb_lof2: Number(document.getElementById('channelSatelliteLnbLof2')?.value || 10600000),
    satellite_lnb_slof: Number(document.getElementById('channelSatelliteLnbSlof')?.value || 11700000)
  };
}
function satellitePercent(raw) {
  const value = Number(raw || 0);
  if (!Number.isFinite(value) || value <= 0) return 0;
  return Math.max(0, Math.min(100, Math.round(value * 100 / 65535)));
}
function setSatelliteScanMeter(role, percent) {
  const value = clampPercent(percent);
  const label = document.querySelector(`[data-role="sat-scan-${role}-value"]`);
  const fill = document.querySelector(`[data-role="sat-scan-${role}-fill"]`);
  if (label) label.textContent = `${value}%`;
  if (fill) {
    fill.style.width = `${value}%`;
    fill.className = `dvb-meter-fill ${dvbMeterClass(value)}`;
  }
}
function renderSatelliteChannelResults() {
  const list = document.getElementById('satelliteChannelResults');
  if (!list) return;
  if (!satelliteScanServices.length) {
    list.innerHTML = '<div class="backup-library-empty">После сканирования найденные сервисы появятся здесь.</div>';
    updateSatelliteSelectionCount();
    return;
  }
  list.innerHTML = satelliteScanServices.map((service, index) => {
    const sid = Number(service.service_id || 0);
    const name = service.name || `Service ${sid}`;
    const provider = service.provider || '';
    const scrambled = !!service.scrambled;
    const video = Number(service.video_pid || 0);
    const audio = Number(service.audio_pid || 0);
    const codecs = [service.video_codec, service.audio_codec].filter(Boolean).join(' / ') || '—';
    const search = `${name} ${provider} ${sid} ${video} ${audio}`.toLowerCase();
    return `<label class="sat-channel-row" data-sat-search="${escapeHtmlValue(search)}">
      <input class="sat-channel-choice" type="checkbox" value="${sid}" data-service-index="${index}" onchange="updateSatelliteSelectionCount()" />
      <span class="sat-channel-name"><strong>${escapeHtmlValue(name)}</strong><small>${escapeHtmlValue(provider || 'Провайдер не указан')}</small></span>
      <span class="sat-channel-meta">SID ${sid}</span>
      <span><span class="sat-ca${scrambled?' scrambled':''}">${scrambled?'CA':'FTA'}</span></span>
      <span class="sat-channel-meta sat-channel-codecs">V:${video || '—'} · A:${audio || '—'} · ${escapeHtmlValue(codecs)}</span>
    </label>`;
  }).join('');
  filterSatelliteChannelResults();
  updateSatelliteSelectionCount();
}
function filterSatelliteChannelResults() {
  const query = String(document.getElementById('satelliteChannelSearch')?.value || '').trim().toLowerCase();
  document.querySelectorAll('.sat-channel-row').forEach(row => {
    row.classList.toggle('hidden', !!query && !String(row.dataset.satSearch || '').includes(query));
  });
}
function toggleAllSatelliteChannels(checked) {
  document.querySelectorAll('.sat-channel-row:not(.hidden) .sat-channel-choice').forEach(input => { input.checked = !!checked; });
  updateSatelliteSelectionCount();
}
function updateSatelliteSelectionCount() {
  const count = document.querySelectorAll('.sat-channel-choice:checked').length;
  const label = document.getElementById('satelliteSelectionCount');
  const button = document.getElementById('satelliteCreateChannelsButton');
  if (label) label.textContent = `Выбрано: ${count}`;
  if (button) {
    button.disabled = count === 0;
    button.textContent = count ? `Создать плитки (${count})` : 'Создать плитки';
  }
}
function scanSatelliteChannels() {
  const button = document.getElementById('satelliteScanButton');
  const status = document.getElementById('satelliteScanStatus');
  const payload = satelliteChannelScanPayload();
  if (payload.satellite_frequency <= 0 || payload.satellite_symbol_rate <= 0) {
    if (status) status.textContent = 'Укажите частоту и Symbol Rate.';
    return;
  }
  if (button) button.disabled = true;
  if (status) status.textContent = 'Настройка frontend и сканирование PAT / SDT / PMT…';
  satelliteScanServices = [];
  renderSatelliteChannelResults();
  setSatelliteScanMeter('signal', 0);
  setSatelliteScanMeter('quality', 0);
  fetch('/api/scan-satellite', {
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify(payload)
  }).then(async response => {
    const text = await response.text();
    let result = {};
    try { result = text ? JSON.parse(text) : {}; } catch (_) { throw new Error(text || `HTTP ${response.status}`); }
    if (!response.ok || result.result !== 'ok') throw new Error(result.error || `HTTP ${response.status}`);
    satelliteScanServices = Array.isArray(result.services) ? result.services : [];
    renderSatelliteChannelResults();
    const frontend = result.frontend_status || {};
    const signal = satellitePercent(frontend.signal_strength_raw);
    const quality = satellitePercent(frontend.snr_raw);
    setSatelliteScanMeter('signal', signal);
    setSatelliteScanMeter('quality', quality);
    if (status) status.textContent = `${result.lock ? 'LOCK' : 'без LOCK'} · найдено сервисов: ${satelliteScanServices.length}`;
  }).catch(error => {
    if (status) status.textContent = `Ошибка сканирования: ${error.message || error}`;
    satelliteScanServices = [];
    renderSatelliteChannelResults();
  }).finally(() => { if (button) button.disabled = false; });
}
function incrementIpv4Address(address, offset) {
  const parts = String(address || '').trim().split('.').map(Number);
  if (parts.length !== 4 || parts.some(part => !Number.isInteger(part) || part < 0 || part > 255)) return '';
  let value = (((parts[0] * 256 + parts[1]) * 256 + parts[2]) * 256 + parts[3]) + Number(offset || 0);
  if (value < 0 || value > 0xffffffff) return '';
  const p4 = value % 256; value = Math.floor(value / 256);
  const p3 = value % 256; value = Math.floor(value / 256);
  const p2 = value % 256; value = Math.floor(value / 256);
  const p1 = value % 256;
  return `${p1}.${p2}.${p3}.${p4}`;
}
function satelliteChannelAlreadyExists(scan, serviceId) {
  return (state.streams || []).some(stream =>
    stream.satellite_enabled === true &&
    Number(stream.satellite_adapter) === Number(scan.satellite_adapter) &&
    Number(stream.satellite_frontend) === Number(scan.satellite_frontend) &&
    Number(stream.satellite_frequency) === Number(scan.satellite_frequency) &&
    Number(stream.satellite_symbol_rate) === Number(scan.satellite_symbol_rate) &&
    String(stream.satellite_polarization || '').toUpperCase() === String(scan.satellite_polarization || '').toUpperCase() &&
    Number(stream.satellite_stream_id ?? -1) === Number(scan.satellite_stream_id ?? -1) &&
    Number(stream.satellite_service_id) === Number(serviceId));
}
function createSelectedSatelliteChannels() {
  const selected = [...document.querySelectorAll('.sat-channel-choice:checked')]
    .map(input => satelliteScanServices[Number(input.dataset.serviceIndex)])
    .filter(Boolean);
  const status = document.getElementById('satelliteCreateStatus');
  if (!selected.length) {
    if (status) status.textContent = 'Выберите хотя бы один канал.';
    return;
  }
  const scan = satelliteChannelScanPayload();
  const baseHost = String(document.getElementById('satelliteOutputHost')?.value || '239.100.1.1').trim();
  const basePort = Number(document.getElementById('satelliteOutputPort')?.value || 1234);
  const allocation = document.getElementById('satelliteOutputAllocation')?.value || 'increment-ip';
  const outputType = document.getElementById('satelliteOutputType')?.value || 'udp-cbr';
  const outputInterface = document.getElementById('satelliteOutputInterface')?.value || '';
  const targetBitrate = Math.max(1, Number(document.getElementById('satelliteOutputBitrate')?.value || 12000)) * 1000;
  const caProviderId = document.getElementById('satelliteCaProvider')?.value || '';
  if (basePort <= 0 || basePort > 65535) {
    if (status) status.textContent = 'Укажите корректный порт 1…65535.';
    return;
  }
  if (allocation === 'increment-ip' && !incrementIpv4Address(baseHost, 0)) {
    if (status) status.textContent = 'Для автоматического изменения IP укажите IPv4 адрес, например 239.100.1.1.';
    return;
  }

  const created = [];
  let skipped = 0;
  const stamp = Date.now();
  selected.forEach((service, index) => {
    const sid = Number(service.service_id || 0);
    if (!sid || satelliteChannelAlreadyExists(scan, sid)) { skipped += 1; return; }
    const host = allocation === 'increment-ip' ? incrementIpv4Address(baseHost, created.length) : baseHost;
    const port = allocation === 'increment-port' ? basePort + created.length : basePort;
    if (!host || port > 65535) { skipped += 1; return; }
    const serviceName = service.name && !String(service.name).startsWith('Service ')
      ? String(service.name)
      : `SID ${sid}`;
    created.push({
      id: `sat-${scan.satellite_adapter}-${scan.satellite_frequency}-${sid}-${stamp + index}`,
      name: serviceName,
      input_uri: '',
      input_interface_address: '',
      input_mode: 'auto',
      satellite_enabled: true,
      satellite_adapter: scan.satellite_adapter,
      satellite_frontend: scan.satellite_frontend,
      satellite_frequency: scan.satellite_frequency,
      satellite_symbol_rate: scan.satellite_symbol_rate,
      satellite_polarization: scan.satellite_polarization,
      satellite_delivery_system: scan.satellite_delivery_system,
      satellite_modulation: scan.satellite_modulation,
      satellite_fec: scan.satellite_fec,
      satellite_pilot: scan.satellite_pilot,
      satellite_rolloff: scan.satellite_rolloff,
      satellite_diseqc_source: scan.satellite_diseqc_source,
      satellite_stream_id: scan.satellite_stream_id,
      satellite_service_id: sid,
      satellite_lnb_lof1: scan.satellite_lnb_lof1,
      satellite_lnb_lof2: scan.satellite_lnb_lof2,
      satellite_lnb_slof: scan.satellite_lnb_slof,
      ca_provider_id: caProviderId,
      backup_input_uri: '',
      backup_input_type: 'url',
      backup_file_loop: false,
      output_type: outputType,
      output_mode: 'listener',
      output_host: host,
      output_port: port,
      additional_outputs: [],
      interface_address: outputInterface,
      test_pattern: false,
      auto_start: false,
      remap_enabled: false,
      cbr: outputType === 'udp-cbr',
      target_bitrate: targetBitrate,
      transcode_enabled: false,
      transcode_resolution: '1920x1080',
      transcode_video_bitrate: 6000000,
      transcode_audio_codec: 'aac',
      transcode_audio_bitrate: 192000,
      audio_pid: Number(service.audio_pid || 0),
      video_pid: Number(service.video_pid || 0),
      service_id: sid,
      service_name: serviceName,
      service_provider: String(service.provider || '')
    });
  });
  if (!created.length) {
    if (status) status.textContent = skipped ? 'Все выбранные каналы уже существуют или не удалось выделить выходные адреса.' : 'Не удалось создать каналы.';
    return;
  }

  const nextStreams = [...(state.streams || []), ...created];
  const savePayload = {
    login: state.login,
    server_name: state.server_name,
    telegram_token: state.telegram_token,
    telegram_chat_id: state.telegram_chat_id,
    http_port: state.http_port,
    language,
    ca_providers: state.ca_providers || [],
    streams: nextStreams
  };
  const button = document.getElementById('satelliteCreateChannelsButton');
  if (button) button.disabled = true;
  if (status) status.textContent = `Создание плиток: ${created.length}…`;
  fetch('/api/save-config', {method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(savePayload)})
    .then(response => {
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      state.streams = nextStreams;
      closeModal();
      fetchState();
    })
    .catch(error => {
      if (status) status.textContent = `Ошибка сохранения: ${error.message || error}`;
      if (button) button.disabled = false;
    });
}
function openSatelliteChannelModal() {
  const renderSatelliteModal = () => {
    const devices = state.dvb_devices || {adapters:[], serial_readers:[]};
    const adapters = Array.isArray(devices.adapters) ? devices.adapters : [];
    const firstAdapter = Number(adapters[0]?.adapter || 0);
    const outputInterfaces = (state.interfaces || []).map(i=>`<option value="${i.address}">${i.name} (${i.address})</option>`).join('');
    satelliteScanServices = [];
    openModal(`
      <h2>Добавить канал со спутника</h2>
      <div class="sat-setup-grid">
        <div class="form-row"><label>Adapter</label><select id="channelSatelliteAdapter" onchange="updateChannelSatelliteFrontendOptions()">${satelliteAdapterOptions(firstAdapter)}</select></div>
        <div class="form-row"><label>Frontend</label><select id="channelSatelliteFrontend">${satelliteFrontendOptions(firstAdapter, 0)}</select></div>
        <div class="form-row"><label>Система</label><select id="channelSatelliteDeliverySystem"><option value="dvb-s2">DVB-S2</option><option value="dvb-s">DVB-S</option></select></div>
        <div class="form-row"><label>Поляризация</label><select id="channelSatellitePolarization"><option value="H">Horizontal (H)</option><option value="V">Vertical (V)</option></select></div>

        <div class="form-row"><label>Частота, MHz</label><input id="channelSatelliteFrequency" type="number" min="1" max="30000" step="0.001" placeholder="11531" inputmode="decimal" /></div>
        <div class="form-row"><label>Symbol rate, kBd</label><input id="channelSatelliteSymbolRate" type="number" min="1" step="1" value="22000" /></div>
        <div class="form-row"><label>Модуляция</label><select id="channelSatelliteModulation"><option value="auto">Auto</option><option value="qpsk">QPSK</option><option value="8psk">8PSK</option><option value="16apsk">16APSK</option><option value="32apsk">32APSK</option></select></div>
        <div class="form-row"><label>FEC</label><select id="channelSatelliteFec"><option value="auto">Auto</option>${['1/2','2/3','3/4','4/5','5/6','6/7','7/8','8/9','3/5','9/10','2/5'].map(v=>`<option value="${v}">${v}</option>`).join('')}</select></div>

        <details class="sat-advanced">
          <summary>Дополнительные настройки DVB / LNB / CA</summary>
          <div class="sat-advanced-grid">
            <div class="form-row"><label>Pilot</label><select id="channelSatellitePilot"><option value="auto">Auto</option><option value="on">On</option><option value="off">Off</option></select></div>
            <div class="form-row"><label>Rolloff</label><select id="channelSatelliteRolloff"><option value="auto">Auto</option><option value="35">0.35</option><option value="25">0.25</option><option value="20">0.20</option></select></div>
            <div class="form-row"><label>DiSEqC source</label><input id="channelSatelliteDiseqcSource" type="number" min="-1" max="7" value="-1" /></div>
            <div class="form-row"><label>Stream ID / MIS</label><input id="channelSatelliteStreamId" type="number" min="-1" max="255" value="-1" /></div>
            <div class="form-row"><label>LNB LOF1, kHz</label><input id="channelSatelliteLnbLof1" type="number" value="9750000" /></div>
            <div class="form-row"><label>LNB LOF2, kHz</label><input id="channelSatelliteLnbLof2" type="number" value="10600000" /></div>
            <div class="form-row"><label>LNB switch, kHz</label><input id="channelSatelliteLnbSlof" type="number" value="11700000" /></div>
            <div class="form-row"><label>CA Provider</label><select id="satelliteCaProvider">${caProviderOptions('')}</select><small>Логический provider с общим лимитом активных каналов. Физическое устройство к каналу не привязывается.</small></div>
          </div>
        </details>

        <div class="sat-section">
          <div class="sat-toolbar"><button class="button-primary" id="satelliteScanButton" type="button" onclick="scanSatelliteChannels()">Сканировать транспондер</button><span class="sat-scan-status" id="satelliteScanStatus">Настройте транспондер и запустите поиск каналов.</span></div>
          <div class="sat-signal-grid">
            <div class="sat-signal-box"><div class="sat-signal-head"><strong>Уровень сигнала</strong><span data-role="sat-scan-signal-value">0%</span></div><div class="dvb-meter-track"><div data-role="sat-scan-signal-fill" class="dvb-meter-fill bad" style="width:0%"></div></div></div>
            <div class="sat-signal-box"><div class="sat-signal-head"><strong>Качество сигнала</strong><span data-role="sat-scan-quality-value">0%</span></div><div class="dvb-meter-track"><div data-role="sat-scan-quality-fill" class="dvb-meter-fill bad" style="width:0%"></div></div></div>
          </div>
        </div>

        <div class="sat-section">
          <div class="sat-section-title"><span>Найденные каналы</span><span id="satelliteSelectionCount">Выбрано: 0</span></div>
          <div class="sat-result-tools"><input id="satelliteChannelSearch" type="search" placeholder="Поиск: имя, провайдер, SID, PID" oninput="filterSatelliteChannelResults()" /><label class="checkbox-inline"><input type="checkbox" onchange="toggleAllSatelliteChannels(this.checked)" /><span>Выбрать видимые</span></label></div>
          <div class="sat-channel-list" id="satelliteChannelResults"><div class="backup-library-empty">После сканирования найденные сервисы появятся здесь.</div></div>
        </div>

        <div class="sat-section">
          <div class="sat-section-title"><span>Выход для создаваемых плиток</span><small>Каждый выбранный SID создаёт отдельную остановленную плитку.</small></div>
          <div class="sat-setup-grid">
            <div class="form-row"><label>Тип</label><select id="satelliteOutputType"><option value="udp-cbr">UDP MPEG-TS CBR</option><option value="udp-vbr">UDP MPEG-TS VBR</option></select></div>
            <div class="form-row"><label>Интерфейс</label><select id="satelliteOutputInterface"><option value="">Auto / все интерфейсы</option>${outputInterfaces}</select></div>
            <div class="form-row"><label>Начальный IP</label><input id="satelliteOutputHost" value="239.100.1.1" placeholder="239.100.1.1" /></div>
            <div class="form-row"><label>Порт</label><input id="satelliteOutputPort" type="number" min="1" max="65535" value="1234" /></div>
            <div class="form-row"><label>Распределение</label><select id="satelliteOutputAllocation"><option value="increment-ip">Следующий multicast IP</option><option value="increment-port">Следующий порт</option></select></div>
            <div class="form-row"><label>CBR / target, кбит/с</label><input id="satelliteOutputBitrate" type="number" min="1" value="12000" /></div>
          </div>
        </div>
      </div>
      <div class="modal-actions">
        <span id="satelliteCreateStatus" style="margin-right:auto;color:#aeb8ca"></span>
        <button class="button-secondary" onclick="closeModal()">Отмена</button>
        <button class="button-primary" id="satelliteCreateChannelsButton" onclick="createSelectedSatelliteChannels()" disabled>Создать плитки</button>
      </div>
    `);
    const modalContent = document.getElementById('modalContent');
    modalContent.classList.add('stream-modal','satellite-modal');
    updateHeaderHeight();
    document.getElementById('modal').classList.add('stream-open');
  };
  const loaders = [];
  if (!state.interfaces || !state.interfaces.length) loaders.push(loadInterfaces());
  if (!state.dvb_devices) loaders.push(loadDvbDevices());
  if (loaders.length) Promise.all(loaders).then(renderSatelliteModal);
  else renderSatelliteModal();
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
    const transcoderStatus = transcoderAvailable
      ? `Доступно: H.264 ${transcoderInfo.video_encoder || 'encoder'}, AAC ${transcoderInfo.aac_encoder || 'нет'}, MP3 ${transcoderInfo.mp3_encoder || 'нет'}, deinterlace ${transcoderInfo.deinterlace ? 'да' : 'нет'}`
      : `Недоступно: ${transcoderMissing || transcoderInfo.message || 'не установлены необходимые GStreamer-плагины'}`;
    const satelliteSummary = stream.satellite_enabled ? satelliteInputSummary(stream) : '';
    openModal(`
      <h2>${stream.name ? 'Редактирование трансляции' : 'Настройка трансляции'}</h2>
      <div class="form-grid">
        <div class="form-row full"><label>Имя плитки</label><input class="compact" id="streamName" value="${stream.name||''}" placeholder="Belarus 5" /></div>
        ${stream.satellite_enabled ? `<div class="form-row full"><label>Источник основного потока</label><div class="sat-signal-box"><strong>Спутниковый канал</strong><div style="margin-top:4px;color:#aeb8ca">${escapeHtmlValue(satelliteSummary)}</div><small>DVB-настройки и сканирование перенесены в отдельное меню «Добавить канал». Здесь сохраняются общие параметры плитки, выхода, резерва и транскодирования.</small></div></div><div class="form-row full"><label>CA Provider</label><select id="streamCaProvider">${caProviderOptions(stream.ca_provider_id || '')}</select><small>Канал использует только ID логического provider. Прямой выбор /dev/ttyUSB*, /dev/ttyACM* или DVB CA здесь отсутствует.</small></div>` : `<div class="form-row full" id="streamPrimaryUrlSettings"><div class="input-main-row"><div class="form-row"><label>Входной URL (Основной)</label><input id="streamInput" value="${stream.input_uri||''}" placeholder="rtsp://camera/live, udp://@:9087, udp://239.1.1.1:1234 или https://host/live.m3u8" /></div><div class="form-row"><label>Интерфейс входа</label><select id="streamInputInterface"><option value="">Auto / все интерфейсы</option>${inputOptions}</select></div><div class="form-row"><label>Режим входа</label><select id="streamInputMode"><option value="auto" ${(!stream.input_mode || stream.input_mode==='auto')?'selected':''}>Auto</option><option value="hls" ${stream.input_mode==='hls'?'selected':''}>HLS</option><option value="caller" ${stream.input_mode==='caller'?'selected':''}>SRT Caller</option><option value="listener" ${stream.input_mode==='listener'?'selected':''}>SRT Listener</option></select></div></div></div>`}
        <div class="form-row full"><label>Резерв / файл замены</label><div class="backup-source"><select id="streamBackupInputType" onchange="updateBackupInputMode()"><option value="url" ${(!stream.backup_input_type || stream.backup_input_type==='url')?'selected':''}>URL резерва</option><option value="file" ${stream.backup_input_type==='file'?'selected':''}>Файл замены</option></select><input id="streamBackupInput" value="${stream.backup_input_uri||''}" placeholder="http://192.168.1.2/..." /><div class="backup-library" id="streamBackupLibrary"><button class="backup-library-button" id="streamBackupLibraryButton" type="button" onclick="toggleBackupFileLibrary()">Выбрать ранее загруженный файл</button><div class="backup-library-menu" id="streamBackupLibraryMenu"></div></div><div class="backup-file-row" id="streamBackupFileRow"><input id="streamBackupFilePicker" type="file" accept="video/*,.ts,.mts,.m2ts,.mp4,.mov,.m4v" onchange="uploadBackupReplacementFile('${stream.id}', this)" /><span id="streamBackupUploadStatus"></span></div></div></div>
        <div class="form-row full" id="streamBackupFileLoopRow"><label>Зациклить файл замены</label><div class="checkbox-inline"><input id="streamBackupFileLoop" type="checkbox" ${stream.backup_file_loop ? 'checked' : ''} /><span>Повторять до появления основного потока</span></div></div>
        <div class="form-row full"><label>Тестовая таблица</label><div class="checkbox-inline"><input id="streamTestPattern" type="checkbox" ${stream.test_pattern ? 'checked' : ''} /><span>Использовать вместо входных потоков</span></div></div>
        <div class="form-row full"><label>Интерфейс вывода</label><select class="compact" id="streamInterface" onchange="syncOutputHostWithInterface()"><option value="">Auto / все интерфейсы</option>${outputOptions}</select></div>
        <div class="form-row full"><label>Выходные форматы</label><div id="streamOutputs" class="output-list">${renderOutputRows(outputs, links)}</div><button class="button-secondary" type="button" onclick="addStreamOutput()">+ Добавить формат</button></div>
        <div class="form-row full"><label>V-PID / A-PID</label><div class="row-inline compact-row"><input class="compact" id="streamAudioPid" type="number" value="${stream.audio_pid||257}" placeholder="257" /><input class="compact" id="streamVideoPid" type="number" value="${stream.video_pid||258}" placeholder="258" /></div></div>
        <div class="form-row"><label>SID</label><input class="compact" id="streamServiceId" type="number" value="${stream.service_id||1}" placeholder="1" /></div>
        <div class="form-row full"><label>Имя Канала и Провайдер</label><div class="row-inline compact-row"><input class="compact" id="streamServiceName" value="${stream.service_name||''}" placeholder="Belarus 5" /><input class="compact" id="streamProvider" value="${stream.service_provider||''}" placeholder="BTRC" /></div></div>
        <div class="form-row full"><label>Target bitrate (кбит/с)</label><input id="streamBitrate" type="number" value="${Math.round((stream.target_bitrate||2000000)/1000)}" placeholder="2000" /></div>
        <div class="form-row full"><label>Транскодирование</label><div class="checkbox-inline"><input id="streamTranscodeEnabled" type="checkbox" ${(stream.transcode_enabled && transcoderAvailable) ? 'checked' : ''} ${transcoderAvailable ? '' : 'disabled'} onchange="updateTranscodeControls()" /><span>Транскодировать видео в H.264 CBR, устранить черезстрочность и перекодировать звук</span></div><small style="color:${transcoderAvailable ? '#7ee2a8' : '#ff9f9f'}">${transcoderStatus}</small></div>
        <div class="form-row full" id="streamTranscodeControls" style="display:${(stream.transcode_enabled && transcoderAvailable)?'block':'none'}"><label>Параметры транскодирования</label><div class="row-inline compact-row"><select id="streamTranscodeResolution" onchange="applyRecommendedTranscodeBitrate()"><option value="3840x2160" ${stream.transcode_resolution==='3840x2160'?'selected':''}>3840×2160 (4K UHD)</option><option value="3200x1800" ${stream.transcode_resolution==='3200x1800'?'selected':''}>3200×1800 (3K)</option><option value="2560x1440" ${stream.transcode_resolution==='2560x1440'?'selected':''}>2560×1440 (2K QHD)</option><option value="1920x1080" ${(!stream.transcode_resolution||stream.transcode_resolution==='1920x1080')?'selected':''}>1920×1080 (Full HD)</option><option value="1280x720" ${stream.transcode_resolution==='1280x720'?'selected':''}>1280×720 (HD)</option><option value="720x576" ${stream.transcode_resolution==='720x576'?'selected':''}>720×576 (PAL SD)</option></select><input id="streamTranscodeBitrate" type="number" min="500" max="100000" step="100" value="${Math.round((stream.transcode_video_bitrate||6000000)/1000)}" placeholder="6000" /><span>кбит/с CBR</span></div><div class="row-inline compact-row" style="margin-top:8px"><select id="streamTranscodeAudioCodec" onchange="updateTranscodeAudioControls()"><option value="copy" ${stream.transcode_audio_codec==='copy'?'selected':''}>Проброс оригинальной дорожки</option><option value="aac" ${(stream.transcode_audio_codec||'aac')==='aac'?'selected':''} ${transcoderInfo.aac_encoder?'':'disabled'}>AAC-LC${transcoderInfo.aac_encoder?'':' (недоступен)'}</option><option value="mp3" ${stream.transcode_audio_codec==='mp3'?'selected':''} ${transcoderInfo.mp3_encoder?'':'disabled'}>MP3${transcoderInfo.mp3_encoder?'':' (недоступен)'}</option></select><select id="streamTranscodeAudioBitrate" ${stream.transcode_audio_codec==='copy'?'disabled':''}><option value="96000" ${(stream.transcode_audio_bitrate||192000)===96000?'selected':''}>96 кбит/с</option><option value="128000" ${(stream.transcode_audio_bitrate||192000)===128000?'selected':''}>128 кбит/с</option><option value="160000" ${(stream.transcode_audio_bitrate||192000)===160000?'selected':''}>160 кбит/с</option><option value="192000" ${(stream.transcode_audio_bitrate||192000)===192000?'selected':''}>192 кбит/с</option><option value="256000" ${(stream.transcode_audio_bitrate||192000)===256000?'selected':''}>256 кбит/с</option><option value="320000" ${(stream.transcode_audio_bitrate||192000)===320000?'selected':''}>320 кбит/с</option></select><span>аудио</span></div><small>Видео всегда преобразуется в прогрессивный режим 25p. По умолчанию: Full HD — 6000 кбит/с, звук AAC 192 кбит/с. В режиме проброса исходная аудиодорожка не перекодируется.</small></div>
        <div class="form-row full"><label>Автозапуск</label><div class="checkbox-inline"><input id="streamAutoStart" type="checkbox" ${stream.auto_start ? 'checked' : ''} /><span>Запускать после перезапуска программы</span></div></div>
        <div class="form-row full" id="streamCbrRow"><label>Включить CBR</label><div class="checkbox-inline"><input id="streamCbr" type="checkbox" ${stream.cbr ? 'checked' : ''} /><span>CBR</span></div></div>
        <div class="form-row full"><label>Включить Remap</label><div class="checkbox-inline"><input id="streamRemapEnabled" type="checkbox" ${stream.remap_enabled ? 'checked' : ''} /><span>Remap PID / Service</span></div></div>
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

  const loaders = [];
  if (!state.interfaces || !state.interfaces.length) loaders.push(loadInterfaces());
  if (loaders.length) {
    Promise.all(loaders).then(renderStreamForm);
  } else {
    renderStreamForm();
  }
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
    cbrInput.checked = primaryType === 'udp-cbr' || (!udpMode && cbrInput.checked);
    cbrInput.disabled = udpMode;
    cbrRow.style.display = udpMode ? 'none' : '';
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
    ca_providers: state.ca_providers || [],
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
  const selectedOutputType = primaryOutput.output_type;
  const selectedCbr = selectedOutputType === 'udp-cbr'
    ? true
    : (selectedOutputType === 'udp-vbr' ? false : document.getElementById('streamCbr').checked);
  const previous = (state.streams || []).find(stream => stream.id === id) || {};
  const satelliteEnabled = previous.satellite_enabled === true;
  const payload = {
    id: id,
    name: document.getElementById('streamName').value,
    input_uri: document.getElementById('streamInput')?.value ?? previous.input_uri ?? '',
    output_type: selectedOutputType,
    output_mode: primaryOutput.output_mode,
    output_host: primaryOutput.output_host,
    output_port: primaryOutput.output_port,
    additional_outputs: outputs.slice(1),
    backup_input_uri: document.getElementById('streamBackupInput').value,
    backup_input_type: document.getElementById('streamBackupInputType').value,
    backup_file_loop: document.getElementById('streamBackupInputType').value === 'file' && document.getElementById('streamBackupFileLoop').checked,
    interface_address: document.getElementById('streamInterface').value,
    input_interface_address: document.getElementById('streamInputInterface')?.value ?? previous.input_interface_address ?? '',
    input_mode: document.getElementById('streamInputMode')?.value ?? previous.input_mode ?? 'auto',
    satellite_enabled: satelliteEnabled,
    satellite_adapter: Number(previous.satellite_adapter || 0),
    satellite_frontend: Number(previous.satellite_frontend || 0),
    satellite_frequency: Number(previous.satellite_frequency || 0),
    satellite_symbol_rate: Number(previous.satellite_symbol_rate || 27500),
    satellite_polarization: previous.satellite_polarization || 'H',
    satellite_delivery_system: previous.satellite_delivery_system || 'dvb-s2',
    satellite_modulation: previous.satellite_modulation || 'auto',
    satellite_fec: previous.satellite_fec || 'auto',
    satellite_pilot: previous.satellite_pilot || 'auto',
    satellite_rolloff: previous.satellite_rolloff || 'auto',
    satellite_diseqc_source: Number(previous.satellite_diseqc_source ?? -1),
    satellite_stream_id: Number(previous.satellite_stream_id ?? -1),
    satellite_service_id: Number(previous.satellite_service_id || 0),
    satellite_lnb_lof1: Number(previous.satellite_lnb_lof1 || 9750000),
    satellite_lnb_lof2: Number(previous.satellite_lnb_lof2 || 10600000),
    satellite_lnb_slof: Number(previous.satellite_lnb_slof || 11700000),
    ca_provider_id: satelliteEnabled ? (document.getElementById('streamCaProvider')?.value || previous.ca_provider_id || '') : '',
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
    service_id: Number(document.getElementById('streamServiceId').value),
    service_name: document.getElementById('streamServiceName').value,
    service_provider: document.getElementById('streamProvider').value
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
    ca_providers: state.ca_providers || [],
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
  const stored = localStorage.getItem('tvstreamer-quality-refresh-ms');
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
  localStorage.setItem('tvstreamer-quality-refresh-ms', String(qualityChart.refreshMs));
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
  const title = `${state.server_name || 'TVStreamer5'}: Поток: ${streamName}${streamLinkText ? ` (${streamLinkText})` : ''}`;
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
function loadDvbDevices() {
  return fetch('/api/dvb-devices', {cache:'no-store'})
    .then(r=>r.json())
    .then(data=>{ state.dvb_devices=data || {adapters:[], serial_readers:[]}; return state.dvb_devices; })
    .catch(() => { state.dvb_devices={adapters:[], serial_readers:[]}; return state.dvb_devices; });
}
window.onload = () => {
  applyLanguage();
  loadInterfaces();
  loadDvbDevices();
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
