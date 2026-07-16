#include "vhid/hid_source_decoder.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace vhid {
namespace {

constexpr uint32_t kPageGenericDesktop = 0x01;
constexpr uint32_t kPageButton = 0x09;
constexpr uint32_t kPageSensors = 0x20;
constexpr uint32_t kUsageHatSwitch = 0x39;
constexpr uint32_t kUsageAccelerationAxisX = 0x0453;
constexpr uint32_t kUsageAccelerationAxisY = 0x0454;
constexpr uint32_t kUsageAccelerationAxisZ = 0x0455;
constexpr uint32_t kUsageAngularVelocityX = 0x0457;
constexpr uint32_t kUsageAngularVelocityY = 0x0458;
constexpr uint32_t kUsageAngularVelocityZ = 0x0459;
constexpr double kMetersPerGravity = 9.80665;
constexpr double kRadiansPerDegree = 0.017453292519943295;

enum class FieldKind { button, button_array, hat, axis, motion };

struct Usage {
  uint32_t page = 0;
  uint32_t usage = 0;
};

struct Field {
  uint8_t report_id = 0;
  size_t bit_offset = 0;
  uint8_t bit_size = 0;
  int32_t logical_min = 0;
  int32_t logical_max = 0;
  Usage usage;
  Usage usage_min;
  Usage usage_max;
  FieldKind kind = FieldKind::axis;
  uint8_t index = 0;
  int32_t unit_exponent = 0;
};

struct GlobalState {
  uint32_t usage_page = 0;
  int32_t logical_min = 0;
  int32_t logical_max = 1;
  uint32_t report_size = 0;
  uint8_t report_id = 0;
  uint32_t report_count = 0;
  int32_t unit_exponent = 0;
};

struct LocalState {
  std::vector<Usage> usages;
  std::optional<Usage> usage_min;
  std::optional<Usage> usage_max;

  void clear() {
    usages.clear();
    usage_min.reset();
    usage_max.reset();
  }
};

uint32_t read_unsigned(std::span<const uint8_t> bytes) {
  uint32_t value = 0;
  for (size_t i = 0; i < bytes.size(); ++i)
    value |= static_cast<uint32_t>(bytes[i]) << (8 * i);
  return value;
}

int32_t read_signed(std::span<const uint8_t> bytes) {
  const uint32_t value = read_unsigned(bytes);
  const size_t bits = bytes.size() * 8;
  if (!bits || bits >= 32) return static_cast<int32_t>(value);
  const uint32_t sign = uint32_t{1} << (bits - 1);
  if (!(value & sign)) return static_cast<int32_t>(value);
  const uint32_t mask = std::numeric_limits<uint32_t>::max() << bits;
  return static_cast<int32_t>(value | mask);
}

int32_t read_unit_exponent(std::span<const uint8_t> bytes) {
  if (bytes.empty()) return 0;
  const uint8_t nibble = bytes[0] & 0x0f;
  return nibble < 8 ? nibble : static_cast<int32_t>(nibble) - 16;
}

Usage usage_from_item(uint32_t value, size_t size, uint32_t usage_page) {
  if (size == 4) return Usage{value >> 16, value & 0xffffu};
  return Usage{usage_page, value};
}

Usage usage_for_index(const LocalState& local, const GlobalState& global,
                      uint32_t index) {
  if (index < local.usages.size()) return local.usages[index];
  if (local.usage_min && local.usage_max) {
    const uint32_t usage = local.usage_min->usage + index;
    if (usage <= local.usage_max->usage)
      return Usage{local.usage_min->page, usage};
  }
  return Usage{global.usage_page, 0};
}

bool is_axis_usage(const Usage& usage) {
  if (usage.page != kPageGenericDesktop) return false;
  switch (usage.usage) {
    case 0x30:
    case 0x31:
    case 0x32:
    case 0x33:
    case 0x34:
    case 0x35:
    case 0x36:
    case 0x37:
    case 0x38:
      return true;
    default:
      return false;
  }
}

bool is_acceleration_usage(const Usage& usage, uint8_t& index) {
  if (usage.page != kPageSensors) return false;
  switch (usage.usage) {
    case kUsageAccelerationAxisX:
      index = 0;
      return true;
    case kUsageAccelerationAxisY:
      index = 1;
      return true;
    case kUsageAccelerationAxisZ:
      index = 2;
      return true;
    default:
      return false;
  }
}

bool is_angular_velocity_usage(const Usage& usage, uint8_t& index) {
  if (usage.page != kPageSensors) return false;
  switch (usage.usage) {
    case kUsageAngularVelocityX:
      index = 0;
      return true;
    case kUsageAngularVelocityY:
      index = 1;
      return true;
    case kUsageAngularVelocityZ:
      index = 2;
      return true;
    default:
      return false;
  }
}

uint64_t read_bits(std::span<const uint8_t> data, size_t bit_offset,
                   uint8_t bit_size) {
  uint64_t value = 0;
  for (uint8_t bit = 0; bit < bit_size; ++bit) {
    const size_t source_bit = bit_offset + bit;
    const size_t byte_index = source_bit / 8;
    if (byte_index >= data.size()) break;
    if (data[byte_index] & (uint8_t{1} << (source_bit % 8)))
      value |= uint64_t{1} << bit;
  }
  return value;
}

int64_t signed_value(uint64_t value, uint8_t bit_size) {
  if (!bit_size || bit_size >= 64) return static_cast<int64_t>(value);
  const uint64_t sign = uint64_t{1} << (bit_size - 1);
  if (!(value & sign)) return static_cast<int64_t>(value);
  const uint64_t mask = std::numeric_limits<uint64_t>::max() << bit_size;
  return static_cast<int64_t>(value | mask);
}

int16_t normalize_axis(int64_t value, int32_t logical_min,
                       int32_t logical_max) {
  if (logical_max <= logical_min) return 0;
  const double normalized =
      (static_cast<double>(value) - logical_min) /
      static_cast<double>(logical_max - logical_min);
  if (logical_min >= 0) {
    const long scaled = std::lround(std::clamp(normalized, 0.0, 1.0) *
                                    32767.0);
    return static_cast<int16_t>(std::clamp(scaled, 0l, 32767l));
  }
  const long scaled =
      std::lround(std::clamp(normalized, 0.0, 1.0) * 65535.0 -
                  32768.0);
  return static_cast<int16_t>(std::clamp(scaled, -32768l, 32767l));
}

void copy_text(char* out, size_t out_size, std::string_view value) {
  if (!out_size) return;
  const size_t copy_size = std::min(out_size - 1, value.size());
  std::memset(out, 0, out_size);
  std::memcpy(out, value.data(), copy_size);
}

class GenericHidSourceDecoder final : public HidSourceDecoder {
 public:
  GenericHidSourceDecoder(DeviceDescription description,
                          std::vector<Field> fields)
      : description_(description),
        fields_(std::move(fields)) {
    std::fill(std::begin(state_.hats), std::end(state_.hats), uint8_t{8});
  }

  const DeviceDescription& description() const override {
    return description_;
  }

  bool decode_input(const ParsedHidReport& report,
                    InputState& state) override {
    if (report.header->report_type !=
        static_cast<uint8_t>(HidReportType::input)) {
      return false;
    }
    bool matched = false;
    uint64_t buttons = state_.buttons;
    std::array<bool, kMaxButtons> array_buttons{};
    for (const Field& field : fields_) {
      if (field.report_id != report.header->report_id) continue;
      if (field.bit_size > 63) continue;
      matched = true;
      const uint64_t raw =
          read_bits(report.data, field.bit_offset, field.bit_size);
      switch (field.kind) {
        case FieldKind::button: {
          const uint64_t mask = uint64_t{1} << field.index;
          if (raw)
            buttons |= mask;
          else
            buttons &= ~mask;
          break;
        }
        case FieldKind::button_array:
          if (raw && raw >= field.usage_min.usage &&
              raw <= field.usage_max.usage) {
            const uint32_t button = raw - 1;
            if (button < kMaxButtons) array_buttons[button] = true;
          }
          break;
        case FieldKind::hat: {
          const int64_t value =
              field.logical_min < 0
                  ? signed_value(raw, field.bit_size)
                  : static_cast<int64_t>(raw);
          if (value >= field.logical_min && value <= field.logical_max) {
            state_.hats[field.index] = static_cast<uint8_t>(
                std::clamp<int64_t>(value - field.logical_min,
                                    0, 7));
          } else {
            state_.hats[field.index] = 8;
          }
          break;
        }
        case FieldKind::axis: {
          const int64_t value =
              field.logical_min < 0
                  ? signed_value(raw, field.bit_size)
                  : static_cast<int64_t>(raw);
          state_.axes[field.index] =
              normalize_axis(value, field.logical_min, field.logical_max);
          break;
        }
        case FieldKind::motion: {
          const int64_t value =
              field.logical_min < 0
                  ? signed_value(raw, field.bit_size)
                  : static_cast<int64_t>(raw);
          const float scaled = static_cast<float>(
              static_cast<double>(value) *
              std::pow(10.0, static_cast<double>(field.unit_exponent)));
          if (field.index < 3) {
            state_.acceleration[field.index] =
                scaled * static_cast<float>(kMetersPerGravity);
          } else if (field.index < 6) {
            state_.angular_velocity[field.index - 3] =
                scaled * static_cast<float>(kRadiansPerDegree);
          }
          break;
        }
      }
    }
    if (!matched) return false;
    for (const Field& field : fields_) {
      if (field.report_id != report.header->report_id ||
          field.kind != FieldKind::button_array) {
        continue;
      }
      const uint32_t first =
          std::max<uint32_t>(field.usage_min.usage, uint32_t{1});
      const uint32_t last =
          std::min<uint32_t>(field.usage_max.usage, kMaxButtons);
      for (uint32_t button = first; button <= last; ++button)
        buttons &= ~(uint64_t{1} << (button - 1));
    }
    for (size_t i = 0; i < array_buttons.size(); ++i) {
      if (array_buttons[i])
        buttons |= uint64_t{1} << i;
    }
    state_.buttons = buttons;
    state = state_;
    return true;
  }

 private:
  DeviceDescription description_{};
  std::vector<Field> fields_;
  InputState state_{};
};

class DescriptorBuilder {
 public:
  bool parse(const ParsedHidDeviceAdd& source, std::string& error) {
    description_ = {};
    description_.requested_profile =
        static_cast<uint8_t>(DeviceProfile::generic);
    description_.vendor_id = source.header->vendor_id;
    description_.product_id = source.header->product_id;
    description_.version_number = source.header->version_number;
    copy_text(description_.product, sizeof(description_.product),
              source.product);
    copy_text(description_.manufacturer, sizeof(description_.manufacturer),
              source.manufacturer);
    copy_text(description_.serial, sizeof(description_.serial),
              source.serial);

    GlobalState global;
    LocalState local;
    std::vector<GlobalState> global_stack;
    std::array<size_t, 256> input_offsets{};
    const auto descriptor = source.descriptor;
    for (size_t offset = 0; offset < descriptor.size();) {
      const uint8_t prefix = descriptor[offset++];
      if (prefix == 0xfe) {
        if (offset + 2 > descriptor.size()) {
          error = "truncated long HID descriptor item";
          return false;
        }
        const uint8_t data_size = descriptor[offset++];
        ++offset;
        if (offset + data_size > descriptor.size()) {
          error = "truncated long HID descriptor payload";
          return false;
        }
        offset += data_size;
        continue;
      }
      const uint8_t size_code = prefix & 0x03;
      const size_t data_size = size_code == 3 ? 4 : size_code;
      if (offset + data_size > descriptor.size()) {
        error = "truncated HID descriptor item";
        return false;
      }
      const auto data = descriptor.subspan(offset, data_size);
      offset += data_size;
      const uint8_t type = (prefix >> 2) & 0x03;
      const uint8_t tag = (prefix >> 4) & 0x0f;
      const uint32_t unsigned_value = read_unsigned(data);
      if (type == 0) {
        if (tag == 8) {
          add_input_fields(global, local, unsigned_value,
                           input_offsets[global.report_id]);
        }
        local.clear();
      } else if (type == 1) {
        switch (tag) {
          case 0:
            global.usage_page = unsigned_value;
            break;
          case 1:
            global.logical_min = read_signed(data);
            break;
          case 2:
            global.logical_max = read_signed(data);
            break;
          case 5:
            global.unit_exponent = read_unit_exponent(data);
            break;
          case 7:
            global.report_size = unsigned_value;
            break;
          case 8:
            if (!unsigned_value || unsigned_value > 255) {
              error = "invalid HID report ID";
              return false;
            }
            global.report_id = static_cast<uint8_t>(unsigned_value);
            break;
          case 9:
            global.report_count = unsigned_value;
            break;
          case 10:
            global_stack.push_back(global);
            break;
          case 11:
            if (global_stack.empty()) {
              error = "HID descriptor global stack underflow";
              return false;
            }
            global = global_stack.back();
            global_stack.pop_back();
            break;
          default:
            break;
        }
      } else if (type == 2) {
        switch (tag) {
          case 0:
            local.usages.push_back(usage_from_item(
                unsigned_value, data_size, global.usage_page));
            break;
          case 1:
            local.usage_min = usage_from_item(unsigned_value, data_size,
                                              global.usage_page);
            break;
          case 2:
            local.usage_max = usage_from_item(unsigned_value, data_size,
                                              global.usage_page);
            break;
          default:
            break;
        }
      }
    }
    if (fields_.empty()) {
      error = "HID descriptor did not expose supported input controls";
      return false;
    }
    return true;
  }

  std::unique_ptr<HidSourceDecoder> finish() {
    return std::make_unique<GenericHidSourceDecoder>(description_,
                                                     std::move(fields_));
  }

 private:
  void add_input_fields(const GlobalState& global, const LocalState& local,
                        uint32_t input_flags, size_t& bit_offset) {
    const bool constant = input_flags & 0x01u;
    const bool variable = input_flags & 0x02u;
    if (!global.report_size || !global.report_count) return;
    if (constant) {
      bit_offset += global.report_size * global.report_count;
      return;
    }
    if (!variable) {
      add_array_fields(global, local, bit_offset);
      bit_offset += global.report_size * global.report_count;
      return;
    }
    for (uint32_t i = 0; i < global.report_count; ++i) {
      const Usage usage = usage_for_index(local, global, i);
      add_variable_field(global, usage, bit_offset);
      bit_offset += global.report_size;
    }
  }

  void add_variable_field(const GlobalState& global, Usage usage,
                          size_t bit_offset) {
    if (global.report_size > 63) return;
    if (usage.page == kPageButton && usage.usage >= 1 &&
        usage.usage <= kMaxButtons) {
      fields_.push_back(Field{global.report_id, bit_offset,
                              static_cast<uint8_t>(global.report_size),
                              global.logical_min, global.logical_max, usage,
                              {}, {}, FieldKind::button,
                              static_cast<uint8_t>(usage.usage - 1)});
      description_.button_count = std::max<uint8_t>(
          description_.button_count, static_cast<uint8_t>(usage.usage));
    } else if (usage.page == kPageGenericDesktop &&
               usage.usage == kUsageHatSwitch &&
               description_.hat_count < kMaxHats) {
      fields_.push_back(Field{global.report_id, bit_offset,
                              static_cast<uint8_t>(global.report_size),
                              global.logical_min, global.logical_max, usage,
                              {}, {}, FieldKind::hat,
                              description_.hat_count});
      ++description_.hat_count;
    } else if (is_axis_usage(usage) && description_.axis_count < kMaxAxes) {
      const uint8_t index = description_.axis_count++;
      AxisDescriptor& axis = description_.axes[index];
      axis.usage_page = static_cast<uint16_t>(usage.page);
      axis.usage = static_cast<uint16_t>(usage.usage);
      if (global.logical_min >= 0) {
        axis.logical_min = 0;
        axis.logical_max = INT16_MAX;
        axis.flags = kAxisUnipolar;
      } else {
        axis.logical_min = INT16_MIN;
        axis.logical_max = INT16_MAX;
      }
      fields_.push_back(Field{global.report_id, bit_offset,
                              static_cast<uint8_t>(global.report_size),
                              global.logical_min, global.logical_max, usage,
                              {}, {}, FieldKind::axis, index});
    } else {
      uint8_t motion_index = 0;
      if (is_acceleration_usage(usage, motion_index)) {
        description_.device_flags |= kDeviceHasMotion;
        description_.motion_flags |= kMotionAcceleration;
        fields_.push_back(Field{global.report_id, bit_offset,
                                static_cast<uint8_t>(global.report_size),
                                global.logical_min, global.logical_max, usage,
                                {}, {}, FieldKind::motion, motion_index,
                                global.unit_exponent});
      } else if (is_angular_velocity_usage(usage, motion_index)) {
        description_.device_flags |= kDeviceHasMotion;
        description_.motion_flags |= kMotionAngularVelocity;
        fields_.push_back(Field{global.report_id, bit_offset,
                                static_cast<uint8_t>(global.report_size),
                                global.logical_min, global.logical_max, usage,
                                {}, {}, FieldKind::motion,
                                static_cast<uint8_t>(motion_index + 3),
                                global.unit_exponent});
      }
    }
  }

  void add_array_fields(const GlobalState& global, const LocalState& local,
                        size_t bit_offset) {
    if (global.report_size > 63) return;
    if (!local.usage_min || !local.usage_max ||
        local.usage_min->page != kPageButton ||
        local.usage_max->page != kPageButton) {
      return;
    }
    const uint32_t max_button =
        std::min<uint32_t>(local.usage_max->usage, kMaxButtons);
    if (max_button)
      description_.button_count = std::max<uint8_t>(
          description_.button_count, static_cast<uint8_t>(max_button));
    for (uint32_t i = 0; i < global.report_count; ++i) {
      fields_.push_back(Field{global.report_id,
                              bit_offset + i * global.report_size,
                              static_cast<uint8_t>(global.report_size),
                              global.logical_min, global.logical_max, {},
                              *local.usage_min, *local.usage_max,
                              FieldKind::button_array, 0});
    }
  }

  DeviceDescription description_{};
  std::vector<Field> fields_;
};

}  // namespace

std::unique_ptr<HidSourceDecoder> HidSourceDecoder::create(
    const ParsedHidDeviceAdd& source, std::string& error) {
  DescriptorBuilder builder;
  if (!builder.parse(source, error)) return nullptr;
  return builder.finish();
}

}  // namespace vhid
