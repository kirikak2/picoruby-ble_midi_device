/*
 * PicoRuby BLE-MIDI Device - VM selection wrapper
 */

#include "../include/ble_midi_device.h"

#if defined(PICORB_VM_MRUBY)
  #include "mruby/ble_midi_device.c"
#elif defined(PICORB_VM_MRUBYC)
  #include "mrubyc/ble_midi_device.c"
#endif
