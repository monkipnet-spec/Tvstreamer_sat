#include "HlsSegmentScheduler.h"

#include "protocols/inputs/GstHlsInputProtocol.h"

#include <gst/app/gstappsrc.h>
#include <curl/curl.h>
#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kNsPerSecond = 1000000000ULL;
constexpr uint64_t kLowAheadNs = 6ULL * kNsPerSecond;
constexpr uint64_t kTargetAheadNs = 8ULL * kNsPerSecond;
constexpr uint64_t kHighAheadNs = 12ULL * kNsPerSecond;
constexpr std::size_t kMinimumStartupSegments = 2;
constexpr std::size_t kTsPacketSize = 188;
constexpr std::size_t kPushChunkBytes = 64 * kTsPacketSize; // 12032 bytes
constexpr long kHttpConnectTimeoutMs = 3000;
constexpr long kHttpTransferTimeoutMs = 12000;
constexpr uint64_t kPlaylistPollNs = 500ULL * 1000ULL * 1000ULL;

std::once_flag gCurlInitOnce;

uint64_t monotonicNanoseconds() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string stripQuotes(std::string value) {
    value = trim(std::move(value));
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

std::string resolveUrl(const std::string& base, const std::string& reference) {
    if (reference.empty()) return base;
    const std::string lower = toLower(reference);
    if (lower.rfind("http://", 0) == 0 || lower.rfind("https://", 0) == 0) return reference;

    const auto schemeEnd = base.find("://");
    if (schemeEnd == std::string::npos) return reference;
    const auto authorityEnd = base.find('/', schemeEnd + 3);
    const std::string origin = authorityEnd == std::string::npos ? base : base.substr(0, authorityEnd);
    if (reference.front() == '/') return origin + reference;

    std::string pathBase = base;
    const auto query = pathBase.find_first_of("?#");
    if (query != std::string::npos) pathBase.resize(query);
    const auto slash = pathBase.rfind('/');
    if (slash == std::string::npos || slash < schemeEnd + 3) return origin + "/" + reference;
    return pathBase.substr(0, slash + 1) + reference;
}

std::string appendQueryAccess(const std::string& uri, const StreamConfig& cfg) {
    if (toLower(cfg.hlsAccessKeyMode) != "query" || cfg.hlsAccessKeyName.empty() ||
        cfg.hlsAccessKeyValue.empty()) return uri;

    CURL* easy = curl_easy_init();
    if (!easy) return uri;
    char* escapedName = curl_easy_escape(easy, cfg.hlsAccessKeyName.c_str(), 0);
    char* escapedValue = curl_easy_escape(easy, cfg.hlsAccessKeyValue.c_str(), 0);
    std::string result = uri;
    if (escapedName && escapedValue) {
        const std::string key = std::string(escapedName) + "=";
        const auto q = uri.find('?');
        const std::string query = q == std::string::npos ? std::string{} : uri.substr(q + 1);
        if (query.rfind(key, 0) != 0 && query.find("&" + key) == std::string::npos) {
            result += (q == std::string::npos ? "?" : "&");
            result += escapedName;
            result += "=";
            result += escapedValue;
        }
    }
    if (escapedName) curl_free(escapedName);
    if (escapedValue) curl_free(escapedValue);
    curl_easy_cleanup(easy);
    return result;
}

size_t writeVector(void* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t bytes = size * nmemb;
    auto* out = static_cast<std::vector<uint8_t>*>(userdata);
    const auto* begin = static_cast<const uint8_t*>(ptr);
    out->insert(out->end(), begin, begin + bytes);
    return bytes;
}

struct HttpContext {
    std::atomic<bool>* stopping = nullptr;
};

int transferProgress(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<HttpContext*>(userdata);
    return (ctx && ctx->stopping && ctx->stopping->load(std::memory_order_relaxed)) ? 1 : 0;
}

bool httpGet(const std::string& rawUrl,
             const StreamConfig& cfg,
             std::atomic<bool>& stopping,
             std::vector<uint8_t>& body,
             long& status,
             std::string& effectiveUrl,
             std::string& error) {
    std::call_once(gCurlInitOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "curl_easy_init failed";
        return false;
    }

    body.clear();
    status = 0;
    effectiveUrl.clear();
    const std::string url = appendQueryAccess(rawUrl, cfg);
    struct curl_slist* headers = nullptr;
    if (toLower(cfg.hlsAccessKeyMode) == "header" && !cfg.hlsAccessKeyName.empty() &&
        !cfg.hlsAccessKeyValue.empty()) {
        const std::string header = cfg.hlsAccessKeyName + ": " + cfg.hlsAccessKeyValue;
        headers = curl_slist_append(headers, header.c_str());
    }

    HttpContext progress{&stopping};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, kHttpConnectTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, kHttpTransferTimeoutMs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeVector);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT,
        cfg.hlsUserAgent.empty() ? "Mozilla/5.0 TVStreammerSAT5" : cfg.hlsUserAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transferProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective) effectiveUrl = effective;

    if (headers) curl_slist_free_all(headers);
    if (rc != CURLE_OK) error = curl_easy_strerror(rc);
    curl_easy_cleanup(curl);

    if (stopping.load(std::memory_order_relaxed)) return false;
    if (rc != CURLE_OK) return false;
    if (status < 200 || status >= 300) {
        error = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
}

struct Variant {
    uint64_t bandwidth = 0;
    std::string url;
};

struct Segment {
    uint64_t sequence = 0;
    double durationSeconds = 0.0;
    std::string url;
    bool discontinuity = false;
    std::string keyUri;
    std::string keyIvHex;
};

struct MediaPlaylist {
    bool master = false;
    bool endList = false;
    uint64_t mediaSequence = 0;
    double targetDurationSeconds = 2.0;
    std::vector<Variant> variants;
    std::vector<Segment> segments;
};

uint64_t parseUnsignedAttribute(const std::string& line, const std::string& name) {
    const auto pos = line.find(name + "=");
    if (pos == std::string::npos) return 0;
    std::size_t start = pos + name.size() + 1;
    std::size_t end = start;
    while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) ++end;
    if (end == start) return 0;
    try { return std::stoull(line.substr(start, end - start)); } catch (...) { return 0; }
}

std::string parseQuotedAttribute(const std::string& line, const std::string& name) {
    const std::string token = name + "=";
    const auto pos = line.find(token);
    if (pos == std::string::npos) return {};
    std::size_t start = pos + token.size();
    if (start >= line.size()) return {};
    if (line[start] == '"') {
        ++start;
        const auto end = line.find('"', start);
        return end == std::string::npos ? line.substr(start) : line.substr(start, end - start);
    }
    const auto end = line.find(',', start);
    return trim(line.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

MediaPlaylist parsePlaylist(const std::string& text, const std::string& baseUrl) {
    MediaPlaylist out;
    std::istringstream input(text);
    std::string line;
    double pendingDuration = -1.0;
    bool pendingDiscontinuity = false;
    uint64_t pendingBandwidth = 0;
    uint64_t nextSequence = 0;
    std::string currentKeyUri;
    std::string currentKeyIvHex;

    while (std::getline(input, line)) {
        line = trim(std::move(line));
        if (line.empty()) continue;
        if (line.rfind("#EXT-X-MEDIA-SEQUENCE:", 0) == 0) {
            try { out.mediaSequence = std::stoull(trim(line.substr(22))); } catch (...) { out.mediaSequence = 0; }
            nextSequence = out.mediaSequence;
        } else if (line.rfind("#EXT-X-TARGETDURATION:", 0) == 0) {
            try { out.targetDurationSeconds = std::stod(trim(line.substr(22))); } catch (...) {}
        } else if (line.rfind("#EXT-X-STREAM-INF:", 0) == 0) {
            out.master = true;
            pendingBandwidth = parseUnsignedAttribute(line, "BANDWIDTH");
        } else if (line.rfind("#EXTINF:", 0) == 0) {
            const auto comma = line.find(',');
            try { pendingDuration = std::stod(line.substr(8, comma == std::string::npos ? std::string::npos : comma - 8)); }
            catch (...) { pendingDuration = out.targetDurationSeconds; }
        } else if (line == "#EXT-X-DISCONTINUITY") {
            pendingDiscontinuity = true;
        } else if (line.rfind("#EXT-X-KEY:", 0) == 0) {
            const std::string method = toLower(parseQuotedAttribute(line, "METHOD"));
            if (method.empty() || method == "none") {
                currentKeyUri.clear();
                currentKeyIvHex.clear();
            } else if (method == "aes-128") {
                currentKeyUri = resolveUrl(baseUrl, parseQuotedAttribute(line, "URI"));
                currentKeyIvHex = parseQuotedAttribute(line, "IV");
            } else {
                // Unsupported SAMPLE-AES/etc. is represented with a sentinel so
                // the worker logs a clear error rather than emitting encrypted TS.
                currentKeyUri = "unsupported:" + method;
                currentKeyIvHex.clear();
            }
        } else if (line == "#EXT-X-ENDLIST") {
            out.endList = true;
        } else if (!line.empty() && line.front() != '#') {
            if (pendingBandwidth || out.master) {
                out.variants.push_back({pendingBandwidth, resolveUrl(baseUrl, line)});
                pendingBandwidth = 0;
            } else if (pendingDuration >= 0.0) {
                Segment segment;
                segment.sequence = nextSequence++;
                segment.durationSeconds = pendingDuration > 0.0 ? pendingDuration : out.targetDurationSeconds;
                segment.url = resolveUrl(baseUrl, line);
                segment.discontinuity = pendingDiscontinuity;
                segment.keyUri = currentKeyUri;
                segment.keyIvHex = currentKeyIvHex;
                out.segments.push_back(std::move(segment));
                pendingDuration = -1.0;
                pendingDiscontinuity = false;
            }
        }
    }
    return out;
}

std::optional<Variant> chooseVariant(const std::vector<Variant>& variants, uint64_t targetBitrate) {
    if (variants.empty()) return std::nullopt;
    const Variant* bestBelow = nullptr;
    const Variant* lowestAbove = nullptr;
    for (const auto& variant : variants) {
        if (!bestBelow || (variant.bandwidth <= targetBitrate && variant.bandwidth > bestBelow->bandwidth)) {
            if (variant.bandwidth <= targetBitrate) bestBelow = &variant;
        }
        if (variant.bandwidth > targetBitrate && (!lowestAbove || variant.bandwidth < lowestAbove->bandwidth)) {
            lowestAbove = &variant;
        }
    }
    if (bestBelow) return *bestBelow;
    if (lowestAbove) return *lowestAbove;
    return variants.front();
}

} // namespace

namespace tvs::hls_scheduler {

class Scheduler::Impl {
public:
    Impl(GstElement* appsrc, GstElement* terminalQueue, StreamConfig config)
        : appsrc_(appsrc), terminalQueue_(terminalQueue), config_(std::move(config)) {
        if (appsrc_) gst_object_ref(appsrc_);
        if (terminalQueue_) gst_object_ref(terminalQueue_);
    }

    ~Impl() {
        stop();
        if (terminalProbePad_ && terminalProbeId_ != 0) {
            gst_pad_remove_probe(terminalProbePad_, terminalProbeId_);
            terminalProbeId_ = 0;
        }
        if (terminalProbePad_) gst_object_unref(terminalProbePad_);
        if (appsrc_) gst_object_unref(appsrc_);
        if (terminalQueue_) gst_object_unref(terminalQueue_);
    }

    bool start(std::string& error) {
        if (!appsrc_ || !GST_IS_APP_SRC(appsrc_) || !terminalQueue_) {
            error = "HLS scheduler: invalid appsrc/terminal queue";
            return false;
        }
        terminalProbePad_ = gst_element_get_static_pad(terminalQueue_, "src");
        if (!terminalProbePad_) {
            error = "HLS scheduler: terminal queue has no src pad";
            return false;
        }
        terminalProbeId_ = gst_pad_add_probe(
            terminalProbePad_, GST_PAD_PROBE_TYPE_BUFFER,
            &Impl::onTerminalBufferStatic, this, nullptr);
        if (terminalProbeId_ == 0) {
            error = "HLS scheduler: failed to attach consumption probe";
            return false;
        }
        stopping_.store(false, std::memory_order_relaxed);
        worker_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        const bool wasStopping = stopping_.exchange(true, std::memory_order_relaxed);
        wake_.notify_all();
        if (!wasStopping && appsrc_ && GST_IS_APP_SRC(appsrc_)) {
            gst_app_src_end_of_stream(GST_APP_SRC(appsrc_));
        }
        if (worker_.joinable()) worker_.join();
    }

private:
    static GstPadProbeReturn onTerminalBufferStatic(GstPad*, GstPadProbeInfo* info, gpointer userData) {
        return static_cast<Impl*>(userData)->onTerminalBuffer(info);
    }

    struct ConsumptionSpan {
        std::size_t remainingBytes = 0;
        uint64_t remainingDurationNs = 0;
    };

    GstPadProbeReturn onTerminalBuffer(GstPadProbeInfo* info) {
        GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buffer) return GST_PAD_PROBE_OK;

        // 203.18: HLS TS buffers must not carry a synthetic byte-position time
        // scale.  The provider MPEG-TS already contains the authoritative PCR
        // and PES PTS/DTS.  Track scheduler consumption privately by accounting
        // bytes that cross the terminal queue against the EXTINF duration ledger.
        // This clock is used ONLY to decide when another HTTP segment may be
        // fetched; it is never written into GstBuffer timestamps.
        std::size_t bytes = gst_buffer_get_size(buffer);
        uint64_t consumedNs = 0;
        {
            std::lock_guard<std::mutex> lock(consumptionMutex_);
            while (bytes > 0 && !consumptionSpans_.empty()) {
                auto& span = consumptionSpans_.front();
                if (span.remainingBytes == 0) {
                    consumptionSpans_.pop_front();
                    continue;
                }

                const std::size_t take = std::min(bytes, span.remainingBytes);
                uint64_t takeNs = 0;
                if (take == span.remainingBytes) {
                    // Give the final bytes the exact remaining duration so
                    // integer rounding cannot accumulate across a segment.
                    takeNs = span.remainingDurationNs;
                } else if (span.remainingDurationNs > 0) {
                    takeNs = static_cast<uint64_t>(
                        (static_cast<__uint128_t>(span.remainingDurationNs) * take) /
                        span.remainingBytes);
                    if (takeNs == 0) takeNs = 1;
                    takeNs = std::min(takeNs, span.remainingDurationNs);
                }

                span.remainingBytes -= take;
                span.remainingDurationNs -= takeNs;
                bytes -= take;
                consumedNs += takeNs;
                if (span.remainingBytes == 0) consumptionSpans_.pop_front();
            }
        }

        if (consumedNs > 0) {
            consumedDurationNs_.fetch_add(consumedNs, std::memory_order_relaxed);
            wake_.notify_one();
        }
        return GST_PAD_PROBE_OK;
    }

    uint64_t effectiveConsumedNs() const {
        const uint64_t downstream = consumedDurationNs_.load(std::memory_order_relaxed);
        if (firstPushMonotonicNs_ == 0) return 0;
        const uint64_t now = monotonicNanoseconds();
        const uint64_t wallClock = now > firstPushMonotonicNs_ ? now - firstPushMonotonicNs_ : 0;
        // A downstream reservoir can accept several HLS segments instantly even
        // though production consumes media in real time. Count consumption only
        // when BOTH downstream flow and wall-clock playback have advanced. This
        // is the key that prevents startup prefetch from filling StableUDP first.
        return std::min<uint64_t>(downstream, wallClock);
    }

    uint64_t aheadNs() const {
        const uint64_t consumed = effectiveConsumedNs();
        return pushedDurationNs_ > consumed ? pushedDurationNs_ - consumed : 0;
    }

    bool waitForRefillNeed(uint64_t maxWaitNs) {
        std::unique_lock<std::mutex> lock(wakeMutex_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(maxWaitNs);
        wake_.wait_until(lock, deadline, [&] {
            return stopping_.load(std::memory_order_relaxed) || aheadNs() <= kLowAheadNs;
        });
        return !stopping_.load(std::memory_order_relaxed);
    }

    std::string streamLabel() const {
        if (!config_.name.empty()) return config_.name;
        if (!config_.id.empty()) return config_.id;
        return "unnamed";
    }

    bool fetchBytesWithRetry(const std::string& url,
                             std::vector<uint8_t>& body,
                             std::string& effectiveUrl,
                             const char* what) {
        unsigned attempt = 0;
        while (!stopping_.load(std::memory_order_relaxed)) {
            long status = 0;
            std::string error;
            if (httpGet(url, config_, stopping_, body, status, effectiveUrl, error)) return true;
            if (stopping_.load(std::memory_order_relaxed)) return false;
            ++attempt;
            const uint64_t backoffMs = std::min<uint64_t>(2000, 250ULL << std::min<unsigned>(attempt - 1, 3));
            std::cerr << "HLS scheduler 203.18: stream=" << streamLabel() << " " << what
                      << " fetch failed attempt=" << attempt
                      << " url=" << url
                      << " error=" << error
                      << " retry_ms=" << backoffMs << std::endl;
            std::unique_lock<std::mutex> lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(backoffMs), [&] {
                return stopping_.load(std::memory_order_relaxed);
            });
        }
        return false;
    }

    bool loadPlaylist(MediaPlaylist& playlist) {
        std::vector<uint8_t> bytes;
        std::string effective;
        if (!fetchBytesWithRetry(activePlaylistUrl_, bytes, effective, "playlist")) return false;
        const std::string text(bytes.begin(), bytes.end());
        const std::string base = effective.empty() ? activePlaylistUrl_ : effective;
        playlist = parsePlaylist(text, base);
        if (playlist.master) {
            const auto variant = chooseVariant(playlist.variants, config_.targetBitrate);
            if (!variant) {
                std::cerr << "HLS scheduler 203.18: stream=" << streamLabel() << " master playlist has no variants" << std::endl;
                return false;
            }
            if (variant->url != activePlaylistUrl_) {
                std::cerr << "HLS scheduler 203.18: stream=" << streamLabel() << " master selected bandwidth=" << variant->bandwidth
                          << " target=" << config_.targetBitrate
                          << " url=" << variant->url << std::endl;
                activePlaylistUrl_ = variant->url;
            }
            bytes.clear();
            effective.clear();
            if (!fetchBytesWithRetry(activePlaylistUrl_, bytes, effective, "media-playlist")) return false;
            const std::string mediaText(bytes.begin(), bytes.end());
            playlist = parsePlaylist(mediaText, effective.empty() ? activePlaylistUrl_ : effective);
        }
        return !playlist.segments.empty();
    }

    std::optional<std::size_t> findSegmentIndex(const MediaPlaylist& playlist, uint64_t sequence) const {
        for (std::size_t i = 0; i < playlist.segments.size(); ++i) {
            if (playlist.segments[i].sequence == sequence) return i;
        }
        return std::nullopt;
    }

    uint64_t chooseStartupSequence(const MediaPlaylist& playlist) const {
        if (playlist.segments.empty()) return playlist.mediaSequence;
        double duration = 0.0;
        std::size_t count = 0;
        std::size_t start = playlist.segments.size() - 1;
        for (std::size_t i = playlist.segments.size(); i-- > 0;) {
            const double candidate = duration + playlist.segments[i].durationSeconds;
            if (count >= kMinimumStartupSegments && candidate * kNsPerSecond > kHighAheadNs) break;
            duration = candidate;
            start = i;
            ++count;
            if (count >= kMinimumStartupSegments && duration * kNsPerSecond >= kTargetAheadNs) break;
        }
        return playlist.segments[start].sequence;
    }

    bool decryptSegmentIfNeeded(const Segment& segment, std::vector<uint8_t>& bytes) {
        if (segment.keyUri.empty()) return true;
        if (segment.keyUri.rfind("unsupported:", 0) == 0) {
            std::cerr << "HLS scheduler 203.18: stream=" << streamLabel() << " unsupported encryption method="
                      << segment.keyUri.substr(12) << " sequence=" << segment.sequence << std::endl;
            return false;
        }

        if (cachedKeyUri_ != segment.keyUri || cachedKey_.size() != 16) {
            std::vector<uint8_t> keyBody;
            std::string effective;
            if (!fetchBytesWithRetry(segment.keyUri, keyBody, effective, "AES-128-key")) return false;
            if (keyBody.size() < 16) {
                std::cerr << "HLS scheduler 203.18: stream=" << streamLabel() << " AES-128 key too short bytes=" << keyBody.size() << std::endl;
                return false;
            }
            cachedKey_.assign(keyBody.begin(), keyBody.begin() + 16);
            cachedKeyUri_ = segment.keyUri;
        }

        unsigned char iv[16] = {};
        if (!segment.keyIvHex.empty()) {
            std::string hex = segment.keyIvHex;
            if (hex.rfind("0x", 0) == 0 || hex.rfind("0X", 0) == 0) hex.erase(0, 2);
            if (hex.size() > 32) hex = hex.substr(hex.size() - 32);
            if (hex.size() < 32) hex.insert(hex.begin(), 32 - hex.size(), '0');
            for (std::size_t i = 0; i < 16; ++i) {
                try { iv[i] = static_cast<unsigned char>(std::stoul(hex.substr(i * 2, 2), nullptr, 16)); }
                catch (...) { return false; }
            }
        } else {
            uint64_t seq = segment.sequence;
            for (int i = 15; i >= 8; --i) {
                iv[i] = static_cast<unsigned char>(seq & 0xffU);
                seq >>= 8;
            }
        }

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return false;
        std::vector<uint8_t> plain(bytes.size() + EVP_MAX_BLOCK_LENGTH);
        int out1 = 0;
        int out2 = 0;
        bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, cachedKey_.data(), iv) == 1 &&
                  EVP_DecryptUpdate(ctx, plain.data(), &out1, bytes.data(), static_cast<int>(bytes.size())) == 1 &&
                  EVP_DecryptFinal_ex(ctx, plain.data() + out1, &out2) == 1;
        EVP_CIPHER_CTX_free(ctx);
        if (!ok) {
            std::cerr << "HLS scheduler 203.18: stream=" << streamLabel() << " AES-128 decrypt failed sequence=" << segment.sequence << std::endl;
            return false;
        }
        plain.resize(static_cast<std::size_t>(out1 + out2));
        bytes.swap(plain);
        return true;
    }

    bool pushSegment(const Segment& segment, const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) return true;
        if (firstPushMonotonicNs_ == 0) firstPushMonotonicNs_ = monotonicNanoseconds();
        if (bytes.size() < kTsPacketSize) return true;

        std::size_t start = 0;
        while (start < bytes.size() && bytes[start] != 0x47) ++start;
        if (start >= bytes.size()) return true;
        if (start != 0) {
            std::cerr << "HLS scheduler 203.18: stream=" << streamLabel() << " segment resync discarded=" << start
                      << " sequence=" << segment.sequence << std::endl;
        }
        const std::size_t usable = ((bytes.size() - start) / kTsPacketSize) * kTsPacketSize;
        if (usable == 0) return true;

        const uint64_t segmentDurationNs = std::max<uint64_t>(
            1ULL, static_cast<uint64_t>(std::llround(segment.durationSeconds * static_cast<double>(kNsPerSecond))));

        // Register the segment in the scheduler's private consumption ledger
        // BEFORE pushing its first buffer. The terminal queue may drain on a
        // different thread immediately after gst_app_src_push_buffer().
        {
            std::lock_guard<std::mutex> lock(consumptionMutex_);
            consumptionSpans_.push_back({usable, segmentDurationNs});
        }

        std::size_t offset = 0;
        bool first = true;
        while (offset < usable && !stopping_.load(std::memory_order_relaxed)) {
            const std::size_t chunkBytes = std::min<std::size_t>(kPushChunkBytes, usable - offset);
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, chunkBytes, nullptr);
            if (!buffer) return false;
            gst_buffer_fill(buffer, 0, bytes.data() + start + offset, chunkBytes);

            // 203.18: DO NOT derive timestamps from byte position inside EXTINF.
            // VBR MPEG-TS byte density is not linear in media time. Leaving these
            // unset makes StableUDP ignore the appsrc clock and use the provider
            // PCR carried inside the TS, while AAC/H264 PES PTS/DTS stay untouched.
            GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DURATION(buffer) = GST_CLOCK_TIME_NONE;
            if (first && segment.discontinuity) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);

            const GstFlowReturn flow = gst_app_src_push_buffer(GST_APP_SRC(appsrc_), buffer);
            if (flow != GST_FLOW_OK) {
                if (!stopping_.load(std::memory_order_relaxed)) {
                    std::cerr << "HLS scheduler 203.18: stream=" << streamLabel()
                              << " appsrc push stopped flow=" << flow
                              << " sequence=" << segment.sequence << std::endl;
                }
                return false;
            }
            offset += chunkBytes;
            first = false;
        }
        if (stopping_.load(std::memory_order_relaxed)) return false;
        pushedDurationNs_ += segmentDurationNs;
        ++segmentsPushed_;
        return true;
    }

    bool downloadAndPush(const Segment& segment) {
        std::vector<uint8_t> bytes;
        std::string effective;
        if (!fetchBytesWithRetry(segment.url, bytes, effective, "segment")) return false;
        if (!decryptSegmentIfNeeded(segment, bytes)) return false;
        if (!pushSegment(segment, bytes)) return false;
        ++segmentsDownloaded_;
        const uint64_t ahead = aheadNs();
        std::cerr << "HLS scheduler 203.18: stream=" << streamLabel()
                  << " segment=" << segment.sequence
                  << " duration_ms=" << static_cast<uint64_t>(segment.durationSeconds * 1000.0)
                  << " bytes=" << bytes.size()
                  << " ahead_ms=" << ahead / 1000000ULL
                  << " downloaded=" << segmentsDownloaded_
                  << " consumed_effective_ms=" << effectiveConsumedNs() / 1000000ULL
                  << " consumed_downstream_ms=" << consumedDurationNs_.load(std::memory_order_relaxed) / 1000000ULL
                  << std::endl;
        return true;
    }

    void run() {
        activePlaylistUrl_ = tvs::protocols::inputs::hlsInputUri(config_);
        if (activePlaylistUrl_.empty()) return;
        std::cerr << "HLS scheduler 203.18: stream=" << streamLabel()
                  << " mode=duration-controlled"
                  << " low_ms=" << kLowAheadNs / 1000000ULL
                  << " target_ms=" << kTargetAheadNs / 1000000ULL
                  << " high_ms=" << kHighAheadNs / 1000000ULL
                  << " min_start_segments=" << kMinimumStartupSegments
                  << " fetch_policy=fast-segment-on-demand"
                  << " provider_ts=byte-preserved"
                  << " gst_timestamps=provider-ts-only"
                  << " consumption_clock=private-byte-duration-ledger"
                  << std::endl;

        MediaPlaylist playlist;
        if (!loadPlaylist(playlist)) return;
        nextSequence_ = chooseStartupSequence(playlist);
        bool startup = true;
        uint64_t lastPlaylistLoadNs = monotonicNanoseconds();

        while (!stopping_.load(std::memory_order_relaxed)) {
            uint64_t ahead = aheadNs();
            if (!startup && ahead > kLowAheadNs) {
                const uint64_t now = monotonicNanoseconds();
                const uint64_t untilReload = now > lastPlaylistLoadNs && now - lastPlaylistLoadNs >= kPlaylistPollNs
                    ? 1ULL : kPlaylistPollNs - std::min<uint64_t>(kPlaylistPollNs, now - lastPlaylistLoadNs);
                if (!waitForRefillNeed(std::min<uint64_t>(untilReload, 250ULL * 1000ULL * 1000ULL))) break;
                continue;
            }

            bool madeProgress = false;
            while (!stopping_.load(std::memory_order_relaxed)) {
                ahead = aheadNs();
                const bool haveStartupSegments = segmentsPushed_ >= kMinimumStartupSegments;
                if ((startup && haveStartupSegments && ahead >= kTargetAheadNs) ||
                    (!startup && ahead >= kTargetAheadNs)) {
                    startup = false;
                    break;
                }

                auto index = findSegmentIndex(playlist, nextSequence_);
                if (!index) {
                    const uint64_t first = playlist.segments.front().sequence;
                    const uint64_t last = playlist.segments.back().sequence;
                    if (nextSequence_ < first) {
                        const uint64_t old = nextSequence_;
                        nextSequence_ = chooseStartupSequence(playlist);
                        std::cerr << "HLS scheduler 203.18: stream=" << streamLabel()
                                  << " fell behind live window old_sequence=" << old
                                  << " new_sequence=" << nextSequence_
                                  << " action=jump-near-live-edge" << std::endl;
                        continue;
                    }
                    if (nextSequence_ > last) break;
                } else {
                    const Segment& segment = playlist.segments[*index];
                    const uint64_t durationNs = std::max<uint64_t>(
                        1ULL, static_cast<uint64_t>(segment.durationSeconds * static_cast<double>(kNsPerSecond)));
                    if (!startup && ahead > 0 && ahead + durationNs > kHighAheadNs) break;
                    if (!downloadAndPush(segment)) return;
                    nextSequence_ = segment.sequence + 1;
                    madeProgress = true;
                }
            }

            if (stopping_.load(std::memory_order_relaxed)) break;
            if (!madeProgress || findSegmentIndex(playlist, nextSequence_) == std::nullopt) {
                std::unique_lock<std::mutex> lock(wakeMutex_);
                wake_.wait_for(lock, std::chrono::milliseconds(250), [&] {
                    return stopping_.load(std::memory_order_relaxed);
                });
                if (stopping_.load(std::memory_order_relaxed)) break;
                MediaPlaylist refreshed;
                if (loadPlaylist(refreshed)) {
                    playlist = std::move(refreshed);
                    lastPlaylistLoadNs = monotonicNanoseconds();
                }
            }
        }
    }

    GstElement* appsrc_ = nullptr;
    GstElement* terminalQueue_ = nullptr;
    StreamConfig config_;
    GstPad* terminalProbePad_ = nullptr;
    gulong terminalProbeId_ = 0;
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> consumedDurationNs_{0};
    uint64_t pushedDurationNs_ = 0;
    std::mutex consumptionMutex_;
    std::deque<ConsumptionSpan> consumptionSpans_;
    uint64_t firstPushMonotonicNs_ = 0;
    uint64_t nextSequence_ = 0;
    uint64_t segmentsDownloaded_ = 0;
    uint64_t segmentsPushed_ = 0;
    std::thread worker_;
    std::mutex wakeMutex_;
    std::condition_variable wake_;
    std::string activePlaylistUrl_;
    std::string cachedKeyUri_;
    std::vector<uint8_t> cachedKey_;
};

Scheduler::Scheduler(GstElement* appsrc, GstElement* terminalQueue, StreamConfig config)
    : impl_(std::make_unique<Impl>(appsrc, terminalQueue, std::move(config))) {}
Scheduler::~Scheduler() = default;
bool Scheduler::start(std::string& error) { return impl_->start(error); }
void Scheduler::stop() { impl_->stop(); }

} // namespace tvs::hls_scheduler
