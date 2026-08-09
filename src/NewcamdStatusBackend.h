#pragma once

#include <string>

namespace ca_provider {

struct NewcamdStatusResult {
    bool configured = false;
    bool online = false;
    std::string host;
    int port = 0;
    std::string status;
    std::string error;
};

// Status-only network backend. It performs DNS/TCP reachability checks only.
// It intentionally does not implement Newcamd authentication, ECM/EMM exchange,
// CW handling, descrambling, or key storage.
class NewcamdStatusBackend {
public:
    static NewcamdStatusResult probe(const std::string& endpoint, int timeoutMs = 1200);
    static bool parseEndpoint(const std::string& endpoint, std::string& host, int& port);
};

} // namespace ca_provider
