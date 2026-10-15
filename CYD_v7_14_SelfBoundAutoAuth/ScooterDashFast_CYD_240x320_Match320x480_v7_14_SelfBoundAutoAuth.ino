#include <Arduino.h>
#include <TFT_eSPI.h>
#include <math.h>
#include <esp_heap_caps.h>
#include "dashboard.h"
#include "ninebot_telemetry.h"

static constexpr bool DEMO_MODE = false;
static constexpr uint8_t DISPLAY_ROTATION = 0; // CYD portrait: logical 240 x 320. Use 2 if mounted upside-down.
static constexpr bool DISPLAY_INVERT = false;
static constexpr int BACKLIGHT_PIN = 21;       // Standard ESP32-2432S028R CYD backlight

static TFT_eSPI tft;
static uint8_t *draw_buffer = nullptr;
static size_t draw_buffer_bytes = 0;

static bool allocateDisplayBuffer() {
    // Same partial-buffer strategy as the fast v6.15 build.
    // At 240 px width, 8 RGB565 rows use only 3840 bytes.
    static const uint8_t row_options[] = { 8, 4, 2, 1 };
    for (uint8_t rows : row_options) {
        const size_t bytes = (size_t)DASH_WIDTH * rows * 2U;
        draw_buffer = static_cast<uint8_t *>(
            heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (draw_buffer) {
            draw_buffer_bytes = bytes;
            Serial.printf("LVGL draw buffer: %u rows / %u bytes | free heap=%u\n",
                          (unsigned)rows, (unsigned)bytes, (unsigned)ESP.getFreeHeap());
            return true;
        }
    }
    Serial.println("FEHLER: Kein LVGL draw buffer verfuegbar.");
    return false;
}

static uint32_t pixels_sent, flush_time_us, frames_sent;
static bool performance_log;
static float session_trip_base_km = NAN;
static constexpr float DELTA_SWITCH_KMH = 30.0f;
static bool delta_mode_active = false;

static uint32_t tick_ms() { return millis(); }

static void display_flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels) {
    const uint32_t width = area->x2 - area->x1 + 1;
    const uint32_t height = area->y2 - area->y1 + 1;
    const uint32_t started = micros();

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, width, height);
    tft.pushColors(reinterpret_cast<uint16_t *>(pixels), width * height, true);
    tft.endWrite();

    pixels_sent += width * height;
    flush_time_us += micros() - started;
    if (lv_display_flush_is_last(display)) frames_sent++;
    lv_display_flush_ready(display);
}

static void feedNinebotToDashboard(uint32_t now) {
    static uint32_t last_feed = 0;
    if ((uint32_t)(now - last_feed) < 100) return;
    last_feed = now;

    NinebotTelemetrySnapshot src;
    if (!ninebotTelemetryGet(&src)) return;

    dash_data_t data = dashboard_empty_data();
    const bool fresh = src.last_update_ms != 0 &&
                       (uint32_t)(now - src.last_update_ms) <= 5000U;
    data.connected = src.connected && src.ready && fresh;
    data.demo = false;
    data.speed_kmh = src.speed_kmh;
    data.battery_percent = src.battery_percent;
    data.voltage_v = src.voltage_v;
    data.battery_current_a = src.battery_current_a;
    data.bms_temp_c = src.bms_temp_c;
    data.odometer_km = src.odometer_km;
    data.esc_temp_c = NAN;

    if (isfinite(src.odometer_km)) {
        if (!isfinite(session_trip_base_km)) session_trip_base_km = src.odometer_km;
        data.trip_km = src.odometer_km - session_trip_base_km;
        if (data.trip_km < 0) data.trip_km = 0;
    }

    const bool new_delta = data.connected && isfinite(src.speed_kmh) &&
                           src.speed_kmh > DELTA_SWITCH_KMH;
    if (new_delta != delta_mode_active) {
        delta_mode_active = new_delta;
        Serial.printf("RELAY:%d\n", delta_mode_active ? 1 : 0);
        Serial.printf("[DELTA] %s at %.1f km/h\n",
                      delta_mode_active ? "ON" : "OFF",
                      isfinite(src.speed_kmh) ? src.speed_kmh : 0.0f);
    }
    data.delta_active = delta_mode_active;
    data.max_speed_kmh = NAN;

    data.headlight = src.headlight_on;
    data.headlight_valid = src.headlight_valid;
    data.fault_code = 0;
    if (src.ride_mode == 1) data.mode = DASH_ECO;
    else if (src.ride_mode == 2) data.mode = DASH_SPORT;
    else data.mode = DASH_DRIVE;

    dashboard_feed(&data, now);
}

void setup() {
    Serial.begin(115200);
    delay(1200);
    Serial.println();
    Serial.println("[BOOT 1] CYD 240x320 portrait MATCH v7.14");
    Serial.printf("[BOOT 1] free heap=%u\n", (unsigned)ESP.getFreeHeap());

    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, HIGH);

    Serial.println("[BOOT 2] BLE init");
    ninebotTelemetryBegin();
    Serial.println("[BOOT 3] BLE init done");
    delay(50);

    Serial.println("[BOOT 4] TFT init");
    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.invertDisplay(DISPLAY_INVERT);
    tft.fillScreen(TFT_BLACK);

    Serial.println("[BOOT 5] LVGL init");
    lv_init();
    lv_tick_set_cb(tick_ms);

    Serial.println("[BOOT 6] draw buffer");
    if (!allocateDisplayBuffer()) while (true) delay(1000);

    lv_display_t *display = lv_display_create(DASH_WIDTH, DASH_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display, display_flush);
    lv_display_set_buffers(display, draw_buffer, nullptr, draw_buffer_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x050910), 0);

    Serial.println("[BOOT 7] dashboard create");
    if (!dashboard_create(lv_screen_active())) Serial.println("Dashboard creation failed");
    dashboard_tick(millis());
    lv_timer_handler();

    Serial.println("[BOOT 8] setup complete");
    Serial.println("ScooterDash CYD 240x320 PORTRAIT v7.14 SelfBound AutoAuth + AutoConnect + FastTelemetry + DPC | LVGL 9.4");
}

void loop() {
    static uint32_t last_demo_feed = 0;
    static uint32_t last_log = 0;

    ninebotTelemetryLoop();
    uint32_t now = millis();

    if (DEMO_MODE && (uint32_t)(now - last_demo_feed) >= 100) {
        last_demo_feed = now;
        dash_data_t data = dashboard_demo_sample(now);
        dashboard_feed(&data, now);
    } else if (!DEMO_MODE) {
        feedNinebotToDashboard(now);
    }

    static String serialLine;
    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            serialLine.trim();
            if (serialLine.length() == 1) {
                char cmd = serialLine.charAt(0);
                if (cmd == 'b') dashboard_set_theme(DASH_BLUE);
                if (cmd == 'r') dashboard_set_theme(DASH_RED);
                if (cmd == 'p') performance_log = !performance_log;
            } else if (serialLine.equalsIgnoreCase("TARGET?")) {
                if (ninebotTelemetryHasAutoConnectTarget()) {
                    Serial.print("[AUTOCONNECT] Ziel: ");
                    Serial.println(ninebotTelemetryAutoConnectTarget());
                } else {
                    Serial.println("[AUTOCONNECT] Kein Ziel gebunden.");
                }
            } else if (serialLine.equalsIgnoreCase("TARGET=CLEAR")) {
                ninebotTelemetryClearAutoConnectTarget();
            } else if (serialLine.equalsIgnoreCase("APPKEY?")) {
                Serial.println(ninebotTelemetryHasSegwayAppKey()
                    ? "[AUTH-APP] Segway-App-Key ist gespeichert."
                    : "[AUTH-APP] Kein Segway-App-Key gespeichert.");
            } else if (serialLine.equalsIgnoreCase("APPKEY=CLEAR")) {
                ninebotTelemetryClearSegwayAppKey();
            } else if (serialLine.startsWith("APPKEY=") || serialLine.startsWith("appkey=")) {
                String key = serialLine.substring(7);
                key.trim();
                if (ninebotTelemetrySetSegwayAppKeyHex(key.c_str())) {
                    Serial.println("[AUTH-APP] OK. ESP32 neu starten; danach wird 0x5C/Powerbutton uebersprungen.");
                } else {
                    Serial.println("[AUTH-APP] Ungueltig. Erwartet: APPKEY=<32 Hex-Zeichen>");
                }
            } else if (serialLine.length()) {
                Serial.println("Serial: b / r / p / TARGET? / TARGET=CLEAR / APPKEY? / APPKEY=<32hex> / APPKEY=CLEAR");
            }
            serialLine = "";
        } else if (serialLine.length() < 96) {
            serialLine += c;
        }
    }

    dashboard_tick(now);
    uint32_t wait = lv_timer_handler();
    now = millis();

    if ((uint32_t)(now - last_log) >= 1000) {
        uint32_t elapsed = now - last_log;
        if (performance_log) {
            Serial.printf("updates/s=%.1f | pixels/s=%lu | SPI busy=%.1f%% | heap=%u\n",
                          frames_sent * 1000.0f / elapsed,
                          (unsigned long)(pixels_sent * 1000ULL / elapsed),
                          flush_time_us / (elapsed * 10.0f),
                          (unsigned)ESP.getFreeHeap());
        }
        pixels_sent = flush_time_us = frames_sent = 0;
        last_log = now;
    }

    delay(wait < 5 ? 1 : 5);
}
