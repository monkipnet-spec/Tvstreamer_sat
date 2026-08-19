#include "GstTranscoderProcess.h"

#include "TranscoderModule.h"
#include "protocols/GstInputProtocols.h"
#include "protocols/GstOutputProtocols.h"
#include "protocols/GstProtocolTypes.h"
#include "utils.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <sys/wait.h>
#include <unistd.h>

using tvs::protocols::ContainerKind;
using tvs::protocols::GstOutputSpec;

namespace {

void markOpenDescriptorsCloseOnExec() {
    DIR* directory = ::opendir("/proc/self/fd");
    if (directory) {
        const int directoryFd = ::dirfd(directory);
        while (dirent* entry = ::readdir(directory)) {
            char* end = nullptr;
            errno = 0;
            const long value = std::strtol(entry->d_name, &end, 10);
            if (errno != 0 || !end || *end != '\0' || value <= STDERR_FILENO || value == directoryFd) {
                continue;
            }
            const int fd = static_cast<int>(value);
            const int flags = ::fcntl(fd, F_GETFD);
            if (flags >= 0) {
                ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
            }
        }
        ::closedir(directory);
        return;
    }

    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd <= 0) maxFd = 4096;
    for (int fd = STDERR_FILENO + 1; fd < maxFd; ++fd) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags >= 0) {
            ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
        }
    }
}

void appendAvailableStderr(int fd, std::string& output) {
    if (fd < 0) return;
    char buffer[1024];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            output.append(buffer, static_cast<size_t>(count));
            if (output.size() > 8192) output.erase(0, output.size() - 8192);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
}

void relayChildStderr(int fd) {
    if (fd < 0) return;
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    char buffer[1024];
    for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof(buffer));
        if (count > 0) {
            std::cerr.write(buffer, count);
            std::cerr.flush();
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    ::close(fd);
}

bool executableInPath(const std::string& name, std::string* path = nullptr) {
    const char* envPath = std::getenv("PATH");
    std::string paths = envPath ? envPath : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    std::stringstream ss(paths);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        if (dir.empty()) continue;
        std::filesystem::path candidate = std::filesystem::path(dir) / name;
        if (::access(candidate.c_str(), X_OK) == 0) {
            if (path) *path = candidate.string();
            return true;
        }
    }
    return false;
}

bool hasFactory(const char* name) {
    GstElementFactory* factory = gst_element_factory_find(name);
    if (!factory) return false;
    gst_object_unref(factory);
    return true;
}

bool validateFactories(const std::vector<std::string>& names, std::vector<std::string>& missing) {
    bool ok = true;
    for (const auto& name : names) {
        if (!hasFactory(name.c_str())) {
            missing.push_back(name);
            ok = false;
        }
    }
    return ok;
}

std::string findAacEncoder() {
    for (const char* name : {"voaacenc", "fdkaacenc", "avenc_aac"}) {
        if (hasFactory(name)) return name;
    }
    return {};
}

std::string findMp3Encoder() {
    for (const char* name : {"lamemp3enc", "avenc_mp3"}) {
        if (hasFactory(name)) return name;
    }
    return {};
}

void addQueue(std::vector<std::string>& args, const std::string& name, uint64_t maxTimeNs = 5000000000ULL) {
    args.insert(args.end(), {
        "queue",
        "name=" + name,
        "max-size-buffers=0",
        "max-size-bytes=0",
        "max-size-time=" + std::to_string(maxTimeNs)
    });
}

std::string property(const std::string& name, const std::string& value) {
    return name + "=" + value;
}

std::string shellQuote(const std::string& value) {
    if (value.empty()) return "''";
    bool safe = true;
    for (unsigned char ch : value) {
        if (!(std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.' || ch == '/' ||
              ch == ':' || ch == '=' || ch == ',' || ch == '+' || ch == '?' ||
              ch == '&' || ch == '@' || ch == '%' || ch == ';')) {
            safe = false;
            break;
        }
    }
    if (safe) return value;
    std::string quoted = "'";
    for (char ch : value) {
        if (ch == '\'') quoted += "'\\''";
        else quoted += ch;
    }
    quoted += "'";
    return quoted;
}

std::string commandLineForLog(const std::vector<std::string>& args) {
    std::ostringstream ss;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) ss << ' ';
        ss << shellQuote(args[i]);
    }
    return ss.str();
}

std::string scaledVideoCaps(int width, int height) {
    return "video/x-raw,format=I420,width=" + std::to_string(width) +
           ",height=" + std::to_string(height) +
           ",framerate=25/1,pixel-aspect-ratio=(fraction)1/1,interlace-mode=progressive";
}

bool validateOutputAvailability(const StreamConfig& outputConfig, std::string& error) {
    std::vector<std::string> missing;
    validateFactories(tvs::protocols::requiredElementsForOutput(tvs::protocols::outputKind(outputConfig)), missing);
    if (!missing.empty()) {
        std::ostringstream ss;
        ss << "missing output protocol elements for " << tvs::protocols::normalizedOutputType(outputConfig);
        for (size_t i = 0; i < missing.size(); ++i) {
            ss << (i == 0 ? ": " : ", ") << missing[i];
        }
        error = ss.str();
        return false;
    }
    return true;
}


uint32_t effectiveInputServiceId(const StreamConfig& cfg) {
    // input_service_id=0 means AUTO. In AUTO mode the decoder sees the live
    // source directly and performs its normal program selection. Never fall
    // back to output service_id: that value is reserved for output remapping.
    return cfg.inputServiceId;
}

bool isSidAwareMpegTsInput(const StreamConfig& cfg) {
    const uint32_t sid = effectiveInputServiceId(cfg);
    if (sid == 0 || cfg.testPattern) return false;
    const std::string uri = toLower(tvs::protocols::inputUriForGstreamer(cfg));
    // Live IPTV/SRT transport streams are the paths where automatic URI
    // decoding can silently pick program 1 instead of the configured SID.
    // Keep non-TS containers/adaptive inputs on uridecodebin.
    return uri.rfind("srt://", 0) == 0 ||
           uri.rfind("udp://", 0) == 0;
}

bool appendTranscoderDecodeInput(
    std::vector<std::string>& args,
    const StreamConfig& cfg,
    std::string& error) {
    if (!isSidAwareMpegTsInput(cfg)) {
        tvs::protocols::appendDecodeInput(args, cfg);
        return true;
    }

    std::vector<std::string> missing;
    validateFactories({"urisourcebin", "tsparse", "tsdemux", "decodebin3"}, missing);
    if (!missing.empty()) {
        std::ostringstream ss;
        ss << "missing SID-aware transcoder input elements";
        for (size_t i = 0; i < missing.size(); ++i) {
            ss << (i == 0 ? ": " : ", ") << missing[i];
        }
        error = ss.str();
        return false;
    }

    const uint32_t inputSid = effectiveInputServiceId(cfg);
    const std::string uri = tvs::protocols::inputUriForGstreamer(cfg);

    // Select the requested MPEG-TS service *before* decodebin.  The old
    // uridecodebin-only path auto-selected the first/default program, which is
    // why transcoding worked for SID 1 but produced no usable UDP output when
    // Input SID was another program.  ':' asks gst-launch to link all compatible
    // elementary pads from the selected tsdemux program into decodebin3.
    args.insert(args.end(), {
        "urisourcebin",
        "name=input_uri_src",
        "uri=" + uri,
        "use-buffering=false",
        "input_uri_src.", "!",
        "queue",
        "name=transcode_sid_input_queue",
        "max-size-buffers=0",
        "max-size-bytes=0",
        "max-size-time=8000000000",
        "!", "tsparse",
        "!", "tsdemux",
        "name=transcode_sid_demux",
        "program-number=" + std::to_string(inputSid),
        "latency=700",
        "transcode_sid_demux.", ":", "decodebin3", "name=dec"
    });

    std::cerr << "GStreamer transcoder input selector: input_sid=" << inputSid
              << " method=tsdemux-program-number decode=decodebin3"
              << " uri=" << uri << std::endl;
    return true;
}

void addVideoBranch(std::vector<std::string>& args, const StreamConfig& cfg, const GstOutputSpec& spec) {
    int width = 1920;
    int height = 1080;
    TranscoderModule::resolutionSize(cfg.transcodeResolution, width, height);
    const uint64_t bitrateKbps = tvs::protocols::safeVideoBitrate(cfg) / 1000;
    const bool flv = spec.container == ContainerKind::Flv;
    const bool rtsp = spec.container == ContainerKind::Rtsp;

    args.insert(args.end(), {"dec.", "!"});
    addQueue(args, "transcode_video_queue", 8000000000ULL);
    args.insert(args.end(), {
        "!", "watchdog",
        "name=transcode_input_watchdog",
        "timeout=5000",
        "!", "video/x-raw",
        "!", "videoconvert",
        "!", "deinterlace", "method=yadif", "mode=auto-strict", "fields=top", "locking=passive",
        "!", "videoscale", "add-borders=false", "method=lanczos",
        "!", "videorate", "drop-only=false",
        "!", scaledVideoCaps(width, height),
        "!", "x264enc",
        "tune=zerolatency",
        "speed-preset=superfast",
        property("bitrate", std::to_string(bitrateKbps)),
        "key-int-max=25",
        "bframes=0",
        property("byte-stream", flv ? "false" : "true"),
        "aud=true",
        "insert-vui=true",
        "sliced-threads=true",
        "vbv-buf-capacity=1000",
        "option-string=nal-hrd=cbr:force-cfr=1:repeat-headers=1:scenecut=0",
        "!", "h264parse", property("config-interval", flv ? "-1" : "1"),
        "!", flv
            ? "video/x-h264,stream-format=avc,alignment=au"
            : "video/x-h264,stream-format=byte-stream,alignment=au",
        "!"
    });
    addQueue(args, "transcode_video_mux_queue", 3000000000ULL);
    args.insert(args.end(), {"!", spec.videoPad});
}

void addAudioBranch(std::vector<std::string>& args, const StreamConfig& cfg, const GstOutputSpec& spec, std::string& error) {
    const std::string audioCodec = toLower(cfg.transcodeAudioCodec);
    const uint64_t bitrate = tvs::protocols::safeAudioBitrate(cfg);
    const bool flv = spec.container == ContainerKind::Flv;
    const bool rtsp = spec.container == ContainerKind::Rtsp;

    args.insert(args.end(), {"dec.", "!"});
    addQueue(args, "transcode_audio_queue", 8000000000ULL);

    std::string selectedAacEncoder;
    std::string selectedMp3Encoder;
    if (audioCodec == "mp3") {
        selectedMp3Encoder = findMp3Encoder();
    } else {
        selectedAacEncoder = findAacEncoder();
    }

    const std::string rawAudioCaps = selectedAacEncoder == "avenc_aac"
        ? "audio/x-raw,format=F32LE,layout=interleaved,rate=48000,channels=2"
        : "audio/x-raw,format=S16LE,layout=interleaved,rate=48000,channels=2";

    args.insert(args.end(), {
        "!", "audio/x-raw",
        "!", "audioconvert",
        "!", "audioresample",
        "!", "audiorate",
        "!", rawAudioCaps,
        "!"
    });

    if (audioCodec == "mp3") {
        const std::string encoder = selectedMp3Encoder;
        if (encoder.empty()) {
            error = "MP3 encoder is not available";
            return;
        }
        if (encoder == "lamemp3enc") {
            args.insert(args.end(), {
                "lamemp3enc",
                "target=bitrate",
                "cbr=true",
                property("bitrate", std::to_string(std::max<uint64_t>(bitrate / 1000, 64))),
                "!", "mpegaudioparse",
                "!", "audio/mpeg,mpegversion=1,layer=3"
            });
        } else {
            args.insert(args.end(), {
                "avenc_mp3",
                property("bitrate", std::to_string(bitrate)),
                "!", "mpegaudioparse",
                "!", "audio/mpeg,mpegversion=1,layer=3"
            });
        }
    } else {
        const std::string encoder = selectedAacEncoder;
        if (encoder.empty()) {
            error = "AAC encoder is not available";
            return;
        }
        args.insert(args.end(), {
            encoder,
            property("bitrate", std::to_string(bitrate)),
            "!", "aacparse",
            "!", (flv || rtsp)
                ? "audio/mpeg,mpegversion=4,stream-format=raw"
                : "audio/mpeg,mpegversion=4,stream-format=adts"
        });
    }

    args.insert(args.end(), {"!"});
    addQueue(args, "transcode_audio_mux_queue", 3000000000ULL);
    args.insert(args.end(), {"!", spec.audioPad});
}

void addTestSources(std::vector<std::string>& args, const StreamConfig& cfg, const GstOutputSpec& spec, std::string& error) {
    StreamConfig testCfg = cfg;
    testCfg.transcodeResolution = cfg.transcodeResolution.empty() ? "1280x720" : cfg.transcodeResolution;
    int width = 1280;
    int height = 720;
    TranscoderModule::resolutionSize(testCfg.transcodeResolution, width, height);

    args.insert(args.end(), {
        "videotestsrc", "is-live=true", "pattern=smpte", "!", "video/x-raw,framerate=25/1", "!"
    });
    addQueue(args, "test_video_queue", 3000000000ULL);
    args.insert(args.end(), {
        "!", "videoconvert", "!", "videoscale", "add-borders=false", "method=lanczos", "!", "videorate",
        "!", scaledVideoCaps(width, height),
        "!", "x264enc", "tune=zerolatency", "speed-preset=superfast",
        property("bitrate", std::to_string(tvs::protocols::safeVideoBitrate(testCfg) / 1000)),
        "key-int-max=25", "bframes=0",
        property("byte-stream", spec.container == ContainerKind::Flv ? "false" : "true"),
        "aud=true", "insert-vui=true", "sliced-threads=true", "vbv-buf-capacity=1000",
        "option-string=nal-hrd=cbr:force-cfr=1:repeat-headers=1:scenecut=0",
        "!", "h264parse", property("config-interval", spec.container == ContainerKind::Flv ? "-1" : "1"),
        "!", spec.container == ContainerKind::Flv
            ? "video/x-h264,stream-format=avc,alignment=au"
            : "video/x-h264,stream-format=byte-stream,alignment=au",
        "!", spec.videoPad,
        "audiotestsrc", "is-live=true", "wave=sine", "freq=1000", "!", "audio/x-raw,rate=48000,channels=2", "!"
    });
    const std::string encoder = findAacEncoder();
    if (encoder.empty()) {
        error = "AAC encoder is not available";
        return;
    }
    args.insert(args.end(), {
        encoder, property("bitrate", std::to_string(tvs::protocols::safeAudioBitrate(cfg))),
        "!", "aacparse", "!",
        (spec.container == ContainerKind::Flv || spec.container == ContainerKind::Rtsp)
            ? "audio/mpeg,mpegversion=4,stream-format=raw"
            : "audio/mpeg,mpegversion=4,stream-format=adts",
        "!", spec.audioPad
    });
}

} // namespace

GstTranscoderProcess::~GstTranscoderProcess() {
    stop();
}

bool GstTranscoderProcess::isAvailable(std::string* error) {
    std::string gstLaunchPath;
    if (!executableInPath("gst-launch-1.0", &gstLaunchPath)) {
        if (error) *error = "gst-launch-1.0 executable was not found in PATH";
        return false;
    }

    std::vector<std::string> required = tvs::protocols::requiredInputElements();
    const std::vector<std::string> common = {
        "queue", "watchdog", "videoconvert", "deinterlace", "videoscale", "videorate",
        "x264enc", "h264parse", "audioconvert", "audioresample", "audiorate", "aacparse"
    };
    required.insert(required.end(), common.begin(), common.end());

    std::vector<std::string> missing;
    validateFactories(required, missing);
    if (findAacEncoder().empty()) {
        missing.emplace_back("AAC encoder: fdkaacenc, voaacenc or avenc_aac");
    }
    if (!missing.empty()) {
        std::ostringstream ss;
        ss << "missing GStreamer transcoder elements";
        for (size_t i = 0; i < missing.size(); ++i) {
            ss << (i == 0 ? ": " : ", ") << missing[i];
        }
        if (error) *error = ss.str();
        return false;
    }
    if (error) *error = "GStreamer transcoder is available: " + gstLaunchPath;
    return true;
}

bool GstTranscoderProcess::spawnProcess(
    const std::vector<std::string>& args,
    const std::string& description,
    ChildProcess& child,
    std::string& error) {
    if (args.empty()) {
        error = "empty gst-launch command";
        return false;
    }

    // gst-launch does not need any TVStreammerSAT5 sockets. Mark every currently open
    // non-standard descriptor close-on-exec before forking so HTTP/metrics/listener
    // sockets cannot remain alive in the external transcoder process.
    markOpenDescriptorsCloseOnExec();

    int stderrPipe[2] = {-1, -1};
    const bool captureStderr = ::pipe(stderrPipe) == 0;
    if (captureStderr) {
        for (int fd : stderrPipe) {
            const int fdFlags = ::fcntl(fd, F_GETFD);
            if (fdFlags >= 0) ::fcntl(fd, F_SETFD, fdFlags | FD_CLOEXEC);
        }
        const int flags = ::fcntl(stderrPipe[0], F_GETFL);
        if (flags >= 0) ::fcntl(stderrPipe[0], F_SETFL, flags | O_NONBLOCK);
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        if (captureStderr) {
            ::close(stderrPipe[0]);
            ::close(stderrPipe[1]);
        }
        error = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }

    if (pid == 0) {
        if (captureStderr) {
            ::close(stderrPipe[0]);
            if (::dup2(stderrPipe[1], STDERR_FILENO) < 0) std::_Exit(126);
            if (stderrPipe[1] != STDERR_FILENO) ::close(stderrPipe[1]);
        }

        int devNull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            if (devNull > STDERR_FILENO) ::close(devNull);
        }

        std::vector<std::string> storage = args;
        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);
        for (auto& arg : storage) argv.push_back(arg.data());
        argv.push_back(nullptr);
        ::execvp(argv[0], argv.data());
        std::cerr << "GStreamer transcoder exec failed: " << std::strerror(errno) << std::endl;
        std::_Exit(127);
    }

    if (captureStderr) ::close(stderrPipe[1]);
    child.pid = pid;
    child.description = description;

    // Capture early gst-launch diagnostics.  SRT/relay setup can fail slightly
    // after process creation, so give SRT outputs a longer observation window.
    const int attempts = description.rfind("srt-", 0) == 0 ? 24 : 8;
    std::string startupStderr;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (captureStderr) appendAvailableStderr(stderrPipe[0], startupStderr);

        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == 0) continue;
        if (done == pid) {
            if (captureStderr) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                appendAvailableStderr(stderrPipe[0], startupStderr);
                ::close(stderrPipe[0]);
            }
            std::ostringstream ss;
            ss << "GStreamer transcoder exited during startup for " << description;
            if (WIFEXITED(status)) {
                ss << " (exit=" << WEXITSTATUS(status) << ")";
            } else if (WIFSIGNALED(status)) {
                ss << " (signal=" << WTERMSIG(status) << ")";
            } else {
                ss << " (status=" << status << ")";
            }
            if (!startupStderr.empty()) ss << "\n" << startupStderr;
            error = ss.str();
            child.pid = -1;
            return false;
        }
        if (done < 0 && errno != EINTR) {
            if (captureStderr) ::close(stderrPipe[0]);
            error = std::string("waitpid failed after gst-launch start: ") + std::strerror(errno);
            child.pid = -1;
            return false;
        }
    }

    if (captureStderr) {
        appendAvailableStderr(stderrPipe[0], startupStderr);
        if (!startupStderr.empty()) {
            std::cerr << startupStderr;
            if (startupStderr.back() != '\n') std::cerr << std::endl;
        }
        try {
            std::thread(relayChildStderr, stderrPipe[0]).detach();
        } catch (const std::exception& ex) {
            std::cerr << "Resource guard: transcoder stderr relay thread creation failed: "
                      << ex.what() << std::endl;
            ::close(stderrPipe[0]);
        }
    }
    return true;
}

std::vector<std::string> GstTranscoderProcess::buildCommand(
    const StreamConfig& baseConfig,
    const StreamConfig& outputConfig,
    std::string& description,
    std::string& error) {
    if (!validateOutputAvailability(outputConfig, error)) {
        return {};
    }

    std::vector<std::string> args = {"gst-launch-1.0", "-e"};
    GstOutputSpec outputSpec;
    if (!tvs::protocols::appendOutputMuxAndSink(args, outputConfig, outputSpec, error)) {
        return {};
    }

    if (outputSpec.kind == tvs::protocols::OutputKind::Http ||
        outputSpec.kind == tvs::protocols::OutputKind::Srt) {
        std::cerr << "Transcoded HTTP/SRT post-mux A/V reservoir: output="
                  << tvs::protocols::normalizedOutputType(outputConfig)
                  << " reservoir_ms=1500 queue_max_ms=6000"
                  << " placement=after-mpegtsmux-remap-and-cbr-pacer"
                  << " remap_preserved="
                  << (outputConfig.remapEnabled ? "yes" : "not-requested")
                  << std::endl;
    }

    if (baseConfig.testPattern) {
        addTestSources(args, baseConfig, outputSpec, error);
    } else {
        std::cerr << "Transcoder input watchdog: timeout_ms=5000"
                  << " source=" << tvs::protocols::inputUriForGstreamer(baseConfig)
                  << " action=exit-for-parent-failover"
                  << std::endl;
        if (!appendTranscoderDecodeInput(args, baseConfig, error)) {
            return {};
        }
        addVideoBranch(args, baseConfig, outputSpec);
        addAudioBranch(args, baseConfig, outputSpec, error);
    }
    if (!error.empty()) return {};

    description = outputSpec.description;
    return args;
}

bool GstTranscoderProcess::start(const StreamConfig& config, std::string& error) {
    stop();
    stopping = false;

    std::string availableMessage;
    if (!isAvailable(&availableMessage)) {
        error = availableMessage;
        return false;
    }

    const auto outputs = tvs::protocols::outputConfigs(config);
    if (outputs.empty()) {
        error = "no outputs configured";
        return false;
    }

    std::vector<ChildProcess> started;
    for (const auto& output : outputs) {
        std::string description;
        std::string commandError;
        std::vector<std::string> args = buildCommand(config, output, description, commandError);
        if (!commandError.empty()) {
            error = commandError;
            for (auto& startedChild : started) {
                if (startedChild.pid > 0) {
                    ::kill(startedChild.pid, SIGTERM);
                    ::waitpid(startedChild.pid, nullptr, 0);
                }
            }
            return false;
        }

        std::cerr << "GStreamer transcoder command: " << commandLineForLog(args) << std::endl;

        ChildProcess child;
        if (!spawnProcess(args, description, child, error)) {
            for (auto& startedChild : started) {
                if (startedChild.pid > 0) {
                    ::kill(startedChild.pid, SIGTERM);
                    ::waitpid(startedChild.pid, nullptr, 0);
                }
            }
            return false;
        }
        std::cerr << "GStreamer transcoder started pid=" << child.pid
                  << " output=" << description
                  << " remap=" << (config.remapEnabled ? "on" : "off")
                  << " input_sid=" << effectiveInputServiceId(config)
                  << " service=" << config.serviceId
                  << " vpid=" << config.videoPid
                  << " apid=" << config.audioPid;
        const auto outputKind = tvs::protocols::outputKind(output);
        if (outputKind == tvs::protocols::OutputKind::FifoRelay) {
            std::cerr << " ts-relay=unpaced";
        } else if (tvs::protocols::isTsOutput(outputKind)) {
            if (tvs::protocols::transportCbrEnabled(output)) {
                std::cerr << " ts-cbr-bitrate=" << tvs::protocols::muxBitrate(output);
            } else {
                std::cerr << " ts-cbr=off";
            }
        } else {
            std::cerr << " encoder-cbr-bitrate=" << tvs::protocols::safeVideoBitrate(output);
        }
        std::cerr << std::endl;
        started.push_back(child);
    }

    {
        std::lock_guard<std::mutex> lock(childrenMutex);
        children = std::move(started);
    }
    return true;
}

void GstTranscoderProcess::stop() {
    stopping = true;
    std::lock_guard<std::mutex> lock(childrenMutex);
    for (auto& child : children) {
        if (child.pid <= 0) continue;
        int status = 0;
        pid_t done = ::waitpid(child.pid, &status, WNOHANG);
        if (done == 0) {
            ::kill(child.pid, SIGTERM);
            for (int i = 0; i < 40; ++i) {
                done = ::waitpid(child.pid, &status, WNOHANG);
                if (done == child.pid) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (done == 0) {
                ::kill(child.pid, SIGKILL);
                ::waitpid(child.pid, &status, 0);
            }
        }
        child.pid = -1;
    }
    children.clear();
}

bool GstTranscoderProcess::isRunning() {
    std::lock_guard<std::mutex> lock(childrenMutex);
    bool anyRunning = false;
    for (auto& child : children) {
        if (child.pid <= 0) continue;
        int status = 0;
        pid_t done = ::waitpid(child.pid, &status, WNOHANG);
        if (done == 0) {
            anyRunning = true;
            continue;
        }
        if (done == child.pid) {
            std::cerr << "GStreamer transcoder exited pid=" << child.pid
                      << " output=" << child.description
                      << " status=" << status << std::endl;
            child.pid = -1;
        }
    }
    return anyRunning;
}

std::vector<pid_t> GstTranscoderProcess::childPids() const {
    std::lock_guard<std::mutex> lock(childrenMutex);
    std::vector<pid_t> result;
    for (const auto& child : children) {
        if (child.pid > 0) {
            result.push_back(child.pid);
        }
    }
    return result;
}

std::string GstTranscoderProcess::description() const {
    std::lock_guard<std::mutex> lock(childrenMutex);
    std::ostringstream ss;
    for (size_t i = 0; i < children.size(); ++i) {
        if (i > 0) ss << ", ";
        ss << children[i].description;
    }
    return ss.str();
}
