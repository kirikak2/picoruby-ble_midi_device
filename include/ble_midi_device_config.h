/*
 * PicoRuby BLE-MIDI Device - build-time configuration
 *
 * The gem knows no board or product: the host project injects these as
 * compile definitions (Midori: components/midori-runtime/CMakeLists.txt).
 */

#ifndef BLE_MIDI_DEVICE_CONFIG_DEFINED_H_
#define BLE_MIDI_DEVICE_CONFIG_DEFINED_H_

/* Built with BLE-MIDI device support. Needs NimBLE (CONFIG_BT_NIMBLE_ENABLED)
 * and picoruby-ble's ESP32 port (its shared NimBLE owner). Without it the
 * port compiles to stubs. */
#ifndef BLE_MIDI_DEVICE_ENABLED
#define BLE_MIDI_DEVICE_ENABLED 0
#endif

/* Name in the scan response / GAP device name (at most 29 bytes) */
#ifndef BLE_MIDI_DEVICE_NAME
#define BLE_MIDI_DEVICE_NAME "PicoRuby MIDI"
#endif

/* Characteristic needs an encrypted link (pairing) for reads, writes and
 * notifications. Off: pairing is still accepted (Just Works, bonded) when
 * the central asks for it. */
#ifndef BLE_MIDI_DEVICE_REQUIRE_ENCRYPTION
#define BLE_MIDI_DEVICE_REQUIRE_ENCRYPTION 0
#endif

/* Connection interval to ask the central for after connecting, in 1.25 ms
 * units (BLE-MIDI asks for 15 ms or less). 0 leaves what the central chose. */
#ifndef BLE_MIDI_DEVICE_CONN_ITVL_MIN
#define BLE_MIDI_DEVICE_CONN_ITVL_MIN 6     /* 7.5 ms */
#endif
#ifndef BLE_MIDI_DEVICE_CONN_ITVL_MAX
#define BLE_MIDI_DEVICE_CONN_ITVL_MAX 12    /* 15 ms */
#endif

/* Advertising interval, 0.625 ms units */
#ifndef BLE_MIDI_DEVICE_ADV_ITVL
#define BLE_MIDI_DEVICE_ADV_ITVL 160        /* 100 ms */
#endif

/* Queues (entries). RX holds 4-byte packets, TX packet + timestamp. */
#ifndef BLE_MIDI_DEVICE_RX_PACKETS
#define BLE_MIDI_DEVICE_RX_PACKETS 256      /* power of 2 */
#endif
#ifndef BLE_MIDI_DEVICE_TX_PACKETS
#define BLE_MIDI_DEVICE_TX_PACKETS 256      /* power of 2 */
#endif

/* Wait after the first queued packet before sending, so that a chord goes
 * in one notification (microseconds) */
#ifndef BLE_MIDI_DEVICE_TX_BATCH_US
#define BLE_MIDI_DEVICE_TX_BATCH_US 1000
#endif

/* The gem's task: same core as the NimBLE host task, below it */
#ifndef BLE_MIDI_DEVICE_TASK_CORE
#ifdef CONFIG_BT_NIMBLE_PINNED_TO_CORE
#define BLE_MIDI_DEVICE_TASK_CORE CONFIG_BT_NIMBLE_PINNED_TO_CORE
#else
#define BLE_MIDI_DEVICE_TASK_CORE 0
#endif
#endif
#ifndef BLE_MIDI_DEVICE_TASK_PRIORITY
#define BLE_MIDI_DEVICE_TASK_PRIORITY 5
#endif
#ifndef BLE_MIDI_DEVICE_TASK_STACK
#define BLE_MIDI_DEVICE_TASK_STACK 4096
#endif

#endif /* BLE_MIDI_DEVICE_CONFIG_DEFINED_H_ */
