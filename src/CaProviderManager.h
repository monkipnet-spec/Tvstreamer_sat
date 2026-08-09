// CaProviderManager.h
#pragma once

#include "ConfigManager.h"

#include <jsoncpp/json/json.h>
#include <string>
#include <cstdint>

namespace ca_provider {

// Структура для хранения контрольных слов (CW)
struct CWResponse {
    bool success = false;
    unsigned char cw_0[8];
    unsigned char cw_1[8];
    uint16_t caid = 0;
    uint16_t provider = 0;
    uint16_t sid = 0;
};

// Enumerates serial Phoenix/USB readers through stable /dev/serial/by-id links.
// This module is intentionally only a reader/card/session inventory layer. It does
// not implement ECM/CW exchange, software descrambling, or key storage/export.
Json::Value enumerateSerialReadersJson();

// Finds an enumerated reader by its stable /dev/serial/by-id path.
const Json::Value* findSerialReaderById(const Json::Value& readers, const std::string& byId);

// Finds a configured CA Card/Provider by logical id, for example ca-card-1.
const CaProviderConfig* findProvider(const AppConfig& config, const std::string& id);

// Per-card capacity helper. Auto mode currently falls back to the configured value
// until a documented card/provider capability interface reports a value.
int effectiveMaxChannels(const CaProviderConfig& provider);

std::string cardStatus(const CaProviderConfig& provider, const Json::Value& serialReaders);
std::string managerStatus(const CaProviderConfig& provider, const Json::Value& serialReaders);

// Returns true for the local serial-reader backend.
bool isReaderBackend(const CaProviderConfig& provider);

// Backend status for UI/API. newcamd-status performs TCP reachability only.
Json::Value backendStatusJson(const CaProviderConfig& provider, const Json::Value& serialReaders);

// ---------------------------------------------------------------------------
// Функции для работы с кэшем CW (контрольных слов)
// ---------------------------------------------------------------------------

// Получение CW из кэша или от backend'а.
// Если в кэше есть валидный ключ, возвращает его, иначе запрашивает у backend'а
// (через requestCWFromBackend) и сохраняет в кэш.
CWResponse getCW(const CaProviderConfig& provider, uint16_t sid,
                 uint16_t caid = 0, uint16_t providerId = 0);

// Очистка всего кэша CW (вызывается при остановке или перезагрузке конфигурации).
void clearCache();

// Установка времени жизни (TTL) в секундах для новых записей кэша.
// По умолчанию 7 секунд (типичное время жизни CW).
void setCacheTTL(int ttlSeconds);

// Завершение работы модуля CA Provider – очистка кэша и освобождение ресурсов.
// Должна вызываться при остановке программы.
void shutdownCaProvider();

} // namespace ca_provider