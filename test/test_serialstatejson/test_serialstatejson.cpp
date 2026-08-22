#include <unity.h>

#include "core/RuntimeState.h"
#include "core/api/SerialStateJson.h"

using namespace awtrix;

void setUp() {}
void tearDown() {}

static void test_sensor_query_reports_values_and_unavailable_inputs_as_null() {
  RuntimeState s;
  s.hasTemperature = true;
  s.temperatureC = 21.5f;
  s.hasHumidity = false;
  s.hasPressure = true;
  s.pressureHpa = 1013.2f;
  s.hasLightSensor = true;
  s.lightLevel = 42.5f;
  s.ldrRaw = 1234;
  s.hasBattery = false;

  TEST_ASSERT_EQUAL_STRING(
      "{\"temperature\":21.5,\"humidity\":null,\"pressureHpa\":1013.2,"
      "\"lightLevel\":42.5,\"ldrRaw\":1234,\"batteryPercent\":null,"
      "\"batteryVoltage\":null,\"batteryPinMillivolts\":null,\"lowBattery\":null}",
      api::serialSensorsJson(s).c_str());
}

static void test_button_query_uses_physical_state() {
  RuntimeState s;
  s.buttons = {true, false, true};
  TEST_ASSERT_EQUAL_STRING("{\"left\":true,\"select\":false,\"right\":true}",
                           api::serialButtonsJson(s).c_str());
}

static void test_capabilities_advertise_button_events_without_capture() {
  RuntimeState s;
  s.hasTemperature = true;
  s.hasHumidity = false;
  s.hasPressure = true;
  s.hasLightSensor = false;
  s.hasBattery = true;
  TEST_ASSERT_EQUAL_STRING(
      "{\"protocol\":2,\"queries\":[\"qry/capabilities\",\"qry/sensors\",\"qry/buttons\"],"
      "\"sensors\":{\"temperature\":true,\"humidity\":false,\"pressure\":true,"
      "\"light\":false,\"battery\":true},\"events\":true,\"eventTypes\":[\"button\"],"
      "\"buttonCapture\":false}",
      api::serialCapabilitiesJson(s).c_str());
}

static void test_button_event_reports_edge_and_monotonic_device_time() {
  TEST_ASSERT_EQUAL_STRING(
      "{\"event\":\"button\",\"button\":\"select\",\"pressed\":true,\"atMs\":12345}",
      api::serialButtonEventJson("select", true, 12345).c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sensor_query_reports_values_and_unavailable_inputs_as_null);
  RUN_TEST(test_button_query_uses_physical_state);
  RUN_TEST(test_capabilities_advertise_button_events_without_capture);
  RUN_TEST(test_button_event_reports_edge_and_monotonic_device_time);
  return UNITY_END();
}
