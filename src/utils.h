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
std::vector<NetworkInterface> enumerateNetworkInterfaces();
