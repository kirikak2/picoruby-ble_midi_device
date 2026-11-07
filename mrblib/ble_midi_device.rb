# BLE-MIDI Device (peripheral) transport layer
#
# The board as a BLE-MIDI instrument: it advertises the MIDI service, and a
# Mac / iPad / PC connects to it. Use picoruby-midi for MIDI operations:
#
#   dev = MIDI::Device.new(BLE_MIDI_DEVICE.instance)
#
# The link is C state that lives across scripts. Whether the board
# advertises at all is a setting (BLE_MIDI_DEVICE.enable = true / false).
class BLE_MIDI_DEVICE
  def self.instance
    $__ble_midi_device_instance__ = new if $__ble_midi_device_instance__.nil?
    $__ble_midi_device_instance__
  end

  # Whether this build has BLE-MIDI device support
  def self.available?
    _available
  end

  # Advertise (true) or drop the link and stop advertising (false).
  # Turning it on returns at once (the BLE host takes a moment to come up);
  # turning it off waits until it is off (up to 3 s), so that a script can
  # use the BLE class right after (it needs the BLE host to itself).
  # For this script only: the saved setting comes back when it ends.
  def self.enable=(on)
    _enable(on ? true : false)
    return if on
    started = Machine.board_millis
    while _info[:state] != :off && Machine.board_millis - started < 3000
      sleep_ms 20
    end
  end

  def self.enabled?
    _enabled
  end

  # Name in the scan response (at most 29 bytes; applied on the next
  # advertising start)
  def self.name
    _name
  end

  def self.name=(name)
    _set_name(name.to_s)
  end

  # :off / :starting / :advertising / :linked / :connected / :error
  def self.state
    _info[:state]
  end

  # Address of the connected central ("AA:BB:CC:DD:EE:FF"), or nil
  def self.peer
    _info[:peer]
  end

  # State, peer, MTU, connection interval and traffic counters
  def self.info
    _info
  end

  def self.disconnect
    _disconnect
  end

  def initialize
    _init
  end

  # Bit in picoruby-midi's transport registry (assigned at start-up)
  def transport_id
    _transport_bit
  end

  # Connected and subscribed to notifications (MIDI flows)
  def connected?
    _connected
  end

  # Send a USB-MIDI packet to the central. 0, or -1 when not connected.
  def send_packet(cable, cin, midi1, midi2, midi3)
    _send_packet(cable, cin, midi1, midi2, midi3)
  end

  def bytes_available
    _bytes_available
  end

  # 4-byte USB-MIDI packets from the central (String), or nil
  def read_available
    _read_available
  end
end
