#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "ConfigManager.h"

class TelegramNotifier {
public:
    explicit TelegramNotifier(ConfigManager& cfg);
    ~TelegramNotifier();

    TelegramNotifier(const TelegramNotifier&) = delete;
    TelegramNotifier& operator=(const TelegramNotifier&) = delete;

    // 203.19: callers only enqueue notifications. Telegram network I/O runs on
    // one bounded background worker and can no longer delay stream startup.
    void sendMessage(const std::string& text);

private:
    struct StreamEventState {
        std::string title;
        std::chrono::steady_clock::time_point lastSent =
            std::chrono::steady_clock::time_point::min();
    };

    void workerLoop();
    void sendMessageBlocking(const std::string& text);

    ConfigManager& manager;

    std::mutex queueMutex;
    std::condition_variable queueCondition;
    std::deque<std::string> pendingMessages;
    bool stopping = false;
    std::thread worker;

    std::mutex repeatMutex;
    std::unordered_map<std::string, StreamEventState> streamEvents;
};
