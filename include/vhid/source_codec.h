#ifndef VHID_SOURCE_CODEC_H
#define VHID_SOURCE_CODEC_H

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "vhid/protocol.h"

namespace vhid {

struct SourceReport {
  HidReportType type = HidReportType::output;
  uint8_t report_id = 0;
  std::vector<uint8_t> data;
};

class SourceInputCodec {
 public:
  virtual ~SourceInputCodec() = default;

  virtual DeviceProfile profile() const = 0;
  virtual const DeviceDescription& description() const = 0;
  virtual bool decode_input(const ParsedHidReport& report,
                            InputState& state) = 0;
};

class SourceOutputCodec {
 public:
  virtual ~SourceOutputCodec() = default;

  virtual DeviceProfile profile() const = 0;
  virtual bool accepts_native_reports_from(DeviceProfile output_profile) const {
    return output_profile == profile();
  }
  virtual bool encode_output(const OutputState& output,
                             SourceReport& report) {
    (void)output;
    (void)report;
    return false;
  }
};

std::unique_ptr<SourceInputCodec> make_source_input_codec(
    const ParsedHidDeviceAdd& source, std::string& error);
std::optional<DeviceProfile> source_input_profile(
    const HidDeviceAddHeader& header);
std::optional<DeviceProfile> source_output_profile(
    const HidDeviceAddHeader& header);
std::unique_ptr<SourceOutputCodec> make_source_output_codec(
    DeviceProfile profile);

}  // namespace vhid

#endif
