#include "GstTranscoderProcess.h"

#include "TranscoderModule.h"
#include "protocols/GstInputProtocols.h"
#include "protocols/GstOutputProtocols.h"
#include "protocols/GstProtocolTypes.h"
#include "utils.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <net/if.h>
#include <netinet/in.h>
#include <sstream>
#include <thread>
#include <unordered_set>

#include <dirent.h>
#include <fcntl.h>
#include <gst/gst.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

using tvs::protocols::ContainerKind;
using tvs::protocols::GstOutputSpec;

namespace {

#if defined(__linux__) && !defined(IP_MULTICAST_ALL)
#define IP_MULTICAST_ALL 49
#endif

constexpr int kTranscoderUdpReceiveBufferBytes = 16 * 1024 * 1024;
constexpr int kTranscoderTransportWatchdogMs = 5000;
constexpr int kTranscoderDecodedVideoWatchdogMs = 15000;

struct ProtectedUdpInput {
    int fd = -1;
    std::string group;
    int port = 0;
    std::vector<std::string> joinedInterfaces;
    std::string boundDevice;
    int effectiveReceiveBuffer = 0;
};

bool parseIpv4MulticastUdpUri(const StreamConfig& cfg, std::string& group, int& port) {
    std::string uri = normalizeInputUri(tvs::protocols::inputUriForGstreamer(cfg));
    const std::string lower = toLower(uri);
    if (lower.rfind("udp://", 0) != 0) return false;

    std::string endpoint = uri.substr(6);
    if (!endpoint.empty() && endpoint.front() == '@') endpoint.erase(endpoint.begin());
    const size_t query = endpoint.find_first_of("/?");
    if (query != std::string::npos) endpoint.resize(query);
    const size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) return false;

    group = endpoint.substr(0, colon);
    char* end = nullptr;
    errno = 0;
    const long parsedPort = std::strtol(endpoint.c_str() + colon + 1, &end, 10);
    if (errno != 0 || !end || *end != '\0' || parsedPort <= 0 || parsedPort > 65535) return false;
    port = static_cast<int>(parsedPort);

    in_addr address {};
    if (::inet_pton(AF_INET, group.c_str(), &address) != 1) return false;
    const uint32_t hostOrder = ntohl(address.s_addr);
    return (hostOrder & 0xF0000000U) == 0xE0000000U;
}

std::string effectiveTranscoderInputInterface(const StreamConfig& cfg) {
    if (cfg.inputInterfaceAddressConfigured) return cfg.inputInterfaceAddress;
    return cfg.interfaceAddress;
}

std::vector<NetworkInterface> transcoderMulticastInterfaces(
    const StreamConfig& cfg,
    std::string& error) {
    const std::string configured = effectiveTranscoderInputInterface(cfg);
    std::vector<NetworkInterface> result;
    std::unordered_set<std::string> added;
    for (const auto& iface : enumerateNetworkInterfaces(true)) {
        const bool loopback = iface.name == "lo" || iface.address.rfind("127.", 0) == 0;
        if (!configured.empty() && iface.name != configured && iface.address != configured) continue;
        if (iface.name.empty() || iface.address.empty() || !iface.isUp ||
            (!iface.supportsMulticast && !loopback)) {
            continue;
        }
        if (added.insert(iface.name).second) result.push_back(iface);
    }
    if (!configured.empty() && result.empty()) {
        error = "selected transcoder multicast input interface was not found/up: " + configured;
    }
    return result;
}

bool bindSocketToDevice(int fd, const std::string& interfaceName, std::string& error) {
#if defined(__linux__) && defined(SO_BINDTODEVICE)
    if (interfaceName.empty()) return true;
    if (::setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE,
                     interfaceName.c_str(), static_cast<socklen_t>(interfaceName.size() + 1)) != 0) {
        error = "SO_BINDTODEVICE(" + interfaceName + ") failed: " + std::strerror(errno);
        return false;
    }
    return true;
#else
    (void)fd;
    (void)interfaceName;
    return true;
#endif
}

bool createProtectedUdpInput(const StreamConfig& cfg, ProtectedUdpInput& input, std::string& error) {
    if (!parseIpv4MulticastUdpUri(cfg, input.group, input.port)) return false;

    std::string interfaceError;
    const auto interfaces = transcoderMulticastInterfaces(cfg, interfaceError);
    if (!interfaceError.empty()) {
        error = interfaceError;
        return false;
    }
    if (interfaces.empty()) {
        error = "no active IPv4 multicast interface is available for protected transcoder UDP input";
        return false;
    }

    const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        error = std::string("protected transcoder UDP socket failed: ") + std::strerror(errno);
        return false;
    }

    auto fail = [&](const std::string& message) {
        ::close(fd);
        error = message;
        return false;
    };

    const int reuse = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        return fail(std::string("protected transcoder UDP SO_REUSEADDR failed: ") + std::strerror(errno));
    }

    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                     &kTranscoderUdpReceiveBufferBytes, sizeof(kTranscoderUdpReceiveBufferBytes)) != 0) {
        std::cerr << "Protected transcoder UDP 203.09: warning SO_RCVBUF request="
                  << kTranscoderUdpReceiveBufferBytes << " failed: " << std::strerror(errno) << std::endl;
    }

#if defined(__linux__) && defined(IP_MULTICAST_ALL)
    const int multicastAll = 0;
    if (::setsockopt(fd, IPPROTO_IP, IP_MULTICAST_ALL, &multicastAll, sizeof(multicastAll)) != 0) {
        return fail(std::string("protected transcoder UDP IP_MULTICAST_ALL=0 failed: ") +
                    std::strerror(errno));
    }
#endif

    sockaddr_in bindAddress {};
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddress.sin_port = htons(static_cast<uint16_t>(input.port));
    if (::bind(fd, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) != 0) {
        return fail("protected transcoder UDP bind 0.0.0.0:" + std::to_string(input.port) +
                    " failed: " + std::strerror(errno));
    }

    const std::string configured = effectiveTranscoderInputInterface(cfg);
    if (!configured.empty()) {
        std::string bindError;
        if (!bindSocketToDevice(fd, interfaces.front().name, bindError)) {
            return fail("protected transcoder UDP strict interface failed: " + bindError);
        }
        input.boundDevice = interfaces.front().name;
    }

    in_addr groupAddress {};
    if (::inet_pton(AF_INET, input.group.c_str(), &groupAddress) != 1) {
        return fail("invalid protected transcoder multicast group: " + input.group);
    }

    for (const auto& iface : interfaces) {
        in_addr localAddress {};
        if (::inet_pton(AF_INET, iface.address.c_str(), &localAddress) != 1) continue;
        ip_mreq membership {};
        membership.imr_multiaddr = groupAddress;
        membership.imr_interface = localAddress;
        if (::setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) == 0) {
            input.joinedInterfaces.push_back(iface.name);
            continue;
        }
        const std::string joinError = "join " + input.group + " on " + iface.name +
            " failed: " + std::strerror(errno);
        if (!configured.empty()) return fail("protected transcoder UDP " + joinError);
        std::cerr << "Protected transcoder UDP 203.09: warning " << joinError << std::endl;
    }
    if (input.joinedInterfaces.empty()) {
        return fail("protected transcoder UDP failed to join " + input.group + " on any interface");
    }

    socklen_t receiveBufferLength = sizeof(input.effectiveReceiveBuffer);
    if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF,
                     &input.effectiveReceiveBuffer, &receiveBufferLength) != 0) {
        input.effectiveReceiveBuffer = 0;
    }

    input.fd = fd;
    return true;
}

std::string joinedNames(const std::vector<std::string>& names) {
    std::ostringstream ss;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) ss << ',';
        ss << names[i];
    }
    return ss.str();
}

bool factoryLongNameContains(const char* factoryName, const std::string& needle) {
    GstElementFactory* factory = gst_element_factory_find(factoryName);
    if (!factory) return false;
    const gchar* longName = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_LONGNAME);
    const bool matched = longName && toLower(longName).find(toLower(needle)) != std::string::npos;
    gst_object_unref(factory);
    return matched;
}

bool forceSoftwareH264DecodeForPlatform() {
    // Ivy Bridge VA decode recovered poorly from damaged live MPEG-TS in field
    // tests, while avdec_h264 recovered repeatedly without stopping the pipeline.
    // Keep hardware *encoding* enabled; only decode is forced to software on
    // this old Intel generation.
    return factoryLongNameContains("vah264enc", "ivybridge") ||
           factoryLongNameContains("vah264dec", "ivybridge");
}

std::string softwareH264FeatureRankOverride() {
    const char* existing = std::getenv("GST_PLUGIN_FEATURE_RANK");
    std::string value = existing ? existing : "";
    if (!value.empty() && value.back() != ',') value += ',';
    value += "vah264dec:NONE,vaapih264dec:NONE,qsvh264dec:NONE";
    return value;
}

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

std::string intelVideoEncoderFactory() {
    // 203.08: factory registration is not proof that the Intel backend works.
    // TranscoderModule probes qsv/VA in an isolated child process and caches
    // the first encoder that actually produces H.264 without aborting.
    return TranscoderModule::workingIntelVideoEncoderFactory();
}

bool isIntelVideoEncoder(const std::string& factory) {
    return factory == "qsvh264enc" || factory == "vah264enc" || factory == "vaapih264enc";
}

std::string selectedVideoEncoderFactory(const StreamConfig& cfg) {
    if (cfg.transcodeVideoEncoder == "nvenc") return hasFactory("nvh264enc") ? "nvh264enc" : std::string();
    if (cfg.transcodeVideoEncoder == "intel") return intelVideoEncoderFactory();
    if (cfg.transcodeVideoEncoder == "x264") return hasFactory("x264enc") ? "x264enc" : std::string();
    if (hasFactory("nvh264enc")) return "nvh264enc";
    if (const std::string intel = intelVideoEncoderFactory(); !intel.empty()) return intel;
    if (hasFactory("x264enc")) return "x264enc";
    return {};
}

std::string scaledVideoCaps(int width, int height, const std::string& encoderFactory) {
    const char* format = (encoderFactory == "nvh264enc" || isIntelVideoEncoder(encoderFactory)) ? "NV12" : "I420";
    return "video/x-raw,format=" + std::string(format) + ",width=" + std::to_string(width) +
           ",height=" + std::to_string(height) +
           ",pixel-aspect-ratio=(fraction)1/1,interlace-mode=progressive";
}

bool appendVideoEncoder(std::vector<std::string>& args, const StreamConfig& cfg,
                        bool flv, int keyInt, std::string& error) {
    const std::string encoderFactory = selectedVideoEncoderFactory(cfg);
    if (encoderFactory.empty()) {
        if (cfg.transcodeVideoEncoder == "nvenc") {
            error = "NVIDIA NVENC was requested but GStreamer nvh264enc is not available";
        } else if (cfg.transcodeVideoEncoder == "intel") {
            error = "Intel hardware H.264 was requested but no qsv/VA H.264 encoder passed the runtime probe";
        } else if (cfg.transcodeVideoEncoder == "x264") {
            error = "CPU x264 was requested but GStreamer x264enc is not available";
        } else {
            error = "no H.264 video encoder is available (need nvh264enc, Intel qsv/va or x264enc)";
        }
        return false;
    }

    const uint64_t bitrateKbps = tvs::protocols::safeVideoBitrate(cfg) / 1000;
    args.insert(args.end(), {"!", encoderFactory});
    if (encoderFactory == "nvh264enc") {
        args.insert(args.end(), {
            property("bitrate", std::to_string(bitrateKbps)),
            property("gop-size", std::to_string(keyInt)),
            "bframes=0",
            "rc-mode=cbr",
            "zerolatency=true",
            "aud=true",
            "repeat-sequence-header=true",
            "strict-gop=true",
            property("vbv-buffer-size", std::to_string(std::max<uint64_t>(bitrateKbps, 500)))
        });
    } else if (encoderFactory == "qsvh264enc") {
        args.insert(args.end(), {
            property("bitrate", std::to_string(bitrateKbps)),
            property("gop-size", std::to_string(keyInt)),
            "b-frames=0",
            "rate-control=cbr",
            "idr-interval=0"
        });
    } else if (encoderFactory == "vah264enc") {
        // Keep the GstVA command line on the conservative property set that is
        // known to work on older Intel generations (including Ivy Bridge).
        // h264parse downstream repeats codec headers, so encoder-specific AUD /
        // target-usage knobs are intentionally not required here.
        args.insert(args.end(), {
            property("bitrate", std::to_string(bitrateKbps)),
            property("key-int-max", std::to_string(keyInt)),
            "b-frames=0",
            "rate-control=cbr"
        });
    } else if (encoderFactory == "vaapih264enc") {
        // Legacy VAAPI fallback. Keep the argument set conservative because
        // property names differ between Ubuntu/GStreamer generations.
        args.insert(args.end(), {
            property("bitrate", std::to_string(bitrateKbps)),
            "rate-control=cbr"
        });
    } else {
        args.insert(args.end(), {
            "tune=zerolatency",
            "speed-preset=superfast",
            property("bitrate", std::to_string(bitrateKbps)),
            property("key-int-max", std::to_string(keyInt)),
            "bframes=0",
            property("byte-stream", flv ? "false" : "true"),
            "aud=true",
            "insert-vui=true",
            "sliced-threads=true",
            "vbv-buf-capacity=1000",
            "option-string=nal-hrd=cbr:force-cfr=1:repeat-headers=1:scenecut=0"
        });
    }
    return true;
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
    int& inheritedInputFd,
    bool& forceSoftwareH264Decode,
    std::string& error) {
    inheritedInputFd = -1;
    forceSoftwareH264Decode = forceSoftwareH264DecodeForPlatform();
    if (forceSoftwareH264Decode && !hasFactory("avdec_h264")) {
        error = "Intel Ivy Bridge transcoder recovery requires GStreamer avdec_h264";
        return false;
    }

    std::string multicastGroup;
    int multicastPort = 0;
    if (parseIpv4MulticastUdpUri(cfg, multicastGroup, multicastPort)) {
        std::vector<std::string> missing;
        validateFactories({"fdsrc", "tee", "queue", "watchdog", "fakesink",
                           "tsparse", "tsdemux", "decodebin3"}, missing);
        if (!missing.empty()) {
            std::ostringstream ss;
            ss << "missing protected UDP transcoder input elements";
            for (size_t i = 0; i < missing.size(); ++i) {
                ss << (i == 0 ? ": " : ", ") << missing[i];
            }
            error = ss.str();
            return false;
        }

        ProtectedUdpInput input;
        if (!createProtectedUdpInput(cfg, input, error)) return false;
        inheritedInputFd = input.fd;

        const uint32_t inputSid = effectiveInputServiceId(cfg);
        args.insert(args.end(), {
            "fdsrc",
            "name=transcode_udp_fdsrc",
            "fd=" + std::to_string(inheritedInputFd),
            "blocksize=65536",
            "do-timestamp=true",
            "!", "tee", "name=transcode_ts_input_tee",

            // Independent heartbeat branch: source loss is now detected on raw
            // UDP/TS buffers, not after the decoder.  Its leaky queue prevents
            // encoder/decode backpressure from masquerading as multicast loss.
            "transcode_ts_input_tee.", "!", "queue",
            "name=transcode_ts_watchdog_queue",
            "max-size-buffers=0",
            "max-size-bytes=2097152",
            "max-size-time=1000000000",
            "leaky=downstream",
            "!", "watchdog",
            "name=transcode_ts_input_watchdog",
            "timeout=" + std::to_string(kTranscoderTransportWatchdogMs),
            "!", "fakesink", "sync=false", "async=false",

            // Decode branch has enough headroom that the 15 s decoded-video
            // watchdog fires before this queue can backpressure the source tee.
            "transcode_ts_input_tee.", "!", "queue",
            "name=transcode_ts_decode_queue",
            "max-size-buffers=0",
            "max-size-bytes=33554432",
            "max-size-time=20000000000",
            "leaky=0",
            "!", "tsparse",
            "!", "tsdemux",
            "name=transcode_udp_demux",
            "latency=700"
        });
        if (inputSid != 0) {
            args.push_back("program-number=" + std::to_string(inputSid));
        }
        args.insert(args.end(), {
            "transcode_udp_demux.", ":", "decodebin3", "name=dec"
        });

        std::cerr << "Protected transcoder UDP 203.09: group=" << input.group
                  << " port=" << input.port
                  << " rcvbuf_bytes=" << input.effectiveReceiveBuffer
                  << " multicast_all=off"
                  << " interfaces=" << joinedNames(input.joinedInterfaces)
                  << " strict_device=" << (input.boundDevice.empty() ? "auto" : input.boundDevice)
                  << " input_watchdog_ms=" << kTranscoderTransportWatchdogMs
                  << " decoded_video_watchdog_ms=" << kTranscoderDecodedVideoWatchdogMs
                  << " input_sid=" << inputSid
                  << std::endl;
        if (forceSoftwareH264Decode) {
            std::cerr << "Transcoder H264 decode 203.09: platform=Intel-IvyBridge"
                      << " policy=software-recovery decoder=avdec_h264"
                      << " hardware_encode=preserved" << std::endl;
        }
        return true;
    }

    if (!isSidAwareMpegTsInput(cfg)) {
        tvs::protocols::appendDecodeInput(args, cfg);
        if (forceSoftwareH264Decode) {
            std::cerr << "Transcoder H264 decode 203.09: platform=Intel-IvyBridge"
                      << " policy=software-recovery decoder=avdec_h264"
                      << " hardware_encode=preserved" << std::endl;
        }
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
    if (forceSoftwareH264Decode) {
        std::cerr << "Transcoder H264 decode 203.09: platform=Intel-IvyBridge"
                  << " policy=software-recovery decoder=avdec_h264"
                  << " hardware_encode=preserved" << std::endl;
    }
    return true;
}

void addVideoBranch(std::vector<std::string>& args, const StreamConfig& cfg,
                    const GstOutputSpec& spec, std::string& error) {
    int width = 1920;
    int height = 1080;
    TranscoderModule::resolutionSize(cfg.transcodeResolution, width, height);
    const bool flv = spec.container == ContainerKind::Flv;
    const std::string encoderFactory = selectedVideoEncoderFactory(cfg);
    if (encoderFactory.empty()) {
        if (cfg.transcodeVideoEncoder == "nvenc") error = "NVIDIA NVENC nvh264enc is not available";
        else if (cfg.transcodeVideoEncoder == "intel") error = "Intel qsv/VA H.264 encoder did not pass the runtime probe";
        else if (cfg.transcodeVideoEncoder == "x264") error = "CPU x264enc is not available";
        else error = "no H.264 video encoder is available";
        return;
    }

    args.insert(args.end(), {"dec.", "!"});
    addQueue(args, "transcode_video_queue", 8000000000ULL);
    args.insert(args.end(), {
        "!", "watchdog",
        "name=transcode_decoded_video_watchdog",
        "timeout=" + std::to_string(kTranscoderDecodedVideoWatchdogMs),
        "!", "video/x-raw",
        "!", "videoconvert",
        "!", "deinterlace", "method=yadif", "mode=auto-strict", "fields=all", "locking=passive",
        "!", "videoscale", "add-borders=false", "method=lanczos"
    });
    if (encoderFactory == "nvh264enc" || isIntelVideoEncoder(encoderFactory)) {
        // Hardware encoders use NV12 system-memory input. Convert after YADIF/scale so the
        // deinterlacer can keep its proven CPU format and hand NV12 to the selected hardware encoder.
        args.insert(args.end(), {"!", "videoconvert"});
    }
    args.insert(args.end(), {"!", scaledVideoCaps(width, height, encoderFactory)});
    if (!appendVideoEncoder(args, cfg, flv, 50, error)) return;
    args.insert(args.end(), {
        // Keep parameter sets on every IDR for CPU and hardware encoders. nvh264enc
        // also enables repeat-sequence-header; h264parse normalizes the output
        // for late SRT/UDP joins and FLV/TS stream-format requirements.
        "!", "h264parse", property("config-interval", "-1"),
        "!", flv
            ? "video/x-h264,stream-format=avc,alignment=au"
            : "video/x-h264,stream-format=byte-stream,alignment=au",
        "!"
    });
    addQueue(args, "transcode_video_mux_queue", 3000000000ULL);
    args.insert(args.end(), {"!", spec.videoPad});

    std::cerr << "GStreamer transcoder video encoder: requested=" << cfg.transcodeVideoEncoder
              << " selected=" << encoderFactory
              << " output=" << width << "x" << height
              << " bitrate=" << tvs::protocols::safeVideoBitrate(cfg)
              << " headers=every-idr" << std::endl;
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
        "!", "audioresample", "quality=6",
        // Keep long-running HLS audio locked to the 48 kHz clock. A small
        // tolerance lets audiorate correct source jitter before it accumulates
        // into audible gaps at segment boundaries.
        "!", "audiorate", "skip-to-first=true", "tolerance=20000000",
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

    const std::string testVideoEncoder = selectedVideoEncoderFactory(testCfg);
    if (testVideoEncoder.empty()) {
        if (testCfg.transcodeVideoEncoder == "nvenc") error = "NVIDIA NVENC nvh264enc is not available";
        else if (testCfg.transcodeVideoEncoder == "intel") error = "Intel qsv/VA H.264 encoder did not pass the runtime probe";
        else error = "no H.264 video encoder is available";
        return;
    }
    args.insert(args.end(), {
        "videotestsrc", "is-live=true", "pattern=smpte", "!", "video/x-raw,framerate=25/1", "!"
    });
    addQueue(args, "test_video_queue", 3000000000ULL);
    args.insert(args.end(), {
        "!", "videoconvert", "!", "videoscale", "add-borders=false", "method=lanczos", "!", "videorate"
    });
    if (testVideoEncoder == "nvh264enc" || isIntelVideoEncoder(testVideoEncoder)) args.insert(args.end(), {"!", "videoconvert"});
    args.insert(args.end(), {"!", scaledVideoCaps(width, height, testVideoEncoder)});
    if (!appendVideoEncoder(args, testCfg, spec.container == ContainerKind::Flv, 25, error)) return;
    args.insert(args.end(), {
        "!", "h264parse", property("config-interval", "-1"),
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
        "h264parse", "audioconvert", "audioresample", "audiorate", "aacparse"
    };
    required.insert(required.end(), common.begin(), common.end());

    std::vector<std::string> missing;
    validateFactories(required, missing);
    if (!hasFactory("nvh264enc") && intelVideoEncoderFactory().empty() && !hasFactory("x264enc")) {
        missing.emplace_back("H.264 encoder: nvh264enc, Intel qsv/va or x264enc");
    }
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
    int inheritedInputFd,
    bool forceSoftwareH264Decode,
    ChildProcess& child,
    std::string& error) {
    if (args.empty()) {
        if (inheritedInputFd >= 0) ::close(inheritedInputFd);
        error = "empty gst-launch command";
        return false;
    }

    // gst-launch does not need any TVStreammerSAT5 sockets. Mark every currently open
    // non-standard descriptor close-on-exec before forking so HTTP/metrics/listener
    // sockets cannot remain alive in the external transcoder process.
    markOpenDescriptorsCloseOnExec();
    if (inheritedInputFd >= 0) {
        const int flags = ::fcntl(inheritedInputFd, F_GETFD);
        if (flags < 0 || ::fcntl(inheritedInputFd, F_SETFD, flags & ~FD_CLOEXEC) != 0) {
            const std::string fdError = std::strerror(errno);
            ::close(inheritedInputFd);
            error = "failed to preserve protected transcoder UDP fd across exec: " + fdError;
            return false;
        }
    }

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
        if (inheritedInputFd >= 0) ::close(inheritedInputFd);
        if (captureStderr) {
            ::close(stderrPipe[0]);
            ::close(stderrPipe[1]);
        }
        error = std::string("fork failed: ") + std::strerror(errno);
        return false;
    }

    if (pid == 0) {
        if (forceSoftwareH264Decode) {
            const std::string rankOverride = softwareH264FeatureRankOverride();
            if (::setenv("GST_PLUGIN_FEATURE_RANK", rankOverride.c_str(), 1) != 0) {
                std::_Exit(126);
            }
        }
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

    if (inheritedInputFd >= 0) ::close(inheritedInputFd);
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
    int& inheritedInputFd,
    bool& forceSoftwareH264Decode,
    std::string& description,
    std::string& error) {
    inheritedInputFd = -1;
    forceSoftwareH264Decode = false;
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
        std::cerr << "Transcoder watchdog 203.09: transport_timeout_ms="
                  << kTranscoderTransportWatchdogMs
                  << " decoded_video_timeout_ms=" << kTranscoderDecodedVideoWatchdogMs
                  << " source=" << tvs::protocols::inputUriForGstreamer(baseConfig)
                  << " transport_scope=raw-udp-ts-when-protected"
                  << " video_scope=decoded-video-recovery"
                  << std::endl;
        if (!appendTranscoderDecodeInput(
                args, baseConfig, inheritedInputFd, forceSoftwareH264Decode, error)) {
            if (inheritedInputFd >= 0) {
                ::close(inheritedInputFd);
                inheritedInputFd = -1;
            }
            return {};
        }
        std::cerr << "GStreamer transcoder 203.09: video=h264"
                  << " encoder_request=" << baseConfig.transcodeVideoEncoder
                  << " deinterlace=yadif-all-fields"
                  << " cadence=preserve-progressive/double-interlaced-fields"
                  << " output=" << baseConfig.transcodeResolution << std::endl;
        addVideoBranch(args, baseConfig, outputSpec, error);
        if (!error.empty()) {
            if (inheritedInputFd >= 0) {
                ::close(inheritedInputFd);
                inheritedInputFd = -1;
            }
            return {};
        }
        addAudioBranch(args, baseConfig, outputSpec, error);
    }
    if (!error.empty()) {
        if (inheritedInputFd >= 0) {
            ::close(inheritedInputFd);
            inheritedInputFd = -1;
        }
        return {};
    }

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
        int inheritedInputFd = -1;
        bool forceSoftwareH264Decode = false;
        std::vector<std::string> args = buildCommand(
            config, output, inheritedInputFd, forceSoftwareH264Decode,
            description, commandError);
        if (!commandError.empty()) {
            if (inheritedInputFd >= 0) ::close(inheritedInputFd);
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
        if (!spawnProcess(
                args, description, inheritedInputFd, forceSoftwareH264Decode,
                child, error)) {
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
