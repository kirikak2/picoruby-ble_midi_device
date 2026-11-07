# picoruby-ble_midi_device

BLE-MIDI device (peripheral) transport for PicoRuby: the board advertises the
BLE-MIDI service and a Mac / iPad / PC connects to it. A
[picoruby-midi](../picoruby-midi) transport like picoruby-usb_midi_device.

```ruby
require 'ble_midi_device'

BLE_MIDI_DEVICE.enable = true             # advertise (asynchronous)
dev = BLE_MIDI_DEVICE.instance
dev.connected?                            # linked and subscribed
MIDI::Device.new(dev).note_on(60, 100)
BLE_MIDI_DEVICE.info                      # state, peer, MTU, interval, counters
```

- ESP32 only, on NimBLE through picoruby-ble's ESP32 port (its shared NimBLE
  owner, `nimble_owner.h`). The host project compiles `ports/esp32` and
  defines `BLE_MIDI_DEVICE_ENABLED=1`; without it the port is stubs.
- BLE-MIDI framing: picoruby-midi's `ble_midi_codec.h`.
- Build-time knobs (name, connection interval, queues, task):
  `include/ble_midi_device_config.h`.
- The link is process-wide C state: it survives script changes.

Design and details: Midori's docs/BLE_MIDI.md.
