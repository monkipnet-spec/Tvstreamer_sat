#include "../../CaBackendPluginApi.h"
#include "NewcamdClient.h"

#include <dvbcsa/dvbcsa.h>
#include <jsoncpp/json/json.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>

struct NewcamdInstance {
    std::unique_ptr<NewcamdClient> client;
    bool connected = false;
    dvbcsa_key_t* key = nullptr;
    std::mutex key_mutex;

    NewcamdInstance() { key = dvbcsa_key_alloc(); }
    ~NewcamdInstance() { if (key) dvbcsa_key_free(key); }
};

namespace {

void write_error(char* error, size_t error_size, const std::string& message) {
    if (!error || error_size == 0) return;
    const size_t count = std::min(error_size - 1, message.size());
    std::memcpy(error, message.data(), count);
    error[count] = '\0';
}

Json::Value parse_config(const char* json, std::string& error) {
    Json::Value config;
    Json::CharReaderBuilder builder;
    std::istringstream input(json && *json ? json : "{}");
    if (!Json::parseFromStream(builder, input, &config, &error)) return Json::Value();
    return config;
}

} // namespace

extern "C" {

static void* newcamd_create(const struct tvs_ca_host_api_v1* host) {
    (void)host;
    return new NewcamdInstance();
}

static void newcamd_destroy(void* instance) {
    delete static_cast<NewcamdInstance*>(instance);
}

static int newcamd_open_reader(void* instance, const struct tvs_ca_reader_info_v1* reader, char* error, size_t error_size) {
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !reader) {
        write_error(error, error_size, "invalid Newcamd reader instance");
        return TVS_CA_RESULT_ERROR;
    }

    std::string parse_error;
    Json::Value config = parse_config(reader->backend_config_json, parse_error);
    if (!parse_error.empty()) {
        write_error(error, error_size, "invalid Newcamd backend_config JSON: " + parse_error);
        return TVS_CA_RESULT_ERROR;
    }

    inst->client = std::make_unique<NewcamdClient>(
        config.get("host", "127.0.0.1").asString(),
        config.get("port", 15000).asInt(),
        config.get("user", "user").asString(),
        config.get("pass", "pass").asString(),
        config.get("des", "0102030405060708091011121314").asString());

    inst->client->set_key_update_callback([inst](const uint8_t* dcw) {
        if (!dcw || !inst->key) return;
        std::lock_guard<std::mutex> lock(inst->key_mutex);
        dvbcsa_key_set(dcw, inst->key);
    });

    if (inst->client->connect() && inst->client->login()) {
        inst->client->start_receiver();
        inst->connected = true;
        return TVS_CA_RESULT_OK;
    }

    inst->client.reset();
    inst->connected = false;
    write_error(error, error_size, "Newcamd connect/login failed");
    return TVS_CA_RESULT_ERROR;
}

static void newcamd_close_reader(void* instance, const char* reader_key) {
    (void)reader_key;
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst) return;
    inst->client.reset();
    inst->connected = false;
}

static int newcamd_start_service(void* instance, const char* reader_key, const struct tvs_ca_service_info_v1* service, char* error, size_t error_size) {
    (void)instance;
    (void)reader_key;
    (void)service;
    (void)error;
    (void)error_size;
    return TVS_CA_RESULT_OK;
}

static void newcamd_stop_service(void* instance, const char* stream_id) {
    (void)instance;
    (void)stream_id;
}

static int newcamd_process_ts(void* instance, const char* stream_id, uint8_t* data, size_t size, struct tvs_ca_ts_result_v1* result) {
    (void)stream_id;
    auto* inst = static_cast<NewcamdInstance*>(instance);
    if (!inst || !inst->connected || !data || !result || !inst->key) return TVS_CA_RESULT_PASSTHROUGH;

    constexpr size_t kTsPacketSize = 188;
    bool changed = false;
    for (size_t offset = 0; offset + kTsPacketSize <= size; offset += kTsPacketSize) {
        uint8_t* packet = data + offset;
        if (packet[0] != 0x47) continue;

        result->packets_seen++;
        if ((packet[3] & 0xC0) == 0) {
            result->packets_clear++;
            continue;
        }

        result->packets_scrambled++;
        std::lock_guard<std::mutex> lock(inst->key_mutex);
        dvbcsa_decrypt(inst->key, packet + 4, kTsPacketSize - 4);
        packet[3] &= 0x3F;
        result->packets_changed++;
        changed = true;
    }

    return changed ? TVS_CA_RESULT_OK : TVS_CA_RESULT_PASSTHROUGH;
}

static const char* newcamd_status_json(void* instance) {
    static thread_local std::string status;
    auto* inst = static_cast<NewcamdInstance*>(instance);
    status = inst && inst->connected ? "{\"status\":\"connected\"}" : "{\"status\":\"disconnected\"}";
    return status.c_str();
}

static const tvs_ca_backend_api_v1 api = {
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

const tvs_ca_backend_api_v1* tvstreammersat5_ca_backend_get_api_v1(void) {
    return &api;
}

}
