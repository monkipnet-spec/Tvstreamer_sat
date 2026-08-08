#include "DvbManager.h"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <glib.h>

#include <linux/dvb/frontend.h>

#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace tvs::dvb {
namespace {

struct ServiceInfo {
    uint16_t serviceId = 0;
    uint16_t pmtPid = 0;
    uint16_t videoPid = 0;
    uint16_t audioPid = 0;
    std::string name;
    std::string provider;
    std::string videoCodec;
    std::string audioCodec;
    bool scrambled = false;
    bool pmtSeen = false;
};

std::string lowerCopy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool startsWith(const std::string& value, const std::string& prefix) {
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool parseNumericSuffix(const std::string& value, const std::string& prefix, int& out) {
    if (!startsWith(value, prefix) || value.size() == prefix.size()) {
        return false;
    }
    const std::string suffix = value.substr(prefix.size());
    if (!std::all_of(suffix.begin(), suffix.end(), [](unsigned char ch) { return std::isdigit(ch); })) {
        return false;
    }
    try {
        out = std::stoi(suffix);
        return out >= 0;
    } catch (...) {
        return false;
    }
}

std::string deliverySystemName(fe_delivery_system system) {
    switch (system) {
        case SYS_DVBS: return "DVB-S";
        case SYS_DVBS2: return "DVB-S2";
#ifdef SYS_DVBS2X
        case SYS_DVBS2X: return "DVB-S2X";
#endif
        case SYS_DVBT: return "DVB-T";
        case SYS_DVBT2: return "DVB-T2";
        case SYS_DVBC_ANNEX_A: return "DVB-C Annex A";
        case SYS_DVBC_ANNEX_B: return "DVB-C Annex B";
        case SYS_DVBC_ANNEX_C: return "DVB-C Annex C";
        case SYS_ATSC: return "ATSC";
        case SYS_ISDBT: return "ISDB-T";
        case SYS_ISDBS: return "ISDB-S";
        default: return "system-" + std::to_string(static_cast<int>(system));
    }
}

std::vector<std::string> frontendDeliverySystems(int fd) {
    std::vector<std::string> result;
#ifdef DTV_ENUM_DELSYS
    dtv_property property {};
    property.cmd = DTV_ENUM_DELSYS;
    dtv_properties properties {};
    properties.num = 1;
    properties.props = &property;
    if (::ioctl(fd, FE_GET_PROPERTY, &properties) == 0) {
        for (uint32_t i = 0; i < property.u.buffer.len; ++i) {
            result.push_back(deliverySystemName(
                static_cast<fe_delivery_system>(property.u.buffer.data[i])));
        }
    }
#endif
    return result;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input.is_open()) return {};
    std::ostringstream stream;
    stream << input.rdbuf();
    std::string value = stream.str();
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) value.pop_back();
    return value;
}

std::string firstExistingParentAttribute(std::filesystem::path path, const std::string& name) {
    std::error_code ec;
    for (int depth = 0; depth < 8 && !path.empty(); ++depth) {
        const auto candidate = path / name;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            const std::string value = readTextFile(candidate);
            if (!value.empty()) return value;
        }
        ec.clear();
        path = path.parent_path();
    }
    return {};
}

std::string firstExistingDriver(std::filesystem::path path) {
    std::error_code ec;
    for (int depth = 0; depth < 8 && !path.empty(); ++depth) {
        const auto candidate = path / "driver";
        if (std::filesystem::exists(candidate, ec) && !ec) {
            const auto link = std::filesystem::read_symlink(candidate, ec);
            if (!ec) return link.filename().string();
        }
        ec.clear();
        path = path.parent_path();
    }
    return {};
}

Json::Value enumerateSerialReaders() {
    Json::Value readers(Json::arrayValue);
    const std::filesystem::path dev("/dev");
    std::error_code ec;
    if (!std::filesystem::exists(dev, ec)) return readers;

    for (const auto& entry : std::filesystem::directory_iterator(dev, ec)) {
        if (ec) break;
        const std::string name = entry.path().filename().string();
        if (!startsWith(name, "ttyUSB") && !startsWith(name, "ttyACM")) continue;

        Json::Value item;
        item["device"] = entry.path().string();
        item["name"] = name;

        std::filesystem::path sysPath = std::filesystem::path("/sys/class/tty") / name / "device";
        const auto canonical = std::filesystem::weakly_canonical(sysPath, ec);
        if (!ec) sysPath = canonical;
        ec.clear();

        const std::string driver = firstExistingDriver(sysPath);
        const std::string vendor = firstExistingParentAttribute(sysPath, "idVendor");
        const std::string productId = firstExistingParentAttribute(sysPath, "idProduct");
        const std::string manufacturer = firstExistingParentAttribute(sysPath, "manufacturer");
        const std::string product = firstExistingParentAttribute(sysPath, "product");
        const std::string serial = firstExistingParentAttribute(sysPath, "serial");

        item["driver"] = driver;
        item["vendor_id"] = vendor;
        item["product_id"] = productId;
        item["manufacturer"] = manufacturer;
        item["product"] = product;
        item["serial"] = serial;

        const std::string identity = lowerCopy(driver + " " + manufacturer + " " + product);
        const bool phoenixCandidate =
            identity.find("phoenix") != std::string::npos ||
            identity.find("smartmouse") != std::string::npos ||
            identity.find("smartcard") != std::string::npos ||
            driver == "ftdi_sio" || driver == "pl2303" || driver == "cp210x" ||
            driver == "ch341" || driver == "usbserial";
        item["phoenix_candidate"] = phoenixCandidate;
        readers.append(item);
    }
    return readers;
}

bool hasObjectProperty(GObject* object, const char* name) {
    return object && g_object_class_find_property(G_OBJECT_GET_CLASS(object), name) != nullptr;
}

void setIntIfPresent(GObject* object, const char* name, gint value) {
    if (hasObjectProperty(object, name)) g_object_set(object, name, value, nullptr);
}

void setUIntIfPresent(GObject* object, const char* name, guint value) {
    if (hasObjectProperty(object, name)) g_object_set(object, name, value, nullptr);
}

void setStringIfPresent(GObject* object, const char* name, const std::string& value) {
    if (hasObjectProperty(object, name) && !value.empty()) {
        g_object_set(object, name, value.c_str(), nullptr);
    }
}

void setSerializedIfPresent(GObject* object, const char* name, const std::string& value) {
    if (!hasObjectProperty(object, name) || value.empty()) return;
    gst_util_set_object_arg(object, name, value.c_str());
}

void configureSatelliteSource(GstElement* source, const StreamConfig& cfg) {
    GObject* object = G_OBJECT(source);
    setIntIfPresent(object, "adapter", cfg.satelliteAdapter);
    setIntIfPresent(object, "frontend", cfg.satelliteFrontend);
    setUIntIfPresent(object, "frequency", cfg.satelliteFrequency);
    setUIntIfPresent(object, "symbol-rate", cfg.satelliteSymbolRate);
    setStringIfPresent(object, "polarity", cfg.satellitePolarization);
    setSerializedIfPresent(object, "delsys", cfg.satelliteDeliverySystem);
    setSerializedIfPresent(object, "modulation", cfg.satelliteModulation);
    setSerializedIfPresent(object, "code-rate-hp", cfg.satelliteFec);
    setSerializedIfPresent(object, "pilot", cfg.satellitePilot);
    setSerializedIfPresent(object, "rolloff", cfg.satelliteRolloff);
    setIntIfPresent(object, "diseqc-source", cfg.satelliteDiseqcSource);
    setIntIfPresent(object, "stream-id", cfg.satelliteStreamId);
    setUIntIfPresent(object, "lnb-lof1", cfg.satelliteLnbLof1);
    setUIntIfPresent(object, "lnb-lof2", cfg.satelliteLnbLof2);
    setUIntIfPresent(object, "lnb-slof", cfg.satelliteLnbSlof);
    setUIntIfPresent(object, "stats-reporting-interval", 10);
}

std::string dvbTextToUtf8(const uint8_t* data, size_t length) {
    if (!data || length == 0) return {};

    const char* charset = "ISO_6937";
    size_t offset = 0;
    if (data[0] == 0x15) {
        charset = "UTF-8";
        offset = 1;
    } else if (data[0] >= 0x01 && data[0] <= 0x0B) {
        // EN 300 468 single-byte selectors. 0x01 maps to ISO-8859-5.
        static const std::array<const char*, 12> charsets = {
            "", "ISO-8859-5", "ISO-8859-6", "ISO-8859-7", "ISO-8859-8",
            "ISO-8859-9", "ISO-8859-10", "ISO-8859-11", "ISO-8859-12",
            "ISO-8859-13", "ISO-8859-14", "ISO-8859-15"
        };
        charset = charsets[data[0]];
        offset = 1;
    } else if (data[0] == 0x10 && length >= 3 && data[1] == 0x00 && data[2] >= 1 && data[2] <= 15) {
        static thread_local std::string dynamicCharset;
        dynamicCharset = "ISO-8859-" + std::to_string(data[2]);
        charset = dynamicCharset.c_str();
        offset = 3;
    }

    if (offset >= length) return {};
    if (std::string(charset) == "UTF-8") {
        return std::string(reinterpret_cast<const char*>(data + offset), length - offset);
    }

    GError* error = nullptr;
    gsize written = 0;
    gchar* converted = g_convert(
        reinterpret_cast<const gchar*>(data + offset),
        static_cast<gssize>(length - offset),
        "UTF-8", charset, nullptr, &written, &error);
    if (converted) {
        std::string result(converted, written);
        g_free(converted);
        if (error) g_error_free(error);
        return result;
    }
    if (error) g_error_free(error);

    // Fallback keeps ASCII readable even when the broadcaster uses a charset
    // that iconv on the host does not know.
    std::string fallback;
    fallback.reserve(length - offset);
    for (size_t i = offset; i < length; ++i) {
        const unsigned char ch = data[i];
        fallback.push_back(ch >= 0x20 ? static_cast<char>(ch) : ' ');
    }
    return fallback;
}

bool descriptorLoopContainsCa(const uint8_t* data, size_t length) {
    size_t pos = 0;
    while (pos + 2 <= length) {
        const uint8_t tag = data[pos];
        const size_t descriptorLength = data[pos + 1];
        if (pos + 2 + descriptorLength > length) break;
        if (tag == 0x09) return true; // CA_descriptor
        pos += 2 + descriptorLength;
    }
    return false;
}

std::pair<std::string, std::string> parseServiceDescriptor(const uint8_t* data, size_t length) {
    size_t pos = 0;
    while (pos + 2 <= length) {
        const uint8_t tag = data[pos];
        const size_t descriptorLength = data[pos + 1];
        if (pos + 2 + descriptorLength > length) break;
        if (tag == 0x48 && descriptorLength >= 3) {
            const uint8_t* descriptor = data + pos + 2;
            size_t inner = 1; // service_type
            if (inner >= descriptorLength) break;
            const size_t providerLength = descriptor[inner++];
            if (inner + providerLength > descriptorLength) break;
            std::string provider = dvbTextToUtf8(descriptor + inner, providerLength);
            inner += providerLength;
            if (inner >= descriptorLength) return {provider, {}};
            const size_t nameLength = descriptor[inner++];
            if (inner + nameLength > descriptorLength) break;
            std::string name = dvbTextToUtf8(descriptor + inner, nameLength);
            return {provider, name};
        }
        pos += 2 + descriptorLength;
    }
    return {};
}

bool isVideoStreamType(uint8_t type) {
    return type == 0x01 || type == 0x02 || type == 0x10 || type == 0x1B || type == 0x24 || type == 0x42;
}

bool isAudioStreamType(uint8_t type) {
    return type == 0x03 || type == 0x04 || type == 0x0F || type == 0x11 || type == 0x81 || type == 0x87;
}

std::string videoCodecName(uint8_t type) {
    switch (type) {
        case 0x01: return "MPEG-1 Video";
        case 0x02: return "MPEG-2 Video";
        case 0x10: return "MPEG-4 Video";
        case 0x1B: return "H.264/AVC";
        case 0x24: return "H.265/HEVC";
        case 0x42: return "AVS";
        default: return "video";
    }
}

std::string audioCodecName(uint8_t type, const uint8_t* descriptors, size_t descriptorLength) {
    switch (type) {
        case 0x03: return "MPEG-1 Audio";
        case 0x04: return "MPEG-2 Audio";
        case 0x0F: return "AAC";
        case 0x11: return "AAC LATM";
        case 0x81: return "AC-3";
        case 0x87: return "E-AC-3";
        case 0x06: {
            size_t pos = 0;
            while (pos + 2 <= descriptorLength) {
                const uint8_t tag = descriptors[pos];
                const size_t len = descriptors[pos + 1];
                if (pos + 2 + len > descriptorLength) break;
                if (tag == 0x6A) return "AC-3";
                if (tag == 0x7A) return "E-AC-3";
                if (tag == 0x7B) return "DTS";
                if (tag == 0x56) return "Teletext";
                pos += 2 + len;
            }
            return "Private audio/data";
        }
        default: return "audio";
    }
}

class PsiScanner {
public:
    void feed(const uint8_t* data, size_t length) {
        if (!data || length == 0) return;
        remainder_.insert(remainder_.end(), data, data + length);
        size_t pos = 0;
        while (pos + 188 <= remainder_.size()) {
            if (remainder_[pos] != 0x47) {
                const auto sync = std::find(remainder_.begin() + static_cast<std::ptrdiff_t>(pos + 1),
                                            remainder_.end(), 0x47);
                if (sync == remainder_.end()) {
                    pos = remainder_.size();
                    break;
                }
                pos = static_cast<size_t>(std::distance(remainder_.begin(), sync));
                continue;
            }
            feedPacket(remainder_.data() + pos);
            pos += 188;
        }
        if (pos > 0) remainder_.erase(remainder_.begin(), remainder_.begin() + static_cast<std::ptrdiff_t>(pos));
        if (remainder_.size() > 188 * 8) remainder_.clear();
    }

    bool hasPat() const { return patSeen_; }
    bool hasSdt() const { return sdtSeen_; }

    bool allKnownPmtsSeen() const {
        bool any = false;
        for (const auto& [sid, service] : services_) {
            (void)sid;
            if (service.pmtPid == 0) continue;
            any = true;
            if (!service.pmtSeen) return false;
        }
        return any;
    }

    const std::map<uint16_t, ServiceInfo>& services() const { return services_; }

private:
    struct SectionState {
        std::vector<uint8_t> bytes;
    };

    void feedPacket(const uint8_t* packet) {
        if ((packet[1] & 0x80) != 0) return; // transport error
        const bool payloadStart = (packet[1] & 0x40) != 0;
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        if (pid != 0x0000 && pid != 0x0011 && pmtPidToService_.find(pid) == pmtPidToService_.end()) {
            return;
        }
        const uint8_t adaptationControl = (packet[3] >> 4) & 0x03;
        if (adaptationControl == 0 || adaptationControl == 2) return;

        size_t pos = 4;
        if (adaptationControl == 3) {
            const size_t adaptationLength = packet[pos];
            pos += 1 + adaptationLength;
            if (pos >= 188) return;
        }
        const uint8_t* payload = packet + pos;
        size_t payloadLength = 188 - pos;
        auto& state = sections_[pid];

        if (payloadStart) {
            if (payloadLength == 0) return;
            const size_t pointer = payload[0];
            ++payload;
            --payloadLength;
            const size_t previousBytes = std::min(pointer, payloadLength);
            if (!state.bytes.empty() && previousBytes > 0) {
                appendSectionBytes(pid, state, payload, previousBytes);
            }
            payload += previousBytes;
            payloadLength -= previousBytes;
            if (!state.bytes.empty()) state.bytes.clear();
            if (pointer > previousBytes) return;
        }

        appendSectionBytes(pid, state, payload, payloadLength);
    }

    void appendSectionBytes(uint16_t pid, SectionState& state, const uint8_t* data, size_t length) {
        if (!data || length == 0) return;
        state.bytes.insert(state.bytes.end(), data, data + length);
        while (state.bytes.size() >= 3) {
            if (state.bytes[0] == 0xFF) {
                state.bytes.clear();
                return;
            }
            const size_t sectionLength = static_cast<size_t>(((state.bytes[1] & 0x0F) << 8) | state.bytes[2]);
            const size_t totalLength = 3 + sectionLength;
            if (totalLength < 4 || totalLength > 4096) {
                state.bytes.erase(state.bytes.begin());
                continue;
            }
            if (state.bytes.size() < totalLength) return;
            std::vector<uint8_t> section(state.bytes.begin(), state.bytes.begin() + static_cast<std::ptrdiff_t>(totalLength));
            state.bytes.erase(state.bytes.begin(), state.bytes.begin() + static_cast<std::ptrdiff_t>(totalLength));
            parseSection(pid, section);
        }
    }

    void parseSection(uint16_t pid, const std::vector<uint8_t>& section) {
        if (section.size() < 8) return;
        switch (section[0]) {
            case 0x00: parsePat(section); break;
            case 0x42: parseSdt(section); break;
            case 0x02: parsePmt(pid, section); break;
            default: break;
        }
    }

    void parsePat(const std::vector<uint8_t>& section) {
        if (section.size() < 12) return;
        const size_t end = section.size() >= 4 ? section.size() - 4 : 0;
        for (size_t pos = 8; pos + 4 <= end; pos += 4) {
            const uint16_t program = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
            const uint16_t pid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
            if (program == 0) continue;
            auto& service = services_[program];
            service.serviceId = program;
            service.pmtPid = pid;
            pmtPidToService_[pid] = program;
        }
        patSeen_ = true;
    }

    void parseSdt(const std::vector<uint8_t>& section) {
        if (section.size() < 15) return;
        const size_t end = section.size() - 4;
        size_t pos = 11;
        while (pos + 5 <= end) {
            const uint16_t serviceId = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
            const bool freeCa = (section[pos + 3] & 0x10) != 0;
            const size_t descriptorsLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
            if (pos + 5 + descriptorsLength > end) break;
            auto& service = services_[serviceId];
            service.serviceId = serviceId;
            service.scrambled = service.scrambled || freeCa ||
                descriptorLoopContainsCa(section.data() + pos + 5, descriptorsLength);
            const auto [provider, name] = parseServiceDescriptor(section.data() + pos + 5, descriptorsLength);
            if (!provider.empty()) service.provider = provider;
            if (!name.empty()) service.name = name;
            pos += 5 + descriptorsLength;
        }
        sdtSeen_ = true;
    }

    void parsePmt(uint16_t pid, const std::vector<uint8_t>& section) {
        if (section.size() < 16) return;
        uint16_t serviceId = static_cast<uint16_t>((section[3] << 8) | section[4]);
        const auto mapped = pmtPidToService_.find(pid);
        if (mapped != pmtPidToService_.end()) serviceId = mapped->second;
        auto& service = services_[serviceId];
        service.serviceId = serviceId;
        if (service.pmtPid == 0) service.pmtPid = pid;
        service.pmtSeen = true;

        const size_t end = section.size() - 4;
        const size_t programInfoLength = static_cast<size_t>(((section[10] & 0x0F) << 8) | section[11]);
        if (12 + programInfoLength > end) return;
        if (descriptorLoopContainsCa(section.data() + 12, programInfoLength)) service.scrambled = true;

        size_t pos = 12 + programInfoLength;
        while (pos + 5 <= end) {
            const uint8_t streamType = section[pos];
            const uint16_t elementaryPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
            const size_t esInfoLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
            if (pos + 5 + esInfoLength > end) break;
            const uint8_t* descriptors = section.data() + pos + 5;
            if (descriptorLoopContainsCa(descriptors, esInfoLength)) service.scrambled = true;

            if (service.videoPid == 0 && isVideoStreamType(streamType)) {
                service.videoPid = elementaryPid;
                service.videoCodec = videoCodecName(streamType);
            }
            if (service.audioPid == 0 && (isAudioStreamType(streamType) || streamType == 0x06)) {
                const std::string codec = audioCodecName(streamType, descriptors, esInfoLength);
                if (streamType != 0x06 || codec != "Private audio/data") {
                    service.audioPid = elementaryPid;
                    service.audioCodec = codec;
                }
            }
            pos += 5 + esInfoLength;
        }
    }

    std::vector<uint8_t> remainder_;
    std::unordered_map<uint16_t, SectionState> sections_;
    std::map<uint16_t, ServiceInfo> services_;
    std::unordered_map<uint16_t, uint16_t> pmtPidToService_;
    bool patSeen_ = false;
    bool sdtSeen_ = false;
};

Json::Value frontendStatusJsonImpl(int adapter, int frontend) {
    Json::Value result;
    const std::string path = "/dev/dvb/adapter" + std::to_string(adapter) +
        "/frontend" + std::to_string(frontend);
    const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return result;

    fe_status_t status = static_cast<fe_status_t>(0);
    if (::ioctl(fd, FE_READ_STATUS, &status) == 0) {
        result["has_signal"] = (status & FE_HAS_SIGNAL) != 0;
        result["has_carrier"] = (status & FE_HAS_CARRIER) != 0;
        result["has_viterbi"] = (status & FE_HAS_VITERBI) != 0;
        result["has_sync"] = (status & FE_HAS_SYNC) != 0;
        result["has_lock"] = (status & FE_HAS_LOCK) != 0;
    }
    uint16_t strength = 0;
    if (::ioctl(fd, FE_READ_SIGNAL_STRENGTH, &strength) == 0) result["signal_strength_raw"] = strength;
    uint16_t snr = 0;
    if (::ioctl(fd, FE_READ_SNR, &snr) == 0) result["snr_raw"] = snr;
    uint32_t ber = 0;
    if (::ioctl(fd, FE_READ_BER, &ber) == 0) result["ber"] = Json::UInt(ber);
    uint32_t unc = 0;
    if (::ioctl(fd, FE_READ_UNCORRECTED_BLOCKS, &unc) == 0) result["uncorrected_blocks"] = Json::UInt(unc);
    ::close(fd);
    return result;
}

std::string gstErrorFromMessage(GstMessage* message) {
    if (!message || GST_MESSAGE_TYPE(message) != GST_MESSAGE_ERROR) return {};
    GError* error = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &error, &debug);
    std::string result = error && error->message ? error->message : "GStreamer DVB scan error";
    if (debug && *debug) result += std::string(" | ") + debug;
    if (error) g_error_free(error);
    g_free(debug);
    return result;
}

} // namespace

Json::Value frontendStatus(int adapter, int frontend) {
    return frontendStatusJsonImpl(adapter, frontend);
}

Json::Value enumerateDevices() {
    Json::Value root;
    Json::Value adapters(Json::arrayValue);
    const std::filesystem::path dvbRoot("/dev/dvb");
    std::error_code ec;

    if (std::filesystem::exists(dvbRoot, ec) && !ec) {
        std::vector<std::pair<int, std::filesystem::path>> adapterPaths;
        for (const auto& entry : std::filesystem::directory_iterator(dvbRoot, ec)) {
            if (ec || !entry.is_directory(ec)) continue;
            int adapter = -1;
            if (parseNumericSuffix(entry.path().filename().string(), "adapter", adapter)) {
                adapterPaths.emplace_back(adapter, entry.path());
            }
        }
        std::sort(adapterPaths.begin(), adapterPaths.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });

        for (const auto& [adapterNumber, adapterPath] : adapterPaths) {
            Json::Value adapter;
            adapter["adapter"] = adapterNumber;
            adapter["path"] = adapterPath.string();
            Json::Value frontends(Json::arrayValue);
            Json::Value caDevices(Json::arrayValue);
            std::vector<std::pair<int, std::filesystem::path>> frontendPaths;

            for (const auto& child : std::filesystem::directory_iterator(adapterPath, ec)) {
                if (ec) break;
                const std::string name = child.path().filename().string();
                int number = -1;
                if (parseNumericSuffix(name, "frontend", number)) {
                    frontendPaths.emplace_back(number, child.path());
                } else if (parseNumericSuffix(name, "ca", number)) {
                    Json::Value ca;
                    ca["index"] = number;
                    ca["device"] = child.path().string();
                    caDevices.append(ca);
                }
            }
            ec.clear();
            std::sort(frontendPaths.begin(), frontendPaths.end(), [](const auto& left, const auto& right) {
                return left.first < right.first;
            });

            for (const auto& [frontendNumber, frontendPath] : frontendPaths) {
                Json::Value frontend;
                frontend["frontend"] = frontendNumber;
                frontend["device"] = frontendPath.string();
                frontend["name"] = frontendPath.filename().string();
                const int fd = ::open(frontendPath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
                if (fd >= 0) {
                    dvb_frontend_info info {};
                    if (::ioctl(fd, FE_GET_INFO, &info) == 0) {
                        frontend["name"] = info.name;
                        frontend["frequency_min"] = Json::UInt(info.frequency_min);
                        frontend["frequency_max"] = Json::UInt(info.frequency_max);
                        frontend["symbol_rate_min"] = Json::UInt(info.symbol_rate_min);
                        frontend["symbol_rate_max"] = Json::UInt(info.symbol_rate_max);
                    }
                    Json::Value systems(Json::arrayValue);
                    for (const auto& system : frontendDeliverySystems(fd)) systems.append(system);
                    frontend["delivery_systems"] = systems;
                    ::close(fd);
                } else {
                    frontend["open_error"] = "permission denied or frontend unavailable";
                }
                frontends.append(frontend);
            }

            adapter["frontends"] = frontends;
            adapter["ca_devices"] = caDevices;
            adapters.append(adapter);
        }
    }

    root["adapters"] = adapters;
    root["serial_readers"] = enumerateSerialReaders();
    root["dvb_root_present"] = std::filesystem::exists(dvbRoot, ec) && !ec;
    return root;
}

Json::Value scanTransponder(const StreamConfig& cfg, unsigned timeoutMs) {
    Json::Value result;
    result["adapter"] = cfg.satelliteAdapter;
    result["frontend"] = cfg.satelliteFrontend;
    result["frequency_khz"] = cfg.satelliteFrequency;
    result["symbol_rate_kbd"] = cfg.satelliteSymbolRate;
    result["delivery_system"] = cfg.satelliteDeliverySystem;
    result["polarization"] = cfg.satellitePolarization;

    if (cfg.satelliteAdapter < 0 || cfg.satelliteFrontend < 0 ||
        cfg.satelliteFrequency == 0 || cfg.satelliteSymbolRate == 0) {
        result["result"] = "error";
        result["error"] = "adapter/frontend/frequency/symbol rate are required";
        return result;
    }

    GstElement* pipeline = gst_pipeline_new("satellite_scan_pipeline");
    GstElement* source = gst_element_factory_make("dvbbasebin", "satellite_scan_source");
    GstElement* sink = gst_element_factory_make("appsink", "satellite_scan_sink");
    if (!pipeline || !source || !sink) {
        result["result"] = "error";
        result["error"] = "GStreamer dvbbasebin/appsink is not available";
        if (pipeline) gst_object_unref(pipeline);
        if (source) gst_object_unref(source);
        if (sink) gst_object_unref(sink);
        return result;
    }

    configureSatelliteSource(source, cfg);
    g_object_set(sink,
        "sync", FALSE,
        "async", FALSE,
        "emit-signals", FALSE,
        "max-buffers", 64U,
        "drop", TRUE,
        nullptr);

    gst_bin_add_many(GST_BIN(pipeline), source, sink, nullptr);
    if (!gst_element_link(source, sink)) {
        result["result"] = "error";
        result["error"] = "failed to link dvbbasebin to appsink";
        gst_object_unref(pipeline);
        return result;
    }

    GstBus* bus = gst_element_get_bus(pipeline);
    const GstStateChangeReturn stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (stateResult == GST_STATE_CHANGE_FAILURE) {
        result["result"] = "error";
        result["error"] = "failed to start DVB scan pipeline";
        gst_element_set_state(pipeline, GST_STATE_NULL);
        if (bus) gst_object_unref(bus);
        gst_object_unref(pipeline);
        return result;
    }

    PsiScanner scanner;
    bool receivedTransport = false;
    std::string scanError;
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(std::max(1000U, timeoutMs));

    while (std::chrono::steady_clock::now() < deadline) {
        if (bus) {
            while (GstMessage* message = gst_bus_pop_filtered(bus,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                    scanError = gstErrorFromMessage(message);
                } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS && scanError.empty()) {
                    scanError = "DVB scan pipeline reached EOS";
                }
                gst_message_unref(message);
                if (!scanError.empty()) break;
            }
        }
        if (!scanError.empty()) break;

        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 250 * GST_MSECOND);
        if (sample) {
            GstBuffer* buffer = gst_sample_get_buffer(sample);
            if (buffer) {
                GstMapInfo map {};
                if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    if (map.size > 0) {
                        receivedTransport = true;
                        scanner.feed(map.data, map.size);
                    }
                    gst_buffer_unmap(buffer, &map);
                }
            }
            gst_sample_unref(sample);
        }

        const auto elapsed = std::chrono::steady_clock::now() - started;
        if (elapsed > std::chrono::milliseconds(1500) && scanner.hasPat() && scanner.hasSdt() && scanner.allKnownPmtsSeen()) {
            break;
        }
    }

    Json::Value frontend = frontendStatusJsonImpl(cfg.satelliteAdapter, cfg.satelliteFrontend);
    result["frontend_status"] = frontend;
    result["lock"] = frontend.isMember("has_lock")
        ? frontend.get("has_lock", false).asBool()
        : receivedTransport;

    Json::Value services(Json::arrayValue);
    for (const auto& [serviceId, service] : scanner.services()) {
        if (serviceId == 0) continue;
        Json::Value item;
        item["service_id"] = serviceId;
        item["pmt_pid"] = service.pmtPid;
        item["video_pid"] = service.videoPid;
        item["audio_pid"] = service.audioPid;
        item["name"] = service.name.empty() ? ("Service " + std::to_string(serviceId)) : service.name;
        item["provider"] = service.provider;
        item["video_codec"] = service.videoCodec;
        item["audio_codec"] = service.audioCodec;
        item["scrambled"] = service.scrambled;
        item["pmt_seen"] = service.pmtSeen;
        services.append(item);
    }
    result["services"] = services;
    result["service_count"] = Json::UInt(services.size());
    result["pat_seen"] = scanner.hasPat();
    result["sdt_seen"] = scanner.hasSdt();
    result["transport_received"] = receivedTransport;

    if (!scanError.empty()) {
        result["result"] = "error";
        result["error"] = scanError;
    } else if (!receivedTransport) {
        result["result"] = "error";
        result["error"] = "no MPEG-TS data received from the configured transponder";
    } else if (services.empty()) {
        result["result"] = "error";
        result["error"] = "transport received, but no PAT/SDT services were discovered";
    } else {
        result["result"] = "ok";
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (bus) gst_object_unref(bus);
    gst_object_unref(pipeline);
    return result;
}

} // namespace tvs::dvb
