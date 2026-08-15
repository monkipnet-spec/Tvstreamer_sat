#include "DvbSatellite.h"

#include <gst/app/gstappsink.h>
#include <glib.h>
#include <linux/dvb/frontend.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string_view>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct DvbService {
    uint16_t serviceId = 0;
    uint16_t pmtPid = 0;
    uint16_t pcrPid = 0x1FFF;
    uint8_t serviceType = 0;
    bool scrambled = false;
    bool pmtParsed = false;
    std::vector<uint16_t> streamPids;
    std::vector<uint16_t> caPids;      // compatibility: ECM + EMM
    std::vector<uint16_t> ecmPids;
    std::vector<uint16_t> emmPids;
    std::string name;
    std::string provider;
};

struct FrontendStats {
    bool available = false;
    bool locked = false;
    int signalPercent = 0;
    int qualityPercent = 0;
    double signalDb = 0.0;
    double cnrDb = 0.0;
    bool hasSignalDb = false;
    bool hasCnrDb = false;
};

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

std::string percentEncode(const std::string& value) {
    std::ostringstream out;
    const char hex[] = "0123456789ABCDEF";
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out << static_cast<char>(c);
        } else {
            out << '%' << hex[c >> 4] << hex[c & 0x0F];
        }
    }
    return out.str();
}

int fromHex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string percentDecode(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            const int hi = fromHex(value[i + 1]);
            const int lo = fromHex(value[i + 2]);
            if (hi >= 0 && lo >= 0) {
                result.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        if (value[i] == '+') result.push_back(' ');
        else result.push_back(value[i]);
    }
    return result;
}

std::map<std::string, std::string> parseQuery(const std::string& uri) {
    std::map<std::string, std::string> values;
    const auto pos = uri.find('?');
    if (pos == std::string::npos) return values;
    std::string_view query(uri.data() + pos + 1, uri.size() - pos - 1);
    size_t start = 0;
    while (start <= query.size()) {
        const size_t end = query.find('&', start);
        const std::string_view item = query.substr(start, end == std::string_view::npos ? query.size() - start : end - start);
        const size_t eq = item.find('=');
        if (eq != std::string_view::npos) {
            values[percentDecode(std::string(item.substr(0, eq)))] = percentDecode(std::string(item.substr(eq + 1)));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return values;
}

bool parseInt(const std::map<std::string, std::string>& query, const std::string& key, int& value) {
    const auto found = query.find(key);
    if (found == query.end() || found->second.empty()) return true;
    try {
        size_t used = 0;
        const int parsed = std::stoi(found->second, &used);
        if (used != found->second.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool parseUInt(const std::map<std::string, std::string>& query, const std::string& key, uint32_t& value) {
    const auto found = query.find(key);
    if (found == query.end() || found->second.empty()) return true;
    try {
        size_t used = 0;
        const unsigned long parsed = std::stoul(found->second, &used);
        if (used != found->second.size() || parsed > 0xFFFFFFFFUL) return false;
        value = static_cast<uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

DvbSatelliteParams paramsFromJson(const Json::Value& root, std::string& error) {
    DvbSatelliteParams params;
    params.adapter = std::clamp(root.get("adapter", 0).asInt(), 0, 31);
    params.frontend = std::clamp(root.get("frontend", 0).asInt(), 0, 31);

    const double frequencyMHz = root.get("frequency_mhz", 11727.0).asDouble();
    if (!std::isfinite(frequencyMHz) || frequencyMHz < 900.0 || frequencyMHz > 14000.0) {
        error = "Frequency must be specified in MHz (900..14000)";
        return params;
    }
    params.frequencyKHz = static_cast<uint32_t>(std::llround(frequencyMHz * 1000.0));
    params.symbolRateK = root.get("symbol_rate", 27500).asUInt();
    if (params.symbolRateK < 100 || params.symbolRateK > 60000) {
        error = "Symbol rate must be in kSym/s (100..60000)";
        return params;
    }

    params.polarity = upper(root.get("polarity", "H").asString());
    if (params.polarity != "H" && params.polarity != "V") {
        error = "Polarity must be H or V";
        return params;
    }
    params.deliverySystem = lower(root.get("delivery_system", "dvb-s2").asString());
    if (params.deliverySystem != "dvb-s" && params.deliverySystem != "dvb-s2") {
        error = "Delivery system must be dvb-s or dvb-s2";
        return params;
    }
    params.modulation = lower(root.get("modulation", "auto").asString());
    static const std::set<std::string> allowedModulations = {"auto", "qpsk", "8psk", "16apsk", "32apsk"};
    if (!allowedModulations.count(params.modulation)) params.modulation = "auto";
    params.fec = lower(root.get("fec", "auto").asString());
    static const std::set<std::string> allowedFec = {"auto", "1/2", "2/3", "3/4", "4/5", "5/6", "6/7", "7/8", "8/9", "9/10", "3/5", "2/5"};
    if (!allowedFec.count(params.fec)) params.fec = "auto";
    params.diseqcSource = std::clamp(root.get("diseqc_source", -1).asInt(), -1, 7);
    params.lnbLof1KHz = root.get("lnb_lof1_mhz", 9750).asUInt() * 1000U;
    params.lnbLof2KHz = root.get("lnb_lof2_mhz", 10600).asUInt() * 1000U;
    params.lnbSlofKHz = root.get("lnb_slof_mhz", 11700).asUInt() * 1000U;
    params.streamId = std::clamp(root.get("stream_id", -1).asInt(), -1, 255);
    return params;
}

std::string frontendPath(const DvbSatelliteParams& params) {
    return "/dev/dvb/adapter" + std::to_string(params.adapter) + "/frontend" + std::to_string(params.frontend);
}

int clampPercent(double value) {
    return std::clamp(static_cast<int>(std::lround(value)), 0, 100);
}

bool readPropertyStat(int fd, uint32_t command, int& percent, double& db, bool& hasDb) {
    dtv_property property{};
    property.cmd = command;
    dtv_properties properties{};
    properties.num = 1;
    properties.props = &property;
    if (ioctl(fd, FE_GET_PROPERTY, &properties) != 0 || property.u.st.len < 1) return false;

    const auto& stat = property.u.st.stat[0];
    if (stat.scale == FE_SCALE_RELATIVE) {
        percent = clampPercent(static_cast<double>(stat.uvalue) * 100.0 / 65535.0);
        return true;
    }
    if (stat.scale == FE_SCALE_DECIBEL) {
        db = static_cast<double>(stat.svalue) / 1000.0;
        hasDb = true;
        return true;
    }
    return false;
}

FrontendStats readFrontendStats(const DvbSatelliteParams& params) {
    FrontendStats result;
    const int fd = open(frontendPath(params).c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) return result;
    result.available = true;

    fe_status_t status{};
    if (ioctl(fd, FE_READ_STATUS, &status) == 0) result.locked = (status & FE_HAS_LOCK) != 0;

    int signalRelative = -1;
    int qualityRelative = -1;
    readPropertyStat(fd, DTV_STAT_SIGNAL_STRENGTH, signalRelative, result.signalDb, result.hasSignalDb);
    readPropertyStat(fd, DTV_STAT_CNR, qualityRelative, result.cnrDb, result.hasCnrDb);

    if (signalRelative < 0) {
        uint16_t legacy = 0;
        if (ioctl(fd, FE_READ_SIGNAL_STRENGTH, &legacy) == 0) {
            signalRelative = clampPercent(static_cast<double>(legacy) * 100.0 / 65535.0);
        }
    }
    if (qualityRelative < 0) {
        uint16_t legacy = 0;
        if (ioctl(fd, FE_READ_SNR, &legacy) == 0) {
            qualityRelative = clampPercent(static_cast<double>(legacy) * 100.0 / 65535.0);
        }
    }

    if (signalRelative >= 0) result.signalPercent = signalRelative;
    else if (result.hasSignalDb) result.signalPercent = clampPercent((result.signalDb + 100.0) * 1.5);

    if (qualityRelative >= 0) result.qualityPercent = qualityRelative;
    else if (result.hasCnrDb) result.qualityPercent = clampPercent(result.cnrDb * 100.0 / 18.0);

    close(fd);
    return result;
}

Json::Value statsToJson(const FrontendStats& stats) {
    Json::Value root;
    root["available"] = stats.available;
    root["locked"] = stats.locked;
    root["signal"] = stats.signalPercent;
    root["quality"] = stats.qualityPercent;
    if (stats.hasSignalDb) root["signal_db"] = stats.signalDb;
    if (stats.hasCnrDb) root["cnr_db"] = stats.cnrDb;
    return root;
}

std::string decodeDvbText(const uint8_t* data, size_t size) {
    if (!data || size == 0) return {};
    while (size > 0 && *data < 0x20) {
        ++data;
        --size;
    }
    if (!size) return {};
    std::string raw(reinterpret_cast<const char*>(data), size);
    if (g_utf8_validate(raw.data(), static_cast<gssize>(raw.size()), nullptr)) return raw;

    GError* error = nullptr;
    gsize bytesRead = 0;
    gsize bytesWritten = 0;
    gchar* converted = g_convert(raw.data(), raw.size(), "UTF-8", "ISO_6937", &bytesRead, &bytesWritten, &error);
    if (!converted) {
        if (error) g_error_free(error);
        error = nullptr;
        converted = g_convert(raw.data(), raw.size(), "UTF-8", "ISO-8859-1", &bytesRead, &bytesWritten, &error);
    }
    std::string result = converted ? std::string(converted, bytesWritten) : raw;
    if (converted) g_free(converted);
    if (error) g_error_free(error);
    return result;
}

class PsiScanner {
public:
    void feed(const uint8_t* data, size_t size) {
        if (!data || !size) return;
        remainder.insert(remainder.end(), data, data + size);
        size_t offset = 0;
        while (remainder.size() - offset >= 188) {
            if (remainder[offset] != 0x47) {
                const auto next = std::find(remainder.begin() + static_cast<std::ptrdiff_t>(offset + 1), remainder.end(), 0x47);
                if (next == remainder.end()) {
                    remainder.clear();
                    return;
                }
                offset = static_cast<size_t>(std::distance(remainder.begin(), next));
                continue;
            }
            parsePacket(remainder.data() + offset);
            offset += 188;
        }
        if (offset) remainder.erase(remainder.begin(), remainder.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    std::vector<DvbService> services() const {
        std::vector<DvbService> result;
        for (const auto& [sid, pmt] : programMap) {
            DvbService item;
            item.serviceId = sid;
            item.pmtPid = pmt;
            const auto meta = serviceMap.find(sid);
            if (meta != serviceMap.end()) {
                item = meta->second;
                item.serviceId = sid;
                item.pmtPid = pmt;
            }
            if (item.scrambled) {
                for (uint16_t caPid : catCaPids) {
                    if (std::find(item.emmPids.begin(), item.emmPids.end(), caPid) == item.emmPids.end())
                        item.emmPids.push_back(caPid);
                    if (std::find(item.caPids.begin(), item.caPids.end(), caPid) == item.caPids.end())
                        item.caPids.push_back(caPid);
                }
            }
            if (item.name.empty()) item.name = "Service " + std::to_string(sid);
            result.push_back(std::move(item));
        }
        std::sort(result.begin(), result.end(), [](const DvbService& a, const DvbService& b) {
            if (a.name != b.name) return a.name < b.name;
            return a.serviceId < b.serviceId;
        });
        return result;
    }

private:
    struct SectionState {
        std::vector<uint8_t> bytes;
        size_t expected = 0;
    };

    bool isPmtPid(uint16_t pid) const {
        for (const auto& [sid, pmtPid] : programMap) {
            (void)sid;
            if (pmtPid == pid) return true;
        }
        return false;
    }

    void markPacketScrambled(uint16_t pid) {
        for (auto& [sid, service] : serviceMap) {
            (void)sid;
            if (service.pcrPid == pid ||
                std::find(service.streamPids.begin(), service.streamPids.end(), pid) != service.streamPids.end()) {
                service.scrambled = true;
            }
        }
    }

    void parsePacket(const uint8_t* packet) {
        if (packet[0] != 0x47 || (packet[1] & 0x80)) return;
        const uint16_t pid = static_cast<uint16_t>(((packet[1] & 0x1F) << 8) | packet[2]);
        const uint8_t scrambling = static_cast<uint8_t>((packet[3] >> 6) & 0x03);
        if (scrambling != 0) markPacketScrambled(pid);

        if (pid != 0x0000 && pid != 0x0001 && pid != 0x0011 && !isPmtPid(pid)) return;
        const bool payloadStart = (packet[1] & 0x40) != 0;
        const uint8_t adaptation = static_cast<uint8_t>((packet[3] >> 4) & 0x03);
        if (adaptation == 0 || adaptation == 2) return;
        size_t offset = 4;
        if (adaptation == 3) {
            offset += static_cast<size_t>(packet[4]) + 1;
            if (offset >= 188) return;
        }
        feedSection(pid, packet + offset, 188 - offset, payloadStart);
    }

    void feedSection(uint16_t pid, const uint8_t* payload, size_t size, bool payloadStart) {
        if (!payload || !size) return;
        SectionState& state = sections[pid];
        size_t offset = 0;
        if (payloadStart) {
            const size_t pointer = payload[0];
            offset = 1;
            if (!state.bytes.empty() && pointer && offset + pointer <= size) {
                appendSection(pid, state, payload + offset, pointer);
            }
            offset += pointer;
            state.bytes.clear();
            state.expected = 0;
        }

        while (offset < size) {
            if (!state.bytes.empty()) {
                if (state.expected == 0) {
                    const size_t headerNeeded = state.bytes.size() < 3 ? 3 - state.bytes.size() : 0;
                    const size_t takeHeader = std::min(headerNeeded, size - offset);
                    state.bytes.insert(state.bytes.end(), payload + offset, payload + offset + takeHeader);
                    offset += takeHeader;
                    if (state.bytes.size() < 3) return;
                    const size_t sectionLength = static_cast<size_t>(((state.bytes[1] & 0x0F) << 8) | state.bytes[2]);
                    state.expected = 3 + sectionLength;
                    if (state.expected < 8 || state.expected > 4096) {
                        state.bytes.clear();
                        state.expected = 0;
                        return;
                    }
                }
                const size_t needed = state.expected > state.bytes.size() ? state.expected - state.bytes.size() : 0;
                const size_t take = std::min(needed, size - offset);
                state.bytes.insert(state.bytes.end(), payload + offset, payload + offset + take);
                offset += take;
                if (state.expected && state.bytes.size() >= state.expected) {
                    parseSection(pid, state.bytes.data(), state.expected);
                    state.bytes.clear();
                    state.expected = 0;
                }
                continue;
            }
            if (payload[offset] == 0xFF) return;
            if (size - offset < 3) {
                state.bytes.assign(payload + offset, payload + size);
                state.expected = 0;
                return;
            }
            const size_t sectionLength = static_cast<size_t>(((payload[offset + 1] & 0x0F) << 8) | payload[offset + 2]);
            const size_t total = 3 + sectionLength;
            if (total < 8 || total > 4096) return;
            if (size - offset >= total) {
                parseSection(pid, payload + offset, total);
                offset += total;
            } else {
                state.bytes.assign(payload + offset, payload + size);
                state.expected = total;
                return;
            }
        }
    }

    void appendSection(uint16_t pid, SectionState& state, const uint8_t* data, size_t size) {
        if (!state.bytes.empty() && state.expected) {
            const size_t take = std::min(size, state.expected - state.bytes.size());
            state.bytes.insert(state.bytes.end(), data, data + take);
            if (state.bytes.size() >= state.expected) parseSection(pid, state.bytes.data(), state.expected);
        }
        state.bytes.clear();
        state.expected = 0;
    }

    void parseSection(uint16_t pid, const uint8_t* section, size_t size) {
        if (pid == 0x0000) parsePat(section, size);
        else if (pid == 0x0001) parseCat(section, size);
        else if (pid == 0x0011) parseSdt(section, size);
        else if (isPmtPid(pid)) parsePmt(section, size);
    }

    void parsePat(const uint8_t* section, size_t size) {
        if (size < 12 || section[0] != 0x00) return;
        const size_t end = size >= 4 ? size - 4 : 0;
        for (size_t pos = 8; pos + 4 <= end; pos += 4) {
            const uint16_t program = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
            const uint16_t pid = static_cast<uint16_t>(((section[pos + 2] & 0x1F) << 8) | section[pos + 3]);
            if (program != 0) {
                programMap[program] = pid;
                DvbService& service = serviceMap[program];
                service.serviceId = program;
                service.pmtPid = pid;
            }
        }
    }

    static bool collectCaDescriptors(const uint8_t* data, size_t size, std::vector<uint16_t>* caPids) {
        bool found = false;
        size_t pos = 0;
        while (pos + 2 <= size) {
            const uint8_t tag = data[pos];
            const size_t length = data[pos + 1];
            if (pos + 2 + length > size) break;
            if (tag == 0x09) { // conditional_access_descriptor
                found = true;
                if (caPids && length >= 4) {
                    const uint8_t* d = data + pos + 2;
                    const uint16_t caPid = static_cast<uint16_t>(((d[2] & 0x1F) << 8) | d[3]);
                    if (caPid < 0x1FFF &&
                        std::find(caPids->begin(), caPids->end(), caPid) == caPids->end()) {
                        caPids->push_back(caPid);
                    }
                }
            }
            pos += 2 + length;
        }
        return found;
    }

    void parseCat(const uint8_t* section, size_t size) {
        if (size < 12 || section[0] != 0x01) return;
        const size_t end = size >= 4 ? size - 4 : 0;
        if (end <= 8) return;
        std::vector<uint16_t> found;
        collectCaDescriptors(section + 8, end - 8, &found);
        for (uint16_t pid : found) catCaPids.insert(pid);
    }

    void parsePmt(const uint8_t* section, size_t size) {
        if (size < 16 || section[0] != 0x02) return;
        const uint16_t sid = static_cast<uint16_t>((section[3] << 8) | section[4]);
        const size_t end = size >= 4 ? size - 4 : 0;
        if (end <= 12) return;

        DvbService& service = serviceMap[sid];
        service.serviceId = sid;
        if (const auto it = programMap.find(sid); it != programMap.end()) service.pmtPid = it->second;
        service.pcrPid = static_cast<uint16_t>(((section[8] & 0x1F) << 8) | section[9]);

        const size_t programInfoLength = static_cast<size_t>(((section[10] & 0x0F) << 8) | section[11]);
        size_t pos = 12;
        if (pos + programInfoLength > end) return;
        std::vector<uint16_t> programCaPids;
        if (collectCaDescriptors(section + pos, programInfoLength, &programCaPids)) service.scrambled = true;
        for (uint16_t caPid : programCaPids) {
            if (std::find(service.ecmPids.begin(), service.ecmPids.end(), caPid) == service.ecmPids.end()) service.ecmPids.push_back(caPid);
            if (std::find(service.caPids.begin(), service.caPids.end(), caPid) == service.caPids.end()) service.caPids.push_back(caPid);
        }
        pos += programInfoLength;

        std::vector<uint16_t> streamPids;
        while (pos + 5 <= end) {
            const uint16_t elementaryPid = static_cast<uint16_t>(((section[pos + 1] & 0x1F) << 8) | section[pos + 2]);
            const size_t esInfoLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
            pos += 5;
            if (pos + esInfoLength > end) break;
            std::vector<uint16_t> esCaPids;
            if (collectCaDescriptors(section + pos, esInfoLength, &esCaPids)) service.scrambled = true;
            for (uint16_t caPid : esCaPids) {
                if (std::find(service.ecmPids.begin(), service.ecmPids.end(), caPid) == service.ecmPids.end()) service.ecmPids.push_back(caPid);
                if (std::find(service.caPids.begin(), service.caPids.end(), caPid) == service.caPids.end()) service.caPids.push_back(caPid);
            }
            if (elementaryPid != 0x1FFF &&
                std::find(streamPids.begin(), streamPids.end(), elementaryPid) == streamPids.end()) {
                streamPids.push_back(elementaryPid);
            }
            pos += esInfoLength;
        }
        if (!streamPids.empty()) service.streamPids = std::move(streamPids);
        service.pmtParsed = true;
    }

    void parseSdt(const uint8_t* section, size_t size) {
        if (size < 15 || (section[0] != 0x42 && section[0] != 0x46)) return;
        const size_t end = size >= 4 ? size - 4 : 0;
        size_t pos = 11;
        while (pos + 5 <= end) {
            const uint16_t sid = static_cast<uint16_t>((section[pos] << 8) | section[pos + 1]);
            DvbService& service = serviceMap[sid];
            service.serviceId = sid;
            if ((section[pos + 3] & 0x10) != 0) service.scrambled = true; // free_CA_mode
            const size_t descriptorLength = static_cast<size_t>(((section[pos + 3] & 0x0F) << 8) | section[pos + 4]);
            pos += 5;
            const size_t descriptorEnd = std::min(end, pos + descriptorLength);
            while (pos + 2 <= descriptorEnd) {
                const uint8_t tag = section[pos];
                const size_t length = section[pos + 1];
                if (pos + 2 + length > descriptorEnd) break;
                if (tag == 0x48 && length >= 3) {
                    const uint8_t* descriptor = section + pos + 2;
                    service.serviceType = descriptor[0];
                    size_t inner = 1;
                    if (inner < length) {
                        const size_t providerLength = descriptor[inner++];
                        if (inner + providerLength <= length) {
                            service.provider = decodeDvbText(descriptor + inner, providerLength);
                            inner += providerLength;
                        }
                    }
                    if (inner < length) {
                        const size_t nameLength = descriptor[inner++];
                        if (inner + nameLength <= length) {
                            service.name = decodeDvbText(descriptor + inner, nameLength);
                        }
                    }
                } else if (tag == 0x09) {
                    service.scrambled = true;
                }
                pos += 2 + length;
            }
            pos = descriptorEnd;
        }
    }

    std::vector<uint8_t> remainder;
    std::map<uint16_t, SectionState> sections;
    std::map<uint16_t, uint16_t> programMap;
    std::map<uint16_t, DvbService> serviceMap;
    std::set<uint16_t> catCaPids;
};

std::string servicePidsString(const DvbService& service) {
    // Never build a PSI-only service filter. If PMT/ES data has not arrived,
    // keep the full transport stream so startup can retry PID discovery.
    if (!service.pmtParsed || service.streamPids.empty()) return {};

    std::set<uint16_t> pids = {0x0000, 0x0001, 0x0010, 0x0011, 0x0012, 0x0014};
    if (service.pmtPid > 0 && service.pmtPid < 0x1FFF) pids.insert(service.pmtPid);
    if (service.pcrPid < 0x1FFF) pids.insert(service.pcrPid);
    for (uint16_t pid : service.streamPids) {
        if (pid < 0x1FFF) pids.insert(pid);
    }
    for (uint16_t pid : service.caPids) {
        if (pid < 0x1FFF) pids.insert(pid);
    }
    std::ostringstream out;
    bool first = true;
    for (uint16_t pid : pids) {
        if (!first) out << ':';
        first = false;
        out << pid;
    }
    return out.str();
}

bool waitForTune(GstElement* pipeline, GstElement* sink, const DvbSatelliteParams& params,
                 int timeoutMs, PsiScanner* scanner, FrontendStats& bestStats, std::string& error) {
    GstBus* bus = gst_element_get_bus(pipeline);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    auto lastStatsRead = std::chrono::steady_clock::time_point{};
    bool receivedData = false;

    while (std::chrono::steady_clock::now() < deadline) {
        GstMessage* message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
        if (message) {
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                GError* gstError = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &gstError, &debug);
                error = gstError ? gstError->message : "DVB tuning failed";
                if (gstError) g_error_free(gstError);
                if (debug) g_free(debug);
                gst_message_unref(message);
                gst_object_unref(bus);
                return false;
            }
            gst_message_unref(message);
        }

        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 150 * GST_MSECOND);
        if (sample) {
            receivedData = true;
            if (scanner) {
                GstBuffer* buffer = gst_sample_get_buffer(sample);
                GstMapInfo map{};
                if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                    scanner->feed(map.data, map.size);
                    gst_buffer_unmap(buffer, &map);
                }
            }
            gst_sample_unref(sample);
        }

        const auto now = std::chrono::steady_clock::now();
        if (lastStatsRead.time_since_epoch().count() == 0 || now - lastStatsRead >= std::chrono::milliseconds(350)) {
            FrontendStats stats = readFrontendStats(params);
            bestStats.available = bestStats.available || stats.available;
            if (stats.signalPercent >= bestStats.signalPercent) bestStats.signalPercent = stats.signalPercent;
            if (stats.qualityPercent >= bestStats.qualityPercent) bestStats.qualityPercent = stats.qualityPercent;
            bestStats.locked = bestStats.locked || stats.locked;
            if (stats.hasSignalDb) bestStats.signalDb = stats.signalDb, bestStats.hasSignalDb = true;
            if (stats.hasCnrDb) bestStats.cnrDb = stats.cnrDb, bestStats.hasCnrDb = true;
            lastStatsRead = now;
        }
    }
    gst_object_unref(bus);
    return receivedData || bestStats.locked;
}

std::mutex& frontendTuneMutex(const DvbSatelliteParams& params) {
    // The web scan modal polls /api/dvb-signal and /api/dvb-scan can be
    // requested from the same or another browser.  Serialize tune pipelines
    // per physical frontend so a signal probe cannot overlap the scan/open
    // sequence and transiently produce EBUSY.
    static std::array<std::mutex, 32 * 32> locks;
    const int adapter = std::clamp(params.adapter, 0, 31);
    const int frontend = std::clamp(params.frontend, 0, 31);
    return locks[static_cast<std::size_t>(adapter * 32 + frontend)];
}

std::string takePipelineError(GstElement* pipeline, GstClockTime timeout) {
    if (!pipeline) return {};
    GstBus* bus = gst_element_get_bus(pipeline);
    if (!bus) return {};
    GstMessage* message = gst_bus_timed_pop_filtered(bus, timeout, GST_MESSAGE_ERROR);
    if (!message) {
        gst_object_unref(bus);
        return {};
    }

    GError* gstError = nullptr;
    gchar* debug = nullptr;
    gst_message_parse_error(message, &gstError, &debug);
    std::string error = gstError && gstError->message
        ? std::string(gstError->message)
        : std::string("DVB frontend could not start tuning");
    if (gstError) g_error_free(gstError);
    if (debug) g_free(debug);
    gst_message_unref(message);
    gst_object_unref(bus);
    return error;
}

void resetTunePipeline(GstElement* pipeline) {
    if (!pipeline) return;
    gst_element_set_state(pipeline, GST_STATE_NULL);
    // Wait for dvbsrc to actually close /dev/dvb before another attempt.
    gst_element_get_state(pipeline, nullptr, nullptr, GST_SECOND);
}

Json::Value runTune(const DvbSatelliteParams& params, bool collectServices, int timeoutMs) {
    Json::Value result;
    const std::string device = frontendPath(params);
    result["adapter"] = params.adapter;
    result["frontend"] = params.frontend;
    result["device"] = device;
    std::cerr << "DVB " << (collectServices ? "scan" : "signal")
              << " tune request: adapter=" << params.adapter
              << " frontend=" << params.frontend
              << " device=" << device
              << " frequency_khz=" << params.frequencyKHz
              << " symbol_rate=" << params.symbolRateK
              << " polarity=" << params.polarity
              << std::endl;
    if (!std::filesystem::exists(device)) {
        result["error"] = "DVB frontend not found: " + device;
        return result;
    }

    GstElement* pipeline = gst_pipeline_new("satellite_scan");
    GstElement* source = gst_element_factory_make("dvbsrc", "satellite_source");
    GstElement* queue = gst_element_factory_make("queue", "satellite_scan_queue");
    GstElement* sink = gst_element_factory_make("appsink", "satellite_scan_sink");
    if (!pipeline || !source || !queue || !sink) {
        result["error"] = "GStreamer DVB plugin is unavailable (dvbsrc/appsink)";
        if (pipeline) gst_object_unref(pipeline);
        else {
            if (source) gst_object_unref(source);
            if (queue) gst_object_unref(queue);
            if (sink) gst_object_unref(sink);
        }
        return result;
    }

    std::string configureError;
    if (!DvbSatellite::configureSource(source, params, configureError)) {
        result["error"] = configureError;
        gst_object_unref(pipeline);
        gst_object_unref(source);
        gst_object_unref(queue);
        gst_object_unref(sink);
        return result;
    }

    g_object_set(sink,
        "sync", FALSE,
        "emit-signals", FALSE,
        "max-buffers", 64U,
        "drop", TRUE,
        nullptr);
    g_object_set(queue,
        "max-size-buffers", 0U,
        "max-size-bytes", 0U,
        "max-size-time", static_cast<guint64>(2 * GST_SECOND),
        nullptr);

    gst_bin_add_many(GST_BIN(pipeline), source, queue, sink, nullptr);
    if (!gst_element_link_many(source, queue, sink, nullptr)) {
        result["error"] = "Failed to build DVB scan pipeline";
        gst_object_unref(pipeline);
        return result;
    }

    std::unique_lock<std::mutex> frontendLock(frontendTuneMutex(params));

    PsiScanner scanner;
    FrontendStats stats;
    std::string tuneError;
    bool tuned = false;
    constexpr int kTuneStartupAttempts = 3;
    constexpr auto kTuneRetryDelay = std::chrono::milliseconds(700);

    for (int attempt = 1; attempt <= kTuneStartupAttempts; ++attempt) {
        PsiScanner attemptScanner;
        FrontendStats attemptStats;
        std::string attemptError;

        const GstStateChangeReturn stateResult =
            gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (stateResult == GST_STATE_CHANGE_FAILURE) {
            attemptError = takePipelineError(pipeline, 250 * GST_MSECOND);
            if (attemptError.empty()) attemptError = "DVB frontend could not start tuning";
        } else {
            tuned = waitForTune(
                pipeline, sink, params, timeoutMs,
                collectServices ? &attemptScanner : nullptr,
                attemptStats, attemptError);
        }

        scanner = std::move(attemptScanner);
        stats = attemptStats;
        tuneError = attemptError;

        // A normal no-lock/no-data result is not an open/start failure. Do not
        // triple the scan timeout in that case. Retry only GStreamer/frontend
        // startup failures (notably the transient EBUSY observed right after
        // a previous DVB pipeline has been stopped).
        if (tuned || tuneError.empty()) break;

        resetTunePipeline(pipeline);
        if (attempt < kTuneStartupAttempts) {
            std::cerr << "DVB " << (collectServices ? "scan" : "signal")
                      << " startup retry: adapter=" << params.adapter
                      << " frontend=" << params.frontend
                      << " attempt=" << attempt
                      << " error=" << tuneError << std::endl;
            std::this_thread::sleep_for(kTuneRetryDelay);
            tuneError.clear();
            continue;
        }
    }

    resetTunePipeline(pipeline);
    gst_object_unref(pipeline);

    result["locked"] = stats.locked;
    result["signal"] = stats.signalPercent;
    result["quality"] = stats.qualityPercent;
    if (stats.hasSignalDb) result["signal_db"] = stats.signalDb;
    if (stats.hasCnrDb) result["cnr_db"] = stats.cnrDb;
    if (!tuneError.empty()) result["error"] = tuneError;
    else if (!tuned) result["error"] = "No satellite signal / frontend lock";

    if (collectServices) {
        Json::Value services(Json::arrayValue);
        for (const auto& service : scanner.services()) {
            Json::Value item;
            item["service_id"] = service.serviceId;
            item["pmt_pid"] = service.pmtPid;
            item["pcr_pid"] = service.pcrPid < 0x1FFF ? service.pcrPid : 0;
            item["service_type"] = service.serviceType;
            item["name"] = service.name;
            item["provider"] = service.provider;
            item["scrambled"] = service.scrambled;
            const bool pmtReady = service.pmtParsed && !service.streamPids.empty();
            item["pmt_ready"] = pmtReady;
            item["access"] = service.scrambled ? "CA" : (pmtReady ? "FTA" : "UNKNOWN");
            Json::Value streamPids(Json::arrayValue);
            for (uint16_t pid : service.streamPids) streamPids.append(pid);
            item["stream_pids"] = streamPids;
            Json::Value caPids(Json::arrayValue);
            for (uint16_t pid : service.caPids) caPids.append(pid);
            item["ca_pids"] = caPids;
            Json::Value ecmPids(Json::arrayValue);
            for (uint16_t pid : service.ecmPids) ecmPids.append(pid);
            item["ecm_pids"] = ecmPids;
            Json::Value emmPids(Json::arrayValue);
            for (uint16_t pid : service.emmPids) emmPids.append(pid);
            item["emm_pids"] = emmPids;
            item["emm_receive_enabled"] = service.scrambled && !service.emmPids.empty();
            const std::string pids = servicePidsString(service);
            item["service_pids"] = pids;
            DvbSatelliteParams serviceParams = params;
            serviceParams.pids = pids.empty() ? "8192" : pids;
            item["input_uri"] = DvbSatellite::buildUri(serviceParams);
            services.append(item);
        }
        result["services"] = services;
        result["service_count"] = services.size();
    }
    return result;
}

} // namespace

namespace DvbSatellite {

bool isDvbUri(const std::string& uri) {
    return lower(uri).rfind("dvb://", 0) == 0;
}

bool parseUri(const std::string& uri, DvbSatelliteParams& params, std::string& error) {
    if (!isDvbUri(uri)) {
        error = "Not a DVB URI";
        return false;
    }
    const auto query = parseQuery(uri);
    if (!parseInt(query, "adapter", params.adapter) ||
        !parseInt(query, "frontend", params.frontend) ||
        !parseUInt(query, "frequency_khz", params.frequencyKHz) ||
        !parseUInt(query, "symbol_rate", params.symbolRateK) ||
        !parseInt(query, "diseqc_source", params.diseqcSource) ||
        !parseUInt(query, "lnb_lof1_khz", params.lnbLof1KHz) ||
        !parseUInt(query, "lnb_lof2_khz", params.lnbLof2KHz) ||
        !parseUInt(query, "lnb_slof_khz", params.lnbSlofKHz) ||
        !parseInt(query, "stream_id", params.streamId)) {
        error = "Invalid DVB URI numeric parameter";
        return false;
    }
    if (const auto it = query.find("polarity"); it != query.end()) params.polarity = upper(it->second);
    if (const auto it = query.find("delivery_system"); it != query.end()) params.deliverySystem = lower(it->second);
    if (const auto it = query.find("modulation"); it != query.end()) params.modulation = lower(it->second);
    if (const auto it = query.find("fec"); it != query.end()) params.fec = lower(it->second);
    if (const auto it = query.find("pids"); it != query.end() && !it->second.empty()) params.pids = it->second;

    if (params.frequencyKHz < 900000 || params.frequencyKHz > 14000000 ||
        params.symbolRateK < 100 || params.symbolRateK > 60000 ||
        (params.polarity != "H" && params.polarity != "V") ||
        (params.deliverySystem != "dvb-s" && params.deliverySystem != "dvb-s2")) {
        error = "Invalid DVB-S/S2 tuning parameters";
        return false;
    }
    return true;
}

std::string buildUri(const DvbSatelliteParams& params) {
    std::ostringstream uri;
    uri << "dvb://satellite?adapter=" << params.adapter
        << "&frontend=" << params.frontend
        << "&frequency_khz=" << params.frequencyKHz
        << "&symbol_rate=" << params.symbolRateK
        << "&polarity=" << percentEncode(params.polarity)
        << "&delivery_system=" << percentEncode(params.deliverySystem)
        << "&modulation=" << percentEncode(params.modulation)
        << "&fec=" << percentEncode(params.fec)
        << "&diseqc_source=" << params.diseqcSource
        << "&lnb_lof1_khz=" << params.lnbLof1KHz
        << "&lnb_lof2_khz=" << params.lnbLof2KHz
        << "&lnb_slof_khz=" << params.lnbSlofKHz
        << "&stream_id=" << params.streamId
        << "&pids=" << percentEncode(params.pids.empty() ? "8192" : params.pids);
    return uri.str();
}

bool configureSource(GstElement* source, const DvbSatelliteParams& params, std::string& error) {
    if (!source) {
        error = "DVB source is null";
        return false;
    }
    const std::string factoryName = GST_OBJECT_NAME(source) ? GST_OBJECT_NAME(source) : "dvbsrc";
    if (!g_object_class_find_property(G_OBJECT_GET_CLASS(source), "frequency")) {
        error = factoryName + ": incompatible dvbsrc plugin";
        return false;
    }

    g_object_set(source,
        "adapter", params.adapter,
        "frontend", params.frontend,
        "frequency", params.frequencyKHz,
        "symbol-rate", params.symbolRateK,
        "polarity", params.polarity.c_str(),
        "diseqc-source", params.diseqcSource,
        "lnb-lof1", params.lnbLof1KHz,
        "lnb-lof2", params.lnbLof2KHz,
        "lnb-slof", params.lnbSlofKHz,
        "pids", (params.pids.empty() ? "8192" : params.pids.c_str()),
        "stats-reporting-interval", 20U,
        "tuning-timeout", static_cast<guint64>(5000000),
        nullptr);

    gint configuredAdapter = -1;
    gint configuredFrontend = -1;
    g_object_get(source,
        "adapter", &configuredAdapter,
        "frontend", &configuredFrontend,
        nullptr);
    if (configuredAdapter != params.adapter || configuredFrontend != params.frontend) {
        std::ostringstream mismatch;
        mismatch << "dvbsrc adapter/frontend mismatch: requested "
                 << params.adapter << ':' << params.frontend
                 << " but element reports "
                 << configuredAdapter << ':' << configuredFrontend;
        error = mismatch.str();
        return false;
    }
    gst_util_set_object_arg(G_OBJECT(source), "delsys", params.deliverySystem.c_str());
    gst_util_set_object_arg(G_OBJECT(source), "modulation", params.modulation.c_str());
    gst_util_set_object_arg(G_OBJECT(source), "code-rate-hp", params.fec.c_str());
    gst_util_set_object_arg(G_OBJECT(source), "pilot", "auto");
    gst_util_set_object_arg(G_OBJECT(source), "rolloff", "auto");
    if (params.streamId >= 0 && g_object_class_find_property(G_OBJECT_GET_CLASS(source), "stream-id")) {
        g_object_set(source, "stream-id", params.streamId, nullptr);
    }
    return true;
}

Json::Value adapters() {
    Json::Value root;
    Json::Value list(Json::arrayValue);
    const std::filesystem::path base("/dev/dvb");
    std::error_code ec;
    if (std::filesystem::exists(base, ec) && !ec) {
        for (const auto& adapterEntry : std::filesystem::directory_iterator(base, ec)) {
            if (ec || !adapterEntry.is_directory()) continue;
            const std::string adapterName = adapterEntry.path().filename().string();
            if (adapterName.rfind("adapter", 0) != 0) continue;
            int adapter = -1;
            try { adapter = std::stoi(adapterName.substr(7)); } catch (...) { continue; }
            for (const auto& frontendEntry : std::filesystem::directory_iterator(adapterEntry.path(), ec)) {
                if (ec) break;
                const std::string frontendName = frontendEntry.path().filename().string();
                if (frontendName.rfind("frontend", 0) != 0) continue;
                int frontend = -1;
                try { frontend = std::stoi(frontendName.substr(8)); } catch (...) { continue; }
                Json::Value item;
                item["adapter"] = adapter;
                item["frontend"] = frontend;
                item["device"] = frontendEntry.path().string();
                list.append(item);
            }
        }
    }
    root["adapters"] = list;
    root["available"] = !list.empty();
    GstElementFactory* dvbFactory = gst_element_factory_find("dvbsrc");
    root["dvbsrc_available"] = dvbFactory != nullptr;
    if (dvbFactory) gst_object_unref(dvbFactory);
    return root;
}

Json::Value scan(const Json::Value& request) {
    std::string error;
    const DvbSatelliteParams params = paramsFromJson(request, error);
    if (!error.empty()) {
        Json::Value result;
        result["error"] = error;
        return result;
    }
    const bool holdLock = request.get("hold_lock", false).asBool();
    const int timeoutMs = holdLock ? 18000 : 6500;
    Json::Value result = runTune(params, true, timeoutMs);
    result["hold_lock"] = holdLock;
    result["scan_timeout_ms"] = timeoutMs;
    result["input_uri"] = buildUri(params);
    result["frequency_mhz"] = static_cast<double>(params.frequencyKHz) / 1000.0;
    result["symbol_rate"] = params.symbolRateK;
    result["polarity"] = params.polarity;
    result["delivery_system"] = params.deliverySystem;
    result["modulation"] = params.modulation;
    return result;
}

Json::Value signal(const Json::Value& request) {
    std::string error;
    const DvbSatelliteParams params = paramsFromJson(request, error);
    if (!error.empty()) {
        Json::Value result;
        result["error"] = error;
        return result;
    }
    return runTune(params, false, 900);
}

Json::Value signalFromUri(const std::string& uri) {
    Json::Value result;
    DvbSatelliteParams params;
    std::string error;
    if (!parseUri(uri, params, error)) {
        result["error"] = error.empty() ? "Invalid DVB URI" : error;
        result["locked"] = false;
        result["signal"] = 0;
        result["quality"] = 0;
        return result;
    }

    // Important: this path does NOT instantiate dvbsrc and does NOT issue a
    // tune command. It only reads FE status/statistics from the frontend that
    // the live stream pipeline already owns. This prevents the dashboard from
    // retuning or interrupting a channel while refreshing Signal/Quality.
    const FrontendStats stats = readFrontendStats(params);
    result = statsToJson(stats);
    result["adapter"] = params.adapter;
    result["frontend"] = params.frontend;
    return result;
}

bool resolveServicePids(const DvbSatelliteParams& params, uint32_t serviceId,
                        std::string& pids, bool& scrambled, std::string& error) {
    pids.clear();
    scrambled = false;
    if (serviceId == 0 || serviceId > 0xFFFF) {
        error = "Invalid DVB service id";
        return false;
    }

    DvbSatelliteParams scanParams = params;
    scanParams.pids = "8192";
    Json::Value result = runTune(scanParams, true, 3200);
    if (result.isMember("services") && result["services"].isArray()) {
        for (const auto& item : result["services"]) {
            if (item.get("service_id", 0).asUInt() != serviceId) continue;
            pids = item.get("service_pids", "").asString();
            scrambled = item.get("scrambled", false).asBool();
            if (!pids.empty()) {
                error.clear();
                return true;
            }
        }
    }
    error = result.get("error", "").asString();
    if (error.empty()) error = "DVB service PMT/PIDs not found for SID " + std::to_string(serviceId);
    return false;
}

} // namespace DvbSatellite
