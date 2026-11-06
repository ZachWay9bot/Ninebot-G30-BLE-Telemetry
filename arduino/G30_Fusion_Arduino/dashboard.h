#ifndef SCOOTER_DASHBOARD_H
#define SCOOTER_DASHBOARD_H

#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* LVGL 9.4; CYD 240 x 320 portrait. */
#define DASH_WIDTH 240
#define DASH_HEIGHT 320
#define DASH_FRAME_MS 33U
#define DASH_STALE_MS 1500U

#ifndef DASH_LOAD_FULL_CURRENT_A
#define DASH_LOAD_FULL_CURRENT_A 30.0f
#endif

typedef enum { DASH_ECO, DASH_DRIVE, DASH_SPORT } dash_mode_t;
typedef enum { DASH_BLUE, DASH_RED } dash_theme_t;

typedef struct {
    float speed_kmh;
    float battery_percent;
    float voltage_v;
    float battery_current_a;
    float esc_temp_c;
    float bms_temp_c;
    float trip_km;
    float odometer_km;
    float max_speed_kmh;
    bool connected;
    bool demo;
    bool headlight;
    bool headlight_valid;
    bool delta_active;
    uint16_t fault_code;
    dash_mode_t mode;
} dash_data_t;

dash_data_t dashboard_empty_data(void);
bool dashboard_create(lv_obj_t *parent);
void dashboard_destroy(void);
void dashboard_feed(const dash_data_t *data, uint32_t now_ms);
void dashboard_tick(uint32_t now_ms);
void dashboard_set_theme(dash_theme_t theme);
void dashboard_set_speed_max(unsigned kmh);
bool dashboard_is_live(uint32_t now_ms);
dash_data_t dashboard_demo_sample(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
