```
 ██▓███   ▒█████   ██▀███   ██ ▄█▀ ▄████▄   ██░ ██  ▒█████   ██▓███  
▓██░  ██▒▒██▒  ██▒▓██ ▒ ██▒ ██▄█▒ ▒██▀ ▀█  ▓██░ ██▒▒██▒  ██▒▓██░  ██▒
▓██░ ██▓▒▒██░  ██▒▓██ ░▄█ ▒▓███▄░ ▒▓█    ▄ ▒██▀▀██░▒██░  ██▒▓██░ ██▓▒
▒██▄█▓▒ ▒▒██   ██░▒██▀▀█▄  ▓██ █▄ ▒▓▓▄ ▄██▒░▓█ ░██ ▒██   ██░▒██▄█▓▒ ▒
▒██▒ ░  ░░ ████▓▒░░██▓ ▒██▒▒██▒ █▄▒ ▓███▀ ░░▓█▒░██▓░ ████▓▒░▒██▒ ░  ░
▒▓▒░ ░  ░░ ▒░▒░▒░ ░ ▒▓ ░▒▓░▒ ▒▒ ▓▒░ ░▒ ▒  ░ ▒ ░░▒░▒░ ▒░▒░▒░ ▒▓▒░ ░  ░
░▒ ░       ░ ▒ ▒░   ░▒ ░ ▒░░ ░▒ ▒░  ░  ▒    ▒ ░▒░ ░  ░ ▒ ▒░ ░▒ ░     
░░       ░ ░ ░ ▒    ░░   ░ ░ ░░ ░ ░         ░  ░░ ░░ ░ ░ ▒  ░░       
             ░ ░     ░     ░  ░   ░ ░       ░  ░  ░    ░ ░           
                                  ░                                  
                     [ CYD EDITION — ESP32-2432S028R ]
```

**Original project by [0ct0sec](https://github.com/0ct0sec/M5PORKCHOP)**
**CYD port created by xom**

FLASH AVAILABLE AT https://espterminator.com/

---

--[ Contents

    1  - What is this
    2  - Credits
    3  - Hardware
    4  - Differences from M5PORKCHOP
    5  - Installation
         5.1 - Requirements
         5.2 - Arduino IDE Setup
         5.3 - Library Dependencies
         5.4 - API Keys
         5.5 - Flashing
    6  - Pin Map
    7  - SD Card Setup
    8  - Modes
         8.1  - IDLE / Home
         8.2  - OINK MODE
         8.3  - DO NO HAM
         8.4  - SGT WARHOG
         8.5  - SPECTRUM
         8.6  - BACON MODE
         8.7  - PIGGY BLUES
         8.8  - PORK PATROL
         8.9  - SWINE STATS
         8.10 - CAPTURES
         8.11 - BOUNTY
         8.12 - BOAR BROS
         8.13 - CHALLENGES
         8.14 - ACHIEVEMENTS
         8.15 - UNLOCKABLES
         8.16 - DIAGNOSTICS
         8.17 - SETTINGS
         8.18 - WEB REMOTE
    9  - Navigation
    10 - GPS
    11 - SD Card Saves
    12 - WiGLE Upload
    13 - WPA-SEC Upload
    14 - Speaker & Sound
    15 - XP & Leveling
    16 - Challenges
    17 - Achievements
    18 - Change Log


--[ 1 - What is this

    PORKCHOP CYD is a WiFi security research tool ported from the original
    M5PORKCHOP project (M5Stack Cardputer) to the ESP32-2432S028R "Cheap
    Yellow Display" — a $15 ESP32 board with a 2.8" ILI9341 TFT, resistive
    touchscreen, SD card slot, speaker amp, and UART connector.

    It does 802.11 passive scanning, deauth, handshake capture, PMKID
    harvesting, wardriving with GPS, fake beacon injection, RF spectrum
    analysis, Flock Safety ALPR camera detection, Axon bodycam detection,
    WiGLE/WPA-SEC upload, and more.

    Everything runs in a single .ino file. Flash and go.

    This is a research and educational tool. Use it only on networks and in
    areas where you have explicit permission. Laws regarding WiFi monitoring,
    packet injection, and surveillance detection vary by jurisdiction.


--[ 2 - Credits

    Original project:

        M5PORKCHOP by 0ct0sec
        https://github.com/0ct0sec/M5PORKCHOP

        All core WiFi attack modes, game systems, XP system, challenge
        system, achievement system, mood/avatar system, lore, and the pig
        himself are the original work of 0ct0sec. This port exists because
        the original is excellent and the CYD is cheap. Star the original.

    CYD port:

        Created by xom

        Hardware adaptation for ESP32-2432S028R, ILI9341/XPT2046 display
        and touch integration, single-file Arduino port, SPI/pin remapping,
        SD card integration, GPS (ATGM336H) support, ledc-based SFX system,
        zone-based touch navigation, WebUI implementation, WiGLE/WPA-SEC
        HTTPS upload, ISR-safe promiscuous mode, WDT crash resolution,
        PORK PATROL surveillance detection (Flock Safety ALPR + Axon
        bodycams), and all CYD-specific adaptations.

    Surveillance detection research:

        deflock.me — crowdsourced ALPR camera location and detection data
        flock-you by colonelpanichacks — Flock Safety detection methodology
        WiGLE / vertex.link — Axon OUI and bodycam SSID research
        DeFlock project (FoggedLens) — detection pattern foundations

    Libraries:

        TFT_eSPI            Bodmer
        XPT2046_Touchscreen Paul Stoffregen
        TinyGPS++           Mikal Hart
        ESP32 Arduino Core  Espressif Systems


--[ 3 - Hardware

    Required:

        ESP32-2432S028R ("Cheap Yellow Display")
        ~$15 on AliExpress / Amazon / Elegoo store

    Tested on:

        Elegoo ESP32-32E 2.8" 240x320
        ILI9341 display, XPT2046 touch, NS4168 speaker amp

    Optional:

        MicroSD card (FAT32, any size, Class 10+ recommended)
        Passive 8Ω speaker (JST connector on back)
        PAM8403 amplifier module (~$2) for louder audio
        ATGM336H GPS module (connects to UART connector on back)


--[ 4 - Differences from M5PORKCHOP

    The CYD has no keyboard, no battery IC, and a different display/touch
    configuration. The following have been adapted or changed:

    +-----------------------------+------------------------------------------+
    | Feature                     | Status                                   |
    +-----------------------------+------------------------------------------+
    | All core WiFi modes         | PORTED — fully working                   |
    | Achievements (46 total)     | PORTED — fully working                   |
    | XP / leveling system        | PORTED — fully working                   |
    | Challenges system           | PORTED — fully working                   |
    | Mood / avatar system        | PORTED — fully working                   |
    | SD card saves               | PORTED — .22000 + WiGLE CSV             |
    | WiGLE upload                | PORTED — HTTPS POST from CAPTURES       |
    | WPA-SEC upload              | PORTED — HTTPS POST from CAPTURES       |
    | Boar Bros / PigSync         | PORTED — ESP-NOW peer sync              |
    | Unlockables                 | PORTED — SHA256 phrase verification     |
    | Speaker / SFX               | PORTED — ledc PWM, 25 sounds            |
    | GPS wardriving              | PORTED — ATGM336H via UART connector    |
    | Web Remote                  | PORTED — AP mode HTTP server            |
    | PORK PATROL                 | NEW — Flock ALPR + Axon bodycam detect  |
    | Keyboard text entry         | NOT PORTED — no keyboard on CYD         |
    |                             | (API keys set in source before flash)   |
    | PIGGY BLUES                 | STUB — BLE stack exceeds RAM budget     |
    | Charging screen             | NOT PORTED — CYD has no battery IC     |
    | Crash viewer                | NOT PORTED — low value on CYD          |
    | SD format tool              | NOT PORTED — format card before use    |
    | Stress test                 | NOT PORTED — dev tool                  |
    +-----------------------------+------------------------------------------+


--[ 5 - Installation


----[ 5.1 - Requirements

    Arduino IDE 2.x
    ESP32 Arduino Core 3.3.7 by Espressif
    USB driver for CH340 or CP2102 (board dependent)
    MicroSD card formatted as FAT32


----[ 5.2 - Arduino IDE Setup

    1. File → Preferences → Additional Boards Manager URLs:
       https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

    2. Tools → Board → Boards Manager → search "esp32" → install Espressif

    3. Select: ESP32 Dev Module

    Board settings:

        Board:              ESP32 Dev Module
        Upload Speed:       921600
        CPU Frequency:      240MHz
        Flash Frequency:    80MHz
        Flash Mode:         DIO
        Flash Size:         4MB (32Mb)
        Partition Scheme:   Default 4MB with spiffs
        PSRAM:              Disabled


----[ 5.3 - Library Dependencies

    Install via Library Manager (Tools → Manage Libraries):

        TFT_eSPI                by Bodmer
        XPT2046_Touchscreen     by Paul Stoffregen
        TinyGPS++               by Mikal Hart  (for GPS wardriving)

    Copy User_Setup.h into your TFT_eSPI library folder, replacing the
    existing one. This sets the correct pin config for the CYD variant.

    The following are included in the ESP32 Arduino Core (no install):

        WiFi, WiFiClientSecure, SD, SPI, Preferences, SPIFFS,
        esp_wifi, lwip/sockets, mbedtls/base64


----[ 5.4 - API Keys

    Open PorkChop_CYD.ino and search for:

        PASTE YOUR API KEYS HERE

    You will find three lines around line 196:

        char wigleApiName[48]  = "";   // paste WiGLE account name here
        char wigleApiToken[48] = "";   // paste WiGLE API token here
        char wpasecKey[48]     = "";   // paste WPA-SEC key here

    Paste your keys between the quotes, save, and flash.
    Keys survive reflash (stored in NVS).

    To get your keys:

        WiGLE:   wigle.net → Account → API → show token
        WPA-SEC: wpa-sec.stanev.org → ?show_key after registering


----[ 5.5 - Flashing

    1. Place PorkChop_CYD.ino and User_Setup.h in a folder named
       PorkChop_CYD/
    2. Open PorkChop_CYD.ino in Arduino IDE
    3. Insert FAT32-formatted SD card with /captures and /wigle folders
    4. Connect CYD via USB, select correct COM port
    5. Click Upload
    6. First boot: pig appears, boot sound plays (if speaker connected)


--[ 6 - Pin Map

    Display (ILI9341) — HSPI:

        MOSI    GPIO 13
        MISO    GPIO 12
        CLK     GPIO 14
        CS      GPIO 15
        DC      GPIO 2
        BL      GPIO 21

    Touch (XPT2046) — VSPI:

        CLK     GPIO 25
        MISO    GPIO 39
        MOSI    GPIO 32
        CS      GPIO 33
        IRQ     GPIO 36

    SD Card — VSPI default:

        CLK     GPIO 18
        MISO    GPIO 19
        MOSI    GPIO 23
        CS      GPIO 5

    Speaker (NS4168 amp):

        Signal  GPIO 26   ← DO NOT reassign

    GPS (ATGM336H via UART connector):

        RX      GPIO 3    ← CYD UART connector RXD pin
        TX      not used

    RGB LED:

        R       GPIO 4
        G       GPIO 16
        B       GPIO 17

    UART Connector (back of board):

        5V  |  GND  |  TXD  |  RXD
        ↑ power GPS here    ↑ GPS TX wire goes here

    CN1 Connector (back of board):

        3.3V  |  GPIO 27  |  GPIO 22  |  GND

    CRITICAL: GPIO 26 = speaker only. Never reassign.
    CRITICAL: GPIO 25 = touch CLK. Never drive as output.
    CRITICAL: SD CS = GPIO 5 only.


--[ 7 - SD Card Setup

    Format card as FAT32 before first use.

    Create these folders on the card:

        /captures/    ← handshake .22000 files saved here
        /wigle/       ← wardriving .csv files saved here

    The firmware creates these directories automatically if they don't
    exist, but creating them manually avoids the first-save delay.

    Files are named automatically:

        /captures/SSID_BSSID.22000      hashcat WPA format
        /captures/SSID_BSSID.16800      PMKID-only format
        /wigle/wardrive_XXXXXX.csv      WiGLE wardriving CSV


--[ 8 - Modes


----[ 8.1 - IDLE / Home

    The pig lives here. Mood shifts with network activity.
    Status bar shows mode, heap, scan count, XP level.

    Tap:  open menu
    Hold: cycle theme


----[ 8.2 - OINK MODE

    Active deauth + EAPOL handshake capture + PMKID harvest.

    Scans for WPA2 targets, sends deauth frames to disconnect clients,
    captures 4-way handshake when they reconnect. Also captures PMKID
    from EAPOL M1 without requiring a client to be present.

    Captures save automatically to /captures/ on SD card.

    Note: promiscuous filter set to DATA-only during OINK to prevent
    WiFi driver watchdog timeouts in dense RF environments.

    Bottom bar tap: exit
    Hold: stop + back to IDLE


----[ 8.3 - DO NO HAM

    Passive monitoring only. Zero transmission.

    Sniffs 802.11 management and data frames. Tracks networks,
    clients, handshakes, PMKIDs. No deauth, no injection.

    Bottom bar tap: exit
    Hold: back to IDLE


----[ 8.4 - SGT WARHOG

    Wardriving mode with GPS coordinate logging.

    Scans and logs all visible networks with RSSI, encryption,
    channel, BSSID, and GPS coordinates (if fix available).
    Saves WiGLE-compatible CSV to /wigle/ on exit.

    GPS fix shown in status bar as GPS:FIX or satellite count.
    With GPS fix, each network entry gets real lat/lon/altitude.
    Without fix, coordinates are 0.000000.

    Bottom bar tap: exit + save CSV
    Hold: save + back to IDLE


----[ 8.5 - SPECTRUM

    RF channel utilization visualizer.

    Live bar graph of signal strength per 2.4GHz channel (1-13).
    Shows channel congestion and interference sources.

    Bottom bar tap: exit
    Hold: back to IDLE


----[ 8.6 - BACON MODE

    Fake beacon injection.

    Broadcasts 802.11 beacon frames with spoofed SSIDs fingerprinted
    from nearby access points. Tap bottom zones to cycle beacon rate.

    Bottom bar tap: exit
    Hold: stop + back to IDLE


----[ 8.7 - PIGGY BLUES

    BLE advertisement spam — stubbed on CYD.

    The BLE stack requires ~60KB contiguous RAM which cannot coexist
    with the display sprite and WiFi stack on the CYD's 320KB heap.
    Mode displays a message explaining the constraint.

    Hardware limitation. Cannot be resolved without reducing other
    features.


----[ 8.8 - PORK PATROL

    Surveillance camera detection. The pig goes undercover.

    Passively scans the WiFi network list for known signatures of:

        [CAM] Flock Safety ALPR cameras
              — 20 known OUI prefixes from deflock.me dataset
              — SSID patterns: flock, fs ext, pigvision, penguin,
                flockca

        [BWC] Axon / Motorola body-worn cameras
              — Axon OUI 00:25:DF (confirmed bodycam prefix)
              — SSID patterns: axon, bwcviewer, evidence, taser,
                motorola bwc, vievu, v300, v500, ibr

    Display shows FLOCK:X BODYCAM:Y TOTAL:Z when devices spotted.
    Each detection triggers an alert sound and logs to serial with
    GPS coordinates if a fix is available.

    Detection uses existing passive scan data — zero extra transmission.
    Works alongside any other mode that keeps NetworkRecon running.

    Detection signatures sourced from deflock.me, flock-you
    (colonelpanichacks), and WiGLE OUI research (vertex.link).

    Bottom bar tap: exit
    Hold: stop + back to IDLE


----[ 8.9 - SWINE STATS

    Three-tab statistics screen.

    ST4TS:  Session captures, deauths, networks, runtime, XP.
    B00STS: Active XP multipliers and mood buffs.
    W1GL3:  WiGLE file count on SD card.

    Tap: cycle tabs
    Hold: back to IDLE


----[ 8.10 - CAPTURES

    File management and upload screen.

    Shows handshake and PMKID counts saved to SD.
    One-tap upload to WiGLE and WPA-SEC services.

        Bottom-left:  WPASEC — upload all .22000 from /captures/
        Bottom-right: WIGLE  — upload all .csv from /wigle/

    Buttons show green when API keys are configured.
    Requires WiFi connection to upload.

    Long hold: serial dump + SPIFFS save


----[ 8.11 - BOUNTY

    Scored target tracking.

    Tracks previously seen networks with bounty scores based on
    encryption type, signal strength, and capture status.

    Tap: select target
    Hold: back to IDLE


----[ 8.12 - BOAR BROS

    ESP-NOW peer sync (PigSync protocol).

    Discovers nearby PorkChop devices. Syncs capture counts,
    bounty lists, XP events between units.

    Tap: scan / select peer
    Hold: back to IDLE


----[ 8.13 - CHALLENGES

    Three active challenges: EASY / MEDIUM / HARD.
    Refreshes each boot. Completing all three triggers bonus XP.

    Tap: view details
    Hold: back to IDLE


----[ 8.14 - ACHIEVEMENTS

    46 unlockable achievements across all activity types.
    Triggered automatically. Earning one plays a fanfare and
    bounces the pig.

    Tap: scroll list
    Hold: back to IDLE


----[ 8.15 - UNLOCKABLES

    Four secret phrases hidden in the lore.
    Enter via Serial Monitor while in SETTINGS.

    Tap: select slot
    Hold: back to IDLE


----[ 8.16 - DIAGNOSTICS

    System health dashboard.

    Shows: free heap, largest block, heap pressure, WiFi state,
    SD status, SPIFFS usage, GPS fix/satellites, uptime, XP.

    Tap: toggle Knuth heap visualizer
    Hold: back to IDLE


----[ 8.17 - SETTINGS

    Theme and sound configuration + API key entry.

    Top half tap:    cycle color theme (15 themes)
    Bottom half tap: toggle sound ON/OFF
    Hold:            save all settings + back to IDLE

    API key entry (Serial Monitor at 115200 baud):
        Navigate to SETTINGS on device
        Type: WIGLE   — enter WiGLE API name and token
        Type: WPASEC  — enter WPA-SEC API key
        Type: CLEAR   — wipe all API credentials

    Easier method: paste keys directly in source before flashing.
    See section 5.4.


----[ 8.18 - WEB REMOTE

    Browser-based remote control over WiFi AP.

    Tap to start: broadcasts "PORKCHOP" open WiFi AP.
    Connect to PORKCHOP, browse to http://192.168.4.1

    Features:
        Live screen mirror (80x50 pixels, updates ~600ms)
        Mode switching buttons
        Touch zone simulation
        WiGLE + WPA-SEC one-tap upload

    Tap again: stop AP, return to STA scanning.
    Hold: back to IDLE.

    Note: scanning pauses while Web Remote is active.


--[ 9 - Navigation

    All navigation uses the touchscreen. No keyboard.

    Menu uses zone-based taps:

        +---------------------------+
        |   TOP THIRD — scroll up   |
        +---------------------------+
        |   MID THIRD — select      |
        +---------------------------+
        |   BOT THIRD — scroll down |
        +---------------------------+

    Top-left corner (any screen): back
    Bottom bar tap (any mode):    exit mode → IDLE
    Long press (1.5s):            hold action (save/exit/confirm)

    Menu shows 17 items. Scroll with top/bottom taps.


--[ 10 - GPS

    Module: ATGM336H (NMEA 0183, 9600 baud)
    Connection: CYD UART connector (back of board)

    Wiring:

        ATGM336H 3V3  →  UART connector 5V  (module accepts 3.3-5V)
        ATGM336H GND  →  UART connector GND
        ATGM336H TX   →  UART connector RXD
        ATGM336H RX   →  not connected

    GPS feeds real coordinates into WiGLE CSV during wardriving and
    PORK PATROL detection logs.

    Status bar in WARHOG shows GPS:FIX when locked or satellite count
    while searching. Cold fix typically takes 30-90 seconds outdoors.

    Note: A few garbage characters may appear at boot in Serial Monitor
    due to GPIO 3 being shared between GPS NMEA output and UART0.
    This is cosmetic only and does not affect operation.


--[ 11 - SD Card Saves

    Handshakes (.22000 format, hashcat compatible):

        /captures/SSID_BSSID.22000

    PMKID-only captures (.16800 format):

        /captures/SSID_BSSID.16800

    WiGLE wardriving CSV:

        /wigle/wardrive_XXXXXX.csv

    Files save automatically:
        Handshakes/PMKIDs: immediately on capture in OINK/DNH mode
        WiGLE CSV: on exit from WARHOG mode

    SD card must be FAT32. Folders /captures and /wigle must exist
    at the root of the card (created automatically on first save
    if missing).


--[ 12 - WiGLE Upload

    Uploads wardriving CSV files to api.wigle.net.

    Setup: paste WiGLE API name and token in source (section 5.4)
    or enter via Serial Monitor in SETTINGS.

    To upload:
        CAPTURES screen → tap WIGLE button (bottom-right)
        All CSV files in /wigle/ are uploaded via HTTPS POST

    Requires WiFi connection. Button shows green when configured.


--[ 13 - WPA-SEC Upload

    Uploads captured handshakes to wpa-sec.stanev.org for
    distributed cracking against 10+ billion passwords.

    Setup: paste WPA-SEC API key in source (section 5.4)
    or enter via Serial Monitor in SETTINGS.

    To upload:
        CAPTURES screen → tap WPASEC button (bottom-left)
        All .22000 files in /captures/ are uploaded via HTTPS POST

    Button shows green when API key is configured.


--[ 14 - Speaker & Sound

    Amp: NS4168 class-D on GPIO 26, AC-coupled input.
    Signal: ledc PWM at 50% duty (true square wave) on 10-bit timer.

    For louder output, wire a PAM8403 amplifier module between
    GPIO 26 and your speaker. Search "PAM8403 audio amplifier module"
    on Amazon (~$2). Gives 3W vs the built-in 0.5W.

    PAM8403 wiring:

        CYD GPIO 26  →  PAM8403 Left IN+
        CYD GND      →  PAM8403 Left IN-
        CYD 3.3V     →  PAM8403 VCC
        CYD GND      →  PAM8403 GND
        PAM8403 OUT+ →  Speaker +
        PAM8403 OUT- →  Speaker -

    25 sound events: BOOT, CLICK, HANDSHAKE, PMKID, DEAUTH,
    ACHIEVEMENT, LEVEL_UP, CHALLENGE_COMPLETE, UPLOAD_OK, and more.

    Sound toggle: SETTINGS → bottom half tap (ON/OFF).
    Volume is always maximum (50% duty = loudest square wave through
    the AC coupling cap).


--[ 15 - XP & Leveling

    XP accumulates across all activity. Level is permanent (NVS).

    Key XP events:

        Network scan          +1 XP per unique network
        Handshake capture     +50 XP
        PMKID capture         +30 XP
        Deauth sent           +2 XP
        Challenge complete    +15 to +300 XP (by difficulty)
        Achievement unlock    +100 XP
        WiGLE upload          +25 XP
        WPA-SEC upload        +25 XP

    Current level and total XP shown in status bar and SWINE STATS.


--[ 16 - Challenges

    Three challenges active at all times (EASY / MEDIUM / HARD).
    Regenerate each boot with randomized targets and XP rewards.

    Challenge types include: scan N nets, capture N handshakes,
    capture N PMKIDs, send N deauths, find N open/hidden/WPA3 nets,
    lurk silently, and more.

    Completing all three in one session triggers CHALLENGE_SWEEP bonus.
    Progress tracked automatically across all modes.


--[ 17 - Achievements

    46 achievements unlock automatically during use.
    Stored in NVS — survive reflashing.

    Categories: network discovery, capture milestones, deauth
    milestones, wardriving, XP milestones, social/upload,
    and hidden triggers.

    Earning an achievement plays a fanfare and displays the name
    as a toast. The pig does a happy bounce.


--[ 18 - Change Log

    CYD port created by xom — changes from original M5PORKCHOP:

    Hardware:
        Ported from M5Stack Cardputer to ESP32-2432S028R
        ILI9341 TFT via HSPI (MOSI=13 MISO=12 CLK=14 CS=15 DC=2)
        XPT2046 touch via VSPI (CLK=25 MISO=39 MOSI=32 CS=33 IRQ=36)
        NS4168 speaker amp via GPIO 26 (ledc PWM, no tone())
        ATGM336H GPS via UART connector (GPIO 3 RX, 9600 baud)
        SD card via VSPI default (CLK=18 MISO=19 MOSI=23 CS=5)

    Navigation:
        Zone-based touch navigation (no keyboard)
        Top/mid/bottom tap zones for menu scroll/select
        Top-left corner = back on all screens
        Bottom bar [X] button = exit any mode

    New features:
        PORK PATROL — Flock Safety ALPR + Axon bodycam detection
            20 Flock OUI prefixes + 5 SSID patterns (deflock.me)
            3 Axon OUI prefixes + 9 SSID patterns (bodycam WiFi)
            GPS-tagged serial log on detection
            Passive — runs on existing scan data, zero extra TX
        WiGLE HTTPS upload directly from CAPTURES screen
        WPA-SEC HTTPS upload directly from CAPTURES screen
        Web Remote (HTTP AP server, browser screen mirror)
        GPS coordinate logging in WiGLE wardriving CSV
        GPS coordinate logging in PORK PATROL detections
        ledc-based SFX system (25 sounds, no tone() crashes)
        ISR-safe promiscuous mode (DATA-only filter during OINK)
        API keys configurable in source before flashing

    Bug fixes:
        tone() crash on ESP32 core 3.x → replaced with ledc
        WDT crash in OINK → beacon callbacks filtered at driver level
        WiFi driver deadlock → volatile callback pointer, no mutex
        Stack canary in WebUI → increased task stack
        SD directory creation → parent dirs created automatically
        NimBLE/sprite memory conflict → BLE stubbed, sprite preserved

    Known limitations:
        PIGGY BLUES unavailable — BLE stack (~60KB) + sprite (102KB)
          + WiFi + SD exceeds 320KB heap. Hardware constraint.
        Web Remote screen mirror occasionally stalls (Chrome issue)
        GPS NMEA causes brief garbage at boot (GPIO 3 shared)
        SPIFFS mount error at boot is cosmetic (formats on first use)


==[EOF]==
