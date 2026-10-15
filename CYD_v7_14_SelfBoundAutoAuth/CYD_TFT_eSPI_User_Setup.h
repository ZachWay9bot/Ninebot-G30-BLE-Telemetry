// TFT_eSPI setup for the common ESP32-2432S028R "Cheap Yellow Display" (CYD)
// Copy these definitions into TFT_eSPI/User_Setup.h OR include this file from
// TFT_eSPI/User_Setup_Select.h. The sketch itself cannot reliably override
// TFT_eSPI's separately compiled library configuration.

#define USER_SETUP_INFO "ESP32-2432S028R CYD ILI9341"
#define ILI9341_DRIVER
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1

#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH

#define TFT_RGB_ORDER TFT_BGR

// 40 MHz is a conservative stable default for the common CYD.
#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY 2500000

// Touch is not used by this dashboard, but the common CYD uses CS 33.
#define TOUCH_CS 33

// TFT_eSPI only needs its basic font enabled here; LVGL draws dashboard text.
#define LOAD_GLCD
