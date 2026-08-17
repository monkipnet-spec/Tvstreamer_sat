#pragma once

#include <gst/gst.h>
#include <gio/gio.h>
#include <jsoncpp/json/json.h>
#include <array>
#include <atomic>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <chrono>
#include <condition_variable>
#include <cstdint>
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
    GstPad* preallocatedVideoMuxPad = nullptr;
    GstPad* preallocatedAudioMuxPad = nullptr;
    std::string videoPadName;
    std::string audioPadName;

    ~RemapContext() {
        if (preallocatedVideoMuxPad) gst_object_unref(preallocatedVideoMuxPad);
        if (preallocatedAudioMuxPad) gst_object_unref(preallocatedAudioMuxPad);
    }
};


struct ExternalSrtOutputState {
    StreamConfig config;
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::thread busThread;
};

struct SharedDvbFrontendState {
    std::string tuningSignature;
    GstElement* pipeline = nullptr;
    GstElement* source = nullptr;
    GstBus* bus = nullptr;
    std::string multicastAddress;
    uint16_t multicastPort = 0;
    size_t consumers = 0;
    std::map<std::string, std::set<uint16_t>> consumerPids;
    std::string requestedPids;
    bool fullTsCapture = false;
    // v178: one in-process dispatcher owns TS framing and PID routing for all
    // services on this physical frontend.  Kept opaque here so the hot-path
    // implementation stays local to StreamManager.cpp.
    std::shared_ptr<void> dispatcherState;
};

struct DvbServiceRelayState {
    // v178 no longer creates a per-service GStreamer full-MPTS relay.  The
    // shared dispatcher emits the already-selected SPTS to this localhost UDP
    // port and the normal stream pipeline consumes it.
    uint16_t outputPort = 0;
    std::shared_ptr<void> dispatcherConsumer;
};

struct StreamState {
    std::atomic<bool> active{false};
    std::atomic<bool> running{false};
    bool usingBackup = false;
    bool backupAttempted = false;
    bool primaryRetryPending = false;
    bool inputLossNotified = false;
    std::string statusMessage = "stopped";
    std::string primaryInputUri;
    std::string activeInputUri;
    GstElement* pipeline = nullptr;
    GstBus* bus = nullptr;
    std::thread busThread;
    StreamConfig config;
    // runtimeConfig may point at the internal shared-DVB SPTS relay while
    // config always remains the user-visible/original stream configuration.
    StreamConfig runtimeConfig;
    bool sharedDvbInput = false;
    std::string sharedDvbFrontendKey;
    std::string sharedDvbMulticastAddress;
    uint16_t sharedDvbMulticastPort = 0;
    std::string sharedDvbServiceRelayUri;
    std::string sharedDvbServicePids;
    bool sharedDvbPreferFullTsCapture = false;
    bool dvbTsRemapApplied = false;
    std::unique_ptr<DvbServiceRelayState> dvbServiceRelay;
    // Runtime PAT result used only when input_service_id=0 (Auto).
    // The configured value remains 0; effective selection is kept in state->config.
    std::atomic<uint64_t> inputBitrate{0};
    std::atomic<uint64_t> outputBitrate{0};
    std::atomic<uint64_t> inputBytes{0};
    std::atomic<uint64_t> outputBytes{0};
    std::atomic<uint64_t> stableUdpNetworkBytes{0};
    std::atomic<bool> stableUdpNetworkTelemetry{false};
    std::atomic<uint64_t> inputCcErrors{0};
    std::atomic<uint64_t> inputCcErrorsDelta{0};
    std::atomic<uint64_t> outputCcErrors{0};
    std::atomic<uint64_t> outputCcErrorsDelta{0};
    // Passive MPEG-TS scrambling telemetry for the web tile. These counters
    // only inspect transport_scrambling_control on output packets; they never
    // carry ECM/CW/key material and do not modify the transport path.
    std::atomic<uint64_t> outputTsPayloadPackets{0};
    std::atomic<uint64_t> outputTsScrambledPackets{0};
    std::atomic<uint64_t> outputTsClearPesStarts{0};
    std::atomic<uint64_t> outputTsPayloadPacketsDelta{0};
    std::atomic<uint64_t> outputTsScrambledPacketsDelta{0};
    std::atomic<uint64_t> outputTsClearPesStartsDelta{0};
    std::chrono::steady_clock::time_point lastBitrateSample = std::chrono::steady_clock::now();
    uint64_t lastInputBytesSample = 0;
    uint64_t lastOutputBytesSample = 0;
    uint64_t lastStableUdpNetworkBytesSample = 0;
    uint64_t lastInputCcErrorsSample = 0;
    uint64_t lastOutputCcErrorsSample = 0;
    uint64_t lastOutputTsPayloadPacketsSample = 0;
    uint64_t lastOutputTsScrambledPacketsSample = 0;
    uint64_t lastOutputTsClearPesStartsSample = 0;
    uint64_t lastInputBytesSeen = 0;
    std::chrono::steady_clock::time_point lastInputActivity = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastPrimaryRetry = std::chrono::steady_clock::now();
    std::array<uint8_t, 8192> inputContinuity {};
    std::array<bool, 8192> inputContinuityValid {};
    std::vector<uint8_t> inputTsRemainder;
    std::mutex inputContinuityMutex;
    std::array<uint8_t, 8192> outputContinuity {};
    std::array<bool, 8192> outputContinuityValid {};
    std::vector<uint8_t> outputTsRemainder;
    std::mutex outputContinuityMutex;
    std::vector<uint8_t> outputScramblingRemainder;
    std::mutex outputScramblingMutex;
    // Media PID discovery for the decode indicator. Clear PSI/ECM/teletext
    // packets must never be enough to report a channel as decoded.
    uint16_t outputTelemetryPmtPid = 0x1FFF;
    std::array<bool, 8192> outputTelemetryMediaPids {};
    bool outputTelemetryMediaPidsKnown = false;
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
    bool stopStreamAsync(const std::string& id);
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
    GstElement* createOutputSink(StreamState* state, const StreamConfig& cfg, GstElement* pipeline, const std::string& sinkName);
    GstElement* createExternalSrtOutputPipeline(const StreamConfig& cfg, std::string& error);
    bool startExternalSrtOutputs(StreamState* state, std::string& error);
    void stopExternalSrtOutputs(StreamState* state);
    bool restartPipelineWithInput(StreamState* state, const std::string& inputUri, bool useBackup);
    bool restartTranscodedInput(StreamState* state, const std::string& inputUri, bool useBackup);
    bool restartActiveInput(StreamState* state, const std::string& inputUri, bool useBackup);
    bool probeInputAvailable(const StreamConfig& baseConfig, const std::string& inputUri, std::chrono::milliseconds timeout);
    bool prepareSharedDvbInput(StreamState* state, std::string& error);
    void releaseSharedDvbInput(StreamState* state);
    bool acquireSharedDvbFrontend(StreamState* state, std::string& error);
    void releaseSharedDvbFrontend(StreamState* state);
    bool startDvbServiceRelay(StreamState* state, std::string& error);
    void stopDvbServiceRelay(StreamState* state);
    bool cleanupStreamState(const std::string& id, bool notifyManualStop);
    uint16_t allocateDvbServiceRelayPort(const std::string& streamId);
    void releaseDvbServiceRelayPort(uint16_t port);
    void notifyStreamState(const StreamConfig& cfg, const std::string& color, const std::string& title, const std::string& details);
    static void onDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data);
    static void onFlvDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data);
    static void onRtspPadAdded(GstElement* src, GstPad* pad, gpointer user_data);
    void monitorBus(const std::string& id);
    void monitorExternalSrtBus(const std::string& id, size_t outputIndex);
    GstElement* createTranscodedUdpRelayPipeline(StreamState* state, std::string& error);
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
    std::map<std::string, std::unique_ptr<SharedDvbFrontendState>> sharedDvbFrontends;
    // When the last consumer stops, frontend shutdown happens on the stop
    // thread. Keep a short release barrier so a new transponder cannot race
    // the old GstDvbSrc while the kernel device is still closing.
    std::set<std::string> releasingDvbFrontends;
    std::condition_variable dvbReleaseCondition;
    std::set<uint16_t> dvbServiceRelayPorts;
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
