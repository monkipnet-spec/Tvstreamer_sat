#include "StreamManager.h"
#include "TranscoderModule.h"
#include "UdpCbrOutput.h"
#include "UdpInput.h"
#include "UdpVbrOutput.h"
#include "protocols/GstProtocolTypes.h"
#include "protocols/GstInputProtocols.h"
#include "protocols/stream/StreamInputProtocol.h"
#include "protocols/stream/StreamOutputProtocol.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <functional>
#include <thread>
#include <vector>

#include <glib.h>
#include <gio/gio.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define GST_USE_UNSTABLE_API
#include <gst/mpegts/mpegts.h>

namespace {

constexpr guint kTsPacketSize = 188;
constexpr guint kTsPacketsPerUdpBuffer = 7;
constexpr guint64 kTsSmoothingLatency = 300 * GST_MSECOND;
constexpr guint64 kUdpQueueLatency = 10 * GST_SECOND;
constexpr auto kInputFailoverDelay = std::chrono::seconds(5);
constexpr auto kPrimaryRetryInterval = std::chrono::seconds(10);
constexpr auto kCaProviderStartSpacing = std::chrono::milliseconds(1500);
constexpr auto kSatelliteRelayRestartInterval = std::chrono::seconds(12);
constexpr uint64_t kInputCcRecoveryThreshold = 80;
constexpr auto kHlsSessionTtl = std::chrono::seconds(15);
constexpr int kSrtRestartAttempts = 4;
constexpr auto kSrtRestartRetryDelay = std::chrono::milliseconds(250);
constexpr const char* kTestPatternUri = "test://bars";

struct MulticastInterfaceSelection {
    std::string name;
    std::string address;
};

MulticastInterfaceSelection selectInternalMulticastInterface(const StreamConfig& cfg) {
    const auto interfaces = enumerateNetworkInterfaces();

    auto usable = [](const NetworkInterface& iface) {
        return !iface.name.empty() && !iface.address.empty() && iface.isUp && iface.supportsMulticast;
    };

    // Prefer an explicitly configured input interface, then the stream/output
    // interface.  Both may be either an IPv4 address or a Linux interface name.
    const std::string preferredInput = cfg.inputInterfaceAddressConfigured
        ? cfg.inputInterfaceAddress : std::string{};
    for (const auto& preferred : {preferredInput, cfg.interfaceAddress}) {
        if (preferred.empty() || preferred == "127.0.0.1" || preferred == "lo") continue;
        for (const auto& iface : interfaces) {
            if (usable(iface) && (iface.address == preferred || iface.name == preferred)) {
                return {iface.name, iface.address};
            }
        }
    }

    // Internal DVB multicast must never be pinned to loopback.  utils.cpp
    // intentionally excludes lo because Linux loopback normally has no
    // IFF_MULTICAST flag.  Pick the first usable real interface instead.
    for (const auto& iface : interfaces) {
        if (usable(iface)) {
            return {iface.name, iface.address};
        }
    }
    return {};
}

const CaProviderConfig* findCaProvider(const ConfigManager& manager, const std::string& id) {
    if (id.empty()) return nullptr;
    for (const auto& provider : manager.config.caProviders) {
        if (provider.id == id) return &provider;
    }
    return nullptr;
}

bool hasElementFactory(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) {
        return false;
    }
    gst_object_unref(factory);
    return true;
}

std::string missingElementStatus(const std::string& elementName) {
    return "missing element: " + elementName;
}

bool isHlsUri(const std::string& inputLower, const std::string& inputMode) {
    return inputLower.rfind("hls://", 0) == 0 ||
           toLower(inputMode) == "hls" ||
           inputLower.find(".m3u8") != std::string::npos;
}

bool addElementOrFail(GstElement* pipeline, GstElement* element) {
    return element != nullptr && gst_bin_add(GST_BIN(pipeline), element);
}

void drainDynamicPad(GstElement* anchor, GstPad* pad) {
    if (!anchor || !pad || gst_pad_is_linked(pad)) return;
    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(anchor));
    if (!pipeline) return;
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (!queue || !sink || !gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), sink) || !gst_element_link(queue, sink)) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
        gst_object_unref(pipeline);
        return;
    }
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    GstPad* queueSink = gst_element_get_static_pad(queue, "sink");
    if (queueSink) {
        gst_pad_link(pad, queueSink);
        gst_object_unref(queueSink);
    }
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(sink);
    gst_object_unref(pipeline);
}

std::string socketAddressToString(GSocketAddress* address) {
    if (!address) {
        return {};
    }

    auto* inetAddress = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(address));
    if (!inetAddress) {
        return {};
    }

    gchar* raw = g_inet_address_to_string(inetAddress);
    if (!raw) {
        return {};
    }
    std::string result = normalizeIpAddress(raw);
    g_free(raw);
    return result;
}

struct SrtAccessContext {
    StreamManager* manager = nullptr;
    std::string streamId;
};

void freeSrtAccessContext(gpointer data) {
    delete static_cast<SrtAccessContext*>(data);
}

bool hasProperty(GstElement* element, const char* propertyName) {
    return element && g_object_class_find_property(G_OBJECT_GET_CLASS(element), propertyName) != nullptr;
}

void setBooleanPropertyIfPresent(GstElement* element, const char* propertyName, gboolean value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setIntPropertyIfPresent(GstElement* element, const char* propertyName, gint value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setUIntPropertyIfPresent(GstElement* element, const char* propertyName, guint value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setUInt64PropertyIfPresent(GstElement* element, const char* propertyName, guint64 value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setInt64PropertyIfPresent(GstElement* element, const char* propertyName, gint64 value) {
    if (hasProperty(element, propertyName)) {
        g_object_set(element, propertyName, value, nullptr);
    }
}

void setStringPropertyIfPresent(GstElement* element, const char* propertyName, const std::string& value) {
    if (hasProperty(element, propertyName) && !value.empty()) {
        g_object_set(element, propertyName, value.c_str(), nullptr);
    }
}

void setSerializedPropertyIfPresent(GstElement* element, const char* propertyName, const std::string& value) {
    if (hasProperty(element, propertyName) && !value.empty()) {
        gst_util_set_object_arg(G_OBJECT(element), propertyName, value.c_str());
    }
}

std::string outputType(const StreamConfig& cfg) {
    std::string type = toLower(cfg.outputType);
    if (type == "udp_vbr" || type == "udpvbr") {
        type = "udp-vbr";
    } else if (type == "udp_cbr" || type == "udpcbr") {
        type = "udp-cbr";
    }

    if (type != "udp" && type != "udp-vbr" && type != "udp-cbr" &&
        type != "srt" && type != "http" && type != "hls" && type != "rtsp" && type != "rtmp" && type != "youtube") {
        type = "udp";
    }
    return type;
}

bool isUdpOutputType(const std::string& type) {
    return type == "udp" || type == "udp-vbr" || type == "udp-cbr";
}

bool isUdpOutput(const StreamConfig& cfg) {
    return isUdpOutputType(outputType(cfg));
}

bool udpCbrOutputEnabled(const StreamConfig& cfg) {
    const std::string type = outputType(cfg);
    if (type == "udp-cbr") {
        return true;
    }
    if (type == "udp-vbr") {
        return false;
    }
    return type == "udp" && cfg.cbr && cfg.targetBitrate > 0;
}

bool cbrMuxEnabled(const StreamConfig& cfg) {
    const std::string type = outputType(cfg);
    if (type == "udp-vbr") {
        return false;
    }
    if (type == "udp-cbr") {
        return cfg.targetBitrate > 0;
    }
    return cfg.cbr && cfg.targetBitrate > 0;
}

std::string srtOutputMode(const StreamConfig& cfg);

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

    const std::string type = outputType(cfg);
    if (type == "udp-cbr") {
        cfg.cbr = true;
    } else if (type == "udp-vbr") {
        cfg.cbr = false;
    }
    return cfg;
}

std::vector<StreamConfig> outputConfigs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    outputs.push_back(configForOutput(cfg, primaryOutputConfig(cfg)));
    for (const auto& output : cfg.additionalOutputs) {
        outputs.push_back(configForOutput(cfg, output));
    }
    return outputs;
}

std::vector<StreamConfig> pipelineOutputConfigs(const StreamConfig& cfg) {
    std::vector<StreamConfig> outputs;
    bool httpAdded = false;
    bool hlsAdded = false;
    for (const auto& output : outputConfigs(cfg)) {
        const std::string type = outputType(output);
        if (type == "http") {
            if (httpAdded) {
                continue;
            }
            httpAdded = true;
        } else if (type == "hls") {
            if (hlsAdded) {
                continue;
            }
            hlsAdded = true;
        }
        outputs.push_back(output);
    }
    return outputs;
}


bool hasTranscodedHttpOutput(const StreamConfig& cfg) {
    for (const auto& output : outputConfigs(cfg)) {
        if (outputType(output) == "http") {
            return true;
        }
    }
    return false;
}

int connectLocalTcpWithRetry(uint16_t port, std::string& error) {
    for (int attempt = 0; attempt < 30; ++attempt) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            error = std::string("socket failed: ") + std::strerror(errno);
            return -1;
        }

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
            ::close(fd);
            error = "inet_pton failed for 127.0.0.1";
            return -1;
        }

        if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
            return fd;
        }

        error = std::string("connect to transcoded HTTP relay tcp://127.0.0.1:") +
            std::to_string(port) + " failed: " + std::strerror(errno);
        ::close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return -1;
}

bool writeAllToFd(int fd, const char* data, size_t size) {
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = ::write(fd, data + offset, size - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) return false;
        offset += static_cast<size_t>(written);
    }
    return true;
}

std::string branchName(const std::string& base, size_t branchIndex) {
    return base + "_" + std::to_string(branchIndex);
}

bool hasSrtListenerOutput(const StreamConfig& cfg) {
    for (const auto& output : outputConfigs(cfg)) {
        if (outputType(output) == "srt" && srtOutputMode(output) == "listener") {
            return true;
        }
    }
    return false;
}

uint64_t transcodeVideoBitrateForStats(const StreamConfig& cfg) {
    return std::max<uint64_t>(cfg.transcodeVideoBitrate, 500000);
}

uint64_t transcodeAudioBitrateForStats(const StreamConfig& cfg) {
    return std::clamp<uint64_t>(cfg.transcodeAudioBitrate, 64000, 320000);
}

uint64_t transcodeInputBitrateForStats(const StreamConfig& cfg) {
    // The external GStreamer transcoder owns the input socket, so StreamManager cannot
    // attach its normal source pad probe. Keep the UI graph alive with a conservative
    // estimate derived from the configured encode rate until the transcoder is moved
    // fully in-process.
    return transcodeVideoBitrateForStats(cfg) + transcodeAudioBitrateForStats(cfg) + 300000;
}

uint64_t transcodeMuxBitrateForStats(const StreamConfig& cfg) {
    return tvs::protocols::muxBitrate(cfg);
}

uint16_t transcodeRelayPort(const StreamConfig& cfg) {
    const std::string key = cfg.id.empty() ? cfg.name : cfg.id;
    const size_t hash = std::hash<std::string>{}(key);
    return static_cast<uint16_t>(30000 + (hash % 20000));
}

StreamConfig transcodeRelayOutputConfig(const StreamConfig& cfg) {
    StreamConfig relay = cfg;
    relay.outputType = "fifo";
    relay.outputMode.clear();
    relay.outputHost = tvs::protocols::transcodedFifoRelayPath(cfg);
    relay.outputPort = 0;
    relay.additionalOutputs.clear();
    relay.cbr = true;
    relay.targetBitrate = transcodeMuxBitrateForStats(cfg);
    return relay;
}

StreamConfig transcodeRelayPipelineConfig(const StreamConfig& cfg) {
    StreamConfig relay = cfg;
    relay.inputUri = "file://" + tvs::protocols::transcodedFifoRelayPath(cfg);
    relay.inputMode = "file";
    relay.inputInterfaceAddress.clear();
    relay.inputInterfaceAddressConfigured = true;
    relay.testPattern = false;
    relay.transcodeEnabled = false;
    relay.remapEnabled = false;
    return relay;
}

uint64_t initialConfiguredOutputBitrate(const StreamConfig& cfg) {
    if (cfg.transcodeEnabled) {
        return transcodeMuxBitrateForStats(cfg);
    }

    uint64_t total = 0;
    for (const auto& output : pipelineOutputConfigs(cfg)) {
        if (cbrMuxEnabled(output) || udpCbrOutputEnabled(output)) {
            total += output.targetBitrate;
        }
    }
    return total;
}

std::vector<GstElement*> findSinksByFactory(GstElement* pipeline, const char* expectedFactory) {
    std::vector<GstElement*> result;
    if (!pipeline || !expectedFactory) {
        return result;
    }

    GstIterator* iterator = gst_bin_iterate_elements(GST_BIN(pipeline));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
        GstElement* element = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory* factory = gst_element_get_factory(element);
        const gchar* factoryName = factory
            ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
            : nullptr;
        if (factoryName && g_strcmp0(factoryName, expectedFactory) == 0) {
            result.push_back(GST_ELEMENT(gst_object_ref(element)));
        }
        g_value_unset(&item);
    }
    gst_iterator_free(iterator);
    return result;
}

std::string srtOutputMode(const StreamConfig& cfg) {
    const std::string mode = toLower(cfg.outputMode);
    return mode == "caller" ? "caller" : "listener";
}

bool isRtmpUri(const std::string& uriLower) {
    return uriLower.rfind("rtmp://", 0) == 0 ||
           uriLower.rfind("rtmps://", 0) == 0 ||
           uriLower.rfind("rtmpt://", 0) == 0 ||
           uriLower.rfind("rtmpe://", 0) == 0 ||
           uriLower.rfind("rtmpte://", 0) == 0 ||
           uriLower.rfind("rtmpts://", 0) == 0;
}

bool isLocalFileInput(const std::string& input) {
    const std::string inputLower = toLower(input);
    return inputLower.rfind("file://", 0) == 0 || input.find("://") == std::string::npos;
}

bool isBackupFileInput(const StreamConfig& cfg, const std::string& input) {
    return toLower(cfg.backupInputType) == "file" || isLocalFileInput(input);
}

std::string fileLocationFromInput(const std::string& input) {
    if (toLower(input).rfind("file://", 0) != 0) {
        return input;
    }

    gchar* location = gst_uri_get_location(input.c_str());
    if (!location) {
        return input.substr(7);
    }
    std::string result(location);
    g_free(location);
    return result;
}

bool isMpegTsFile(const std::string& input) {
    const std::filesystem::path path(fileLocationFromInput(input));
    const std::string extension = toLower(path.extension().string());
    return extension == ".ts" || extension == ".mts" || extension == ".m2ts" || extension == ".mpegts";
}

std::string hlsDirectory(const StreamConfig& cfg) {
    return "/tmp/tvstreamer5-hls/" + cfg.id;
}

std::string telegramEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (char ch : value) {
        switch (ch) {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool telegramUsesEnglish(const ConfigManager& manager) {
    return toLower(manager.config.language) == "en";
}

std::string telegramText(const ConfigManager& manager, const char* ru, const char* en) {
    return telegramUsesEnglish(manager) ? en : ru;
}

std::string displayName(const StreamConfig& cfg) {
    return cfg.name.empty() ? cfg.id : cfg.name;
}

std::string configuredInputInterfaceAddress(const StreamConfig& cfg) {
    return cfg.inputInterfaceAddressConfigured
        ? cfg.inputInterfaceAddress
        : cfg.interfaceAddress;
}

uint64_t bufferListSize(GstBufferList* list) {
    if (!list) {
        return 0;
    }

    uint64_t total = 0;
    const guint length = gst_buffer_list_length(list);
    for (guint i = 0; i < length; ++i) {
        GstBuffer* buffer = gst_buffer_list_get(list, i);
        if (buffer) {
            total += gst_buffer_get_size(buffer);
        }
    }
    return total;
}

std::size_t findTsAlignment(const guint8* data, std::size_t size) {
    if (!data || size < kTsPacketSize * 2) {
        return std::string::npos;
    }

    const std::size_t maxOffset = std::min<std::size_t>(kTsPacketSize, size);
    for (std::size_t offset = 0; offset < maxOffset; ++offset) {
        if (data[offset] != 0x47) {
            continue;
        }

        std::size_t confirmed = 1;
        for (std::size_t next = offset + kTsPacketSize;
             next < size && confirmed < 4;
             next += kTsPacketSize) {
            if (data[next] != 0x47) {
                confirmed = 0;
                break;
            }
            ++confirmed;
        }

        // Require at least two consecutive sync bytes. Three or four are
        // automatically checked when enough data is available.
        if (confirmed >= 2) {
            return offset;
        }
    }
    return std::string::npos;
}

uint64_t countContinuityErrors(
    const guint8* data,
    std::size_t size,
    std::array<uint8_t, 8192>& continuity,
    std::array<bool, 8192>& continuityValid,
    std::vector<uint8_t>& remainder,
    std::mutex& continuityMutex) {
    if (!data || size == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(continuityMutex);

    // HTTP and HLS buffers are not guaranteed to begin or end on a 188-byte
    // MPEG-TS boundary. Preserve the incomplete tail and prepend it to the
    // next buffer instead of treating skipped fragments as packet loss.
    std::vector<uint8_t> bytes;
    bytes.reserve(remainder.size() + size);
    bytes.insert(bytes.end(), remainder.begin(), remainder.end());
    bytes.insert(bytes.end(), data, data + size);
    remainder.clear();

    const std::size_t start = findTsAlignment(bytes.data(), bytes.size());
    if (start == std::string::npos) {
        // Retain only enough bytes to detect a sync sequence in the next call.
        const std::size_t keep = std::min<std::size_t>(bytes.size(), kTsPacketSize * 4 - 1);
        remainder.assign(bytes.end() - keep, bytes.end());
        return 0;
    }

    uint64_t errors = 0;
    std::size_t offset = start;
    for (; offset + kTsPacketSize <= bytes.size(); offset += kTsPacketSize) {
        const guint8* packet = bytes.data() + offset;
        if (packet[0] != 0x47) {
            // Lost alignment. Keep the remaining bytes and re-synchronize on
            // the next invocation without reporting synthetic CC errors.
            break;
        }

        if ((packet[1] & 0x80) != 0) {
            ++errors; // transport_error_indicator
        }

        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1f) << 8) | packet[2]);
        if (pid == 0x1fff) {
            continue;
        }

        const guint8 adaptationControl = static_cast<guint8>((packet[3] >> 4) & 0x03);
        if (adaptationControl == 0) {
            ++errors;
            continuityValid[pid] = false;
            continue;
        }

        bool discontinuity = false;
        if ((adaptationControl == 2 || adaptationControl == 3) && packet[4] > 0 && packet[4] <= 183) {
            discontinuity = (packet[5] & 0x80) != 0;
        }
        if (discontinuity) {
            continuityValid[pid] = false;
        }

        const bool hasPayload = adaptationControl == 1 || adaptationControl == 3;
        if (!hasPayload) {
            continue;
        }

        const guint8 continuityCounter = static_cast<guint8>(packet[3] & 0x0f);
        if (continuityValid[pid]) {
            const guint8 expected = static_cast<guint8>((continuity[pid] + 1) & 0x0f);
            if (continuityCounter != expected) {
                ++errors;
            }
        }
        continuity[pid] = continuityCounter;
        continuityValid[pid] = true;
    }

    if (offset < bytes.size()) {
        remainder.assign(bytes.begin() + offset, bytes.end());
        if (remainder.size() > kTsPacketSize * 4) {
            remainder.erase(remainder.begin(), remainder.end() - (kTsPacketSize * 4));
        }
    }
    return errors;
}

void updateInputContinuityErrors(StreamState* state, GstBuffer* buffer) {
    if (!state || !buffer) return;
    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    const uint64_t errors = countContinuityErrors(
        map.data, map.size, state->inputContinuity, state->inputContinuityValid,
        state->inputTsRemainder, state->inputContinuityMutex);
    gst_buffer_unmap(buffer, &map);
    if (errors > 0) state->inputCcErrors.fetch_add(errors, std::memory_order_relaxed);
}

void updateOutputContinuityErrors(StreamState* state, GstBuffer* buffer) {
    if (!state || !buffer) return;
    GstMapInfo map {};
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    const uint64_t errors = countContinuityErrors(
        map.data, map.size, state->outputContinuity, state->outputContinuityValid,
        state->outputTsRemainder, state->outputContinuityMutex);
    gst_buffer_unmap(buffer, &map);
    if (errors > 0) state->outputCcErrors.fetch_add(errors, std::memory_order_relaxed);
}

void updateInputContinuityErrors(StreamState* state, GstBufferList* list) {
    if (!state || !list) return;
    for (guint i = 0; i < gst_buffer_list_length(list); ++i) {
        updateInputContinuityErrors(state, gst_buffer_list_get(list, i));
    }
}

void updateOutputContinuityErrors(StreamState* state, GstBufferList* list) {
    if (!state || !list) return;
    for (guint i = 0; i < gst_buffer_list_length(list); ++i) {
        updateOutputContinuityErrors(state, gst_buffer_list_get(list, i));
    }
}

void configureSrtSink(GstElement* sink, const StreamConfig& cfg, bool accessFilteringEnabled) {
    const std::string mode = srtOutputMode(cfg);
    const bool caller = mode == "caller";
    const bool transcoded = cfg.transcodeEnabled;
    const std::string targetHost = cfg.outputHost.empty() || cfg.outputHost == "0.0.0.0" || cfg.outputHost == "::"
        ? "127.0.0.1"
        : cfg.outputHost;
    const std::string bindHost = cfg.interfaceAddress.empty() ? "0.0.0.0" : cfg.interfaceAddress;
    const std::string uri = "srt://" + (caller ? targetHost : bindHost) + ":" +
        std::to_string(cfg.outputPort) + "?mode=" + mode;

    g_object_set(sink,
        "uri", uri.c_str(),
        "sync", FALSE,
        "async", FALSE,
        "blocksize", static_cast<guint>(kTsPacketsPerUdpBuffer * 188),
        nullptr);

    setIntPropertyIfPresent(sink, "mode", caller ? 1 : 2);
    setBooleanPropertyIfPresent(sink, "authentication", accessFilteringEnabled ? TRUE : FALSE);
    setBooleanPropertyIfPresent(sink, "wait-for-connection", FALSE);
    if (!caller) {
        setBooleanPropertyIfPresent(sink, "keep-listening", TRUE);
        setUIntPropertyIfPresent(sink, "localport", static_cast<guint>(cfg.outputPort));
    }
    setBooleanPropertyIfPresent(sink, "auto-reconnect", TRUE);
    setBooleanPropertyIfPresent(sink, "qos", FALSE);
    setIntPropertyIfPresent(sink, "latency", transcoded ? 2500 : 250);
    setInt64PropertyIfPresent(sink, "max-lateness", -1);
    setStringPropertyIfPresent(sink, "localaddress", cfg.interfaceAddress);
    if (caller) {
        setUIntPropertyIfPresent(sink, "localport", 0);
    }

    if (transcoded) {
        // The external transcoder already produces and clock-paces a CBR TS.
        // Do not apply GstBaseSink max-bitrate here: targetBitrate may still
        // contain the UI default (for example 2 Mbit/s) while the transcoder
        // is producing 6+ Mbit/s. Throttling the public SRT sink below the
        // real mux bitrate creates periodic queue build-up and A/V stutter.
        setUInt64PropertyIfPresent(sink, "max-bitrate", 0);
    } else if (cfg.targetBitrate > 0) {
        setUInt64PropertyIfPresent(sink, "max-bitrate", static_cast<guint64>(cfg.targetBitrate * 12 / 10));
    }

    std::cerr << "SRT output: mode=" << mode
              << " uri=" << uri
              << " advertised=" << (cfg.outputHost.empty() ? "auto" : cfg.outputHost)
              << ":" << cfg.outputPort
              << " iface=" << (cfg.interfaceAddress.empty() ? "auto" : cfg.interfaceAddress)
              << " auth=" << (accessFilteringEnabled ? "on" : "off")
              << " transcode=" << (transcoded ? "yes" : "no")
              << " latency-ms=" << (transcoded ? 2500 : 250)
              << std::endl;
}

std::string rtmpOutputLocation(const StreamConfig& cfg) {
    const std::string type = outputType(cfg);
    const std::string host = cfg.outputHost;
    const std::string hostLower = toLower(host);

    if (isRtmpUri(hostLower)) {
        return host;
    }

    if (type == "youtube") {
        return "rtmp://a.rtmp.youtube.com/live2/" + host;
    }

    const std::string targetHost = host.empty() ? "127.0.0.1" : host;
    return "rtmp://" + targetHost + ":" + std::to_string(cfg.outputPort) + "/live/" + cfg.id;
}

void configureRtmpSink(GstElement* sink, const StreamConfig& cfg) {
    const std::string location = rtmpOutputLocation(cfg);
    g_object_set(sink,
        "location", location.c_str(),
        "sync", FALSE,
        "async", FALSE,
        "qos", FALSE,
        nullptr);
    setInt64PropertyIfPresent(sink, "max-lateness", -1);
    if (cfg.targetBitrate > 0) {
        setUInt64PropertyIfPresent(sink, "max-bitrate", static_cast<guint64>(cfg.targetBitrate * 12 / 10));
    }
}

void configureHttpSink(GstElement* sink, const StreamConfig& cfg) {
    (void)cfg;
    // multifdsink must follow the incoming buffer timestamps. With sync disabled
    // it writes every available MPEG-TS buffer immediately, producing short
    // network bursts followed by idle gaps. Timestamp-synchronised delivery
    // smooths HTTP output without modifying the transport stream itself.
    g_object_set(sink,
        "sync", TRUE,
        "async", FALSE,
        "qos", FALSE,
        nullptr);
    setInt64PropertyIfPresent(sink, "max-lateness", -1);
}

void configureHlsSink(GstElement* sink, const StreamConfig& cfg) {
    std::filesystem::create_directories(hlsDirectory(cfg));
    const std::string playlist = hlsDirectory(cfg) + "/playlist.m3u8";
    const std::string location = hlsDirectory(cfg) + "/segment%05d.ts";
    g_object_set(sink,
        "playlist-location", playlist.c_str(),
        "location", location.c_str(),
        "target-duration", 4,
        "max-files", 8,
        nullptr);
}

void configureQueue(GstElement* queue, guint64 maxSizeTime = 3000000000ULL) {
    if (!queue) {
        return;
    }

    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", maxSizeTime,
        nullptr);
}

void configureOutputQueue(GstElement* queue, const StreamConfig& cfg) {
    configureQueue(queue, isUdpOutput(cfg) ? kUdpQueueLatency : 3000000000ULL);
}

void configureTsPacketAlignment(GstElement* element) {
    setIntPropertyIfPresent(element, "alignment", static_cast<gint>(kTsPacketsPerUdpBuffer));
}

void configureCbrPacer(GstElement* pacer, const StreamConfig& cfg) {
    if (!pacer || !cfg.cbr || cfg.targetBitrate == 0) {
        return;
    }

    g_object_set(pacer,
        "sync", TRUE,
        "silent", TRUE,
        "single-segment", TRUE,
        nullptr);

    const uint64_t bytesPerSecond = cfg.targetBitrate / 8;
    if (bytesPerSecond > 0 && bytesPerSecond <= static_cast<uint64_t>(G_MAXINT)) {
        setIntPropertyIfPresent(pacer, "datarate", static_cast<gint>(bytesPerSecond));
    }
}

void linkDemuxPadToQueue(GstElement* demux, GstPad* pad, gpointer userData) {
    (void)demux;
    auto* queue = static_cast<GstElement*>(userData);
    if (!queue) {
        return;
    }

    GstPad* sinkPad = gst_element_get_static_pad(queue, "sink");
    if (!sinkPad) {
        return;
    }

    if (!gst_pad_is_linked(sinkPad)) {
        GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
        if (ret != GST_PAD_LINK_OK) {
            std::cerr << "HLS demux pad link failed: " << gst_pad_link_get_name(ret) << std::endl;
        }
    }

    gst_object_unref(sinkPad);
}

void configureTsMux(GstElement* mux, const StreamConfig& cfg) {
    g_object_set(mux,
        "alignment", static_cast<gint>(kTsPacketsPerUdpBuffer),
        "pcr-interval", 1800U,
        "pat-interval", 9000U,
        "pmt-interval", 9000U,
        "si-interval", 9000U,
        nullptr);
    if (cbrMuxEnabled(cfg)) {
        setUInt64PropertyIfPresent(mux, "bitrate", static_cast<guint64>(cfg.targetBitrate));
    }
}

void sendServiceDescription(GstElement* mux, const StreamConfig& cfg) {
    if (!mux || (cfg.serviceName.empty() && cfg.serviceProvider.empty())) {
        return;
    }

    GstMpegtsSDT* sdt = gst_mpegts_sdt_new();
    GstMpegtsSDTService* service = gst_mpegts_sdt_service_new();
    if (!sdt || !service) {
        return;
    }

    sdt->actual_ts = TRUE;
    sdt->transport_stream_id = 1;
    sdt->original_network_id = 1;

    service->service_id = static_cast<guint16>(cfg.serviceId ? cfg.serviceId : 1);
    service->EIT_schedule_flag = FALSE;
    service->EIT_present_following_flag = FALSE;
    service->running_status = GST_MPEGTS_RUNNING_STATUS_RUNNING;
    service->free_CA_mode = FALSE;

    GstMpegtsDescriptor* descriptor = gst_mpegts_descriptor_from_dvb_service(
        GST_DVB_SERVICE_DIGITAL_TELEVISION,
        cfg.serviceName.empty() ? cfg.name.c_str() : cfg.serviceName.c_str(),
        cfg.serviceProvider.empty() ? "TVStreamer5" : cfg.serviceProvider.c_str());
    if (descriptor) {
        g_ptr_array_add(service->descriptors, descriptor);
    }
    g_ptr_array_add(sdt->services, service);

    GstMpegtsSection* section = gst_mpegts_section_from_sdt(sdt);
    if (section) {
        gst_mpegts_section_send_event(section, mux);
        gst_mpegts_section_unref(section);
    }
}

GstPad* requestMuxSinkPad(GstElement* mux, uint32_t requestedPid) {
    GstPad* pad = nullptr;
    if (requestedPid > 0) {
        std::string requestedName = "sink_" + std::to_string(requestedPid);
        pad = gst_element_request_pad_simple(mux, requestedName.c_str());
    }
    if (!pad) {
        pad = gst_element_request_pad_simple(mux, "sink_%d");
    }
    return pad;
}

GstPad* requestFlvMuxSinkPad(GstElement* mux, bool isVideo) {
    return gst_element_request_pad_simple(mux, isVideo ? "video" : "audio");
}

uint32_t pidFromDemuxPadName(GstPad* pad) {
    const gchar* padName = pad ? GST_PAD_NAME(pad) : nullptr;
    if (!padName) {
        return 0;
    }

    std::string name(padName);
    const std::size_t separator = name.rfind('_');
    if (separator == std::string::npos || separator + 1 >= name.size()) {
        return 0;
    }

    try {
        const unsigned long pid = std::stoul(name.substr(separator + 1), nullptr, 16);
        return pid <= 0x1FFF ? static_cast<uint32_t>(pid) : 0;
    } catch (...) {
        return 0;
    }
}

void updateMuxProgramMap(RemapContext* ctx) {
    if (!ctx || !ctx->mux || ctx->flvMux || ctx->programMapApplied) {
        return;
    }

    // mpegtsmux does not reliably accept repeated live prog-map replacements while
    // pads are still being added. Applying a video-only map first and replacing it
    // when audio appears can leave the PMT without a stable audio entry. Wait until
    // both elementary streams are linked, then publish one immutable program map.
    if (ctx->videoPadName.empty() || ctx->audioPadName.empty()) {
        return;
    }

    // Do not install a live prog-map here. Applying it after request pads have
    // started receiving data can split video and audio into separate MPEG-TS
    // programs. The default mpegtsmux mapping places all requested elementary
    // streams in the same program, which is the desired behaviour for one channel.
    ctx->programMapApplied = true;
    std::cerr << "remap using default single-program mapping: video="
              << ctx->videoPadName << " audio=" << ctx->audioPadName << std::endl;
}

std::string parserForCaps(GstCaps* caps, const std::string& capsString) {
    const GstStructure* structure = (caps && gst_caps_get_size(caps) > 0)
        ? gst_caps_get_structure(caps, 0)
        : nullptr;
    const char* mediaType = structure ? gst_structure_get_name(structure) : nullptr;

    if (g_strcmp0(mediaType, "video/x-h264") == 0 || capsString.find("video/x-h264") != std::string::npos) {
        return "h264parse";
    }
    if (g_strcmp0(mediaType, "video/x-h265") == 0 || capsString.find("video/x-h265") != std::string::npos) {
        return "h265parse";
    }
    if (g_strcmp0(mediaType, "video/mpeg") == 0 || capsString.find("video/mpeg") != std::string::npos) {
        return "mpegvideoparse";
    }
    if (g_strcmp0(mediaType, "audio/mpeg") == 0 || capsString.find("audio/mpeg") != std::string::npos) {
        gint mpegVersion = 0;
        gint layer = 0;
        if (structure) {
            gst_structure_get_int(structure, "mpegversion", &mpegVersion);
            gst_structure_get_int(structure, "layer", &layer);
        }
        if (mpegVersion == 4 || capsString.find("mpegversion=4") != std::string::npos ||
            capsString.find("mpegversion=(int)4") != std::string::npos) {
            return "aacparse";
        }
        if (mpegVersion == 1 || layer == 3 || capsString.find("mpegversion=1") != std::string::npos ||
            capsString.find("mpegversion=(int)1") != std::string::npos) {
            return "mpegaudioparse";
        }
    }
    if (g_strcmp0(mediaType, "audio/x-ac3") == 0 || capsString.find("audio/x-ac3") != std::string::npos) {
        return "ac3parse";
    }
    if (g_strcmp0(mediaType, "audio/x-dts") == 0 || capsString.find("audio/x-dts") != std::string::npos) {
        return "dtsparse";
    }
    return "";
}

GstElement* capsFilterForMux(
    bool flvMux,
    bool isVideo,
    bool isAudio,
    const std::string& capsString,
    const std::string& parserFactory) {
    std::string capsDescription;
    if (flvMux && isVideo && capsString.find("video/x-h264") != std::string::npos) {
        capsDescription = "video/x-h264,stream-format=(string)avc";
    } else if (flvMux && isAudio && parserFactory == "aacparse") {
        capsDescription = "audio/mpeg,mpegversion=(int)4,stream-format=(string)raw";
    } else if (!flvMux && isVideo && capsString.find("video/x-h264") != std::string::npos) {
        capsDescription = "video/x-h264,stream-format=(string)byte-stream,alignment=(string)au";
    } else if (!flvMux && isAudio && parserFactory == "aacparse") {
        // Do not force raw AAC to ADTS by changing caps. A capsfilter does not add
        // ADTS headers and can produce an AAC PID that is detected but undecodable.
        // Let aacparse negotiate its real output format directly with mpegtsmux.
        capsDescription.clear();
    }

    if (capsDescription.empty()) {
        return nullptr;
    }

    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    if (!filter) {
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string(capsDescription.c_str());
    if (!caps) {
        gst_object_unref(filter);
        return nullptr;
    }
    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    return filter;
}

struct RtspPayloadFactories {
    const char* depay = nullptr;
    const char* parser = nullptr;
    const char* muxCaps = nullptr;
    bool isVideo = false;
    bool isAudio = false;
};

RtspPayloadFactories rtspPayloadFactories(const std::string& capsString) {
    const std::string capsLower = toLower(capsString);
    if (capsLower.find("media=(string)video") != std::string::npos &&
        capsLower.find("encoding-name=(string)h264") != std::string::npos) {
        return {"rtph264depay", "h264parse", "video/x-h264,stream-format=(string)byte-stream", true, false};
    }
    if (capsLower.find("media=(string)video") != std::string::npos &&
        (capsLower.find("encoding-name=(string)h265") != std::string::npos ||
         capsLower.find("encoding-name=(string)hevc") != std::string::npos)) {
        return {"rtph265depay", "h265parse", "video/x-h265,stream-format=(string)byte-stream", true, false};
    }
    if (capsLower.find("media=(string)audio") != std::string::npos &&
        (capsLower.find("encoding-name=(string)mpeg4-generic") != std::string::npos ||
         capsLower.find("encoding-name=(string)mp4a-latm") != std::string::npos)) {
        return {"rtpmp4gdepay", "aacparse", nullptr, false, true};
    }
    if (capsLower.find("media=(string)audio") != std::string::npos &&
        capsLower.find("encoding-name=(string)mpa") != std::string::npos) {
        return {"rtpmpadepay", "mpegaudioparse", nullptr, false, true};
    }
    return {};
}

GstElement* makeCapsFilter(const char* capsDescription) {
    if (!capsDescription) {
        return nullptr;
    }

    GstElement* filter = gst_element_factory_make("capsfilter", nullptr);
    GstCaps* caps = gst_caps_from_string(capsDescription);
    if (!filter || !caps) {
        if (filter) gst_object_unref(filter);
        if (caps) gst_caps_unref(caps);
        return nullptr;
    }

    g_object_set(filter, "caps", caps, nullptr);
    gst_caps_unref(caps);
    return filter;
}


bool isExternalSrtListenerOutput(const StreamConfig& outputConfig) {
    return tvs::protocols::outputKind(outputConfig) == tvs::protocols::OutputKind::Srt &&
           tvs::protocols::srtOutputMode(outputConfig) != "caller" &&
           outputConfig.outputPort > 0;
}

std::string satelliteFrontendKeyForConfig(const StreamConfig& cfg) {
    return std::to_string(cfg.satelliteAdapter) + ":" + std::to_string(cfg.satelliteFrontend);
}

bool sameSatelliteTune(const StreamConfig& left, const StreamConfig& right) {
    return left.satelliteAdapter == right.satelliteAdapter &&
           left.satelliteFrontend == right.satelliteFrontend &&
           left.satelliteFrequency == right.satelliteFrequency &&
           left.satelliteSymbolRate == right.satelliteSymbolRate &&
           toLower(left.satellitePolarization) == toLower(right.satellitePolarization) &&
           toLower(left.satelliteDeliverySystem) == toLower(right.satelliteDeliverySystem) &&
           toLower(left.satelliteModulation) == toLower(right.satelliteModulation) &&
           toLower(left.satelliteFec) == toLower(right.satelliteFec) &&
           toLower(left.satellitePilot) == toLower(right.satellitePilot) &&
           toLower(left.satelliteRolloff) == toLower(right.satelliteRolloff) &&
           left.satelliteDiseqcSource == right.satelliteDiseqcSource &&
           left.satelliteStreamId == right.satelliteStreamId &&
           left.satelliteLnbLof1 == right.satelliteLnbLof1 &&
           left.satelliteLnbLof2 == right.satelliteLnbLof2 &&
           left.satelliteLnbSlof == right.satelliteLnbSlof;
}

std::string satelliteHubMulticastAddress(const StreamConfig& cfg) {
    const unsigned slot = static_cast<unsigned>((cfg.satelliteAdapter * 16 + cfg.satelliteFrontend) % 250);
    return "239.255.250." + std::to_string(slot + 1);
}

uint16_t satelliteHubMulticastPort(const StreamConfig& cfg) {
    const unsigned slot = static_cast<unsigned>((cfg.satelliteAdapter * 16 + cfg.satelliteFrontend) % 1000);
    return static_cast<uint16_t>(45000 + slot);
}

std::string popPipelineError(GstBus* bus, const std::string& fallback) {
    if (!bus) return fallback;
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 1500 * GST_MSECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
    if (!msg) return fallback;
    std::string result = fallback;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        if (err && err->message) result = err->message;
        if (dbg && *dbg) result += std::string(" | ") + dbg;
        if (err) g_error_free(err);
        g_free(dbg);
    }
    gst_message_unref(msg);
    return result;
}

void stopPipelineAndWait(GstElement* pipeline, GstClockTime timeout = 2 * GST_SECOND) {
    if (!pipeline) return;
    const GstStateChangeReturn result = gst_element_set_state(pipeline, GST_STATE_NULL);
    if (result == GST_STATE_CHANGE_ASYNC) {
        gst_element_get_state(pipeline, nullptr, nullptr, timeout);
    }
}

} // namespace

StreamManager::StreamManager(ConfigManager& cfg, TelegramNotifier& notifier)
    : configManager(cfg), telegramNotifier(notifier), gstreamerInitialized(gst_is_initialized()) {
    std::cerr << "StreamManager constructed" << std::endl;
}

StreamManager::~StreamManager() {
    stopAll();
}

uint16_t StreamManager::allocateSatelliteServiceRelayPort(const std::string& streamId) {
    constexpr uint16_t kFirstPort = 47000;
    constexpr uint16_t kPortCount = 12000;
    const size_t hash = std::hash<std::string>{}(streamId);
    std::lock_guard<std::mutex> lock(managerMutex);
    for (uint16_t offset = 0; offset < kPortCount; ++offset) {
        const uint16_t port = static_cast<uint16_t>(kFirstPort + ((hash + offset) % kPortCount));
        if (satelliteServiceRelayPorts.insert(port).second) {
            return port;
        }
    }
    return 0;
}

void StreamManager::releaseSatelliteServiceRelayPort(uint16_t port) {
    if (!port) return;
    std::lock_guard<std::mutex> lock(managerMutex);
    satelliteServiceRelayPorts.erase(port);
}

bool StreamManager::acquireSatelliteTransponder(
    const StreamConfig& cfg,
    std::string& frontendKey,
    std::string& multicastAddress,
    uint16_t& multicastPort,
    std::string& error) {
    frontendKey = satelliteFrontendKeyForConfig(cfg);
    error.clear();

    std::lock_guard<std::mutex> lock(managerMutex);
    auto existing = satelliteTransponders.find(frontendKey);
    if (existing != satelliteTransponders.end()) {
        if (!sameSatelliteTune(existing->second->tuningConfig, cfg)) {
            const auto& active = existing->second->tuningConfig;
            std::ostringstream ss;
            ss << "DVB adapter " << cfg.satelliteAdapter << " frontend " << cfg.satelliteFrontend
               << " is already tuned to " << active.satelliteFrequency << " kHz SR "
               << active.satelliteSymbolRate << " " << active.satellitePolarization
               << "; stop those channels before tuning another transponder";
            error = ss.str();
            return false;
        }
        ++existing->second->consumers;
        multicastAddress = existing->second->multicastAddress;
        multicastPort = existing->second->multicastPort;
        std::cerr << "Shared DVB frontend reused: " << frontendKey
                  << " consumers=" << existing->second->consumers
                  << " relay=udp://@" << multicastAddress << ":" << multicastPort << std::endl;
        return true;
    }

    auto shared = std::make_unique<SatelliteTransponderState>();
    shared->tuningConfig = cfg;
    shared->multicastAddress = satelliteHubMulticastAddress(cfg);
    shared->multicastPort = satelliteHubMulticastPort(cfg);
    shared->consumers = 1;

    GstElement* pipeline = gst_pipeline_new(("dvb_shared_" + std::to_string(cfg.satelliteAdapter) + "_" + std::to_string(cfg.satelliteFrontend)).c_str());
    GstElement* source = gst_element_factory_make("dvbsrc", "shared_satellite_source");
    if (!source) {
        source = gst_element_factory_make("dvbbasebin", "shared_satellite_source");
    }
    GstElement* queue = gst_element_factory_make("queue", "shared_satellite_queue");
    GstElement* sink = gst_element_factory_make("udpsink", "shared_satellite_multicast_sink");
    if (!pipeline || !source || !queue || !sink ||
        !addElementOrFail(pipeline, source) || !addElementOrFail(pipeline, queue) || !addElementOrFail(pipeline, sink)) {
        if (pipeline) {
            stopPipelineAndWait(pipeline);
            gst_object_unref(pipeline);
        } else {
            if (source && !GST_OBJECT_PARENT(source)) gst_object_unref(source);
            if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
            if (sink && !GST_OBJECT_PARENT(sink)) gst_object_unref(sink);
        }
        error = "failed to create shared DVB frontend pipeline";
        return false;
    }

    configureQueue(queue, 12000000000ULL);
    setIntPropertyIfPresent(source, "adapter", cfg.satelliteAdapter);
    setIntPropertyIfPresent(source, "frontend", cfg.satelliteFrontend);
    setUIntPropertyIfPresent(source, "frequency", cfg.satelliteFrequency);
    setUIntPropertyIfPresent(source, "symbol-rate", cfg.satelliteSymbolRate);
    setStringPropertyIfPresent(source, "polarity", cfg.satellitePolarization);
    setSerializedPropertyIfPresent(source, "delsys", cfg.satelliteDeliverySystem);
    setSerializedPropertyIfPresent(source, "modulation", cfg.satelliteModulation);
    setSerializedPropertyIfPresent(source, "code-rate-hp", cfg.satelliteFec);
    setSerializedPropertyIfPresent(source, "pilot", cfg.satellitePilot);
    setSerializedPropertyIfPresent(source, "rolloff", cfg.satelliteRolloff);
    setIntPropertyIfPresent(source, "diseqc-source", cfg.satelliteDiseqcSource);
    setIntPropertyIfPresent(source, "stream-id", cfg.satelliteStreamId);
    setUIntPropertyIfPresent(source, "lnb-lof1", cfg.satelliteLnbLof1);
    setUIntPropertyIfPresent(source, "lnb-lof2", cfg.satelliteLnbLof2);
    setUIntPropertyIfPresent(source, "lnb-slof", cfg.satelliteLnbSlof);
    setUInt64PropertyIfPresent(source, "tuning-timeout", 10000000ULL);
    // v90: for the shared transponder hub prefer dvbsrc and request the full
    // transport stream. dvbbasebin may expose only selected program pads and on
    // some DVB-S2 services the downstream SID relay never receives TS packets.
    // PID 8192 means full TS for dvbsrc. If the property is absent this is a no-op.
    setStringPropertyIfPresent(source, "pids", "8192");

    const auto internalMulticastInterface = selectInternalMulticastInterface(cfg);
    if (internalMulticastInterface.name.empty()) {
        stopPipelineAndWait(pipeline);
        gst_object_unref(pipeline);
        error = "no active multicast-capable network interface for shared DVB hub";
        return false;
    }

    g_object_set(sink,
        "host", shared->multicastAddress.c_str(),
        "port", static_cast<gint>(shared->multicastPort),
        "sync", FALSE,
        "async", FALSE,
        nullptr);
    setStringPropertyIfPresent(sink, "multicast-iface", internalMulticastInterface.name);
    setBooleanPropertyIfPresent(sink, "auto-multicast", TRUE);
    setBooleanPropertyIfPresent(sink, "loop", TRUE);
    setBooleanPropertyIfPresent(sink, "qos", FALSE);
    setIntPropertyIfPresent(sink, "ttl-mc", 1);
    setIntPropertyIfPresent(sink, "buffer-size", 8 * 1024 * 1024);

    if (!gst_element_link_many(source, queue, sink, nullptr)) {
        stopPipelineAndWait(pipeline);
        gst_object_unref(pipeline);
        error = "failed to link shared DVB frontend pipeline";
        return false;
    }

    shared->pipeline = pipeline;
    shared->bus = gst_element_get_bus(pipeline);
    const GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        error = popPipelineError(shared->bus, "failed to start shared DVB frontend");
        stopPipelineAndWait(pipeline);
        if (shared->bus) gst_object_unref(shared->bus);
        gst_object_unref(pipeline);
        return false;
    }

    multicastAddress = shared->multicastAddress;
    multicastPort = shared->multicastPort;
    std::cerr << "Shared DVB frontend started: " << frontendKey
              << " source=" << GST_OBJECT_NAME(source)
              << " frequency_khz=" << cfg.satelliteFrequency
              << " symbol_rate_kbd=" << cfg.satelliteSymbolRate
              << " polarity=" << cfg.satellitePolarization
              << " relay=udp://@" << multicastAddress << ":" << multicastPort
              << " iface=" << internalMulticastInterface.name
              << " localaddr=" << internalMulticastInterface.address << std::endl;
    satelliteTransponders[frontendKey] = std::move(shared);
    return true;
}

void StreamManager::releaseSatelliteTransponder(const std::string& frontendKey) {
    if (frontendKey.empty()) return;
    std::unique_ptr<SatelliteTransponderState> released;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto found = satelliteTransponders.find(frontendKey);
        if (found == satelliteTransponders.end()) return;
        if (found->second->consumers > 1) {
            --found->second->consumers;
            std::cerr << "Shared DVB frontend retained: " << frontendKey
                      << " consumers=" << found->second->consumers << std::endl;
            return;
        }
        released = std::move(found->second);
        satelliteTransponders.erase(found);
    }
    if (released->pipeline) {
        stopPipelineAndWait(released->pipeline);
    }
    if (released->bus) gst_object_unref(released->bus);
    if (released->pipeline) gst_object_unref(released->pipeline);
    std::cerr << "Shared DVB frontend stopped: " << frontendKey << std::endl;
}

bool StreamManager::startSatelliteServiceRelay(
    StreamState* state,
    const std::string& multicastAddress,
    uint16_t multicastPort,
    std::string& error) {
    if (!state) {
        error = "invalid stream state for satellite service relay";
        return false;
    }
    const bool wholeTransponder = state->config.satelliteServiceId == 0;

    const uint16_t outputPort = allocateSatelliteServiceRelayPort(state->config.id);
    if (!outputPort) {
        error = "no free internal UDP port for satellite service relay";
        return false;
    }

    auto relay = std::make_unique<SatelliteServiceRelayState>();
    relay->outputPort = outputPort;
    if (!wholeTransponder) {
        relay->context = std::make_unique<RemapContext>();
        relay->context->config = state->config;
        relay->context->config.cbr = false;
        relay->context->config.targetBitrate = 0;
        relay->context->config.remapEnabled = false;
    }

    GstElement* pipeline = gst_pipeline_new(("dvb_service_" + state->config.id).c_str());
    GstElement* source = gst_element_factory_make("udpsrc", "shared_transponder_src");
    GstElement* inputQueue = gst_element_factory_make("queue", "shared_transponder_queue");
    GstElement* parse = gst_element_factory_make("tsparse", "shared_transponder_parse");
    GstElement* demux = wholeTransponder ? nullptr : gst_element_factory_make("tsdemux", "shared_service_demux");
    GstElement* mux = wholeTransponder ? nullptr : gst_element_factory_make("mpegtsmux", "shared_service_mux");
    GstElement* outputQueue = gst_element_factory_make("queue", "shared_service_queue");
    GstElement* sink = gst_element_factory_make("udpsink", "shared_service_sink");
    if (!pipeline || !source || !inputQueue || !parse || !outputQueue || !sink ||
        (!wholeTransponder && (!demux || !mux)) ||
        !addElementOrFail(pipeline, source) || !addElementOrFail(pipeline, inputQueue) ||
        !addElementOrFail(pipeline, parse) ||
        (!wholeTransponder && (!addElementOrFail(pipeline, demux) || !addElementOrFail(pipeline, mux))) ||
        !addElementOrFail(pipeline, outputQueue) || !addElementOrFail(pipeline, sink)) {
        if (pipeline) {
            stopPipelineAndWait(pipeline);
            gst_object_unref(pipeline);
        }
        releaseSatelliteServiceRelayPort(outputPort);
        error = "failed to create satellite service relay pipeline";
        return false;
    }

    GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true,packetsize=(int)188");
    g_object_set(source,
        "address", multicastAddress.c_str(),
        "port", static_cast<gint>(multicastPort),
        "reuse", TRUE,
        "auto-multicast", TRUE,
        "buffer-size", 16 * 1024 * 1024,
        "caps", caps,
        nullptr);
    if (caps) gst_caps_unref(caps);
    const auto internalMulticastInterface = selectInternalMulticastInterface(state->config);
    if (internalMulticastInterface.name.empty()) {
        stopPipelineAndWait(pipeline);
        gst_object_unref(pipeline);
        releaseSatelliteServiceRelayPort(outputPort);
        error = "no active multicast-capable network interface for satellite service relay";
        return false;
    }
    setStringPropertyIfPresent(source, "multicast-iface", internalMulticastInterface.name);
    configureQueue(inputQueue, 12000000000ULL);
    configureTsPacketAlignment(parse);
    setBooleanPropertyIfPresent(parse, "set-timestamps", FALSE);
    if (!wholeTransponder) {
        setIntPropertyIfPresent(demux, "program-number", static_cast<gint>(state->config.satelliteServiceId));
        configureTsMux(mux, relay->context->config);
        sendServiceDescription(mux, relay->context->config);
    }
    configureQueue(outputQueue, 8000000000ULL);

    // v92: measure service-relay activity at the last queue before udpsink.
    // The previous watchdog only trusted the downstream playback pipeline's
    // input probe.  On the DVB-S2 shared-input path that probe could remain at
    // zero even while tsdemux had already discovered and linked the service,
    // causing a healthy FTA relay to be restarted every five seconds.
    GstPad* relayOutputPad = gst_element_get_static_pad(outputQueue, "src");
    if (relayOutputPad) {
        gst_pad_add_probe(
            relayOutputPad,
            static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
            +[](GstPad*, GstPadProbeInfo* info, gpointer userData) -> GstPadProbeReturn {
                auto* relayState = static_cast<SatelliteServiceRelayState*>(userData);
                if (!relayState) return GST_PAD_PROBE_OK;
                if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
                    GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
                    if (buffer) {
                        relayState->outputBytes.fetch_add(
                            gst_buffer_get_size(buffer), std::memory_order_relaxed);
                    }
                } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
                    GstBufferList* list = gst_pad_probe_info_get_buffer_list(info);
                    if (list) {
                        guint n = gst_buffer_list_length(list);
                        uint64_t bytes = 0;
                        for (guint i = 0; i < n; ++i) {
                            GstBuffer* buffer = gst_buffer_list_get(list, i);
                            if (buffer) bytes += gst_buffer_get_size(buffer);
                        }
                        relayState->outputBytes.fetch_add(bytes, std::memory_order_relaxed);
                    }
                }
                return GST_PAD_PROBE_OK;
            },
            relay.get(),
            nullptr);
        gst_object_unref(relayOutputPad);
    }

    g_object_set(sink,
        "host", "127.0.0.1",
        "port", static_cast<gint>(outputPort),
        "sync", FALSE,
        "async", FALSE,
        nullptr);
    setBooleanPropertyIfPresent(sink, "qos", FALSE);
    setIntPropertyIfPresent(sink, "buffer-size", 8 * 1024 * 1024);

    const bool linked = wholeTransponder
        ? gst_element_link_many(source, inputQueue, parse, outputQueue, sink, nullptr)
        : (gst_element_link_many(source, inputQueue, parse, demux, nullptr) &&
           gst_element_link_many(mux, outputQueue, sink, nullptr));
    if (!linked) {
        stopPipelineAndWait(pipeline);
        gst_object_unref(pipeline);
        releaseSatelliteServiceRelayPort(outputPort);
        error = "failed to link satellite service relay pipeline";
        return false;
    }

    if (!wholeTransponder) {
        relay->context->mux = mux;
        relay->context->sink = sink;
        relay->context->flvMux = false;
        g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), relay->context.get());
    }
    relay->pipeline = pipeline;
    relay->bus = gst_element_get_bus(pipeline);

    const GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        error = popPipelineError(relay->bus, "failed to start satellite service relay");
        stopPipelineAndWait(pipeline);
        if (relay->bus) gst_object_unref(relay->bus);
        gst_object_unref(pipeline);
        releaseSatelliteServiceRelayPort(outputPort);
        return false;
    }

    state->satelliteServiceRelayUri = "udp://127.0.0.1:" + std::to_string(outputPort);
    state->satelliteServiceRelay = std::move(relay);
    std::cerr << "Satellite service relay started: stream=" << state->config.id
              << " SID=" << state->config.satelliteServiceId
              << " transponder=udp://@" << multicastAddress << ":" << multicastPort
              << " service=" << state->satelliteServiceRelayUri << std::endl;
    return true;
}

void StreamManager::stopSatelliteServiceRelay(StreamState* state) {
    if (!state || !state->satelliteServiceRelay) return;
    auto relay = std::move(state->satelliteServiceRelay);
    if (relay->pipeline) {
        stopPipelineAndWait(relay->pipeline);
    }
    if (relay->bus) gst_object_unref(relay->bus);
    if (relay->pipeline) gst_object_unref(relay->pipeline);
    releaseSatelliteServiceRelayPort(relay->outputPort);
}

bool StreamManager::prepareSharedSatelliteInput(StreamState* state, std::string& error) {
    if (!state) {
        error = "invalid stream state";
        return false;
    }
    state->runtimeConfig = state->config;
    if (!state->config.satelliteEnabled || state->config.testPattern) {
        return true;
    }

    std::string multicastAddress;
    uint16_t multicastPort = 0;
    if (!acquireSatelliteTransponder(
            state->config, state->satelliteFrontendKey, multicastAddress, multicastPort, error)) {
        return false;
    }

    // v95: FTA channels must not open the DVB frontend a second time.  The
    // scanner/shared hub has already proven that adapter/frontend can be tuned
    // successfully.  Feed the full transponder multicast directly into the
    // channel's main pipeline and let the v94 single-service tsdemux/remux pick
    // the configured SID there.  This removes both failure modes seen in v94:
    //   1) dvbbasebin/dvbsrc "Failed to start" because the frontend is busy;
    //   2) the old per-service loopback relay watchdog/restart loop.
    // Scrambled services keep the per-service relay path for now because their
    // CA backend may need a dedicated service branch later.
    if (!state->config.satelliteScrambled) {
        state->sharedSatelliteInput = true;
        state->lastSatelliteRelayBytesSeen = 0;
        state->satelliteServiceRelayUri =
            "udp://@" + multicastAddress + ":" + std::to_string(multicastPort);
        state->runtimeConfig = state->config;
        state->runtimeConfig.satelliteEnabled = false;
        state->runtimeConfig.inputUri = state->satelliteServiceRelayUri;
        state->runtimeConfig.inputMode = "udp";
        // v96: join the internal multicast on the same real multicast-capable
        // interface used by the shared DVB hub. Never force 127.0.0.1/lo here:
        // Linux loopback is not advertised by enumerateNetworkInterfaces() as a
        // multicast-capable interface, which caused the v95 startup failure.
        const auto internalMulticastInterface = selectInternalMulticastInterface(state->config);
        if (internalMulticastInterface.address.empty()) {
            releaseSatelliteTransponder(state->satelliteFrontendKey);
            state->satelliteFrontendKey.clear();
            error = "no active multicast-capable network interface for FTA shared DVB input";
            return false;
        }
        state->runtimeConfig.inputInterfaceAddress = internalMulticastInterface.address;
        state->runtimeConfig.inputInterfaceAddressConfigured = true;
        std::cerr << "FTA shared-transponder input attached: stream=" << state->config.id
                  << " frontend=" << state->satelliteFrontendKey
                  << " SID=" << state->config.satelliteServiceId
                  << " source=" << state->satelliteServiceRelayUri
                  << " iface=" << internalMulticastInterface.name
                  << " localaddr=" << internalMulticastInterface.address << std::endl;
        return true;
    }

    if (!startSatelliteServiceRelay(state, multicastAddress, multicastPort, error)) {
        releaseSatelliteTransponder(state->satelliteFrontendKey);
        state->satelliteFrontendKey.clear();
        return false;
    }

    state->sharedSatelliteInput = true;
    state->lastSatelliteRelayBytesSeen = 0;
    state->runtimeConfig = state->config;
    state->runtimeConfig.satelliteEnabled = false;
    state->runtimeConfig.inputUri = state->satelliteServiceRelayUri;
    state->runtimeConfig.inputMode = "udp";
    state->runtimeConfig.inputInterfaceAddress.clear();
    state->runtimeConfig.inputInterfaceAddressConfigured = true;
    std::cerr << "Satellite shared-input attached: stream=" << state->config.id
              << " frontend=" << state->satelliteFrontendKey
              << " SID=" << state->config.satelliteServiceId
              << " source=" << state->satelliteServiceRelayUri << std::endl;
    return true;
}

void StreamManager::releaseSharedSatelliteInput(StreamState* state) {
    if (!state) return;
    stopSatelliteServiceRelay(state);
    if (!state->satelliteFrontendKey.empty()) {
        releaseSatelliteTransponder(state->satelliteFrontendKey);
    }
    state->satelliteFrontendKey.clear();
    state->satelliteServiceRelayUri.clear();
    state->sharedSatelliteInput = false;
}

void StreamManager::throttleCaProviderStart(const std::string& providerId) {
    if (providerId.empty()) {
        return;
    }

    std::chrono::milliseconds delay{0};
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        const auto now = std::chrono::steady_clock::now();
        auto& last = caProviderLastStart[providerId];
        if (last.time_since_epoch().count() != 0) {
            const auto nextAllowed = last + kCaProviderStartSpacing;
            if (now < nextAllowed) {
                delay = std::chrono::duration_cast<std::chrono::milliseconds>(nextAllowed - now);
                last = nextAllowed;
            } else {
                last = now;
            }
        } else {
            last = now;
        }
    }

    if (delay.count() > 0) {
        std::cerr << "CA provider start throttle: provider=" << providerId
                  << " delay_ms=" << delay.count() << std::endl;
        std::this_thread::sleep_for(delay);
    }
}

bool StreamManager::restartSharedSatelliteInput(StreamState* state, const std::string& reason, std::string& error) {
    if (!state || !state->config.satelliteEnabled || state->caProviderTransport) {
        error = "not a shared DVB satellite input";
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (state->lastSatelliteRelayRestart.time_since_epoch().count() != 0 &&
        now - state->lastSatelliteRelayRestart < kSatelliteRelayRestartInterval) {
        error = "satellite relay restart throttled";
        return false;
    }
    state->lastSatelliteRelayRestart = now;
    ++state->satelliteRelayRestartCount;

    std::cerr << "Restarting shared satellite input: stream=" << state->config.id
              << " SID=" << state->config.satelliteServiceId
              << " reason=" << reason
              << " attempt=" << state->satelliteRelayRestartCount << std::endl;

    // v90: do not switch to a second direct DVB pipeline on the same frontend.
    // For this architecture the reliable path is: dvbsrc full transponder hub
    // -> per-service relay. If the selected SID still produces no packets after
    // one relay rebuild, stop recovery attempts and report the real problem
    // instead of entering a GStreamer teardown/restart crash loop.
    if (state->satelliteRelayRestartCount > 1) {
        error = "satellite service relay has no packets for SID " + std::to_string(state->config.satelliteServiceId) +
                "; rescan transponder or verify service_id/video_pid/audio_pid";
        state->statusMessage = error;
        state->satelliteRelayRecoveryDisabled = true;
        std::cerr << "Satellite relay recovery disabled after repeated no-input: stream="
                  << state->config.id << " SID=" << state->config.satelliteServiceId << std::endl;
        return false;
    }

    releaseSharedSatelliteInput(state);
    std::string prepareError;
    if (!prepareSharedSatelliteInput(state, prepareError)) {
        error = prepareError.empty() ? "failed to prepare shared satellite input" : prepareError;
        state->statusMessage = "satellite relay recovery failed: " + error;
        return false;
    }

    if (!restartPipelineWithInput(state, state->satelliteServiceRelayUri, false)) {
        error = "failed to restart pipeline after satellite relay recovery";
        state->statusMessage = "satellite relay recovery failed: " + error;
        return false;
    }

    state->inputLossNotified = false;
    state->primaryRetryPending = false;
    state->statusMessage = "running after satellite relay recovery";
    return true;
}

void StreamManager::attachSrtConnectionMonitoring(GstElement* sink, const StreamConfig& cfg) {
    if (!sink) {
        return;
    }

    auto* ctx = new SrtAccessContext{this, cfg.id};
    g_object_set_data_full(G_OBJECT(sink), "srt-access-context", ctx, freeSrtAccessContext);

    if (configManager.subscribers.filteringEnabled) {
        g_signal_connect_data(
            sink, "caller-connecting", G_CALLBACK(StreamManager::onSrtCallerConnecting),
            ctx, nullptr, static_cast<GConnectFlags>(0));
        g_signal_connect_data(
            sink, "caller-rejected", G_CALLBACK(StreamManager::onSrtCallerRejected),
            ctx, nullptr, static_cast<GConnectFlags>(0));
    }

    g_signal_connect_data(
        sink, "caller-added", G_CALLBACK(StreamManager::onSrtCallerAdded),
        ctx, nullptr, static_cast<GConnectFlags>(0));
    g_signal_connect_data(
        sink, "caller-removed", G_CALLBACK(StreamManager::onSrtCallerRemoved),
        ctx, nullptr, static_cast<GConnectFlags>(0));

    std::cerr << "SRT connection monitoring attached for stream " << cfg.id
              << " port=" << cfg.outputPort
              << " filtering=" << (configManager.subscribers.filteringEnabled ? "on" : "off")
              << std::endl;
}

GstElement* StreamManager::createExternalSrtOutputPipeline(const StreamConfig& cfg, std::string& error) {
    error.clear();
    const uint16_t relayPort = tvs::protocols::transcodedSrtInternalPort(cfg);

    for (const char* factory : {"udpsrc", "queue", "tsparse"}) {
        if (!hasElementFactory(factory)) {
            error = missingElementStatus(factory);
            return nullptr;
        }
    }

    GstElement* pipeline = gst_pipeline_new(nullptr);
    GstElement* src = gst_element_factory_make("udpsrc", nullptr);
    GstElement* inputQueue = gst_element_factory_make("queue", nullptr);
    GstElement* tsparse = gst_element_factory_make("tsparse", nullptr);
    GstElement* outputQueue = gst_element_factory_make("queue", nullptr);

    if (!pipeline || !src || !inputQueue || !tsparse || !outputQueue) {
        error = "failed to create transcoded SRT relay elements";
        if (pipeline) gst_object_unref(pipeline);
        if (src) gst_object_unref(src);
        if (inputQueue) gst_object_unref(inputQueue);
        if (tsparse) gst_object_unref(tsparse);
        if (outputQueue) gst_object_unref(outputQueue);
        return nullptr;
    }

    gst_bin_add_many(GST_BIN(pipeline), src, inputQueue, tsparse, outputQueue, nullptr);

    // Use the exact same SRT sink constructor as a non-transcoded stream.
    // This is important for subscriber monitoring: caller-connecting,
    // caller-added, caller-removed and caller-rejected are connected in one
    // common place and feed the same active-session table.
    GstElement* sink = createOutputSink(cfg, pipeline, "transcoded_srt_sink");
    if (!sink) {
        error = "failed to create monitored SRT output sink";
        gst_object_unref(pipeline);
        return nullptr;
    }

    g_object_set(src,
        "address", "127.0.0.1",
        "port", static_cast<gint>(relayPort),
        "reuse", TRUE,
        "auto-multicast", FALSE,
        // The external CBR pacer controls packet timing. Timestamp buffers at
        // their loopback arrival time instead of rebuilding PCR timestamps a
        // second time in the in-process SRT relay.
        "do-timestamp", TRUE,
        "buffer-size", 16 * 1024 * 1024,
        nullptr);
    GstCaps* caps = gst_caps_from_string("video/mpegts,systemstream=(boolean)true");
    if (caps) {
        g_object_set(src, "caps", caps, nullptr);
        gst_caps_unref(caps);
    }

    g_object_set(inputQueue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", static_cast<guint64>(5000000000ULL),
        nullptr);
    // The external path already performs PCR smoothing and CBR pacing. A
    // second set-timestamps/smoothing stage here periodically corrects the
    // clock against loopback arrival time and can produce regular SRT pauses.
    setBooleanPropertyIfPresent(tsparse, "set-timestamps", FALSE);
    setIntPropertyIfPresent(tsparse, "alignment", 7);
    g_object_set(outputQueue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", static_cast<guint64>(5000000000ULL),
        nullptr);

    if (!gst_element_link_many(src, inputQueue, tsparse, outputQueue, sink, nullptr)) {
        error = "failed to link transcoded SRT relay pipeline";
        gst_object_unref(pipeline);
        return nullptr;
    }

    std::cerr << "Transcoded SRT output relay: stream=" << cfg.id
              << " udp=127.0.0.1:" << relayPort
              << " -> srt=" << (cfg.outputHost.empty() ? "auto" : cfg.outputHost)
              << ":" << cfg.outputPort
              << " mode=" << tvs::protocols::srtOutputMode(cfg)
              << " monitoring=direct-callbacks"
              << std::endl;
    return pipeline;
}

bool StreamManager::startExternalSrtOutputs(StreamState* state, std::string& error) {
    if (!state) {
        return true;
    }
    error.clear();

    for (const auto& outputConfig : tvs::protocols::outputConfigs(state->config)) {
        if (!isExternalSrtListenerOutput(outputConfig)) {
            continue;
        }

        auto output = std::make_unique<ExternalSrtOutputState>();
        output->config = outputConfig;
        output->pipeline = createExternalSrtOutputPipeline(outputConfig, error);
        if (!output->pipeline) {
            stopExternalSrtOutputs(state);
            if (error.empty()) error = "failed to create transcoded SRT output relay";
            return false;
        }

        output->bus = gst_element_get_bus(output->pipeline);
        const GstStateChangeReturn stateChange = gst_element_set_state(output->pipeline, GST_STATE_PLAYING);
        if (stateChange == GST_STATE_CHANGE_FAILURE) {
            if (output->bus) {
                GstMessage* msg = gst_bus_timed_pop_filtered(
                    output->bus,
                    2 * GST_SECOND,
                    static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
                if (msg) {
                    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                        GError* err = nullptr;
                        gchar* dbg = nullptr;
                        gst_message_parse_error(msg, &err, &dbg);
                        if (err && err->message) error = err->message;
                        if (dbg && *dbg) error += std::string(" | ") + dbg;
                        if (err) g_error_free(err);
                        g_free(dbg);
                    }
                    gst_message_unref(msg);
                }
            }
            if (error.empty()) error = "failed to start transcoded SRT output relay";
            if (output->pipeline) {
                stopPipelineAndWait(output->pipeline);
                gst_object_unref(output->pipeline);
                output->pipeline = nullptr;
            }
            if (output->bus) {
                gst_object_unref(output->bus);
                output->bus = nullptr;
            }
            stopExternalSrtOutputs(state);
            return false;
        }

        state->externalSrtOutputs.push_back(std::move(output));
    }
    return true;
}

void StreamManager::stopExternalSrtOutputs(StreamState* state) {
    if (!state) {
        return;
    }
    for (auto& output : state->externalSrtOutputs) {
        if (!output) continue;
        if (output->bus) {
            gst_bus_set_flushing(output->bus, TRUE);
        }
        if (output->busThread.joinable()) {
            output->busThread.join();
        }
        if (output->pipeline) {
            stopPipelineAndWait(output->pipeline);
        }
        if (output->bus) {
            gst_object_unref(output->bus);
            output->bus = nullptr;
        }
        if (output->pipeline) {
            gst_object_unref(output->pipeline);
            output->pipeline = nullptr;
        }
    }
    state->externalSrtOutputs.clear();
}

void StreamManager::monitorExternalSrtBus(const std::string& id, size_t outputIndex) {
    (void)id;
    (void)outputIndex;
}

bool StreamManager::startStream(const StreamConfig& streamConfig, std::string* error) {
    if (error) error->clear();
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        if (streams.count(streamConfig.id)) {
            if (error) *error = "stream is already active: " + streamConfig.id;
            return false;
        }

        if (!gstreamerInitialized) {
            gst_init(nullptr, nullptr);
            gstreamerInitialized = true;
        }
    }

    auto state = std::make_unique<StreamState>();
    state->config = streamConfig;
    state->runtimeConfig = streamConfig;
    state->primarySatelliteEnabled = streamConfig.satelliteEnabled;

    const CaProviderConfig* caProvider = (streamConfig.satelliteEnabled && streamConfig.satelliteScrambled)
        ? findCaProvider(configManager, streamConfig.caProviderId)
        : nullptr;
    if (caProvider) {
        // v85+ CA Provider is a dynamic reader/card/session manager. It no longer replaces the
        // satellite input with a pre-decoded endpoint. The MPEG-TS source remains the configured
        // DVB frontend; reader/card capability integrations can be attached independently.
        state->caProviderId = caProvider->id;
        state->caProviderName = caProvider->name;
        throttleCaProviderStart(caProvider->id);
    }
    state->primaryInputUri = streamConfig.satelliteEnabled
        ? tvs::protocols::inputUriForGstreamer(streamConfig)
        : streamConfig.inputUri;
    state->activeInputUri = streamConfig.testPattern
        ? kTestPatternUri
        : state->primaryInputUri;
    state->sourceContext = std::make_unique<RemapContext>();
    state->sourceContext->config = state->runtimeConfig;

    // v95: every satellite channel acquires the shared tuned transponder first.
    // FTA then reads that hub multicast directly and performs the v94 SID filter
    // inside the main output pipeline.  This guarantees that only one GStreamer
    // DVB source owns /dev/dvb/adapterN/frontendM at a time.
    const bool useSharedSatelliteInput = streamConfig.satelliteEnabled;

    std::string sharedSatelliteError;
    if (!state->caProviderTransport && useSharedSatelliteInput &&
        !prepareSharedSatelliteInput(state.get(), sharedSatelliteError)) {
        state->statusMessage = "satellite shared input failed: " + sharedSatelliteError;
        if (error) *error = sharedSatelliteError;
        return false;
    }
    if (streamConfig.satelliteEnabled && !streamConfig.satelliteScrambled) {
        std::cerr << "FTA shared DVB hub selected: stream=" << streamConfig.id
                  << " adapter=" << streamConfig.satelliteAdapter
                  << " frontend=" << streamConfig.satelliteFrontend
                  << " SID=" << streamConfig.satelliteServiceId
                  << " frequency_khz=" << streamConfig.satelliteFrequency
                  << " source=" << state->satelliteServiceRelayUri
                  << std::endl;
        state->statusMessage = "starting FTA via shared DVB-S/S2 transponder hub";
    }

    if (streamConfig.transcodeEnabled && GstTranscoderProcess::isAvailable()) {
        std::string srtRelayError;
        if (!startExternalSrtOutputs(state.get(), srtRelayError)) {
            std::cerr << "Transcoded SRT output setup failed for " << streamConfig.id
                      << ": " << srtRelayError << std::endl;
            state->statusMessage = "transcoded srt output failed: " + srtRelayError;
            releaseSharedSatelliteInput(state.get());
            if (error) *error = srtRelayError.empty() ? "failed to start transcoded SRT output" : srtRelayError;
            return false;
        }

        auto gstTranscoder = std::make_unique<GstTranscoderProcess>();
        std::string gstError;
        if (!gstTranscoder->start(state->runtimeConfig, gstError)) {
            std::cerr << "GStreamer transcoder setup failed for " << streamConfig.id
                      << ": " << gstError << std::endl;
            state->statusMessage = "gstreamer transcoder failed: " + gstError;
            stopExternalSrtOutputs(state.get());
            releaseSharedSatelliteInput(state.get());
            if (error) *error = gstError.empty() ? "GStreamer transcoder failed to start" : gstError;
            return false;
        }

        std::cerr << "Pipeline for stream '" << streamConfig.name
                  << "': gstreamer-transcoder input=" << state->primaryInputUri
                  << " transcode=" << streamConfig.transcodeResolution
                  << "@" << streamConfig.transcodeVideoBitrate
                  << " outputs=" << gstTranscoder->description() << std::endl;

        state->gstTranscoder = std::move(gstTranscoder);
        state->running = true;
        state->active = true;
        state->statusMessage = "running via gstreamer";
        state->outputBitrate = initialConfiguredOutputBitrate(streamConfig);
        state->inputBitrate = transcodeInputBitrateForStats(streamConfig);
        state->lastInputActivity = std::chrono::steady_clock::now();
        state->lastPrimaryRetry = state->lastInputActivity;
        state->lastBitrateSample = state->lastInputActivity;
        const std::string notificationInputUri = state->primaryInputUri;

        bool duplicateStart = false;
        {
            std::lock_guard<std::mutex> lock(managerMutex);
            if (streams.count(streamConfig.id)) {
                duplicateStart = true;
            } else {
                streams[streamConfig.id] = std::move(state);
                streams[streamConfig.id]->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);
            }
        }
        if (duplicateStart) {
            if (state) {
                stopExternalSrtOutputs(state.get());
                if (state->gstTranscoder) {
                    state->gstTranscoder->stop();
                }
                releaseSharedSatelliteInput(state.get());
            }
            if (error) *error = "duplicate stream start detected: " + streamConfig.id;
            return false;
        }
        notifyStreamState(
            streamConfig,
            "🟢",
            telegramText(configManager, "Поток запущен", "Stream started"),
            telegramText(configManager, "GStreamer-транскодер", "GStreamer transcoder") + "\nURL: " + notificationInputUri);
        return true;
    }

    GstElement* pipeline = createPipeline(state.get());
    if (!pipeline) {
        state->statusMessage = "pipeline build failed";
        releaseSharedSatelliteInput(state.get());
        if (error) *error = "failed to build GStreamer pipeline for stream: " + streamConfig.name;
        return false;
    }

    std::cerr << "Pipeline for stream '" << streamConfig.name
              << "': " << buildPipelineDescription(state->runtimeConfig) << std::endl;

    state->pipeline = pipeline;
    state->bus = gst_element_get_bus(pipeline);
    state->running = true;
    state->active = true;
    state->statusMessage = "starting";
    state->outputBitrate = initialConfiguredOutputBitrate(streamConfig);
    state->lastInputActivity = std::chrono::steady_clock::now();
    state->lastPrimaryRetry = state->lastInputActivity;
    state->lastBitrateSample = state->lastInputActivity;
    state->ccRecoveryBurstCount = 0;
    attachBitrateProbes(state.get());

    GstStateChangeReturn stateChange = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateChange == GST_STATE_CHANGE_ASYNC) {
        const GstStateChangeReturn settled = gst_element_get_state(
            pipeline, nullptr, nullptr, 1200 * GST_MSECOND);
        if (settled == GST_STATE_CHANGE_FAILURE) {
            stateChange = GST_STATE_CHANGE_FAILURE;
        } else if (settled == GST_STATE_CHANGE_SUCCESS || settled == GST_STATE_CHANGE_NO_PREROLL) {
            stateChange = settled;
        }
    }
    if (stateChange == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "Failed to set pipeline to PLAYING for stream: " << streamConfig.name << std::endl;
        std::string playingError = "failed to set pipeline to PLAYING for stream: " + streamConfig.name;
        if (state->bus) {
            GstMessage* msg = gst_bus_timed_pop_filtered(
                state->bus,
                2 * GST_SECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING));
            if (msg) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    GError* err = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_error(msg, &err, &dbg);
                    std::cerr << "PLAYING error: " << (err ? err->message : "unknown")
                              << " debug=" << (dbg ? dbg : "") << std::endl;
                    if (err && err->message) playingError = err->message;
                    if (dbg && *dbg) playingError += std::string(" | ") + dbg;
                    if (err) g_error_free(err);
                    g_free(dbg);
                } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_WARNING) {
                    GError* err = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_warning(msg, &err, &dbg);
                    std::cerr << "PLAYING warning: " << (err ? err->message : "unknown")
                              << " debug=" << (dbg ? dbg : "") << std::endl;
                    if (err) g_error_free(err);
                    g_free(dbg);
                }
                gst_message_unref(msg);
            }
            gst_object_unref(state->bus);
            state->bus = nullptr;
        }
        // A failed source can leave child elements in READY while the parent
        // pipeline transition to PLAYING has already failed.  Dropping the last
        // pipeline reference in that state is unsafe for some DVB drivers/plugins
        // (notably dvbsrc/dvbbasebin) and can lead to use-after-free/heap corruption.
        // Always drive the whole pipeline back to NULL before releasing it.
        state->running = false;
        state->active = false;
        state->statusMessage = "error: " + playingError;

        stopPipelineAndWait(pipeline);

        if (state->gstTranscoder) {
            state->gstTranscoder->stop();
            state->gstTranscoder.reset();
        }
        state->pipeline = nullptr;
        gst_object_unref(pipeline);
        releaseSharedSatelliteInput(state.get());
        if (error) *error = playingError;
        return false;
    }

    state->statusMessage = (stateChange == GST_STATE_CHANGE_ASYNC) ? "starting" : "running";
    const std::string notificationInputUri = state->primaryInputUri;
    bool duplicateStart = false;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        if (streams.count(streamConfig.id)) {
            duplicateStart = true;
        } else {
            streams[streamConfig.id] = std::move(state);
            streams[streamConfig.id]->busThread = std::thread(&StreamManager::monitorBus, this, streamConfig.id);
        }
    }
    if (duplicateStart) {
        state->running = false;
        if (state->pipeline) {
            stopPipelineAndWait(state->pipeline);
        }
        if (state->bus) {
            gst_object_unref(state->bus);
            state->bus = nullptr;
        }
        if (state->pipeline) {
            gst_object_unref(state->pipeline);
            state->pipeline = nullptr;
        }
        releaseSharedSatelliteInput(state.get());
        if (error) *error = "duplicate stream start detected: " + streamConfig.id;
        return false;
    }
    notifyStreamState(
        streamConfig,
        "🟢",
        telegramText(configManager, "Поток запущен", "Stream started"),
        telegramText(configManager, "Источник: основной", "Source: primary") + "\nURL: " + notificationInputUri);
    return true;
}

bool StreamManager::stopStream(const std::string& id) {
    std::unique_ptr<StreamState> statePtr;
    StreamConfig stoppedConfig;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto found = streams.find(id);
        if (found == streams.end()) {
            return false;
        }

        statePtr = std::move(found->second);
        streams.erase(found);
        stoppedConfig = statePtr->config;
        statePtr->running = false;
        statePtr->active = false;
        statePtr->statusMessage = "stopped";

        for (auto it = httpClients.begin(); it != httpClients.end();) {
            if (it->second.streamId == id) {
                it = httpClients.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
            if (it->second.streamId == id) {
                it = adHocSessions.erase(it);
            } else {
                ++it;
            }
        }
    }

    auto& state = *statePtr;
    stopExternalSrtOutputs(&state);
    // Stop the bus consumer before tearing down the GStreamer task graph.
    // The old order put the pipeline into NULL while monitorBus() could still
    // be polling the same bus, which is a plausible trigger for repeated
    // gst_poll_wait/GST_IS_TASK critical assertions during shutdown.
    if (state.bus) {
        gst_bus_set_flushing(state.bus, TRUE);
    }
    if (state.busThread.joinable()) {
        state.busThread.join();
    }
    if (state.pipeline) {
        stopPipelineAndWait(state.pipeline);
    }
    if (state.gstTranscoder) {
        state.gstTranscoder->stop();
        state.gstTranscoder.reset();
    }
    if (state.bus) {
        gst_object_unref(state.bus);
        state.bus = nullptr;
    }
    if (state.pipeline) {
        gst_object_unref(state.pipeline);
        state.pipeline = nullptr;
    }
    releaseSharedSatelliteInput(&state);
    state.outputContexts.clear();
    state.sourceContext.reset();

    notifyStreamState(
        stoppedConfig,
        "⚪",
        telegramText(configManager, "Поток остановлен", "Stream stopped"),
        telegramText(configManager, "Остановлен вручную", "Stopped manually"));
    return true;
}

bool StreamManager::restartStream(const StreamConfig& streamConfig, std::string* error) {
    if (error) error->clear();
    std::cerr << "Hard restarting stream: " << streamConfig.id << std::endl;
    const bool stopped = stopStream(streamConfig.id);
    if (stopped) {
        std::this_thread::sleep_for(kSrtRestartRetryDelay);
    }
    return startStream(streamConfig, error);
}

void StreamManager::stopAll() {
    std::vector<std::unique_ptr<StreamState>> stoppedStreams;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        httpClients.clear();
        adHocSessions.clear();
        for (auto& [id, statePtr] : streams) {
            (void)id;
            statePtr->running = false;
            statePtr->active = false;
            stoppedStreams.push_back(std::move(statePtr));
        }
        streams.clear();
    }

    for (auto& statePtr : stoppedStreams) {
        auto& state = *statePtr;
        stopExternalSrtOutputs(&state);
        if (state.bus) {
            gst_bus_set_flushing(state.bus, TRUE);
        }
        if (state.busThread.joinable()) {
            state.busThread.join();
        }
        if (state.pipeline) {
            stopPipelineAndWait(state.pipeline);
        }
        if (state.gstTranscoder) {
            state.gstTranscoder->stop();
            state.gstTranscoder.reset();
        }
        if (state.bus) {
            gst_object_unref(state.bus);
        }
        if (state.pipeline) {
            gst_object_unref(state.pipeline);
        }
        releaseSharedSatelliteInput(&state);
        state.outputContexts.clear();
        state.sourceContext.reset();
    }
}

bool StreamManager::isStreamActive(const std::string& id) {
    std::lock_guard<std::mutex> lock(managerMutex);
    auto found = streams.find(id);
    return found != streams.end() && found->second->active.load();
}

std::vector<std::string> StreamManager::activeStreams() {
    std::lock_guard<std::mutex> lock(managerMutex);
    std::vector<std::string> result;
    for (auto& [id, statePtr] : streams) {
        if (statePtr->active.load()) {
            result.push_back(id);
        }
    }
    return result;
}

std::map<std::string, StreamState*> StreamManager::snapshot() {
    std::lock_guard<std::mutex> lock(managerMutex);
    std::map<std::string, StreamState*> result;
    for (auto& [id, statePtr] : streams) {
        if (statePtr->gstTranscoder && !statePtr->gstTranscoder->isRunning()) {
            statePtr->running = false;
            statePtr->active = false;
            statePtr->statusMessage = "gstreamer transcoder exited";
        }
        if (statePtr->pipeline) {
            updateBitrateEstimates(statePtr.get());
        }
        result[id] = statePtr.get();
    }
    return result;
}

uint64_t StreamManager::queryPipelineBitrate(GstElement* pipeline) {
    (void)pipeline;
    return 0;
}
std::string StreamManager::buildPipelineDescription(const StreamConfig& cfg) {
    const std::string inputInterface = configuredInputInterfaceAddress(cfg);
    std::ostringstream desc;
    desc << "manual-pipeline"
         << " input=" << (cfg.satelliteEnabled ? tvs::protocols::inputUriForGstreamer(cfg) : cfg.inputUri)
         << " input_proto=" << tvs::stream_protocols::inputKindName(tvs::stream_protocols::inputKind(cfg))
         << " output_proto=" << tvs::stream_protocols::outputKindName(tvs::stream_protocols::outputKind(cfg))
         << " input_mode=" << cfg.inputMode
         << " input_iface=" << (inputInterface.empty() ? "auto" : inputInterface)
         << " test_pattern=" << (cfg.testPattern ? "on" : "off")
         << " remap=" << (cfg.remapEnabled ? "on" : "off")
         << " transcode=" << (cfg.transcodeEnabled ? cfg.transcodeResolution + "@" + std::to_string(cfg.transcodeVideoBitrate) : "off")
         << " outputs=";
    const auto outputs = outputConfigs(cfg);
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (i > 0) {
            desc << ",";
        }
        desc << outputType(outputs[i])
             << "/" << srtOutputMode(outputs[i])
             << "@" << outputs[i].outputHost << ":" << outputs[i].outputPort;
    }
    desc
         << " iface=" << cfg.interfaceAddress
         << " service_id=" << cfg.serviceId
         << " vpid=" << cfg.videoPid
         << " apid=" << cfg.audioPid;
    return desc.str();
}

bool StreamManager::addHttpClient(const std::string& id, int fd, const std::string& clientIp) {
    std::lock_guard<std::mutex> lock(managerMutex);
    auto found = streams.find(id);
    if (found == streams.end()) {
        close(fd);
        return false;
    }

    if (!found->second->pipeline && found->second->gstTranscoder && hasTranscodedHttpOutput(found->second->config)) {
        const uint16_t relayPort = tvs::protocols::transcodedHttpInternalPort(found->second->config);
        std::string relayError;
        int upstreamFd = connectLocalTcpWithRetry(relayPort, relayError);
        if (upstreamFd < 0) {
            std::cerr << "Transcoded HTTP relay failed for stream " << id
                      << ": " << relayError << std::endl;
            close(fd);
            return false;
        }

        httpClients[fd] = {id, normalizeIpAddress(clientIp), "mpegts"};
        std::thread([this, id, fd, upstreamFd]() {
            std::array<char, 65536> buffer {};
            while (true) {
                ssize_t readBytes = ::read(upstreamFd, buffer.data(), buffer.size());
                if (readBytes < 0) {
                    if (errno == EINTR) continue;
                    break;
                }
                if (readBytes == 0) break;
                if (!writeAllToFd(fd, buffer.data(), static_cast<size_t>(readBytes))) break;
            }
            ::close(upstreamFd);
            ::close(fd);
            std::lock_guard<std::mutex> relayLock(managerMutex);
            httpClients.erase(fd);
        }).detach();
        return true;
    }

    if (!found->second->pipeline) {
        close(fd);
        return false;
    }

    auto sinks = findSinksByFactory(found->second->pipeline, "multifdsink");
    if (sinks.empty()) {
        close(fd);
        return false;
    }

    GstElement* sink = sinks.front();
    for (size_t i = 1; i < sinks.size(); ++i) {
        gst_object_unref(sinks[i]);
    }

    g_signal_connect(sink, "client-fd-removed", G_CALLBACK(StreamManager::onHttpClientFdRemoved), this);
    g_signal_emit_by_name(sink, "add", fd);
    httpClients[fd] = {id, normalizeIpAddress(clientIp), "mpegts"};
    gst_object_unref(sink);
    return true;
}

bool StreamManager::addStreamSession(const std::string& streamId, const std::string& clientIp, const std::string& protocol) {
    if (streamId.empty() || clientIp.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    const auto now = std::chrono::steady_clock::now();
    const std::string key = protocol == "hls"
        ? protocol + ":" + streamId + ":" + normalizedClientIp
        : protocol + ":" + streamId + ":" + normalizedClientIp + ":" + std::to_string(nextSessionId.fetch_add(1, std::memory_order_relaxed));
    adHocSessions[key] = {streamId, normalizedClientIp, protocol, now};
    return true;
}

bool StreamManager::removeStreamSession(const std::string& streamId, const std::string& clientIp, const std::string& protocol) {
    if (streamId.empty() || clientIp.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        const auto& session = it->second;
        if (session.streamId == streamId && session.clientIp == normalizedClientIp && session.protocol == protocol) {
            it = adHocSessions.erase(it);
            return true;
        }
        ++it;
    }
    return false;
}

void StreamManager::onHttpClientFdRemoved(GstElement* sink, gint fd, gpointer userData) {
    (void)sink;
    auto* manager = static_cast<StreamManager*>(userData);
    if (!manager) return;
    std::lock_guard<std::mutex> lock(manager->managerMutex);
    manager->httpClients.erase(fd);
}

void StreamManager::pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::time_point now) {
    for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
        const auto& session = it->second;
        if (session.protocol == "hls" && now - session.lastActivity > kHlsSessionTtl) {
            it = adHocSessions.erase(it);
        } else {
            ++it;
        }
    }
}

size_t StreamManager::activeHttpSessions(const std::string& clientIp) const {
    std::lock_guard<std::mutex> lock(managerMutex);
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    size_t count = 0;
    for (const auto& [fd, session] : httpClients) {
        (void)fd;
        if (normalizeIpAddress(session.clientIp) == normalizedClientIp) ++count;
    }
    for (const auto& [key, session] : adHocSessions) {
        (void)key;
        if (normalizeIpAddress(session.clientIp) == normalizedClientIp) ++count;
    }
    return count;
}

size_t StreamManager::activeSubscriberSessions(const SubscriberConfig& subscriber) {
    if (!subscriber.enabled || subscriber.streamIds.empty()) {
        return 0;
    }

    const std::string primaryIp = normalizeIpAddress(subscriber.primaryIp);
    const std::string backupIp = normalizeIpAddress(subscriber.backupIp);
    auto ipMatches = [&](const std::string& clientIp) {
        const std::string normalizedClientIp = normalizeIpAddress(clientIp);
        return normalizedClientIp == primaryIp || (!backupIp.empty() && normalizedClientIp == backupIp);
    };
    auto streamMatches = [&](const std::string& streamId) {
        return std::find(subscriber.streamIds.begin(), subscriber.streamIds.end(), streamId) != subscriber.streamIds.end();
    };

    std::lock_guard<std::mutex> lock(managerMutex);
    pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
    size_t count = 0;
    for (const auto& [fd, session] : httpClients) {
        (void)fd;
        if (ipMatches(session.clientIp) && streamMatches(session.streamId)) {
            ++count;
        }
    }
    for (const auto& [key, session] : adHocSessions) {
        (void)key;
        if (ipMatches(session.clientIp) && streamMatches(session.streamId)) {
            ++count;
        }
    }
    return count;
}

size_t StreamManager::resetHttpSessions(const std::string& clientIp) {
    std::vector<std::pair<GstElement*, int>> sinks;
    size_t removed = 0;
    const std::string normalizedClientIp = normalizeIpAddress(clientIp);
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
        for (auto it = httpClients.begin(); it != httpClients.end();) {
            if (normalizeIpAddress(it->second.clientIp) != normalizedClientIp) {
                ++it;
                continue;
            }
            auto found = streams.find(it->second.streamId);
            if (found != streams.end() && found->second->pipeline) {
                auto foundSinks = findSinksByFactory(found->second->pipeline, "multifdsink");
                for (auto* sink : foundSinks) {
                    sinks.emplace_back(sink, it->first);
                }
            }
            ++removed;
            it = httpClients.erase(it);
        }
        for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
            if (normalizeIpAddress(it->second.clientIp) == normalizedClientIp) {
                ++removed;
                it = adHocSessions.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto& [sink, fd] : sinks) {
        g_signal_emit_by_name(sink, "remove", fd);
        gst_object_unref(sink);
    }
    return removed;
}

size_t StreamManager::enforceSubscriberAccess() {
    std::vector<std::pair<GstElement*, int>> sinks;
    std::vector<std::string> srtStreams;
    size_t removed = 0;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        pruneExpiredAdHocSessionsLocked(std::chrono::steady_clock::now());
        for (auto it = httpClients.begin(); it != httpClients.end();) {
            if (isClientAllowedForStream(it->second.streamId, it->second.clientIp)) {
                ++it;
                continue;
            }
            std::cerr << "Disconnecting unauthorized " << it->second.protocol
                      << " session stream=" << it->second.streamId
                      << " ip=" << it->second.clientIp << std::endl;
            auto found = streams.find(it->second.streamId);
            if (found != streams.end() && found->second->pipeline) {
                auto foundSinks = findSinksByFactory(found->second->pipeline, "multifdsink");
                for (auto* sink : foundSinks) {
                    sinks.emplace_back(sink, it->first);
                }
            }
            ++removed;
            it = httpClients.erase(it);
        }

        for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
            if (isClientAllowedForStream(it->second.streamId, it->second.clientIp)) {
                ++it;
                continue;
            }
            std::cerr << "Disconnecting unauthorized " << it->second.protocol
                      << " session stream=" << it->second.streamId
                      << " ip=" << it->second.clientIp << std::endl;
            if (it->second.protocol == "srt") {
                srtStreams.push_back(it->second.streamId);
            }
            ++removed;
            it = adHocSessions.erase(it);
        }
    }

    for (const auto& [sink, fd] : sinks) {
        g_signal_emit_by_name(sink, "remove", fd);
        gst_object_unref(sink);
    }
    if (!srtStreams.empty()) {
        restartSrtOutputsForStreams(srtStreams);
    }
    return removed;
}

size_t StreamManager::restartSrtOutputsForStreams(const std::vector<std::string>& streamIds) {
    std::vector<StreamConfig> configs;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        for (const auto& streamId : streamIds) {
            if (streamId.empty() || std::find_if(configs.begin(), configs.end(), [&streamId](const StreamConfig& config) {
                    return config.id == streamId;
                }) != configs.end()) {
                continue;
            }
            auto found = streams.find(streamId);
            if (found == streams.end() || !hasSrtListenerOutput(found->second->config)) {
                continue;
            }
            if (!found->second->pipeline && found->second->externalSrtOutputs.empty()) {
                continue;
            }
            configs.push_back(found->second->config);
        }

        for (const auto& config : configs) {
            for (auto it = adHocSessions.begin(); it != adHocSessions.end();) {
                if (it->second.protocol == "srt" && it->second.streamId == config.id) {
                    it = adHocSessions.erase(it);
                } else {
                    ++it;
                }
            }
        }
    }

    size_t restarted = 0;
    for (const auto& config : configs) {
        std::cerr << "Rebuilding SRT listener to drop clients for stream " << config.id << std::endl;
        if (!stopStream(config.id)) {
            std::cerr << "Unable to stop SRT listener for stream " << config.id << std::endl;
            continue;
        }

        bool started = false;
        for (int attempt = 1; attempt <= kSrtRestartAttempts; ++attempt) {
            std::this_thread::sleep_for(kSrtRestartRetryDelay);
            if (startStream(config)) {
                started = true;
                break;
            }
            std::cerr << "SRT listener restart attempt " << attempt
                      << " failed for stream " << config.id << std::endl;
        }

        if (started) {
            ++restarted;
        } else {
            std::cerr << "SRT listener remains stopped after restart attempts for stream "
                      << config.id << std::endl;
        }
    }
    return restarted;
}

size_t StreamManager::restartAllSrtOutputs() {
    std::vector<std::string> streamIds;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        for (const auto& [id, state] : streams) {
            if (state && hasSrtListenerOutput(state->config) &&
                (state->pipeline || !state->externalSrtOutputs.empty())) {
                streamIds.push_back(id);
            }
        }
    }
    return restartSrtOutputsForStreams(streamIds);
}

bool StreamManager::isClientAllowedForStream(const std::string& streamId, const std::string& clientIp) const {
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

gboolean StreamManager::onSrtCallerConnecting(GstElement* sink, GSocketAddress* addr, const gchar* streamId, gpointer userData) {
    auto* ctx = static_cast<SrtAccessContext*>(userData);
    if (!ctx || !ctx->manager) {
        return TRUE;
    }

    const std::string clientIp = socketAddressToString(addr);
    const std::string requestedStreamId = streamId ? streamId : "";
    const bool allowed = ctx->manager->isClientAllowedForStream(ctx->streamId, clientIp);
    if (!allowed) {
        std::cerr << "SRT access denied for stream " << ctx->streamId << " from " << clientIp;
        if (!requestedStreamId.empty()) {
            std::cerr << " requested_streamid=" << requestedStreamId;
        }
        std::cerr << std::endl;
    } else {
        std::cerr << "SRT access allowed for stream " << ctx->streamId << " from " << clientIp;
        if (!requestedStreamId.empty()) {
            std::cerr << " requested_streamid=" << requestedStreamId;
        }
        std::cerr << std::endl;
    }
    return allowed ? TRUE : FALSE;
}

void StreamManager::onSrtCallerAdded(GstElement* sink, gint, GSocketAddress* addr, gpointer userData) {
    auto* ctx = static_cast<SrtAccessContext*>(userData);
    if (!ctx || !ctx->manager) {
        return;
    }
    const std::string clientIp = socketAddressToString(addr);
    if (!clientIp.empty()) {
        if (!ctx->manager->isClientAllowedForStream(ctx->streamId, clientIp)) {
            std::cerr << "Rebuilding SRT listener after unauthorized caller for stream " << ctx->streamId
                      << " from " << clientIp << std::endl;
            ctx->manager->restartSrtOutputsForStreams({ctx->streamId});
            return;
        }
        std::cerr << "SRT caller added for stream " << ctx->streamId
                  << " from " << clientIp << std::endl;
        ctx->manager->addStreamSession(ctx->streamId, clientIp, "srt");
    }
    (void)sink;
}

void StreamManager::onSrtCallerRemoved(GstElement* sink, gint, GSocketAddress* addr, gpointer userData) {
    auto* ctx = static_cast<SrtAccessContext*>(userData);
    if (!ctx || !ctx->manager) {
        return;
    }
    const std::string clientIp = socketAddressToString(addr);
    if (!clientIp.empty()) {
        std::cerr << "SRT caller removed for stream " << ctx->streamId
                  << " from " << clientIp << std::endl;
        ctx->manager->removeStreamSession(ctx->streamId, clientIp, "srt");
    }
    (void)sink;
}

void StreamManager::onSrtCallerRejected(GstElement* sink, GSocketAddress* addr, const gchar* streamId, gpointer userData) {
    auto* ctx = static_cast<SrtAccessContext*>(userData);
    if (!ctx) {
        return;
    }
    const std::string clientIp = socketAddressToString(addr);
    const std::string requestedStreamId = streamId ? streamId : "";
    std::cerr << "SRT caller rejected for stream " << ctx->streamId
              << " from " << (clientIp.empty() ? "unknown" : clientIp);
    if (!requestedStreamId.empty()) {
        std::cerr << " requested_streamid=" << requestedStreamId;
    }
    std::cerr << std::endl;
    (void)sink;
}

void StreamManager::notifyStreamState(
    const StreamConfig& cfg,
    const std::string& color,
    const std::string& title,
    const std::string& details) {
    const std::string serverName = configManager.config.serverName.empty()
        ? "TVStreamer5"
        : configManager.config.serverName;
    std::ostringstream message;
    const bool english = telegramUsesEnglish(configManager);
    message << color << " <b>" << telegramEscape(title) << "</b>\n"
            << (english ? "Server" : "Сервер") << ": <b>" << telegramEscape(serverName) << "</b>\n"
            << (english ? "Channel" : "Канал") << ": <b>" << telegramEscape(displayName(cfg)) << "</b>\n"
            << "ID: <code>" << telegramEscape(cfg.id) << "</code>";
    if (!details.empty()) {
        message << "\n" << telegramEscape(details);
    }
    telegramNotifier.sendMessage(message.str());
}

bool StreamManager::probeInputAvailable(
    const StreamConfig& baseConfig,
    const std::string& inputUri,
    std::chrono::milliseconds timeout) {
    if (inputUri.empty()) {
        return false;
    }

    StreamState probeState;
    probeState.config = baseConfig;
    probeState.config.inputUri = inputUri;
    probeState.config.testPattern = false;
    probeState.runtimeConfig = probeState.config;
    probeState.sourceContext = std::make_unique<RemapContext>();
    probeState.sourceContext->config = probeState.runtimeConfig;

    GstElement* pipeline = gst_pipeline_new(nullptr);
    if (!pipeline) {
        return false;
    }

    GstElement* sourceTail = nullptr;
    GstElement* source = createSourceChain(&probeState, pipeline, sourceTail);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    if (!source || !sourceTail || !sink ||
        !addElementOrFail(pipeline, sink) ||
        !gst_element_link(sourceTail, sink)) {
        stopPipelineAndWait(pipeline);
        gst_object_unref(pipeline);
        return false;
    }

    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    std::atomic<bool> receivedData{false};
    GstPad* probePad = gst_element_get_static_pad(sourceTail, "src");
    if (probePad) {
        gst_pad_add_probe(
            probePad,
            static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
            [](GstPad*, GstPadProbeInfo*, gpointer userData) -> GstPadProbeReturn {
                static_cast<std::atomic<bool>*>(userData)->store(true, std::memory_order_relaxed);
                return GST_PAD_PROBE_OK;
            },
            &receivedData,
            nullptr);
        gst_object_unref(probePad);
    }

    GstBus* bus = gst_element_get_bus(pipeline);
    const GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    bool available = stateResult != GST_STATE_CHANGE_FAILURE;
    const auto deadline = std::chrono::steady_clock::now() + timeout;

    while (available && !receivedData.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < deadline) {
        GstMessage* message = gst_bus_timed_pop_filtered(
            bus,
            100 * GST_MSECOND,
            static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (!message) {
            continue;
        }
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR ||
            GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            available = false;
        }
        gst_message_unref(message);
    }

    available = available && receivedData.load(std::memory_order_relaxed);
    stopPipelineAndWait(pipeline);
    if (bus) {
        gst_object_unref(bus);
    }
    gst_object_unref(pipeline);
    return available;
}

bool StreamManager::restartPipelineWithInput(StreamState* state, const std::string& inputUri, bool useBackup) {
    if (!state || inputUri.empty()) {
        return false;
    }

    GstElement* oldPipeline = state->pipeline;
    GstBus* oldBus = state->bus;

    if (oldBus && GST_IS_BUS(oldBus)) {
        gst_bus_set_flushing(oldBus, TRUE);
    }
    if (oldPipeline) {
        stopPipelineAndWait(oldPipeline);
    }

    state->runtimeConfig = state->config;
    if (useBackup) {
        state->runtimeConfig.satelliteEnabled = false;
        state->runtimeConfig.inputUri = inputUri;
    } else if (state->sharedSatelliteInput) {
        state->runtimeConfig.satelliteEnabled = false;
        state->runtimeConfig.inputUri = state->satelliteServiceRelayUri;
        state->runtimeConfig.inputMode = "udp";
        if (state->config.satelliteEnabled && !state->config.satelliteScrambled &&
            state->satelliteServiceRelay == nullptr) {
            state->runtimeConfig.inputInterfaceAddress = "127.0.0.1";
        } else {
            state->runtimeConfig.inputInterfaceAddress.clear();
        }
        state->runtimeConfig.inputInterfaceAddressConfigured = true;
    } else {
        state->runtimeConfig.inputUri = inputUri;
        state->runtimeConfig.satelliteEnabled = state->primarySatelliteEnabled;
    }
    state->sourceContext = std::make_unique<RemapContext>();
    state->sourceContext->config = state->runtimeConfig;
    state->outputContexts.clear();

    GstElement* newPipeline = createPipeline(state);
    if (!newPipeline) {
        state->pipeline = oldPipeline;
        state->bus = oldBus;
        state->statusMessage = "error: restart failed";
        if (oldPipeline) {
            gst_element_set_state(oldPipeline, GST_STATE_PLAYING);
        }
        return false;
    }

    GstBus* newBus = gst_element_get_bus(newPipeline);
    state->pipeline = newPipeline;
    state->bus = newBus;
    state->usingBackup = useBackup;
    state->backupAttempted = useBackup;
    state->primaryRetryPending = !useBackup;
    state->inputLossNotified = false;
    state->activeInputUri = useBackup ? inputUri : state->primaryInputUri;
    state->active = true;
    state->statusMessage = useBackup ? "running on backup" : "running on primary";
    state->inputBytes = 0;
    state->outputBytes = 0;
    state->inputCcErrors = 0;
    state->inputCcErrorsDelta = 0;
    state->outputCcErrors = 0;
    state->outputCcErrorsDelta = 0;
    state->inputBitrate = 0;
    state->outputBitrate = initialConfiguredOutputBitrate(state->config);
    state->lastInputBytesSample = 0;
    state->lastOutputBytesSample = 0;
    state->lastInputCcErrorsSample = 0;
    state->lastOutputCcErrorsSample = 0;
    state->lastInputBytesSeen = 0;
    {
        std::lock_guard<std::mutex> lock(state->inputContinuityMutex);
        state->inputContinuityValid.fill(false);
        state->inputTsRemainder.clear();
    }
    {
        std::lock_guard<std::mutex> lock(state->outputContinuityMutex);
        state->outputContinuityValid.fill(false);
        state->outputTsRemainder.clear();
    }
    state->lastInputActivity = std::chrono::steady_clock::now();
    state->lastPrimaryRetry = state->lastInputActivity;
    state->lastBitrateSample = state->lastInputActivity;
    state->ccRecoveryBurstCount = 0;
    attachBitrateProbes(state);

    GstStateChangeReturn ret = gst_element_set_state(newPipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        state->statusMessage = "error: restart playback failed";
        state->active = false;
    }

    if (oldBus) {
        gst_object_unref(oldBus);
    }
    if (oldPipeline) {
        gst_object_unref(oldPipeline);
    }
    return ret != GST_STATE_CHANGE_FAILURE;
}

GstElement* StreamManager::createPipeline(StreamState* state) {
    if (!state) {
        return nullptr;
    }
    const StreamConfig& cfg = state->config;
    GstElement* pipeline = gst_pipeline_new(cfg.id.c_str());
    if (!pipeline) {
        return nullptr;
    }

    GstElement* sourceTail = nullptr;
    if (!createSourceChain(state, pipeline, sourceTail) || !sourceTail) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    state->outputContexts.clear();
    if (cfg.transcodeEnabled) {
        std::string transcodeError;
        GstElement* transcoderBin = TranscoderModule::createBin(cfg, transcodeError);
        if (!transcoderBin || !addElementOrFail(pipeline, transcoderBin) ||
            !gst_element_link(sourceTail, transcoderBin)) {
            std::cerr << "Transcoder setup failed for " << cfg.id << ": " << transcodeError << std::endl;
            if (transcoderBin && !GST_OBJECT_PARENT(transcoderBin)) gst_object_unref(transcoderBin);
            gst_object_unref(pipeline);
            return nullptr;
        }
        sourceTail = transcoderBin;
    }
    if (!buildOutputBranches(state, pipeline, sourceTail)) {
        gst_object_unref(pipeline);
        return nullptr;
    }

    return pipeline;
}

GstElement* StreamManager::createSourceChain(StreamState* state, GstElement* pipeline, GstElement*& terminalElement) {
    terminalElement = nullptr;
    if (!state) {
        return nullptr;
    }
    const StreamConfig& cfg = state->runtimeConfig;
    const std::string input = cfg.testPattern ? kTestPatternUri : cfg.inputUri;
    const std::string inputLower = toLower(input);
    const auto inputProtocol = tvs::stream_protocols::inputKind(cfg);

    auto addQueue = [&](const char* name, guint64 maxSizeTime = 3000000000ULL) -> GstElement* {
        if (state->caProviderTransport && maxSizeTime < 8000000000ULL) {
            maxSizeTime = 8000000000ULL;
        }
        GstElement* queue = gst_element_factory_make("queue", name);
        if (!addElementOrFail(pipeline, queue)) {
            return nullptr;
        }
        configureQueue(queue, maxSizeTime);
        return queue;
    };

    if (tvs::stream_protocols::isTestPatternInput(inputProtocol)) {
        return createTestPatternChain(cfg, pipeline, terminalElement);
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Satellite) {
        if (!hasElementFactory("dvbbasebin")) {
            std::cerr << missingElementStatus("dvbbasebin")
                      << " (install gstreamer1.0-plugins-bad)" << std::endl;
            return nullptr;
        }
        if (cfg.satelliteFrequency == 0 || cfg.satelliteSymbolRate == 0) {
            std::cerr << "satellite input requires frequency and symbol rate" << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("dvbbasebin", "satellite_input_src");
        GstElement* queue = addQueue("input_queue", 8000000000ULL);
        if (!src || !queue || !addElementOrFail(pipeline, src)) {
            return nullptr;
        }

        setIntPropertyIfPresent(src, "adapter", cfg.satelliteAdapter);
        setIntPropertyIfPresent(src, "frontend", cfg.satelliteFrontend);
        setUIntPropertyIfPresent(src, "frequency", cfg.satelliteFrequency);
        setUIntPropertyIfPresent(src, "symbol-rate", cfg.satelliteSymbolRate);
        setStringPropertyIfPresent(src, "polarity", cfg.satellitePolarization);
        setSerializedPropertyIfPresent(src, "delsys", cfg.satelliteDeliverySystem);
        setSerializedPropertyIfPresent(src, "modulation", cfg.satelliteModulation);
        setSerializedPropertyIfPresent(src, "code-rate-hp", cfg.satelliteFec);
        setSerializedPropertyIfPresent(src, "pilot", cfg.satellitePilot);
        setSerializedPropertyIfPresent(src, "rolloff", cfg.satelliteRolloff);
        setIntPropertyIfPresent(src, "diseqc-source", cfg.satelliteDiseqcSource);
        setIntPropertyIfPresent(src, "stream-id", cfg.satelliteStreamId);
        setUIntPropertyIfPresent(src, "lnb-lof1", cfg.satelliteLnbLof1);
        setUIntPropertyIfPresent(src, "lnb-lof2", cfg.satelliteLnbLof2);
        setUIntPropertyIfPresent(src, "lnb-slof", cfg.satelliteLnbSlof);
        setUInt64PropertyIfPresent(src, "tuning-timeout", 10000000ULL);
        if (cfg.satelliteServiceId > 0) {
            setStringPropertyIfPresent(src, "program-numbers", std::to_string(cfg.satelliteServiceId));
        }

        if (!gst_element_link(src, queue)) {
            std::cerr << "failed to link DVB-S/S2 source to input queue" << std::endl;
            return nullptr;
        }

        std::cerr << "Satellite input: adapter=" << cfg.satelliteAdapter
                  << " frontend=" << cfg.satelliteFrontend
                  << " frequency_khz=" << cfg.satelliteFrequency
                  << " symbol_rate_kbd=" << cfg.satelliteSymbolRate
                  << " polarity=" << cfg.satellitePolarization
                  << " delsys=" << cfg.satelliteDeliverySystem
                  << " modulation=" << cfg.satelliteModulation
                  << " fec=" << cfg.satelliteFec
                  << " diseqc=" << cfg.satelliteDiseqcSource
                  << " stream_id=" << cfg.satelliteStreamId
                  << " service_id=" << cfg.satelliteServiceId
                  << std::endl;

        terminalElement = queue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Rtmp) {
        if (!hasElementFactory("rtmpsrc") || !hasElementFactory("flvdemux") || !hasElementFactory("mpegtsmux")) {
            std::cerr << "missing RTMP input elements: rtmpsrc, flvdemux or mpegtsmux" << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("rtmpsrc", "input_src");
        GstElement* inputQueue = addQueue("input_queue", 5000000000ULL);
        GstElement* demux = gst_element_factory_make("flvdemux", "input_flv_demux");
        GstElement* mux = gst_element_factory_make("mpegtsmux", "input_rtmp_ts_mux");
        GstElement* outputQueue = gst_element_factory_make("queue", "input_rtmp_ts_queue");
        if (!src || !inputQueue || !demux || !mux || !outputQueue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, demux) ||
            !addElementOrFail(pipeline, mux) ||
            !addElementOrFail(pipeline, outputQueue)) {
            return nullptr;
        }

        g_object_set(src,
            "location", input.c_str(),
            "do-timestamp", TRUE,
            "timeout", 15,
            nullptr);
        configureQueue(outputQueue);
        configureTsMux(mux, cfg);

        if (!gst_element_link_many(src, inputQueue, demux, nullptr) ||
            !gst_element_link(mux, outputQueue)) {
            return nullptr;
        }

        if (!state->sourceContext) {
            state->sourceContext = std::make_unique<RemapContext>();
        }
        state->sourceContext->mux = mux;
        state->sourceContext->config = cfg;
        state->sourceContext->flvMux = false;
        g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onFlvDemuxPadAdded), state->sourceContext.get());

        terminalElement = outputQueue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Rtsp) {
        if (!hasElementFactory("rtspsrc") || !hasElementFactory("mpegtsmux")) {
            std::cerr << "missing RTSP input elements: rtspsrc or mpegtsmux" << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("rtspsrc", "input_src");
        GstElement* mux = gst_element_factory_make("mpegtsmux", "input_rtsp_ts_mux");
        GstElement* outputQueue = gst_element_factory_make("queue", "input_queue");
        if (!src || !mux || !outputQueue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, mux) ||
            !addElementOrFail(pipeline, outputQueue)) {
            return nullptr;
        }

        g_object_set(src,
            "location", input.c_str(),
            "latency", 300,
            "do-rtsp-keep-alive", TRUE,
            nullptr);
        setUInt64PropertyIfPresent(src, "timeout", 15000000);
        setBooleanPropertyIfPresent(src, "ntp-sync", FALSE);
        configureQueue(outputQueue, 5000000000ULL);
        configureTsMux(mux, cfg);

        if (!gst_element_link(mux, outputQueue)) {
            return nullptr;
        }

        if (!state->sourceContext) {
            state->sourceContext = std::make_unique<RemapContext>();
        }
        state->sourceContext->mux = mux;
        state->sourceContext->config = cfg;
        state->sourceContext->flvMux = false;
        g_signal_connect(src, "pad-added", G_CALLBACK(StreamManager::onRtspPadAdded), state->sourceContext.get());

        terminalElement = outputQueue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Srt) {
        std::string mode = toLower(cfg.inputMode);
        const std::string inputInterface = configuredInputInterfaceAddress(cfg);
        const char* factory = (mode == "listener") ? "srtsrc" : "srtclientsrc";
        if (!hasElementFactory(factory)) {
            std::cerr << missingElementStatus(factory) << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make(factory, "input_src");
        GstElement* queue = addQueue("input_queue");
        if (!src || !queue || !addElementOrFail(pipeline, src)) {
            return nullptr;
        }

        g_object_set(src, "uri", input.c_str(), nullptr);
        setBooleanPropertyIfPresent(src, "do-timestamp", TRUE);
        setBooleanPropertyIfPresent(src, "auto-reconnect", TRUE);
        setIntPropertyIfPresent(src, "latency", 500);
        setStringPropertyIfPresent(src, "localaddress", inputInterface);
        if (mode == "listener") {
            setIntPropertyIfPresent(src, "mode", 2);
            setBooleanPropertyIfPresent(src, "wait-for-connection", TRUE);
            setBooleanPropertyIfPresent(src, "keep-listening", TRUE);
        } else {
            setIntPropertyIfPresent(src, "mode", 1);
            setBooleanPropertyIfPresent(src, "wait-for-connection", FALSE);
            setUIntPropertyIfPresent(src, "localport", 0);
        }

        if (!gst_element_link(src, queue)) {
            return nullptr;
        }

        terminalElement = queue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Hls) {
        if (!hasElementFactory("souphttpsrc")) {
            std::cerr << missingElementStatus("souphttpsrc") << std::endl;
            return nullptr;
        }
        if (!hasElementFactory("hlsdemux")) {
            std::cerr << missingElementStatus("hlsdemux") << std::endl;
            return nullptr;
        }
        if (!hasElementFactory("mpegtsmux")) {
            std::cerr << missingElementStatus("mpegtsmux") << std::endl;
            return nullptr;
        }

        std::string location = input;
        if (inputLower.rfind("hls://", 0) == 0) {
            location = "http://" + input.substr(6);
        }

        const std::string inputInterface = configuredInputInterfaceAddress(cfg);
        GstElement* src = gst_element_factory_make("souphttpsrc", "input_src");
        GstElement* demux = gst_element_factory_make("hlsdemux", "hls_demux");
        GstElement* mux = gst_element_factory_make("mpegtsmux", "input_hls_ts_mux");
        GstElement* queue = addQueue("input_queue", 5000000000ULL);
        if (!src || !demux || !mux || !queue ||
            !addElementOrFail(pipeline, src) ||
            !addElementOrFail(pipeline, demux) ||
            !addElementOrFail(pipeline, mux)) {
            return nullptr;
        }

        g_object_set(src, "location", location.c_str(), "is-live", TRUE, "do-timestamp", TRUE, nullptr);
        setIntPropertyIfPresent(src, "timeout", 15);
        if (!inputInterface.empty()) {
            std::cerr << "HLS input: input_iface=" << inputInterface
                      << " is selected, but souphttpsrc uses the kernel route for HTTP sockets"
                      << std::endl;
        }
        setIntPropertyIfPresent(demux, "connection-speed", static_cast<gint>(std::max<uint64_t>(cfg.targetBitrate / 1000, 1)));
        configureTsMux(mux, cfg);

        if (!gst_element_link(src, demux) || !gst_element_link(mux, queue)) {
            return nullptr;
        }

        if (!state->sourceContext) {
            state->sourceContext = std::make_unique<RemapContext>();
        }
        state->sourceContext->mux = mux;
        state->sourceContext->config = cfg;
        state->sourceContext->flvMux = false;
        g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), state->sourceContext.get());
        terminalElement = queue;
        return src;
    }

    if (inputProtocol == tvs::stream_protocols::InputProtocolKind::Http) {
        if (!hasElementFactory("souphttpsrc")) {
            std::cerr << missingElementStatus("souphttpsrc") << std::endl;
            return nullptr;
        }

        GstElement* src = gst_element_factory_make("souphttpsrc", "input_src");
        GstElement* queue = addQueue("input_queue");
        if (!src || !queue || !addElementOrFail(pipeline, src)) {
            return nullptr;
        }

        const std::string inputInterface = configuredInputInterfaceAddress(cfg);
        g_object_set(src, "location", input.c_str(), "is-live", TRUE, "do-timestamp", TRUE, nullptr);
        setIntPropertyIfPresent(src, "timeout", 15);
        if (!inputInterface.empty()) {
            std::cerr << "HTTP input: input_iface=" << inputInterface
                      << " is selected, but souphttpsrc uses the kernel route for HTTP sockets"
                      << std::endl;
        }

        if (!gst_element_link(src, queue)) {
            return nullptr;
        }

        terminalElement = queue;
        return src;
    }

    if (UdpInput::handles(input)) {
        std::string error;
        GstElement* src = UdpInput::build(pipeline, cfg, terminalElement, error);
        if (!src) {
            std::cerr << error << std::endl;
        }
        return src;
    }
    const std::string location = fileLocationFromInput(input);
    GstElement* src = gst_element_factory_make("filesrc", "input_src");
    if (!src || !addElementOrFail(pipeline, src)) {
        return nullptr;
    }
    g_object_set(src, "location", location.c_str(), nullptr);

    // MPEG-TS replacement files can be passed directly to the existing TS output chain.
    if (isMpegTsFile(input)) {
        GstElement* queue = addQueue("input_queue");
        if (!queue || !gst_element_link(src, queue)) {
            return nullptr;
        }
        terminalElement = queue;
        return src;
    }

    // Container files such as MP4/MOV must be demuxed and remuxed to MPEG-TS first.
    if (!hasElementFactory("qtdemux") || !hasElementFactory("mpegtsmux")) {
        std::cerr << "missing MP4 replacement file elements: qtdemux or mpegtsmux" << std::endl;
        return nullptr;
    }

    GstElement* demux = gst_element_factory_make("qtdemux", "input_file_demux");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "input_file_ts_mux");
    GstElement* outputQueue = gst_element_factory_make("queue", "input_queue");
    if (!demux || !mux || !outputQueue ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, outputQueue)) {
        return nullptr;
    }

    configureQueue(outputQueue, 5000000000ULL);
    configureTsMux(mux, cfg);
    if (!gst_element_link(src, demux) || !gst_element_link(mux, outputQueue)) {
        return nullptr;
    }

    if (!state->sourceContext) {
        state->sourceContext = std::make_unique<RemapContext>();
    }
    state->sourceContext->mux = mux;
    state->sourceContext->config = cfg;
    state->sourceContext->flvMux = false;
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), state->sourceContext.get());

    terminalElement = outputQueue;
    return src;
}

GstElement* StreamManager::createTestPatternChain(const StreamConfig& cfg, GstElement* pipeline, GstElement*& terminalElement) {
    terminalElement = nullptr;
    const std::vector<const char*> required = {
        "videotestsrc", "capsfilter", "videoconvert", "x264enc", "h264parse",
        "audiotestsrc", "audioconvert", "audioresample", "avenc_aac", "aacparse",
        "mpegtsmux", "queue"
    };
    for (const char* element : required) {
        if (!hasElementFactory(element)) {
            std::cerr << missingElementStatus(element) << std::endl;
            return nullptr;
        }
    }

    GstElement* src = gst_element_factory_make("videotestsrc", "test_bars_src");
    GstElement* capsfilter = gst_element_factory_make("capsfilter", "test_bars_caps");
    GstElement* convert = gst_element_factory_make("videoconvert", "test_bars_convert");
    GstElement* encoder = gst_element_factory_make("x264enc", "test_bars_encoder");
    GstElement* parser = gst_element_factory_make("h264parse", "test_bars_h264parse");
    GstElement* videoQueue = gst_element_factory_make("queue", "test_bars_video_queue");
    GstElement* audioSrc = gst_element_factory_make("audiotestsrc", "test_tone_src");
    GstElement* audioConvert = gst_element_factory_make("audioconvert", "test_tone_convert");
    GstElement* audioResample = gst_element_factory_make("audioresample", "test_tone_resample");
    GstElement* audioCapsfilter = gst_element_factory_make("capsfilter", "test_tone_caps");
    GstElement* audioEncoder = gst_element_factory_make("avenc_aac", "test_tone_encoder");
    GstElement* audioParser = gst_element_factory_make("aacparse", "test_tone_aacparse");
    GstElement* audioQueue = gst_element_factory_make("queue", "test_tone_queue");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "test_bars_mux");
    GstElement* queue = gst_element_factory_make("queue", "test_bars_queue");

    if (!src || !capsfilter || !convert || !encoder || !parser || !videoQueue ||
        !audioSrc || !audioConvert || !audioResample || !audioCapsfilter || !audioEncoder ||
        !audioParser || !audioQueue || !mux || !queue) {
        return nullptr;
    }

    if (!addElementOrFail(pipeline, src) ||
        !addElementOrFail(pipeline, capsfilter) ||
        !addElementOrFail(pipeline, convert) ||
        !addElementOrFail(pipeline, encoder) ||
        !addElementOrFail(pipeline, parser) ||
        !addElementOrFail(pipeline, videoQueue) ||
        !addElementOrFail(pipeline, audioSrc) ||
        !addElementOrFail(pipeline, audioConvert) ||
        !addElementOrFail(pipeline, audioResample) ||
        !addElementOrFail(pipeline, audioCapsfilter) ||
        !addElementOrFail(pipeline, audioEncoder) ||
        !addElementOrFail(pipeline, audioParser) ||
        !addElementOrFail(pipeline, audioQueue) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, queue)) {
        return nullptr;
    }

    GstCaps* caps = gst_caps_from_string(
        "video/x-raw,format=I420,width=1920,height=1080,framerate=25/1,pixel-aspect-ratio=1/1");
    g_object_set(capsfilter, "caps", caps, nullptr);
    gst_caps_unref(caps);

    GstCaps* audioCaps = gst_caps_from_string("audio/x-raw,format=F32LE,rate=48000,channels=2");
    g_object_set(audioCapsfilter, "caps", audioCaps, nullptr);
    gst_caps_unref(audioCaps);

    constexpr guint audioBitrate = 128000;
    const uint64_t availableVideoBitrate = cfg.targetBitrate > 500000
        ? (cfg.targetBitrate * 85 / 100) - audioBitrate
        : 1000000;
    guint bitrateKbps = static_cast<guint>(std::max<uint64_t>(availableVideoBitrate / 1000, 350));
    g_object_set(src,
        "is-live", TRUE,
        "pattern", 0,
        nullptr);
    g_object_set(encoder,
        "bitrate", bitrateKbps,
        "key-int-max", 50,
        "bframes", 0,
        "byte-stream", TRUE,
        nullptr);
    gst_util_set_object_arg(G_OBJECT(encoder), "tune", "zerolatency");
    gst_util_set_object_arg(G_OBJECT(encoder), "speed-preset", "veryfast");
    g_object_set(parser, "config-interval", 1, nullptr);
    g_object_set(audioSrc, "is-live", TRUE, "wave", 0, "freq", 1000.0, nullptr);
    g_object_set(audioEncoder, "bitrate", static_cast<gint64>(audioBitrate), nullptr);
    configureTsMux(mux, cfg);
    configureQueue(videoQueue);
    configureQueue(audioQueue);
    configureQueue(queue);

    if (!gst_element_link_many(src, capsfilter, convert, encoder, parser, videoQueue, mux, nullptr) ||
        !gst_element_link_many(audioSrc, audioConvert, audioResample, audioCapsfilter,
            audioEncoder, audioParser, audioQueue, mux, nullptr) ||
        !gst_element_link(mux, queue)) {
        return nullptr;
    }

    terminalElement = queue;
    return src;
}

bool StreamManager::buildOutputBranches(StreamState* state, GstElement* pipeline, GstElement* sourceTail) {
    if (!state || !pipeline || !sourceTail) {
        return false;
    }

    const auto outputs = pipelineOutputConfigs(state->config);
    if (outputs.empty()) {
        return false;
    }
    if (outputs.size() == 1) {
        return buildOutputBranch(state, pipeline, sourceTail, outputs.front(), 0);
    }

    if (!hasElementFactory("tee")) {
        std::cerr << missingElementStatus("tee") << std::endl;
        return false;
    }

    GstElement* tee = gst_element_factory_make("tee", "output_tee");
    if (!addElementOrFail(pipeline, tee)) {
        return false;
    }
    if (!gst_element_link(sourceTail, tee)) {
        return false;
    }

    for (size_t i = 0; i < outputs.size(); ++i) {
        GstElement* queue = gst_element_factory_make("queue", branchName("tee_queue", i).c_str());
        if (!addElementOrFail(pipeline, queue)) {
            return false;
        }
        configureQueue(queue);

        GstPad* teeSrcPad = gst_element_request_pad_simple(tee, "src_%u");
        GstPad* queueSinkPad = gst_element_get_static_pad(queue, "sink");
        if (!teeSrcPad || !queueSinkPad) {
            if (teeSrcPad) gst_object_unref(teeSrcPad);
            if (queueSinkPad) gst_object_unref(queueSinkPad);
            return false;
        }
        const bool linked = gst_pad_link(teeSrcPad, queueSinkPad) == GST_PAD_LINK_OK;
        gst_object_unref(teeSrcPad);
        gst_object_unref(queueSinkPad);
        if (!linked) {
            return false;
        }

        if (!buildOutputBranch(state, pipeline, queue, outputs[i], i)) {
            return false;
        }
    }

    return true;
}

bool StreamManager::buildOutputBranch(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    const std::string type = outputType(outputConfig);
    if (type == "rtmp" || type == "youtube") {
        return buildRtmpOutputPipeline(state, pipeline, sourceTail, outputConfig, branchIndex);
    }

    // A transcoded stream is already a finished single-program MPEG-TS produced by
    // TranscoderModule. Re-demuxing and remuxing it separately for UDP/SRT/HTTP/HLS
    // can drop the copied audio PID or split audio/video into different programs.
    // Feed the same transcoded TS to every TS-capable protocol and only apply remap
    // to non-transcoded passthrough streams.
    const bool transcodedInput = state && state->config.transcodeEnabled;

    // v94: a direct FTA DVB-S/S2 source can still expose the complete
    // transponder transport stream even when dvbbasebin program-numbers is
    // configured.  Passing that TS straight to HTTP/UDP makes VLC see every
    // service on the transponder and may leave only PSI/audio traffic for the
    // selected channel.  For FTA satellite services always perform a local
    // single-service demux/remux in the main pipeline.  This does not use the
    // old loopback UDP service relay.
    const bool forceFtaSatelliteServiceRemux =
        state && state->config.satelliteEnabled &&
        !state->config.satelliteScrambled &&
        state->config.satelliteServiceId > 0 &&
        !transcodedInput;
    const bool needsRemux = (outputConfig.remapEnabled || forceFtaSatelliteServiceRemux) && !transcodedInput;
    if (needsRemux) {
        if (forceFtaSatelliteServiceRemux) {
            std::cerr << "FTA single-service remux: stream=" << state->config.id
                      << " SID=" << state->config.satelliteServiceId
                      << " video_pid=" << state->config.videoPid
                      << " audio_pid=" << state->config.audioPid
                      << " branch=" << branchIndex << std::endl;
        }
        return buildRemapPipeline(state, pipeline, sourceTail, outputConfig, branchIndex);
    }
    return buildPassthroughPipeline(state, pipeline, sourceTail, outputConfig, branchIndex);
}

bool StreamManager::buildPassthroughPipeline(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    if (!state) {
        return false;
    }
    const StreamConfig& cfg = outputConfig;
    GstElement* tsparse = gst_element_factory_make("tsparse", branchName("tsparse", branchIndex).c_str());
    GstElement* queue = gst_element_factory_make("queue", branchName("output_queue", branchIndex).c_str());
    const bool cbrPacingActive = !isUdpOutput(cfg) && cbrMuxEnabled(cfg);
    GstElement* pacer = cbrPacingActive
        ? gst_element_factory_make("identity", branchName("cbr_pacer", branchIndex).c_str())
        : nullptr;
    GstElement* sink = createOutputSink(cfg, pipeline, branchName("output_sink", branchIndex));

    if (!tsparse || !queue || !sink || (cbrPacingActive && !pacer)) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, queue) ||
        (pacer && !addElementOrFail(pipeline, pacer))) {
        return false;
    }

    configureOutputQueue(queue, cfg);
    configureCbrPacer(pacer, cfg);
    configureTsPacketAlignment(tsparse);
    if (isUdpOutput(cfg)) {
        setBooleanPropertyIfPresent(tsparse, "set-timestamps", TRUE);
        setUInt64PropertyIfPresent(tsparse, "smoothing-latency", kTsSmoothingLatency);
    }

    return pacer
        ? gst_element_link_many(sourceTail, tsparse, queue, pacer, sink, nullptr)
        : gst_element_link_many(sourceTail, tsparse, queue, sink, nullptr);
}

bool StreamManager::buildRemapPipeline(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    if (!state) {
        return false;
    }
    if (!hasElementFactory("tsparse") || !hasElementFactory("tsdemux") || !hasElementFactory("mpegtsmux")) {
        std::cerr << "missing remap elements: tsparse, tsdemux or mpegtsmux" << std::endl;
        return false;
    }

    const StreamConfig& cfg = outputConfig;
    GstElement* tsparse = gst_element_factory_make("tsparse", branchName("remap_tsparse", branchIndex).c_str());
    GstElement* preDemuxQueue = gst_element_factory_make("queue", branchName("remap_pre_demux_queue", branchIndex).c_str());
    GstElement* demux = gst_element_factory_make("tsdemux", branchName("demux", branchIndex).c_str());
    GstElement* mux = gst_element_factory_make("mpegtsmux", branchName("mux", branchIndex).c_str());
    const bool cbrActive = !isUdpOutput(cfg) && cbrMuxEnabled(cfg);
    GstElement* outputQueue = gst_element_factory_make("queue", branchName("output_queue", branchIndex).c_str());
    GstElement* pacer = cbrActive ? gst_element_factory_make("identity", branchName("cbr_pacer", branchIndex).c_str()) : nullptr;
    GstElement* sink = createOutputSink(cfg, pipeline, branchName("output_sink", branchIndex));
    if (!tsparse || !preDemuxQueue || !demux || !mux || !outputQueue || !sink ||
        (cbrActive && !pacer)) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, preDemuxQueue) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, outputQueue) ||
        (pacer && !addElementOrFail(pipeline, pacer))) {
        return false;
    }

    configureQueue(preDemuxQueue);
    configureOutputQueue(outputQueue, cfg);
    configureCbrPacer(pacer, cfg);
    configureTsMux(mux, cfg);
    sendServiceDescription(mux, cfg);

    // v94: explicitly select the scanned DVB service in tsdemux.
    // program-numbers on dvbbasebin is not sufficient on every driver/plugin
    // combination; some frontends still expose the whole transport stream.
    // The output branch therefore filters by the actual scanned satellite SID
    // before linking elementary video/audio streams into the new mpegtsmux.
    if (state->config.satelliteEnabled && state->config.satelliteServiceId > 0) {
        setIntPropertyIfPresent(
            demux,
            "program-number",
            static_cast<gint>(state->config.satelliteServiceId));
        std::cerr << "FTA/DVB service filter configured: SID="
                  << state->config.satelliteServiceId
                  << " branch=" << branchIndex << std::endl;
    }

    if (!gst_element_link_many(sourceTail, tsparse, preDemuxQueue, demux, nullptr)) {
        return false;
    }
    const bool outputLinked = pacer
        ? gst_element_link_many(mux, outputQueue, pacer, sink, nullptr)
        : gst_element_link_many(mux, outputQueue, sink, nullptr);
    if (!outputLinked) {
        return false;
    }

    auto context = std::make_unique<RemapContext>();
    context->mux = mux;
    context->sink = sink;
    context->config = cfg;
    RemapContext* contextPtr = context.get();
    state->outputContexts.push_back(std::move(context));
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), contextPtr);
    return true;
}

bool StreamManager::buildRtmpOutputPipeline(
    StreamState* state,
    GstElement* pipeline,
    GstElement* sourceTail,
    const StreamConfig& outputConfig,
    size_t branchIndex) {
    if (!state) {
        return false;
    }
    if (!hasElementFactory("tsparse") || !hasElementFactory("tsdemux") || !hasElementFactory("flvmux")) {
        std::cerr << "missing RTMP output elements: tsparse, tsdemux or flvmux" << std::endl;
        return false;
    }

    const StreamConfig& cfg = outputConfig;
    GstElement* tsparse = gst_element_factory_make("tsparse", branchName("rtmp_tsparse", branchIndex).c_str());
    GstElement* preDemuxQueue = gst_element_factory_make("queue", branchName("rtmp_pre_demux_queue", branchIndex).c_str());
    GstElement* demux = gst_element_factory_make("tsdemux", branchName("rtmp_ts_demux", branchIndex).c_str());
    GstElement* mux = gst_element_factory_make("flvmux", branchName("rtmp_flv_mux", branchIndex).c_str());
    GstElement* outputQueue = gst_element_factory_make("queue", branchName("output_queue", branchIndex).c_str());
    GstElement* sink = createOutputSink(cfg, pipeline, branchName("output_sink", branchIndex));
    if (!tsparse || !preDemuxQueue || !demux || !mux || !outputQueue || !sink) {
        return false;
    }

    if (!addElementOrFail(pipeline, tsparse) ||
        !addElementOrFail(pipeline, preDemuxQueue) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, outputQueue)) {
        return false;
    }

    configureQueue(preDemuxQueue);
    configureQueue(outputQueue);
    configureTsPacketAlignment(tsparse);
    g_object_set(mux,
        "streamable", TRUE,
        "enforce-increasing-timestamps", TRUE,
        "skip-backwards-streams", TRUE,
        nullptr);

    if (!gst_element_link_many(sourceTail, tsparse, preDemuxQueue, demux, nullptr)) {
        return false;
    }
    if (!gst_element_link_many(mux, outputQueue, sink, nullptr)) {
        return false;
    }

    auto context = std::make_unique<RemapContext>();
    context->mux = mux;
    context->sink = sink;
    context->config = cfg;
    context->flvMux = true;
    RemapContext* contextPtr = context.get();
    state->outputContexts.push_back(std::move(context));
    g_signal_connect(demux, "pad-added", G_CALLBACK(StreamManager::onDemuxPadAdded), contextPtr);
    return true;
}

GstElement* StreamManager::createOutputSink(const StreamConfig& cfg, GstElement* pipeline, const std::string& sinkName) {
    const std::string type = outputType(cfg);
    const auto outputProtocol = tvs::stream_protocols::outputKind(cfg);
    if (outputProtocol == tvs::stream_protocols::OutputProtocolKind::Unknown) {
        std::cerr << "unknown output protocol module for type: " << type << std::endl;
        return nullptr;
    }
    if (isUdpOutputType(type)) {
        std::string error;
        GstElement* sink = udpCbrOutputEnabled(cfg)
            ? UdpCbrOutput::createSink(pipeline, cfg, sinkName, error)
            : UdpVbrOutput::createSink(pipeline, cfg, sinkName, error);
        if (!sink) {
            std::cerr << error << std::endl;
        }
        return sink;
    }

    const char* factory = "srtsink";
    if (type == "http") {
        factory = "multifdsink";
    } else if (type == "hls") {
        factory = "hlssink";
    } else if (type == "rtmp" || type == "youtube") {
        factory = "rtmpsink";
    }

    if (!hasElementFactory(factory)) {
        std::cerr << missingElementStatus(factory) << std::endl;
        return nullptr;
    }

    GstElement* sink = gst_element_factory_make(factory, sinkName.c_str());
    if (!sink || !addElementOrFail(pipeline, sink)) {
        return nullptr;
    }

    if (type == "srt") {
        attachSrtConnectionMonitoring(sink, cfg);
        configureSrtSink(sink, cfg, configManager.subscribers.filteringEnabled);
    } else if (type == "http") {
        configureHttpSink(sink, cfg);
    } else if (type == "hls") {
        configureHlsSink(sink, cfg);
    } else if (type == "rtmp" || type == "youtube") {
        configureRtmpSink(sink, cfg);
    }

    return sink;
}

void StreamManager::onDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data) {
    (void)demux;
    auto* ctx = static_cast<RemapContext*>(user_data);
    if (!ctx || !ctx->mux) {
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    std::string capsString = caps ? gst_caps_to_string(caps) : "unknown";
    bool isAudio = capsString.find("audio/") != std::string::npos;
    bool isVideo = capsString.find("video/") != std::string::npos;
    bool isPrivateTs = capsString.find("private") != std::string::npos || capsString.find("subpicture") != std::string::npos;

    std::cerr << "tsdemux pad detected: caps=" << capsString
              << " audio=" << (isAudio ? "yes" : "no")
              << " video=" << (isVideo ? "yes" : "no")
              << " private=" << (isPrivateTs ? "yes" : "no") << std::endl;

    if ((!isAudio && !isVideo) || isPrivateTs) {
        if (caps) gst_caps_unref(caps);
        drainDynamicPad(ctx->mux, pad);
        return;
    }
    if ((isVideo && ctx->videoLinked) || (isAudio && ctx->audioLinked)) {
        if (caps) gst_caps_unref(caps);
        drainDynamicPad(ctx->mux, pad);
        return;
    }

    std::string parserFactory = parserForCaps(caps, capsString);
    if (caps) {
        gst_caps_unref(caps);
        caps = nullptr;
    }
    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* parser = parserFactory.empty() ? nullptr : gst_element_factory_make(parserFactory.c_str(), nullptr);
    GstElement* capsfilter = capsFilterForMux(ctx->flvMux, isVideo, isAudio, capsString, parserFactory);

    if (!queue || !parser) {
        std::cerr << "remap skipped unsupported elementary stream caps: " << capsString << std::endl;
        if (queue) gst_object_unref(queue);
        if (parser) gst_object_unref(parser);
        if (capsfilter) gst_object_unref(capsfilter);
        drainDynamicPad(ctx->mux, pad);
        return;
    }

    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(ctx->mux));
    if (!pipeline) {
        gst_object_unref(queue);
        return;
    }

    if (!gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), parser) ||
        (capsfilter && !gst_bin_add(GST_BIN(pipeline), capsfilter))) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (parser && !GST_OBJECT_PARENT(parser)) gst_object_unref(parser);
        if (capsfilter && !GST_OBJECT_PARENT(capsfilter)) gst_object_unref(capsfilter);
        gst_object_unref(pipeline);
        return;
    }

    configureQueue(queue);
    if (parserFactory == "h264parse" || parserFactory == "h265parse") {
        g_object_set(parser, "config-interval", 1, nullptr);
    }
    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(parser);
    if (capsfilter) {
        gst_element_sync_state_with_parent(capsfilter);
    }

    const bool parserLinked = capsfilter
        ? gst_element_link_many(queue, parser, capsfilter, nullptr)
        : gst_element_link(queue, parser);
    if (!parserLinked) {
        gst_object_unref(pipeline);
        drainDynamicPad(ctx->mux, pad);
        return;
    }

    GstPad* queueSinkPad = gst_element_get_static_pad(queue, "sink");
    if (!queueSinkPad) {
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(pad, queueSinkPad) != GST_PAD_LINK_OK) {
        gst_object_unref(queueSinkPad);
        gst_object_unref(pipeline);
        drainDynamicPad(ctx->mux, pad);
        return;
    }
    gst_object_unref(queueSinkPad);

    uint32_t requestedPid = isVideo ? ctx->config.videoPid : ctx->config.audioPid;
    if (requestedPid == 0) {
        requestedPid = pidFromDemuxPadName(pad);
    }

    GstElement* muxSourceElement = capsfilter ? capsfilter : parser;
    GstPad* parserSrcPad = gst_element_get_static_pad(muxSourceElement, "src");
    GstPad* muxSinkPad = ctx->flvMux
        ? requestFlvMuxSinkPad(ctx->mux, isVideo)
        : requestMuxSinkPad(ctx->mux, requestedPid);
    if (!parserSrcPad || !muxSinkPad) {
        if (parserSrcPad) gst_object_unref(parserSrcPad);
        if (muxSinkPad) gst_object_unref(muxSinkPad);
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(parserSrcPad, muxSinkPad) == GST_PAD_LINK_OK) {
        std::cerr << "remap linked " << (isAudio ? "audio" : "video")
                  << " caps=" << capsString << " parser=" << parserFactory
                  << " pid=" << requestedPid << std::endl;
        const gchar* padName = GST_PAD_NAME(muxSinkPad);
        if (isVideo) {
            ctx->videoLinked = true;
            ctx->videoPadName = padName ? padName : "";
        }
        if (isAudio) {
            ctx->audioLinked = true;
            ctx->audioPadName = padName ? padName : "";
        }
        updateMuxProgramMap(ctx);
    }

    gst_object_unref(parserSrcPad);
    gst_object_unref(muxSinkPad);
    gst_object_unref(pipeline);
}

void StreamManager::onFlvDemuxPadAdded(GstElement* demux, GstPad* pad, gpointer user_data) {
    onDemuxPadAdded(demux, pad, user_data);
}

void StreamManager::onRtspPadAdded(GstElement* src, GstPad* pad, gpointer user_data) {
    (void)src;
    auto* ctx = static_cast<RemapContext*>(user_data);
    if (!ctx || !ctx->mux) {
        return;
    }

    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (!caps) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    std::string capsString = caps ? gst_caps_to_string(caps) : "unknown";
    if (caps) {
        gst_caps_unref(caps);
    }

    RtspPayloadFactories factories = rtspPayloadFactories(capsString);
    if (!factories.depay || !factories.parser) {
        std::cerr << "RTSP skipped unsupported RTP caps: " << capsString << std::endl;
        return;
    }
    if ((factories.isVideo && ctx->videoLinked) || (factories.isAudio && ctx->audioLinked)) {
        return;
    }
    if (!hasElementFactory(factories.depay) || !hasElementFactory(factories.parser)) {
        std::cerr << "missing RTSP payload elements: " << factories.depay
                  << " or " << factories.parser << std::endl;
        return;
    }

    GstElement* pipeline = GST_ELEMENT(gst_element_get_parent(ctx->mux));
    if (!pipeline) {
        return;
    }

    GstElement* queue = gst_element_factory_make("queue", nullptr);
    GstElement* depay = gst_element_factory_make(factories.depay, nullptr);
    GstElement* parser = gst_element_factory_make(factories.parser, nullptr);
    GstElement* capsfilter = makeCapsFilter(factories.muxCaps);
    if (!queue || !depay || !parser) {
        if (queue) gst_object_unref(queue);
        if (depay) gst_object_unref(depay);
        if (parser) gst_object_unref(parser);
        if (capsfilter) gst_object_unref(capsfilter);
        gst_object_unref(pipeline);
        return;
    }

    if (!gst_bin_add(GST_BIN(pipeline), queue) ||
        !gst_bin_add(GST_BIN(pipeline), depay) ||
        !gst_bin_add(GST_BIN(pipeline), parser) ||
        (capsfilter && !gst_bin_add(GST_BIN(pipeline), capsfilter))) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        if (depay && !GST_OBJECT_PARENT(depay)) gst_object_unref(depay);
        if (parser && !GST_OBJECT_PARENT(parser)) gst_object_unref(parser);
        if (capsfilter && !GST_OBJECT_PARENT(capsfilter)) gst_object_unref(capsfilter);
        gst_object_unref(pipeline);
        return;
    }

    configureQueue(queue, 5000000000ULL);
    if (g_strcmp0(factories.parser, "h264parse") == 0 || g_strcmp0(factories.parser, "h265parse") == 0) {
        g_object_set(parser, "config-interval", 1, nullptr);
    }

    gst_element_sync_state_with_parent(queue);
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(parser);
    if (capsfilter) {
        gst_element_sync_state_with_parent(capsfilter);
    }

    const bool parserLinked = capsfilter
        ? gst_element_link_many(queue, depay, parser, capsfilter, nullptr)
        : gst_element_link_many(queue, depay, parser, nullptr);
    if (!parserLinked) {
        gst_object_unref(pipeline);
        return;
    }

    GstPad* queueSinkPad = gst_element_get_static_pad(queue, "sink");
    if (!queueSinkPad) {
        gst_object_unref(pipeline);
        return;
    }
    if (gst_pad_link(pad, queueSinkPad) != GST_PAD_LINK_OK) {
        gst_object_unref(queueSinkPad);
        gst_object_unref(pipeline);
        return;
    }
    gst_object_unref(queueSinkPad);

    const uint32_t requestedPid = factories.isVideo ? ctx->config.videoPid : ctx->config.audioPid;
    GstElement* muxSourceElement = capsfilter ? capsfilter : parser;
    GstPad* parserSrcPad = gst_element_get_static_pad(muxSourceElement, "src");
    GstPad* muxSinkPad = requestMuxSinkPad(ctx->mux, requestedPid);
    if (!parserSrcPad || !muxSinkPad) {
        if (parserSrcPad) gst_object_unref(parserSrcPad);
        if (muxSinkPad) gst_object_unref(muxSinkPad);
        gst_object_unref(pipeline);
        return;
    }

    if (gst_pad_link(parserSrcPad, muxSinkPad) == GST_PAD_LINK_OK) {
        std::cerr << "RTSP remap linked " << (factories.isAudio ? "audio" : "video")
                  << " pid=" << requestedPid << std::endl;
        const gchar* padName = GST_PAD_NAME(muxSinkPad);
        if (factories.isVideo) {
            ctx->videoLinked = true;
            ctx->videoPadName = padName ? padName : "";
        }
        if (factories.isAudio) {
            ctx->audioLinked = true;
            ctx->audioPadName = padName ? padName : "";
        }
        updateMuxProgramMap(ctx);
    }

    gst_object_unref(parserSrcPad);
    gst_object_unref(muxSinkPad);
    gst_object_unref(pipeline);
}

void StreamManager::attachBitrateProbes(StreamState* state) {
    if (!state || !state->pipeline) {
        return;
    }

    gboolean inputAttached = FALSE;
    gboolean outputAttached = FALSE;

    GstElement* inputQueue = gst_bin_get_by_name(GST_BIN(state->pipeline), "input_queue");
    if (inputQueue) {
        GstPad* sinkPad = gst_element_get_static_pad(inputQueue, "sink");
        if (sinkPad) {
            gst_pad_add_probe(
                sinkPad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                inputPadProbe,
                state,
                nullptr);
            gst_object_unref(sinkPad);
            inputAttached = TRUE;
        }
        gst_object_unref(inputQueue);
    }

    // For streams with multiple outputs, measure and validate the common
    // MPEG-TS only once before output_tee. Attaching the same continuity
    // tracker to every output_queue makes each packet appear repeatedly and
    // produces false output CC errors and an aggregate bitrate multiplied by
    // the number of branches.
    GstElement* outputTee = gst_bin_get_by_name(GST_BIN(state->pipeline), "output_tee");
    if (outputTee) {
        GstPad* sinkPad = gst_element_get_static_pad(outputTee, "sink");
        if (sinkPad) {
            gst_pad_add_probe(
                sinkPad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                outputPadProbe,
                state,
                nullptr);
            gst_object_unref(sinkPad);
            outputAttached = TRUE;
        }
        gst_object_unref(outputTee);
    }

    // A single-output pipeline has no output_tee, so retain the existing
    // output_queue probe in that case. Stop after the first queue to guarantee
    // that one logical TS stream is counted exactly once.
    if (!outputAttached) {
        GstIterator* outputIterator = gst_bin_iterate_elements(GST_BIN(state->pipeline));
        GValue outputItem = G_VALUE_INIT;
        while (gst_iterator_next(outputIterator, &outputItem) == GST_ITERATOR_OK) {
            GstElement* element = GST_ELEMENT(g_value_get_object(&outputItem));
            const gchar* name = GST_ELEMENT_NAME(element);
            if (name && g_str_has_prefix(name, "output_queue")) {
                GstPad* srcPad = gst_element_get_static_pad(element, "src");
                if (srcPad) {
                    gst_pad_add_probe(
                        srcPad,
                        static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                        outputPadProbe,
                        state,
                        nullptr);
                    gst_object_unref(srcPad);
                    outputAttached = TRUE;
                }
            }
            g_value_unset(&outputItem);
            if (outputAttached) {
                break;
            }
        }
        gst_iterator_free(outputIterator);
    }

    GstIterator* iterator = gst_bin_iterate_elements(GST_BIN(state->pipeline));
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(iterator, &item) == GST_ITERATOR_OK) {
        GstElement* element = GST_ELEMENT(g_value_get_object(&item));
        GstElementFactory* factory = gst_element_get_factory(element);
        const gchar* factoryName = factory
            ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
            : nullptr;

        if (!inputAttached && factoryName &&
            (g_strcmp0(factoryName, "srtsrc") == 0 ||
             g_strcmp0(factoryName, "srtclientsrc") == 0 ||
             g_strcmp0(factoryName, "rtspsrc") == 0 ||
             g_strcmp0(factoryName, "rtmpsrc") == 0 ||
             g_strcmp0(factoryName, "souphttpsrc") == 0 ||
             g_strcmp0(factoryName, "udpsrc") == 0 ||
             g_strcmp0(factoryName, "filesrc") == 0)) {
            GstPad* srcPad = gst_element_get_static_pad(element, "src");
            if (srcPad) {
                gst_pad_add_probe(
                    srcPad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                    inputPadProbe,
                    state,
                    nullptr);
                gst_object_unref(srcPad);
                inputAttached = TRUE;
            }
        }

        if (!outputAttached && factoryName &&
            (g_strcmp0(factoryName, "udpsink") == 0 ||
             g_strcmp0(factoryName, "srtsink") == 0 ||
             g_strcmp0(factoryName, "rtmpsink") == 0 ||
             g_strcmp0(factoryName, "multifdsink") == 0 ||
             g_strcmp0(factoryName, "hlssink") == 0 ||
             g_strcmp0(factoryName, "appsink") == 0)) {
            GstPad* sinkPad = gst_element_get_static_pad(element, "sink");
            if (sinkPad) {
                gst_pad_add_probe(
                    sinkPad,
                    static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
                    outputPadProbe,
                    state,
                    nullptr);
                gst_object_unref(sinkPad);
                outputAttached = TRUE;
            }
        }

        g_value_unset(&item);
        if (inputAttached && outputAttached) {
            break;
        }
    }

    gst_iterator_free(iterator);
}

void StreamManager::updateBitrateEstimates(StreamState* state) {
    if (!state) {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - state->lastBitrateSample).count();
    if (elapsedMs < 1000) {
        return;
    }

    uint64_t currentInputBytes = state->inputBytes.load();
    uint64_t currentOutputBytes = state->outputBytes.load();
    uint64_t currentInputCcErrors = state->inputCcErrors.load();
    uint64_t currentOutputCcErrors = state->outputCcErrors.load();
    uint64_t inputDelta = currentInputBytes - state->lastInputBytesSample;
    uint64_t outputDelta = currentOutputBytes - state->lastOutputBytesSample;
    uint64_t inputCcDelta = currentInputCcErrors - state->lastInputCcErrorsSample;
    uint64_t outputCcDelta = currentOutputCcErrors - state->lastOutputCcErrorsSample;
    double seconds = static_cast<double>(elapsedMs) / 1000.0;

    state->inputBitrate = static_cast<uint64_t>((inputDelta * 8) / seconds);
    const uint64_t measuredOutputBitrate = static_cast<uint64_t>((outputDelta * 8) / seconds);
    state->outputBitrate = udpCbrOutputEnabled(state->config) && state->config.targetBitrate > 0
        ? state->config.targetBitrate
        : measuredOutputBitrate;
    state->inputCcErrorsDelta = inputCcDelta;
    state->outputCcErrorsDelta = outputCcDelta;

    state->lastInputBytesSample = currentInputBytes;
    state->lastOutputBytesSample = currentOutputBytes;
    state->lastInputCcErrorsSample = currentInputCcErrors;
    state->lastOutputCcErrorsSample = currentOutputCcErrors;
    state->lastBitrateSample = now;
}
GstPadProbeReturn StreamManager::inputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    (void)pad;
    auto* state = static_cast<StreamState*>(user_data);
    if (!state) {
        return GST_PAD_PROBE_OK;
    }

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer) {
            state->inputBytes.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
            updateInputContinuityErrors(state, buffer);
        }
    } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList* list = gst_pad_probe_info_get_buffer_list(info);
        state->inputBytes.fetch_add(bufferListSize(list), std::memory_order_relaxed);
        updateInputContinuityErrors(state, list);
    }

    return GST_PAD_PROBE_OK;
}

GstPadProbeReturn StreamManager::outputPadProbe(GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
    (void)pad;
    auto* state = static_cast<StreamState*>(user_data);
    if (!state) {
        return GST_PAD_PROBE_OK;
    }

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer* buffer = gst_pad_probe_info_get_buffer(info);
        if (buffer) {
            state->outputBytes.fetch_add(gst_buffer_get_size(buffer), std::memory_order_relaxed);
            updateOutputContinuityErrors(state, buffer);
        }
    } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList* list = gst_pad_probe_info_get_buffer_list(info);
        state->outputBytes.fetch_add(bufferListSize(list), std::memory_order_relaxed);
        updateOutputContinuityErrors(state, list);
    }

    return GST_PAD_PROBE_OK;
}

void StreamManager::monitorBus(const std::string& id) {
    StreamState* state = nullptr;
    {
        std::lock_guard<std::mutex> lock(managerMutex);
        auto found = streams.find(id);
        if (found == streams.end()) {
            return;
        }
        state = found->second.get();
    }
    GstBus* bus = state->bus;

    if (state->gstTranscoder && !state->pipeline) {
        auto lastSyntheticSample = std::chrono::steady_clock::now();
        while (state->running.load()) {
            auto now = std::chrono::steady_clock::now();
            if (!state->gstTranscoder->isRunning()) {
                state->statusMessage = "error: gstreamer transcoder exited";
                state->active = false;
                notifyStreamState(
                    state->config,
                    "🔴",
                    telegramText(configManager, "Ошибка GStreamer-транскодера", "GStreamer transcoder error"),
                    telegramText(configManager, "Процесс gst-launch завершился", "gst-launch process exited"));
                return;
            }

            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSyntheticSample).count();
            if (elapsedMs >= 1000) {
                const double seconds = static_cast<double>(elapsedMs) / 1000.0;
                const uint64_t inputEstimate = transcodeInputBitrateForStats(state->config);
                const uint64_t outputEstimate = transcodeMuxBitrateForStats(state->config);
                state->inputBitrate = inputEstimate;
                state->outputBitrate = outputEstimate;
                state->inputBytes.fetch_add(static_cast<uint64_t>((inputEstimate * seconds) / 8.0), std::memory_order_relaxed);
                state->outputBytes.fetch_add(static_cast<uint64_t>((outputEstimate * seconds) / 8.0), std::memory_order_relaxed);
                state->lastInputActivity = now;
                state->lastBitrateSample = now;
                state->statusMessage = "running via gstreamer";
                lastSyntheticSample = now;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        return;
    }

    if (!bus) {
        return;
    }

    while (state->running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (state->gstTranscoder) {
            if (!state->gstTranscoder->isRunning()) {
                state->statusMessage = "error: gstreamer transcoder exited";
                state->active = false;
                notifyStreamState(
                    state->config,
                    "🔴",
                    telegramText(configManager, "Ошибка GStreamer-транскодера", "GStreamer transcoder error"),
                    telegramText(configManager, "Процесс gst-launch завершился", "gst-launch process exited"));
                return;
            }
            // The external transcoder owns the original input socket. Treat a live process as
            // input activity; its dedicated monitor loop updates the synthetic input counters.
            state->lastInputActivity = now;
        }
        // v92: shared DVB-S2 liveness is based on bytes emitted by the
        // per-service relay itself.  Seeing tsdemux pads means the service was
        // found, and bytes on this probe mean the selected SID is actively
        // being remuxed toward the loopback UDP source.  Do not restart that
        // healthy relay just because the generic downstream input probe did
        // not advance.
        if (state->sharedSatelliteInput && state->satelliteServiceRelay) {
            const uint64_t relayBytes = state->satelliteServiceRelay->outputBytes.load(std::memory_order_relaxed);
            if (relayBytes != state->lastSatelliteRelayBytesSeen) {
                state->lastSatelliteRelayBytesSeen = relayBytes;
                state->lastInputActivity = now;
                if (state->inputLossNotified && !state->usingBackup) {
                    state->inputLossNotified = false;
                    state->statusMessage = "running (DVB-S2 service relay active)";
                }
            }
        }

        const uint64_t currentInputBytes = state->inputBytes.load();
        if (currentInputBytes != state->lastInputBytesSeen) {
            state->lastInputBytesSeen = currentInputBytes;
            state->lastInputActivity = now;
            if (state->inputLossNotified && !state->usingBackup && !state->primaryRetryPending) {
                state->statusMessage = "running";
                notifyStreamState(
                    state->config,
                    "🟢",
                    telegramText(configManager, "Входной сигнал восстановлен", "Input signal restored"),
                    telegramText(configManager, "Активный источник: основной", "Active source: primary") + "\nURL: " + state->activeInputUri);
            }
            state->inputLossNotified = false;
            if (state->primaryRetryPending && !state->usingBackup) {
                state->primaryRetryPending = false;
                state->backupAttempted = false;
                state->statusMessage = "running on primary";
                notifyStreamState(
                    state->config,
                    "🟢",
                    telegramText(configManager, "Основной поток восстановлен", "Primary stream restored"),
                    telegramText(configManager, "Активный источник: основной", "Active source: primary") + "\nURL: " + state->activeInputUri);
            }
        }

        if (!state->config.testPattern) {
            const bool inputTimedOut = now - state->lastInputActivity >= kInputFailoverDelay;
            const bool sharedSatellitePrimary = state->config.satelliteEnabled && state->sharedSatelliteInput &&
                state->satelliteServiceRelay != nullptr && !state->usingBackup;
            const bool excessiveInputCcErrors = state->inputCcErrorsDelta.load(std::memory_order_relaxed) >= kInputCcRecoveryThreshold;
            if (sharedSatellitePrimary && !state->satelliteRelayRecoveryDisabled &&
                state->config.backupInputUri.empty() &&
                (inputTimedOut || excessiveInputCcErrors) &&
                now - state->lastSatelliteRelayRestart >= kSatelliteRelayRestartInterval) {
                std::string recoveryError;
                const std::string reason = inputTimedOut ? "input timeout" : "continuity errors";
                if (restartSharedSatelliteInput(state, reason, recoveryError)) {
                    bus = state->bus;
                    state->active = true;
                    state->inputLossNotified = false;
                    notifyStreamState(
                        state->config,
                        "🟢",
                        telegramText(configManager, "Спутниковый relay восстановлен", "Satellite relay recovered"),
                        telegramText(configManager, "Перезапущен внутренний DVB service relay", "Internal DVB service relay restarted") +
                            "\n" + telegramText(configManager, "Причина", "Reason") + ": " + reason);
                    continue;
                } else if (!state->inputLossNotified) {
                    state->inputLossNotified = true;
                    state->statusMessage = "satellite relay recovery failed: " + recoveryError;
                    notifyStreamState(
                        state->config,
                        "🟡",
                        telegramText(configManager, "Не удалось восстановить спутниковый relay", "Satellite relay recovery failed"),
                        telegramText(configManager, "Причина", "Reason") + ": " + recoveryError);
                }
            }
            if (inputTimedOut && !state->usingBackup && !state->config.backupInputUri.empty()) {
                notifyStreamState(
                    state->config,
                    "🟡",
                    telegramText(configManager, "Основной поток пропал", "Primary stream lost"),
                    telegramText(configManager, "Нет входных данных 5 секунд", "No input data for 5 seconds") +
                        "\n" + telegramText(configManager, "Переключаюсь на резерв", "Switching to backup") +
                        "\nBackup: " + state->config.backupInputUri);
                if (restartPipelineWithInput(state, state->config.backupInputUri, true)) {
                    bus = state->bus;
                    state->inputLossNotified = false;
                    notifyStreamState(
                        state->config,
                        "🟠",
                        telegramText(configManager, "Работаю с резервного источника", "Running from backup source"),
                        telegramText(configManager, "Активный источник: резерв", "Active source: backup") + "\nURL: " + state->activeInputUri);
                } else {
                    state->inputLossNotified = true;
                    notifyStreamState(
                        state->config,
                        "🔴",
                        telegramText(configManager, "Не удалось включить резерв", "Failed to start backup"),
                        telegramText(configManager, "Backup pipeline не стартовал", "Backup pipeline did not start") +
                            "\nBackup: " + state->config.backupInputUri);
                }
            } else if (state->usingBackup && now - state->lastPrimaryRetry >= kPrimaryRetryInterval) {
                const std::string primaryUri = state->primaryInputUri;
                state->lastPrimaryRetry = now;

                if (!primaryUri.empty()) {
                    // Probe the primary input with an independent temporary pipeline.
                    // The active backup-file pipeline keeps playing uninterrupted while
                    // availability is checked. Switch only after real media data arrives.
                    StreamConfig primaryProbeConfig = state->config;
                    std::string primaryProbeUri = primaryUri;
                    if (state->sharedSatelliteInput) {
                        primaryProbeConfig.satelliteEnabled = false;
                        primaryProbeConfig.inputUri = state->satelliteServiceRelayUri;
                        primaryProbeConfig.inputMode = "udp";
                        if (state->config.satelliteEnabled && !state->config.satelliteScrambled &&
                            state->satelliteServiceRelay == nullptr) {
                            primaryProbeConfig.inputInterfaceAddress = "127.0.0.1";
                        } else {
                            primaryProbeConfig.inputInterfaceAddress.clear();
                        }
                        primaryProbeConfig.inputInterfaceAddressConfigured = true;
                        primaryProbeUri = state->satelliteServiceRelayUri;
                    } else {
                        primaryProbeConfig.satelliteEnabled = state->primarySatelliteEnabled;
                    }
                    if (probeInputAvailable(primaryProbeConfig, primaryProbeUri, kInputFailoverDelay)) {
                        notifyStreamState(
                            state->config,
                            "🟢",
                            telegramText(configManager, "Основной поток снова доступен", "Primary stream is available again"),
                            telegramText(configManager, "Переключаюсь с файла подмены на основной источник", "Switching from the replacement file to the primary source") +
                                "\nURL: " + primaryUri);
                        if (restartPipelineWithInput(state, primaryUri, false)) {
                            bus = state->bus;
                            state->inputLossNotified = false;
                        }
                    }
                }
            } else if (inputTimedOut && !state->usingBackup && state->primaryRetryPending && !state->config.backupInputUri.empty()) {
                notifyStreamState(
                    state->config,
                    "🟡",
                    telegramText(configManager, "Основной пока недоступен", "Primary is still unavailable"),
                    telegramText(configManager, "Возвращаюсь на резервный источник", "Returning to backup source") +
                        "\nBackup: " + state->config.backupInputUri);
                if (restartPipelineWithInput(state, state->config.backupInputUri, true)) {
                    bus = state->bus;
                    state->inputLossNotified = false;
                }
            } else if (inputTimedOut && !state->usingBackup && state->config.backupInputUri.empty() &&
                       state->caProviderTransport && now - state->lastPrimaryRetry >= kPrimaryRetryInterval) {
                state->lastPrimaryRetry = now;
                state->statusMessage = "reconnecting authorized TS provider";
                if (restartPipelineWithInput(state, state->primaryInputUri, false)) {
                    bus = state->bus;
                    state->active = true;
                    state->inputLossNotified = false;
                    state->statusMessage = "running via authorized TS provider";
                    notifyStreamState(
                        state->config,
                        "🟢",
                        telegramText(configManager, "CA Provider переподключён", "CA Provider reconnected"),
                        telegramText(configManager, "Авторизованный TS endpoint снова активен", "Authorized TS endpoint is active again") +
                            "\nURL: " + state->primaryInputUri);
                } else if (!state->inputLossNotified) {
                    state->inputLossNotified = true;
                    notifyStreamState(
                        state->config,
                        "🟡",
                        telegramText(configManager, "CA Provider недоступен", "CA Provider unavailable"),
                        telegramText(configManager, "Повтор подключения через 10 секунд", "Retrying in 10 seconds") +
                            "\nURL: " + state->primaryInputUri);
                }
            } else if (inputTimedOut && !state->usingBackup && state->config.backupInputUri.empty() && !state->inputLossNotified) {
                state->inputLossNotified = true;
                state->statusMessage = state->config.satelliteEnabled && !state->config.satelliteScrambled
                    ? "no input signal (FTA; CA not involved; relay recovery attempted)"
                    : "no input signal";
                notifyStreamState(
                    state->config,
                    "🔴",
                    telegramText(configManager, "Нет входного сигнала", "No input signal"),
                    telegramText(configManager, "Входных данных нет 5 секунд", "No input data for 5 seconds") +
                        "\n" + telegramText(configManager, "Резервная ссылка не задана", "Backup URL is not configured") +
                        "\nURL: " + state->activeInputUri);
            }
        }

        if (!bus || !GST_IS_BUS(bus)) {
            bus = state->bus;
            if (!bus || !GST_IS_BUS(bus)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        GstMessage* msg = gst_bus_timed_pop(bus, 500000000LL);
        if (!msg) {
            continue;
        }

        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                std::string message = err ? err->message : "unknown";
                if (err) {
                    g_error_free(err);
                }
                g_free(dbg);

                state->statusMessage = "error: " + message;
                state->active = false;
                if (state->caProviderTransport) {
                    notifyStreamState(
                        state->config,
                        "🟡",
                        telegramText(configManager, "Ошибка CA Provider transport", "CA Provider transport error"),
                        telegramText(configManager, "Пробую восстановить authorized TS endpoint", "Trying to restore authorized TS endpoint") +
                            "\n" + telegramText(configManager, "Причина", "Reason") + ": " + message);
                    gst_message_unref(msg);
                    if (restartPipelineWithInput(state, state->primaryInputUri, false)) {
                        bus = state->bus;
                        state->active = true;
                        state->inputLossNotified = false;
                        state->statusMessage = "running via authorized TS provider";
                        continue;
                    }
                    state->statusMessage = "authorized TS provider reconnect failed";
                    return;
                }
                notifyStreamState(
                    state->config,
                    "🔴",
                    telegramText(configManager, "Ошибка потока", "Stream error"),
                    telegramText(configManager, "Причина", "Reason") + ": " + message);
                gst_message_unref(msg);
                return;
            }
            case GST_MESSAGE_EOS:
                if (state->usingBackup &&
                    state->config.backupFileLoop &&
                    isBackupFileInput(state->config, state->activeInputUri) &&
                    state->pipeline) {
                    // Recreate the file pipeline after EOS instead of seeking the completed
                    // pipeline. Demuxers such as qtdemux may accept a seek after EOS but stay
                    // drained, which leaves the output running with a black frame.
                    const std::string loopFile = state->activeInputUri;
                    gst_message_unref(msg);
                    if (restartPipelineWithInput(state, loopFile, true)) {
                        bus = state->bus;
                        state->statusMessage = "running on backup file loop";
                        state->active = true;
                        continue;
                    }
                    std::cerr << "Failed to restart backup file loop for stream: " << id << std::endl;
                    state->statusMessage = "error: backup file loop restart failed";
                    state->active = false;
                    notifyStreamState(
                        state->config,
                        "🔴",
                        telegramText(configManager, "Ошибка повтора файла подмены", "Replacement file loop failed"),
                        telegramText(configManager, "Не удалось запустить файл подмены с начала", "Failed to restart the replacement file from the beginning") +
                            "\nFile: " + loopFile);
                    return;
                }
                state->statusMessage = "ended";
                state->active = false;
                notifyStreamState(
                    state->config,
                    "⚫",
                    telegramText(configManager, "Поток завершился", "Stream ended"),
                    telegramText(configManager, "GStreamer получил EOS", "GStreamer received EOS"));
                gst_message_unref(msg);
                return;
            default:
                gst_message_unref(msg);
                break;
        }
    }
}
