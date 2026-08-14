#pragma once

#include <boost/asio.hpp>
#include <boost/beast/http.hpp>
#include <jsoncpp/json/json.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <functional>
#include <unordered_map>

#include "ConfigManager.h"
#include "StreamManager.h"

using tcp = boost::asio::ip::tcp;
namespace http = boost::beast::http;

class HttpServer {
public:
    HttpServer(boost::asio::io_context& ioc, ConfigManager& cfg, StreamManager& sm);
    bool start();
    void addEndpoint(const std::string& path, std::function<void(const boost::asio::ip::tcp::socket&)> handler);

private:
    struct QualitySample {
        int64_t timestamp = 0;
        bool active = false;
        uint64_t inputKbps = 0;
        uint64_t outputKbps = 0;
        uint64_t targetKbps = 0;
        uint64_t inputCcErrors = 0;
        uint64_t outputCcErrors = 0;
        std::string status;
        std::string level;
        std::string message;
    };

    void doAccept(std::shared_ptr<tcp::acceptor> listener, int port, uint64_t generation);
    void handleSession(tcp::socket socket);
    bool requiresAuthentication(const std::string& target) const;
    bool isAuthorized(const http::request<http::string_body>& req) const;
    bool isStreamClientAllowed(const tcp::socket& socket, const std::string& target) const;
    bool isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const;
    void writeUnauthorized(http::response<http::string_body>& res) const;
    std::set<int> configuredHttpPorts() const;
    bool bindHttpPorts(const std::set<int>& ports);
    void refreshHttpPorts();
    std::string listInterfaces();
    std::string systemMetrics();
    std::string currentState();
    std::string qualityHistory(const std::string& target);
    std::string dvbAdapters();
    std::string caManagerStatus();
    std::string handleCamClientSettings(const std::string& body);
    std::string handleDvbTune(const std::string& body, bool scan);
    std::string handleDvbAddChannels(const std::string& body);
    bool handleHttpStream(tcp::socket& socket, const std::string& target);
    bool serveHlsFile(const tcp::socket& socket, const std::string& target, http::response<http::string_body>& res);
    void handleSaveConfig(const std::string& body);
    std::string listBackupFiles();
    std::string handleUploadBackupFile(const std::string& target, const std::string& body);
    std::string handleDeleteBackupFile(const std::string& body);
    std::string handleStartStream(const std::string& body);
    std::string handleStopStream(const std::string& body);
    void handleRestartProgram();
    void handleDeleteStream(const std::string& body);
    void handleSaveSubscribers(const std::string& body);
    void handleResetSubscriber(const std::string& body);
    std::string renderIndexPage();
    void recordQualitySample(const StreamConfig& cfg, const Json::Value& state);

    boost::asio::io_context& ioContext;
    std::unordered_map<int, std::shared_ptr<tcp::acceptor>> acceptors;
    std::atomic<uint64_t> acceptGeneration{0};
    ConfigManager& configManager;
    StreamManager& streamManager;
    std::mutex qualityMutex;
    std::mutex metricsMutex;
    uint64_t previousCpuTotal = 0;
    uint64_t previousCpuIdle = 0;
    std::chrono::steady_clock::time_point previousMetricsSample;
    std::map<std::string, std::pair<uint64_t, uint64_t>> previousNetworkBytes;
    std::unordered_map<std::string, std::deque<QualitySample>> qualitySamples;
    std::unordered_map<std::string, std::function<void(const boost::asio::ip::tcp::socket&)>> endpointHandlers;
};
