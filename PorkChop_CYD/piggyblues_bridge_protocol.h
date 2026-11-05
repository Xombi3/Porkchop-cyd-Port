#pragma once
// PIGGY BLUES — WarDriveGo (GhostLink) BLE bridge, wire protocol.
//
// Hardware-agnostic protocol logic: GATT UUIDs, 12-byte GhostLink framing, the
// bounded RX command reassembler, the WarDriveGo command parser, the AP line
// formatter and the outbound queue. No BLE or Arduino dependency here, so this
// header compiles and can be reasoned about on its own.
//
// Adapted from slickrick773's umbravox C5 bridge (device/ble_bridge_protocol.h),
// trimmed to the WarDriveGo surface and retargeted to porkchop's scan records.
// The wire contract follows RocketGod's public WardriveESP / GhostLink protocol
// for interoperability only; no RocketGod firmware source is copied. See
// ideas-and-lessons/assets/ble/iosesp-protocol-and-companion.md.
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <algorithm>
#include <esp_wifi_types.h>

namespace pbwire {

constexpr size_t MAX_LINE = 384, MAX_CMD = 192, HEADER = 12;

// Compatibility sentinel from slickrick773's ESP32Marauder PR #1671. NOT a
// porkchop release number — the phone app keys interop off the exact value.
constexpr const char* FW_VERSION = "99.99.99";
constexpr const char* WD_CAPS =
    "WD:CAPS fw=99.99.99 chip=esp32c5 bands=2g,5g ota=0 bt=0 gps=0 cap=0 usb=0 "
    "board_name=Porkchop_WS_C5_35 link=ble";

// GhostLink service + characteristics (WardriveESP v1.6 contract).
constexpr const char* WD_SERVICE = "47686F73-7445-5350-4272-696467655376";
constexpr const char* WD_RX      = "0147686F-7374-4553-5042-726964675852"; // phone -> board
constexpr const char* WD_TX      = "0247686F-7374-4553-5042-726964675854"; // board -> phone (notify)
constexpr const char* WD_CTRL    = "0347686F-7374-4553-5042-72694C525443"; // phone -> board

// esp_read_mac() gives canonical byte order, unlike NimBLE's native address.
inline void formatDeviceName(char* out, size_t cap, const uint8_t mac[6]) {
    snprintf(out, cap, "Porkchop-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

// One exported AP record. Mirrors the fields porkchop's DetectedNetwork holds
// that the wire format needs.
struct Ap {
    uint8_t  mac[6]{};
    char     ssid[33]{};
    int8_t   rssi = 0;
    uint8_t  channel = 0;
    wifi_auth_mode_t authmode = WIFI_AUTH_OPEN;
    bool     hidden = false;
    uint32_t at = 0; // ms timestamp (lastSeen)
};

// Fresh-AP eligibility: at least 3 s after connect, and keep six lines free for
// control traffic.
inline bool apReady(uint32_t now, uint32_t connectedAt, unsigned freeLines) {
    return uint32_t(now - connectedAt) >= 3000 && freeLines >= 6;
}
constexpr uint32_t AP_FRESH_MS = 60000;

inline uint32_t u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// GhostLink frame: 'G','B',ver=1,type,status,flags,id(LE32),len(LE16),payload.
inline size_t frame(uint8_t* out, size_t cap, uint8_t type, uint32_t id,
                    const char* text, size_t n, uint8_t status = 0) {
    if (cap < HEADER || n > cap - HEADER || n > 65535) return 0;
    out[0] = 'G'; out[1] = 'B'; out[2] = 1; out[3] = type; out[4] = status; out[5] = 0;
    for (int i = 0; i < 4; ++i) out[6 + i] = uint8_t(id >> (8 * i));
    out[10] = uint8_t(n); out[11] = uint8_t(n >> 8);
    if (n) memcpy(out + HEADER, text, n);
    return HEADER + n;
}

// Reassembles one bounded command across GATT writes. Raw (unframed) writes are
// taken as a complete command line.
struct Command {
    char text[MAX_CMD + 1]{};
    size_t used = 0;
    uint32_t id = 1, at = 0;
    bool partial = false, framed = false;
    void reset() { used = 0; partial = false; text[0] = 0; }
    int accept(const uint8_t* p, size_t n, uint32_t now) { // -1 reject, 0 pending, 1 ready
        if (!p || !n) return -1;
        if (partial && uint32_t(now - at) > 3000) reset();
        const bool gb = n >= 2 && p[0] == 'G' && p[1] == 'B';
        if (gb) {
            if (n < HEADER || p[2] != 1 || p[3] != 1 || p[4] != 0 || (p[5] & ~3) ||
                size_t(p[10] | uint16_t(p[11]) << 8) != n - HEADER) { reset(); return -1; }
            const uint32_t next = u32(p + 6);
            const bool first = p[5] & 1, more = p[5] & 2;
            if (first) { reset(); id = next; }
            else if (partial) { if (id != next) { reset(); return -1; } }
            else if (more) { reset(); return -1; }
            else { reset(); id = next; }
            framed = true; p += HEADER; n -= HEADER;
            if (n > MAX_CMD - used) { reset(); return -1; }
            for (size_t i = 0; i < n; ++i)
                if (p[i] < 32 && p[i] != '\r' && p[i] != '\n' && p[i] != '\t') { reset(); return -1; }
            memcpy(text + used, p, n); used += n; text[used] = 0; partial = more; at = now;
            return more ? 0 : 1;
        }
        reset(); framed = false; id = 1;
        if (n > MAX_CMD) return -1;
        for (size_t i = 0; i < n; ++i)
            if (p[i] < 32 && p[i] != '\r' && p[i] != '\n' && p[i] != '\t') return -1;
        memcpy(text, p, n); used = n; text[n] = 0; return 1;
    }
};

enum Action { INVALID, START, STOP, IDENTITY, UNSUPPORTED };
struct ChannelFilter {
    bool enabled = false;
    uint8_t bits[32]{};
    bool allows(uint8_t ch) const { return ch && (!enabled || (bits[ch / 8] & (1u << (ch % 8)))); }
};
struct Parsed { Action action = INVALID; char name[32]{}; ChannelFilter channels; };

// WarDriveGo command grammar. Never executes a prefix or multiple commands in
// one write.
inline Parsed parse(const char* input) {
    Parsed r;
    char s[MAX_CMD + 1];
    size_t n = strlen(input);
    if (n > MAX_CMD) return r;
    while (n && (input[n - 1] == '\n' || input[n - 1] == '\r' || input[n - 1] == ' ' || input[n - 1] == '\t')) --n;
    while (n && (*input == ' ' || *input == '\t')) { ++input; --n; }
    memcpy(s, input, n); s[n] = 0;
    if (strchr(s, '\n') || strchr(s, '\r')) return r;
    if (!strcmp(s, "wdstream start -wifi") || !strcmp(s, "wdstream start") || !strcmp(s, "start"))
        r.action = START;
    else if (!strncmp(s, "wdstream start -wifi -ch ", sizeof("wdstream start -wifi -ch ") - 1)) {
        const char* p = s + sizeof("wdstream start -wifi -ch ") - 1;
        if (!*p) return r;
        while (*p) {
            unsigned ch = 0, digits = 0;
            while (*p >= '0' && *p <= '9') { if (++digits > 3) return r; ch = ch * 10 + unsigned(*p++ - '0'); }
            if (!digits || !ch || ch > 196) return r;
            r.channels.bits[ch / 8] |= uint8_t(1u << (ch % 8));
            if (!*p) break;
            if (*p++ != ',' || !*p) return r;
        }
        r.channels.enabled = true; r.action = START;
    } else if (!strncmp(s, "iam ", 4) && (s[4] == 'a' || s[4] == 'i') && s[5] == ' ' && s[6]) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s + 6); *p; ++p)
            if (*p < 32 || *p > 126) return r;
        r.action = IDENTITY;
    } else if (!strcmp(s, "stop") || !strcmp(s, "wdstream stop"))
        r.action = STOP;
    else
        r.action = UNSUPPORTED;
    return r;
}

// porkchop stores wifi_auth_mode_t directly; map it to the GhostLink auth token.
inline const char* authName(wifi_auth_mode_t a) {
    switch (a) {
        case WIFI_AUTH_OPEN:            return "OPEN";
        case WIFI_AUTH_WEP:             return "WEP";
        case WIFI_AUTH_WPA_PSK:         return "WPA";
        case WIFI_AUTH_WPA2_PSK:        return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2";
        case WIFI_AUTH_WPA3_PSK:        return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2_WPA3";
        default:                        return "UNKNOWN";
    }
}

// Poll for a newly completed scan on a short cadence. The real provider returns
// no rows until its staging snapshot is ready; dedup happens on the phone by BSSID.
inline bool restartRound(bool done, uint32_t now, uint32_t at) { return done && uint32_t(now - at) >= 5000; }

inline size_t apLine(char* out, size_t cap, const Ap& a) {
    char mac[18], hex[65];
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X",
             a.mac[0], a.mac[1], a.mac[2], a.mac[3], a.mac[4], a.mac[5]);
    size_t n = 0; const char* h = "0123456789abcdef";
    while (n < 32 && a.ssid[n]) { const uint8_t c = a.ssid[n]; hex[n * 2] = h[c >> 4]; hex[n * 2 + 1] = h[c & 15]; ++n; }
    hex[n * 2] = 0;
    const int k = snprintf(out, cap,
        "WD:AP ts=%lu bssid=%s ssid_hex=%s rssi=%d ch=%u auth=%s hidden=%u\n",
        (unsigned long)a.at, mac, hex, int(a.rssi), unsigned(a.channel),
        authName(a.authmode), (a.hidden || !n) ? 1u : 0u);
    if (k < 0 || size_t(k) >= cap) { if (cap) out[0] = 0; return 0; }
    return size_t(k);
}

struct Line { char text[MAX_LINE]{}; uint16_t len = 0, offset = 0; uint8_t type = 3, status = 0; uint32_t id = 1; };
struct Outbox {
    static constexpr unsigned CAP = 12;
    Line lines[CAP];
    unsigned head = 0, count = 0;
    uint32_t dropped = 0;
    void clear() { head = count = 0; }
    bool put(const char* text, uint32_t id = 1, uint8_t type = 3, uint8_t status = 0) {
        const size_t n = strlen(text);
        if (count == CAP || n >= MAX_LINE) { ++dropped; return false; }
        Line& l = lines[(head + count) % CAP];
        memcpy(l.text, text, n + 1); l.len = n; l.offset = 0; l.id = id; l.type = type; l.status = status; ++count;
        return true;
    }
    Line* front() { return count ? &lines[head] : nullptr; }
    void pop() { if (count) { head = (head + 1) % CAP; --count; } }
    unsigned free() const { return CAP - count; }
};

// Notifications obey the negotiated ATT MTU, including the GhostLink header.
inline size_t packet(const Line& l, unsigned mtu, uint8_t* out, size_t cap, size_t& consumed) {
    consumed = 0;
    const size_t att = mtu > 3 ? std::min<size_t>(mtu - 3, cap) : 0;
    if (att <= HEADER || l.offset > l.len) return 0;
    consumed = std::min<size_t>(l.len - l.offset, att - HEADER);
    return frame(out, cap, l.type, l.id, l.text + l.offset, consumed, l.status);
}

// On a control change, finish only a line already partly transmitted; never
// splice a new message into an incomplete record at the receiver.
inline void discardUnsent(Outbox& q) {
    if (q.count && q.front()->offset) q.count = 1;
    else q.clear();
}

} // namespace pbwire
