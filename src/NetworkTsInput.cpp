#include "NetworkTsInput.h"

#include "StreamManager.h"
#include "protocols/inputs/GstHlsInputProtocol.h"
#include "protocols/inputs/GstHttpInputProtocol.h"
#include "protocols/inputs/GstSrtInputProtocol.h"
#include "protocols/stream/StreamInputProtocol.h"
#include "utils.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

constexpr guint64 kNetworkInputQueue = 3 * GST_SECOND;
constexpr guint64 kHlsInputQueue = 5 * GST_SECOND;
constexpr gint kNetworkSourceTimeoutSeconds = 15;
constexpr gint kSrtLatencyMs = 500;

bool hasElementFactory(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

bool hasProperty(GstElement* element, const char* propertyName) {
    return element &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(element), propertyName) != nullptr;
}

void setBooleanPropertyIfPresent(GstElement* element, const char* propertyName, gboolean value) {
    if (hasProperty(element, propertyName)) g_object_set(element, propertyName, value, nullptr);
}

void setIntPropertyIfPresent(GstElement* element, const char* propertyName, gint value) {
    if (hasProperty(element, propertyName)) g_object_set(element, propertyName, value, nullptr);
}

void setUIntPropertyIfPresent(GstElement* element, const char* propertyName, guint value) {
    if (hasProperty(element, propertyName)) g_object_set(element, propertyName, value, nullptr);
}

void setStringPropertyIfPresent(
    GstElement* element, const char* propertyName, const std::string& value) {
    if (hasProperty(element, propertyName) && !value.empty()) {
        g_object_set(element, propertyName, value.c_str(), nullptr);
    }
}

bool addElementOrFail(GstElement* pipeline, GstElement* element) {
    return pipeline && element && gst_bin_add(GST_BIN(pipeline), element);
}

GstElement* addQueue(GstElement* pipeline, const char* name, guint64 maxSizeTime) {
    GstElement* queue = gst_element_factory_make("queue", name);
    if (!queue || !addElementOrFail(pipeline, queue)) {
        if (queue && !GST_OBJECT_PARENT(queue)) gst_object_unref(queue);
        return nullptr;
    }
    g_object_set(queue,
        "max-size-buffers", 0,
        "max-size-bytes", 0,
        "max-size-time", maxSizeTime,
        "leaky", 0,
        nullptr);
    return queue;
}

std::string configuredInputInterfaceAddress(const StreamConfig& cfg) {
    return cfg.inputInterfaceAddressConfigured
        ? cfg.inputInterfaceAddress
        : cfg.interfaceAddress;
}

std::string appendAccessQuery(const std::string& uri, const StreamConfig& cfg) {
    if (cfg.hlsAccessKeyMode != "query" || cfg.hlsAccessKeyName.empty() ||
        cfg.hlsAccessKeyValue.empty()) {
        return uri;
    }

    gchar* escapedName = g_uri_escape_string(cfg.hlsAccessKeyName.c_str(), nullptr, TRUE);
    gchar* escapedValue = g_uri_escape_string(cfg.hlsAccessKeyValue.c_str(), nullptr, TRUE);
    if (!escapedName || !escapedValue) {
        if (escapedName) g_free(escapedName);
        if (escapedValue) g_free(escapedValue);
        return uri;
    }

    const std::string keyPrefix = std::string(escapedName) + "=";
    const auto queryPos = uri.find('?');
    if (queryPos != std::string::npos) {
        const std::string query = uri.substr(queryPos + 1);
        if (query.rfind(keyPrefix, 0) == 0 ||
            query.find("&" + keyPrefix) != std::string::npos) {
            g_free(escapedName);
            g_free(escapedValue);
            return uri;
        }
    }

    const std::string result = uri + (queryPos == std::string::npos ? "?" : "&") +
        escapedName + "=" + escapedValue;
    g_free(escapedName);
    g_free(escapedValue);
    return result;
}

void configureHttpCredentials(GstElement* element, const StreamConfig& cfg) {
    if (!element) return;
    GstElementFactory* factory = gst_element_get_factory(element);
    const gchar* factoryName = factory
        ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
        : nullptr;
    if (!factoryName ||
        (g_strcmp0(factoryName, "souphttpsrc") != 0 &&
         g_strcmp0(factoryName, "curlhttpsrc") != 0)) {
        return;
    }

    setStringPropertyIfPresent(element, "user-agent", cfg.hlsUserAgent);
    setBooleanPropertyIfPresent(element, "keep-alive", TRUE);
    setIntPropertyIfPresent(element, "timeout", kNetworkSourceTimeoutSeconds);

    if (cfg.hlsAccessKeyMode == "header" && !cfg.hlsAccessKeyName.empty() &&
        !cfg.hlsAccessKeyValue.empty() && hasProperty(element, "extra-headers")) {
        GstStructure* headers = gst_structure_new_empty("extra-headers");
        gst_structure_set(headers,
            cfg.hlsAccessKeyName.c_str(), G_TYPE_STRING, cfg.hlsAccessKeyValue.c_str(),
            nullptr);
        g_object_set(element, "extra-headers", headers, nullptr);
        gst_structure_free(headers);
    }
}

void onHlsChildLocationChanged(GObject* object, GParamSpec*, gpointer userData) {
    auto* cfg = static_cast<StreamConfig*>(userData);
    if (!cfg || cfg->hlsAccessKeyMode != "query" || cfg->hlsAccessKeyValue.empty()) return;
    if (g_object_get_data(object, "tvs-network-hls-query-update")) return;

    gchar* current = nullptr;
    g_object_get(object, "location", &current, nullptr);
    if (!current || !*current) {
        if (current) g_free(current);
        return;
    }

    const std::string updated = appendAccessQuery(current, *cfg);
    if (updated != current) {
        g_object_set_data(object, "tvs-network-hls-query-update", GINT_TO_POINTER(1));
        g_object_set(object, "location", updated.c_str(), nullptr);
        g_object_set_data(object, "tvs-network-hls-query-update", nullptr);
    }
    g_free(current);
}

void configureHlsChildSource(GstElement* element, StreamConfig& cfg) {
    if (!element) return;
    GstElementFactory* factory = gst_element_get_factory(element);
    const gchar* factoryName = factory
        ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))
        : nullptr;
    if (!factoryName ||
        (g_strcmp0(factoryName, "souphttpsrc") != 0 &&
         g_strcmp0(factoryName, "curlhttpsrc") != 0)) {
        return;
    }

    configureHttpCredentials(element, cfg);
    if (cfg.hlsAccessKeyMode == "query" && hasProperty(element, "location")) {
        if (!g_object_get_data(G_OBJECT(element), "tvs-network-hls-location-watch")) {
            g_signal_connect(element, "notify::location",
                G_CALLBACK(onHlsChildLocationChanged), &cfg);
            g_object_set_data(
                G_OBJECT(element), "tvs-network-hls-location-watch", GINT_TO_POINTER(1));
        }
        onHlsChildLocationChanged(G_OBJECT(element), nullptr, &cfg);
    }
}

void onHlsDeepElementAdded(GstBin*, GstBin*, GstElement* element, gpointer userData) {
    auto* ctx = static_cast<RemapContext*>(userData);
    if (!ctx) return;
    configureHlsChildSource(element, ctx->config);
}

GstElement* buildSrt(
    const StreamConfig& cfg,
    GstElement* pipeline,
    GstElement*& terminalElement,
    std::string& error) {
    const std::string mode = toLower(cfg.inputMode) == "listener" ? "listener" : "caller";
    const char* preferredFactory = mode == "listener" ? "srtsrc" : "srtclientsrc";
    const char* factory = preferredFactory;

    // TVStreamer5 uses srtclientsrc for caller mode.  Keep a compatibility
    // fallback for older GStreamer installations which expose only srtsrc.
    if (!hasElementFactory(factory)) {
        if (mode == "caller" && hasElementFactory("srtsrc")) {
            factory = "srtsrc";
            std::cerr << "Network TS input 202.21: srtclientsrc unavailable; "
                      << "falling back to srtsrc caller mode" << std::endl;
        } else {
            error = std::string("missing element: ") + preferredFactory;
            return nullptr;
        }
    }

    GstElement* src = gst_element_factory_make(factory, "input_src");
    GstElement* queue = addQueue(pipeline, "input_queue", kNetworkInputQueue);
    if (!src || !queue || !addElementOrFail(pipeline, src)) {
        if (src && !GST_OBJECT_PARENT(src)) gst_object_unref(src);
        error = "SRT input: failed to create srtsrc/srtclientsrc or queue";
        return nullptr;
    }

    const std::string uri = tvs::protocols::inputs::srtInputUri(cfg);
    g_object_set(src, "uri", uri.c_str(), nullptr);
    setBooleanPropertyIfPresent(src, "do-timestamp", TRUE);
    setBooleanPropertyIfPresent(src, "auto-reconnect", TRUE);
    setIntPropertyIfPresent(src, "latency", kSrtLatencyMs);
    setStringPropertyIfPresent(src, "localaddress", configuredInputInterfaceAddress(cfg));

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
        error = "SRT input: failed to link source -> queue";
        return nullptr;
    }

    terminalElement = queue;
    std::cerr << "Network TS input 202.24: protocol=SRT mode=" << mode
              << " factory=" << factory
              << " latency_ms=" << kSrtLatencyMs
              << " queue_ms=3000 leaky=off prebuffer=off"
              << " do_timestamp=on input_pacing=off" << std::endl;
    return src;
}

GstElement* buildHttp(
    const StreamConfig& cfg,
    GstElement* pipeline,
    GstElement*& terminalElement,
    std::string& error) {
    if (!hasElementFactory("souphttpsrc") || !hasElementFactory("tsparse")) {
        error = "HTTP MPEG-TS input: missing souphttpsrc/tsparse";
        return nullptr;
    }

    GstElement* src = gst_element_factory_make("souphttpsrc", "input_src");
    GstElement* capsFilter = gst_element_factory_make("capsfilter", "input_http_ts_caps");
    GstElement* tsparse = gst_element_factory_make("tsparse", "input_http_media_clock");
    GstElement* queue = addQueue(pipeline, "input_queue", kNetworkInputQueue);
    if (!src || !capsFilter || !tsparse || !queue ||
        !addElementOrFail(pipeline, src) || !addElementOrFail(pipeline, capsFilter) ||
        !addElementOrFail(pipeline, tsparse)) {
        if (src && !GST_OBJECT_PARENT(src)) gst_object_unref(src);
        if (capsFilter && !GST_OBJECT_PARENT(capsFilter)) gst_object_unref(capsFilter);
        if (tsparse && !GST_OBJECT_PARENT(tsparse)) gst_object_unref(tsparse);
        error = "HTTP MPEG-TS input: failed to create souphttpsrc/capsfilter/tsparse/queue";
        return nullptr;
    }

    const std::string location = appendAccessQuery(
        tvs::protocols::inputs::httpInputUri(cfg), cfg);
    g_object_set(src,
        "location", location.c_str(),
        "is-live", TRUE,
        "do-timestamp", TRUE,
        nullptr);
    configureHttpCredentials(src, cfg);
    setBooleanPropertyIfPresent(src, "compress", FALSE);

    GstCaps* tsCaps = gst_caps_from_string(
        "video/mpegts,systemstream=(boolean)true,packetsize=(int)188");
    if (!tsCaps) {
        error = "HTTP MPEG-TS input: failed to create TS caps";
        return nullptr;
    }
    g_object_set(capsFilter, "caps", tsCaps, nullptr);
    gst_caps_unref(tsCaps);

    // HTTP/TCP delivery can be very bursty. Ask tsparse to attach media-time
    // timestamps derived from the embedded PCR, but do NOT clock/sleep here.
    // StableUdpOutput consumes these timestamps only as a rate observation;
    // it remains the single physical output scheduler.
    setIntPropertyIfPresent(tsparse, "alignment", 7);
    setBooleanPropertyIfPresent(tsparse, "set-timestamps", TRUE);
    setUIntPropertyIfPresent(tsparse, "smoothing-latency", 100000U);

    const std::string inputInterface = configuredInputInterfaceAddress(cfg);
    if (!inputInterface.empty()) {
        std::cerr << "HTTP MPEG-TS input: input_iface=" << inputInterface
                  << " selected; souphttpsrc follows the kernel HTTP route" << std::endl;
    }

    if (!gst_element_link_many(src, capsFilter, tsparse, queue, nullptr)) {
        error = "HTTP MPEG-TS input: failed to link souphttpsrc -> caps -> tsparse -> queue";
        return nullptr;
    }

    terminalElement = queue;
    std::cerr << "Network TS input 202.24: protocol=HTTP transport=souphttpsrc"
              << " queue_ms=3000 leaky=off prebuffer=off"
              << " do_timestamp=on libcurl_appsrc=off"
              << " media_clock=PCR-tsparse-timestamps clocksync=off"
              << " access=" << (cfg.hlsAccessKeyMode.empty() ? "none" : cfg.hlsAccessKeyMode)
              << std::endl;
    return src;
}

GstElement* buildHls(
    StreamState* state,
    GstElement* pipeline,
    GstElement*& terminalElement,
    GCallback hlsPadAddedCallback,
    tvs::network_input::ConfigureTsMuxFn configureTsMux,
    std::string& error) {
    if (!state) {
        error = "HLS input: missing stream state";
        return nullptr;
    }
    if (!hasElementFactory("souphttpsrc") || !hasElementFactory("hlsdemux") ||
        !hasElementFactory("mpegtsmux") || !hasElementFactory("input-selector")) {
        error = "HLS input: missing souphttpsrc/hlsdemux/mpegtsmux/input-selector";
        return nullptr;
    }
    if (!hlsPadAddedCallback || !configureTsMux) {
        error = "HLS input: missing integration callback";
        return nullptr;
    }

    StreamConfig& cfg = state->runtimeConfig;
    GstElement* src = gst_element_factory_make("souphttpsrc", "input_src");
    GstElement* demux = gst_element_factory_make("hlsdemux", "hls_demux");
    GstElement* mux = gst_element_factory_make("mpegtsmux", "input_hls_ts_mux");
    GstElement* selector =
        gst_element_factory_make("input-selector", "input_hls_transport_selector");
    GstElement* queue = addQueue(pipeline, "input_queue", kHlsInputQueue);
    if (!src || !demux || !mux || !selector || !queue ||
        !addElementOrFail(pipeline, src) ||
        !addElementOrFail(pipeline, demux) ||
        !addElementOrFail(pipeline, mux) ||
        !addElementOrFail(pipeline, selector)) {
        if (src && !GST_OBJECT_PARENT(src)) gst_object_unref(src);
        if (demux && !GST_OBJECT_PARENT(demux)) gst_object_unref(demux);
        if (mux && !GST_OBJECT_PARENT(mux)) gst_object_unref(mux);
        if (selector && !GST_OBJECT_PARENT(selector)) gst_object_unref(selector);
        error = "HLS input: failed to create network transport chain";
        return nullptr;
    }

    std::string location = appendAccessQuery(
        tvs::protocols::inputs::hlsInputUri(cfg), cfg);
    g_object_set(src,
        "location", location.c_str(),
        "is-live", TRUE,
        "do-timestamp", TRUE,
        nullptr);
    configureHttpCredentials(src, cfg);
    setBooleanPropertyIfPresent(src, "compress", TRUE);

    // Match TVStreamer5: let hlsdemux know the configured service/output scale,
    // but do not add a second application-side bitrate/pacing controller.
    setIntPropertyIfPresent(demux, "connection-speed",
        static_cast<gint>(std::max<uint64_t>(cfg.targetBitrate / 1000ULL, 1ULL)));
    configureTsMux(mux, cfg);
    setBooleanPropertyIfPresent(selector, "sync-streams", FALSE);
    setBooleanPropertyIfPresent(selector, "cache-buffers", FALSE);

    if (!gst_element_link(src, demux) || !gst_element_link(selector, queue)) {
        error = "HLS input: failed to link HTTP/demux/selector queue";
        return nullptr;
    }

    if (!state->sourceContext) state->sourceContext = std::make_unique<RemapContext>();
    auto* ctx = state->sourceContext.get();
    ctx->mux = mux;
    ctx->hlsInputSelector = selector;
    ctx->config = cfg;
    ctx->flvMux = false;

    // Preserve the already proven direct MPEG-TS path.  TVStreamer5-style
    // source handling is used up to hlsdemux; if hlsdemux exposes complete TS,
    // no demux/remux cycle is introduced.  ES-only HLS keeps mpegtsmux fallback.
    GstPad* muxSrcPad = gst_element_get_static_pad(mux, "src");
    GstPad* muxSelectorPad = gst_element_request_pad_simple(selector, "sink_%u");
    if (!muxSrcPad || !muxSelectorPad ||
        gst_pad_link(muxSrcPad, muxSelectorPad) != GST_PAD_LINK_OK) {
        if (muxSrcPad) gst_object_unref(muxSrcPad);
        if (muxSelectorPad) gst_object_unref(muxSelectorPad);
        error = "HLS input: failed to connect fallback remux selector pad";
        return nullptr;
    }
    gst_object_unref(muxSrcPad);
    ctx->hlsMuxSelectorPad = muxSelectorPad;
    g_object_set(selector, "active-pad", muxSelectorPad, nullptr);

    configureHlsChildSource(src, ctx->config);
    g_signal_connect(demux, "deep-element-added",
        G_CALLBACK(onHlsDeepElementAdded), ctx);
    g_signal_connect_data(
        demux, "pad-added", hlsPadAddedCallback, ctx, nullptr,
        static_cast<GConnectFlags>(0));

    const std::string inputInterface = configuredInputInterfaceAddress(cfg);
    if (!inputInterface.empty()) {
        std::cerr << "HLS input: input_iface=" << inputInterface
                  << " selected; souphttpsrc follows the kernel HTTP route" << std::endl;
    }

    terminalElement = queue;
    std::cerr << "Network TS input 202.24: protocol=HLS source=souphttpsrc+hlsdemux"
              << " queue_ms=5000 leaky=off prebuffer=off do_timestamp=on"
              << " direct_mpegts=preferred remux=fallback-only input_pacing=off"
              << std::endl;
    return src;
}

} // namespace

namespace tvs::network_input {

bool handles(const StreamConfig& cfg) {
    const auto kind = tvs::stream_protocols::inputKind(cfg);
    return kind == tvs::stream_protocols::InputProtocolKind::Srt ||
           kind == tvs::stream_protocols::InputProtocolKind::Http ||
           kind == tvs::stream_protocols::InputProtocolKind::Hls;
}

GstElement* build(
    StreamState* state,
    GstElement* pipeline,
    GstElement*& terminalElement,
    GCallback hlsPadAddedCallback,
    ConfigureTsMuxFn configureTsMux,
    std::string& error) {
    terminalElement = nullptr;
    if (!state || !pipeline) {
        error = "Network TS input: invalid stream state/pipeline";
        return nullptr;
    }

    const StreamConfig& cfg = state->runtimeConfig;
    const auto kind = tvs::stream_protocols::inputKind(cfg);
    switch (kind) {
        case tvs::stream_protocols::InputProtocolKind::Srt:
            return buildSrt(cfg, pipeline, terminalElement, error);
        case tvs::stream_protocols::InputProtocolKind::Http:
            return buildHttp(cfg, pipeline, terminalElement, error);
        case tvs::stream_protocols::InputProtocolKind::Hls:
            return buildHls(
                state, pipeline, terminalElement,
                hlsPadAddedCallback, configureTsMux, error);
        default:
            error = "Network TS input: unsupported protocol";
            return nullptr;
    }
}

} // namespace tvs::network_input
