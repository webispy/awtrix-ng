#include "core/api/SerialStateJson.h"

#include "core/RuntimeState.h"
#include "core/Settings.h"
#include "core/api/JsonWriter.h"

namespace awtrix::api {

namespace {

void available(JsonWriter& w, const char* name, bool present) {
  w.member(name, present);
}

}

std::string serialSensorsJson(const RuntimeState& s) {
  std::string out;
  out.reserve(256);
  JsonWriter w(out);
  w.beginObject();
  if (s.hasTemperature) w.member("temperature", s.temperatureC, 1);
  else w.memberNull("temperature");
  if (s.hasHumidity) w.member("humidity", s.humidity, 1);
  else w.memberNull("humidity");
  if (s.hasPressure) w.member("pressureHpa", s.pressureHpa, 1);
  else w.memberNull("pressureHpa");
  if (s.hasLightSensor) {
    w.member("lightLevel", s.lightLevel, 1);
    w.member("ldrRaw", s.ldrRaw);
  } else {
    w.memberNull("lightLevel");
    w.memberNull("ldrRaw");
  }
  if (s.hasBattery) {
    w.member("batteryPercent", s.batteryPercent);
    w.member("batteryVoltage", s.batteryVoltage, 2);
    w.member("batteryPinMillivolts", s.batteryPinMillivolts);
    w.member("lowBattery", s.lowBattery);
  } else {
    w.memberNull("batteryPercent");
    w.memberNull("batteryVoltage");
    w.memberNull("batteryPinMillivolts");
    w.memberNull("lowBattery");
  }
  w.endObject();
  return out;
}

std::string serialButtonsJson(const RuntimeState& s) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.member("left", s.buttons[0]);
  w.member("select", s.buttons[1]);
  w.member("right", s.buttons[2]);
  w.endObject();
  return out;
}

std::string serialDisplayJson(const Settings& settings, const RuntimeState& state) {
  std::string out;
  JsonWriter w(out);
  w.beginObject();
  w.member("brightness", settings.brightness);
  w.member("brightnessActual", state.brightnessActual);
  w.member("autoBrightness", settings.autoBrightness);
  w.member("gamma", settings.gamma, 2);
  w.endObject();
  return out;
}

std::string serialCapabilitiesJson(const RuntimeState& s) {
  std::string out;
  out.reserve(256);
  JsonWriter w(out);
  w.beginObject();
  w.member("protocol", 3);
  w.key("queries");
  w.beginArray();
  w.value("qry/capabilities");
  w.value("qry/sensors");
  w.value("qry/buttons");
  w.value("qry/display");
  w.endArray();
  w.key("controls");
  w.beginArray();
  w.value("brightness");
  w.endArray();
  w.key("sensors");
  w.beginObject();
  available(w, "temperature", s.hasTemperature);
  available(w, "humidity", s.hasHumidity);
  available(w, "pressure", s.hasPressure);
  available(w, "light", s.hasLightSensor);
  available(w, "battery", s.hasBattery);
  w.endObject();
  w.member("events", true);
  w.key("eventTypes");
  w.beginArray();
  w.value("button");
  w.endArray();
  // Events observe physical buttons; they do not take ownership away from local navigation.
  w.member("buttonCapture", false);
  w.endObject();
  return out;
}

std::string serialButtonEventJson(const char* button, bool pressed, int64_t atMs) {
  std::string out;
  out.reserve(96);
  JsonWriter w(out);
  w.beginObject();
  w.member("event", "button");
  w.member("button", button);
  w.member("pressed", pressed);
  w.member("atMs", atMs);
  w.endObject();
  return out;
}

}
