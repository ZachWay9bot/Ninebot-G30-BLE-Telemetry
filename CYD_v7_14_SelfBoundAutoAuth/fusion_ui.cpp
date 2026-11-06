#include "fusion_ui.h"

#include <Preferences.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <math.h>

#include "dashboard.h"
#include "ninebot_telemetry.h"

namespace {

static constexpr uint8_t TOUCH_CS   = 33;
static constexpr uint8_t TOUCH_IRQ  = 36;
static constexpr uint8_t TOUCH_CLK  = 25;
static constexpr uint8_t TOUCH_MISO = 39;
static constexpr uint8_t TOUCH_MOSI = 32;
static constexpr int TOUCH_Z_MIN = 350;
static constexpr float TOUCH_LOCK_KMH = 3.0f;

static constexpr uint32_t PREF_MAGIC = 0x46554931UL; // FUI1
static constexpr uint32_t CAL_MAGIC  = 0x43414C32UL; // CAL2

struct UiPrefs {
    uint32_t magic;
    uint8_t theme;
    uint8_t brightness;
    uint8_t touch_lock;
    uint8_t reserved;
    uint16_t speed_max;
};

struct TouchCal {
    uint32_t magic;
    int32_t ax0;
    int32_t ax1;
    int32_t ay0;
    int32_t ay1;
    uint8_t swap;
    uint8_t rotation;
    uint16_t reserved;
};

static Preferences prefs;
static UiPrefs ui = {PREF_MAGIC, (uint8_t)DASH_RED, 100, 1, 0, 60};
static TouchCal cal = {};
static uint8_t display_rotation = 0;
static fusion_backlight_cb_t backlight_cb = nullptr;

static SPIClass touch_spi(VSPI);
static XPT2046_Touchscreen touch(TOUCH_CS, TOUCH_IRQ);
static lv_indev_t *touch_indev = nullptr;

static lv_obj_t *host = nullptr;
static lv_obj_t *settings_button = nullptr;
static lv_obj_t *overlay = nullptr;
static lv_obj_t *settings_page = nullptr;
static lv_obj_t *info_page = nullptr;
static lv_obj_t *cal_page = nullptr;

static lv_obj_t *speed_value = nullptr;
static lv_obj_t *brightness_value = nullptr;
static lv_obj_t *touch_lock_switch = nullptr;
static lv_obj_t *info_text = nullptr;
static lv_obj_t *cal_text = nullptr;
static lv_obj_t *cal_target = nullptr;

static float current_speed = 0.0f;
static bool overlay_open = false;
static bool calibrating = false;
static bool calibration_return_to_settings = false;

static bool cal_pressed = false;
static int64_t cal_sum_x = 0;
static int64_t cal_sum_y = 0;
static uint16_t cal_samples = 0;
static int32_t cal_x[3] = {};
static int32_t cal_y[3] = {};
static uint8_t cal_step = 0;

static lv_color_t col(uint32_t rgb) {
    return lv_color_hex(rgb);
}

static void style_panel(lv_obj_t *o, uint32_t bg, uint32_t border = 0x25384a) {
    lv_obj_set_style_bg_color(o, col(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, col(border), 0);
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_radius(o, 12, 0);
}

static void style_button(lv_obj_t *o, uint32_t bg = 0x172638) {
    lv_obj_set_style_bg_color(o, col(bg), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_text_color(o, col(0xf3f8ff), 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y,
                            const lv_font_t *font = &lv_font_montserrat_14,
                            uint32_t color = 0xf3f8ff) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col(color), 0);
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_size(b, w, h);
    style_button(b);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static void save_ui() {
    if (!prefs.begin("dash_ui", false)) return;
    prefs.putBytes("prefs", &ui, sizeof(ui));
    prefs.end();
}

static void save_cal() {
    if (!prefs.begin("dash_ui", false)) return;
    prefs.putBytes("cal", &cal, sizeof(cal));
    prefs.end();
}

static void load_storage() {
    if (!prefs.begin("dash_ui", true)) return;

    if (prefs.getBytesLength("prefs") == sizeof(UiPrefs)) {
        UiPrefs tmp = {};
        prefs.getBytes("prefs", &tmp, sizeof(tmp));
        if (tmp.magic == PREF_MAGIC &&
            tmp.theme <= (uint8_t)DASH_RED &&
            tmp.brightness >= 10 && tmp.brightness <= 100 &&
            tmp.speed_max >= 20 && tmp.speed_max <= 160 &&
            (tmp.speed_max % 10) == 0) {
            ui = tmp;
        }
    }

    if (prefs.getBytesLength("cal") == sizeof(TouchCal)) {
        TouchCal tmp = {};
        prefs.getBytes("cal", &tmp, sizeof(tmp));
        if (tmp.magic == CAL_MAGIC && tmp.rotation == display_rotation &&
            tmp.ax0 != tmp.ax1 && tmp.ay0 != tmp.ay1) {
            cal = tmp;
        }
    }
    prefs.end();
}

static bool map_touch(int32_t rx, int32_t ry, int16_t &sx, int16_t &sy) {
    if (cal.magic != CAL_MAGIC || cal.ax0 == cal.ax1 || cal.ay0 == cal.ay1) return false;

    const int32_t a = cal.swap ? ry : rx;
    const int32_t b = cal.swap ? rx : ry;

    int32_t x = 20 + (int64_t)(a - cal.ax0) * 200 / (cal.ax1 - cal.ax0);
    int32_t y = 20 + (int64_t)(b - cal.ay0) * 280 / (cal.ay1 - cal.ay0);

    if (x < 0) x = 0;
    if (x > 239) x = 239;
    if (y < 0) y = 0;
    if (y > 319) y = 319;

    sx = (int16_t)x;
    sy = (int16_t)y;
    return true;
}

static bool touch_is_locked() {
    return ui.touch_lock && isfinite(current_speed) && current_speed > TOUCH_LOCK_KMH;
}

static void touch_read(lv_indev_t *, lv_indev_data_t *data) {
    data->state = LV_INDEV_STATE_RELEASED;

    if (calibrating || touch_is_locked()) return;
    if (!touch.touched()) return;

    TS_Point p = touch.getPoint();
    if (p.z <= TOUCH_Z_MIN) return;

    int16_t x = 0, y = 0;
    if (!map_touch(p.x, p.y, x, y)) return;

    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

static void refresh_info() {
    if (!info_text) return;

    const char *target = ninebotTelemetryHasAutoConnectTarget()
        ? ninebotTelemetryAutoConnectTarget() : "(not bound)";
    const char *app = ninebotTelemetryHasSegwayAppKey() ? "stored" : "not set";

    char buf[320];
    snprintf(buf, sizeof(buf),
             "G30 fusion backend\n"
             "BLE/Crypto: v7.14\n"
             "Self-Bound AutoAuth: enabled\n"
             "Target: %s\n"
             "Segway app-key: %s\n"
             "DPC 0x76: backend managed\n"
             "UI: LVGL 9.4 + partial redraw\n"
             "Touch lock: %s above 3 km/h",
             target, app, ui.touch_lock ? "ON" : "OFF");
    lv_label_set_text(info_text, buf);
}

static void show_page(lv_obj_t *page) {
    if (!overlay) return;
    lv_obj_add_flag(settings_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(info_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(cal_page, LV_OBJ_FLAG_HIDDEN);
    if (page) lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
}

static void close_overlay_internal() {
    if (!overlay) return;
    calibrating = false;
    overlay_open = false;
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    if (settings_button) lv_obj_remove_flag(settings_button, LV_OBJ_FLAG_HIDDEN);
}

static void open_settings_internal() {
    if (!overlay || touch_is_locked()) return;
    calibrating = false;
    overlay_open = true;
    show_page(settings_page);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    lv_obj_add_flag(settings_button, LV_OBJ_FLAG_HIDDEN);
}

static void close_event(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) close_overlay_internal();
}

static void info_event(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    refresh_info();
    show_page(info_page);
}

static void info_back_event(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) show_page(settings_page);
}

static void theme_event(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    dash_theme_t t = (dash_theme_t)(uintptr_t)lv_event_get_user_data(e);
    ui.theme = (uint8_t)t;
    dashboard_set_theme(t);
    save_ui();
}

static void speed_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    int tens = (int)lv_slider_get_value(slider);
    unsigned kmh = (unsigned)(tens * 10);

    if (speed_value) lv_label_set_text_fmt(speed_value, "%u km/h", kmh);
    if (code == LV_EVENT_RELEASED) {
        ui.speed_max = (uint16_t)kmh;
        dashboard_set_speed_max(kmh);
        save_ui();
    }
}

static void brightness_event(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    uint8_t value = (uint8_t)lv_slider_get_value(slider);
    if (brightness_value) lv_label_set_text_fmt(brightness_value, "%u%%", value);
    if (backlight_cb) backlight_cb(value);
    if (code == LV_EVENT_RELEASED) {
        ui.brightness = value;
        save_ui();
    }
}

static void lock_event(lv_event_t *e) {
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED) return;
    lv_obj_t *sw = (lv_obj_t *)lv_event_get_target(e);
    ui.touch_lock = lv_obj_has_state(sw, LV_STATE_CHECKED) ? 1 : 0;
    save_ui();
}

static void update_cal_target() {
    static const int tx[3] = {20, 220, 20};
    static const int ty[3] = {20, 20, 300};
    if (!cal_target || cal_step >= 3) return;
    lv_obj_set_pos(cal_target, tx[cal_step] - 9, ty[cal_step] - 9);
    if (cal_text) {
        lv_label_set_text_fmt(cal_text, "Touch calibration %u/3\nTap the cyan target",
                              (unsigned)(cal_step + 1));
    }
}

static void finish_calibration() {
    const int32_t hdx = abs(cal_x[1] - cal_x[0]);
    const int32_t hdy = abs(cal_y[1] - cal_y[0]);
    const bool swap = hdy > hdx;

    cal.magic = CAL_MAGIC;
    cal.swap = swap ? 1 : 0;
    cal.rotation = display_rotation;

    if (!swap) {
        cal.ax0 = cal_x[0];
        cal.ax1 = cal_x[1];
        cal.ay0 = cal_y[0];
        cal.ay1 = cal_y[2];
    } else {
        cal.ax0 = cal_y[0];
        cal.ax1 = cal_y[1];
        cal.ay0 = cal_x[0];
        cal.ay1 = cal_x[2];
    }

    if (abs(cal.ax1 - cal.ax0) < 250 || abs(cal.ay1 - cal.ay0) < 250) {
        cal.magic = 0;
        cal_step = 0;
        if (cal_text) lv_label_set_text(cal_text, "Calibration failed\nTry again");
        update_cal_target();
        Serial.println("[FUSION-UI] calibration rejected: raw span too small.");
        return;
    }

    save_cal();
    Serial.printf("[FUSION-UI] calibration OK | swap=%u ax=%ld..%ld ay=%ld..%ld\n",
                  (unsigned)cal.swap, (long)cal.ax0, (long)cal.ax1,
                  (long)cal.ay0, (long)cal.ay1);
    calibrating = false;

    if (calibration_return_to_settings) {
        show_page(settings_page);
    } else {
        close_overlay_internal();
    }
}

static void calibration_tick() {
    if (!calibrating || cal_step >= 3) return;

    bool pressed = false;
    int32_t rx = 0, ry = 0;
    if (touch.touched()) {
        TS_Point p = touch.getPoint();
        if (p.z > TOUCH_Z_MIN) {
            pressed = true;
            rx = p.x;
            ry = p.y;
        }
    }

    if (pressed) {
        if (!cal_pressed) {
            cal_pressed = true;
            cal_sum_x = cal_sum_y = 0;
            cal_samples = 0;
        }
        if (cal_samples < 128) {
            cal_sum_x += rx;
            cal_sum_y += ry;
            cal_samples++;
        }
        return;
    }

    if (cal_pressed) {
        cal_pressed = false;
        if (cal_samples >= 2) {
            cal_x[cal_step] = (int32_t)(cal_sum_x / cal_samples);
            cal_y[cal_step] = (int32_t)(cal_sum_y / cal_samples);
            cal_step++;
            if (cal_step >= 3) finish_calibration();
            else update_cal_target();
        }
    }
}

static void start_calibration(bool return_to_settings) {
    calibration_return_to_settings = return_to_settings;
    overlay_open = true;
    calibrating = true;
    cal_step = 0;
    cal_pressed = false;
    cal_sum_x = cal_sum_y = 0;
    cal_samples = 0;
    show_page(cal_page);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(overlay);
    lv_obj_add_flag(settings_button, LV_OBJ_FLAG_HIDDEN);
    update_cal_target();
}

static void calibration_event(lv_event_t *e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) start_calibration(true);
}

static void build_settings() {
    settings_page = lv_obj_create(overlay);
    lv_obj_remove_style_all(settings_page);
    lv_obj_set_size(settings_page, 240, 320);
    lv_obj_set_style_bg_color(settings_page, col(0x08111a), 0);
    lv_obj_set_style_bg_opa(settings_page, LV_OPA_COVER, 0);

    make_label(settings_page, "G30 FUSION", 12, 10, &lv_font_montserrat_20, 0x20caff);
    make_label(settings_page, "LVGL settings", 13, 34, &lv_font_montserrat_14, 0x9aaebe);

    lv_obj_t *close = make_button(settings_page, "X", 198, 8, 32, 30);
    lv_obj_add_event_cb(close, close_event, LV_EVENT_CLICKED, nullptr);

    make_label(settings_page, "Theme", 12, 70);
    lv_obj_t *red = make_button(settings_page, "RED", 12, 91, 96, 34);
    lv_obj_t *blue = make_button(settings_page, "BLUE", 132, 91, 96, 34);
    lv_obj_add_event_cb(red, theme_event, LV_EVENT_CLICKED, (void *)(uintptr_t)DASH_RED);
    lv_obj_add_event_cb(blue, theme_event, LV_EVENT_CLICKED, (void *)(uintptr_t)DASH_BLUE);

    make_label(settings_page, "Speed scale", 12, 139);
    speed_value = make_label(settings_page, "", 158, 139, &lv_font_montserrat_14, 0x20caff);
    lv_obj_t *speed = lv_slider_create(settings_page);
    lv_obj_set_pos(speed, 12, 162);
    lv_obj_set_size(speed, 216, 16);
    lv_slider_set_range(speed, 2, 16);
    lv_slider_set_value(speed, ui.speed_max / 10, LV_ANIM_OFF);
    lv_obj_add_event_cb(speed, speed_event, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(speed, speed_event, LV_EVENT_RELEASED, nullptr);
    lv_label_set_text_fmt(speed_value, "%u km/h", (unsigned)ui.speed_max);

    make_label(settings_page, "Brightness", 12, 193);
    brightness_value = make_label(settings_page, "", 180, 193, &lv_font_montserrat_14, 0x20caff);
    lv_obj_t *brightness = lv_slider_create(settings_page);
    lv_obj_set_pos(brightness, 12, 216);
    lv_obj_set_size(brightness, 216, 16);
    lv_slider_set_range(brightness, 10, 100);
    lv_slider_set_value(brightness, ui.brightness, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness, brightness_event, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(brightness, brightness_event, LV_EVENT_RELEASED, nullptr);
    lv_label_set_text_fmt(brightness_value, "%u%%", ui.brightness);

    make_label(settings_page, "Touch lock >3 km/h", 12, 247);
    touch_lock_switch = lv_switch_create(settings_page);
    lv_obj_set_pos(touch_lock_switch, 184, 242);
    if (ui.touch_lock) lv_obj_add_state(touch_lock_switch, LV_STATE_CHECKED);
    lv_obj_add_event_cb(touch_lock_switch, lock_event, LV_EVENT_VALUE_CHANGED, nullptr);

    lv_obj_t *cal_btn = make_button(settings_page, "CAL", 12, 280, 62, 30);
    lv_obj_t *info_btn = make_button(settings_page, "INFO", 86, 280, 68, 30);
    lv_obj_t *done_btn = make_button(settings_page, "DONE", 166, 280, 62, 30);
    lv_obj_add_event_cb(cal_btn, calibration_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(info_btn, info_event, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(done_btn, close_event, LV_EVENT_CLICKED, nullptr);
}

static void build_info() {
    info_page = lv_obj_create(overlay);
    lv_obj_remove_style_all(info_page);
    lv_obj_set_size(info_page, 240, 320);
    lv_obj_set_style_bg_color(info_page, col(0x08111a), 0);
    lv_obj_set_style_bg_opa(info_page, LV_OPA_COVER, 0);

    make_label(info_page, "SYSTEM INFO", 12, 10, &lv_font_montserrat_20, 0x20caff);
    lv_obj_t *back = make_button(info_page, "BACK", 168, 8, 60, 30);
    lv_obj_add_event_cb(back, info_back_event, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *card = lv_obj_create(info_page);
    lv_obj_set_pos(card, 10, 52);
    lv_obj_set_size(card, 220, 238);
    style_panel(card, 0x101c28);

    info_text = lv_label_create(card);
    lv_obj_set_pos(info_text, 8, 8);
    lv_obj_set_width(info_text, 202);
    lv_obj_set_style_text_font(info_text, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(info_text, col(0xf3f8ff), 0);
    lv_label_set_long_mode(info_text, LV_LABEL_LONG_WRAP);
    refresh_info();
}

static void build_calibration() {
    cal_page = lv_obj_create(overlay);
    lv_obj_remove_style_all(cal_page);
    lv_obj_set_size(cal_page, 240, 320);
    lv_obj_set_style_bg_color(cal_page, col(0x050910), 0);
    lv_obj_set_style_bg_opa(cal_page, LV_OPA_COVER, 0);

    cal_text = make_label(cal_page, "Touch calibration", 42, 135,
                          &lv_font_montserrat_16, 0xf3f8ff);
    lv_obj_set_width(cal_text, 170);
    lv_obj_set_style_text_align(cal_text, LV_TEXT_ALIGN_CENTER, 0);

    cal_target = lv_obj_create(cal_page);
    lv_obj_remove_style_all(cal_target);
    lv_obj_set_size(cal_target, 18, 18);
    lv_obj_set_style_radius(cal_target, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cal_target, col(0x20caff), 0);
    lv_obj_set_style_bg_opa(cal_target, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cal_target, col(0xf3f8ff), 0);
    lv_obj_set_style_border_width(cal_target, 2, 0);
}

static void build_shell() {
    settings_button = make_button(host, "SET", 4, 4, 42, 24);
    lv_obj_set_style_bg_color(settings_button, col(0x172638), 0);
    lv_obj_set_style_bg_opa(settings_button, LV_OPA_80, 0);
    lv_obj_add_event_cb(settings_button, [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_CLICKED) open_settings_internal();
    }, LV_EVENT_CLICKED, nullptr);

    overlay = lv_obj_create(host);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, 240, 320);
    lv_obj_set_style_bg_color(overlay, col(0x050910), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

    build_settings();
    build_info();
    build_calibration();
    show_page(settings_page);
}

} // namespace

bool fusionUiBegin(lv_obj_t *parent, uint8_t displayRotation, fusion_backlight_cb_t backlightCb) {
    if (!parent) return false;
    host = parent;
    display_rotation = displayRotation;
    backlight_cb = backlightCb;

    load_storage();
    dashboard_set_theme((dash_theme_t)ui.theme);
    dashboard_set_speed_max(ui.speed_max);
    if (backlight_cb) backlight_cb(ui.brightness);

    touch_spi.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    if (!touch.begin(touch_spi)) {
        Serial.println("[FUSION-UI] Touch controller not detected.");
        return false;
    }
    touch.setRotation(0);

    touch_indev = lv_indev_create();
    lv_indev_set_type(touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch_indev, touch_read);

    build_shell();

    if (cal.magic != CAL_MAGIC) {
        Serial.println("[FUSION-UI] No valid touch calibration; starting calibration.");
        start_calibration(false);
    }

    Serial.printf("[FUSION-UI] ready | theme=%u scale=%u brightness=%u lock=%u\n",
                  (unsigned)ui.theme, (unsigned)ui.speed_max,
                  (unsigned)ui.brightness, (unsigned)ui.touch_lock);
    return true;
}

void fusionUiTick(uint32_t, float speedKmh) {
    current_speed = speedKmh;

    if (touch_is_locked() && overlay_open && !calibrating) {
        close_overlay_internal();
    }

    calibration_tick();
}

void fusionUiOpenSettings() {
    open_settings_internal();
}

void fusionUiStartCalibration() {
    if (overlay) start_calibration(false);
}

void fusionUiCloseOverlay() {
    close_overlay_internal();
}

bool fusionUiOverlayOpen() {
    return overlay_open;
}

uint8_t fusionUiBrightness() {
    return ui.brightness;
}

bool fusionUiTouchLocked() {
    return ui.touch_lock != 0;
}
