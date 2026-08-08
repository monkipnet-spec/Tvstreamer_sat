#pragma once

#include <string>
#include <vector>

struct NetworkInterface {
    std::string name;
    std::string address;
    bool isUp = false;
    bool supportsMulticast = false;
};

std::string toLower(const std::string& value);
std::string gstQuote(const std::string& value);
std::string normalizeIpAddress(const std::string& value);
std::string md5Hex(const std::string& value);
std::string normalizeMd5Password(const std::string& value);
bool verifyMd5Password(const std::string& candidate, const std::string& storedValue);
std::vector<NetworkInterface> enumerateNetworkInterfaces();
