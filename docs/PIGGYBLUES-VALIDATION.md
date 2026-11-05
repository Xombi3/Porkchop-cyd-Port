# PIGGY BLUES hardware validation

PIGGY BLUES bridges PorkChop's Wi-Fi observations to WarDriveGo over the
GhostLink BLE GATT protocol. This document records the behavior verified on the
Waveshare ESP32-C5-Touch-LCD-3.5.

## Build

- Arduino-ESP32 3.3.10, ESP32-C5 target.
- 8 MB flash, `huge_app` partition and PSRAM enabled.
- TFT_eSPI with ESP32-C5 support and this repository's `User_Setup.h`.
- PiggyBlues enabled by default; define `PIGGYBLUES_DISABLE` for the no-BLE
  fallback build.

Both the default bridge build and the fallback build compile successfully.

## Design

- NimBLE is reserved before display, GPS and audio allocations fragment
  internal RAM; advertising starts only after setup completes.
- PorkChop's promiscuous `NetworkRecon` scanner remains the single Wi-Fi scan
  source while BLE is active. The redundant asynchronous active scan is
  suppressed because it depleted the DMA pool and destabilized BLE.
- The 40-record UI recon table remains bounded in internal RAM. New observations
  rotate through that table and are harvested into a deduplicated 256-record
  catalog in PSRAM.
- Notifications are paced and retained for retry when the controller cannot
  accept them. Leaving the dashboard does not stop the bridge.
- The dashboard reports catalog APs, queued AP records, disconnects and
  notification retries. WarDriveGo's displayed count can be lower because the
  app applies its own BSSID deduplication, run, GPS and geofence rules.

## Verified results

- iOS and Android both completed GhostLink discovery, connection, capability
  negotiation and live AP ingestion.
- Automatic advertising and reconnection recovered after receiver restarts and
  supervision timeouts.
- Normal PorkChop UI, touch, audio, GPS and background Wi-Fi recon remained
  operational while the bridge streamed.
- A 1.72 km field run recorded 1,269 total Wi-Fi observations and 425 new Wi-Fi
  observations in WarDriveGo.
- The C5 catalog reached 256 APs and transmitted 2,420 AP records with zero
  notification retries or allocation failures during the validated run.

The exact hardware-tested application image had SHA-256
`39c84edc219c3f3f073dc14e13024b3939264f8eaf53df2d73916a38df9d8473`.
