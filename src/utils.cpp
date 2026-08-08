#include "utils.h"

#include <boost/algorithm/string.hpp>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/socket.h>
#include <glib.h>
#include <algorithm>
#include <cctype>

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


namespace {

bool isHexDigest(const std::string& value) {
    return value.size() == 32 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isxdigit(ch) != 0;
    });
}

bool constantTimeStringEquals(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < left.size(); ++i) {
        diff |= static_cast<unsigned char>(left[i] ^ right[i]);
    }
    return diff == 0;
}

} // namespace

std::string md5Hex(const std::string& value) {
    gchar* checksum = g_compute_checksum_for_string(G_CHECKSUM_MD5, value.c_str(), static_cast<gssize>(value.size()));
    if (!checksum) return {};
    std::string result(checksum);
    g_free(checksum);
    return toLower(result);
}

std::string normalizeMd5Password(const std::string& value) {
    std::string normalized = value;
    if (normalized.rfind("md5:", 0) == 0) {
        std::string digest = toLower(normalized.substr(4));
        if (isHexDigest(digest)) return "md5:" + digest;
    }
    if (isHexDigest(normalized)) {
        return "md5:" + toLower(normalized);
    }
    return "md5:" + md5Hex(normalized);
}

bool verifyMd5Password(const std::string& candidate, const std::string& storedValue) {
    const std::string normalized = normalizeMd5Password(storedValue);
    if (normalized.rfind("md5:", 0) != 0) return false;
    return constantTimeStringEquals(md5Hex(candidate), normalized.substr(4));
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
