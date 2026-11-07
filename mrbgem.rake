MRuby::Gem::Specification.new('picoruby-ble_midi_device') do |spec|
  spec.license = 'MIT'
  spec.author  = 'Toshio Maki'
  spec.summary = 'BLE-MIDI Device (peripheral) transport layer'
  spec.require_name = 'ble_midi_device'
  # midi_transport.h / ble_midi_codec.h; the ESP32 port is compiled by the
  # host project (it needs NimBLE and picoruby-ble's ESP32 port)
  spec.add_dependency 'picoruby-midi'
  spec.add_dependency 'picoruby-machine'   # sleep_ms / board_millis
end
