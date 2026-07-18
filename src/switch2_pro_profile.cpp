#include "vhid/hid_profile.h"
#include "vhid/haptics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace vhid {
namespace {

constexpr uint8_t kInputReport = 0x05;
constexpr uint8_t kOutputRumble = 0x02;
constexpr uint16_t kSwitch2VendorId = 0x057e;
constexpr uint16_t kSwitch2ProProductId = 0x2069;
constexpr uint16_t kSwitch2Version = 0x0100;
constexpr float kGravity = 9.80665f;
constexpr float kSwitch2GyroRadiansPerSecond = 34.8f;

constexpr uint8_t kSwitch2ProReportDescriptor[] = {
    0x05, 0x01, 0x09, 0x05, 0xA1, 0x01,

    0x85, kInputReport,
    0x06, 0x00, 0xFF, 0x09, 0x01,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x04, 0x81, 0x03,

    0x05, 0x09,
    0x19, 0x01, 0x29, 0x08,
    0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x19, 0x09, 0x29, 0x0F,
    0x75, 0x01, 0x95, 0x07, 0x81, 0x02,
    0x95, 0x01, 0x81, 0x03,
    0x19, 0x10, 0x29, 0x13,
    0x75, 0x01, 0x95, 0x04, 0x81, 0x02,
    0x95, 0x02, 0x81, 0x03,
    0x19, 0x14, 0x29, 0x15,
    0x95, 0x02, 0x81, 0x02,
    0x19, 0x16, 0x29, 0x17,
    0x95, 0x02, 0x81, 0x02,
    0x95, 0x06, 0x81, 0x03,

    0x06, 0x00, 0xFF, 0x09, 0x02,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x02, 0x81, 0x03,

    0x05, 0x01,
    0x09, 0x30, 0x09, 0x31, 0x09, 0x33, 0x09, 0x34,
    0x15, 0x00, 0x26, 0xFF, 0x0F,
    0x75, 0x0C, 0x95, 0x04, 0x81, 0x02,

    0x06, 0x00, 0xFF, 0x09, 0x03,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x20, 0x81, 0x03,

    0x06, 0x20, 0x00,
    0x0A, 0x53, 0x04, 0x0A, 0x55, 0x04, 0x0A, 0x54, 0x04,
    0x0A, 0x57, 0x04, 0x0A, 0x59, 0x04, 0x0A, 0x58, 0x04,
    0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F,
    0x75, 0x10, 0x95, 0x06, 0x81, 0x02,

    0x06, 0x00, 0xFF, 0x09, 0x04,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x03, 0x81, 0x03,

    0x85, kOutputRumble, 0x09, 0x05,
    0x15, 0x00, 0x26, 0xFF, 0x00,
    0x75, 0x08, 0x95, 0x3F, 0x91, 0x02,
    0xC0,
};

std::string bounded_string(const char* text, size_t size,
                           const char* fallback) {
  const size_t length = strnlen(text, size);
  return length ? std::string(text, length) : fallback;
}

bool button_pressed(const InputState& state, uint8_t index) {
  return index < kMaxButtons && (state.buttons & (uint64_t{1} << index));
}

void write_i16(uint8_t* out, int16_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8);
}

void write_u32(uint8_t* out, uint32_t value) {
  out[0] = static_cast<uint8_t>(value);
  out[1] = static_cast<uint8_t>(value >> 8);
  out[2] = static_cast<uint8_t>(value >> 16);
  out[3] = static_cast<uint8_t>(value >> 24);
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
    const double normalized =
        (static_cast<double>(value) + 32768.0) / 65535.0;
    const long scaled = std::lround(normalized * 4095.0);
    return static_cast<uint16_t>(std::clamp(scaled, 0l, 4095l));
  };
  const uint16_t ux = scale(x);
  const uint16_t uy = scale(y);
  out[0] = static_cast<uint8_t>(ux & 0xFF);
  out[1] = static_cast<uint8_t>((ux >> 8) | ((uy & 0x0F) << 4));
  out[2] = static_cast<uint8_t>(uy >> 4);
}

int16_t clamp_i16(long value) {
  return static_cast<int16_t>(std::clamp(value, -32768l, 32767l));
}

int16_t encode_acceleration(float meters_per_second_squared) {
  return clamp_i16(std::lround(
      meters_per_second_squared * INT16_MAX / (kGravity * 8.0f)));
}

int16_t encode_angular_velocity(float radians_per_second) {
  return clamp_i16(std::lround(
      radians_per_second * INT16_MAX / kSwitch2GyroRadiansPerSecond));
}

std::vector<uint8_t> make_switch2_rumble_report(
    const OutputState& output, uint8_t sequence) {
  std::vector<uint8_t> report(64);
  report[0] = kOutputRumble;
  report[1] = static_cast<uint8_t>(0x50 | (sequence & 0x0F));
  if (output_has_haptics(output)) {
    encode_switch2_usb_motor_rumble(output, 0, &report[2]);
    report[0x11] = report[1];
    encode_switch2_usb_motor_rumble(output, 1, &report[0x12]);
  } else {
    encode_switch2_usb_rumble(output, &report[2]);
    std::copy(report.begin() + 1, report.begin() + 7, report.begin() + 0x11);
  }
  return report;
}

bool decode_switch2_rumble_report(std::span<const uint8_t> report,
                                  OutputState& output) {
  std::array<uint8_t, 64> packet{};
  if (report.size() == 63) {
    packet[0] = kOutputRumble;
    std::copy(report.begin(), report.end(), packet.begin() + 1);
    report = std::span<const uint8_t>(packet);
  }
  if (report.size() < 64 || report[0] != kOutputRumble) return false;
  output = {};
  HapticMotorState left{};
  HapticMotorState right{};
  decode_switch2_usb_motor_rumble(report.subspan(2, 5), left);
  decode_switch2_usb_motor_rumble(report.subspan(0x12, 5), right);
  set_haptic_motor(output, 0, left);
  set_haptic_motor(output, 1, right);
  if (output.low_frequency || output.high_frequency) output.duration_ms = 100;
  return true;
}

class Switch2ProProfile final : public HidProfile {
 public:
  explicit Switch2ProProfile(const DeviceDescription& description) {
    has_hat_ = description.hat_count > 0;
    properties_.vendor_id = kSwitch2VendorId;
    properties_.product_id = kSwitch2ProProductId;
    properties_.version_number = kSwitch2Version;
    properties_.primary_usage_page = 0x01;
    properties_.primary_usage = 0x05;
    properties_.vendor_id_source = 1;
    properties_.product = "Nintendo Switch 2 Pro Controller";
    properties_.manufacturer = "Nintendo Co., Ltd.";
    properties_.serial = bounded_string(description.serial,
                                        sizeof(description.serial),
                                        "vhid-switch2-pro-1");
    properties_.transport = "USB";
    properties_.report_descriptor.assign(
        std::begin(kSwitch2ProReportDescriptor),
        std::end(kSwitch2ProReportDescriptor));
    std::fill(std::begin(last_state_.hats), std::end(last_state_.hats),
              uint8_t{8});
  }

  const HidDeviceProperties& properties() const override {
    return properties_;
  }

  std::vector<uint8_t> encode(const InputState& state) const override {
    return encode_input_report(state);
  }

  bool decode_output(std::span<const uint8_t> report,
                     OutputState& output) const override {
    return decode_switch2_rumble_report(report, output);
  }

  bool handle_host_report(HidReportType type, uint8_t report_id,
                          std::span<const uint8_t> report,
                          std::vector<uint8_t>& response) override {
    (void)response;
    return type == HidReportType::output &&
           (report_id == kOutputRumble ||
            (!report.empty() && report[0] == kOutputRumble));
  }

  bool forward_host_report_to_source(
      HidReportType type, uint8_t report_id,
      std::span<const uint8_t> report) const override {
    if (type != HidReportType::output) return true;
    return report_id == kOutputRumble ||
           (!report.empty() && report[0] == kOutputRumble);
  }

  bool get_host_report(HidReportType type, uint8_t report_id,
                       std::span<uint8_t> report,
                       size_t& report_size) override {
    if (type != HidReportType::input || report_id != kInputReport)
      return false;
    const auto source = encode_input_report(last_state_);
    const size_t payload_size = source.size() - 1;
    if (report.size() < payload_size) return false;
    std::copy(source.begin() + 1, source.end(), report.begin());
    report_size = payload_size;
    return true;
  }

 private:
  std::vector<uint8_t> encode_input_report(const InputState& state) const {
    last_state_ = state;
    std::vector<uint8_t> report(64);
    report[0] = kInputReport;

    if (button_pressed(state, 2)) report[5] |= 0x01;
    if (button_pressed(state, 3)) report[5] |= 0x02;
    if (button_pressed(state, 0)) report[5] |= 0x04;
    if (button_pressed(state, 1)) report[5] |= 0x08;
    if (button_pressed(state, 5)) report[5] |= 0x40;
    if (button_pressed(state, 7)) report[5] |= 0x80;

    if (button_pressed(state, 8)) report[6] |= 0x01;
    if (button_pressed(state, 9)) report[6] |= 0x02;
    if (button_pressed(state, 11)) report[6] |= 0x04;
    if (button_pressed(state, 10)) report[6] |= 0x08;
    if (button_pressed(state, 16)) report[6] |= 0x10;
    if (button_pressed(state, 17)) report[6] |= 0x20;
    if (button_pressed(state, 18)) report[6] |= 0x40;

    if (button_pressed(state, 13)) report[7] |= 0x01;
    if (button_pressed(state, 12)) report[7] |= 0x02;
    if (button_pressed(state, 15)) report[7] |= 0x04;
    if (button_pressed(state, 14)) report[7] |= 0x08;
    if (button_pressed(state, 4)) report[7] |= 0x40;
    if (button_pressed(state, 6)) report[7] |= 0x80;
    if (has_hat_ && (report[7] & 0x0F) == 0)
      apply_hat(state.hats[0], report[7]);

    if (button_pressed(state, 19)) report[8] |= 0x01;
    if (button_pressed(state, 20)) report[8] |= 0x02;

    pack_stick(&report[11], state.axes[0], state.axes[1]);
    pack_stick(&report[14], state.axes[2], state.axes[3]);

    sensor_timestamp_ += 4000;
    if (!sensor_timestamp_) sensor_timestamp_ = 4000;
    write_u32(&report[0x2B], sensor_timestamp_);
    write_i16(&report[0x31], encode_acceleration(state.acceleration[0]));
    write_i16(&report[0x33], encode_acceleration(-state.acceleration[2]));
    write_i16(&report[0x35], encode_acceleration(state.acceleration[1]));
    write_i16(&report[0x37], encode_angular_velocity(state.angular_velocity[0]));
    write_i16(&report[0x39], encode_angular_velocity(-state.angular_velocity[2]));
    write_i16(&report[0x3B], encode_angular_velocity(state.angular_velocity[1]));
    return report;
  }
  HidDeviceProperties properties_{};
  mutable InputState last_state_{};
  bool has_hat_ = false;
  mutable uint32_t sensor_timestamp_ = 0;
};

}  // namespace

std::unique_ptr<HidProfile> make_switch2_pro_profile(
    const DeviceDescription& description) {
  return std::make_unique<Switch2ProProfile>(description);
}

std::vector<uint8_t> make_switch2_pro_rumble_report(
    const OutputState& output, uint8_t sequence) {
  return make_switch2_rumble_report(output, sequence);
}

bool decode_switch2_pro_rumble_report(std::span<const uint8_t> report,
                                      OutputState& output) {
  return decode_switch2_rumble_report(report, output);
}

}  // namespace vhid
