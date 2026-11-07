/*
 * PicoRuby BLE-MIDI Device - mruby/c bindings
 *
 * mruby/c does not tell class methods from instance methods, so the class
 * methods of the mruby binding are plain methods here.
 */

#include <stdio.h>
#include <string.h>

#include <mrubyc.h>

#include "../../include/ble_midi_device.h"

static void c_bmd_init(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(BLE_MIDI_DEVICE_init());
}

static void c_bmd_transport_bit(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN((mrbc_int_t)BLE_MIDI_DEVICE_transport_bit());
}

static void c_bmd_connected(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (BLE_MIDI_DEVICE_is_connected()) { SET_TRUE_RETURN(); } else { SET_FALSE_RETURN(); }
}

static void c_bmd_send_packet(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc != 5) { SET_INT_RETURN(-1); return; }
    SET_INT_RETURN(BLE_MIDI_DEVICE_send_packet((uint8_t)GET_INT_ARG(1), (uint8_t)GET_INT_ARG(2),
                                               (uint8_t)GET_INT_ARG(3), (uint8_t)GET_INT_ARG(4),
                                               (uint8_t)GET_INT_ARG(5)));
}

static void c_bmd_bytes_available(mrbc_vm *vm, mrbc_value v[], int argc)
{
    SET_INT_RETURN(BLE_MIDI_DEVICE_bytes_available());
}

static void c_bmd_read_available(mrbc_vm *vm, mrbc_value v[], int argc)
{
    uint8_t buf[64];
    int n = BLE_MIDI_DEVICE_read_packet(buf, sizeof(buf));
    if (n <= 0) { SET_NIL_RETURN(); return; }
    mrbc_value s = mrbc_string_new(vm, buf, n);
    SET_RETURN(s);
}

static void c_bmd_available(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (BLE_MIDI_DEVICE_available()) { SET_TRUE_RETURN(); } else { SET_FALSE_RETURN(); }
}

static void c_bmd_enable(mrbc_vm *vm, mrbc_value v[], int argc)
{
    bool on = argc >= 1 && v[1].tt == MRBC_TT_TRUE;
    if (BLE_MIDI_DEVICE_enable(on) == 0) { SET_TRUE_RETURN(); } else { SET_FALSE_RETURN(); }
}

static void c_bmd_enabled(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (BLE_MIDI_DEVICE_is_enabled()) { SET_TRUE_RETURN(); } else { SET_FALSE_RETURN(); }
}

static void c_bmd_name(mrbc_vm *vm, mrbc_value v[], int argc)
{
    mrbc_value s = mrbc_string_new_cstr(vm, BLE_MIDI_DEVICE_name());
    SET_RETURN(s);
}

static void c_bmd_set_name(mrbc_vm *vm, mrbc_value v[], int argc)
{
    if (argc >= 1 && v[1].tt == MRBC_TT_STRING) {
        BLE_MIDI_DEVICE_set_name((const char *)mrbc_string_cstr(&v[1]));
    }
    mrbc_value s = mrbc_string_new_cstr(vm, BLE_MIDI_DEVICE_name());
    SET_RETURN(s);
}

static void c_bmd_disconnect(mrbc_vm *vm, mrbc_value v[], int argc)
{
    BLE_MIDI_DEVICE_disconnect();
    SET_NIL_RETURN();
}

static void hset(mrbc_value *h, const char *key, mrbc_value *val)
{
    mrbc_value k = mrbc_symbol_value(mrbc_str_to_symid(key));
    mrbc_hash_set(h, &k, val);
}

static void hset_int(mrbc_vm *vm, mrbc_value *h, const char *key, mrbc_int_t n)
{
    (void)vm;
    mrbc_value val = mrbc_integer_value(n);
    hset(h, key, &val);
}

static void c_bmd_info(mrbc_vm *vm, mrbc_value v[], int argc)
{
    static const char *const names[] = { "off", "starting", "advertising", "linked", "connected", "error" };
    ble_midi_device_stats_t st;
    BLE_MIDI_DEVICE_stats(&st);
    mrbc_value h = mrbc_hash_new(vm, 12);
    int s = (int)st.state;
    mrbc_value state = mrbc_symbol_value(mrbc_str_to_symid((s >= 0 && s < 6) ? names[s] : "error"));
    hset(&h, "state", &state);
    mrbc_value peer = mrbc_nil_value();
    if (st.state == BLE_MIDI_DEVICE_LINKED || st.state == BLE_MIDI_DEVICE_CONNECTED) {
        char addr[18];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 st.peer[0], st.peer[1], st.peer[2], st.peer[3], st.peer[4], st.peer[5]);
        peer = mrbc_string_new_cstr(vm, addr);
    }
    hset(&h, "peer", &peer);
    hset_int(vm, &h, "mtu", st.mtu);
    hset_int(vm, &h, "rx_messages", st.rx_messages);
    hset_int(vm, &h, "rx_dropped", st.rx_dropped);
    hset_int(vm, &h, "decode_errors", st.decode_errors);
    hset_int(vm, &h, "tx_messages", st.tx_messages);
    hset_int(vm, &h, "tx_packets", st.tx_packets);
    hset_int(vm, &h, "tx_dropped", st.tx_dropped);
    hset_int(vm, &h, "connections", st.connections);
    SET_RETURN(h);
}

void
mrbc_ble_midi_device_init(mrbc_vm *vm)
{
    mrbc_class *cls = mrbc_define_class(vm, "BLE_MIDI_DEVICE", mrbc_class_object);

    mrbc_define_method(vm, cls, "_init",            c_bmd_init);
    mrbc_define_method(vm, cls, "_transport_bit",   c_bmd_transport_bit);
    mrbc_define_method(vm, cls, "_connected",       c_bmd_connected);
    mrbc_define_method(vm, cls, "_send_packet",     c_bmd_send_packet);
    mrbc_define_method(vm, cls, "_bytes_available", c_bmd_bytes_available);
    mrbc_define_method(vm, cls, "_read_available",  c_bmd_read_available);
    mrbc_define_method(vm, cls, "_available",       c_bmd_available);
    mrbc_define_method(vm, cls, "_enable",          c_bmd_enable);
    mrbc_define_method(vm, cls, "_enabled",         c_bmd_enabled);
    mrbc_define_method(vm, cls, "_name",            c_bmd_name);
    mrbc_define_method(vm, cls, "_set_name",        c_bmd_set_name);
    mrbc_define_method(vm, cls, "_disconnect",      c_bmd_disconnect);
    mrbc_define_method(vm, cls, "_info",            c_bmd_info);
}
