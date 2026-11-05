#include "piggyblues_bridge.h"

#if defined(PIGGYBLUES_BLE)
// WarDriveGo (GhostLink) BLE peripheral for porkchop. Adapted from slickrick773's
// umbravox C5 bridge (device/ble_bridge.cpp), trimmed to the WarDriveGo surface.
// PorkChop has no other BLE user, so this module owns the native NimBLE host,
// controller, GATT service and advertising lifecycle.
#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <atomic>
#include <new>
#include <host/ble_hs.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>

namespace pbble {
namespace {
using namespace pbwire;

constexpr uint16_t NO_CONN = 0xffff;
std::atomic<uint16_t> connection{NO_CONN}, mtu{23};
std::atomic<unsigned> epoch{0};
std::atomic<bool> subscribed{false}, streaming{false}, selected{false};
std::atomic<uint32_t> g_connCount{0};   // increments on each GATT connect (for a post-reconnect marker)
std::atomic<uint32_t> g_disconnectCount{0}, g_scanGeneration{0}, g_scanCaptured{0}, g_scanDropped{0};
std::atomic<int32_t> g_lastDisconnectReason{0};
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

ApProvider apProvider = nullptr;

// --- Instrumentation: failed-alloc hook (must not allocate or block) ---------
struct FailRec { volatile uint32_t count = 0; volatile size_t size = 0; volatile uint32_t caps = 0; const char* volatile fn = nullptr; };
FailRec g_fail;
void IRAM_ATTR onAllocFail(size_t size, uint32_t caps, const char* fn) {
    // Record only (no alloc / no block). fn is a static string (caller's __func__),
    // so retaining the pointer is safe and gives allocation provenance.
    g_fail.size = size; g_fail.caps = caps; g_fail.fn = fn; g_fail.count = g_fail.count + 1;
}
bool g_hookInstalled = false;

struct Rx { unsigned epoch; uint16_t len; uint8_t data[244]; };
struct State {
    Outbox out; Command command; char scratch[MAX_LINE]{};
    Rx rx[4]{}; unsigned rxHead = 0, rxCount = 0; uint32_t rxDrops = 0;
    unsigned seenEpoch = 0; bool greeted = false;
    ChannelFilter channels;
    uint32_t reply = 1, cursor = 0, sent = 0, sweepAps = 0, sweeps = 0;
    uint32_t connectedAt = 0, lastTx = 0, roundAt = 0, advAt = 0, diagAt = 0, notifyFails = 0;
    volatile uint32_t connParamsAt = 0;
    volatile uint8_t connParamAttempts = 0;
    uint32_t txLines = 0, mbufNull = 0;
    bool roundDone = false;
};
std::atomic<uint32_t> lastAcceptedNotify{0};
State* st = nullptr;
bool stackStarted = false;
bool hostSynced = false;
std::atomic<bool> advertisingAllowed{true};
uint16_t wdTxHandle = 0;
char name[24]{};

const ble_uuid128_t serviceUuid = BLE_UUID128_INIT(
    0x76,0x53,0x65,0x67,0x64,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47);
const ble_uuid128_t rxUuid = BLE_UUID128_INIT(
    0x52,0x58,0x67,0x64,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47,0x01);
const ble_uuid128_t txUuid = BLE_UUID128_INIT(
    0x54,0x58,0x67,0x64,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47,0x02);
const ble_uuid128_t ctrlUuid = BLE_UUID128_INIT(
    0x43,0x54,0x52,0x4C,0x69,0x72,0x42,0x50,0x53,0x45,0x74,0x73,0x6F,0x68,0x47,0x03);

void clearSession(uint32_t now) {
    st->out.clear(); st->command.reset(); st->greeted = false;
    st->txLines = 0; st->reply = 1; st->cursor = 0; st->sent = 0; st->sweepAps = 0; st->sweeps = 0;
    st->channels = ChannelFilter{}; st->roundDone = false; st->connectedAt = now;
    streaming = false;
}
void resetRx() { if (st) { portENTER_CRITICAL(&rxMux); st->rxHead = st->rxCount = 0; portEXIT_CRITICAL(&rxMux); } }

void advertise();

int onCommandWrite(uint16_t connHandle, uint16_t, ble_gatt_access_ctxt* context, void*) {
    if (!st || !selected || connection != connHandle || context->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return 0;
    const size_t length = OS_MBUF_PKTLEN(context->om);
    portENTER_CRITICAL(&rxMux);
    if (length && length <= sizeof(Rx::data) && st->rxCount < 4) {
        Rx& rx = st->rx[(st->rxHead + st->rxCount) % 4];
        rx.epoch = epoch.load();
        rx.len = length;
        os_mbuf_copydata(context->om, 0, length, rx.data);
        ++st->rxCount;
    } else {
        ++st->rxDrops;
    }
    portEXIT_CRITICAL(&rxMux);
    return 0;
}

int onTxAccess(uint16_t, uint16_t, ble_gatt_access_ctxt*, void*) { return 0; }

const ble_gatt_chr_def characteristics[] = {
    { .uuid = &rxUuid.u, .access_cb = onCommandWrite,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &txUuid.u, .access_cb = onTxAccess,
      .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &wdTxHandle },
    { .uuid = &ctrlUuid.u, .access_cb = onCommandWrite,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { 0 },
};

const ble_gatt_svc_def services[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &serviceUuid.u, .characteristics = characteristics },
    { 0 },
};

int onGapEvent(ble_gap_event* event, void*) {
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) { advertise(); break; }
            if (!selected || connection != NO_CONN) {
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            connection = event->connect.conn_handle;
            mtu = 23;
            subscribed = false;
            resetRx();
            ++epoch;
            ++g_connCount;
            {
                ble_gap_conn_desc description{};
                if (ble_gap_conn_find(event->connect.conn_handle, &description) == 0)
                    Serial.printf("[blues] initial timing interval=%u latency=%u timeout=%u\n",
                                  description.conn_itvl, description.conn_latency,
                                  description.supervision_timeout);
                // This C5/iOS pairing can begin with supervision_timeout=72
                // (720 ms). Request safer timing promptly; if iOS has an LL
                // update in flight, tick() retries the controller-busy result.
                if (st) { st->connParamsAt = millis() + 100; st->connParamAttempts = 0; }
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT: {
            const uint32_t last = lastAcceptedNotify.load();
            ++g_disconnectCount;
            g_lastDisconnectReason = event->disconnect.reason;
            Serial.printf("[blues] disconnect reason=%d at=%lu last-notify=%lu age=%lu\n",
                          event->disconnect.reason, (unsigned long)millis(),
                          (unsigned long)last, (unsigned long)(millis() - last));
            connection = NO_CONN;
            subscribed = false;
            streaming = false;
            resetRx();
            ++epoch;
            advertise();
            break;
        }
        case BLE_GAP_EVENT_SUBSCRIBE:
            if (connection == event->subscribe.conn_handle && event->subscribe.attr_handle == wdTxHandle) {
                subscribed = event->subscribe.cur_notify;
                resetRx();
                ++epoch;
            }
            break;
        case BLE_GAP_EVENT_MTU:
            if (connection == event->mtu.conn_handle) mtu = event->mtu.value;
            break;
        case BLE_GAP_EVENT_CONN_UPDATE: {
            ble_gap_conn_desc description{};
            if (ble_gap_conn_find(event->conn_update.conn_handle, &description) == 0)
                Serial.printf("[blues] timing status=%d interval=%u latency=%u timeout=%u\n",
                              event->conn_update.status, description.conn_itvl,
                              description.conn_latency, description.supervision_timeout);
            if (st && event->conn_update.status != 0 && st->connParamAttempts < 3)
                st->connParamsAt = millis() + 1000;
            break;
        }
        default: break;
    }
    return 0;
}

bool init() {
    if (stackStarted) return true;
    installFailedAllocHook();
    heapProbe("pre-ble-init");
    if (!st) {
        void* p = heap_caps_malloc(sizeof(State), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_malloc(sizeof(State), MALLOC_CAP_8BIT); // PSRAM preferred, internal fallback
        if (!p) return false;
        st = new (p) State{};
    }
    uint8_t mac[6]{};
    if (esp_read_mac(mac, ESP_MAC_BT) != ESP_OK) return false;
    formatDeviceName(name, sizeof name, mac);
    // Force Arduino's BLEDevice translation unit into the final image.  The C5
    // core keeps controller bootstrap glue in that archive member; calling the
    // native NimBLE entry point from an otherwise-native sketch left it out and
    // crashed in r_ble_log_internal_x0 during esp_bt_controller_init().
    (void)BLEDevice::getInitialized();
    if (nimble_port_init() != ESP_OK) return false;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(services) != 0 || ble_gatts_add_svcs(services) != 0) return false;
    if (ble_svc_gap_device_name_set(name) != 0) return false;
    ble_att_set_preferred_mtu(247);
    ble_hs_cfg.sync_cb = [] {
        hostSynced = true;
        advertise();
    };
    ble_hs_cfg.reset_cb = [](int reason) {
        hostSynced = false;
        Serial.printf("[blues] host reset reason=%d\n", reason);
    };
    nimble_port_freertos_init([](void*) {
        nimble_port_run();
        nimble_port_freertos_deinit();
    });
    stackStarted = true;
    const bool ok = true;
    heapProbe("post-gatt");
    Serial.printf("[blues] BLE up=%d heap=%u\n", int(ok), ESP.getFreeHeap());
    return ok;
}

void advertise() {
    if (!advertisingAllowed || !hostSynced || ble_gap_adv_active()) return;
    heapProbe("pre-adv");
    ble_hs_adv_fields fields{};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = const_cast<ble_uuid128_t*>(&serviceUuid);
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    const int fieldsResult = ble_gap_adv_set_fields(&fields);
    uint8_t addressType = 0;
    int result = fieldsResult ? fieldsResult : ble_hs_id_infer_auto(0, &addressType);
    ble_gap_adv_params parameters{};
    parameters.conn_mode = BLE_GAP_CONN_MODE_UND;
    parameters.disc_mode = BLE_GAP_DISC_MODE_GEN;
    if (!result) result = ble_gap_adv_start(addressType, nullptr, BLE_HS_FOREVER,
                                             &parameters, onGapEvent, nullptr);
    heapProbe(result == 0 ? "post-adv-ok" : "post-adv-fail");
    Serial.printf("[blues] advertising rc=%d\n", result);
}

void greet() {
    snprintf(st->scratch, sizeof st->scratch, "%s chans=auto\n", WD_CAPS);
    st->out.put(st->scratch, st->reply);
    st->out.put("WD:MODE mode=wifi\n", st->reply);
    st->out.put("WD:BEGIN type=wifi interval=0 channel=auto\n", st->reply);
    st->out.put("WD:PLAN chans=auto\n", st->reply);
    st->greeted = true;
}

void command(const Rx& rx, uint32_t now) {
    const int result = st->command.accept(rx.data, rx.len, now);
    if (result == 0) return;
    if (result < 0) { st->out.put("Malformed command", st->reply, 5, 1); return; }
    st->reply = st->command.id;
    Serial.printf("[blues] command %s\n", st->command.text);
    const Parsed parsed = parse(st->command.text);
    if (parsed.action == STOP || parsed.action == START) discardUnsent(st->out);
    if (st->command.framed) st->out.put("", st->reply, 2); // framed ACK
    if (parsed.action == START) {
        streaming = true; st->channels = parsed.channels; st->cursor = 0; st->sent = 0; st->sweepAps = 0; st->sweeps = 0;
        st->roundDone = false; st->roundAt = now;
        greet();
    } else if (parsed.action == STOP) {
        streaming = false;
        st->out.put("WD:END reason=stop\n", st->reply);
        st->out.put("", st->reply, 4);
    } else if (parsed.action == IDENTITY) {
        // WDGO phone announcement; framed ACK already queued, nothing else to do.
    } else {
        st->out.put("Unsupported command", st->reply, 5, 1);
    }
}

void transmit(uint32_t now) {
    if (!subscribed || connection == NO_CONN || st->seenEpoch != epoch.load()) return;
    // Pace notifications to the connection interval (~30-60ms). Blasting faster
    // overruns the controller's tx buffers and silently drops lines over-air
    // (measured: 50 host-accepted -> ~8 received). 50ms stays under link capacity.
    if (uint32_t(now - st->lastTx) < 50) return;
    Line* line = st->out.front();
    if (!line) return;
    uint8_t bytes[244]; size_t n = 0;
    const size_t len = packet(*line, mtu.load(), bytes, sizeof bytes, n);
    if (!len) return;
    os_mbuf* buffer = ble_hs_mbuf_from_flat(bytes, len);
    if (!buffer) ++st->mbufNull; // DIRECT attribution: the notification mbuf alloc itself failed
    const int rc = buffer ? ble_gatts_notify_custom(connection.load(), wdTxHandle, buffer) : BLE_HS_ENOMEM;
    st->lastTx = now;
    if (rc) { ++st->notifyFails; return; } // same bytes retained; never advance on failure
    lastAcceptedNotify = now;
    line->offset += n;
    if (line->offset == line->len) { st->out.pop(); ++st->txLines; }
}

} // anonymous namespace

void setApProvider(ApProvider provider) { apProvider = provider; }
void setScanMetrics(uint32_t generation, uint32_t captured, uint32_t dropped) {
    g_scanGeneration = generation;
    g_scanCaptured = captured;
    g_scanDropped = dropped;
}
uint32_t connectionCount() { return g_connCount.load(); }

void installFailedAllocHook() {
    if (g_hookInstalled) return;
    // Records only; does not allocate or block in the failure path.
    g_hookInstalled = (heap_caps_register_failed_alloc_callback(onAllocFail) == ESP_OK);
}

void heapProbe(const char* phase) {
    const uint32_t c0 = MALLOC_CAP_INTERNAL;
    const uint32_t c1 = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t c2 = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA;
    Serial.printf("[probe] %-13s cap0 f=%u l=%u | cap1 f=%u l=%u | cap2(DMA) f=%u l=%u | fail=%u last=%u/0x%x/%s\n",
        phase,
        (unsigned)heap_caps_get_free_size(c0), (unsigned)heap_caps_get_largest_free_block(c0),
        (unsigned)heap_caps_get_free_size(c1), (unsigned)heap_caps_get_largest_free_block(c1),
        (unsigned)heap_caps_get_free_size(c2), (unsigned)heap_caps_get_largest_free_block(c2),
        (unsigned)g_fail.count, (unsigned)g_fail.size, (unsigned)g_fail.caps,
        g_fail.fn ? g_fail.fn : "-");
}

bool setEnabled(bool on) {
    if (on == selected) return on ? stackStarted : true;
    selected = on;
    if (on) {
        if (!init()) { selected = false; Serial.println("[blues] bridge unavailable"); return false; }
        clearSession(millis());
        st->seenEpoch = epoch.load();
        Serial.printf("[blues] WarDriveGo bridge enabled as %s\n", name);
    } else {
        if (stackStarted) {
            if (ble_gap_adv_active()) ble_gap_adv_stop();
            if (connection != NO_CONN) ble_gap_terminate(connection.load(), BLE_ERR_REM_USER_CONN_TERM);
            delay(20); // let the disconnect/advertising-stop settle before teardown
            nimble_port_stop();
            nimble_port_deinit();
            stackStarted = false;
            hostSynced = false;
            wdTxHandle = 0;
        }
        connection = NO_CONN; subscribed = false; streaming = false; resetRx(); ++epoch;
        Serial.printf("[blues] bridge off, heap=%u\n", ESP.getFreeHeap());
    }
    return true;
}
bool enabled() { return selected.load(); }

void setAdvertisingAllowed(bool allowed) {
    advertisingAllowed = allowed;
    if (!stackStarted) return;
    if (!allowed) {
        if (ble_gap_adv_active()) ble_gap_adv_stop();
        return;
    }
    if (selected && connection == NO_CONN) advertise();
}

void tick(uint32_t now) {
    if (!st || !stackStarted || !selected) return;
    if (st->seenEpoch != epoch.load()) { clearSession(now); st->seenEpoch = epoch.load(); }
#if defined(PB_BRIDGE_DIAGNOSTICS)
    // Optional 1 Hz DMA-pool sampler for lab builds.
    static uint32_t sampleAt = 0;
    if (uint32_t(now - sampleAt) >= 1000) {
        sampleAt = now;
        heapProbe(connection == NO_CONN ? "adv-idle" : "connected");
    }
#endif
    if (connection == NO_CONN) {
        if (uint32_t(now - st->advAt) >= 4000) {   // backoff: don't hammer a starved pool
            st->advAt = now;
            if (!ble_gap_adv_active()) advertise();
        }
        return;
    }
    if (st->connParamsAt && int32_t(now - st->connParamsAt) >= 0 && st->connParamAttempts < 3) {
        st->connParamsAt = 0;
        ++st->connParamAttempts;
        const ble_gap_upd_params parameters{
            .itvl_min = 24, .itvl_max = 40, .latency = 0,
            .supervision_timeout = 500, .min_ce_len = 0, .max_ce_len = 0,
        };
        const int updateResult = ble_gap_update_params(connection.load(), &parameters);
        Serial.printf("[blues] delayed timing request attempt=%u rc=%d\n",
                      unsigned(st->connParamAttempts), updateResult);
        if (updateResult != 0 && st->connParamAttempts < 3) st->connParamsAt = now + 1000;
    }
    if (!subscribed) return;
    if (!st->greeted) { greet(); heapProbe("on-connect"); streaming = true; st->roundAt = now; }
    { // service one RX command per pass, even under notification backpressure
        Rx rx{}; bool have = false;
        portENTER_CRITICAL(&rxMux);
        if (st->rxCount) { rx = st->rx[st->rxHead]; st->rxHead = (st->rxHead + 1) % 4; --st->rxCount; have = true; }
        portEXIT_CRITICAL(&rxMux);
        if (have && rx.epoch == st->seenEpoch) command(rx, now);
    }
    if (streaming && apProvider && apReady(now, st->connectedAt, st->out.free())) {
        if (restartRound(st->roundDone, now, st->roundAt)) { st->roundDone = false; st->cursor = 0; st->roundAt = now; }
        Ap ap;
        const int result = st->roundDone ? 0 : apProvider(st->cursor, ap, now);
        if (result < 0 && !st->roundDone) {
            st->roundDone = true;
            char line[192];
            snprintf(line, sizeof line,
                     "WD:STATUS aps=%lu scans=%lu generation=%lu captured=%lu dropped=%lu connects=%lu disconnects=%lu last_reason=%ld\n",
                     (unsigned long)st->sweepAps, (unsigned long)++st->sweeps,
                     (unsigned long)g_scanGeneration.load(), (unsigned long)g_scanCaptured.load(),
                     (unsigned long)g_scanDropped.load(), (unsigned long)g_connCount.load(),
                     (unsigned long)g_disconnectCount.load(), (long)g_lastDisconnectReason.load());
            st->out.put(line, st->reply);
            st->sweepAps = 0;
        } else if (result > 0 && st->channels.allows(ap.channel)) {
            st->roundDone = false;
            if (apLine(st->scratch, sizeof st->scratch, ap)) { st->out.put(st->scratch, st->reply); ++st->sent; ++st->sweepAps; }
        }
    }
    transmit(now);
    if (uint32_t(now - st->diagAt) >= 10000) {
        st->diagAt = now;
        heapProbe("streaming");
        Serial.printf("[blues] stream %u sent %lu queue %u rx-drop %lu tx-retry %lu mbuf-null %lu heap %u mtu %u\n",
                      unsigned(streaming.load()), (unsigned long)st->sent, st->out.count,
                      (unsigned long)st->rxDrops, (unsigned long)st->notifyFails,
                      (unsigned long)st->mbufNull, unsigned(ESP.getFreeHeap()), unsigned(mtu.load()));
    }
}

Stats stats() {
    Stats s;
    s.enabled = selected.load();
    s.connected = connection != NO_CONN;
    s.streaming = streaming.load();
    s.captured = g_scanCaptured.load(); s.scanDrops = g_scanDropped.load();
    s.connections = g_connCount.load(); s.disconnects = g_disconnectCount.load();
    s.lastDisconnectReason = g_lastDisconnectReason.load();
    if (st) {
        s.queued = st->out.count; s.sent = st->sent; s.txRetries = st->notifyFails;
        portENTER_CRITICAL(&rxMux); s.rxDrops = st->rxDrops; portEXIT_CRITICAL(&rxMux);
    }
    return s;
}
const char* deviceName() { return name[0] ? name : "NOT STARTED"; }
const char* status() { return !selected ? "OFF" : streaming ? "STREAMING" : connection != NO_CONN ? "CONNECTED" : "ADVERTISING"; }

} // namespace pbble

#else // !PIGGYBLUES_BLE — bridge compiled out; default ws-c5 firmware unchanged.
namespace pbble {
void setApProvider(ApProvider) {}
void setScanMetrics(uint32_t, uint32_t, uint32_t) {}
bool setEnabled(bool) { return false; }
bool enabled() { return false; }
void setAdvertisingAllowed(bool) {}
void tick(uint32_t) {}
Stats stats() { return {}; }
const char* deviceName() { return "DISABLED"; }
const char* status() { return "DISABLED"; }
}
#endif
