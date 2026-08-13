#include "PhoenixManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <unistd.h>

namespace PhoenixManager {
namespace {

std::string trim(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string upper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) return {};
    std::ostringstream out;
    out << input.rdbuf();
    return trim(out.str());
}

std::string parentAttribute(std::filesystem::path path, const char* name) {
    std::error_code ec;
    for (int i = 0; i < 8 && !path.empty(); ++i) {
        const auto candidate = path / name;
        if (std::filesystem::exists(candidate, ec) && !ec) {
            const std::string value = readTextFile(candidate);
            if (!value.empty()) return value;
        }
        path = path.parent_path();
    }
    return {};
}

std::string ttyDriver(const std::string& ttyName) {
    std::error_code ec;
    const auto driver = std::filesystem::path("/sys/class/tty") / ttyName / "device/driver";
    if (!std::filesystem::exists(driver, ec) || ec) return {};
    const auto target = std::filesystem::read_symlink(driver, ec);
    return ec ? std::string{} : target.filename().string();
}

bool ttyLooksLikeReader(const std::string& ttyName, const std::string& driver,
                        const std::string& manufacturer, const std::string& product) {
    if (ttyName.rfind("ttyUSB", 0) != 0 && ttyName.rfind("ttyACM", 0) != 0) return false;
    const std::string identity = lower(driver + " " + manufacturer + " " + product);
    if (identity.find("phoenix") != std::string::npos ||
        identity.find("smartmouse") != std::string::npos ||
        identity.find("smart card") != std::string::npos ||
        identity.find("smartcard") != std::string::npos) return true;
    return driver == "ftdi_sio" || driver == "pl2303" || driver == "cp210x" ||
           driver == "ch341" || driver == "usbserial" || ttyName.rfind("ttyUSB", 0) == 0;
}

std::string canonicalDevice(const std::filesystem::path& path) {
    std::error_code ec;
    auto resolved = std::filesystem::canonical(path, ec);
    return ec ? path.string() : resolved.string();
}

bool usedByAnotherProcess(const std::string& canonicalPath) {
    std::error_code ec;
    const pid_t self = ::getpid();
    const std::filesystem::path proc("/proc");
    for (const auto& process : std::filesystem::directory_iterator(proc, ec)) {
        if (ec || !process.is_directory(ec)) continue;
        const std::string pidText = process.path().filename().string();
        if (pidText.empty() || !std::all_of(pidText.begin(), pidText.end(), ::isdigit)) continue;
        pid_t pid = 0;
        try { pid = static_cast<pid_t>(std::stol(pidText)); } catch (...) { continue; }
        if (pid == self) continue;
        const auto fdDir = process.path() / "fd";
        if (!std::filesystem::exists(fdDir, ec) || ec) { ec.clear(); continue; }
        for (const auto& fd : std::filesystem::directory_iterator(fdDir, ec)) {
            if (ec) { ec.clear(); break; }
            const auto link = std::filesystem::read_symlink(fd.path(), ec);
            if (ec) { ec.clear(); continue; }
            std::string target = link.string();
            if (target == canonicalPath) return true;
        }
    }
    return false;
}

std::string hexBytes(const std::vector<unsigned char>& bytes) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(bytes.size() * 3);
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) out.push_back(' ');
        out.push_back(hex[(bytes[i] >> 4) & 0x0F]);
        out.push_back(hex[bytes[i] & 0x0F]);
    }
    return out;
}

std::vector<unsigned char> historicalBytes(const std::vector<unsigned char>& atr) {
    if (atr.size() < 2 || (atr[0] != 0x3B && atr[0] != 0x3F)) return {};
    size_t pos = 2;
    unsigned y = (atr[1] >> 4) & 0x0F;
    const size_t k = atr[1] & 0x0F;
    bool more = true;
    while (more && pos < atr.size()) {
        if (y & 0x1) { if (pos >= atr.size()) return {}; ++pos; }
        if (y & 0x2) { if (pos >= atr.size()) return {}; ++pos; }
        if (y & 0x4) { if (pos >= atr.size()) return {}; ++pos; }
        if (y & 0x8) {
            if (pos >= atr.size()) return {};
            y = (atr[pos] >> 4) & 0x0F;
            ++pos;
        } else {
            more = false;
        }
    }
    if (pos >= atr.size()) return {};
    const size_t available = std::min(k, atr.size() - pos);
    return std::vector<unsigned char>(atr.begin() + static_cast<std::ptrdiff_t>(pos),
                                      atr.begin() + static_cast<std::ptrdiff_t>(pos + available));
}

std::string printableHistorical(const std::vector<unsigned char>& atr) {
    const auto historical = historicalBytes(atr);
    std::string result;
    for (unsigned char byte : historical) {
        if (byte >= 32 && byte <= 126) result.push_back(static_cast<char>(byte));
        else if (!result.empty() && result.back() != ' ') result.push_back(' ');
    }
    return trim(result);
}

void identifyCard(const std::vector<unsigned char>& atr, std::string& system, std::string& provider) {
    const std::string text = upper(printableHistorical(atr));
    const std::string all = upper(hexBytes(atr) + " " + text);

    if (text.find("IRDETO") != std::string::npos) system = "Irdeto";
    else if (text.find("VIACCESS") != std::string::npos) system = "Viaccess";
    else if (text.find("NAGRA") != std::string::npos) system = "Nagravision";
    else if (text.find("CONAX") != std::string::npos) system = "Conax";
    else if (text.find("SECA") != std::string::npos || text.find("MEDIAGUARD") != std::string::npos) system = "Mediaguard/SECA";
    else if (text.find("DRE") != std::string::npos || text.find("EXSET") != std::string::npos) system = "DRE-Crypt/Exset";
    else if (text.find("VIDEOGUARD") != std::string::npos || text.find("NDS") != std::string::npos) system = "VideoGuard/NDS";

    if (text.find("TRICOLOR") != std::string::npos || text.find("TRICOLOR TV") != std::string::npos) provider = "Триколор";
    else if (text.find("NTV+") != std::string::npos || text.find("NTV PLUS") != std::string::npos || text.find("NTVPLUS") != std::string::npos) provider = "НТВ-ПЛЮС";
    else if (text.find("CANAL+") != std::string::npos || text.find("CANAL PLUS") != std::string::npos) provider = "Canal+";
    else if (text.find("ORANGE") != std::string::npos) provider = "Orange";
    else if (text.find("HD+") != std::string::npos) provider = "HD+";
    else if (text.find("SKY") != std::string::npos) provider = "Sky";
    (void)all;
}

struct ProbeResult {
    std::string status = "unknown";
    std::vector<unsigned char> atr;
    std::string cardSystem;
    std::string provider;
    std::string detail;
};

bool readAtrFor(std::chrono::milliseconds timeout, int fd, std::vector<unsigned char>& atr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<unsigned char> buffer;
    buffer.reserve(64);
    while (std::chrono::steady_clock::now() < deadline && buffer.size() < 64) {
        unsigned char chunk[64];
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got > 0) {
            buffer.insert(buffer.end(), chunk, chunk + got);
            auto start = std::find_if(buffer.begin(), buffer.end(), [](unsigned char b) { return b == 0x3B || b == 0x3F; });
            if (start != buffer.end()) {
                atr.assign(start, buffer.end());
                if (atr.size() >= 2) {
                    const size_t minLikely = 2 + (atr[1] & 0x0F);
                    if (atr.size() >= minLikely) return true;
                }
            }
        } else if (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    return !atr.empty();
}

struct ProbeProfile {
    unsigned baud = 9600;
    std::string label;
};

std::vector<ProbeProfile> probeProfiles(const std::string& serial) {
    // The two installed FTDI Phoenix readers are configured in the current
    // deployment at 6.00 MHz.  ISO-7816 initial Fi=372 therefore gives
    // approximately 6000000 / 372 = 16129 baud for ATR reception.  v125 used
    // only 9600 baud (the usual 3.57 MHz initial rate), which produced a false
    // "no card" result as soon as OSCam released the serial port.
    if (serial == "A104JCGD" || serial == "AD023J2Q") {
        return {{16129u, "6.00MHz/Fi372"}, {9600u, "ISO-default"}};
    }
    // Unknown Phoenix hardware remains conservative: normal ISO default first,
    // then the 6 MHz profile used by this deployment.
    return {{9600u, "ISO-default"}, {16129u, "6.00MHz/Fi372"}};
}

bool getSerialState(int fd, termios2& state) {
    return ::ioctl(fd, TCGETS2, &state) == 0;
}

bool setSerialProfile(int fd, const termios2& base, unsigned baud) {
    termios2 port = base;
    port.c_iflag = INPCK;
    port.c_oflag = 0;
    port.c_lflag = 0;
    port.c_cflag &= ~(CSIZE | PARODD | CBAUD | CRTSCTS);
    port.c_cflag |= CS8 | CLOCAL | CREAD | PARENB | CSTOPB | BOTHER;
    port.c_ispeed = baud;
    port.c_ospeed = baud;
    port.c_cc[VMIN] = 0;
    port.c_cc[VTIME] = 1;
    return ::ioctl(fd, TCSETS2, &port) == 0;
}

void flushSerial(int fd, int selector) {
    // Linux TCFLSH selector values match tcflush(): 0=input, 2=input+output.
    (void)::ioctl(fd, TCFLSH, selector);
}

ProbeResult probeCard(const std::string& device, const std::string& serial) {
    ProbeResult result;
    const std::string canonical = canonicalDevice(device);
    if (usedByAnotherProcess(canonical)) {
        result.status = "busy";
        result.detail = "device is already open by another process";
        return result;
    }

    const int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        result.status = (errno == EACCES || errno == EPERM) ? "permission" : "unavailable";
        result.detail = std::strerror(errno);
        return result;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        result.status = "busy";
        result.detail = "device lock is busy";
        ::close(fd);
        return result;
    }

    termios2 saved{};
    const bool haveSaved = getSerialState(fd, saved);
    if (!haveSaved) {
        result.status = "unavailable";
        result.detail = "cannot read serial port state";
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return result;
    }

    int modemSaved = 0;
    const bool haveModem = ::ioctl(fd, TIOCMGET, &modemSaved) == 0;
    // OSCam configuration for both active readers uses detect=cd.  With this
    // non-inverted setting an asserted carrier-detect line is positive evidence
    // that a card is physically inserted, even if the generic ATR parser cannot
    // complete a reset/ATR exchange.
    const bool cardDetectAsserted = haveModem && ((modemSaved & TIOCM_CAR) != 0);

    auto pulse = [&](int line) {
        int bits = 0;
        if (::ioctl(fd, TIOCMGET, &bits) != 0) return;
        bits &= ~line;
        (void)::ioctl(fd, TIOCMSET, &bits);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        bits |= line;
        (void)::ioctl(fd, TIOCMSET, &bits);
    };

    std::vector<std::string> attempted;
    for (const auto& profile : probeProfiles(serial)) {
        attempted.push_back(std::to_string(profile.baud) + "(" + profile.label + ")");
        if (!setSerialProfile(fd, saved, profile.baud)) continue;
        flushSerial(fd, 2);

        // Common Phoenix/SmartMouse wiring uses RTS or DTR for reset.  Try both
        // without sending any application APDU or CA command.
        pulse(TIOCM_RTS);
        readAtrFor(std::chrono::milliseconds(750), fd, result.atr);
        if (result.atr.empty()) {
            flushSerial(fd, 0);
            pulse(TIOCM_DTR);
            readAtrFor(std::chrono::milliseconds(750), fd, result.atr);
        }
        if (!result.atr.empty()) {
            result.status = "card";
            identifyCard(result.atr, result.cardSystem, result.provider);
            result.detail = "ATR received at " + std::to_string(profile.baud) +
                            " baud (" + profile.label + ")" +
                            (haveModem ? (cardDetectAsserted ? "; CD asserted" : "; CD not asserted") : "");
            break;
        }
    }

    if (haveModem) (void)::ioctl(fd, TIOCMSET, &modemSaved);
    (void)::ioctl(fd, TCSETS2, &saved);
    ::flock(fd, LOCK_UN);
    ::close(fd);

    if (!result.atr.empty()) return result;

    std::ostringstream attempts;
    for (size_t i = 0; i < attempted.size(); ++i) {
        if (i) attempts << ", ";
        attempts << attempted[i];
    }

    if (haveModem && cardDetectAsserted) {
        // Do not report "no card" when the hardware card-detect signal says a
        // card is inserted.  This was the misleading v125 behaviour observed
        // after disabling the OSCam reader.
        result.status = "card_unreadable";
        result.detail = "card-detect asserted; ATR not received; tried " + attempts.str();
    } else if (haveModem) {
        result.status = "no_card";
        result.detail = "card-detect not asserted; ATR not received; tried " + attempts.str();
    } else {
        result.status = "probe_failed";
        result.detail = "ATR not received and card-detect is unavailable; tried " + attempts.str();
    }
    return result;
}

struct ReaderEntry {
    std::string device;
    std::string stableDevice;
};

std::vector<ReaderEntry> enumerateDevicePaths() {
    std::map<std::string, ReaderEntry> unique;
    std::error_code ec;
    const std::filesystem::path byId("/dev/serial/by-id");
    if (std::filesystem::exists(byId, ec) && !ec) {
        for (const auto& entry : std::filesystem::directory_iterator(byId, ec)) {
            if (ec) break;
            const std::string canonical = canonicalDevice(entry.path());
            const std::string tty = std::filesystem::path(canonical).filename().string();
            if (tty.rfind("ttyUSB", 0) != 0 && tty.rfind("ttyACM", 0) != 0) continue;
            unique[canonical] = {canonical, entry.path().string()};
        }
    }
    ec.clear();
    for (const char* prefix : {"ttyUSB", "ttyACM"}) {
        const std::filesystem::path dev("/dev");
        for (const auto& entry : std::filesystem::directory_iterator(dev, ec)) {
            if (ec) break;
            const std::string name = entry.path().filename().string();
            if (name.rfind(prefix, 0) != 0) continue;
            const std::string canonical = canonicalDevice(entry.path());
            if (!unique.count(canonical)) unique[canonical] = {canonical, {}};
        }
        ec.clear();
    }
    std::vector<ReaderEntry> result;
    for (auto& pair : unique) result.push_back(pair.second);
    std::sort(result.begin(), result.end(), [](const ReaderEntry& a, const ReaderEntry& b) {
        const std::string ak = a.stableDevice.empty() ? a.device : a.stableDevice;
        const std::string bk = b.stableDevice.empty() ? b.device : b.stableDevice;
        return ak < bk;
    });
    return result;
}

} // namespace

namespace {

Json::Value enumerateReadersJson(bool probeCardEnabled, const std::string* onlyKey) {
    Json::Value result(Json::arrayValue);
    const auto paths = enumerateDevicePaths();
    unsigned index = 0;
    for (const auto& entry : paths) {
        const std::string ttyName = std::filesystem::path(entry.device).filename().string();
        const auto sysDevice = std::filesystem::path("/sys/class/tty") / ttyName / "device";
        const std::string driver = ttyDriver(ttyName);
        const std::string manufacturer = parentAttribute(sysDevice, "manufacturer");
        const std::string product = parentAttribute(sysDevice, "product");
        if (!ttyLooksLikeReader(ttyName, driver, manufacturer, product)) continue;

        ++index;
        const std::string serial = parentAttribute(sysDevice, "serial");
        const std::string stable = entry.stableDevice.empty() ? entry.device : entry.stableDevice;
        if (onlyKey && !onlyKey->empty() && *onlyKey != stable && *onlyKey != entry.device &&
            *onlyKey != serial && *onlyKey != ttyName) {
            continue;
        }

        Json::Value item;
        item["index"] = index;
        item["name"] = "Phoenix " + std::to_string(index);
        item["device"] = entry.device;
        item["stable_device"] = stable;
        item["tty"] = ttyName;
        item["driver"] = driver;
        item["manufacturer"] = manufacturer;
        item["product"] = product;
        item["vendor_id"] = parentAttribute(sysDevice, "idVendor");
        item["product_id"] = parentAttribute(sysDevice, "idProduct");
        item["serial"] = serial;
        item["phoenix_candidate"] = true;

        const std::string probeDevice = stable;
        if (probeCardEnabled) {
            const ProbeResult probe = probeCard(probeDevice, serial);
            item["status"] = probe.status;
            if (probe.status == "card" || probe.status == "card_unreadable") item["card_present"] = true;
            else if (probe.status == "no_card") item["card_present"] = false;
            else item["card_present"] = Json::Value(Json::nullValue);
            item["atr"] = hexBytes(probe.atr);
            item["historical_text"] = printableHistorical(probe.atr);
            item["card_system"] = probe.cardSystem;
            item["provider_name"] = probe.provider;
            item["detail"] = probe.detail;
        } else {
            const bool busy = usedByAnotherProcess(canonicalDevice(probeDevice));
            item["status"] = busy ? "busy" : "detected";
            item["card_present"] = Json::Value(Json::nullValue);
            item["atr"] = "";
            item["historical_text"] = "";
            item["card_system"] = "";
            item["provider_name"] = "";
            item["detail"] = busy ? "device is already open by another process" : "reader detected; ATR probe not requested";
        }
        result.append(item);
        if (onlyKey) break;
    }
    return result;
}

} // namespace

Json::Value readers(bool probeCardEnabled) {
    return enumerateReadersJson(probeCardEnabled, nullptr);
}

Json::Value reader(const std::string& key, bool probeCardEnabled) {
    Json::Value list = enumerateReadersJson(probeCardEnabled, &key);
    if (!list.isArray() || list.empty()) return Json::Value(Json::nullValue);
    return list[0];
}

} // namespace PhoenixManager
