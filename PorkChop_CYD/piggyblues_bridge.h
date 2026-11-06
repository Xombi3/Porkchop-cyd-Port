#pragma once

// The Waveshare C5 build has sufficient PSRAM for the normal-UI bridge and
// reserves BLE early enough for Wi-Fi scanning and NimBLE to coexist. Keep the
// defaults in this shared header so the sketch and separately compiled bridge
// translation unit receive the same configuration. Define PIGGYBLUES_DISABLE
// to build the legacy no-BLE stub instead.
#if !defined(PIGGYBLUES_DISABLE)
  #define PIGGYBLUES_BLE
  #define PB_BRIDGE_NORMAL_MODE
#endif
// PIGGY BLUES BLE bridge — public API.
//
// Turns porkchop into a WarDriveGo (GhostLink) node over BLE: the phone app
// discovers it by service UUID, subscribes, and receives the WiFi APs porkchop
// has already scanned. porkchop keeps scanning WiFi the whole time — the bridge
// only reads the existing dedup'd table, exactly as umbravox's bridge does.
//
// Lifecycle note (the open question from the 0ct0 notes): unlike umbravox, which
// rides a NimBLE stack its advert-scanner already brought up, porkchop has no
// other BLE user. So this module OWNS BLEDevice::init() and brings NimBLE up on
// enable, concurrently with the live WiFi driver. That first-time init is the
// one real memory risk; setEnabled() reports failure if it can't come up.
//
// Gated behind PIGGYBLUES_BLE. Without that build flag every entry point below
// is a no-op, so the default ws-c5 firmware is unchanged.
#include <stdint.h>
#include "piggyblues_bridge_protocol.h"

namespace pbble {

struct Stats {
    bool enabled = false;
    bool connected = false;
    bool streaming = false;
    uint16_t queued = 0;
    uint32_t sent = 0;     // AP lines handed to the host this session
    uint32_t rxDrops = 0;
    uint32_t txRetries = 0;
    uint32_t captured = 0;
    uint32_t scanDrops = 0;
    uint32_t connections = 0;
    uint32_t disconnects = 0;
    int32_t lastDisconnectReason = 0;
};

// AP source. The bridge calls this to pull the next eligible network for the
// given cursor. Return 1 and fill `ap` for a record, 0 to skip (stale/filtered),
// -1 when the cursor has passed the end of the table. The provider owns cursor
// advancement. Registered from the sketch, where the scan table is visible.
typedef int (*ApProvider)(uint32_t& cursor, pbwire::Ap& ap, uint32_t now);
void setApProvider(ApProvider provider);
void setScanMetrics(uint32_t generation, uint32_t captured, uint32_t dropped);

// Enable/disable the WarDriveGo bridge. Enabling brings up NimBLE (if needed)
// and starts advertising; returns false if BLE could not initialize. Disabling
// stops advertising and drops any link but leaves the stack up for reuse.
bool setEnabled(bool on);
bool enabled();

// Gate discoverability without tearing down the reserved controller/host.
// Normal firmware uses this to reserve BLE memory early, then advertise only
// after display, GPS and audio startup have completed.
void setAdvertisingAllowed(bool allowed);

// Pump the bridge. Call every loop pass with millis().
void tick(uint32_t now);

Stats stats();
const char* deviceName();           // "Porkchop-XXXX" once up, else a placeholder
const char* status();               // OFF / ADVERTISING / CONNECTED / STREAMING

// Instrumentation (directive 2026-11-04). heapProbe logs free + largest-block for
// cap0=INTERNAL, cap1=INTERNAL|8BIT, cap2=INTERNAL|8BIT|DMA (matching the BSP
// probe), plus the running failed-allocation count. installFailedAllocHook arms a
// non-allocating, non-blocking hook that only records the last failed size/caps.
void heapProbe(const char* phase);
void installFailedAllocHook();
uint32_t connectionCount();         // number of GATT connects since boot

} // namespace pbble
