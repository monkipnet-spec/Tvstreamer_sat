#include "../../../CaBackendPluginApi.h"
#include <iostream>
#include <cstring>

static void* newcamd_create(const struct tvs_ca_host_api_v1* host) {
    return new int(1);
}

static void newcamd_destroy(void* instance) {
    delete static_cast<int*>(instance);
}

static int newcamd_open_reader(void* instance, const struct tvs_ca_reader_info_v1* reader, char* error, size_t error_size) {
    return TVS_CA_RESULT_OK;
}

static void newcamd_close_reader(void* instance, const char* reader_key) {}

static int newcamd_start_service(void* instance, const char* reader_key, const struct tvs_ca_service_info_v1* service, char* error, size_t error_size) {
    return TVS_CA_RESULT_OK;
}

static void newcamd_stop_service(void* instance, const char* stream_id) {}

static int newcamd_process_ts(void* instance, const char* stream_id, uint8_t* data, size_t size, struct tvs_ca_ts_result_v1* result) {
    return TVS_CA_RESULT_PASSTHROUGH; 
}

static const char* newcamd_status_json(void* instance) {
    return "{\"status\": \"initialized\"}";
}

static const tvs_ca_backend_api_v1 newcamd_api = {
    TVS_CA_BACKEND_ABI_V1,
    "newcamd",
    "Newcamd OSCAM Client",
    "Monk",
    TVS_CA_CAP_TS_INPLACE | TVS_CA_CAP_MULTI_SERVICE,
    newcamd_create,
    newcamd_destroy,
    newcamd_open_reader,
    newcamd_close_reader,
    newcamd_start_service,
    newcamd_stop_service,
    newcamd_process_ts,
    newcamd_status_json
};

extern "C" const tvs_ca_backend_api_v1* tvstreammersat5_ca_backend_get_api_v1(void) {
    return &newcamd_api;
}
