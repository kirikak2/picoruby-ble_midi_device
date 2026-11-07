/*
 * PicoRuby BLE-MIDI Device - ESP32 port (NimBLE)
 *
 * The NimBLE host is picoruby-ble's (its shared owner, nimble_owner.h): this
 * gem is one client of it, with the MIDI service as its GATT setup.
 *
 *   NimBLE host task : GAP events, characteristic writes -> decoder -> RX ring
 *   any task         : BLE_MIDI_DEVICE_send_packet -> TX ring, arms the batch timer
 *   batch timer      : wakes the gem's task BLE_MIDI_DEVICE_TX_BATCH_US later
 *   gem's task       : TX ring -> encoder -> notifications; enable / disable
 */

#include "../../include/ble_midi_device_config.h"
#include "../../include/ble_midi_device.h"

#if BLE_MIDI_DEVICE_ENABLED

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "nimble_owner.h"       /* picoruby-ble's ESP32 port */
#include "ble_midi_codec.h"     /* picoruby-midi */
#include "midi_timed_ring.h"
#include "midi.h"
#include "midi_route.h"

void ble_store_config_init(void);   /* NimBLE's store/config, no header of its own */

static const char *TAG = "BLE_MIDI_DEV";

#define NB_TX          0x01
#define NB_ENABLE      0x02
#define NB_DISABLE     0x04
#define NB_DISCONNECT  0x08
#define NB_ADV         0x10    /* advertising free again (the BLE class let go) */

#define TX_MASK (BLE_MIDI_DEVICE_TX_PACKETS - 1)

/* Largest ATT MTU NimBLE takes is 527; a notification carries MTU - 3 */
#define TX_BUF_MAX 512

typedef struct {
    uint8_t  p[4];
    uint16_t ts;
} tx_entry_t;

/* --- state --- */
static bool s_inited;
static midi_transport_t s_transport;
static midi_tmask_t s_bit;
static char s_name[30] = BLE_MIDI_DEVICE_NAME;

static volatile bool s_wanted;      /* enable(true) asked for */
static volatile bool s_held;        /* we hold the NimBLE host */
static volatile ble_midi_device_state_t s_state = BLE_MIDI_DEVICE_OFF;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static volatile uint16_t s_mtu = 23;
static uint16_t s_val_handle;

static TaskHandle_t s_task;
static esp_timer_handle_t s_batch_timer;

/* RX: single producer (host task), single consumer (input task or Ruby) */
static midi_timed_ring_t *s_rx;
static ble_midi_decoder_t s_dec;
static ble_midi_sync_t s_sync;
static volatile int32_t s_rx_latency = BLE_MIDI_DEVICE_RX_LATENCY_US;
static int64_t s_rx_now;            /* arrival of the packet being decoded */
static int32_t s_rx_budget;         /* its correction */

/* TX: several producers, the gem's task consumes */
static tx_entry_t *s_tx;
static volatile uint32_t s_tx_head, s_tx_tail;
static portMUX_TYPE s_tx_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_txbuf;            /* TX_BUF_MAX */
static size_t s_tx_pending;         /* a notification refused, to send again */
static uint8_t s_tx_done;           /* how far the oldest TX entry got (SysEx) */
static ble_midi_encoder_t s_enc;
static bool s_enc_open;             /* s_enc holds a packet being filled */

static ble_midi_device_stats_t s_stats;

static int gap_event(struct ble_gap_event *event, void *arg);
static picoruby_nimble_client_t s_client;
static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg);

/* 03B80E5A-EDE8-4B33-A751-6CE34EC4C700 / 7772E5DB-3868-4112-A1A9-F2669D106BF3 */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x00, 0xC7, 0xC4, 0x4E, 0xE3, 0x6C, 0x51, 0xA7,
    0x33, 0x4B, 0xE8, 0xED, 0x5A, 0x0E, 0xB8, 0x03);
static const ble_uuid128_t s_chr_uuid = BLE_UUID128_INIT(
    0xF3, 0x6B, 0x10, 0x9D, 0x66, 0xF2, 0xA9, 0xA1,
    0x12, 0x41, 0x68, 0x38, 0xDB, 0xE5, 0x72, 0x77);

#if BLE_MIDI_DEVICE_REQUIRE_ENCRYPTION
#define CHR_SEC (BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_WRITE_ENC)
#else
#define CHR_SEC 0
#endif

static const struct ble_gatt_chr_def s_chrs[] = {
    {
        .uuid = &s_chr_uuid.u,
        .access_cb = chr_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY | CHR_SEC,
        .val_handle = &s_val_handle,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_chrs,
    },
    { 0 },
};

static inline uint16_t now_ts(void)
{
    return (uint16_t)((esp_timer_get_time() / 1000) & BLE_MIDI_TS_MASK);
}

static void wake(uint32_t bits)
{
    if (s_task) xTaskNotify(s_task, bits, eSetBits);
}

/* ---------------------------------------------------------------------
 * RX
 * ------------------------------------------------------------------- */

static int32_t rx_budget(void)
{
    int32_t l = s_rx_latency;
    if (l < 0) return ble_midi_sync_auto_budget(s_stats.interval_1250us);
    return l;
}

static void rx_push(void *arg, const uint8_t pkt[4], uint16_t ts)
{
    (void)arg;
    int64_t at = ble_midi_sync_release(&s_sync, ts, s_rx_now, s_rx_budget);
    if (!midi_timed_ring_push(s_rx, pkt, at)) {
        s_stats.rx_dropped++;
        return;
    }
    s_stats.rx_messages++;
}

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return 0;   /* BLE-MIDI: reads return no payload */
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        static uint8_t buf[TX_BUF_MAX];    /* the host task is the only caller */
        uint16_t len = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(buf)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_UNLIKELY;
        if (conn != s_conn) return 0;
        s_stats.rx_packets++;
        s_rx_now = esp_timer_get_time();
        s_rx_budget = rx_budget();
        if (ble_midi_decode(&s_dec, buf, len, rx_push, NULL) != 0) {
            s_stats.decode_errors = s_dec.errors;
        }
        midi_timed_ring_commit(s_rx);
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

int BLE_MIDI_DEVICE_bytes_available(void)
{
    if (s_rx == NULL) return 0;
    return midi_timed_ring_bytes_due(s_rx);
}

int BLE_MIDI_DEVICE_read_packet(uint8_t *buf, size_t maxlen)
{
    if (s_rx == NULL) return 0;
    return midi_timed_ring_read(s_rx, buf, maxlen);
}

void BLE_MIDI_DEVICE_set_rx_latency(int32_t us)
{
    if (us < -1) us = -1;
    if (us > 100000) us = 100000;
    s_rx_latency = us;
}

int32_t BLE_MIDI_DEVICE_rx_latency_setting(void) { return s_rx_latency; }

/* ---------------------------------------------------------------------
 * TX
 * ------------------------------------------------------------------- */

static void tx_clear(void)
{
    portENTER_CRITICAL(&s_tx_mux);
    s_tx_tail = s_tx_head;
    portEXIT_CRITICAL(&s_tx_mux);
    s_tx_pending = 0;
    s_tx_done = 0;
    s_enc_open = false;
    s_enc.sysex_open = false;
}

int BLE_MIDI_DEVICE_send_packet(uint8_t cable, uint8_t cin,
                                uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)cable;
    if (s_tx == NULL || !BLE_MIDI_DEVICE_is_connected()) {
        s_stats.tx_dropped++;
        return -1;
    }
    uint16_t ts = now_ts();
    portENTER_CRITICAL(&s_tx_mux);
    uint32_t head = s_tx_head;
    uint32_t next = (head + 1) & TX_MASK;
    if (next == s_tx_tail) {
        portEXIT_CRITICAL(&s_tx_mux);
        s_stats.tx_dropped++;
        return -1;
    }
    s_tx[head].p[0] = cin & 0x0F;
    s_tx[head].p[1] = b1;
    s_tx[head].p[2] = b2;
    s_tx[head].p[3] = b3;
    s_tx[head].ts = ts;
    s_tx_head = next;
    portEXIT_CRITICAL(&s_tx_mux);
    s_stats.tx_messages++;
    if (!esp_timer_is_active(s_batch_timer)) {
        esp_timer_start_once(s_batch_timer, BLE_MIDI_DEVICE_TX_BATCH_US);
    }
    return 0;
}

static void batch_timer_cb(void *arg)
{
    (void)arg;
    wake(NB_TX);
}

/* true: sent (or the link is gone and it is dropped); false: try again later */
static bool notify(const uint8_t *buf, size_t len)
{
    uint16_t conn = s_conn;
    if (conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed) return true;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)len);
    if (om == NULL) {
        s_stats.tx_retries++;
        return false;
    }
    int rc = ble_gatts_notify_custom(conn, s_val_handle, om);   /* consumes om */
    if (rc == 0) {
        s_stats.tx_packets++;
        return true;
    }
    if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY) {
        s_stats.tx_retries++;
        return false;
    }
    return true;
}

/* Returns true when it has to run again soon (a notification was refused) */
static bool tx_flush(void)
{
    if (!BLE_MIDI_DEVICE_is_connected()) {
        tx_clear();
        return false;
    }
    if (s_tx_pending) {
        if (!notify(s_txbuf, s_tx_pending)) return true;
        s_tx_pending = 0;
        s_enc_open = false;
    }
    size_t cap = (size_t)s_mtu - 3;
    if (cap > TX_BUF_MAX) cap = TX_BUF_MAX;
    if (cap < 5) cap = 5;

    for (;;) {
        tx_entry_t ent;
        portENTER_CRITICAL(&s_tx_mux);
        bool empty = (s_tx_tail == s_tx_head);
        if (!empty) ent = s_tx[s_tx_tail];
        portEXIT_CRITICAL(&s_tx_mux);
        if (empty) break;

        if (!s_enc_open) {
            ble_midi_encoder_begin(&s_enc, s_txbuf, cap, ent.ts);
            s_enc_open = true;
        }
        uint8_t done = s_tx_done;
        if (ble_midi_encode(&s_enc, ent.p, ent.ts, &done)) {
            s_tx_done = 0;
            portENTER_CRITICAL(&s_tx_mux);
            s_tx_tail = (s_tx_tail + 1) & TX_MASK;
            portEXIT_CRITICAL(&s_tx_mux);
            continue;
        }
        s_tx_done = done;
        /* packet full */
        if (!notify(s_txbuf, s_enc.len)) {
            s_tx_pending = s_enc.len;
            return true;
        }
        s_enc_open = false;
    }
    if (s_enc_open && ble_midi_encoder_pending(&s_enc)) {
        if (!notify(s_txbuf, s_enc.len)) {
            s_tx_pending = s_enc.len;
            return true;
        }
    }
    s_enc_open = false;
    return false;
}

/* ---------------------------------------------------------------------
 * GAP / advertising
 * ------------------------------------------------------------------- */

static void adv_start(void)
{
    if (!s_wanted || !s_held || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    if (!picoruby_nimble_synced()) return;
    /* one advertising at a time on the shared host; the BLE class (a
     * script's peripheral) goes first and we wait for proc_free */
    if (!picoruby_nimble_claim(&s_client, PICORUBY_NIMBLE_PROC_ADV)) {
        if (s_state == BLE_MIDI_DEVICE_ADVERTISING) {
            ESP_LOGI(TAG, "advertising paused: the BLE class advertises");
        }
        s_state = BLE_MIDI_DEVICE_STARTING;
        return;
    }
    if (ble_gap_adv_active()) return;     /* ours, going */

    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &s_svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising data: %d", rc);
        picoruby_nimble_unclaim(&s_client, PICORUBY_NIMBLE_PROC_ADV);
        s_state = BLE_MIDI_DEVICE_ERROR;
        return;
    }
    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof(r));
    r.name = (uint8_t *)s_name;
    r.name_len = (uint8_t)strlen(s_name);
    r.name_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&r);
    if (rc != 0) ESP_LOGW(TAG, "scan response: %d", rc);

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = BLE_MIDI_DEVICE_ADV_ITVL;
    p.itvl_max = BLE_MIDI_DEVICE_ADV_ITVL;
    rc = ble_gap_adv_start(picoruby_nimble_own_addr_type(), NULL, BLE_HS_FOREVER,
                           &p, gap_event, NULL);
    if (rc == 0 || rc == BLE_HS_EALREADY) {
        s_state = BLE_MIDI_DEVICE_ADVERTISING;
    } else {
        ESP_LOGE(TAG, "advertising: %d", rc);
        picoruby_nimble_unclaim(&s_client, PICORUBY_NIMBLE_PROC_ADV);
        s_state = BLE_MIDI_DEVICE_ERROR;
    }
}

static void read_conn(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(conn, &desc) != 0) return;
    for (int i = 0; i < 6; i++) s_stats.peer[i] = desc.peer_id_addr.val[5 - i];
    s_stats.peer_type = desc.peer_id_addr.type;
    s_stats.encrypted = desc.sec_state.encrypted;
    s_stats.bonded = desc.sec_state.bonded;
    s_stats.interval_1250us = desc.conn_itvl;
}

static void link_down(void)
{
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    s_mtu = 23;
    tx_clear();
    ble_midi_decoder_reset(&s_dec);
    ble_midi_sync_reset(&s_sync);
    if (s_rx) midi_timed_ring_clear(s_rx);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* the advertising that made it is over */
        picoruby_nimble_unclaim(&s_client, PICORUBY_NIMBLE_PROC_ADV);
        if (event->connect.status == 0) {
            uint16_t conn = event->connect.conn_handle;
            link_down();
            s_conn = conn;
            s_state = BLE_MIDI_DEVICE_LINKED;
            s_stats.connections++;
            read_conn(conn);
            uint16_t mtu = ble_att_mtu(conn);
            if (mtu >= 23) s_mtu = mtu;
            ESP_LOGI(TAG, "connected %02X:%02X:%02X:%02X:%02X:%02X",
                     s_stats.peer[0], s_stats.peer[1], s_stats.peer[2],
                     s_stats.peer[3], s_stats.peer[4], s_stats.peer[5]);
            if (!s_wanted) {
                ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
#if BLE_MIDI_DEVICE_CONN_ITVL_MIN > 0
            if (s_stats.interval_1250us > BLE_MIDI_DEVICE_CONN_ITVL_MAX) {
                struct ble_gap_upd_params up = {
                    .itvl_min = BLE_MIDI_DEVICE_CONN_ITVL_MIN,
                    .itvl_max = BLE_MIDI_DEVICE_CONN_ITVL_MAX,
                    .latency = 0,
                    .supervision_timeout = 200,   /* 2 s */
                    .min_ce_len = 0,
                    .max_ce_len = 0,
                };
                ble_gap_update_params(conn, &up);
            }
#endif
        } else {
            adv_start();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle == s_conn) {
            ESP_LOGI(TAG, "disconnected (reason 0x%03X)", event->disconnect.reason);
            link_down();
            s_state = s_wanted ? BLE_MIDI_DEVICE_ADVERTISING : BLE_MIDI_DEVICE_OFF;
            adv_start();
        }
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.conn_handle == s_conn) read_conn(s_conn);
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        picoruby_nimble_unclaim(&s_client, PICORUBY_NIMBLE_PROC_ADV);
        adv_start();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == s_conn
            && event->subscribe.attr_handle == s_val_handle) {
            bool was = s_subscribed;
            s_subscribed = event->subscribe.cur_notify;
            s_state = s_subscribed ? BLE_MIDI_DEVICE_CONNECTED : BLE_MIDI_DEVICE_LINKED;
            if (s_subscribed && !was) {
                /* routes made before the link came up want the input task */
                if (MIDI_Input_was_started() && !MIDI_Input_is_running()) MIDI_Input_start();
            }
            if (!s_subscribed) tx_clear();
        }
        break;

    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == s_conn) s_mtu = event->mtu.value;
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.conn_handle == s_conn) read_conn(s_conn);
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The central lost its bond and pairs again: forget ours */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_NOTIFY_TX:
        if (event->notify_tx.status != 0 && event->notify_tx.status != BLE_HS_EDONE) {
            wake(NB_TX);
        }
        break;

    default:
        break;
    }
    return 0;
}

/* ---------------------------------------------------------------------
 * NimBLE client
 * ------------------------------------------------------------------- */

static int client_setup(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(s_name);
    int rc = ble_gatts_count_cfg(s_svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(s_svcs);
    if (rc != 0) ESP_LOGE(TAG, "MIDI service: %d", rc);
    return rc;
}

static void client_configure(void)
{
    /* Just Works pairing, bonded, when the central asks for it */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    /* The bond store is set up again on every host start (the host frees it
     * when it stops); joining a running host leaves it as it is */
    if (!picoruby_nimble_started()) ble_store_config_init();
}

static void client_on_sync(void)
{
    adv_start();
}

static void client_on_reset(int reason)
{
    (void)reason;
    link_down();
    if (s_wanted) s_state = BLE_MIDI_DEVICE_STARTING;
}

/* The BLE class took the advertising (its peripheral advertises) */
static void client_yield(int proc)
{
    if (proc != PICORUBY_NIMBLE_PROC_ADV) return;
    ble_gap_adv_stop();
    if (s_conn == BLE_HS_CONN_HANDLE_NONE && s_wanted) s_state = BLE_MIDI_DEVICE_STARTING;
}

static void client_proc_free(int proc)
{
    if (proc == PICORUBY_NIMBLE_PROC_ADV) wake(NB_ADV);
}

static picoruby_nimble_client_t s_client = {
    .name = "BLE-MIDI device",
    .setup = client_setup,
    .configure = client_configure,
    .on_sync = client_on_sync,
    .on_reset = client_on_reset,
    .yield = client_yield,
    .proc_free = client_proc_free,
    .provides_gap = true,           /* ble_svc_gap_init / ble_svc_gatt_init */
};

static void do_enable(void)
{
    if (!s_wanted || s_held) return;
    s_state = BLE_MIDI_DEVICE_STARTING;
    int rc = picoruby_nimble_acquire(&s_client);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE host did not start");
        s_state = BLE_MIDI_DEVICE_ERROR;
        return;
    }
    s_held = true;
    ESP_LOGI(TAG, "advertising as \"%s\"", s_name);
    adv_start();
}

static void do_disable(void)
{
    if (s_wanted || !s_held) return;
    uint16_t conn = s_conn;
    if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
    if (picoruby_nimble_holds(&s_client, PICORUBY_NIMBLE_PROC_ADV)) {
        ble_gap_adv_stop();
        picoruby_nimble_unclaim(&s_client, PICORUBY_NIMBLE_PROC_ADV);
    }
    link_down();
    s_held = false;
    picoruby_nimble_release(&s_client);
    s_state = BLE_MIDI_DEVICE_OFF;
}

static void device_task(void *arg)
{
    (void)arg;
    bool again = false;
    for (;;) {
        uint32_t bits = 0;
        TickType_t wait = again ? 1 : portMAX_DELAY;
        xTaskNotifyWait(0, UINT32_MAX, &bits, wait);
        if (bits & NB_ENABLE) do_enable();
        if (bits & NB_DISABLE) do_disable();
        if ((bits & NB_ADV) && s_held) adv_start();
        if (bits & NB_DISCONNECT) {
            uint16_t conn = s_conn;
            if (conn != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        again = tx_flush();
    }
}

/* ---------------------------------------------------------------------
 * Transport + public API
 * ------------------------------------------------------------------- */

static int t_send(void *ctx, uint8_t cable, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)ctx;
    return BLE_MIDI_DEVICE_send_packet(cable, cin, b1, b2, b3);
}
static int t_read(void *ctx, uint8_t *buf, size_t maxlen) { (void)ctx; return BLE_MIDI_DEVICE_read_packet(buf, maxlen); }
static int t_avail(void *ctx) { (void)ctx; return BLE_MIDI_DEVICE_bytes_available(); }
static bool t_connected(void *ctx) { (void)ctx; return BLE_MIDI_DEVICE_is_connected(); }

static const midi_transport_ops_t s_ops = {
    .send_packet     = t_send,
    .read_bytes      = t_read,
    .bytes_available = t_avail,
    .is_connected    = t_connected,
    .transport_id    = MIDI_TRANSPORT_ID_BLE,
};

int BLE_MIDI_DEVICE_init(void)
{
    if (s_inited) return 0;
    /* Buffers off the .bss (docs/MEMORY_ALLOCATION.md); PSRAM is fine */
    if (s_rx == NULL) s_rx = midi_timed_ring_new(BLE_MIDI_DEVICE_RX_PACKETS, "ble_midi_dev_rx");
    s_tx = heap_caps_calloc(BLE_MIDI_DEVICE_TX_PACKETS, sizeof(tx_entry_t), MALLOC_CAP_8BIT);
    s_txbuf = heap_caps_malloc(TX_BUF_MAX, MALLOC_CAP_8BIT);
    if (!s_rx || !s_tx || !s_txbuf) {
        ESP_LOGE(TAG, "out of memory");
        goto fail;
    }
    const esp_timer_create_args_t targs = {
        .callback = batch_timer_cb,
        .name = "ble_midi_dev",
    };
    if (esp_timer_create(&targs, &s_batch_timer) != ESP_OK) goto fail;
    if (xTaskCreatePinnedToCore(device_task, "ble_midi_dev", BLE_MIDI_DEVICE_TASK_STACK, NULL,
                                BLE_MIDI_DEVICE_TASK_PRIORITY, &s_task,
                                BLE_MIDI_DEVICE_TASK_CORE) != pdPASS) {
        esp_timer_delete(s_batch_timer);
        s_batch_timer = NULL;
        goto fail;
    }
    ble_midi_decoder_reset(&s_dec);

    s_transport.ops = &s_ops;
    s_transport.ctx = NULL;
    s_bit = MIDI_transport_register(&s_transport);
    if (s_bit == 0) {
        ESP_LOGW(TAG, "MIDI transport registry full: BLE-MIDI device is no transport");
    } else {
        MIDI_Input_add_packet_source(s_bit);
        MIDI_Input_track_clock(s_bit);   /* external BPM (Tempo's BLE source) */
        ESP_LOGI(TAG, "BLE-MIDI device is transport 0x%04X", (unsigned)s_bit);
    }
    s_inited = true;
    return 0;

fail:
    /* (the RX ring is kept: init is not retried after running out) */
    heap_caps_free(s_tx); s_tx = NULL;
    heap_caps_free(s_txbuf); s_txbuf = NULL;
    return -1;
}

bool BLE_MIDI_DEVICE_available(void) { return true; }

int BLE_MIDI_DEVICE_enable(bool on)
{
    if (BLE_MIDI_DEVICE_init() != 0) return -1;
    s_wanted = on;
    if (on && s_state == BLE_MIDI_DEVICE_ERROR) s_state = BLE_MIDI_DEVICE_OFF;
    if (!on && !s_held) s_state = BLE_MIDI_DEVICE_OFF;   /* nothing to take down */
    wake(on ? NB_ENABLE : NB_DISABLE);
    return 0;
}

bool BLE_MIDI_DEVICE_is_enabled(void) { return s_wanted; }

void BLE_MIDI_DEVICE_set_name(const char *name)
{
    if (name == NULL) return;
    strncpy(s_name, name, sizeof(s_name) - 1);
    s_name[sizeof(s_name) - 1] = '\0';
    if (s_held) ble_svc_gap_device_name_set(s_name);
}

const char *BLE_MIDI_DEVICE_name(void) { return s_name; }

bool BLE_MIDI_DEVICE_is_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE && s_subscribed;
}

midi_tmask_t BLE_MIDI_DEVICE_transport_bit(void) { return s_bit; }

void BLE_MIDI_DEVICE_disconnect(void) { wake(NB_DISCONNECT); }

void BLE_MIDI_DEVICE_stats(ble_midi_device_stats_t *out)
{
    if (out == NULL) return;
    *out = s_stats;
    out->state = s_state;
    out->mtu = s_mtu;
    out->decode_errors = s_dec.errors;
    out->rx_late = s_sync.late;
    out->rx_latency_us = rx_budget();
}

#else  /* !BLE_MIDI_DEVICE_ENABLED */

#include <string.h>

int  BLE_MIDI_DEVICE_init(void) { return -1; }
bool BLE_MIDI_DEVICE_available(void) { return false; }
int  BLE_MIDI_DEVICE_enable(bool on) { (void)on; return -1; }
bool BLE_MIDI_DEVICE_is_enabled(void) { return false; }
void BLE_MIDI_DEVICE_set_name(const char *name) { (void)name; }
const char *BLE_MIDI_DEVICE_name(void) { return ""; }
bool BLE_MIDI_DEVICE_is_connected(void) { return false; }
midi_tmask_t BLE_MIDI_DEVICE_transport_bit(void) { return 0; }
int  BLE_MIDI_DEVICE_send_packet(uint8_t cable, uint8_t cin, uint8_t b1, uint8_t b2, uint8_t b3)
{
    (void)cable; (void)cin; (void)b1; (void)b2; (void)b3;
    return -1;
}
int  BLE_MIDI_DEVICE_bytes_available(void) { return 0; }
int  BLE_MIDI_DEVICE_read_packet(uint8_t *buf, size_t maxlen) { (void)buf; (void)maxlen; return 0; }
void BLE_MIDI_DEVICE_disconnect(void) {}
void BLE_MIDI_DEVICE_set_rx_latency(int32_t us) { (void)us; }
int32_t BLE_MIDI_DEVICE_rx_latency_setting(void) { return 0; }
void BLE_MIDI_DEVICE_stats(ble_midi_device_stats_t *out)
{
    if (out) memset(out, 0, sizeof(*out));
}

#endif /* BLE_MIDI_DEVICE_ENABLED */
