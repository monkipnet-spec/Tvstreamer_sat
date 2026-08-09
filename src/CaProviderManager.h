#pragma once

#include "ConfigManager.h"

#include <jsoncpp/json/json.h>
#include <string>

namespace ca_provider {

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

} // namespace ca_provider
