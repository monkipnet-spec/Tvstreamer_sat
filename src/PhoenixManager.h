#pragma once

#include <jsoncpp/json/json.h>
#include <string>

namespace PhoenixManager {

// Enumerate Phoenix/SmartMouse-style USB serial readers and perform a
// conservative, read-only ATR presence probe when the port is free.
Json::Value readers(bool probeCard = true);

// Inspect one reader by stable path, tty path, or USB serial. Unlike readers(),
// this does not reset/probe unrelated free readers. Returns null when missing.
Json::Value reader(const std::string& key, bool probeCard = true);

} // namespace PhoenixManager
