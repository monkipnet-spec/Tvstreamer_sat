#pragma once

#include <gst/gst.h>
#include <gio/gio.h>
#include <jsoncpp/json/json.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <vector>

#include "ConfigManager.h"
#include "GstTranscoderProcess.h"
#include "TelegramNotifier.h"
#include "utils.h"

struct RemapContext {
    GstElement* mux = nullptr;
    GstElement* sink = nullptr;
    StreamConfig config;
    bool videoLinked = false;
    bool audioLinked = false;
    bool flvMux = false;
    bool programMapApplied = false;
    std::string videoPadName;
    std::string audioPadName;
};


struct ExternalSrtOutputState {
    StreamConfig config;
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::thread busThread;
};

struct SatelliteTransponderState {
    StreamConfig tuningConfig;
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::string multicastAddress;
    uint16_t multicastPort = 0;
    size_t consumers = 0;
};

struct SatelliteServiceRelayState {
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::unique_ptr<RemapContext> context;
    uint16_t outputPort = 0;
    // Count packets that actually leave the per-service relay.  The main
    // playback pipeline has its own probes, but this counter is the most
    // reliable liveness signal for shared DVB-S/S2 input because it is
    // measured immediately before the loopback UDP sink.
    std::atomic<uint64_t> outputBytes{0};
};

struct StreamState {
    std::atomic<bool> active{false};
    std::atomic<bool> running{false};
    bool usingBackup = false;
    bool backupAttempted = false;
    bool primaryRetryPending = false;
    bool inputLossNotified = false;
    bool primarySatelliteEnabled = false;
    bool caProviderTransport = false;
    std::string caProviderId;
    std::string caProviderName;
    std::string statusMessage = "stopped";
    std::string primaryInputUri;
    std::string activeInputUri;
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::thread busThread;
    StreamConfig config;
    StreamConfig runtimeConfig;
    bool sharedSatelliteInput = false;
    std::string satelliteFrontendKey;
    std::string satelliteServiceRelayUri;
    std::unique_ptr<SatelliteServiceRelayState> satelliteServiceRelay;
    std::atomic<uint64_t> inputBitrate{0};
    std::atomic<uint64_t> outputBitrate{0};
    std::atomic<uint64_t> inputBytes{0};
    std::atomic<uint64_t> outputBytes{0};
    std::atomic<uint64_t> inputCcErrors{0};
    std::atomic<uint64_t> inputCcErrorsDelta{0};
    std::atomic<uint64_t> outputCcErrors{0};
    std::atomic<uint64_t> outputCcErrorsDelta{0};
    std::chrono::steady_clock::time_point lastBitrateSample = std::chrono::steady_clock::now();
    uint64_t lastInputBytesSample = 0;
    uint64_t lastOutputBytesSample = 0;
    uint64_t lastInputCcErrorsSample = 0;
    uint64_t lastOutputCcErrorsSample = 0;
    uint64_t lastInputBytesSeen = 0;
    uint64_t lastSatelliteRelayBytesSeen = 0;
    std::chrono::steady_clock::time_point lastInputActivity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPrimaryRetry = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastSatelliteRelayRestart = std::chrono::steady_clock::time_point{};
    uint32_t satelliteRelayRestartCount = 0;
    bool satelliteRelayRecoveryDisabled = false;
    uint32_t ccRecoveryBurstCount = 0;
    std::array<uint8_t, 8192> inputContinuity {};
    std::array<bool, 8192> inputContinuityValid {};
    std::vector<uint8_t> inputTsRemainder;
    std::mutex inputContinuityMutex;
    std::array<uint8_t, 8192> outputContinuity {};
    std::array<bool, 8192> outputContinuityValid {};
    std::vector<uint8_t> outputTsRemainder;
    std::mutex outputContinuityMutex;
    std::unique_ptr<RemapContext> sourceContext;
    std::unique_ptr<GstTranscoderProcess> gstTranscoder;
    std::vector<std::unique_ptr<ExternalSrtOutputState>> externalSrtOutputs;
    std::vector<std::unique_ptr<RemapContext>> outputContexts;
};

class StreamManager {
public:
    explicit StreamManager(ConfigManager& cfg, TelegramNotifier& notifier);
    ~StreamManager();

    bool startStream(const StreamConfig& streamConfig, std::string* error = nullptr);
    bool restartStream(const StreamConfig& streamConfig, std::string* error = nullptr);
    bool stopStream(const std::string& id);
    void stopAll();
    bool isStreamActive(const std::string& id);
    std::vector<std::string> activeStreams();
    std::map<std::string, StreamState*> snapshot();
    bool addHttpClient(const std::string& id, int fd, const std::string& clientIp);
    bool addStreamSession(const std::string& streamId, const std::string& clientIp, const std::string& protocol);
    bool removeStreamSession(const std::string& streamId, const std::string& clientIp, const std::string& protocol);
    size_t activeHttpSessions(const std::string& clientIp) const;
    size_t activeSubscriberSessions(const SubscriberConfig& subscriber);
    size_t resetHttpSessions(const std::string& clientIp);
    size_t enforceSubscriberAccess();
    size_t restartSrtOutputsForStreams(const std::vector<std::string>& streamIds);
    size_t restartAllSrtOutputs();

private:
    bool gstreamerInitialized;
    std::string buildPipelineDescription(const StreamConfig& cfg);
    GstElement* createPipeline(StreamState* state);
    GstElement* createSourceChain(StreamState* state, GstElement* pipeline, GstElement*& terminalElement);
    GstElement* createTestPatternChain(const StreamConfig& cfg, GstElement* pipeline, GstElement*& terminalElement);
    bool buildOutputBranches(StreamState* state, GstElement* pipeline, GstElement* sourceTail);
    bool buildOutputBranch(StreamState* state, GstElement* pipeline, GstElement* sourceTail, const StreamConfig& outputConfig, size_t branchIndex);
    bool buildPassthroughPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail, const StreamConfig& outputConfig, size_t branchIndex);
    bool buildRemapPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail, const StreamConfig& outputConfig, size_t branchIndex);
    bool buildRtmpOutputPipeline(StreamState* state, GstElement* pipeline, GstElement* sourceTail, const StreamConfig& outputConfig, size_t branchIndex);
    GstElement* createOutputSink(const StreamConfig& cfg, GstElement* pipeline, const std::string& sinkName);
    GstElement* createExternalSrtOutputPipeline(const StreamConfig& cfg, std::string& error);
    bool startExternalSrtOutputs(StreamState* state, std::string& error);
    void stopExternalSrtOutputs(StreamState* state);
    bool restartPipelineWithInput(StreamState* state, const std::string& inputUri, bool useBackup);
    bool probeInputAvailable(const StreamConfig& baseConfig, const std::string& inputUri, std::chrono::milliseconds timeout);
    bool prepareSharedSatelliteInput(StreamState* state, std::string& error);
    void releaseSharedSatelliteInput(StreamState* state);
    bool restartSharedSatelliteInput(StreamState* state, const std::string& reason, std::string& error);
    void throttleCaProviderStart(const std::string& providerId);
    bool acquireSatelliteTransponder(const StreamConfig& cfg, std::string& frontendKey, std::string& multicastAddress, uint16_t& multicastPort, std::string& error);
    void releaseSatelliteTransponder(const std::string& frontendKey);
    bool startSatelliteServiceRelay(StreamState* state, const std::string& multicastAddress, uint16_t multicastPort, std::string& error);
    void stopSatelliteServiceRelay(StreamState* state);
    uint16_t allocateSatelliteServiceRelayPort(const std::string& streamId);
    void releaseSatelliteServiceRelayPort(uint16_t port);
    void notifyStreamState(const StreamConfig& cfg, const std::string& color, const std::string& title, const std::string& details);
    static void onDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data);
    static void onFlvDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data);
    static void onRtspPadAdded(GstElement* src, GstPad* pad, gpointer user_data);
    void monitorBus(const std::string& id);
    void monitorExternalSrtBus(const std::string& id, size_t outputIndex);
    uint64_t queryPipelineBitrate(GstElement* pipeline);
    void attachBitrateProbes(StreamState* state);
    void updateBitrateEstimates(StreamState* state);
    static GstPadProbeReturn inputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    static GstPadProbeReturn outputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data);
    bool isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const;
    void attachSrtConnectionMonitoring(GstElement* sink, const StreamConfig& cfg);
    void pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now);
    static gboolean onSrtCallerConnecting(GstElement* sink, GSocketAddress* addr, const gchar* streamId, gpointer userData);
    static void onSrtCallerAdded(GstElement* sink, gint, GSocketAddress* addr, gpointer userData);
    static void onSrtCallerRemoved(GstElement* sink, gint, GSocketAddress* addr, gpointer userData);
    static void onSrtCallerRejected(GstElement* sink, GSocketAddress* addr, const gchar* streamId, gpointer userData);

    ConfigManager& configManager;
    TelegramNotifier& telegramNotifier;
    std::map<std::string, std::unique_ptr<StreamState>> streams;
    std::map<std::string, std::unique_ptr<SatelliteTransponderState>> satelliteTransponders;
    std::set<uint16_t> satelliteServiceRelayPorts;
    std::map<std::string, std::chrono::steady_clock::time_point> caProviderLastStart;
    struct HttpClientSession {
        std::string streamId;
        std::string clientIp;
        std::string protocol;
        std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();
    };
    std::map<int, HttpClientSession> httpClients;
    std::map<std::string, HttpClientSession> adHocSessions;
    mutable std::mutex managerMutex;
    std::atomic<uint64_t> nextSessionId{0};
    static void onHttpClientFdRemoved(GstElement* sink, gint fd, gpointer userData);
};
