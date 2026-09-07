#pragma once

#include "ConfigManager.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

class GstTranscoderProcess {
public:
    GstTranscoderProcess() = default;
    ~GstTranscoderProcess();

    GstTranscoderProcess(const GstTranscoderProcess&) = delete;
    GstTranscoderProcess& operator=(const GstTranscoderProcess&) = delete;

    static bool isAvailable(std::string* error = nullptr);

    bool start(const StreamConfig& config, std::string& error);
    void stop();
    bool isRunning();
    std::string description() const;
    std::vector<pid_t> childPids() const;

private:
    struct ChildProcess {
        pid_t pid = -1;
        std::string description;
    };

    std::vector<ChildProcess> children;
    mutable std::mutex childrenMutex;
    std::atomic<bool> stopping{false};

    static std::vector<std::string> buildCommand(
        const StreamConfig& baseConfig,
        const StreamConfig& outputConfig,
        std::string& description,
        std::string& error);

    static bool spawnProcess(
        const std::vector<std::string>& args,
        const std::string& description,
        ChildProcess& child,
        std::string& error);
};
