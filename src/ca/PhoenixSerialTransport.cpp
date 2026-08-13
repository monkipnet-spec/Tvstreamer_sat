#include "ca/PhoenixSerialTransport.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <asm/ioctls.h>
#include <asm/termbits.h>
#include <unistd.h>

namespace ca {
namespace {

std::string canonicalDevice(const std::string& path) {
    std::error_code ec;
    const auto resolved = std::filesystem::canonical(path, ec);
    return ec ? path : resolved.string();
}

bool usedByAnotherProcess(const std::string& canonicalPath) {
    std::error_code ec;
    const pid_t self = ::getpid();
    for (const auto& process : std::filesystem::directory_iterator("/proc", ec)) {
        if (ec || !process.is_directory(ec)) continue;
        const std::string pidText = process.path().filename().string();
        if (pidText.empty() || !std::all_of(pidText.begin(), pidText.end(), [](unsigned char c) { return c >= '0' && c <= '9'; })) continue;
        pid_t pid = 0;
        try { pid = static_cast<pid_t>(std::stol(pidText)); } catch (...) { continue; }
        if (pid == self) continue;
        const auto fdDir = process.path() / "fd";
        if (!std::filesystem::exists(fdDir, ec) || ec) { ec.clear(); continue; }
        for (const auto& fd : std::filesystem::directory_iterator(fdDir, ec)) {
            if (ec) { ec.clear(); break; }
            const auto target = std::filesystem::read_symlink(fd.path(), ec);
            if (ec) { ec.clear(); continue; }
            if (target.string() == canonicalPath) return true;
        }
    }
    return false;
}

void flushSerial(int fd, int selector) {
    (void)::ioctl(fd, TCFLSH, selector);
}

bool configureProfile(int fd, const termios2& base, unsigned baud) {
    termios2 port = base;
    port.c_iflag = INPCK;
    port.c_oflag = 0;
    port.c_lflag = 0;
    port.c_cflag &= ~(CSIZE | PARODD | CBAUD | CRTSCTS);
    port.c_cflag |= CS8 | CLOCAL | CREAD | PARENB | CSTOPB | BOTHER;
    port.c_ispeed = baud;
    port.c_ospeed = baud;
    // Reads are driven by poll(), not O_NONBLOCK. VTIME is a small safety net.
    port.c_cc[VMIN] = 0;
    port.c_cc[VTIME] = 1;
    return ::ioctl(fd, TCSETS2, &port) == 0;
}

bool pulseLine(int fd, int line, std::chrono::milliseconds lowTime = std::chrono::milliseconds(120)) {
    int bits = 0;
    if (::ioctl(fd, TIOCMGET, &bits) != 0) return false;
    const int original = bits;
    bits &= ~line;
    if (::ioctl(fd, TIOCMSET, &bits) != 0) return false;
    std::this_thread::sleep_for(lowTime);
    bits = original | line;
    return ::ioctl(fd, TIOCMSET, &bits) == 0;
}

bool atrLooksComplete(const std::vector<std::uint8_t>& atr) {
    if (atr.size() < 2 || (atr[0] != 0x3B && atr[0] != 0x3F)) return false;
    size_t pos = 2;
    unsigned y = (atr[1] >> 4) & 0x0F;
    const size_t historicalCount = atr[1] & 0x0F;
    bool anyProtocolNotT0 = false;
    bool more = true;
    while (more) {
        if (y & 0x1) { if (pos >= atr.size()) return false; ++pos; }
        if (y & 0x2) { if (pos >= atr.size()) return false; ++pos; }
        if (y & 0x4) { if (pos >= atr.size()) return false; ++pos; }
        if (y & 0x8) {
            if (pos >= atr.size()) return false;
            const std::uint8_t td = atr[pos++];
            if ((td & 0x0F) != 0) anyProtocolNotT0 = true;
            y = (td >> 4) & 0x0F;
        } else {
            more = false;
        }
    }
    const size_t expected = pos + historicalCount + (anyProtocolNotT0 ? 1u : 0u);
    return atr.size() >= expected;
}

bool readAtr(int fd, std::chrono::milliseconds timeout, std::vector<std::uint8_t>& atr) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::vector<std::uint8_t> buffer;
    buffer.reserve(96);

    while (std::chrono::steady_clock::now() < deadline && buffer.size() < 96) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        const int waitMs = static_cast<int>(std::max<long long>(1, std::min<long long>(100, remaining.count())));
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int pr = ::poll(&pfd, 1, waitMs);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0 || !(pfd.revents & POLLIN)) continue;

        std::uint8_t chunk[64];
        const ssize_t got = ::read(fd, chunk, sizeof(chunk));
        if (got < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return false;
        }
        if (got == 0) continue;
        buffer.insert(buffer.end(), chunk, chunk + got);

        auto start = std::find_if(buffer.begin(), buffer.end(), [](std::uint8_t b) { return b == 0x3B || b == 0x3F; });
        if (start == buffer.end()) continue;
        atr.assign(start, buffer.end());
        if (atrLooksComplete(atr)) return true;
    }
    return !atr.empty();
}

std::string attemptedProfiles(const std::vector<PhoenixProbeProfile>& profiles) {
    std::ostringstream out;
    for (size_t i = 0; i < profiles.size(); ++i) {
        if (i) out << ", ";
        out << profiles[i].baud;
        if (!profiles[i].label.empty()) out << "(" << profiles[i].label << ")";
    }
    return out.str();
}

} // namespace

struct PhoenixSerialTransport::Impl {
    PhoenixSerialConfig config;
    int fd = -1;
    termios2 saved{};
    bool haveSaved = false;
    int modemSaved = 0;
    bool haveModem = false;
    bool locked = false;
};

PhoenixSerialTransport::PhoenixSerialTransport() : impl_(std::make_unique<Impl>()) {}
PhoenixSerialTransport::~PhoenixSerialTransport() { close(); }

bool PhoenixSerialTransport::open(const PhoenixSerialConfig& config, std::string* error) {
    if (error) error->clear();
    close();
    impl_->config = config;
    if (impl_->config.probeProfiles.empty()) impl_->config.probeProfiles = defaultProbeProfiles(config.serial);
    if (impl_->config.devicePath.empty()) {
        if (error) *error = "Phoenix device path is empty";
        return false;
    }

    const std::string canonical = canonicalDevice(impl_->config.devicePath);
    if (usedByAnotherProcess(canonical)) {
        if (error) *error = "device is already open by another process";
        return false;
    }

    impl_->fd = ::open(impl_->config.devicePath.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (impl_->fd < 0) {
        if (error) *error = std::strerror(errno);
        return false;
    }
    if (::flock(impl_->fd, LOCK_EX | LOCK_NB) != 0) {
        if (error) *error = "device lock is busy";
        close();
        return false;
    }
    impl_->locked = true;

    impl_->haveSaved = (::ioctl(impl_->fd, TCGETS2, &impl_->saved) == 0);
    if (!impl_->haveSaved) {
        if (error) *error = "cannot read serial port state";
        close();
        return false;
    }
    impl_->haveModem = (::ioctl(impl_->fd, TIOCMGET, &impl_->modemSaved) == 0);
    return true;
}

void PhoenixSerialTransport::close() {
    if (!impl_ || impl_->fd < 0) return;
    if (impl_->haveModem) (void)::ioctl(impl_->fd, TIOCMSET, &impl_->modemSaved);
    if (impl_->haveSaved) (void)::ioctl(impl_->fd, TCSETS2, &impl_->saved);
    if (impl_->locked) (void)::flock(impl_->fd, LOCK_UN);
    ::close(impl_->fd);
    impl_->fd = -1;
    impl_->haveSaved = false;
    impl_->haveModem = false;
    impl_->locked = false;
}

bool PhoenixSerialTransport::reconnect(std::string* error) {
    const PhoenixSerialConfig copy = impl_->config;
    close();
    std::this_thread::sleep_for(std::chrono::milliseconds(75));
    return open(copy, error);
}

bool PhoenixSerialTransport::isOpen() const { return impl_ && impl_->fd >= 0; }

PhoenixCardPresence PhoenixSerialTransport::cardPresence() const {
    if (!isOpen()) return PhoenixCardPresence::Unknown;
    if (impl_->config.detectMode != "cd") return PhoenixCardPresence::Unknown;
    int bits = 0;
    if (::ioctl(impl_->fd, TIOCMGET, &bits) != 0) return PhoenixCardPresence::Unknown;
    return (bits & TIOCM_CAR) ? PhoenixCardPresence::Present : PhoenixCardPresence::Absent;
}

PhoenixProbeResult PhoenixSerialTransport::resetAndReadAtr(std::chrono::milliseconds perResetTimeout) {
    PhoenixProbeResult result;
    if (!isOpen()) {
        result.status = "unavailable";
        result.detail = "reader is not open";
        return result;
    }

    result.presence = cardPresence();
    if (impl_->config.detectMode == "cd" && result.presence == PhoenixCardPresence::Absent) {
        result.status = "no_card";
        result.detail = "card-detect not asserted";
        return result;
    }

    for (const auto& profile : impl_->config.probeProfiles) {
        if (!configureProfile(impl_->fd, impl_->saved, profile.baud)) continue;
        flushSerial(impl_->fd, 2);

        std::vector<std::uint8_t> atr;
        (void)pulseLine(impl_->fd, TIOCM_RTS);
        (void)readAtr(impl_->fd, perResetTimeout, atr);
        if (atr.empty()) {
            flushSerial(impl_->fd, 0);
            (void)pulseLine(impl_->fd, TIOCM_DTR);
            (void)readAtr(impl_->fd, perResetTimeout, atr);
        }
        if (!atr.empty()) {
            result.status = "card";
            result.atr = std::move(atr);
            result.baud = profile.baud;
            result.profileLabel = profile.label;
            std::ostringstream detail;
            detail << "ATR received at " << profile.baud << " baud";
            if (!profile.label.empty()) detail << " (" << profile.label << ")";
            if (result.presence == PhoenixCardPresence::Present) detail << "; CD asserted";
            else if (result.presence == PhoenixCardPresence::Absent) detail << "; CD not asserted";
            result.detail = detail.str();
            return result;
        }
    }

    if (result.presence == PhoenixCardPresence::Present) {
        result.status = "card_unreadable";
        result.detail = "card-detect asserted; ATR not received; tried " + attemptedProfiles(impl_->config.probeProfiles);
    } else if (result.presence == PhoenixCardPresence::Absent) {
        result.status = "no_card";
        result.detail = "card-detect not asserted; ATR not received; tried " + attemptedProfiles(impl_->config.probeProfiles);
    } else {
        result.status = "probe_failed";
        result.detail = "ATR not received and card-detect is unavailable; tried " + attemptedProfiles(impl_->config.probeProfiles);
    }
    return result;
}

const PhoenixSerialConfig& PhoenixSerialTransport::config() const { return impl_->config; }

PhoenixProbeResult PhoenixSerialTransport::probe(const PhoenixSerialConfig& config,
                                                  std::chrono::milliseconds perResetTimeout) {
    PhoenixSerialTransport transport;
    std::string error;
    if (!transport.open(config, &error)) {
        PhoenixProbeResult result;
        if (error == "device is already open by another process" || error == "device lock is busy") result.status = "busy";
        else if (error == "Permission denied" || error == "Operation not permitted") result.status = "permission";
        else result.status = "unavailable";
        result.detail = error;
        return result;
    }
    return transport.resetAndReadAtr(perResetTimeout);
}

std::vector<PhoenixProbeProfile> PhoenixSerialTransport::defaultProbeProfiles(const std::string& serial) {
    if (serial == "A104JCGD" || serial == "AD023J2Q") {
        return {{16129u, "6.00MHz/Fi372"}, {9600u, "ISO-default"}};
    }
    return {{9600u, "ISO-default"}, {16129u, "6.00MHz/Fi372"}};
}

} // namespace ca
