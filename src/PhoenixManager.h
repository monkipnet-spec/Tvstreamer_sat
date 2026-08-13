#pragma once

#include <jsoncpp/json/json.h>

namespace PhoenixManager {

// Enumerate Phoenix/SmartMouse-style USB serial readers and perform a
// conservative, read-only ATR presence probe when the port is free.
Json::Value readers();

} // namespace PhoenixManager
