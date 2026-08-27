//                            USER DEFINED SETTINGS
//   TFT_eSPI configuration for the ESP32-2432S028R "Cheap Yellow Display" (CYD)
//   Matched to PorkChop_CYD pin map. Place this file in:
//      Documents/Arduino/libraries/TFT_eSPI/User_Setup.h   (replace existing)

#define USER_SETUP_INFO "CYD_ESP32-2432S028R"

// ##################################################################################
// Section 1. Display driver
// ##################################################################################
#define ILI9341_2_DRIVER          // CYD uses the ILI9341 controller (the "_2" variant
                                  // is the more reliable init for the 2432S028R)

#define TFT_WIDTH  240
#define TFT_HEIGHT 320

// Most CYDs render correct colors as-is. If reds/blues look swapped on YOUR unit,
// uncomment the next line:
//#define TFT_RGB_ORDER TFT_BGR

// Some CYD batches have inverted colors. If the whole screen looks like a photo
// negative, uncomment ONE of these (try INVERSION_ON first):
//#define TFT_INVERSION_ON
//#define TFT_INVERSION_OFF

// ##################################################################################
// Section 2. Pins — CYD display is on HSPI
// ##################################################################################
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC    2
#define TFT_RST  -1               // CYD display RST tied to the ESP32 EN/reset line
#define TFT_BL   21               // Backlight
#define TFT_BACKLIGHT_ON HIGH     // Backlight is active HIGH on the CYD

// XPT2046 touch shares pins on the CYD's second SPI bus. The Porkchop sketch
// drives touch itself via the XPT2046_Touchscreen library, so TOUCH_CS is left
// out here on purpose — do NOT define it, or TFT_eSPI will fight the sketch.

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
#define SPI_FREQUENCY       55000000   // CYD ILI9341 runs reliably at 55MHz
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY  2500000
