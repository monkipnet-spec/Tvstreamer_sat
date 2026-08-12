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

std::vector<NetworkInterface> enumerateNetworkInterfaces() {
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
        if (name == "lo") continue;
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
