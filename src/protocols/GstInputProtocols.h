#pragma once

#include "ConfigManager.h"

#include <string>
#include <vector>

namespace tvs::protocols {

std::string inputUriForGstreamer(const StreamConfig& cfg);
void appendDecodeInput(std::vector<std::string>& args, const StreamConfig& cfg);
std::vector<std::string> requiredInputElements();
std::vector<std::string> requiredInputElements(const StreamConfig& cfg);

} // namespace tvs::protocols
