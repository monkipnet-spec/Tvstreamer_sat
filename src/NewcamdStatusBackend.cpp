#include "NewcamdStatusBackend.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace ca_provider {

bool NewcamdStatusBackend::parseEndpoint(const std::string& endpoint, std::string& host, int& port) {
    host.clear();
    port = 0;
    if (endpoint.empty()) return false;

    std::string value = endpoint;
    const std::string prefix = "newcamd://";
    if (value.rfind(prefix, 0) == 0) value.erase(0, prefix.size());

    // Status backend accepts host:port only. Credentials/keys are deliberately not parsed.
    const auto slash = value.find('/');
    if (slash != std::string::npos) value.resize(slash);
    const auto at = value.rfind('@');
    if (at != std::string::npos) value.erase(0, at + 1);

    if (!value.empty() && value.front() == '[') {
        const auto close = value.find(']');
        if (close == std::string::npos || close + 2 > value.size() || value[close + 1] != ':') return false;
        host = value.substr(1, close - 1);
        try { port = std::stoi(value.substr(close + 2)); } catch (...) { return false; }
    } else {
        const auto colon = value.rfind(':');
        if (colon == std::string::npos) return false;
        host = value.substr(0, colon);
        try { port = std::stoi(value.substr(colon + 1)); } catch (...) { return false; }
    }
    return !host.empty() && port > 0 && port <= 65535;
}

NewcamdStatusResult NewcamdStatusBackend::probe(const std::string& endpoint, int timeoutMs) {
    NewcamdStatusResult result;
    if (!parseEndpoint(endpoint, result.host, result.port)) {
        result.status = "NOT_CONFIGURED";
        result.error = endpoint.empty() ? "endpoint is empty" : "expected host:port";
        return result;
    }
    result.configured = true;

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* resolved = nullptr;
    const std::string service = std::to_string(result.port);
    const int gai = ::getaddrinfo(result.host.c_str(), service.c_str(), &hints, &resolved);
    if (gai != 0) {
        result.status = "DNS_ERROR";
        result.error = ::gai_strerror(gai);
        return result;
    }

    std::string lastError = "connection failed";
    for (addrinfo* p = resolved; p; p = p->ai_next) {
        const int fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;

        const int oldFlags = ::fcntl(fd, F_GETFL, 0);
        if (oldFlags >= 0) ::fcntl(fd, F_SETFL, oldFlags | O_NONBLOCK);
        int rc = ::connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc == 0) {
            result.online = true;
        } else if (errno == EINPROGRESS) {
            pollfd pfd{fd, POLLOUT, 0};
            rc = ::poll(&pfd, 1, timeoutMs);
            if (rc > 0 && (pfd.revents & (POLLOUT | POLLERR | POLLHUP))) {
                int error = 0;
                socklen_t len = sizeof(error);
                if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) == 0 && error == 0) {
                    result.online = true;
                } else if (error != 0) {
                    lastError = std::strerror(error);
                }
            } else if (rc == 0) {
                lastError = "connect timeout";
            }
        } else {
            lastError = std::strerror(errno);
        }
        ::close(fd);
        if (result.online) break;
    }
    ::freeaddrinfo(resolved);

    result.status = result.online ? "TCP_ONLINE" : "TCP_OFFLINE";
    if (!result.online) result.error = lastError;
    return result;
}

} // namespace ca_provider
