#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include "vhid/haptics.h"
#include "vhid/hid_profile.h"
#include "vhid/mapping.h"
#include "vhid/protocol.h"
#include "vhid/source_codec.h"
#include "vhid/wire.h"

namespace {

vhid::DeviceDescription test_description() {
  vhid::DeviceDescription description{};
  description.requested_profile =
      static_cast<uint8_t>(vhid::DeviceProfile::generic);
  description.device_flags = vhid::kDeviceHasBattery |
                             vhid::kDeviceHasMotion;
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

vhid::DeviceDescription switch2_description() {
  auto description = test_description();
  description.requested_profile =
      static_cast<uint8_t>(vhid::DeviceProfile::switch_2_pro);
  description.device_flags = vhid::kDeviceHasMotion | vhid::kDeviceHasRumble;
  description.button_count = 21;
  description.hat_count = 1;
  description.axis_count = 4;
  description.motion_flags =
      vhid::kMotionAcceleration | vhid::kMotionAngularVelocity;
  std::strcpy(description.serial, "switch2-pro-test");
  return description;
}

bool near_float(float actual, double expected, double tolerance = 0.001) {
  return std::abs(static_cast<double>(actual) - expected) <= tolerance;
}

bool near_i16(int16_t actual, int16_t expected, int tolerance = 80) {
  return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <=
         tolerance;
}

int16_t read_le_i16(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<int16_t>(
      static_cast<uint16_t>(bytes[offset]) |
      (static_cast<uint16_t>(bytes[offset + 1]) << 8));
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
  device.source_input_profile = VHID_SOURCE_INPUT_PROFILE_DESCRIPTOR;
  device.source_output_profile = VHID_PROFILE_SWITCH_PRO;
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
  assert(parsed_device.header->source_input_profile ==
         VHID_SOURCE_INPUT_PROFILE_DESCRIPTOR);
  assert(parsed_device.header->source_output_profile ==
         VHID_PROFILE_SWITCH_PRO);

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

void source_codec_descriptor_test() {
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
      .source_input_profile = vhid::kHidSourceInputProfileDescriptor,
  };
  const auto add = vhid::make_hid_device_add(
      77, 1, 1000, device, source_profile->properties().report_descriptor,
      "Source Controller", "VHID", "source-1");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto codec = vhid::make_source_input_codec(parsed_device, error);
  assert(codec);
  const auto& decoded_description = codec->description();
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
  assert(codec->decode_input(parsed_report, decoded));
  assert(decoded.buttons == source_state.buttons);
  assert(decoded.hats[0] == 2);
  assert(decoded.axes[0] == source_state.axes[0]);
  assert(decoded.axes[1] == source_state.axes[1]);
}

void source_codec_array_button_test() {
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
      .source_input_profile = vhid::kHidSourceInputProfileDescriptor,
  };
  const auto add = vhid::make_hid_device_add(
      78, 1, 1000, device, descriptor, "Array Controller", "VHID",
      "array-1");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto codec = vhid::make_source_input_codec(parsed_device, error);
  assert(codec);
  assert(codec->description().button_count == 4);

  const uint8_t first_buttons[] = {1, 4};
  const auto first = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 78, 2, 1100,
      vhid::HidReportType::input, 0, first_buttons);
  assert(vhid::parse_message(first, parsed));
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  vhid::InputState decoded{};
  assert(codec->decode_input(parsed_report, decoded));
  assert(decoded.buttons == ((uint64_t{1} << 0) | (uint64_t{1} << 3)));

  const uint8_t second_buttons[] = {0, 2};
  const auto second = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 78, 3, 1200,
      vhid::HidReportType::input, 0, second_buttons);
  assert(vhid::parse_message(second, parsed));
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  assert(codec->decode_input(parsed_report, decoded));
  assert(decoded.buttons == (uint64_t{1} << 1));
}

void source_codec_motion_test() {
  const uint8_t descriptor[] = {
      0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,
      0x06, 0x20, 0x00,
      0x0A, 0x53, 0x04, 0x0A, 0x54, 0x04, 0x0A, 0x55, 0x04,
      0x0A, 0x57, 0x04, 0x0A, 0x58, 0x04, 0x0A, 0x59, 0x04,
      0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F,
      0x55, 0x0E,
      0x75, 0x10, 0x95, 0x06, 0x81, 0x02,
      0x55, 0x00,
      0xC0,
  };
  vhid::HidDeviceAddHeader device{
      .vendor_id = 0x1209,
      .product_id = 0x5342,
      .version_number = 1,
      .transport = static_cast<uint8_t>(vhid::HidTransport::network),
      .source_input_profile = vhid::kHidSourceInputProfileDescriptor,
  };
  const auto add = vhid::make_hid_device_add(
      79, 1, 1000, device, descriptor, "Motion Controller", "VHID",
      "motion-1");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto codec = vhid::make_source_input_codec(parsed_device, error);
  assert(codec);
  assert(codec->description().device_flags & vhid::kDeviceHasMotion);
  assert(codec->description().motion_flags & vhid::kMotionAcceleration);
  assert(codec->description().motion_flags & vhid::kMotionAngularVelocity);

  std::vector<uint8_t> report_bytes;
  const auto append_i16 = [&report_bytes](int16_t value) {
    report_bytes.push_back(static_cast<uint8_t>(value));
    report_bytes.push_back(static_cast<uint8_t>(
        static_cast<uint16_t>(value) >> 8));
  };
  append_i16(100);
  append_i16(-50);
  append_i16(0);
  append_i16(9000);
  append_i16(-4500);
  append_i16(0);
  const auto input = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 79, 2, 1100,
      vhid::HidReportType::input, 0, report_bytes);
  assert(vhid::parse_message(input, parsed));
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  vhid::InputState decoded{};
  assert(codec->decode_input(parsed_report, decoded));
  assert(near_float(decoded.acceleration[0], 9.80665));
  assert(near_float(decoded.acceleration[1], -4.903325));
  assert(near_float(decoded.acceleration[2], 0.0));
  assert(near_float(decoded.angular_velocity[0], 1.5707963267948966));
  assert(near_float(decoded.angular_velocity[1], -0.7853981633974483));
  assert(near_float(decoded.angular_velocity[2], 0.0));
}

void source_codec_profile_test() {
  vhid::HidDeviceAddHeader inferred{
      .vendor_id = 0x057e,
      .product_id = 0x2009,
      .source_input_profile = vhid::kHidSourceInputProfileInfer,
      .source_output_profile = vhid::kHidSourceOutputProfileDefault,
  };
  assert(vhid::source_input_profile(inferred) ==
         vhid::DeviceProfile::switch_pro);
  assert(vhid::source_output_profile(inferred) ==
         vhid::DeviceProfile::switch_pro);

  vhid::HidDeviceAddHeader inferred_switch2 = inferred;
  inferred_switch2.product_id = 0x2069;
  assert(vhid::source_input_profile(inferred_switch2) ==
         vhid::DeviceProfile::switch_2_pro);
  assert(vhid::source_output_profile(inferred_switch2) ==
         vhid::DeviceProfile::switch_2_pro);

  vhid::HidDeviceAddHeader descriptor = inferred;
  descriptor.source_input_profile = vhid::kHidSourceInputProfileDescriptor;
  assert(!vhid::source_input_profile(descriptor));
  assert(!vhid::source_output_profile(descriptor));

  vhid::HidDeviceAddHeader explicit_unknown = inferred;
  explicit_unknown.source_input_profile = 0x80;
  assert(!vhid::source_input_profile(explicit_unknown));
  assert(!vhid::source_output_profile(explicit_unknown));

  vhid::HidDeviceAddHeader explicit_output = descriptor;
  explicit_output.source_output_profile =
      static_cast<uint8_t>(vhid::DeviceProfile::switch_pro);
  assert(vhid::source_output_profile(explicit_output) ==
         vhid::DeviceProfile::switch_pro);

  vhid::HidDeviceAddHeader invalid_output = descriptor;
  invalid_output.source_output_profile = 0x80;
  assert(!vhid::source_output_profile(invalid_output));

  vhid::HidDeviceAddHeader disabled = inferred;
  disabled.source_output_profile = vhid::kHidSourceOutputProfileNone;
  assert(vhid::source_input_profile(disabled) ==
         vhid::DeviceProfile::switch_pro);
  assert(!vhid::source_output_profile(disabled));

  auto output_codec =
      vhid::make_source_output_codec(vhid::DeviceProfile::switch_pro);
  assert(output_codec);
  assert(output_codec->accepts_native_reports_from(
      vhid::DeviceProfile::switch_pro));
  assert(!output_codec->accepts_native_reports_from(
      vhid::DeviceProfile::generic));
  const uint8_t switch1_hd_bytes[] = {
      0x00, 0xC8, 0x80, 0x72,
      0x00, 0xC8, 0x80, 0x72,
  };
  vhid::OutputState switch_rumble{};
  vhid::decode_switch1_hd_rumble(switch1_hd_bytes, switch_rumble);
  vhid::SourceReport switch_source_rumble;
  assert(output_codec->encode_output(switch_rumble, switch_source_rumble));
  assert(switch_source_rumble.type == vhid::HidReportType::output);
  assert(switch_source_rumble.report_id == 0x10);
  assert(switch_source_rumble.data.size() == 9);
  assert(std::equal(switch_source_rumble.data.begin() + 1,
                    switch_source_rumble.data.end(),
                    std::begin(switch1_hd_bytes)));

  auto switch2_output_codec =
      vhid::make_source_output_codec(vhid::DeviceProfile::switch_2_pro);
  assert(switch2_output_codec);
  assert(switch2_output_codec->accepts_native_reports_from(
      vhid::DeviceProfile::switch_2_pro));
  assert(!switch2_output_codec->accepts_native_reports_from(
      vhid::DeviceProfile::switch_pro));
  vhid::OutputState rumble{};
  rumble.low_frequency = 65535;
  rumble.high_frequency = 32768;
  vhid::SourceReport source_rumble;
  assert(switch2_output_codec->encode_output(rumble, source_rumble));
  assert(source_rumble.type == vhid::HidReportType::output);
  assert(source_rumble.report_id == 0);
  assert(source_rumble.data.size() == 64);
  assert(source_rumble.data[0] == 0x02);
  assert((source_rumble.data[1] & 0xF0) == 0x50);
  assert(std::equal(source_rumble.data.begin() + 1,
                    source_rumble.data.begin() + 7,
                    source_rumble.data.begin() + 0x11));

  const auto switch2_high_frequency = [](const std::vector<uint8_t>& data,
                                         size_t offset) {
    return static_cast<uint16_t>(data[offset] |
                                 ((data[offset + 1] & 0x03u) << 8));
  };
  const auto switch2_low_frequency = [](const std::vector<uint8_t>& data,
                                        size_t offset) {
    return static_cast<uint16_t>((data[offset + 2] >> 4) |
                                 ((data[offset + 3] & 0x3Fu) << 4));
  };
  const uint16_t default_switch2_high =
      switch2_high_frequency(source_rumble.data, 2);
  const uint16_t default_switch2_low =
      switch2_low_frequency(source_rumble.data, 2);
  const uint8_t switch1_tone_bytes[] = {
      0x68, 0xBE, 0xBA, 0x6F,
      0x98, 0x1C, 0x46, 0x47,
  };
  vhid::OutputState switch1_tone{};
  vhid::decode_switch1_hd_rumble(switch1_tone_bytes, switch1_tone);
  vhid::SourceReport switch2_tone;
  assert(switch2_output_codec->encode_output(switch1_tone, switch2_tone));
  assert(switch2_tone.data.size() == 64);
  assert(switch2_high_frequency(switch2_tone.data, 2) != default_switch2_high);
  assert(switch2_low_frequency(switch2_tone.data, 2) != default_switch2_low);
  assert(switch2_high_frequency(switch2_tone.data, 0x12) !=
         switch2_high_frequency(switch2_tone.data, 2));
  assert(switch2_low_frequency(switch2_tone.data, 0x12) !=
         switch2_low_frequency(switch2_tone.data, 2));

  vhid::OutputState split_rumble{};
  vhid::HapticMotorState left_motor{};
  left_motor.low.frequency_hz = 160;
  left_motor.high.frequency_hz = 320;
  left_motor.low.amplitude = 65535;
  left_motor.high.amplitude = 32768;
  vhid::set_haptic_motor(split_rumble, 0, left_motor);
  vhid::HapticMotorState right_motor{};
  right_motor.low.frequency_hz = 160;
  right_motor.high.frequency_hz = 320;
  vhid::set_haptic_motor(split_rumble, 1, right_motor);
  vhid::SourceReport split_source_rumble;
  assert(switch2_output_codec->encode_output(split_rumble,
                                             split_source_rumble));
  assert(split_source_rumble.data.size() == 64);
  assert(!std::equal(split_source_rumble.data.begin() + 2,
                     split_source_rumble.data.begin() + 7,
                     split_source_rumble.data.begin() + 0x12));
  assert(split_source_rumble.data[0x11] == split_source_rumble.data[1]);

  auto source_profile = vhid::make_profile(test_description());
  assert(source_profile);
  vhid::HidDeviceAddHeader unsupported{
      .vendor_id = 0x1209,
      .product_id = 0x5342,
      .version_number = 1,
      .transport = static_cast<uint8_t>(vhid::HidTransport::network),
      .source_input_profile =
          static_cast<uint8_t>(vhid::DeviceProfile::dualshock_4),
  };
  const auto add = vhid::make_hid_device_add(
      80, 1, 1000, unsupported,
      source_profile->properties().report_descriptor,
      "Unsupported Native Source", "VHID", "fallback-1");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto input_codec = vhid::make_source_input_codec(parsed_device, error);
  assert(input_codec);
  assert(input_codec->profile() == vhid::DeviceProfile::generic);

  unsupported.source_input_profile = 0x80;
  const auto unknown_add = vhid::make_hid_device_add(
      81, 1, 1000, unsupported,
      source_profile->properties().report_descriptor,
      "Unknown Native Source", "VHID", "fallback-2");
  assert(vhid::parse_message(unknown_add, parsed));
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  error.clear();
  input_codec = vhid::make_source_input_codec(parsed_device, error);
  assert(input_codec);
  assert(input_codec->profile() == vhid::DeviceProfile::generic);
}

void switch_source_codec_test() {
  auto source_profile = vhid::make_profile(switch_description());
  assert(source_profile);
  vhid::HidDeviceAddHeader device{
      .vendor_id = 0x057e,
      .product_id = 0x2009,
      .version_number = 0x0210,
      .transport = static_cast<uint8_t>(vhid::HidTransport::network),
      .source_input_profile = vhid::kHidSourceInputProfileInfer,
      .source_output_profile = vhid::kHidSourceOutputProfileDefault,
  };
  const auto add = vhid::make_hid_device_add(
      82, 1, 1000, device,
      source_profile->properties().report_descriptor,
      "Pro Controller", "Nintendo Co., Ltd.", "switch-source-1");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto codec = vhid::make_source_input_codec(parsed_device, error);
  assert(codec);
  assert(codec->profile() == vhid::DeviceProfile::switch_pro);
  assert(codec->description().requested_profile ==
         static_cast<uint8_t>(vhid::DeviceProfile::switch_pro));

  vhid::InputState source{};
  std::fill(std::begin(source.hats), std::end(source.hats), uint8_t{8});
  source.buttons = (uint64_t{1} << 0) | (uint64_t{1} << 5) |
                   (uint64_t{1} << 8) | (uint64_t{1} << 12) |
                   (uint64_t{1} << 17);
  source.axes[0] = 12000;
  source.axes[1] = -9000;
  source.axes[2] = 22000;
  source.axes[3] = -16000;
  source.battery_percent = 90;
  source.acceleration[0] = 4.903325f;
  source.acceleration[1] = -2.4516625f;
  source.acceleration[2] = 9.80665f;
  source.angular_velocity[0] = 0.25f;
  source.angular_velocity[1] = -0.5f;
  source.angular_velocity[2] = 0.0f;
  const auto encoded = source_profile->encode(source);
  assert(encoded.size() == 64);
  const auto input = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 82, 2, 1100,
      vhid::HidReportType::input, encoded[0],
      std::span<const uint8_t>(encoded.data() + 1, encoded.size() - 1));
  assert(vhid::parse_message(input, parsed));
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  vhid::InputState decoded{};
  assert(codec->decode_input(parsed_report, decoded));
  assert(decoded.buttons == source.buttons);
  assert(near_i16(decoded.axes[0], source.axes[0]));
  assert(near_i16(decoded.axes[1], source.axes[1]));
  assert(near_i16(decoded.axes[2], source.axes[2]));
  assert(near_i16(decoded.axes[3], source.axes[3]));
  assert(decoded.battery_percent == 100);
  assert(near_float(decoded.acceleration[0], source.acceleration[0], 0.05));
  assert(near_float(decoded.acceleration[1], source.acceleration[1], 0.05));
  assert(near_float(decoded.acceleration[2], source.acceleration[2], 0.05));
  assert(near_float(decoded.angular_velocity[0],
                    source.angular_velocity[0], 0.01));
  assert(near_float(decoded.angular_velocity[1],
                    source.angular_velocity[1], 0.01));
}

void switch2_source_codec_test() {
  auto source_profile = vhid::make_profile(switch2_description());
  assert(source_profile);
  vhid::HidDeviceAddHeader device{
      .vendor_id = 0x057e,
      .product_id = 0x2069,
      .version_number = 0x0100,
      .transport = static_cast<uint8_t>(vhid::HidTransport::network),
      .source_input_profile = vhid::kHidSourceInputProfileInfer,
      .source_output_profile = vhid::kHidSourceOutputProfileDefault,
  };
  const auto add = vhid::make_hid_device_add(
      83, 1, 1000, device,
      source_profile->properties().report_descriptor,
      "Nintendo Switch 2 Pro Controller", "Nintendo Co., Ltd.",
      "switch2-source-1");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto codec = vhid::make_source_input_codec(parsed_device, error);
  assert(codec);
  assert(codec->profile() == vhid::DeviceProfile::switch_2_pro);
  assert(codec->description().requested_profile ==
         static_cast<uint8_t>(vhid::DeviceProfile::switch_2_pro));

  vhid::InputState source{};
  std::fill(std::begin(source.hats), std::end(source.hats), uint8_t{8});
  source.buttons = (uint64_t{1} << 0) | (uint64_t{1} << 5) |
                   (uint64_t{1} << 8) | (uint64_t{1} << 12) |
                   (uint64_t{1} << 17) | (uint64_t{1} << 18) |
                   (uint64_t{1} << 19) | (uint64_t{1} << 20);
  source.axes[0] = 12000;
  source.axes[1] = -9000;
  source.axes[2] = 22000;
  source.axes[3] = -16000;
  source.acceleration[0] = 4.903325f;
  source.acceleration[1] = -2.4516625f;
  source.acceleration[2] = 9.80665f;
  source.angular_velocity[0] = 0.25f;
  source.angular_velocity[1] = -0.5f;
  source.angular_velocity[2] = 0.125f;
  auto encoded = source_profile->encode(source);
  assert(read_le_i16(encoded, 0x37) >= 235);
  assert(read_le_i16(encoded, 0x37) <= 236);
  assert(read_le_i16(encoded, 0x39) >= -118);
  assert(read_le_i16(encoded, 0x39) <= -117);
  assert(read_le_i16(encoded, 0x3B) >= -471);
  assert(read_le_i16(encoded, 0x3B) <= -470);
  const auto invert_axis = [](int16_t value) {
    return static_cast<int16_t>(~value);
  };
  const auto pack_raw_switch2_stick = [](uint8_t* out, int16_t x, int16_t y) {
    const auto scale = [](int16_t value) {
      const int32_t scaled =
          2048 + (static_cast<int32_t>(value) * 1536 / 32768);
      return static_cast<uint16_t>(std::clamp(scaled, 0, 4095));
    };
    const uint16_t ux = scale(x);
    const uint16_t uy = scale(y);
    out[0] = static_cast<uint8_t>(ux & 0xFF);
    out[1] = static_cast<uint8_t>((ux >> 8) | ((uy & 0x0F) << 4));
    out[2] = static_cast<uint8_t>(uy >> 4);
  };
  pack_raw_switch2_stick(&encoded[11], source.axes[0],
                         invert_axis(source.axes[1]));
  pack_raw_switch2_stick(&encoded[14], source.axes[2],
                         invert_axis(source.axes[3]));
  assert(encoded.size() == 64);
  const auto input = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 83, 2, 1100,
      vhid::HidReportType::input, 0, encoded);
  assert(vhid::parse_message(input, parsed));
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  vhid::InputState decoded{};
  assert(codec->decode_input(parsed_report, decoded));
  assert(decoded.buttons == source.buttons);
  assert(decoded.hats[0] == 0);
  assert(near_i16(decoded.axes[0], source.axes[0]));
  assert(near_i16(decoded.axes[1], source.axes[1]));
  assert(near_i16(decoded.axes[2], source.axes[2]));
  assert(near_i16(decoded.axes[3], source.axes[3]));
  assert(near_float(decoded.acceleration[0], source.acceleration[0], 0.05));
  assert(near_float(decoded.acceleration[1], source.acceleration[1], 0.05));
  assert(near_float(decoded.acceleration[2], source.acceleration[2], 0.05));
  assert(near_float(decoded.angular_velocity[0],
                    source.angular_velocity[0], 0.01));
  assert(near_float(decoded.angular_velocity[1],
                    source.angular_velocity[1], 0.01));
  assert(near_float(decoded.angular_velocity[2],
                    source.angular_velocity[2], 0.01));
}

void switch2_native_udp_button_round_trip_test() {
  auto source_profile = vhid::make_profile(switch2_description());
  assert(source_profile);
  vhid::HidDeviceAddHeader device{
      .vendor_id = 0x057e,
      .product_id = 0x2069,
      .version_number = 0x0100,
      .transport = static_cast<uint8_t>(vhid::HidTransport::network),
      .source_input_profile = vhid::kHidSourceInputProfileInfer,
      .source_output_profile = vhid::kHidSourceOutputProfileDefault,
  };
  const auto add = vhid::make_hid_device_add(
      84, 1, 1000, device,
      source_profile->properties().report_descriptor,
      "Nintendo Switch 2 Pro Controller", "Nintendo Co., Ltd.",
      "switch2-source-2");
  vhid::ParsedMessage parsed;
  assert(vhid::parse_message(add, parsed));
  vhid::ParsedHidDeviceAdd parsed_device;
  assert(vhid::parse_hid_device_add(parsed.payload, parsed_device));
  std::string error;
  auto source_codec = vhid::make_source_input_codec(parsed_device, error);
  assert(source_codec);

  std::vector<uint8_t> source_report(64);
  source_report[5] = 0xCF;
  source_report[6] = 0x7F;
  source_report[7] = 0xCF;
  source_report[8] = 0x03;
  source_report[11] = 0x00;
  source_report[12] = 0x08;
  source_report[13] = 0x80;
  source_report[14] = 0x00;
  source_report[15] = 0x08;
  source_report[16] = 0x80;
  const auto input = vhid::make_hid_report(
      vhid::MessageType::hid_input_report, 84, 2, 1100,
      vhid::HidReportType::input, 0, source_report);
  assert(vhid::parse_message(input, parsed));
  vhid::ParsedHidReport parsed_report;
  assert(vhid::parse_hid_report(parsed.payload, parsed_report));
  vhid::InputState decoded{};
  assert(source_codec->decode_input(parsed_report, decoded));

  auto output_profile = vhid::make_profile(source_codec->description());
  assert(output_profile);
  const auto output = output_profile->encode(decoded);
  assert(output[5] == source_report[5]);
  assert(output[6] == source_report[6]);
  assert(output[7] == source_report[7]);
  assert(output[8] == source_report[8]);
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
  assert(!profile->decode_output(raw_output, output));
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
  assert(neutral_report[12] == 0x70);
  assert(read_le_i16(neutral_report, 13) == 0);
  assert(read_le_i16(neutral_report, 15) == 0);
  assert(read_le_i16(neutral_report, 17) == 4096);

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

  vhid::InputState motion{};
  std::fill(std::begin(motion.hats), std::end(motion.hats), uint8_t{8});
  motion.acceleration[0] = 9.80665f;
  motion.acceleration[1] = -9.80665f;
  motion.acceleration[2] = 0.0f;
  motion.angular_velocity[0] = 0.60737458f;
  motion.angular_velocity[1] = -0.60737458f;
  motion.angular_velocity[2] = 0.0f;
  const auto motion_report = profile->encode(motion);
  assert(read_le_i16(motion_report, 13) == 0);
  assert(read_le_i16(motion_report, 15) == -4096);
  assert(read_le_i16(motion_report, 17) == -4096);
  assert(read_le_i16(motion_report, 19) == 0);
  assert(read_le_i16(motion_report, 21) >= -500);
  assert(read_le_i16(motion_report, 21) <= -490);
  assert(read_le_i16(motion_report, 23) >= -500);
  assert(read_le_i16(motion_report, 23) <= -490);

  vhid::OutputState neutral_rumble{};
  const uint8_t neutral_output[] = {
      0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
  };
  assert(profile->decode_output(neutral_output, neutral_rumble));
  assert(neutral_rumble.high_frequency == 0);
  assert(neutral_rumble.low_frequency == 0);
  assert(neutral_rumble.duration_ms == 0);

  vhid::OutputState active_rumble{};
  const uint8_t active_output[] = {
      0x10, 0x00, 0x00, 0xC8, 0x80, 0x72,
      0x00, 0xC8, 0x80, 0x72,
  };
  assert(profile->decode_output(active_output, active_rumble));
  assert(active_rumble.high_frequency > 60000);
  assert(active_rumble.low_frequency > 60000);
  assert(active_rumble.duration_ms == 100);
  assert(active_rumble.haptic_flags & vhid::kOutputHapticSwitch1HdPacket);
  assert(active_rumble.haptic_flags & vhid::kOutputHapticMotorLeft);
  assert(active_rumble.haptic_flags & vhid::kOutputHapticMotorRight);
  assert(active_rumble.motor_count == 2);
  assert(std::memcmp(active_rumble.motors[0].switch1_hd,
                     active_output + 2, 4) == 0);
  assert(std::memcmp(active_rumble.motors[1].switch1_hd,
                     active_output + 6, 4) == 0);

  const uint8_t sequenced_output[] = {
      0x10, 0x0C, 0x00, 0xC8, 0x80, 0x72,
      0x00, 0xC8, 0x80, 0x72,
  };
  assert(profile->decode_output(sequenced_output, active_rumble));
  assert(profile->encode(neutral)[12] == 0xC0);

  vhid::OutputState ignored_rumble{};
  const uint8_t usb_output[] = {
      0x80, 0x02, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
  };
  assert(!profile->decode_output(usb_output, ignored_rumble));
  const uint8_t unknown_output[] = {
      0x82, 0x00, 0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40,
  };
  assert(!profile->decode_output(unknown_output, ignored_rumble));

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
  device_info[0] = 0x0B;
  device_info[9] = 0x02;
  assert(profile->handle_host_report(vhid::HidReportType::output, 0x01,
                                     device_info, response));
  assert(response.size() == 64);
  assert(response[0] == 0x21);
  assert(response[12] == 0xB0);
  assert(response[13] == 0x82);
  assert(response[14] == 0x02);
  assert(response[17] == 0x03);

  response.clear();
  uint8_t spi_read[15]{};
  spi_read[0] = 0x0C;
  spi_read[9] = 0x10;
  spi_read[10] = 0x3D;
  spi_read[11] = 0x60;
  spi_read[14] = 18;
  assert(profile->handle_host_report(vhid::HidReportType::output, 0x01,
                                     spi_read, response));
  assert(response.size() == 64);
  assert(response[0] == 0x21);
  assert(response[12] == 0xC0);
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

void switch2_profile_test() {
  auto profile = vhid::make_profile(switch2_description());
  assert(profile);
  const auto& properties = profile->properties();
  assert(properties.vendor_id == 0x057e);
  assert(properties.product_id == 0x2069);
  assert(properties.version_number == 0x0100);
  assert(properties.product == "Nintendo Switch 2 Pro Controller");
  assert(properties.manufacturer == "Nintendo Co., Ltd.");
  assert(properties.serial == "switch2-pro-test");
  assert(properties.transport == "USB");
  assert(properties.primary_usage_page == 0x01);
  assert(properties.primary_usage == 0x05);
  assert(properties.vendor_id_source == 1);
  assert(!properties.report_descriptor.empty());

  vhid::InputState neutral{};
  std::fill(std::begin(neutral.hats), std::end(neutral.hats), uint8_t{8});
  const auto neutral_report = profile->encode(neutral);
  assert(neutral_report.size() == 64);
  assert(neutral_report[5] == 0);
  assert(neutral_report[6] == 0);
  assert(neutral_report[7] == 0);
  assert(neutral_report[8] == 0);
  assert(neutral_report[11] == 0x00);
  assert(neutral_report[12] == 0x08);
  assert(neutral_report[13] == 0x80);
  assert(neutral_report[14] == 0x00);
  assert(neutral_report[15] == 0x08);
  assert(neutral_report[16] == 0x80);
  assert(neutral_report[0x2B] || neutral_report[0x2C] ||
         neutral_report[0x2D] || neutral_report[0x2E]);

  vhid::InputState face{};
  std::fill(std::begin(face.hats), std::end(face.hats), uint8_t{8});
  face.buttons = uint64_t{1} << 0;
  assert(profile->encode(face)[5] == 0x04);
  face.buttons = uint64_t{1} << 1;
  assert(profile->encode(face)[5] == 0x08);
  face.buttons = uint64_t{1} << 2;
  assert(profile->encode(face)[5] == 0x01);
  face.buttons = uint64_t{1} << 3;
  assert(profile->encode(face)[5] == 0x02);

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
                    (uint64_t{1} << 16) | (uint64_t{1} << 17) |
                    (uint64_t{1} << 18) | (uint64_t{1} << 19) |
                    (uint64_t{1} << 20);
  const auto button_report = profile->encode(buttons);
  assert(button_report[5] == 0xCF);
  assert(button_report[6] == 0x7F);
  assert(button_report[7] == 0xCF);
  assert(button_report[8] == 0x03);

  vhid::InputState hat{};
  std::fill(std::begin(hat.hats), std::end(hat.hats), uint8_t{8});
  hat.hats[0] = 1;
  assert((profile->encode(hat)[7] & 0x0F) == 0x06);

  vhid::InputState sticks{};
  std::fill(std::begin(sticks.hats), std::end(sticks.hats), uint8_t{8});
  sticks.axes[0] = INT16_MAX;
  sticks.axes[1] = INT16_MIN;
  const auto stick_report = profile->encode(sticks);
  assert(stick_report[11] == 0xFF);
  assert(stick_report[12] == 0x0F);
  assert(stick_report[13] == 0x00);

  vhid::InputState motion{};
  std::fill(std::begin(motion.hats), std::end(motion.hats), uint8_t{8});
  motion.acceleration[0] = 9.80665f;
  motion.acceleration[1] = -9.80665f;
  motion.acceleration[2] = 0.0f;
  motion.angular_velocity[0] = 0.60737458f;
  motion.angular_velocity[1] = -0.60737458f;
  motion.angular_velocity[2] = 0.0f;
  const auto motion_report = profile->encode(motion);
  assert(read_le_i16(motion_report, 0x31) >= 4095);
  assert(read_le_i16(motion_report, 0x31) <= 4096);
  assert(read_le_i16(motion_report, 0x33) == 0);
  assert(read_le_i16(motion_report, 0x35) >= -4096);
  assert(read_le_i16(motion_report, 0x35) <= -4095);
  assert(read_le_i16(motion_report, 0x37) >= 571);
  assert(read_le_i16(motion_report, 0x37) <= 572);
  assert(read_le_i16(motion_report, 0x39) == 0);
  assert(read_le_i16(motion_report, 0x3B) >= -572);
  assert(read_le_i16(motion_report, 0x3B) <= -571);

  vhid::OutputState neutral_rumble{};
  std::vector<uint8_t> neutral_output(64);
  neutral_output[0] = 0x02;
  assert(profile->decode_output(neutral_output, neutral_rumble));
  assert(neutral_rumble.high_frequency == 0);
  assert(neutral_rumble.low_frequency == 0);
  assert(neutral_rumble.duration_ms == 0);

  vhid::OutputState active_rumble{};
  std::vector<uint8_t> active_output(64);
  active_output[0] = 0x02;
  active_output[1] = 0x50;
  active_output[3] = 0xFC;
  active_output[4] = 0x01;
  active_output[5] = 0xC0;
  active_output[6] = 0x01;
  assert(profile->decode_output(active_output, active_rumble));
  assert(active_rumble.high_frequency > 0);
  assert(active_rumble.low_frequency > 0);
  assert(active_rumble.duration_ms == 100);
  assert(active_rumble.haptic_flags & vhid::kOutputHapticMotorLeft);
  assert(active_rumble.haptic_flags & vhid::kOutputHapticMotorRight);
  assert(active_rumble.motor_count == 2);
  assert(active_rumble.motors[0].high.encoded_amplitude > 0);
  assert(active_rumble.motors[0].low.encoded_amplitude > 0);

  vhid::OutputState split_source{};
  vhid::HapticMotorState left_motor{};
  left_motor.low.frequency_hz = 160;
  left_motor.high.frequency_hz = 320;
  left_motor.low.amplitude = 65535;
  left_motor.high.amplitude = 32768;
  vhid::set_haptic_motor(split_source, 0, left_motor);
  vhid::HapticMotorState right_motor{};
  right_motor.low.frequency_hz = 450;
  right_motor.high.frequency_hz = 900;
  right_motor.low.amplitude = 32768;
  right_motor.high.amplitude = 16384;
  vhid::set_haptic_motor(split_source, 1, right_motor);
  auto source_output =
      vhid::make_source_output_codec(vhid::DeviceProfile::switch_2_pro);
  assert(source_output);
  vhid::SourceReport split_report;
  assert(source_output->encode_output(split_source, split_report));
  vhid::OutputState split_decoded{};
  assert(profile->decode_output(split_report.data, split_decoded));
  assert(split_decoded.haptic_flags & vhid::kOutputHapticMotorLeft);
  assert(split_decoded.haptic_flags & vhid::kOutputHapticMotorRight);
  assert(split_decoded.motors[0].high.encoded_frequency !=
         split_decoded.motors[1].high.encoded_frequency);
  assert(split_decoded.motors[0].low.encoded_frequency !=
         split_decoded.motors[1].low.encoded_frequency);
  assert(split_decoded.motors[0].high.encoded_amplitude !=
         split_decoded.motors[1].high.encoded_amplitude);

  uint8_t get_report[64]{};
  size_t get_report_size = sizeof(get_report);
  assert(profile->get_host_report(vhid::HidReportType::input, 0,
                                  get_report, get_report_size));
  assert(get_report_size == 64);
}

}  // namespace

int main() {
  protocol_test();
  wire_helper_test();
  source_codec_descriptor_test();
  source_codec_array_button_test();
  source_codec_motion_test();
  source_codec_profile_test();
  switch_source_codec_test();
  switch2_source_codec_test();
  switch2_native_udp_button_round_trip_test();
  mapping_test();
  profile_test();
  switch_profile_test();
  switch2_profile_test();
  std::cout << "VHID core protocol, mapping, calibration, descriptor, and "
               "report tests passed\n";
  return 0;
}
