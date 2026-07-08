#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

#include "vhid/hid_profile.h"
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

}  // namespace

int main() {
  protocol_test();
  wire_helper_test();
  mapping_test();
  profile_test();
  std::cout << "VHID core protocol, mapping, calibration, descriptor, and "
               "report tests passed\n";
  return 0;
}
