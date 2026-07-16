#include "vhid/source_output_codec.h"

namespace vhid {
namespace {

class NativeSourceOutputCodec final : public SourceOutputCodec {
 public:
  explicit NativeSourceOutputCodec(DeviceProfile profile) : profile_(profile) {}

  DeviceProfile profile() const override { return profile_; }

 private:
  DeviceProfile profile_;
};

bool valid_profile_byte(uint8_t profile) {
  return profile <= static_cast<uint8_t>(DeviceProfile::xbox);
}

}  // namespace

std::optional<DeviceProfile> infer_source_output_profile(
    uint16_t vendor_id, uint16_t product_id) {
  if (vendor_id == 0x057e && product_id == 0x2009)
    return DeviceProfile::switch_pro;
  return std::nullopt;
}

std::optional<DeviceProfile> announced_source_output_profile(
    const HidDeviceAddHeader& header) {
  if (header.source_output_profile == kHidSourceOutputProfileNone)
    return std::nullopt;
  if (header.source_output_profile != kHidSourceOutputProfileInfer &&
      valid_profile_byte(header.source_output_profile)) {
    return static_cast<DeviceProfile>(header.source_output_profile);
  }
  return infer_source_output_profile(header.vendor_id, header.product_id);
}

std::unique_ptr<SourceOutputCodec> make_source_output_codec(
    DeviceProfile profile) {
  switch (profile) {
    case DeviceProfile::switch_pro:
      return std::make_unique<NativeSourceOutputCodec>(profile);
    default:
      return nullptr;
  }
}

}  // namespace vhid
