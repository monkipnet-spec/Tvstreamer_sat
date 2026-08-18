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
std::vector<NetworkInterface> enumerateNetworkInterfaces(bool includeLoopback = false);

// Accept UDP/RTP destinations entered either as separate host+port fields or
// as a combined endpoint such as 234.1.2.1:8599 / udp://@234.1.2.1:8599.
// The embedded port, when present and valid, takes precedence.
bool normalizeUdpEndpoint(const std::string& rawHost, int configuredPort,
                          std::string& host, int& port);
