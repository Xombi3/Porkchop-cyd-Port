// ============================================================
//  TFT_eSPI User_Setup.h for CYD / Elegoo CYD
//  Based on the OFFICIAL witnessmenow/ESP32-Cheap-Yellow-Display
//  config - this is the one that actually works.
//
//  INSTALL: Replace file at:
//    Documents\Arduino\libraries\TFT_eSPI\User_Setup.h
// ============================================================

// KEY DIFFERENCE from other configs:
// Uses ILI9341_2_DRIVER (alternative driver), NOT ILI9341_DRIVER
#define ILI9341_2_DRIVER

// TFT SPI pins (HSPI)
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1

// Backlight
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

// CRITICAL for CYD: TFT is on HSPI bus, not the default VSPI
#define USE_HSPI_PORT

// SPI speeds
#define SPI_FREQUENCY        55000000
#define SPI_READ_FREQUENCY   20000000
#define SPI_TOUCH_FREQUENCY   2500000

// Fonts
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
