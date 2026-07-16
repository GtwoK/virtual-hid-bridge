#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "vhid/hid_profile.h"
#include "vhid/hid_source_decoder.h"
#include "vhid/mapping.h"
#include "vhid/physical_hid_source.h"
#include "vhid/protocol.h"

namespace {

volatile std::sig_atomic_t stop_requested = 0;

constexpr uint32_t kBridgePeerId = 1;
constexpr uint64_t kSessionOpenRetryUs = 5'000'000;
constexpr uint64_t kSessionKeepaliveUs = 5'000'000;
constexpr uint64_t kSessionTimeoutUs = 15'000'000;
constexpr uint32_t kFirstNetworkDeviceId = 0x40000000u;

uint64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void request_stop(int) {
  stop_requested = 1;
}

const char* profile_name(vhid::DeviceProfile profile) {
  switch (profile) {
    case vhid::DeviceProfile::generic:
      return "generic";
    case vhid::DeviceProfile::standard_gamepad:
      return "standard-gamepad";
    case vhid::DeviceProfile::switch_pro:
      return "switch-pro";
    case vhid::DeviceProfile::switch_2_pro:
      return "switch-2-pro";
    case vhid::DeviceProfile::dualshock_4:
      return "dualshock-4";
    case vhid::DeviceProfile::dualsense:
      return "dualsense";
    case vhid::DeviceProfile::xbox:
      return "xbox";
  }
  return "unknown";
}

const char* profile_name(uint8_t profile) {
  return profile_name(static_cast<vhid::DeviceProfile>(profile));
}

std::string vid_pid_string(const vhid::HidDeviceProperties& properties) {
  char buffer[10];
  std::snprintf(buffer, sizeof(buffer), "%04x:%04x", properties.vendor_id,
                properties.product_id);
  return buffer;
}

struct Route {
  int socket = -1;
  sockaddr_storage address{};
  socklen_t address_length = 0;
  uint32_t source_device_id = 0;
  bool supports_output = false;
};

struct SessionDevice {
  uint32_t source_id = 0;
  uint32_t bridge_id = 0;
};

struct UdpSession {
  sockaddr_storage address{};
  socklen_t address_length = 0;
  std::string label;
  uint32_t session_id = 0;
  uint64_t last_open_us = 0;
  uint64_t last_ping_us = 0;
  uint64_t last_rx_us = 0;
  uint64_t last_tx_us = 0;
  uint32_t sequence = 0;
  bool enabled = false;
  bool open_sent = false;
  bool active = false;
  std::vector<SessionDevice> devices;
};

struct IdentityOverrides {
  bool profile_set = false;
  bool vendor_id_set = false;
  bool product_id_set = false;
  bool version_number_set = false;
  bool product_set = false;
  bool manufacturer_set = false;
  bool serial_set = false;
  bool transport_set = false;
  vhid::DeviceProfile profile = vhid::DeviceProfile::generic;
  uint16_t vendor_id = 0;
  uint16_t product_id = 0;
  uint16_t version_number = 0;
  std::string product;
  std::string manufacturer;
  std::string serial;
  std::string transport;

  static void copy_string(char* out, size_t out_size,
                          const std::string& value) {
    if (!out_size) return;
    std::memset(out, 0, out_size);
    std::strncpy(out, value.c_str(), out_size - 1);
  }

  void apply(vhid::DeviceDescription& description) const {
    if (profile_set)
      description.requested_profile = static_cast<uint8_t>(profile);
    if (vendor_id_set) description.vendor_id = vendor_id;
    if (product_id_set) description.product_id = product_id;
    if (version_number_set) description.version_number = version_number;
    if (product_set)
      copy_string(description.product, sizeof(description.product), product);
    if (manufacturer_set)
      copy_string(description.manufacturer, sizeof(description.manufacturer),
                  manufacturer);
    if (serial_set)
      copy_string(description.serial, sizeof(description.serial), serial);
  }

  void apply(vhid::HidDeviceProperties& properties) const {
    if (vendor_id_set) properties.vendor_id = vendor_id;
    if (product_id_set) properties.product_id = product_id;
    if (version_number_set) properties.version_number = version_number;
    if (product_set) properties.product = product;
    if (manufacturer_set) properties.manufacturer = manufacturer;
    if (serial_set) properties.serial = serial;
    if (transport_set) properties.transport = transport;
  }
};

struct Controller {
  vhid::ControllerMapping mapping = vhid::ControllerMapping::identity();
  std::shared_ptr<vhid::HidProfile> profile;
  std::unique_ptr<vhid::HidSourceDecoder> source_decoder;
  std::unique_ptr<vhid::VirtualDevice> device;
  uint32_t last_sequence = 0;
  bool have_sequence = false;
  bool raw_hid = false;
};

class Runtime {
 public:
  Runtime(bool dry_run, IdentityOverrides overrides)
      : dry_run_(dry_run), overrides_(std::move(overrides)) {}

  bool add_semantic(uint32_t id,
                    const vhid::DeviceDescription& description,
                    const Route& route = {}) {
    std::lock_guard lock(mutex_);
    vhid::DeviceDescription effective_description = description;
    overrides_.apply(effective_description);
    auto profile_unique = vhid::make_profile(effective_description);
    if (!profile_unique) {
      std::cerr << "device " << id
                << ": requested recognized profile is not implemented yet\n";
      return false;
    }
    auto profile =
        std::shared_ptr<vhid::HidProfile>(std::move(profile_unique));
    vhid::HidDeviceProperties properties = profile->properties();
    overrides_.apply(properties);
    Controller controller;
    controller.profile = profile;
    if (!dry_run_) {
      std::string error;
      const Route output_route = route;
      controller.device = vhid::VirtualDevice::create_raw(
          properties, raw_output_handler(id, output_route),
          get_report_handler(id), error);
      if (!controller.device) {
        std::cerr << "device " << id << ": " << error << '\n';
        return false;
      }
    }
    controllers_[id] = std::move(controller);
    std::cout << "device " << id << " added: "
              << properties.product << " (profile "
              << profile_name(effective_description.requested_profile)
              << ", vid:pid " << vid_pid_string(properties)
              << ", descriptor "
              << properties.report_descriptor.size() << " bytes; "
              << unsigned(effective_description.button_count) << " buttons, "
              << unsigned(effective_description.hat_count) << " hats, "
              << unsigned(effective_description.axis_count) << " axes)\n";
    return true;
  }

  bool add_hid_source(uint32_t id, const vhid::ParsedHidDeviceAdd& source,
                      const Route& route) {
    std::string decode_error;
    auto decoder = vhid::HidSourceDecoder::create(source, decode_error);
    if (!decoder) {
      if (!overrides_.profile_set &&
          (source.header->flags & vhid::kHidAllowTransparentOutput)) {
        std::cerr << "device " << id << ": HID source descriptor cannot be "
                  << "decoded for profile conversion (" << decode_error
                  << "); publishing transparent HID\n";
        return add_raw(id, source, route);
      }
      std::cerr << "device " << id
                << ": HID source descriptor cannot be decoded for profile "
                   "conversion";
      if (!decode_error.empty()) std::cerr << ": " << decode_error;
      std::cerr << '\n';
      return false;
    }

    std::lock_guard lock(mutex_);
    vhid::DeviceDescription effective_description = decoder->description();
    overrides_.apply(effective_description);
    auto profile_unique = vhid::make_profile(effective_description);
    if (!profile_unique) {
      std::cerr << "device " << id
                << ": requested output profile is not implemented yet\n";
      return false;
    }
    auto profile =
        std::shared_ptr<vhid::HidProfile>(std::move(profile_unique));
    vhid::HidDeviceProperties properties = profile->properties();
    overrides_.apply(properties);
    Controller controller;
    controller.profile = profile;
    controller.source_decoder = std::move(decoder);
    if (!dry_run_) {
      std::string error;
      const Route output_route = route;
      controller.device = vhid::VirtualDevice::create_raw(
          properties, raw_output_handler(id, output_route),
          get_report_handler(id), error);
      if (!controller.device) {
        std::cerr << "device " << id << ": " << error << '\n';
        return false;
      }
    }
    controllers_[id] = std::move(controller);
    std::cout << "HID source device " << id << " added: "
              << std::string(source.product) << " -> " << properties.product
              << " (profile "
              << profile_name(effective_description.requested_profile)
              << ", vid:pid " << vid_pid_string(properties)
              << ", source descriptor " << source.descriptor.size()
              << " bytes; " << unsigned(effective_description.button_count)
              << " buttons, "
              << unsigned(effective_description.hat_count) << " hats, "
              << unsigned(effective_description.axis_count) << " axes)\n";
    return true;
  }

  bool add_raw(uint32_t id, const vhid::ParsedHidDeviceAdd& source,
               const Route& route) {
    std::lock_guard lock(mutex_);
    if (overrides_.profile_set) {
      std::cerr
          << "device " << id << ": --output-profile "
          << profile_name(overrides_.profile)
          << " cannot be applied to transparent HID publication; use a "
             "decoded HID source or semantic source for profile conversion\n";
      return false;
    }
    if (!(source.header->flags & vhid::kHidAllowTransparentOutput)) {
      std::cerr
          << "device " << id
          << ": raw HID source needs a selected decoder/output profile; "
             "transparent publication was not requested\n";
      return false;
    }
    vhid::HidDeviceProperties properties;
    properties.vendor_id = source.header->vendor_id;
    properties.product_id = source.header->product_id;
    properties.version_number = source.header->version_number;
    properties.product = std::string(source.product);
    properties.manufacturer = std::string(source.manufacturer);
    properties.serial = std::string(source.serial);
    switch (static_cast<vhid::HidTransport>(source.header->transport)) {
      case vhid::HidTransport::usb: properties.transport = "USB"; break;
      case vhid::HidTransport::bluetooth:
        properties.transport = "Bluetooth";
        break;
      case vhid::HidTransport::bluetooth_le:
        properties.transport = "Bluetooth Low Energy";
        break;
      case vhid::HidTransport::network: properties.transport = "Network"; break;
      default: properties.transport = "Virtual"; break;
    }
    overrides_.apply(properties);
    properties.report_descriptor.assign(source.descriptor.begin(),
                                        source.descriptor.end());
    Controller controller;
    controller.raw_hid = true;
    if (!dry_run_) {
      std::string error;
      controller.device = vhid::VirtualDevice::create_raw(
          properties, raw_output_handler(id, route),
          get_report_handler(id), error);
      if (!controller.device) {
        std::cerr << "device " << id << ": " << error << '\n';
        return false;
      }
    }
    controllers_[id] = std::move(controller);
    std::cout << "raw HID device " << id << " added: "
              << properties.product << " (" << source.descriptor.size()
              << "-byte descriptor; transparent diagnostic mode)\n";
    return true;
  }

  void remove(uint32_t id) {
    std::lock_guard lock(mutex_);
    if (controllers_.erase(id))
      std::cout << "device " << id << " removed\n";
  }

  void input(uint32_t id, uint32_t sequence,
             const vhid::InputState& state) {
    std::lock_guard lock(mutex_);
    auto found = controllers_.find(id);
    if (found == controllers_.end()) return;
    Controller& controller = found->second;
    if (controller.have_sequence &&
        static_cast<int32_t>(sequence - controller.last_sequence) <= 0) {
      return;
    }
    controller.have_sequence = true;
    controller.last_sequence = sequence;
    const auto report =
        controller.profile->encode(
            vhid::apply_mapping(state, controller.mapping));
    if (controller.device && !controller.device->send(report))
      std::cerr << "device " << id << ": failed to dispatch HID report\n";
  }

  void raw_input(uint32_t id, uint32_t sequence,
                 const vhid::ParsedHidReport& source) {
    std::lock_guard lock(mutex_);
    auto found = controllers_.find(id);
    if (found == controllers_.end() ||
        source.header->report_type !=
            static_cast<uint8_t>(vhid::HidReportType::input)) {
      return;
    }
    Controller& controller = found->second;
    if (!controller.source_decoder && !controller.raw_hid) return;
    if (controller.have_sequence &&
        static_cast<int32_t>(sequence - controller.last_sequence) <= 0) {
      return;
    }
    if (controller.source_decoder) {
      vhid::InputState state{};
      if (!controller.source_decoder->decode_input(source, state)) return;
      controller.have_sequence = true;
      controller.last_sequence = sequence;
      const auto report =
          controller.profile->encode(
              vhid::apply_mapping(state, controller.mapping));
      if (controller.device && !controller.device->send(report))
        std::cerr << "device " << id
                  << ": failed to dispatch decoded HID report\n";
      return;
    }
    controller.have_sequence = true;
    controller.last_sequence = sequence;
    std::vector<uint8_t> report;
    report.reserve(source.data.size() + (source.header->report_id ? 1 : 0));
    if (source.header->report_id)
      report.push_back(source.header->report_id);
    report.insert(report.end(), source.data.begin(), source.data.end());
    if (controller.device && !controller.device->send(report))
      std::cerr << "device " << id << ": failed to dispatch raw HID report\n";
  }

 private:
  vhid::VirtualDevice::RawReportHandler raw_output_handler(
      uint32_t id, Route route) {
    return [this, id, route](vhid::HidReportType type, uint8_t report_id,
                            std::span<const uint8_t> data) {
      std::vector<uint8_t> response;
      bool consumed = false;
      {
        std::lock_guard lock(mutex_);
        auto found = controllers_.find(id);
        if (found != controllers_.end() && found->second.profile) {
          consumed = found->second.profile->handle_host_report(
              type, report_id, data, response);
          if (!response.empty() && found->second.device &&
              !found->second.device->send(response)) {
            std::cerr << "device " << id
                      << ": failed to dispatch host response report\n";
          }
        }
      }
      if (consumed) return;
      if (!route.supports_output) {
        std::cerr << "device " << id
                  << ": output has no return path for this source\n";
        return;
      }
      const auto message = vhid::make_hid_report(
          vhid::MessageType::hid_output_report, route.source_device_id,
          output_sequence_.fetch_add(1), now_us(), type, report_id, data);
      if (!message.empty())
        sendto(route.socket, message.data(), message.size(), 0,
               reinterpret_cast<const sockaddr*>(&route.address),
               route.address_length);
    };
  }

  vhid::VirtualDevice::RawGetReportHandler get_report_handler(uint32_t id) {
    return [this, id](vhid::HidReportType type, uint8_t report_id,
                      std::span<uint8_t> report, size_t& report_size) {
      std::lock_guard lock(mutex_);
      auto found = controllers_.find(id);
      if (found == controllers_.end() || !found->second.profile) return false;
      return found->second.profile->get_host_report(type, report_id, report,
                                                    report_size);
    };
  }

  bool dry_run_ = false;
  IdentityOverrides overrides_;
  std::mutex mutex_;
  std::atomic<uint32_t> output_sequence_{0};
  std::unordered_map<uint32_t, Controller> controllers_;
};

int make_listener(const std::string& bind_host, uint16_t port) {
  int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (socket_fd < 0) return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  if (inet_pton(AF_INET, bind_host.c_str(), &address.sin_addr) != 1) {
    close(socket_fd);
    return -1;
  }
  address.sin_port = htons(port);
  if (bind(socket_fd, reinterpret_cast<sockaddr*>(&address),
           sizeof(address)) != 0) {
    close(socket_fd);
    return -1;
  }
  return socket_fd;
}

bool same_endpoint(const sockaddr_storage& a, socklen_t a_length,
                   const sockaddr_storage& b, socklen_t b_length) {
  if (a.ss_family != b.ss_family || a.ss_family != AF_INET ||
      a_length < sizeof(sockaddr_in) || b_length < sizeof(sockaddr_in)) {
    return false;
  }
  const auto* left = reinterpret_cast<const sockaddr_in*>(&a);
  const auto* right = reinterpret_cast<const sockaddr_in*>(&b);
  return left->sin_port == right->sin_port &&
         left->sin_addr.s_addr == right->sin_addr.s_addr;
}

std::string endpoint_label(const sockaddr_storage& address,
                           socklen_t address_length) {
  if (address.ss_family != AF_INET ||
      address_length < sizeof(sockaddr_in)) {
    return "unknown";
  }
  const auto* inet_address = reinterpret_cast<const sockaddr_in*>(&address);
  char host[INET_ADDRSTRLEN]{};
  if (!inet_ntop(AF_INET, &inet_address->sin_addr, host, sizeof(host))) {
    return "unknown";
  }
  return std::string(host) + ":" + std::to_string(ntohs(inet_address->sin_port));
}

UdpSession* find_udp_session(std::vector<UdpSession>& sessions,
                             const sockaddr_storage& address,
                             socklen_t address_length) {
  for (auto& session : sessions) {
    if (same_endpoint(session.address, session.address_length, address,
                      address_length)) {
      return &session;
    }
  }
  return nullptr;
}

SessionDevice* find_session_device(UdpSession& session, uint32_t source_id) {
  for (auto& device : session.devices) {
    if (device.source_id == source_id) return &device;
  }
  return nullptr;
}

uint32_t mapped_device_id(UdpSession& session, uint32_t source_id) {
  if (SessionDevice* device = find_session_device(session, source_id))
    return device->bridge_id;
  return 0;
}

uint32_t reserve_device_id(UdpSession& session, uint32_t source_id,
                           uint32_t& next_network_device_id) {
  if (const uint32_t existing = mapped_device_id(session, source_id))
    return existing;
  uint32_t bridge_id = next_network_device_id++;
  if (bridge_id == 0) bridge_id = next_network_device_id++;
  session.devices.push_back(SessionDevice{source_id, bridge_id});
  return bridge_id;
}

void forget_device_id(UdpSession& session, uint32_t source_id) {
  session.devices.erase(
      std::remove_if(session.devices.begin(), session.devices.end(),
                     [source_id](const SessionDevice& device) {
                       return device.source_id == source_id;
                     }),
      session.devices.end());
}

void remove_session_devices(Runtime& runtime, UdpSession& session) {
  for (const auto& device : session.devices)
    runtime.remove(device.bridge_id);
  session.devices.clear();
}

bool resolve_udp_source(const std::string& spec, uint16_t default_port,
                        UdpSession& out, std::string& error) {
  std::string host = spec;
  uint16_t port = default_port;
  const size_t colon = spec.rfind(':');
  if (colon != std::string::npos && spec.find(':') == colon) {
    host = spec.substr(0, colon);
    const long value = std::strtol(spec.c_str() + colon + 1, nullptr, 10);
    if (value <= 0 || value > 65535) {
      error = "invalid UDP source port";
      return false;
    }
    port = static_cast<uint16_t>(value);
  }
  if (host.empty()) {
    error = "empty UDP source host";
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port);
  const int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
  if (rc != 0 || !result) {
    error = "could not resolve UDP source " + spec;
    return false;
  }
  std::memcpy(&out.address, result->ai_addr, result->ai_addrlen);
  out.address_length = static_cast<socklen_t>(result->ai_addrlen);
  freeaddrinfo(result);
  out.label = host + ":" + service;
  out.session_id =
      static_cast<uint32_t>((now_us() ^ static_cast<uint64_t>(getpid())) &
                            0xffffffffu);
  if (!out.session_id) out.session_id = 1;
  out.enabled = true;
  return true;
}

vhid::SessionPayload make_session_payload(uint32_t session_id) {
  vhid::SessionPayload payload{};
  payload.session_id = session_id;
  payload.peer_id = kBridgePeerId;
  payload.keepalive_interval_us =
      static_cast<uint32_t>(kSessionKeepaliveUs);
  payload.timeout_us = static_cast<uint32_t>(kSessionTimeoutUs);
  std::strncpy(payload.name, "vhid-bridge", sizeof(payload.name) - 1);
  return payload;
}

bool send_message_to(int socket_fd, const sockaddr_storage& address,
                     socklen_t address_length, std::span<const uint8_t> data) {
  const ssize_t sent =
      sendto(socket_fd, data.data(), data.size(), 0,
             reinterpret_cast<const sockaddr*>(&address), address_length);
  return sent == static_cast<ssize_t>(data.size());
}

bool send_empty_message_to(int socket_fd, const sockaddr_storage& address,
                           socklen_t address_length, vhid::MessageType type,
                           uint32_t sequence, uint64_t timestamp_us) {
  vhid::MessageHeader message{
      vhid::kWireMagic, vhid::kWireVersion, static_cast<uint8_t>(type),
      0, 0, 0, sequence, timestamp_us};
  return send_message_to(
      socket_fd, address, address_length,
      std::span<const uint8_t>(
          reinterpret_cast<const uint8_t*>(&message), sizeof(message)));
}

bool send_session_open(int socket_fd, UdpSession& source) {
  const auto payload = make_session_payload(source.session_id);
  const auto message = vhid::make_message(
      vhid::MessageType::session_open, 0, source.sequence++, now_us(),
      payload);
  const bool ok =
      send_message_to(socket_fd, source.address, source.address_length,
                      std::span<const uint8_t>(message.data(),
                                               message.size()));
  const uint64_t timestamp = now_us();
  source.last_open_us = timestamp;
  source.last_tx_us = timestamp;
  source.open_sent = true;
  return ok;
}

bool send_session_accept(int socket_fd, const sockaddr_storage& address,
                         socklen_t address_length, uint32_t session_id,
                         uint32_t sequence) {
  const auto payload = make_session_payload(session_id);
  const auto message =
      vhid::make_message(vhid::MessageType::session_accept, 0, sequence,
                         now_us(), payload);
  return send_message_to(socket_fd, address, address_length,
                         std::span<const uint8_t>(message.data(),
                                                  message.size()));
}

bool send_session_ping(int socket_fd, UdpSession& source) {
  const uint64_t timestamp = now_us();
  const bool ok = send_empty_message_to(
      socket_fd, source.address, source.address_length,
      vhid::MessageType::session_ping, source.sequence++, timestamp);
  source.last_ping_us = timestamp;
  source.last_tx_us = timestamp;
  return ok;
}

bool service_udp_session(int socket_fd, Runtime& runtime, UdpSession& source,
                         bool reopen_on_timeout) {
  if (!source.enabled) return true;
  const uint64_t timestamp = now_us();
  if (!source.open_sent) {
    if (!send_session_open(socket_fd, source)) {
      std::cerr << "warning: UDP session open to " << source.label
                << " failed: " << std::strerror(errno) << '\n';
    }
    return true;
  }
  if (!source.active) {
    if (timestamp - source.last_open_us >= kSessionOpenRetryUs) {
      if (!send_session_open(socket_fd, source)) {
        std::cerr << "warning: UDP session open to " << source.label
                  << " failed: " << std::strerror(errno) << '\n';
      }
    }
    return true;
  }
  if (source.last_rx_us &&
      timestamp - source.last_rx_us >= kSessionTimeoutUs) {
    remove_session_devices(runtime, source);
    std::cerr << "warning: UDP session with " << source.label
              << " timed out";
    if (reopen_on_timeout) {
      source.active = false;
      source.open_sent = false;
      std::cerr << "; reopening\n";
      return true;
    }
    std::cerr << '\n';
    return false;
  }
  const uint64_t last_activity =
      std::max(source.last_rx_us, source.last_tx_us);
  if (last_activity &&
      timestamp - last_activity >= kSessionKeepaliveUs &&
      timestamp - source.last_ping_us >= kSessionKeepaliveUs) {
    if (!send_session_ping(socket_fd, source)) {
      std::cerr << "warning: UDP session ping to " << source.label
                << " failed: " << std::strerror(errno) << '\n';
    }
  }
  return true;
}

void service_inbound_sessions(int socket_fd, Runtime& runtime,
                              std::vector<UdpSession>& sessions) {
  auto session = sessions.begin();
  while (session != sessions.end()) {
    if (service_udp_session(socket_fd, runtime, *session, false)) {
      ++session;
    } else {
      session = sessions.erase(session);
    }
  }
}

bool parse_u16(const char* text, uint16_t& out) {
  char* end = nullptr;
  errno = 0;
  const long value = std::strtol(text, &end, 0);
  if (errno || !end || *end != '\0' || value < 0 || value > 0xffff)
    return false;
  out = static_cast<uint16_t>(value);
  return true;
}

bool parse_transport(const std::string& text, std::string& out) {
  if (text == "virtual" || text == "Virtual") {
    out = "Virtual";
  } else if (text == "usb" || text == "USB") {
    out = "USB";
  } else if (text == "bluetooth" || text == "bt" || text == "Bluetooth") {
    out = "Bluetooth";
  } else if (text == "ble" || text == "bluetooth-le" ||
             text == "bluetooth_le" || text == "Bluetooth Low Energy") {
    out = "Bluetooth Low Energy";
  } else if (text == "network" || text == "Network") {
    out = "Network";
  } else {
    return false;
  }
  return true;
}

bool parse_profile(const std::string& text, vhid::DeviceProfile& out) {
  if (text == "generic") {
    out = vhid::DeviceProfile::generic;
  } else if (text == "standard-gamepad" || text == "standard_gamepad" ||
             text == "gamepad") {
    out = vhid::DeviceProfile::standard_gamepad;
  } else if (text == "switch-pro" || text == "switch1-pro" ||
             text == "switch-1-pro" || text == "switch-pro-controller") {
    out = vhid::DeviceProfile::switch_pro;
  } else {
    return false;
  }
  return true;
}

void consume_udp(int socket_fd, Runtime& runtime, UdpSession* configured_source,
                 std::vector<UdpSession>& inbound_sessions,
                 uint32_t& next_network_device_id) {
  std::array<uint8_t, vhid::kMaxDatagramSize> buffer{};
  sockaddr_storage source{};
  socklen_t source_length = sizeof(source);
  const ssize_t length =
      recvfrom(socket_fd, buffer.data(), buffer.size(), 0,
               reinterpret_cast<sockaddr*>(&source), &source_length);
  if (length <= 0) return;
  vhid::ParsedMessage message;
  if (!vhid::parse_message(
          std::span<const uint8_t>(buffer.data(), length), message)) {
    return;
  }
  const auto type =
      static_cast<vhid::MessageType>(message.header->type);
  const uint64_t received_at = now_us();
  UdpSession* session_source = nullptr;
  bool session_is_configured = false;
  if (configured_source && configured_source->enabled &&
      same_endpoint(configured_source->address,
                    configured_source->address_length,
                    source, source_length)) {
    session_source = configured_source;
    session_source->last_rx_us = received_at;
    session_is_configured = true;
  } else if (configured_source && configured_source->enabled) {
    return;
  } else {
    session_source =
        find_udp_session(inbound_sessions, source, source_length);
    if (session_source) session_source->last_rx_us = received_at;
  }
  switch (type) {
    case vhid::MessageType::session_open: {
      vhid::SessionPayload payload{};
      std::memcpy(&payload, message.payload.data(), sizeof(payload));
      if (!payload.session_id) return;
      if (!session_source) {
        UdpSession session;
        session.address = source;
        session.address_length = source_length;
        session.label = endpoint_label(source, source_length);
        session.enabled = true;
        inbound_sessions.push_back(std::move(session));
        session_source = &inbound_sessions.back();
      }
      const bool was_active = session_source->active;
      const bool changing_session =
          was_active && session_source->session_id != payload.session_id;
      if (changing_session) remove_session_devices(runtime, *session_source);
      if (changing_session || !was_active) session_source->sequence = 0;
      session_source->session_id = payload.session_id;
      session_source->last_rx_us = received_at;
      const uint32_t sequence = session_source->sequence++;
      if (send_session_accept(socket_fd, source, source_length,
                              payload.session_id, sequence)) {
        session_source->active = true;
        session_source->open_sent = true;
        session_source->last_tx_us = now_us();
        if (changing_session || !was_active) {
          std::cout << "UDP session opened by source "
                    << session_source->label << '\n';
        }
      } else {
        std::cerr << "warning: UDP session accept failed: "
                  << std::strerror(errno) << '\n';
      }
      return;
    }
    case vhid::MessageType::session_accept: {
      if (!session_source || !session_is_configured) return;
      vhid::SessionPayload payload{};
      std::memcpy(&payload, message.payload.data(), sizeof(payload));
      if (payload.session_id != session_source->session_id) {
        std::cerr << "warning: UDP session accept from "
                  << session_source->label << " had unexpected session id\n";
        return;
      }
      if (!session_source->active)
        std::cout << "UDP session accepted by source "
                  << session_source->label << '\n';
      session_source->active = true;
      session_source->open_sent = true;
      return;
    }
    case vhid::MessageType::session_close:
      if (session_source) {
        remove_session_devices(runtime, *session_source);
        std::cout << "UDP session closed by source "
                  << session_source->label << '\n';
        if (session_is_configured) {
          session_source->active = false;
          session_source->open_sent = false;
        } else {
          inbound_sessions.erase(
              std::remove_if(inbound_sessions.begin(), inbound_sessions.end(),
                             [&source, source_length](
                                 const UdpSession& session) {
                               return same_endpoint(
                                   session.address, session.address_length,
                                   source, source_length);
                             }),
              inbound_sessions.end());
        }
      }
      return;
    case vhid::MessageType::session_ping: {
      if (!session_source || !session_source->active) return;
      const uint64_t timestamp = now_us();
      const uint32_t sequence = session_source->sequence++;
      if (send_empty_message_to(socket_fd, source, source_length,
                                vhid::MessageType::session_pong, sequence,
                                timestamp)) {
        session_source->last_tx_us = timestamp;
      }
      return;
    }
    case vhid::MessageType::session_pong:
      return;
    case vhid::MessageType::hid_device_add: {
      if (!session_source || !session_source->active ||
          !message.header->device_id) {
        return;
      }
      vhid::ParsedHidDeviceAdd source_device;
      if (!vhid::parse_hid_device_add(message.payload, source_device))
        return;
      const uint32_t source_id = message.header->device_id;
      const bool known_device = mapped_device_id(*session_source, source_id);
      const uint32_t bridge_id = reserve_device_id(
          *session_source, source_id, next_network_device_id);
      Route route{socket_fd, source, source_length, source_id, true};
      if (!runtime.add_hid_source(bridge_id, source_device, route) &&
          !known_device)
        forget_device_id(*session_source, source_id);
      return;
    }
    case vhid::MessageType::hid_device_remove: {
      if (!session_source || !session_source->active) return;
      const uint32_t source_id = message.header->device_id;
      const uint32_t bridge_id = mapped_device_id(*session_source, source_id);
      if (bridge_id) {
        runtime.remove(bridge_id);
        forget_device_id(*session_source, source_id);
      }
      return;
    }
    case vhid::MessageType::hid_input_report: {
      if (!session_source || !session_source->active) return;
      const uint32_t bridge_id =
          mapped_device_id(*session_source, message.header->device_id);
      if (!bridge_id) return;
      vhid::ParsedHidReport report;
      if (vhid::parse_hid_report(message.payload, report))
        runtime.raw_input(bridge_id, message.header->sequence, report);
      return;
    }
    case vhid::MessageType::hid_output_report:
    case vhid::MessageType::hid_get_report:
    case vhid::MessageType::hid_get_report_response:
      return;
    case vhid::MessageType::semantic_device_add: {
      if (!session_source || !session_source->active ||
          !message.header->device_id) {
        return;
      }
      vhid::DeviceDescription description{};
      std::memcpy(&description, message.payload.data(), sizeof(description));
      const uint32_t source_id = message.header->device_id;
      const bool known_device = mapped_device_id(*session_source, source_id);
      const uint32_t bridge_id = reserve_device_id(
          *session_source, source_id, next_network_device_id);
      Route route{socket_fd, source, source_length, source_id, true};
      if (!runtime.add_semantic(bridge_id, description, route) && !known_device)
        forget_device_id(*session_source, source_id);
      return;
    }
    case vhid::MessageType::semantic_input_state: {
      if (!session_source || !session_source->active) return;
      const uint32_t bridge_id =
          mapped_device_id(*session_source, message.header->device_id);
      if (!bridge_id) return;
      vhid::InputState state{};
      std::memcpy(&state, message.payload.data(), sizeof(state));
      runtime.input(bridge_id, message.header->sequence, state);
      return;
    }
  }
}

void usage(const char* name) {
  std::cerr << "usage: " << name
            << " [--bind ADDRESS] [--listen-port PORT]"
               " [--udp-source HOST[:PORT]]"
               " [--no-physical] [--seize-physical] [--dry-run]\n"
               "       [--override-vendor-id N] [--override-product-id N]\n"
               "       [--override-version N] [--override-product NAME]\n"
               "       [--override-manufacturer NAME] [--override-serial TEXT]\n"
               "       [--override-transport virtual|usb|bluetooth|ble|network]\n"
               "       [--output-profile generic|standard-gamepad|switch-pro]\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::cout.setf(std::ios::unitbuf);
  std::cerr.setf(std::ios::unitbuf);
  std::signal(SIGINT, request_stop);
  std::signal(SIGTERM, request_stop);
  std::string bind_host = "127.0.0.1";
  uint16_t listen_port = vhid::kDefaultUdpPort;
  std::string udp_source_spec;
  bool bind_explicit = false;
  bool physical = true;
  bool seize = false;
  bool dry_run = false;
  IdentityOverrides identity_overrides;
  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    if (argument == "--help" || argument == "-h") {
      usage(argv[0]);
      return 0;
    } else if (argument == "--bind" && i + 1 < argc) {
      bind_host = argv[++i];
      bind_explicit = true;
    } else if (argument == "--listen-port" && i + 1 < argc) {
      const long value = std::strtol(argv[++i], nullptr, 10);
      if (value <= 0 || value > 65535) {
        usage(argv[0]);
        return 2;
      }
      listen_port = static_cast<uint16_t>(value);
    } else if (argument == "--udp-source" && i + 1 < argc) {
      udp_source_spec = argv[++i];
    } else if (argument == "--no-physical") {
      physical = false;
    } else if (argument == "--seize-physical") {
      seize = true;
    } else if (argument == "--dry-run") {
      dry_run = true;
    } else if (argument == "--override-vendor-id" && i + 1 < argc) {
      if (!parse_u16(argv[++i], identity_overrides.vendor_id)) {
        usage(argv[0]);
        return 2;
      }
      identity_overrides.vendor_id_set = true;
    } else if (argument == "--override-product-id" && i + 1 < argc) {
      if (!parse_u16(argv[++i], identity_overrides.product_id)) {
        usage(argv[0]);
        return 2;
      }
      identity_overrides.product_id_set = true;
    } else if (argument == "--override-version" && i + 1 < argc) {
      if (!parse_u16(argv[++i], identity_overrides.version_number)) {
        usage(argv[0]);
        return 2;
      }
      identity_overrides.version_number_set = true;
    } else if (argument == "--override-product" && i + 1 < argc) {
      identity_overrides.product = argv[++i];
      identity_overrides.product_set = true;
    } else if (argument == "--override-manufacturer" && i + 1 < argc) {
      identity_overrides.manufacturer = argv[++i];
      identity_overrides.manufacturer_set = true;
    } else if (argument == "--override-serial" && i + 1 < argc) {
      identity_overrides.serial = argv[++i];
      identity_overrides.serial_set = true;
    } else if (argument == "--override-transport" && i + 1 < argc) {
      if (!parse_transport(argv[++i], identity_overrides.transport)) {
        usage(argv[0]);
        return 2;
      }
      identity_overrides.transport_set = true;
    } else if (argument == "--output-profile" && i + 1 < argc) {
      if (!parse_profile(argv[++i], identity_overrides.profile)) {
        usage(argv[0]);
        return 2;
      }
      identity_overrides.profile_set = true;
    } else {
      usage(argv[0]);
      return 2;
    }
  }

  if (!udp_source_spec.empty() && !bind_explicit) {
    bind_host = "0.0.0.0";
  }

  const int udp_socket = make_listener(bind_host, listen_port);
  if (udp_socket < 0) {
    std::cerr << "could not bind VHID UDP " << bind_host << ':'
              << listen_port << '\n';
    return 1;
  }
  Runtime runtime(dry_run, std::move(identity_overrides));
  UdpSession udp_source;
  std::vector<UdpSession> inbound_sessions;
  uint32_t next_network_device_id = kFirstNetworkDeviceId;
  if (!udp_source_spec.empty()) {
    std::string error;
    if (!resolve_udp_source(udp_source_spec, listen_port, udp_source, error)) {
      std::cerr << error << '\n';
      close(udp_socket);
      return 2;
    }
    service_udp_session(udp_socket, runtime, udp_source, true);
  }
  std::unique_ptr<vhid::PhysicalHidSource> physical_source;
  if (physical) {
    std::string error;
    physical_source = vhid::PhysicalHidSource::create(
        [&runtime](uint32_t id, const vhid::DeviceDescription& description) {
          runtime.add_semantic(id, description);
        },
        [&runtime](uint32_t id, uint32_t sequence,
                   const vhid::InputState& state) {
          runtime.input(id, sequence, state);
        },
        [&runtime](uint32_t id) { runtime.remove(id); }, seize, error);
    if (!physical_source)
      std::cerr << "physical HID input unavailable: " << error << '\n';
  }

  std::cout << "VHID UDP listening on " << bind_host << ':' << listen_port
            << (dry_run ? " (dry run)" : "") << '\n';
  if (udp_source.enabled) {
    std::cout << "opening UDP session with source " << udp_source.label
              << " (idle keepalive "
              << (kSessionKeepaliveUs / 1'000'000) << "s, timeout "
              << (kSessionTimeoutUs / 1'000'000) << "s)";
    if (!bind_explicit) std::cout << " (auto-bound to 0.0.0.0)";
    std::cout << '\n';
  }
  if (isatty(STDIN_FILENO)) {
    std::cout << "press q then Enter, Ctrl-C, or SIGTERM to quit\n";
  }
  while (!stop_requested) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(udp_socket, &read_set);
    int top = udp_socket;
    const bool read_stdin = isatty(STDIN_FILENO);
    if (read_stdin) {
      FD_SET(STDIN_FILENO, &read_set);
      top = std::max(top, STDIN_FILENO);
    }
    timeval timeout{0, 100000};
    if (select(top + 1, &read_set, nullptr, nullptr, &timeout) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (FD_ISSET(udp_socket, &read_set))
      consume_udp(udp_socket, runtime,
                  udp_source.enabled ? &udp_source : nullptr,
                  inbound_sessions, next_network_device_id);
    if (read_stdin && FD_ISSET(STDIN_FILENO, &read_set)) {
      char input[64];
      const ssize_t n = read(STDIN_FILENO, input, sizeof(input));
      for (ssize_t i = 0; i < n; ++i) {
        if (input[i] == 'q' || input[i] == 'Q') stop_requested = 1;
      }
    }
    service_udp_session(udp_socket, runtime, udp_source, true);
    service_inbound_sessions(udp_socket, runtime, inbound_sessions);
  }
  physical_source.reset();
  close(udp_socket);
  return 0;
}
