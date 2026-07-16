#include "vhid/protocol.h"

#include <cstring>

namespace vhid {

bool host_is_little_endian() {
  const uint16_t value = 1;
  return *reinterpret_cast<const uint8_t*>(&value) == 1;
}

bool valid_device_description(const DeviceDescription& description) {
  if (description.button_count > kMaxButtons ||
      description.hat_count > kMaxHats ||
      description.axis_count > kMaxAxes) {
    return false;
  }
  if (description.requested_profile >
      static_cast<uint8_t>(DeviceProfile::xbox)) {
    return false;
  }
  for (size_t i = 0; i < description.axis_count; ++i) {
    const auto& axis = description.axes[i];
    if (axis.logical_min >= axis.logical_max) return false;
  }
  return std::memchr(description.product, '\0',
                     sizeof(description.product)) != nullptr &&
         std::memchr(description.manufacturer, '\0',
                     sizeof(description.manufacturer)) != nullptr &&
         std::memchr(description.serial, '\0',
                     sizeof(description.serial)) != nullptr;
}

bool parse_message(std::span<const uint8_t> bytes, ParsedMessage& out) {
  out = {};
  if (!host_is_little_endian() || bytes.size() < sizeof(MessageHeader) ||
      bytes.size() > kMaxDatagramSize) {
    return false;
  }
  const auto* header =
      reinterpret_cast<const MessageHeader*>(bytes.data());
  if (header->magic != kWireMagic || header->version != kWireVersion ||
      header->payload_size != bytes.size() - sizeof(MessageHeader)) {
    return false;
  }
  const auto type = static_cast<MessageType>(header->type);
  size_t expected = 0;
  switch (type) {
    case MessageType::session_open:
    case MessageType::session_accept:
      expected = sizeof(SessionPayload);
      break;
    case MessageType::session_close:
    case MessageType::session_ping:
    case MessageType::session_pong:
      expected = 0;
      break;
    case MessageType::hid_device_add:
      expected = SIZE_MAX;
      break;
    case MessageType::hid_device_remove:
      expected = 0;
      break;
    case MessageType::hid_input_report:
    case MessageType::hid_output_report:
    case MessageType::hid_get_report:
    case MessageType::hid_get_report_response:
      expected = SIZE_MAX;
      break;
    case MessageType::semantic_device_add:
      expected = sizeof(DeviceDescription);
      break;
    case MessageType::semantic_input_state:
      expected = sizeof(InputState);
      break;
    default:
      return false;
  }
  if (expected != SIZE_MAX && header->payload_size != expected) return false;
  out.header = header;
  out.payload =
      bytes.subspan(sizeof(MessageHeader), header->payload_size);
  if (type == MessageType::hid_device_add) {
    ParsedHidDeviceAdd device;
    if (!parse_hid_device_add(out.payload, device)) {
      out = {};
      return false;
    }
  } else if (type == MessageType::hid_input_report ||
             type == MessageType::hid_output_report ||
             type == MessageType::hid_get_report ||
             type == MessageType::hid_get_report_response) {
    ParsedHidReport report;
    if (!parse_hid_report(out.payload, report)) {
      out = {};
      return false;
    }
  } else if (type == MessageType::semantic_device_add) {
    DeviceDescription description{};
    std::memcpy(&description, out.payload.data(), sizeof(description));
    if (!valid_device_description(description)) {
      out = {};
      return false;
    }
  }
  return true;
}

bool parse_hid_device_add(std::span<const uint8_t> payload,
                          ParsedHidDeviceAdd& out) {
  out = {};
  if (payload.size() < sizeof(HidDeviceAddHeader)) return false;
  const auto* header =
      reinterpret_cast<const HidDeviceAddHeader*>(payload.data());
  if (header->descriptor_size == 0 ||
      header->transport > static_cast<uint8_t>(HidTransport::network) ||
      (header->source_output_profile != kHidSourceOutputProfileInfer &&
       header->source_output_profile != kHidSourceOutputProfileNone &&
       header->source_output_profile >
           static_cast<uint8_t>(DeviceProfile::xbox))) {
    return false;
  }
  const size_t expected =
      sizeof(*header) + header->descriptor_size + header->product_size +
      header->manufacturer_size + header->serial_size;
  if (payload.size() != expected) return false;
  size_t offset = sizeof(*header);
  out.header = header;
  out.descriptor = payload.subspan(offset, header->descriptor_size);
  offset += header->descriptor_size;
  out.product = std::string_view(
      reinterpret_cast<const char*>(payload.data() + offset),
      header->product_size);
  offset += header->product_size;
  out.manufacturer = std::string_view(
      reinterpret_cast<const char*>(payload.data() + offset),
      header->manufacturer_size);
  offset += header->manufacturer_size;
  out.serial = std::string_view(
      reinterpret_cast<const char*>(payload.data() + offset),
      header->serial_size);
  return true;
}

bool parse_hid_report(std::span<const uint8_t> payload,
                      ParsedHidReport& out) {
  out = {};
  if (payload.size() < sizeof(HidReportHeader)) return false;
  const auto* header =
      reinterpret_cast<const HidReportHeader*>(payload.data());
  if (header->report_type >
          static_cast<uint8_t>(HidReportType::feature) ||
      payload.size() != sizeof(*header) + header->data_size) {
    return false;
  }
  out.header = header;
  out.data = payload.subspan(sizeof(*header), header->data_size);
  return true;
}

namespace {

MessageHeader wire_header(MessageType type, uint32_t device_id,
                          uint32_t sequence, uint64_t timestamp_us,
                          uint32_t payload_size) {
  return MessageHeader{kWireMagic, kWireVersion,
                       static_cast<uint8_t>(type), 0, payload_size,
                       device_id, sequence, timestamp_us};
}

void append(std::vector<uint8_t>& out, const void* data, size_t size) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  out.insert(out.end(), bytes, bytes + size);
}

}  // namespace

std::vector<uint8_t> make_hid_device_add(
    uint32_t device_id, uint32_t sequence, uint64_t timestamp_us,
    const HidDeviceAddHeader& requested,
    std::span<const uint8_t> descriptor, std::string_view product,
    std::string_view manufacturer, std::string_view serial) {
  if (descriptor.empty() || descriptor.size() > UINT16_MAX ||
      product.size() > UINT8_MAX || manufacturer.size() > UINT8_MAX ||
      serial.size() > UINT8_MAX) {
    return {};
  }
  HidDeviceAddHeader device = requested;
  device.descriptor_size = static_cast<uint16_t>(descriptor.size());
  device.product_size = static_cast<uint8_t>(product.size());
  device.manufacturer_size = static_cast<uint8_t>(manufacturer.size());
  device.serial_size = static_cast<uint8_t>(serial.size());
  const uint32_t payload_size =
      static_cast<uint32_t>(sizeof(device) + descriptor.size() +
                            product.size() + manufacturer.size() +
                            serial.size());
  if (sizeof(MessageHeader) + payload_size > kMaxDatagramSize) return {};
  std::vector<uint8_t> out;
  out.reserve(sizeof(MessageHeader) + payload_size);
  const auto header = wire_header(MessageType::hid_device_add, device_id,
                                  sequence, timestamp_us, payload_size);
  append(out, &header, sizeof(header));
  append(out, &device, sizeof(device));
  append(out, descriptor.data(), descriptor.size());
  append(out, product.data(), product.size());
  append(out, manufacturer.data(), manufacturer.size());
  append(out, serial.data(), serial.size());
  return out;
}

std::vector<uint8_t> make_hid_report(
    MessageType type, uint32_t device_id, uint32_t sequence,
    uint64_t timestamp_us, HidReportType report_type, uint8_t report_id,
    std::span<const uint8_t> data) {
  if (type != MessageType::hid_input_report &&
      type != MessageType::hid_output_report &&
      type != MessageType::hid_get_report &&
      type != MessageType::hid_get_report_response) {
    return {};
  }
  if (data.size() > UINT16_MAX ||
      sizeof(MessageHeader) + sizeof(HidReportHeader) + data.size() >
          kMaxDatagramSize) {
    return {};
  }
  const HidReportHeader report{
      static_cast<uint8_t>(report_type), report_id,
      static_cast<uint16_t>(data.size())};
  const uint32_t payload_size =
      static_cast<uint32_t>(sizeof(report) + data.size());
  const auto header =
      wire_header(type, device_id, sequence, timestamp_us, payload_size);
  std::vector<uint8_t> out;
  out.reserve(sizeof(header) + payload_size);
  append(out, &header, sizeof(header));
  append(out, &report, sizeof(report));
  append(out, data.data(), data.size());
  return out;
}

}  // namespace vhid
