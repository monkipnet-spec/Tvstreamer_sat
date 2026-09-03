#include "TelegramNotifier.h"

#include <curl/curl.h>
#include <chrono>
#include <iostream>
#include <sstream>

namespace {
constexpr auto kRepeatedStreamEventWindow = std::chrono::minutes(30);

size_t discardTelegramResponse(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
}

std::string extractBetween(
    const std::string& text, const std::string& begin, const std::string& end,
    size_t startAt = 0) {
    const auto beginPos = text.find(begin, startAt);
    if (beginPos == std::string::npos) return {};
    const auto valueStart = beginPos + begin.size();
    const auto endPos = text.find(end, valueStart);
    if (endPos == std::string::npos || endPos <= valueStart) return {};
    return text.substr(valueStart, endPos - valueStart);
}

std::string streamIdFromTelegramMessage(const std::string& text) {
    return extractBetween(text, "ID: <code>", "</code>");
}

std::string eventTitleFromTelegramMessage(const std::string& text) {
    return extractBetween(text, "<b>", "</b>");
}
}

TelegramNotifier::TelegramNotifier(ConfigManager& cfg)
    : manager(cfg) {
}

void TelegramNotifier::sendMessage(const std::string& text) {
    const auto& config = manager.config;
    if (config.telegramToken.empty() || config.telegramChatId.empty()) {
        return;
    }

    // 202.81: a pipeline rebuild creates a fresh StreamState and can otherwise
    // resend the same warning every few seconds while the same network incident
    // is still active. Suppress the same event title for the same stream for
    // 30 minutes. A different event for that stream (for example, recovery) is
    // delivered immediately and resets the state, so a later real outage is not
    // hidden. This dedupe is process-local and does not affect journald logging.
    const std::string streamId = streamIdFromTelegramMessage(text);
    const std::string eventTitle = eventTitleFromTelegramMessage(text);
    const auto now = std::chrono::steady_clock::now();
    bool hadPreviousStreamEvent = false;
    StreamEventState previousStreamEvent;
    if (!streamId.empty() && !eventTitle.empty()) {
        std::lock_guard<std::mutex> lock(repeatMutex);
        auto it = streamEvents.find(streamId);
        if (it != streamEvents.end()) {
            hadPreviousStreamEvent = true;
            previousStreamEvent = it->second;
            if (it->second.title == eventTitle &&
                now - it->second.lastSent < kRepeatedStreamEventWindow) {
                std::cerr << "Telegram duplicate stream event suppressed: stream="
                          << streamId << " title=" << eventTitle << std::endl;
                return;
            }
        }
        streamEvents[streamId] = StreamEventState{eventTitle, now};
    }

    const auto rollbackRepeatReservation = [&]() {
        if (streamId.empty() || eventTitle.empty()) return;
        std::lock_guard<std::mutex> lock(repeatMutex);
        auto it = streamEvents.find(streamId);
        if (it == streamEvents.end() || it->second.title != eventTitle ||
            it->second.lastSent != now) {
            return;
        }
        if (hadPreviousStreamEvent) {
            it->second = previousStreamEvent;
        } else {
            streamEvents.erase(it);
        }
    };

    CURL* curl = curl_easy_init();
    if (!curl) {
        rollbackRepeatReservation();
        return;
    }

    // 202.48: curl_easy_escape() returns curl-allocated strings. The old
    // notifier streamed those pointers directly into ostringstream and never
    // released them, leaking memory on every Telegram status notification.
    char* escapedToken = curl_easy_escape(curl, config.telegramToken.c_str(), 0);
    char* escapedChatId = curl_easy_escape(curl, config.telegramChatId.c_str(), 0);
    char* escapedText = curl_easy_escape(curl, text.c_str(), 0);
    if (!escapedToken || !escapedChatId || !escapedText) {
        if (escapedToken) curl_free(escapedToken);
        if (escapedChatId) curl_free(escapedChatId);
        if (escapedText) curl_free(escapedText);
        curl_easy_cleanup(curl);
        rollbackRepeatReservation();
        return;
    }

    std::ostringstream url;
    url << "https://api.telegram.org/bot"
        << escapedToken
        << "/sendMessage?chat_id="
        << escapedChatId
        << "&parse_mode=HTML"
        << "&disable_web_page_preview=true"
        << "&text=" << escapedText;
    curl_free(escapedToken);
    curl_free(escapedChatId);
    curl_free(escapedText);

    const std::string requestUrl = url.str();
    curl_easy_setopt(curl, CURLOPT_URL, requestUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    // 202.54: libcurl writes the Telegram JSON response to stdout when no
    // write callback is installed. During a recovery storm that flooded
    // journald with large JSON bodies and added synchronous I/O to monitor
    // threads. Discard successful bodies and bound a failed API request.
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discardTelegramResponse);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 4000L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        std::cerr << "Telegram send error: " << curl_easy_strerror(res) << std::endl;
        rollbackRepeatReservation();
    }
    curl_easy_cleanup(curl);
}
