#include "TelegramNotifier.h"

#include <curl/curl.h>
#include <iostream>
#include <sstream>

namespace {
size_t discardTelegramResponse(char*, size_t size, size_t nmemb, void*) {
    return size * nmemb;
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

    CURL* curl = curl_easy_init();
    if (!curl) {
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
    }
    curl_easy_cleanup(curl);
}
