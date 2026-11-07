/*
 * PicoRuby BLE-MIDI Device - mruby bindings
 */

#include <string.h>

#include <mruby.h>
#include <mruby/presym.h>
#include <mruby/class.h>
#include <mruby/string.h>
#include <mruby/hash.h>

#include "../../include/ble_midi_device.h"

static const char *const STATE_NAMES[] = {
    "off", "starting", "advertising", "linked", "connected", "error",
};

static mrb_value
mrb_bmd_init(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value(BLE_MIDI_DEVICE_init());
}

static mrb_value
mrb_bmd_transport_bit(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value((mrb_int)BLE_MIDI_DEVICE_transport_bit());
}

static mrb_value
mrb_bmd_connected(mrb_state *mrb, mrb_value self)
{
    return mrb_bool_value(BLE_MIDI_DEVICE_is_connected());
}

static mrb_value
mrb_bmd_send_packet(mrb_state *mrb, mrb_value self)
{
    mrb_int cable, cin, b1, b2, b3;
    mrb_get_args(mrb, "iiiii", &cable, &cin, &b1, &b2, &b3);
    return mrb_fixnum_value(BLE_MIDI_DEVICE_send_packet((uint8_t)cable, (uint8_t)cin,
                                                        (uint8_t)b1, (uint8_t)b2, (uint8_t)b3));
}

static mrb_value
mrb_bmd_bytes_available(mrb_state *mrb, mrb_value self)
{
    return mrb_fixnum_value(BLE_MIDI_DEVICE_bytes_available());
}

static mrb_value
mrb_bmd_read_available(mrb_state *mrb, mrb_value self)
{
    uint8_t buf[64];
    int n = BLE_MIDI_DEVICE_read_packet(buf, sizeof(buf));
    if (n <= 0) return mrb_nil_value();
    return mrb_str_new(mrb, (const char *)buf, n);
}

static mrb_value
mrb_bmd_available(mrb_state *mrb, mrb_value self)
{
    return mrb_bool_value(BLE_MIDI_DEVICE_available());
}

static mrb_value
mrb_bmd_enable(mrb_state *mrb, mrb_value self)
{
    mrb_bool on;
    mrb_get_args(mrb, "b", &on);
    return mrb_bool_value(BLE_MIDI_DEVICE_enable(on) == 0);
}

static mrb_value
mrb_bmd_enabled(mrb_state *mrb, mrb_value self)
{
    return mrb_bool_value(BLE_MIDI_DEVICE_is_enabled());
}

static mrb_value
mrb_bmd_name(mrb_state *mrb, mrb_value self)
{
    return mrb_str_new_cstr(mrb, BLE_MIDI_DEVICE_name());
}

static mrb_value
mrb_bmd_set_name(mrb_state *mrb, mrb_value self)
{
    const char *name;
    mrb_get_args(mrb, "z", &name);
    BLE_MIDI_DEVICE_set_name(name);
    return mrb_str_new_cstr(mrb, BLE_MIDI_DEVICE_name());
}

static mrb_value
mrb_bmd_disconnect(mrb_state *mrb, mrb_value self)
{
    BLE_MIDI_DEVICE_disconnect();
    return mrb_nil_value();
}

static void
hset(mrb_state *mrb, mrb_value h, const char *key, mrb_value v)
{
    mrb_hash_set(mrb, h, mrb_symbol_value(mrb_intern_cstr(mrb, key)), v);
}

/* BLE_MIDI_DEVICE._info => Hash */
static mrb_value
mrb_bmd_info(mrb_state *mrb, mrb_value self)
{
    ble_midi_device_stats_t st;
    BLE_MIDI_DEVICE_stats(&st);
    mrb_value h = mrb_hash_new(mrb);
    int s = (int)st.state;
    hset(mrb, h, "state", mrb_symbol_value(mrb_intern_cstr(mrb,
         (s >= 0 && s < (int)(sizeof(STATE_NAMES) / sizeof(STATE_NAMES[0]))) ? STATE_NAMES[s] : "error")));
    if (st.state == BLE_MIDI_DEVICE_LINKED || st.state == BLE_MIDI_DEVICE_CONNECTED) {
        char addr[18];
        snprintf(addr, sizeof(addr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 st.peer[0], st.peer[1], st.peer[2], st.peer[3], st.peer[4], st.peer[5]);
        hset(mrb, h, "peer", mrb_str_new_cstr(mrb, addr));
    } else {
        hset(mrb, h, "peer", mrb_nil_value());
    }
    hset(mrb, h, "encrypted", mrb_bool_value(st.encrypted));
    hset(mrb, h, "bonded", mrb_bool_value(st.bonded));
    hset(mrb, h, "mtu", mrb_fixnum_value(st.mtu));
    hset(mrb, h, "interval_ms", mrb_float_value(mrb, st.interval_1250us * 1.25));
    hset(mrb, h, "rx_packets", mrb_fixnum_value(st.rx_packets));
    hset(mrb, h, "rx_messages", mrb_fixnum_value(st.rx_messages));
    hset(mrb, h, "rx_dropped", mrb_fixnum_value(st.rx_dropped));
    hset(mrb, h, "decode_errors", mrb_fixnum_value(st.decode_errors));
    hset(mrb, h, "tx_messages", mrb_fixnum_value(st.tx_messages));
    hset(mrb, h, "tx_packets", mrb_fixnum_value(st.tx_packets));
    hset(mrb, h, "tx_dropped", mrb_fixnum_value(st.tx_dropped));
    hset(mrb, h, "tx_retries", mrb_fixnum_value(st.tx_retries));
    hset(mrb, h, "connections", mrb_fixnum_value(st.connections));
    return h;
}

void
mrb_picoruby_ble_midi_device_gem_init(mrb_state *mrb)
{
    struct RClass *cls = mrb_define_class_id(mrb, MRB_SYM(BLE_MIDI_DEVICE), mrb->object_class);

    mrb_define_method_id(mrb, cls, MRB_SYM(_init),            mrb_bmd_init,            MRB_ARGS_NONE());
    mrb_define_method_id(mrb, cls, MRB_SYM(_transport_bit),   mrb_bmd_transport_bit,   MRB_ARGS_NONE());
    mrb_define_method_id(mrb, cls, MRB_SYM(_connected),       mrb_bmd_connected,       MRB_ARGS_NONE());
    mrb_define_method_id(mrb, cls, MRB_SYM(_send_packet),     mrb_bmd_send_packet,     MRB_ARGS_REQ(5));
    mrb_define_method_id(mrb, cls, MRB_SYM(_bytes_available), mrb_bmd_bytes_available, MRB_ARGS_NONE());
    mrb_define_method_id(mrb, cls, MRB_SYM(_read_available),  mrb_bmd_read_available,  MRB_ARGS_NONE());

    mrb_define_class_method_id(mrb, cls, MRB_SYM(_available),  mrb_bmd_available,  MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, cls, MRB_SYM(_enable),     mrb_bmd_enable,     MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, cls, MRB_SYM(_enabled),    mrb_bmd_enabled,    MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, cls, MRB_SYM(_name),       mrb_bmd_name,       MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, cls, MRB_SYM(_set_name),   mrb_bmd_set_name,   MRB_ARGS_REQ(1));
    mrb_define_class_method_id(mrb, cls, MRB_SYM(_disconnect), mrb_bmd_disconnect, MRB_ARGS_NONE());
    mrb_define_class_method_id(mrb, cls, MRB_SYM(_info),       mrb_bmd_info,       MRB_ARGS_NONE());
}

void
mrb_picoruby_ble_midi_device_gem_final(mrb_state *mrb)
{
    /* The link stays up across scripts (process-wide C state) */
}
