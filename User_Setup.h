//                            USER DEFINED SETTINGS
//   TFT_eSPI configuration for the Waveshare ESP32-C5-Touch-LCD-3.5
//   (ST7796 320x480). Ported from the CYD (ILI9341) build. Place this file in:
//      Documents/Arduino/libraries/TFT_eSPI/User_Setup.h   (replace existing)
//
//   Pin map verified on this board via the Marauder C5 port. LCD reset and
//   backlight are driven by the on-board I/O expander (0x24), NOT a GPIO, so
//   TFT_RST / TFT_BL are left at -1 here; bring-up lives in ws_c5_exio.h.
//   FT6336 touch is I2C-only, so TOUCH_CS is unused (see ft6336.h).

#define USER_SETUP_INFO "WS_ESP32-C5-Touch-LCD-3.5"

// ##################################################################################
// Section 1. Display driver
// ##################################################################################
#define ST7796_DRIVER

#define TFT_WIDTH  320
#define TFT_HEIGHT 480

// The Waveshare C5 3.5 panel is a negative display — colors are inverted
// without this. (Verified on the Marauder port of this same board.)
#define TFT_INVERSION_ON

// ##################################################################################
// Section 2. Pins — C5 display SPI
// ##################################################################################
#define TFT_MISO  2
#define TFT_MOSI  7
#define TFT_SCLK  6
#define TFT_CS    8
#define TFT_DC    5
#define TFT_RST  -1               // LCD reset is on the I/O expander (EXIO 1)
#define TFT_BL   -1               // Backlight is on the I/O expander PWM register

// FT6336 capacitive touch is I2C-only (SDA 27 / SCL 26, addr 0x38). The sketch
// drives touch itself via ft6336.h, so do NOT define TOUCH_CS here.

// ##################################################################################
// Section 3. Fonts
// ##################################################################################
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT

// ##################################################################################
// Section 4. SPI speeds
// ##################################################################################
// Conservative for first bring-up; Waveshare runs the panel at 60MHz and this
// can be raised once verified on-device.
#define SPI_FREQUENCY       60000000
#define SPI_READ_FREQUENCY  16000000
