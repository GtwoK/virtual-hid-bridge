#ifndef VHID_SOURCE_OUTPUT_CODEC_H
#define VHID_SOURCE_OUTPUT_CODEC_H

#include <memory>
#include <optional>
#include <vector>

#include "vhid/protocol.h"

namespace vhid {

struct SourceOutputReport {
  HidReportType type = HidReportType::output;
  uint8_t report_id = 0;
  std::vector<uint8_t> data;
};

class SourceOutputCodec {
 public:
  virtual ~SourceOutputCodec() = default;

  virtual DeviceProfile profile() const = 0;
  virtual bool accepts_native_reports_from(DeviceProfile output_profile) const {
    return output_profile == profile();
  }
  virtual bool encode(const OutputState& output,
                      SourceOutputReport& report) {
    (void)output;
    (void)report;
    return false;
  }
};

std::optional<DeviceProfile> infer_source_output_profile(
    uint16_t vendor_id, uint16_t product_id);
std::optional<DeviceProfile> announced_source_output_profile(
    const HidDeviceAddHeader& header);
std::unique_ptr<SourceOutputCodec> make_source_output_codec(
    DeviceProfile profile);

}  // namespace vhid

#endif
