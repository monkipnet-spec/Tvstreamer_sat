#pragma once

#include <gst/gst.h>
#include <jsoncpp/json/json.h>

#include <cstdint>
#include <string>

struct DvbSatelliteParams {
    int adapter = 0;
    int frontend = 0;
    uint32_t frequencyKHz = 11727000;
    uint32_t symbolRateK = 27500;
    std::string polarity = "H";
    std::string deliverySystem = "dvb-s2";
    std::string modulation = "auto";
    std::string fec = "auto";
    int diseqcSource = -1;
    uint32_t lnbLof1KHz = 9750000;
    uint32_t lnbLof2KHz = 10600000;
    uint32_t lnbSlofKHz = 11700000;
    int streamId = -1;
};

namespace DvbSatellite {

bool isDvbUri(const std::string& uri);
bool parseUri(const std::string& uri, DvbSatelliteParams& params, std::string& error);
std::string buildUri(const DvbSatelliteParams& params);

bool configureSource(GstElement* source, const DvbSatelliteParams& params, std::string& error);
Json::Value adapters();
Json::Value scan(const Json::Value& request);
Json::Value signal(const Json::Value& request);

} // namespace DvbSatellite
