// Minimal external plugin example for the TVStreammerSAT5 CaBackend ABI.
// It intentionally DOES NOT descramble anything.  It is useful to validate
// loading, reader/service lifecycle, TS callbacks and telemetry before wiring
// an authorised operator/manufacturer SDK behind the same ABI.

#include "ca/CaBackendPluginApi.h"

#include <cstring>
#include <map>
#include <set>
#include <string>

namespace {
struct State {
    const tvs_ca_host_api_v1* host = nullptr;
    std::set<std::string> readers;
    std::map<std::string, std::string> services;
    std::string status = R"({"state":"ready","mode":"passthrough-example"})";
};

void* create(const tvs_ca_host_api_v1* host) {
    auto* state = new State();
    state->host = host;
    return state;
}
void destroy(void* p) { delete static_cast<State*>(p); }
int openReader(void* p, const tvs_ca_reader_info_v1* reader, char*, size_t) {
    auto* state = static_cast<State*>(p);
    state->readers.insert(reader && reader->reader_key ? reader->reader_key : "");
    return TVS_CA_RESULT_OK;
}
void closeReader(void* p, const char* key) {
    static_cast<State*>(p)->readers.erase(key ? key : "");
}
int startService(void* p, const char* readerKey, const tvs_ca_service_info_v1* service, char*, size_t) {
    auto* state = static_cast<State*>(p);
    if (!service || !service->stream_id) return TVS_CA_RESULT_ERROR;
    state->services[service->stream_id] = readerKey ? readerKey : "";
    return TVS_CA_RESULT_PASSTHROUGH;
}
void stopService(void* p, const char* streamId) {
    static_cast<State*>(p)->services.erase(streamId ? streamId : "");
}
int processTs(void*, const char*, uint8_t*, size_t size, tvs_ca_ts_result_v1* result) {
    if (result) {
        result->packets_seen = size / 188;
        std::strncpy(result->status, "PASSTHROUGH_EXAMPLE", sizeof(result->status) - 1);
    }
    return TVS_CA_RESULT_PASSTHROUGH;
}
const char* statusJson(void* p) { return static_cast<State*>(p)->status.c_str(); }

const tvs_ca_backend_api_v1 api = {
    TVS_CA_BACKEND_ABI_V1,
    "passthrough-example",
    "Passthrough example plugin",
    "TVStreammerSAT5 SDK example",
    TVS_CA_CAP_MULTI_SERVICE,
    &create,
    &destroy,
    &openReader,
    &closeReader,
    &startService,
    &stopService,
    &processTs,
    &statusJson
};
} // namespace

extern "C" const tvs_ca_backend_api_v1* tvstreammersat5_ca_backend_get_api_v1() {
    return &api;
}
