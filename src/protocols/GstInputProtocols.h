#pragma once

#include "ConfigManager.h"

#include <string>
#include <vector>

namespace tvs::protocols {

std::string inputUriForGstreamer(const StreamConfig& cfg);
void appendDecodeInput(std::vector<std::string>& args, const StreamConfig& cfg);
std::vector<std::string> requiredInputElements();

} // namespace tvs::protocols
