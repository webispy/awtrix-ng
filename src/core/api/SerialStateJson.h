#pragma once

#include <string>

namespace awtrix {
struct RuntimeState;
struct Settings;

namespace api {

std::string serialSensorsJson(const RuntimeState& state);
std::string serialButtonsJson(const RuntimeState& state);
std::string serialDisplayJson(const Settings& settings, const RuntimeState& state);
std::string serialCapabilitiesJson(const RuntimeState& state);
std::string serialButtonEventJson(const char* button, bool pressed, int64_t atMs);

}
}
