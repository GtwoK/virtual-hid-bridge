#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include "vhid/hid_profile.h"
#include "vhid/hid_source_decoder.h"
#include "vhid/mapping.h"
#include "vhid/protocol.h"
#include "vhid/wire.h"

namespace {

vhid::DeviceDescription test_description() {
  vhid::DeviceDescription description{};
  description.requested_profile =
      static_cast<uint8_t>(vhid::DeviceProfile::generic);
  description.device_flags = vhid::kDeviceHasBattery |
                             vhid::kDeviceHasMotion |
                             vhid::kDeviceHasRumble;
  description.button_count = 18;
  description.axis_count = 6;
  description.motion_flags =
      vhid::kMotionAcceleration | vhid::kMotionAngularVelocity;
  description.vendor_id = 0x1209;
  description.product_id = 0x5342;
  std::strcpy(description.product, "Test Controller");
  std::strcpy(description.manufacturer, "VHID");
  std::strcpy(description.serial, "test-1");
  const uint16_t usages[] = {0x30, 0x31, 0x32, 0x35, 0x33, 0x34};
  for (size_t i = 0; i < description.axis_count; ++i) {
    description.axes[i].usage_page = 1;
    description.axes[i].usage = usages[i];
    description.axes[i].logical_min = i >= 4 ? 0 : INT16_MIN;
    description.axes[i].logical_max = INT16_MAX;
    description.axes[i].flags = i >= 4 ? vhid::kAxisUnipolar : 0;
  }
  return description;
}

vhid::DeviceDescription switch_description() {
  auto description = test_description();
  description.requested_profile =
      static_cast<uint8_t>(vhid::DeviceProfile::switch_pro);
  description.hat_count = 1;
  std::strcpy(description.serial, "switch-pro-test");
  return description;
}

void protocol_test() {
  const auto description = test_description();
  assert(vhid::valid_device_description(description));
  const auto bytes =
      vhid::make_message(vhid::MessageType::semantic_device_add, 42, 7, 1000,
                         description);
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(bytes, parsed));
  assert(parsed.header->device_id == 42);
  assert(parsed.header->sequence == 7);
  assert(parsed.payload.size() == sizeof(description));
  assert(!vhid::parse_message(
      std::span<const uint8_t>(bytes.data(), bytes.size() - 1), parsed));

  vhid::SessionPayload session{};
  session.session_id = 0x12345678;
  session.peer_id = 7;
  session.keepalive_interval_us = 5'000'000;
  session.timeout_us = 15'000'000;
  std::strcpy(session.name, "test-session");
  const auto session_open = vhid::make_message(
      vhid::MessageType::session_open, 0, 8, 1100, session);
  assert(vhid::parse_message(session_open, parsed));
  assert(parsed.header->type ==
         static_cast<uint8_t>(vhid::MessageType::session_open));
  assert(parsed.header->device_id == 0);
  assert(parsed.payload.size() == sizeof(session));

  auto profile = vhid::make_profile(description);
  assert(profile);
  vhid::HidDeviceAddHeader device{
      .vendor_id = 0x1209,
      .product_id = 0x5342,
      .version_number = 1,
      .transport = static_cast<uint8_t>(vhid::HidTransport::network),
      .flags = vhid::kHidAllowTransparentOutput,
  };
  const auto add = vhid::make_hid_device_add(
      9, 1, 2000, device, profile->properties().report_descriptor,
      "Raw Test", "VHID", "raw-1");
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  assert(parsed_device.header->vendor_id == 0x1209);
  assert(parsed_device.product == "Raw Test");
  assert(parsed_device.descriptor.size() ==
         profile->properties().report_descriptor.size());
  assert(std::equal(parsed_device.descriptor.begin(),
                    parsed_device.descriptor.end(),
                    profile->properties().report_descriptor.begin()));

  const uint8_t report_bytes[] = {1, 2, 3, 4};
  const auto report = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 9, 2, 3000,
      vhid::HidReportType::input, 5, report_bytes);
  assert(vhid::parse_message(report, parsed));
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  assert(parsed_report.header->report_id == 5);
  assert(parsed_report.data.size() == sizeof(report_bytes));
}

void wire_helper_test() {
  const uint8_t descriptor[] = {
      0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
      0x05, 0x09, 0x19, 0x01, 0x29, 0x01,
      0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
      0x95, 0x01, 0x81, 0x02, 0x75, 0x07,
      0x95, 0x01, 0x81, 0x03, 0xC0,
  };
  vhid_sender_t sender{};
  vhid_sender_init(&sender, 123);
  uint8_t packet[VHID_MAX_DATAGRAM_SIZE]{};
  vhid_hid_device_info_t device{};
  device.vendor_id = 0x1209;
  device.product_id = 0x5342;
  device.version_number = 1;
  device.transport = VHID_TRANSPORT_NETWORK;
  device.flags = VHID_DEVICE_ALLOW_TRANSPARENT_OUTPUT;
  device.report_descriptor = descriptor;
  device.report_descriptor_size = sizeof(descriptor);
  device.product = "Wire Helper Controller";
  device.manufacturer = "VHID";
  device.serial = "wire-1";

  size_t size =
      vhid_make_hid_device_add(&sender, &device, 10, packet, sizeof(packet));
  assert(size);
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(std::span<const uint8_t>(packet, size), parsed));
  assert(parsed.header->device_id == 123);
  assert(parsed.header->sequence == 0);
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  assert(parsed_device.product == "Wire Helper Controller");
  assert(parsed_device.descriptor.size() == sizeof(descriptor));

  const uint8_t report_bytes[] = {1};
  size = vhid_make_hid_input_report(&sender, 7, report_bytes,
                                    sizeof(report_bytes), 20, packet,
                                    sizeof(packet));
  assert(size);
  assert(vhid::parse_message(std::span<const uint8_t>(packet, size), parsed));
  assert(parsed.header->type ==
         static_cast<uint8_t>(vhid::MessageType::hid_input_report));
  assert(parsed.header->sequence == 1);
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  assert(parsed_report.header->report_id == 7);
  assert(parsed_report.data.size() == sizeof(report_bytes));
  assert(parsed_report.data[0] == report_bytes[0]);

  size = vhid_make_hid_device_remove(&sender, 30, packet, sizeof(packet));
  assert(size == sizeof(vhid_message_header_t));
  assert(vhid::parse_message(std::span<const uint8_t>(packet, size), parsed));
  assert(parsed.header->type ==
         static_cast<uint8_t>(vhid::MessageType::hid_device_remove));
  assert(parsed.header->sequence == 2);
}

void hid_source_decoder_test() {
  vhid::DeviceDescription source_description{};
  source_description.requested_profile =
      static_cast<uint8_t>(vhid::DeviceProfile::generic);
  source_description.button_count = 10;
  source_description.hat_count = 1;
  source_description.axis_count = 2;
  source_description.vendor_id = 0x1209;
  source_description.product_id = 0x5342;
  source_description.version_number = 3;
  std::strcpy(source_description.product, "Source Controller");
  std::strcpy(source_description.manufacturer, "VHID");
  std::strcpy(source_description.serial, "source-1");
  source_description.axes[0].usage_page = 1;
  source_description.axes[0].usage = 0x30;
  source_description.axes[0].logical_min = INT16_MIN;
  source_description.axes[0].logical_max = INT16_MAX;
  source_description.axes[1].usage_page = 1;
  source_description.axes[1].usage = 0x31;
  source_description.axes[1].logical_min = INT16_MIN;
  source_description.axes[1].logical_max = INT16_MAX;

  auto source_profile = vhid::make_profile(source_description);
  assert(source_profile);
  vhid::HidDeviceAddHeader device{
      .vendor_id = 0x1209,
      .product_id = 0x5342,
      .version_number = 3,
      .transport = static_cast<uint8_t>(vhid::HidTransport::network),
  };
  const auto add = vhid::make_hid_device_add(
      77, 1, 1000, device, source_profile->properties().report_descriptor,
      "Source Controller", "VHID", "source-1");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto decoder = vhid::HidSourceDecoder::create(parsed_device, error);
  assert(decoder);
  const auto& decoded_description = decoder->description();
  assert(decoded_description.vendor_id == 0x1209);
  assert(decoded_description.product_id == 0x5342);
  assert(decoded_description.version_number == 3);
  assert(decoded_description.button_count == 10);
  assert(decoded_description.hat_count == 1);
  assert(decoded_description.axis_count == 2);
  assert(decoded_description.axes[0].usage == 0x30);
  assert(decoded_description.axes[1].usage == 0x31);

  vhid::InputState source_state{};
  std::fill(std::begin(source_state.hats), std::end(source_state.hats),
            uint8_t{8});
  source_state.buttons = (uint64_t{1} << 0) | (uint64_t{1} << 3) |
                         (uint64_t{1} << 9);
  source_state.hats[0] = 2;
  source_state.axes[0] = 12345;
  source_state.axes[1] = -12000;
  auto input_report = source_profile->encode(source_state);
  const auto input = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 77, 2, 1100,
      vhid::HidReportType::input, 0, input_report);
  assert(vhid::parse_message(input, parsed));
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  vhid::InputState decoded{};
  assert(decoder->decode_input(parsed_report, decoded));
  assert(decoded.buttons == source_state.buttons);
  assert(decoded.hats[0] == 2);
  assert(decoded.axes[0] == source_state.axes[0]);
  assert(decoded.axes[1] == source_state.axes[1]);
}

void hid_source_array_button_test() {
  const uint8_t descriptor[] = {
      0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
      0x05, 0x09, 0x19, 0x01, 0x29, 0x04,
      0x15, 0x00, 0x25, 0x04, 0x75, 0x08,
      0x95, 0x02, 0x81, 0x00, 0xC0,
  };
  vhid::HidDeviceAddHeader device{
      .vendor_id = 0x1209,
      .product_id = 0x5342,
      .version_number = 1,
      .transport = static_cast<uint8_t>(vhid::HidTransport::network),
  };
  const auto add = vhid::make_hid_device_add(
      78, 1, 1000, device, descriptor, "Array Controller", "VHID",
      "array-1");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto decoder = vhid::HidSourceDecoder::create(parsed_device, error);
  assert(decoder);
  assert(decoder->description().button_count == 4);

  const uint8_t first_buttons[] = {1, 4};
  const auto first = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 78, 2, 1100,
      vhid::HidReportType::input, 0, first_buttons);
  assert(vhid::parse_message(first, parsed));
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  vhid::InputState decoded{};
  assert(decoder->decode_input(parsed_report, decoded));
  assert(decoded.buttons == ((uint64_t{1} << 0) | (uint64_t{1} << 3)));

  const uint8_t second_buttons[] = {0, 2};
  const auto second = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 78, 3, 1200,
      vhid::HidReportType::input, 0, second_buttons);
  assert(vhid::parse_message(second, parsed));
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  assert(decoder->decode_input(parsed_report, decoded));
  assert(decoded.buttons == (uint64_t{1} << 1));
}

void mapping_test() {
  vhid::InputState source{};
  source.buttons = (uint64_t{1} << 0) | (uint64_t{1} << 20);
  source.axes[0] = 8000;
  source.axes[4] = 16384;
  source.hats[0] = 2;
  source.battery_percent = 77;
  auto mapping = vhid::ControllerMapping::identity();
  mapping.axes[0].calibration.inner_deadzone = 0.10f;
  mapping.axes[4].calibration.unipolar = true;
  mapping.axes[4].calibration.minimum = 0;
  const auto output = vhid::apply_mapping(source, mapping);
  assert(output.buttons == source.buttons);
  assert(output.axes[0] > 0 && output.axes[0] < source.axes[0]);
  assert(std::abs(output.axes[4] - 16384) <= 1);
  assert(output.hats[0] == 2);
  assert(output.battery_percent == 77);
  assert(vhid::apply_axis_calibration(1000, {
      .inner_deadzone = 0.10f}) == 0);
}

void profile_test() {
  const auto description = test_description();
  auto profile = vhid::make_profile(description);
  assert(profile);
  assert(!profile->properties().report_descriptor.empty());
  assert(profile->properties().primary_usage_page == 0x01);
  assert(profile->properties().primary_usage == 0x05);
  assert(profile->properties().vendor_id_source == 1);
  vhid::InputState state{};
  state.buttons = 5;
  state.axes[0] = INT16_MIN;
  state.axes[4] = INT16_MAX;
  state.acceleration[2] = 9.80665f;
  state.angular_velocity[0] = 1.0f;
  state.battery_percent = 50;
  const auto report = profile->encode(state);
  assert(report.size() == 28);
  assert(report[0] == 5);

  const uint8_t raw_output[] = {128, 0, 64, 0, 0x34, 0x12};
  vhid::OutputState output{};
  assert(profile->decode_output(raw_output, output));
  assert(output.high_frequency == 128u * 257u);
  assert(output.low_frequency == 64u * 257u);
  assert(output.duration_ms == 0x1234);
}

void switch_profile_test() {
  auto profile = vhid::make_profile(switch_description());
  assert(profile);
  const auto& properties = profile->properties();
  assert(properties.vendor_id == 0x057e);
  assert(properties.product_id == 0x2009);
  assert(properties.version_number == 0x0210);
  assert(properties.product == "Pro Controller");
  assert(properties.manufacturer == "Nintendo Co., Ltd.");
  assert(properties.serial == "switch-pro-test");
  assert(properties.transport == "USB");
  assert(properties.primary_usage_page == 0x01);
  assert(properties.primary_usage == 0x04);
  assert(properties.vendor_id_source == 1);
  assert(properties.report_descriptor.size() == 203);

  vhid::InputState neutral{};
  std::fill(std::begin(neutral.hats), std::end(neutral.hats), uint8_t{8});
  const auto neutral_report = profile->encode(neutral);
  assert(neutral_report.size() == 64);
  assert(neutral_report[0] == 0x30);
  assert(neutral_report[1] == 0);
  assert(neutral_report[2] == 0x91);
  assert(neutral_report[3] == 0);
  assert(neutral_report[4] == 0);
  assert(neutral_report[5] == 0);
  assert(neutral_report[6] == 0x00);
  assert(neutral_report[7] == 0x08);
  assert(neutral_report[8] == 0x80);
  assert(neutral_report[9] == 0x00);
  assert(neutral_report[10] == 0x08);
  assert(neutral_report[11] == 0x80);
  assert(neutral_report[12] == 0x09);

  vhid::InputState face{};
  std::fill(std::begin(face.hats), std::end(face.hats), uint8_t{8});
  face.buttons = uint64_t{1} << 0;
  assert(profile->encode(face)[3] == 0x04);
  face.buttons = uint64_t{1} << 1;
  assert(profile->encode(face)[3] == 0x08);
  face.buttons = uint64_t{1} << 2;
  assert(profile->encode(face)[3] == 0x01);
  face.buttons = uint64_t{1} << 3;
  assert(profile->encode(face)[3] == 0x02);

  vhid::InputState buttons{};
  std::fill(std::begin(buttons.hats), std::end(buttons.hats), uint8_t{8});
  buttons.buttons = (uint64_t{1} << 0) | (uint64_t{1} << 1) |
                    (uint64_t{1} << 2) | (uint64_t{1} << 3) |
                    (uint64_t{1} << 4) | (uint64_t{1} << 5) |
                    (uint64_t{1} << 6) | (uint64_t{1} << 7) |
                    (uint64_t{1} << 8) | (uint64_t{1} << 9) |
                    (uint64_t{1} << 10) | (uint64_t{1} << 11) |
                    (uint64_t{1} << 12) | (uint64_t{1} << 13) |
                    (uint64_t{1} << 14) | (uint64_t{1} << 15) |
                    (uint64_t{1} << 16) | (uint64_t{1} << 17);
  const auto button_report = profile->encode(buttons);
  assert(button_report[3] == 0xCF);
  assert(button_report[4] == 0x3F);
  assert(button_report[5] == 0xCF);

  vhid::InputState hat{};
  std::fill(std::begin(hat.hats), std::end(hat.hats), uint8_t{8});
  hat.hats[0] = 1;
  const auto hat_report = profile->encode(hat);
  assert((hat_report[5] & 0x0F) == 0x06);

  vhid::InputState dpad{};
  std::fill(std::begin(dpad.hats), std::end(dpad.hats), uint8_t{8});
  dpad.buttons = uint64_t{1} << 12;
  assert((profile->encode(dpad)[5] & 0x0F) == 0x02);
  dpad.buttons = uint64_t{1} << 13;
  assert((profile->encode(dpad)[5] & 0x0F) == 0x01);
  dpad.buttons = uint64_t{1} << 14;
  assert((profile->encode(dpad)[5] & 0x0F) == 0x08);
  dpad.buttons = uint64_t{1} << 15;
  assert((profile->encode(dpad)[5] & 0x0F) == 0x04);

  vhid::InputState sticks{};
  std::fill(std::begin(sticks.hats), std::end(sticks.hats), uint8_t{8});
  sticks.axes[0] = INT16_MAX;
  sticks.axes[1] = INT16_MIN;
  const auto stick_report = profile->encode(sticks);
  assert(stick_report[6] == 0xFF);
  assert(stick_report[7] == 0x0D);
  assert(stick_report[8] == 0x20);

  std::vector<uint8_t> response;
  const uint8_t usb_status[] = {0x01};
  assert(profile->handle_host_report(vhid::HidReportType::output, 0x80,
                                     usb_status, response));
  assert(response.size() == 64);
  assert(response[0] == 0x81);
  assert(response[1] == 0x01);
  assert(response[3] == 0x03);

  response.clear();
  const uint8_t usb_handshake[] = {0x02};
  assert(profile->handle_host_report(vhid::HidReportType::output, 0x80,
                                     usb_handshake, response));
  assert(response.size() == 64);
  assert(response[0] == 0x81);
  assert(response[1] == 0x02);

  response.clear();
  uint8_t device_info[10]{};
  device_info[9] = 0x02;
  assert(profile->handle_host_report(vhid::HidReportType::output, 0x01,
                                     device_info, response));
  assert(response.size() == 64);
  assert(response[0] == 0x21);
  assert(response[13] == 0x82);
  assert(response[14] == 0x02);
  assert(response[17] == 0x03);

  response.clear();
  uint8_t spi_read[15]{};
  spi_read[9] = 0x10;
  spi_read[10] = 0x3D;
  spi_read[11] = 0x60;
  spi_read[14] = 18;
  assert(profile->handle_host_report(vhid::HidReportType::output, 0x01,
                                     spi_read, response));
  assert(response.size() == 64);
  assert(response[0] == 0x21);
  assert(response[13] == 0x90);
  assert(response[14] == 0x10);
  assert(response[15] == 0x3D);
  assert(response[16] == 0x60);
  assert(response[19] == 18);
  assert(response[20] != 0xFF);

  uint8_t get_report[63]{};
  size_t get_report_size = sizeof(get_report);
  assert(profile->get_host_report(vhid::HidReportType::input, 0x30,
                                  get_report, get_report_size));
  assert(get_report_size == 63);
}

}  // namespace

int main() {
  protocol_test();
  wire_helper_test();
  hid_source_decoder_test();
  hid_source_array_button_test();
  mapping_test();
  profile_test();
  switch_profile_test();
  std::cout << "VHID core protocol, mapping, calibration, descriptor, and "
               "report tests passed\n";
  return 0;
}
