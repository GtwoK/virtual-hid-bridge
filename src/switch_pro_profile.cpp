#include "vhid/hid_profile.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace vhid {
namespace {

// Switch 1 Pro USB HID report IDs and the 0x30 input layout follow
// dekuNukem/Nintendo_Switch_Reverse_Engineering and the existing
// Switchboard Switch 1 Pro implementation.
constexpr std::array<uint8_t, 203> kSwitchProReportDescriptor = {
    0x05, 0x01, 0x15, 0x00, 0x09, 0x04, 0xA1, 0x01,
    0x85, 0x30, 0x05, 0x01, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x0A, 0x15, 0x00, 0x25, 0x01, 0x75, 0x01,
    0x95, 0x0A, 0x55, 0x00, 0x65, 0x00, 0x81, 0x02,
    0x05, 0x09, 0x19, 0x0B, 0x29, 0x0E, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x75, 0x01, 0x95, 0x02, 0x81, 0x03, 0x0B, 0x01,
    0x00, 0x01, 0x00, 0xA1, 0x00, 0x0B, 0x30, 0x00,
    0x01, 0x00, 0x0B, 0x31, 0x00, 0x01, 0x00, 0x0B,
    0x32, 0x00, 0x01, 0x00, 0x0B, 0x35, 0x00, 0x01,
    0x00, 0x15, 0x00, 0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10, 0x95, 0x04, 0x81, 0x02, 0xC0, 0x0B,
    0x39, 0x00, 0x01, 0x00, 0x15, 0x00, 0x25, 0x07,
    0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14, 0x75,
    0x04, 0x95, 0x01, 0x81, 0x02, 0x05, 0x09, 0x19,
    0x0F, 0x29, 0x12, 0x15, 0x00, 0x25, 0x01, 0x75,
    0x01, 0x95, 0x04, 0x81, 0x02, 0x75, 0x08, 0x95,
    0x34, 0x81, 0x03, 0x06, 0x00, 0xFF, 0x85, 0x21,
    0x09, 0x01, 0x75, 0x08, 0x95, 0x3F, 0x81, 0x03,
    0x85, 0x81, 0x09, 0x02, 0x75, 0x08, 0x95, 0x3F,
    0x81, 0x03, 0x85, 0x01, 0x09, 0x03, 0x75, 0x08,
    0x95, 0x3F, 0x91, 0x83, 0x85, 0x10, 0x09, 0x04,
    0x75, 0x08, 0x95, 0x3F, 0x91, 0x83, 0x85, 0x80,
    0x09, 0x05, 0x75, 0x08, 0x95, 0x3F, 0x91, 0x83,
    0x85, 0x82, 0x09, 0x06, 0x75, 0x08, 0x95, 0x3F,
    0x91, 0x83, 0xC0,
};

std::string bounded_string(const char* text, size_t size,
                           const char* fallback) {
  const size_t length = strnlen(text, size);
  return length ? std::string(text, length) : fallback;
}

bool button_pressed(const InputState& state, uint8_t index) {
  return index < kMaxButtons && (state.buttons & (uint64_t{1} << index));
}

uint8_t switch_battery_connection(uint8_t battery_percent) {
  if (battery_percent == 0) return 0x91;
  if (battery_percent >= 80) return 0x81;
  if (battery_percent >= 50) return 0x61;
  if (battery_percent >= 20) return 0x41;
  return 0x21;
}

void apply_hat(uint8_t hat, uint8_t& dpad_byte) {
  if (hat > 7) return;
  const bool up = hat == 0 || hat == 1 || hat == 7;
  const bool right = hat == 1 || hat == 2 || hat == 3;
  const bool down = hat == 3 || hat == 4 || hat == 5;
  const bool left = hat == 5 || hat == 6 || hat == 7;
  if (down) dpad_byte |= 0x01;
  if (up) dpad_byte |= 0x02;
  if (right) dpad_byte |= 0x04;
  if (left) dpad_byte |= 0x08;
}

void pack_stick(uint8_t* out, int16_t x, int16_t y) {
  const auto scale = [](int16_t value) {
    const int32_t scaled = 2048 + (static_cast<int32_t>(value) * 1536 / 32768);
    return static_cast<uint16_t>(std::clamp(scaled, 0, 4095));
  };
  const uint16_t ux = scale(x);
  const uint16_t uy = scale(y);
  out[0] = static_cast<uint8_t>(ux & 0xFF);
  out[1] = static_cast<uint8_t>((ux >> 8) | ((uy & 0x0F) << 4));
  out[2] = static_cast<uint8_t>(uy >> 4);
}

void append_i16(uint8_t* out, int16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
}

int16_t clamp_i16(long value) {
  return static_cast<int16_t>(std::clamp(value, -32768l, 32767l));
}

class SwitchProProfile final : public HidProfile {
 public:
  explicit SwitchProProfile(const DeviceDescription& description) {
    has_hat_ = description.hat_count > 0;
    properties_.vendor_id = 0x057E;
    properties_.product_id = 0x2009;
    properties_.version_number = 0x0210;
    properties_.product = "Pro Controller";
    properties_.manufacturer = "Nintendo Co., Ltd.";
    properties_.serial = bounded_string(description.serial,
                                        sizeof(description.serial),
                                        "vhid-switch-pro-1");
    properties_.transport = "USB";
    properties_.report_descriptor.assign(kSwitchProReportDescriptor.begin(),
                                         kSwitchProReportDescriptor.end());
  }

  const HidDeviceProperties& properties() const override {
    return properties_;
  }

  std::vector<uint8_t> encode(const InputState& state) const override {
    std::vector<uint8_t> report(64);
    report[0] = 0x30;
    report[1] = timer_++;
    report[2] = switch_battery_connection(state.battery_percent);

    if (button_pressed(state, 2)) report[3] |= 0x01;   // Y
    if (button_pressed(state, 3)) report[3] |= 0x02;   // X
    if (button_pressed(state, 0)) report[3] |= 0x04;   // B
    if (button_pressed(state, 1)) report[3] |= 0x08;   // A
    if (button_pressed(state, 5)) report[3] |= 0x40;   // R
    if (button_pressed(state, 7)) report[3] |= 0x80;   // ZR

    if (button_pressed(state, 8)) report[4] |= 0x01;   // Minus
    if (button_pressed(state, 9)) report[4] |= 0x02;   // Plus
    if (button_pressed(state, 11)) report[4] |= 0x04;  // Right stick
    if (button_pressed(state, 10)) report[4] |= 0x08;  // Left stick
    if (button_pressed(state, 16)) report[4] |= 0x10;  // Home
    if (button_pressed(state, 17)) report[4] |= 0x20;  // Capture

    if (button_pressed(state, 13)) report[5] |= 0x01;  // Down
    if (button_pressed(state, 12)) report[5] |= 0x02;  // Up
    if (button_pressed(state, 15)) report[5] |= 0x04;  // Right
    if (button_pressed(state, 14)) report[5] |= 0x08;  // Left
    if (button_pressed(state, 4)) report[5] |= 0x40;   // L
    if (button_pressed(state, 6)) report[5] |= 0x80;   // ZL
    if (has_hat_ && (report[5] & 0x0F) == 0) {
      apply_hat(state.hats[0], report[5]);
    }

    pack_stick(&report[6], state.axes[0], state.axes[1]);
    pack_stick(&report[9], state.axes[2], state.axes[3]);
    report[12] = 0x09;
    encode_motion(state, report);
    return report;
  }

  bool decode_output(std::span<const uint8_t> report,
                     OutputState& output) const override {
    const bool includes_report_id =
        !report.empty() && (report[0] == 0x01 || report[0] == 0x10);
    const size_t rumble_offset = includes_report_id ? 2 : 1;
    if (report.size() < rumble_offset + 8) return false;
    output = {};
    const auto rumble = report.subspan(rumble_offset, 8);
    const bool active = std::any_of(
        rumble.begin(), rumble.end(), [](uint8_t value) {
          return value != 0;
        });
    if (active) {
      output.low_frequency = 0xFFFF;
      output.high_frequency = 0xFFFF;
    }
    return true;
  }

 private:
  static void encode_motion(const InputState& state,
                            std::vector<uint8_t>& report) {
    const bool has_motion =
        state.acceleration[0] != 0.0f || state.acceleration[1] != 0.0f ||
        state.acceleration[2] != 0.0f || state.angular_velocity[0] != 0.0f ||
        state.angular_velocity[1] != 0.0f || state.angular_velocity[2] != 0.0f;
    const float accel_z =
        has_motion ? state.acceleration[2] : 9.80665f;
    const int16_t accel[] = {
        clamp_i16(std::lround(state.acceleration[0] / 9.80665f * 256.0f)),
        clamp_i16(std::lround(state.acceleration[1] / 9.80665f * 256.0f)),
        clamp_i16(std::lround(accel_z / 9.80665f * 256.0f)),
    };
    constexpr float kRadiansToDegrees = 57.2957795131f;
    const int16_t gyro[] = {
        clamp_i16(std::lround(state.angular_velocity[0] *
                              kRadiansToDegrees * 16.0f)),
        clamp_i16(std::lround(state.angular_velocity[1] *
                              kRadiansToDegrees * 16.0f)),
        clamp_i16(std::lround(state.angular_velocity[2] *
                              kRadiansToDegrees * 16.0f)),
    };
    for (size_t sample = 0; sample < 3; ++sample) {
      const size_t base = 13 + sample * 12;
      append_i16(&report[base + 0], accel[0]);
      append_i16(&report[base + 2], accel[1]);
      append_i16(&report[base + 4], accel[2]);
      append_i16(&report[base + 6], gyro[0]);
      append_i16(&report[base + 8], gyro[1]);
      append_i16(&report[base + 10], gyro[2]);
    }
  }

  HidDeviceProperties properties_{};
  bool has_hat_ = false;
  mutable uint8_t timer_ = 0;
};

}  // namespace

std::unique_ptr<HidProfile> make_switch_pro_profile(
    const DeviceDescription& description) {
  return std::make_unique<SwitchProProfile>(description);
}

}  // namespace vhid
