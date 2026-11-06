#ifndef G30_FUSION_UI_H
#define G30_FUSION_UI_H

#include <Arduino.h>
#include <lvgl.h>

typedef void (*fusion_backlight_cb_t)(uint8_t percent);

/*
 * OpenDash-inspired UI layer for the v7.14 dashboard.
 *
 * The Ninebot BLE/Crypto/AutoAuth backend is deliberately not owned here.
 * This module only adds touch input, calibration, settings and an info overlay
 * on top of the existing LVGL dashboard.
 */
bool fusionUiBegin(lv_obj_t *parent, uint8_t displayRotation, fusion_backlight_cb_t backlightCb);
void fusionUiTick(uint32_t nowMs, float speedKmh);
void fusionUiOpenSettings();
void fusionUiStartCalibration();
void fusionUiCloseOverlay();
bool fusionUiOverlayOpen();
uint8_t fusionUiBrightness();
bool fusionUiTouchLocked();

#endif
