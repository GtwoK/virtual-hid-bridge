#include "vhid/hid_profile.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vhid {

std::unique_ptr<HidProfile> make_switch_pro_profile(
    const DeviceDescription& description);

namespace {

void append_i16(std::vector<uint8_t>& out, int16_t value) {
  out.push_back(static_cast<uint8_t>(value));
  out.push_back(static_cast<uint8_t>(static_cast<uint16_t>(value) >> 8));
}

void append_logical(std::vector<uint8_t>& out, bool minimum, int32_t value) {
  if (value >= INT16_MIN && value <= INT16_MAX) {
    out.push_back(minimum ? 0x16 : 0x26);
    append_i16(out, static_cast<int16_t>(value));
  } else {
    out.push_back(minimum ? 0x17 : 0x27);
    for (int shift = 0; shift < 32; shift += 8)
      out.push_back(static_cast<uint8_t>(
          static_cast<uint32_t>(value) >> shift));
  }
}

std::string bounded_string(const char* text, size_t size,
                           const char* fallback) {
  const size_t length = strnlen(text, size);
  return length ? std::string(text, length) : fallback;
}

class GenericHidProfile final : public HidProfile {
 public:
  explicit GenericHidProfile(const DeviceDescription& description)
      : description_(description) {
    properties_.vendor_id =
        description.vendor_id ? description.vendor_id : 0x1209;
    properties_.product_id =
        description.product_id ? description.product_id : 0x5342;
    properties_.version_number =
        description.version_number ? description.version_number : 1;
    properties_.product = bounded_string(description.product,
                                         sizeof(description.product),
                                         "Virtual HID Gamepad");
    properties_.manufacturer =
        bounded_string(description.manufacturer,
                       sizeof(description.manufacturer), "Virtual HID Bridge");
    properties_.serial =
        bounded_string(description.serial, sizeof(description.serial), "1");
    build_descriptor();
  }

  const HidDeviceProperties& properties() const override {
    return properties_;
  }

  std::vector<uint8_t> encode(const InputState& state) const override {
    std::vector<uint8_t> report;
    report.reserve(64);
    const size_t button_bytes = (description_.button_count + 7) / 8;
    for (size_t i = 0; i < button_bytes; ++i)
      report.push_back(static_cast<uint8_t>(state.buttons >> (8 * i)));
    for (size_t i = 0; i < description_.hat_count; ++i)
      report.push_back(std::min<uint8_t>(state.hats[i], 8));
    for (size_t i = 0; i < description_.axis_count; ++i)
      append_i16(report, state.axes[i]);
    if (description_.device_flags & kDeviceHasBattery)
      report.push_back(state.battery_percent);
    if (description_.motion_flags & kMotionAcceleration) {
      for (float value : state.acceleration) {
        const long scaled = std::lround(value / 9.80665f * 4096.0f);
        append_i16(report, static_cast<int16_t>(
                               std::clamp(scaled, -32768l, 32767l)));
      }
    }
    if (description_.motion_flags & kMotionAngularVelocity) {
      constexpr float kRadiansToDegrees = 57.2957795131f;
      for (float value : state.angular_velocity) {
        const long scaled = std::lround(value * kRadiansToDegrees * 16.0f);
        append_i16(report, static_cast<int16_t>(
                               std::clamp(scaled, -32768l, 32767l)));
      }
    }
    return report;
  }

  bool decode_output(std::span<const uint8_t> report,
                     OutputState& output) const override {
    if (report.size() < 6) return false;
    output = {};
    output.high_frequency = static_cast<uint16_t>(report[0]) * 257u;
    output.low_frequency = static_cast<uint16_t>(report[2]) * 257u;
    output.duration_ms =
        static_cast<uint16_t>(report[4] | (report[5] << 8));
    return true;
  }

 private:
  void build_descriptor() {
    auto& out = properties_.report_descriptor;
    const uint8_t application[] = {
        0x05, 0x01,  // Generic Desktop
        0x09, 0x05,  // Game Pad
        0xA1, 0x01,  // Application collection
    };
    out.insert(out.end(), std::begin(application), std::end(application));

    if (description_.button_count) {
      const uint8_t buttons[] = {
          0x05, 0x09, 0x19, 0x01, 0x29, description_.button_count,
          0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95,
          description_.button_count, 0x81, 0x02,
      };
      out.insert(out.end(), std::begin(buttons), std::end(buttons));
      const uint8_t padding =
          static_cast<uint8_t>((8 - description_.button_count % 8) % 8);
      if (padding) {
        const uint8_t pad[] = {0x75, 0x01, 0x95, padding, 0x81, 0x03};
        out.insert(out.end(), std::begin(pad), std::end(pad));
      }
    }

    for (size_t i = 0; i < description_.hat_count; ++i) {
      const uint8_t hat[] = {
          0x05, 0x01, 0x09, 0x39, 0x15, 0x00, 0x25, 0x07,
          0x35, 0x00, 0x46, 0x3B, 0x01, 0x65, 0x14,
          0x75, 0x08, 0x95, 0x01, 0x81, 0x42,
      };
      out.insert(out.end(), std::begin(hat), std::end(hat));
    }

    for (size_t i = 0; i < description_.axis_count; ++i) {
      const auto& axis = description_.axes[i];
      out.push_back(0x06);
      out.push_back(static_cast<uint8_t>(axis.usage_page));
      out.push_back(static_cast<uint8_t>(axis.usage_page >> 8));
      out.push_back(0x0A);
      out.push_back(static_cast<uint8_t>(axis.usage));
      out.push_back(static_cast<uint8_t>(axis.usage >> 8));
      append_logical(out, true, axis.logical_min);
      append_logical(out, false, axis.logical_max);
      const uint8_t field[] = {0x75, 0x10, 0x95, 0x01, 0x81, 0x02};
      out.insert(out.end(), std::begin(field), std::end(field));
    }

    if (description_.device_flags & kDeviceHasBattery) {
      const uint8_t battery[] = {
          0x05, 0x06, 0x09, 0x20, 0x15, 0x00, 0x26, 0x64,
          0x00, 0x75, 0x08, 0x95, 0x01, 0x81, 0x02,
      };
      out.insert(out.end(), std::begin(battery), std::end(battery));
    }

    const uint8_t motion_fields =
        ((description_.motion_flags & kMotionAcceleration) ? 3 : 0) +
        ((description_.motion_flags & kMotionAngularVelocity) ? 3 : 0);
    if (motion_fields) {
      const uint8_t motion[] = {
          0x06, 0x00, 0xFF, 0x19, 0x01, 0x29, motion_fields,
          0x16, 0x00, 0x80, 0x26, 0xFF, 0x7F,
          0x75, 0x10, 0x95, motion_fields, 0x81, 0x02,
      };
      out.insert(out.end(), std::begin(motion), std::end(motion));
    }

    if (description_.device_flags & kDeviceHasRumble) {
      const uint8_t rumble[] = {
          0x06, 0x00, 0xFF, 0x09, 0x10, 0x15, 0x00, 0x26,
          0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x91, 0x02,
      };
      out.insert(out.end(), std::begin(rumble), std::end(rumble));
    }
    out.push_back(0xC0);
  }

  DeviceDescription description_{};
  HidDeviceProperties properties_{};
};

}  // namespace

std::unique_ptr<HidProfile> make_profile(
    const DeviceDescription& description) {
  const auto requested =
      static_cast<DeviceProfile>(description.requested_profile);
  if (requested == DeviceProfile::generic ||
      requested == DeviceProfile::standard_gamepad) {
    return std::make_unique<GenericHidProfile>(description);
  }
  if (requested == DeviceProfile::switch_pro) {
    return make_switch_pro_profile(description);
  }
  return nullptr;
}

}  // namespace vhid
