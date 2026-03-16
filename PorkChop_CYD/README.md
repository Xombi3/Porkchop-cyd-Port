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

**Original project by [0ct0sec](https://github.com/0ct0sec/M5PORKCHOP) — ported to the Cheap Yellow Display by the community.**

---

--[ Contents

    1 - What is this
    2 - Hardware
    3 - Differences from M5PORKCHOP
    4 - Installation
        4.1 - Requirements
        4.2 - Arduino IDE Setup
        4.3 - Library Dependencies
        4.4 - Flashing
    5 - Pin Map
    6 - Modes
        6.1  - IDLE / Home
        6.2  - OINK MODE
        6.3  - DO NO HAM
        6.4  - SGT WARHOG
        6.5  - SPECTRUM
        6.6  - BACON MODE
        6.7  - PIGGY BLUES
        6.8  - SWINE STATS
        6.9  - CAPTURES
        6.10 - BOUNTY
        6.11 - BOAR BROS
        6.12 - CHALLENGES
        6.13 - ACHIEVEMENTS
        6.14 - UNLOCKABLES
        6.15 - DIAGNOSTICS
        6.16 - SETTINGS
        6.17 - WEB REMOTE
    7 - Navigation
    8 - SD Card
    9 - WiGLE Upload
    10 - WPA-SEC Upload
    11 - Speaker & Sound
    12 - Achievements
    13 - XP & Leveling
    14 - Challenges
    15 - Credits


--[ 1 - What is this

    PORKCHOP CYD is a WiFi security research tool ported from the original
    M5PORKCHOP project (M5Stack Cardputer) to the ESP32-2432S028R "Cheap
    Yellow Display" — a $15 ESP32 board with a 2.8" ILI9341 TFT, resistive
    touchscreen, and built-in speaker amplifier.

    It does 802.11 passive scanning, deauth, handshake capture, PMKID
    harvesting, wardriving, fake beacon injection, BLE advertising spam,
    RF spectrum analysis, and more. Captured data saves to SD card in
    formats ready for hashcat and WPA-SEC upload.

    Everything runs on a single .ino file. No operating system, no Python,
    no dependencies beyond the Arduino libraries listed below. Flash it and
    it works.

    This is a research and educational tool. Use it on networks you own
    or have explicit permission to test.


--[ 2 - Hardware

    Required:

        ESP32-2432S028R ("Cheap Yellow Display")
        ~$15 on AliExpress / Amazon

    Tested variant:

        Elegoo ESP32-32E 2.8" 240x320 TFT
        ILI9341 display, XPT2046 touch, NS4168 speaker amp

    Optional but recommended:

        MicroSD card (FAT32 formatted, any size)
        Passive speaker connected to JST connector
        USB-C cable for power and flashing


--[ 3 - Differences from M5PORKCHOP

    The CYD has no keyboard, no battery management IC, and a different
    display/touch configuration. The following have been adapted or removed:

    +-----------------------------+------------------------------------------+
    | Feature                     | Status                                   |
    +-----------------------------+------------------------------------------+
    | All core WiFi modes         | PORTED — fully working                   |
    | Achievements (46 total)     | PORTED — fully working                   |
    | XP / leveling system        | PORTED — fully working                   |
    | Challenges system           | PORTED — fully working                   |
    | Mood / avatar system        | PORTED — fully working                   |
    | SD card saves               | PORTED — .22000 + WiGLE CSV              |
    | WiGLE upload                | PORTED — HTTPS POST from CAPTURES screen |
    | WPA-SEC upload              | PORTED — HTTPS POST from CAPTURES screen |
    | Boar Bros / PigSync         | PORTED — ESP-NOW peer sync               |
    | Unlockables                 | PORTED — SHA256 phrase verification      |
    | Speaker / SFX               | PORTED — 25 sounds via ledc PWM          |
    | Web Remote                  | PORTED — AP mode HTTP server             |
    | Keyboard text entry         | NOT PORTED — CYD has no keyboard         |
    |                             | (API keys entered via Serial Monitor)    |
    | Charging screen             | NOT PORTED — CYD has no battery IC      |
    | Crash viewer                | NOT PORTED — low value on CYD            |
    | SD format tool              | NOT PORTED — format before use           |
    | GPS distance tracking       | NOT PORTED — no GPS attached by default  |
    | Stress test                 | NOT PORTED — dev tool                    |
    +-----------------------------+------------------------------------------+


--[ 4 - Installation


----[ 4.1 - Requirements

    Arduino IDE 2.x
    ESP32 Arduino Core 3.3.7 by Espressif
    USB driver for CH340 or CP2102 (board dependent)


----[ 4.2 - Arduino IDE Setup

    1. Open File → Preferences
    2. Add to Additional Boards Manager URLs:
       https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json

    3. Tools → Board → Boards Manager → search "esp32" → install by Espressif
    4. Select board: ESP32 Dev Module

    Board settings:

        Board:              ESP32 Dev Module
        Upload Speed:       921600
        CPU Frequency:      240MHz
        Flash Frequency:    80MHz
        Flash Mode:         DIO
        Flash Size:         4MB (32Mb)
        Partition Scheme:   Default 4MB with spiffs
        Core Debug Level:   None
        PSRAM:              Disabled


----[ 4.3 - Library Dependencies

    Install via Arduino IDE Library Manager:

        TFT_eSPI        by Bodmer
        XPT2046_Touchscreen  by Paul Stoffregen

    Copy User_Setup.h into your TFT_eSPI library folder,
    replacing the existing one. This configures the display
    pins for the CYD variant.

    The following are included in ESP32 Arduino Core
    (no separate install needed):

        WiFi, WiFiClientSecure, SD, SPI, Preferences,
        SPIFFS, esp_wifi, lwip/sockets, mbedtls/base64


----[ 4.4 - Flashing

    1. Download PorkChop_CYD.ino and User_Setup.h
    2. Place both in a folder named PorkChop_CYD/
    3. Open PorkChop_CYD.ino in Arduino IDE
    4. Connect CYD via USB
    5. Select the correct COM port
    6. Click Upload

    First boot takes ~3 seconds. You will hear two boot beeps
    if a speaker is connected. The pig appears. You are ready.

    If the display shows nothing: verify User_Setup.h is in
    the TFT_eSPI library folder and matches your board variant.


--[ 5 - Pin Map

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

        Signal  GPIO 26     ← DO NOT use for anything else
                            ← AC-coupled, always-on amp

    RGB LED:

        R       GPIO 4
        G       GPIO 16
        B       GPIO 17

    CRITICAL: GPIO 26 is the speaker. Do not reassign it.
    CRITICAL: GPIO 25 is touch CLK. Do not drive it as output.
    CRITICAL: SD CS is GPIO 5 only. GPIO 26 conflicts with speaker.


--[ 6 - Modes


----[ 6.1 - IDLE / Home

    The pig lives here. Mood shifts with network activity.
    Tap to open the menu. Hold to force-sleep the display.

    Status bar shows: mode name, heap, scan count, XP level.


----[ 6.2 - OINK MODE

    Active deauth + EAPOL frame capture.

    Cycles WiFi channels scanning for targets. Sends 802.11
    deauthentication frames to knock clients off their AP,
    then captures the 4-way handshake when they reconnect.
    Also captures PMKID from EAPOL frame 1 without deauth.

    Captures save to SD: /porkchop/captures/

        .22000 files — hashcat HCCAPX format (WPA-SEC compatible)
        Each file named by BSSID and timestamp

    Tap: cycle deauth targets
    Hold: back to menu


----[ 6.3 - DO NO HAM

    Passive monitoring only. No transmission.

    Sniffs 802.11 management and data frames. Tracks unique
    networks, clients, handshakes, PMKIDs. Logs to SD.
    Safe to run anywhere — zero RF emissions.

    Tap: cycle display views
    Hold: back to menu


----[ 6.4 - SGT WARHOG

    Wardriving mode. Scans and logs all visible networks
    with RSSI, encryption type, channel, and BSSID.

    Saves WiGLE-compatible CSV to SD on exit:
        /porkchop/wigle/

    CSV format matches WiGLE file upload spec exactly.
    Upload directly from the CAPTURES screen.

    Tap: pause/resume
    Hold: save + back to menu


----[ 6.5 - SPECTRUM

    RF channel utilization visualizer.

    Displays signal strength per 2.4GHz channel (1-13) as
    a live bar graph. Shows channel congestion at a glance.
    Useful for selecting clear channels or locating sources.

    Tap: toggle display style
    Hold: back to menu


----[ 6.6 - BACON MODE

    Fake beacon injection.

    Broadcasts 802.11 beacon frames with custom SSIDs.
    Pre-loaded SSID list includes various attention-grabbing
    names. All beacons are open network (no auth required).

    Use for: testing client association behavior, wifi surveys,
    or demonstrating SSID spoofing to clients.

    Tap: cycle beacon set
    Hold: stop + back to menu


----[ 6.7 - PIGGY BLUES

    Bluetooth Low Energy advertising spam.

    Broadcasts BLE advertisement packets at maximum rate.
    Cycles through randomized device names and UUIDs.
    Tests BLE scanner behavior and proximity detection.

    Tap: cycle advertisement type
    Hold: stop + back to menu


----[ 6.8 - SWINE STATS

    Three-tab stats screen.

    ST4TS tab:
        Session handshakes, PMKIDs, deauths, beacons seen,
        unique networks, unique clients, runtime.

    B00STS tab:
        Active XP multipliers and mood buffs with time remaining.

    W1GL3 tab:
        WiGLE file count on SD card.
        Shows total .csv files ready for upload.

    Tap: cycle tabs
    Hold: back to menu


----[ 6.9 - CAPTURES

    File management and upload screen.

    Shows counts of saved handshakes and PMKIDs on SD.
    Provides one-tap upload to WiGLE and WPA-SEC.

    Bottom-left button:  WPASEC upload (requires key set)
    Bottom-right button: WIGLE upload (requires API key set)
    Long hold anywhere:  serial dump + SPIFFS save

    Upload status shows in a toast notification.
    Button color indicates whether API credentials are set.

    See sections 9 and 10 for credential setup.


----[ 6.10 - BOUNTY

    Scored target tracking.

    Tracks previously seen networks and assigns bounty scores
    based on encryption type, signal strength, and whether
    a handshake has been captured. Tap a target to focus
    deauth attempts on it.

    Tap: select target
    Hold: back to menu


----[ 6.11 - BOAR BROS

    ESP-NOW peer synchronization (PigSync protocol).

    Discovers nearby PorkChop devices over ESP-NOW.
    Syncs capture counts, bounty lists, and XP events
    between units. Displays connected peers and their stats.

    Tap: scan for peers / select peer
    Hold: back to menu


----[ 6.12 - CHALLENGES

    Three active challenges at all times: EASY, MEDIUM, HARD.

    New challenges generate on each boot. Completing a
    challenge awards XP and may trigger mood events.
    Examples: capture N handshakes, deauth N targets,
    see N hidden networks, spam N BLE packets.

    Progress tracked automatically across all modes.
    Tap: view challenge details
    Hold: back to menu


----[ 6.13 - ACHIEVEMENTS

    46 unlockable achievements across all activity types.

    Achievements trigger automatically during normal use.
    Earning one plays a sound and shows a toast notification.
    The pig reacts to milestone achievements.

    Tap: scroll list
    Hold: back to menu


----[ 6.14 - UNLOCKABLES

    Four secret phrases hidden in the lore.

    Enter the correct phrase via Serial Monitor to unlock
    cosmetic rewards and XP bonuses. Unlocks are permanent
    and survive reflashing (stored in NVS).

    To enter a phrase:
        1. Open Serial Monitor at 115200 baud
        2. Navigate to SETTINGS
        3. Type your phrase and press Enter

    Tap: select slot
    Hold: back to menu


----[ 6.15 - DIAGNOSTICS

    System health dashboard.

    Displays: free heap, largest free block, heap pressure
    level, WiFi driver state, SD card status, SPIFFS usage,
    uptime, scan rate, and XP session total.

    Knuth heap visualization: shows fragmentation pattern
    as a color-coded bar. Green = healthy. Red = fragmented.

    Tap: toggle Knuth mode
    Hold: back to menu


----[ 6.16 - SETTINGS

    Theme and sound configuration.

    Top half tap:    cycle color theme
    Bottom half tap: cycle volume (25% → 50% → 75% → 100% → OFF)
    Hold:            save and back to menu

    API key entry (via Serial Monitor):
        Navigate to SETTINGS, open Serial Monitor (115200 baud)
        Type WIGLE  — enter WiGLE API name and token
        Type WPASEC — enter WPA-SEC API key
        Type CLEAR  — wipe all API credentials

    Keys survive flash (stored in NVS).


----[ 6.17 - WEB REMOTE

    Browser-based remote control over WiFi AP.

    Tap to start: device broadcasts "PORKCHOP" open WiFi AP.
    Connect your phone or laptop to PORKCHOP.
    Browse to http://192.168.4.1

    Features:
        Live screen mirror (updates every 600ms)
        All mode buttons (OINK, DNH, WARHOG, SPEC, BCN, BLE)
        Navigation controls (tap zones, hold)
        One-tap WiGLE and WPA-SEC upload

    Tap again: stop AP and return to STA scanning mode.
    Hold: back to menu.

    Note: Scanning pauses while Web Remote is active.
    Stop Web Remote to resume passive monitoring.


--[ 7 - Navigation

    The CYD has no keyboard. All navigation uses the touchscreen.

    Menu navigation uses zone-based taps:

        +---------------------------+
        |   TOP THIRD — scroll up   |
        +---------------------------+
        |   MID THIRD — select      |
        +---------------------------+
        |   BOT THIRD — scroll down |
        +---------------------------+

    Top-left corner:  back (from any screen)
    Long press (1.5s): hold action (save, exit, confirm)

    In modes with sub-views, the tap zone behavior is
    documented in the mode description above.

    Menu shows 16 items. Scroll with top/bottom taps.


--[ 8 - SD Card

    Format the card as FAT32 before first use.
    Any capacity works. Class 10 or better recommended.

    Directory structure created automatically on first save:

        /porkchop/
            captures/   .22000 hashcat files
            wigle/      WiGLE wardriving CSV files
            logs/       diagnostic dumps

    Files are named by BSSID and timestamp:
        AA-BB-CC-DD-EE-FF_20240101_120000.22000
        wigle_20240101_120000.csv

    SD uses VSPI on GPIO 18/19/23/5.
    If SD fails to mount, check CS=5 and card format.
    The SD init log shows attempt results at boot.


--[ 9 - WiGLE Upload

    Uploads wardriving CSV files to api.wigle.net.

    Setup (one time):
        1. Create account at wigle.net
        2. Go to Account → API → copy API Name and Token
        3. Open Serial Monitor at 115200 baud
        4. Navigate to SETTINGS on device
        5. Type: WIGLE
        6. Enter your API Name when prompted
        7. Enter your API Token when prompted

    To upload:
        Navigate to CAPTURES screen
        Tap the WIGLE button (bottom-right)
        All CSV files in /porkchop/wigle/ are uploaded

    Upload uses HTTPS POST to api.wigle.net/api/v2/file/upload
    with HTTP Basic Auth (Base64 encoded name:token).

    The WIGLE button shows green when credentials are set,
    gray when not configured.


--[ 10 - WPA-SEC Upload

    Uploads captured handshakes to wpa-sec.stanev.org for
    distributed cracking against a 10+ billion password list.

    Setup (one time):
        1. Visit https://wpa-sec.stanev.org and register
        2. Copy your API key from the dashboard
        3. Open Serial Monitor at 115200 baud
        4. Navigate to SETTINGS on device
        5. Type: WPASEC
        6. Enter your API key when prompted

    To upload:
        Navigate to CAPTURES screen
        Tap the WPASEC button (bottom-left)
        All .22000 files in /porkchop/captures/ are uploaded

    Upload uses HTTPS POST to wpa-sec.stanev.org/?api&key=<key>

    The WPASEC button shows green when credentials are set.


--[ 11 - Speaker & Sound

    The CYD has an NS4168 class-D amplifier on GPIO 26.
    Audio is AC-coupled — the signal swings through a 100nF
    capacitor to the amp's input.

    Sound is generated via ESP32 ledc (hardware PWM timer).
    50% duty square wave at audio frequency. No Arduino tone().

    25 sound events:

        BOOT, CLICK, NETWORK_NEW, HANDSHAKE, PMKID,
        DEAUTH, ACHIEVEMENT, LEVEL_UP, CHALLENGE_COMPLETE,
        CHALLENGE_SWEEP, UPLOAD_OK, UPLOAD_FAIL,
        PEER_FOUND, SYNC_DONE, BEACON_SENT, BLE_BURST,
        LOW_HEAP, UNLOCKED, BOUNTY_HIT, SCAN_SWEEP,
        MOOD_HAPPY, MOOD_SAD, MOOD_ANGRY, RIDDLE_CORRECT,
        RIDDLE_FAIL

    Volume cycles via SETTINGS: 25% → 50% → 75% → 100% → OFF
    Volume is stored in NVS and survives power cycles.

    If no sound: check speaker connector orientation.
    The JST connector on the board accepts a passive 8Ω speaker.


--[ 12 - Achievements

    46 achievements unlock automatically during use.
    All achievement state is stored in NVS (survives reflash).

    Categories:

        Network discovery (first open net, first hidden, WPA3...)
        Capture milestones (1st handshake, 10th, 50th...)
        Deauth milestones (first deauth, 100th...)
        Wardriving (first warhog session, distance milestones...)
        BLE (first BLE burst, spam milestones...)
        XP milestones (level 5, 10, 20...)
        Social (first peer sync, first upload...)
        Hidden (secret triggers not listed here)

    Earning an achievement plays a fanfare sound and
    displays the achievement name as a toast notification.
    The pig does a happy bounce animation.


--[ 13 - XP & Leveling

    XP accumulates across all activity. Level is permanent.

    XP events:

        Network scan          +1 XP per unique network
        Handshake capture     +50 XP
        PMKID capture         +30 XP
        Deauth sent           +2 XP
        Challenge complete    +22 to +300 XP (by difficulty)
        Achievement unlock    +100 XP
        WiGLE upload          +25 XP
        WPA-SEC upload        +25 XP
        Peer sync             +15 XP

    Level thresholds increase exponentially.
    Current level and total XP shown on status bar and SWINE STATS.

    XP multipliers stack from mood buffs and achievements.
    Check the B00STS tab in SWINE STATS for active multipliers.


--[ 14 - Challenges

    Three challenges active at all times (EASY / MEDIUM / HARD).
    New challenges generate on each boot with randomized targets.

    Difficulties and XP rewards:

        EASY:   +15 to +75 XP    (small targets, quick)
        MEDIUM: +45 to +150 XP   (moderate effort)
        HARD:   +60 to +300 XP   (significant grind)

    Challenge types:

        inhale N nets       — scan N unique networks
        spot N WPA3         — find N WPA3 networks
        expose N hidden     — find N hidden SSIDs
        snatch N shakes     — capture N handshakes
        swipe N PMKIDs      — capture N PMKIDs
        drop N deauths      — send N deauth frames
        pwn N targets       — successfully deauth N clients
        evict N peasants    — disconnect N clients
        spam N BLE pkts     — send N BLE advertisements
        serve N BLE         — sustain BLE for N packets
        find N open nets    — find N open (no auth) networks
        discover N APs      — see N total access points
        lurk N silently     — passive scan N events
        45 nets no deauth   — see 45 nets without deauthing

    Progress shown in CHALLENGES screen.
    Completing all three triggers CHALLENGE_SWEEP bonus.


--[ 15 - Credits

    Original project:

        M5PORKCHOP by 0ct0sec
        https://github.com/0ct0sec/M5PORKCHOP

        All core logic, game systems, WiFi attack modes,
        achievement design, XP system, challenge system,
        mood/avatar system, lore, and pig are the original
        work of 0ct0sec. This port exists because the
        original is excellent and the CYD is cheap.

        If you use this, star the original repo.

    CYD port:

        Hardware adaptation, display/touch driver integration,
        single-file Arduino port, pin remapping, SD/SPI fixes,
        WebUI implementation, WiGLE/WPA-SEC upload,
        SFX system (ledc), zone-based touch navigation.

    Libraries used:

        TFT_eSPI            Bodmer
        XPT2046_Touchscreen Paul Stoffregen
        ESP32 Arduino Core  Espressif Systems
        Unity Test          ThrowTheSwitch

    This software is for educational and authorized testing only.
    You are responsible for how you use it.
    Laws regarding WiFi monitoring and packet injection vary
    by jurisdiction. Know yours.


==[EOF]==
