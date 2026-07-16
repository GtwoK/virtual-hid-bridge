/**
 * vhid/wire.h
 *
 * C-compatible sender helpers for the Virtual HID Bridge UDP transport.
 *
 * Sender code should mostly think in terms of a session plus HID lifecycle
 * calls: open a session, add a controller with a report descriptor, send HID
 * input reports, and remove the controller when it disappears. The small
 * versioned envelope is just how those HID artifacts travel over UDP.
 */

#ifndef VHID_WIRE_H
#define VHID_WIRE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#define VHID_STATIC_ASSERT static_assert
extern "C" {
#else
#define VHID_STATIC_ASSERT _Static_assert
#endif

#define VHID_WIRE_MAGIC        0x32424856u /* "VHB2" */
#define VHID_WIRE_VERSION      2
#define VHID_DEFAULT_UDP_PORT  48660
#define VHID_MAX_DATAGRAM_SIZE 1400

typedef enum {
    VHID_MSG_SESSION_OPEN            = 1,
    VHID_MSG_SESSION_ACCEPT          = 2,
    VHID_MSG_SESSION_CLOSE           = 3,
    VHID_MSG_SESSION_PING            = 4,
    VHID_MSG_SESSION_PONG            = 5,

    VHID_MSG_HID_DEVICE_ADD          = 16,
    VHID_MSG_HID_DEVICE_REMOVE       = 17,

    VHID_MSG_HID_INPUT_REPORT        = 32,
    VHID_MSG_HID_OUTPUT_REPORT       = 33,
    VHID_MSG_HID_GET_REPORT          = 34,
    VHID_MSG_HID_GET_REPORT_RESPONSE = 35,

    VHID_MSG_SEMANTIC_DEVICE_ADD     = 48,
    VHID_MSG_SEMANTIC_INPUT_STATE    = 49,
} vhid_message_type_t;

typedef enum {
    VHID_REPORT_INPUT   = 0,
    VHID_REPORT_OUTPUT  = 1,
    VHID_REPORT_FEATURE = 2,
} vhid_report_type_t;

typedef enum {
    VHID_TRANSPORT_VIRTUAL_DEVICE = 0,
    VHID_TRANSPORT_USB            = 1,
    VHID_TRANSPORT_BLUETOOTH      = 2,
    VHID_TRANSPORT_BLUETOOTH_LE   = 3,
    VHID_TRANSPORT_NETWORK        = 4,
} vhid_transport_t;

typedef enum {
    VHID_PROFILE_GENERIC          = 0,
    VHID_PROFILE_STANDARD_GAMEPAD = 1,
    VHID_PROFILE_SWITCH_PRO       = 2,
    VHID_PROFILE_SWITCH_2_PRO     = 3,
    VHID_PROFILE_DUALSHOCK_4      = 4,
    VHID_PROFILE_DUALSENSE        = 5,
    VHID_PROFILE_XBOX             = 6,
} vhid_device_profile_t;

enum {
    VHID_DEVICE_ALLOW_TRANSPARENT_OUTPUT = 1u << 0,
    VHID_SOURCE_OUTPUT_PROFILE_INFER = 0,
    VHID_SOURCE_OUTPUT_PROFILE_NONE = 0xff,
};

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint32_t payload_size;
    uint32_t device_id;
    uint32_t sequence;
    uint64_t timestamp_us;
} vhid_message_header_t;

typedef struct __attribute__((packed)) {
    uint32_t session_id;
    uint32_t peer_id;
    uint32_t capabilities;
    uint32_t keepalive_interval_us;
    uint32_t timeout_us;
    char     name[32];
} vhid_session_payload_t;

typedef struct __attribute__((packed)) {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version_number;
    uint8_t  transport;
    uint8_t  flags;
    uint16_t descriptor_size;
    uint8_t  product_size;
    uint8_t  manufacturer_size;
    uint8_t  serial_size;
    uint8_t  source_output_profile;
} vhid_hid_device_add_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  report_type;
    uint8_t  report_id;
    uint16_t data_size;
} vhid_hid_report_header_t;

typedef struct {
    uint32_t device_id;
    uint32_t next_sequence;
} vhid_sender_t;

typedef struct {
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t version_number;
    vhid_transport_t transport;
    uint8_t flags;
    const uint8_t* report_descriptor;
    uint16_t report_descriptor_size;
    uint8_t source_output_profile;
    const char* product;
    const char* manufacturer;
    const char* serial;
} vhid_hid_device_info_t;

VHID_STATIC_ASSERT(sizeof(vhid_message_header_t) == 28,
                   "VHB2 message header must stay wire-compatible");
VHID_STATIC_ASSERT(sizeof(vhid_session_payload_t) == 52,
                   "VHB2 session payload must stay wire-compatible");
VHID_STATIC_ASSERT(sizeof(vhid_hid_device_add_header_t) == 14,
                   "VHB2 HID add header must stay wire-compatible");
VHID_STATIC_ASSERT(sizeof(vhid_hid_report_header_t) == 4,
                   "VHB2 HID report header must stay wire-compatible");

static inline void vhid_sender_init(vhid_sender_t* sender,
                                    uint32_t device_id) {
    if (!sender) return;
    sender->device_id = device_id;
    sender->next_sequence = 0;
}

static inline size_t vhid_wire_bounded_strlen(const char* text, size_t maximum) {
    size_t length = 0;
    if (!text) return 0;
    while (length < maximum && text[length]) ++length;
    return length;
}

static inline void vhid_wire_write_u16(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
}

static inline void vhid_wire_write_u32(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)(value & 0xffu);
    out[1] = (uint8_t)((value >> 8) & 0xffu);
    out[2] = (uint8_t)((value >> 16) & 0xffu);
    out[3] = (uint8_t)((value >> 24) & 0xffu);
}

static inline void vhid_wire_write_u64(uint8_t* out, uint64_t value) {
    for (uint8_t i = 0; i < 8; ++i)
        out[i] = (uint8_t)((value >> (8u * i)) & 0xffu);
}

static inline void vhid_wire_copy(uint8_t* out, const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < size; ++i) out[i] = bytes[i];
}

static inline size_t vhid_wire_begin_message_for_device(
    vhid_sender_t* sender,
    uint32_t device_id,
    vhid_message_type_t type,
    uint32_t payload_size,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    if (!sender || !out || out_size < sizeof(vhid_message_header_t) ||
        sizeof(vhid_message_header_t) + payload_size > out_size ||
        sizeof(vhid_message_header_t) + payload_size >
            VHID_MAX_DATAGRAM_SIZE) {
        return 0;
    }
    vhid_wire_write_u32(out + 0, VHID_WIRE_MAGIC);
    out[4] = VHID_WIRE_VERSION;
    out[5] = (uint8_t)type;
    vhid_wire_write_u16(out + 6, 0);
    vhid_wire_write_u32(out + 8, payload_size);
    vhid_wire_write_u32(out + 12, device_id);
    vhid_wire_write_u32(out + 16, sender->next_sequence++);
    vhid_wire_write_u64(out + 20, timestamp_us);
    return sizeof(vhid_message_header_t);
}

static inline size_t vhid_wire_begin_message(vhid_sender_t* sender,
                                             vhid_message_type_t type,
                                             uint32_t payload_size,
                                             uint64_t timestamp_us,
                                             uint8_t* out,
                                             size_t out_size) {
    return vhid_wire_begin_message_for_device(
        sender, sender ? sender->device_id : 0, type, payload_size,
        timestamp_us, out, out_size);
}

static inline size_t vhid_make_session_message(
    vhid_sender_t* sender,
    vhid_message_type_t message_type,
    const vhid_session_payload_t* session,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    if (!session ||
        (message_type != VHID_MSG_SESSION_OPEN &&
         message_type != VHID_MSG_SESSION_ACCEPT)) {
        return 0;
    }
    size_t offset = vhid_wire_begin_message_for_device(
        sender, 0, message_type, sizeof(vhid_session_payload_t),
        timestamp_us, out, out_size);
    if (!offset) return 0;
    vhid_wire_write_u32(out + offset + 0, session->session_id);
    vhid_wire_write_u32(out + offset + 4, session->peer_id);
    vhid_wire_write_u32(out + offset + 8, session->capabilities);
    vhid_wire_write_u32(out + offset + 12, session->keepalive_interval_us);
    vhid_wire_write_u32(out + offset + 16, session->timeout_us);
    vhid_wire_copy(out + offset + 20, session->name, sizeof(session->name));
    return sizeof(vhid_message_header_t) + sizeof(vhid_session_payload_t);
}

static inline size_t vhid_make_session_open(
    vhid_sender_t* sender,
    const vhid_session_payload_t* session,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    return vhid_make_session_message(sender, VHID_MSG_SESSION_OPEN, session,
                                     timestamp_us, out, out_size);
}

static inline size_t vhid_make_session_accept(
    vhid_sender_t* sender,
    const vhid_session_payload_t* session,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    return vhid_make_session_message(sender, VHID_MSG_SESSION_ACCEPT, session,
                                     timestamp_us, out, out_size);
}

static inline size_t vhid_make_session_empty(
    vhid_sender_t* sender,
    vhid_message_type_t message_type,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    if (message_type != VHID_MSG_SESSION_CLOSE &&
        message_type != VHID_MSG_SESSION_PING &&
        message_type != VHID_MSG_SESSION_PONG) {
        return 0;
    }
    const size_t offset = vhid_wire_begin_message_for_device(
        sender, 0, message_type, 0, timestamp_us, out, out_size);
    return offset ? sizeof(vhid_message_header_t) : 0;
}

static inline size_t vhid_make_session_close(
    vhid_sender_t* sender,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    return vhid_make_session_empty(sender, VHID_MSG_SESSION_CLOSE,
                                   timestamp_us, out, out_size);
}

static inline size_t vhid_make_session_ping(
    vhid_sender_t* sender,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    return vhid_make_session_empty(sender, VHID_MSG_SESSION_PING,
                                   timestamp_us, out, out_size);
}

static inline size_t vhid_make_session_pong(
    vhid_sender_t* sender,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    return vhid_make_session_empty(sender, VHID_MSG_SESSION_PONG,
                                   timestamp_us, out, out_size);
}

static inline size_t vhid_make_hid_device_add(
    vhid_sender_t* sender,
    const vhid_hid_device_info_t* device,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    if (!device || !device->report_descriptor ||
        device->report_descriptor_size == 0) {
        return 0;
    }
    const size_t product_size = vhid_wire_bounded_strlen(device->product, 256);
    const size_t manufacturer_size =
        vhid_wire_bounded_strlen(device->manufacturer, 256);
    const size_t serial_size = vhid_wire_bounded_strlen(device->serial, 256);
    if (product_size > 255 || manufacturer_size > 255 ||
        serial_size > 255) {
        return 0;
    }
    const uint32_t payload_size =
        (uint32_t)(sizeof(vhid_hid_device_add_header_t) +
                   device->report_descriptor_size + product_size +
                   manufacturer_size + serial_size);
    size_t offset = vhid_wire_begin_message(
        sender, VHID_MSG_HID_DEVICE_ADD, payload_size, timestamp_us, out,
        out_size);
    if (!offset) return 0;
    vhid_wire_write_u16(out + offset + 0, device->vendor_id);
    vhid_wire_write_u16(out + offset + 2, device->product_id);
    vhid_wire_write_u16(out + offset + 4, device->version_number);
    out[offset + 6] = (uint8_t)device->transport;
    out[offset + 7] = device->flags;
    vhid_wire_write_u16(out + offset + 8, device->report_descriptor_size);
    out[offset + 10] = (uint8_t)product_size;
    out[offset + 11] = (uint8_t)manufacturer_size;
    out[offset + 12] = (uint8_t)serial_size;
    out[offset + 13] = device->source_output_profile;
    offset += sizeof(vhid_hid_device_add_header_t);
    vhid_wire_copy(out + offset, device->report_descriptor,
                   device->report_descriptor_size);
    offset += device->report_descriptor_size;
    vhid_wire_copy(out + offset, device->product, product_size);
    offset += product_size;
    vhid_wire_copy(out + offset, device->manufacturer, manufacturer_size);
    offset += manufacturer_size;
    vhid_wire_copy(out + offset, device->serial, serial_size);
    return sizeof(vhid_message_header_t) + payload_size;
}

static inline size_t vhid_make_hid_report(
    vhid_sender_t* sender,
    vhid_message_type_t message_type,
    vhid_report_type_t report_type,
    uint8_t report_id,
    const uint8_t* report,
    uint16_t report_size,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    if ((report_size && !report) ||
        (message_type != VHID_MSG_HID_INPUT_REPORT &&
         message_type != VHID_MSG_HID_OUTPUT_REPORT &&
         message_type != VHID_MSG_HID_GET_REPORT &&
         message_type != VHID_MSG_HID_GET_REPORT_RESPONSE)) {
        return 0;
    }
    const uint32_t payload_size =
        (uint32_t)(sizeof(vhid_hid_report_header_t) + report_size);
    size_t offset = vhid_wire_begin_message(
        sender, message_type, payload_size, timestamp_us, out, out_size);
    if (!offset) return 0;
    out[offset + 0] = (uint8_t)report_type;
    out[offset + 1] = report_id;
    vhid_wire_write_u16(out + offset + 2, report_size);
    offset += sizeof(vhid_hid_report_header_t);
    vhid_wire_copy(out + offset, report, report_size);
    return sizeof(vhid_message_header_t) + payload_size;
}

static inline size_t vhid_make_hid_input_report(
    vhid_sender_t* sender,
    uint8_t report_id,
    const uint8_t* report,
    uint16_t report_size,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    return vhid_make_hid_report(sender, VHID_MSG_HID_INPUT_REPORT,
                                VHID_REPORT_INPUT, report_id, report,
                                report_size, timestamp_us, out, out_size);
}

static inline size_t vhid_make_hid_device_remove(
    vhid_sender_t* sender,
    uint64_t timestamp_us,
    uint8_t* out,
    size_t out_size) {
    const size_t offset = vhid_wire_begin_message(
        sender, VHID_MSG_HID_DEVICE_REMOVE, 0, timestamp_us, out, out_size);
    return offset ? sizeof(vhid_message_header_t) : 0;
}

#ifdef __cplusplus
}
#endif

#undef VHID_STATIC_ASSERT

#endif /* VHID_WIRE_H */
