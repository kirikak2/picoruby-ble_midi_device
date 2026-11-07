/*
 * PicoRuby BLE-MIDI Device
 *
 * The board as a BLE-MIDI peripheral: it advertises the MIDI service and a
 * central (Mac / iPad / PC / phone) connects to it. One central at a time.
 *
 * A picoruby-midi transport like USB-MIDI device: 4-byte USB-MIDI packets
 * both ways, a dynamic bit in the transport registry, read by the MIDI input
 * task while a route / filter / watch starts from it. The BLE-MIDI framing
 * is picoruby-midi's codec (ble_midi_codec.h).
 *
 * Process-wide C state: the link survives script changes; the gem's final
 * does nothing.
 *
 * Thread model:
 *   NimBLE host task: GAP events, GATT writes -> decode -> RX ring (timed,
 *                  midi_timed_ring.h: released at the times the central's
 *                  timestamps give)
 *   any task       : BLE_MIDI_DEVICE_send_packet -> TX ring (never blocks)
 *   ble_midi_dev task (same core as the NimBLE host): batches the TX ring
 *                  into notifications, runs enable / disable
 *
 * Build-time knobs: ble_midi_device_config.h.
 */

#ifndef BLE_MIDI_DEVICE_DEFINED_H_
#define BLE_MIDI_DEVICE_DEFINED_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "midi_transport.h"   /* picoruby-midi: midi_tmask_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Register the transport (gets a bit); BLE is not started. Idempotent.
 * Returns 0, or -1 without BLE in this build / out of memory. */
int BLE_MIDI_DEVICE_init(void);

/* Whether this build has BLE-MIDI device support */
bool BLE_MIDI_DEVICE_available(void);

/* Advertise (on) or drop the link and stop advertising (off). Asynchronous:
 * the BLE host starts on the gem's task (it takes up to 2 s). Returns 0, or
 * -1 when unavailable. */
int  BLE_MIDI_DEVICE_enable(bool on);
bool BLE_MIDI_DEVICE_is_enabled(void);

/* The name in the scan response and the GAP device name (at most 29
 * bytes). Applied on the next advertising start. */
void BLE_MIDI_DEVICE_set_name(const char *name);
const char *BLE_MIDI_DEVICE_name(void);

/* Link up and the central subscribed to notifications */
bool BLE_MIDI_DEVICE_is_connected(void);

/* Bit in picoruby-midi's transport registry (0 before init) */
midi_tmask_t BLE_MIDI_DEVICE_transport_bit(void);

/* USB-MIDI packet to the central. Queued; dropped (-1) while not connected
 * or when the queue is full. */
int BLE_MIDI_DEVICE_send_packet(uint8_t cable, uint8_t cin,
                                uint8_t b1, uint8_t b2, uint8_t b3);

/* 4-byte USB-MIDI packets from the central */
int BLE_MIDI_DEVICE_bytes_available(void);
int BLE_MIDI_DEVICE_read_packet(uint8_t *buf, size_t maxlen);

/* Receive timing correction: -1 automatic (connection interval + margin),
 * 0 off, otherwise microseconds (at most 100 ms). Takes effect at once. */
void    BLE_MIDI_DEVICE_set_rx_latency(int32_t us);
int32_t BLE_MIDI_DEVICE_rx_latency_setting(void);

/* Drop the current link (advertising resumes while enabled) */
void BLE_MIDI_DEVICE_disconnect(void);

typedef enum {
    BLE_MIDI_DEVICE_OFF = 0,       /* not enabled */
    BLE_MIDI_DEVICE_STARTING,      /* BLE host starting, or advertising paused for the BLE class */
    BLE_MIDI_DEVICE_ADVERTISING,
    BLE_MIDI_DEVICE_LINKED,        /* connected, not subscribed yet */
    BLE_MIDI_DEVICE_CONNECTED,     /* subscribed: MIDI flows */
    BLE_MIDI_DEVICE_ERROR,         /* the host did not start */
} ble_midi_device_state_t;

typedef struct {
    ble_midi_device_state_t state;
    uint8_t  peer[6];              /* most significant byte first */
    uint8_t  peer_type;
    bool     encrypted;
    bool     bonded;
    uint16_t mtu;
    uint16_t interval_1250us;      /* connection interval, 1.25 ms units */
    uint32_t rx_packets;           /* BLE packets (characteristic writes) */
    uint32_t rx_messages;          /* USB-MIDI packets decoded */
    uint32_t rx_dropped;           /* RX ring full */
    uint32_t decode_errors;
    uint32_t rx_late;              /* later than the latency allows: passed on at once */
    int32_t  rx_latency_us;        /* correction in use (0: off) */
    uint32_t tx_messages;          /* USB-MIDI packets queued */
    uint32_t tx_packets;           /* notifications sent */
    uint32_t tx_dropped;           /* not connected / queue full */
    uint32_t tx_retries;           /* notification refused (no buffers), sent later */
    uint32_t connections;
} ble_midi_device_stats_t;

void BLE_MIDI_DEVICE_stats(ble_midi_device_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BLE_MIDI_DEVICE_DEFINED_H_ */
