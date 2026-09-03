#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "ConfigManager.h"

class TelegramNotifier {
public:
    explicit TelegramNotifier(ConfigManager& cfg);
    void sendMessage(const std::string& text);

private:
    struct StreamEventState {
        std::string title;
        std::chrono::steady_clock::time_point lastSent =
            std::chrono::steady_clock::time_point::min();
    };

    ConfigManager& manager;
    std::mutex repeatMutex;
    std::unordered_map<std::string, StreamEventState> streamEvents;
};
