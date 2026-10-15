#ifndef NINEBOT_TELEMETRY_H
#define NINEBOT_TELEMETRY_H

#include <Arduino.h>

struct NinebotTelemetrySnapshot {
  float speed_kmh;
  float battery_percent;
  float voltage_v;
  float battery_current_a;
  float bms_temp_c;
  float odometer_km;
  int8_t ride_mode; // -1 unknown, 0=Drive/Normal, 1=Eco, 2=Sport
  bool headlight_on;
  bool headlight_valid; // passively decoded from ESC->dashboard 0x64 when available
  bool connected;
  bool ready;
  uint32_t last_update_ms;
  uint32_t sequence;
};

void ninebotTelemetryBegin();
void ninebotTelemetryLoop();
bool ninebotTelemetryGet(NinebotTelemetrySnapshot* out);

// Optional real Segway-app binding key (<SERIAL>_decrypt), 16 bytes / 32 hex chars.
// When present, v7.11 authenticates directly with 0x5D and skips 0x5C/power-button.
bool ninebotTelemetrySetSegwayAppKeyHex(const char* hex32);
void ninebotTelemetryClearSegwayAppKey();
bool ninebotTelemetryHasSegwayAppKey();

// AutoConnect target learned after the first successfully authenticated READY state.
void ninebotTelemetryClearAutoConnectTarget();
bool ninebotTelemetryHasAutoConnectTarget();
const char* ninebotTelemetryAutoConnectTarget();

#endif
