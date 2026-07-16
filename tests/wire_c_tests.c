#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "vhid/wire.h"

static uint32_t read_u32_le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

int main(void) {
    const uint8_t descriptor[] = {
        0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
        0x05, 0x09, 0x19, 0x01, 0x29, 0x01,
        0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
        0x95, 0x01, 0x81, 0x02, 0x75, 0x07,
        0x95, 0x01, 0x81, 0x03, 0xC0,
    };
    uint8_t packet[VHID_MAX_DATAGRAM_SIZE];
    vhid_sender_t sender;
    vhid_hid_device_info_t device = {0};
    const uint8_t report[] = {1};

    vhid_sender_init(&sender, 77);
    device.vendor_id = 0x1209;
    device.product_id = 0x5342;
    device.version_number = 1;
    device.transport = VHID_TRANSPORT_NETWORK;
    device.flags = VHID_DEVICE_ALLOW_TRANSPARENT_OUTPUT;
    device.report_descriptor = descriptor;
    device.report_descriptor_size = (uint16_t)sizeof(descriptor);
    device.source_output_profile = VHID_PROFILE_SWITCH_PRO;
    device.product = "C Sender";
    device.manufacturer = "VHID";
    device.serial = "c-1";

    size_t size =
        vhid_make_hid_device_add(&sender, &device, 10, packet, sizeof(packet));
    assert(size > sizeof(vhid_message_header_t));
    assert(read_u32_le(packet) == VHID_WIRE_MAGIC);
    assert(packet[4] == VHID_WIRE_VERSION);
    assert(packet[5] == VHID_MSG_HID_DEVICE_ADD);
    assert(read_u32_le(packet + 12) == 77);
    assert(read_u32_le(packet + 16) == 0);
    assert(packet[sizeof(vhid_message_header_t) + 13] ==
           VHID_PROFILE_SWITCH_PRO);

    size = vhid_make_hid_input_report(
        &sender, 0, report, (uint16_t)sizeof(report), 20, packet,
        sizeof(packet));
    assert(size == sizeof(vhid_message_header_t) +
                       sizeof(vhid_hid_report_header_t) + sizeof(report));
    assert(packet[5] == VHID_MSG_HID_INPUT_REPORT);
    assert(read_u32_le(packet + 16) == 1);
    assert(packet[sizeof(vhid_message_header_t)] == VHID_REPORT_INPUT);
    assert(packet[sizeof(vhid_message_header_t) + 1] == 0);
    assert(packet[size - 1] == report[0]);

    size = vhid_make_hid_device_remove(&sender, 30, packet, sizeof(packet));
    assert(size == sizeof(vhid_message_header_t));
    assert(packet[5] == VHID_MSG_HID_DEVICE_REMOVE);
    assert(read_u32_le(packet + 16) == 2);

    vhid_sender_t session_sender;
    vhid_sender_init(&session_sender, 99);
    vhid_session_payload_t session = {0};
    session.session_id = 0x12345678;
    session.peer_id = 42;
    session.keepalive_interval_us = 5000000;
    session.timeout_us = 15000000;
    size = vhid_make_session_open(
        &session_sender, &session, 40, packet, sizeof(packet));
    assert(size == sizeof(vhid_message_header_t) +
                       sizeof(vhid_session_payload_t));
    assert(packet[5] == VHID_MSG_SESSION_OPEN);
    assert(read_u32_le(packet + 12) == 0);
    assert(read_u32_le(packet + 16) == 0);
    assert(read_u32_le(packet + sizeof(vhid_message_header_t)) ==
           session.session_id);

    size = vhid_make_session_accept(
        &session_sender, &session, 50, packet, sizeof(packet));
    assert(size == sizeof(vhid_message_header_t) +
                       sizeof(vhid_session_payload_t));
    assert(packet[5] == VHID_MSG_SESSION_ACCEPT);
    assert(read_u32_le(packet + 12) == 0);
    assert(read_u32_le(packet + 16) == 1);
    assert(read_u32_le(packet + sizeof(vhid_message_header_t)) ==
           session.session_id);

    size = vhid_make_session_ping(
        &session_sender, 60, packet, sizeof(packet));
    assert(size == sizeof(vhid_message_header_t));
    assert(packet[5] == VHID_MSG_SESSION_PING);
    assert(read_u32_le(packet + 12) == 0);
    assert(read_u32_le(packet + 16) == 2);

    size = vhid_make_session_pong(
        &session_sender, 70, packet, sizeof(packet));
    assert(size == sizeof(vhid_message_header_t));
    assert(packet[5] == VHID_MSG_SESSION_PONG);
    assert(read_u32_le(packet + 12) == 0);
    assert(read_u32_le(packet + 16) == 3);

    size = vhid_make_session_close(
        &session_sender, 80, packet, sizeof(packet));
    assert(size == sizeof(vhid_message_header_t));
    assert(packet[5] == VHID_MSG_SESSION_CLOSE);
    assert(read_u32_le(packet + 12) == 0);
    assert(read_u32_le(packet + 16) == 4);
    return 0;
}
