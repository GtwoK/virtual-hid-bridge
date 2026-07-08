/**
 * virtual_hid_transport.h
 *
 * C-compatible wire constants for Virtual HID Bridge Protocol v2 (VHB2).
 * The richer C++ API lives in include/vhid/protocol.h; this header keeps
 * small senders from copying magic numbers by hand.
 */

#ifndef VIRTUAL_HID_TRANSPORT_H
#define VIRTUAL_HID_TRANSPORT_H

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
    VHID_MSG_HELLO                   = 1,
    VHID_MSG_HID_DEVICE_ADD          = 2,
    VHID_MSG_HID_DEVICE_REMOVE       = 3,
    VHID_MSG_HID_INPUT_REPORT        = 4,
    VHID_MSG_HID_OUTPUT_REPORT       = 5,
    VHID_MSG_HID_GET_REPORT          = 6,
    VHID_MSG_HID_GET_REPORT_RESPONSE = 7,
    VHID_MSG_PING                    = 8,
    VHID_MSG_PONG                    = 9,
    VHID_MSG_SEMANTIC_DEVICE_ADD     = 10,
    VHID_MSG_SEMANTIC_INPUT_STATE    = 11,
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

enum {
    VHID_DEVICE_ALLOW_TRANSPARENT_OUTPUT = 1u << 0,
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
    uint32_t client_id;
    uint32_t capabilities;
    char     name[32];
} vhid_hello_payload_t;

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
    uint8_t  reserved;
} vhid_hid_device_add_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  report_type;
    uint8_t  report_id;
    uint16_t data_size;
} vhid_hid_report_header_t;

VHID_STATIC_ASSERT(sizeof(vhid_message_header_t) == 28,
                   "VHB2 message header must stay wire-compatible");
VHID_STATIC_ASSERT(sizeof(vhid_hid_device_add_header_t) == 14,
                   "VHB2 HID add header must stay wire-compatible");
VHID_STATIC_ASSERT(sizeof(vhid_hid_report_header_t) == 4,
                   "VHB2 HID report header must stay wire-compatible");

#ifdef __cplusplus
}
#endif

#undef VHID_STATIC_ASSERT

#endif /* VIRTUAL_HID_TRANSPORT_H */
