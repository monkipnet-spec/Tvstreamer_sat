#include "utils.h"

#include <boost/algorithm/string.hpp>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>

std::string toLower(const std::string& value) {
    std::string lower = value;
    boost::algorithm::to_lower(lower);
    return lower;
}

std::string gstQuote(const std::string& value) {
    std::string result = "'";
    for (char c : value) {
        if (c == '\'') {
            result += "\\'";
        } else if (c == '\\') {
            result += "\\\\";
        } else {
            result += c;
        }
    }
    result += "'";
    return result;
}

std::string normalizeIpAddress(const std::string& value) {
    std::string normalized = value;
    boost::algorithm::trim(normalized);
    if (normalized.size() >= 2 && normalized.front() == '[' && normalized.back() == ']') {
        normalized = normalized.substr(1, normalized.size() - 2);
    }

    const std::string ipv4MappedPrefix = "::ffff:";
    if (boost::algorithm::istarts_with(normalized, ipv4MappedPrefix)) {
        return normalized.substr(ipv4MappedPrefix.size());
    }
    return normalized;
}


std::string normalizeInputUri(const std::string& value) {
    std::string normalized = value;
    boost::algorithm::trim(normalized);

    // Copy/paste from shell/config examples may include a matching quote pair.
    // Strip it only for URI-like values so legitimate local filenames are not
    // silently rewritten.
    if (normalized.size() >= 2 &&
        ((normalized.front() == '"' && normalized.back() == '"') ||
         (normalized.front() == '\'' && normalized.back() == '\''))) {
        std::string inner = normalized.substr(1, normalized.size() - 2);
        boost::algorithm::trim(inner);
        if (inner.find("://") != std::string::npos) {
            normalized = std::move(inner);
        }
    }
    return normalized;
}


bool normalizeUdpEndpoint(const std::string& rawHost, int configuredPort,
                          std::string& host, int& port) {
    std::string endpoint = rawHost;
    boost::algorithm::trim(endpoint);
    if (endpoint.empty()) return false;

    const std::string lower = toLower(endpoint);
    if (lower.rfind("udp://", 0) == 0 || lower.rfind("rtp://", 0) == 0) {
        endpoint.erase(0, 6);
    }
    if (!endpoint.empty() && endpoint.front() == '@') endpoint.erase(endpoint.begin());

    // Strip URI options/path if a complete udp:// URI was pasted into the host box.
    const auto suffix = endpoint.find_first_of("/?");
    if (suffix != std::string::npos) endpoint.resize(suffix);
    boost::algorithm::trim(endpoint);
    if (endpoint.empty()) return false;

    host = endpoint;
    port = configuredPort;

    // UDP output is IPv4 in the current sender implementation. A single colon
    // therefore unambiguously means host:port. Do not reinterpret IPv6 here.
    const auto colon = endpoint.rfind(':');
    if (colon != std::string::npos && endpoint.find(':') == colon && colon > 0 && colon + 1 < endpoint.size()) {
        const std::string portText = endpoint.substr(colon + 1);
        const bool numeric = std::all_of(portText.begin(), portText.end(),
            [](unsigned char c) { return c >= '0' && c <= '9'; });
        if (numeric) {
            try {
                const int embeddedPort = std::stoi(portText);
                if (embeddedPort < 1 || embeddedPort > 65535) return false;
                host = endpoint.substr(0, colon);
                port = embeddedPort;
            } catch (...) {
                return false;
            }
        }
    }

    boost::algorithm::trim(host);
    return !host.empty() && port >= 1 && port <= 65535;
}

std::vector<NetworkInterface> enumerateNetworkInterfaces(bool includeLoopback) {
    std::vector<NetworkInterface> list;
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0) {
        return list;
    }

    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;

        char host[NI_MAXHOST] = {0};
        int result = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in), host, NI_MAXHOST,
                                 nullptr, 0, NI_NUMERICHOST);
        if (result != 0) continue;

        std::string name(ifa->ifa_name);
        if (name == "lo" && !includeLoopback) continue;
        list.push_back({
            name,
            std::string(host),
            (ifa->ifa_flags & IFF_UP) != 0,
            (ifa->ifa_flags & IFF_MULTICAST) != 0
        });
    }

    freeifaddrs(ifaddr);
    return list;
}
