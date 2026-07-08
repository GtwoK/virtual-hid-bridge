#include "vhid/mapping.h"

#include <algorithm>
#include <cmath>

namespace vhid {

ControllerMapping ControllerMapping::identity() {
  ControllerMapping mapping;
  mapping.source_button_for_output.fill(-1);
  mapping.source_hat_for_output.fill(-1);
  for (size_t i = 0; i < kMaxButtons; ++i)
    mapping.source_button_for_output[i] = static_cast<int8_t>(i);
  for (size_t i = 0; i < kMaxHats; ++i)
    mapping.source_hat_for_output[i] = static_cast<int8_t>(i);
  for (size_t i = 0; i < kMaxAxes; ++i)
    mapping.axes[i].source_axis = static_cast<int8_t>(i);
  return mapping;
}

int16_t apply_axis_calibration(int16_t value,
                               const AxisCalibration& calibration) {
  float normalized = 0.0f;
  if (calibration.unipolar) {
    const float range =
        std::max(1.0f, static_cast<float>(calibration.maximum) -
                           calibration.minimum);
    normalized = (static_cast<float>(value) - calibration.minimum) / range;
    normalized = std::clamp(normalized, 0.0f, 1.0f);
  } else if (value >= calibration.center) {
    const float range =
        std::max(1.0f, static_cast<float>(calibration.maximum) -
                           calibration.center);
    normalized = (static_cast<float>(value) - calibration.center) / range;
  } else {
    const float range =
        std::max(1.0f, static_cast<float>(calibration.center) -
                           calibration.minimum);
    normalized = (static_cast<float>(value) - calibration.center) / range;
  }

  if (!calibration.unipolar)
    normalized = std::clamp(normalized, -1.0f, 1.0f);
  const float sign = normalized < 0.0f ? -1.0f : 1.0f;
  float magnitude = std::abs(normalized);
  const float inner = std::clamp(calibration.inner_deadzone, 0.0f, 0.95f);
  const float outer = std::clamp(calibration.outer_deadzone, 0.0f,
                                 0.95f - inner);
  if (magnitude <= inner) {
    magnitude = 0.0f;
  } else {
    magnitude = std::clamp((magnitude - inner) /
                               std::max(0.0001f, 1.0f - inner - outer),
                           0.0f, 1.0f);
  }
  magnitude = std::pow(magnitude, std::max(0.05f, calibration.curve));
  normalized = calibration.unipolar ? magnitude : sign * magnitude;
  if (calibration.invert)
    normalized = calibration.unipolar ? 1.0f - normalized : -normalized;

  const float scale = calibration.unipolar ? 32767.0f : 32767.0f;
  return static_cast<int16_t>(std::clamp(std::lround(normalized * scale),
                                        -32767l, 32767l));
}

InputState apply_mapping(const InputState& source,
                         const ControllerMapping& mapping) {
  InputState output{};
  std::fill(std::begin(output.hats), std::end(output.hats), uint8_t{8});
  for (size_t target = 0; target < kMaxButtons; ++target) {
    const int source_index = mapping.source_button_for_output[target];
    if (source_index >= 0 &&
        (source.buttons & (uint64_t{1} << source_index))) {
      output.buttons |= uint64_t{1} << target;
    }
  }
  for (size_t target = 0; target < kMaxHats; ++target) {
    const int source_index = mapping.source_hat_for_output[target];
    if (source_index >= 0)
      output.hats[target] = source.hats[source_index];
  }
  for (size_t target = 0; target < kMaxAxes; ++target) {
    const auto& route = mapping.axes[target];
    if (route.source_axis >= 0)
      output.axes[target] =
          apply_axis_calibration(source.axes[route.source_axis],
                                 route.calibration);
  }
  std::copy(std::begin(source.acceleration), std::end(source.acceleration),
            std::begin(output.acceleration));
  std::copy(std::begin(source.angular_velocity),
            std::end(source.angular_velocity),
            std::begin(output.angular_velocity));
  std::copy(std::begin(source.orientation), std::end(source.orientation),
            std::begin(output.orientation));
  output.battery_percent = source.battery_percent;
  return output;
}

}  // namespace vhid
