#pragma once

#include <arpa/inet.h>

#include <format>
#include <string>
#include <string_view>

#include "nat/nat_engine_types.hpp"

namespace nat::helpers {

/**
 * @brief Formats a 32-bit IPv4 address (host byte order) into dotted decimal string.
 */
[[nodiscard]] inline std::string FormatIpv4(std::uint32_t ip) {
  const struct in_addr addr{.s_addr{htonl(ip)}};
  char buf[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &addr, buf, sizeof(buf)) != nullptr) {
    return std::string{buf};
  }
  return "0.0.0.0";
}

/**
 * @brief Converts IP protocol number to standard protocol string.
 */
inline std::string_view FormatProtocol(std::uint8_t proto) noexcept {
  switch (proto) {
    case 6:
      return "TCP";
    case 17:
      return "UDP";
    case 1:
      return "ICMP";
    default:
      return "OTHER";
  }
}

}  // namespace nat::helpers

// Custom std::formatter specializations
template <>
struct std::formatter<nat::engine::FiveTuple> : std::formatter<std::string_view> {
  auto format(const nat::engine::FiveTuple& tuple, std::format_context& ctx) const {
    const std::string text{std::format(
        "{}:{} -> {}:{} [{}]", nat::helpers::FormatIpv4(tuple.src_ip), tuple.src_port,
        nat::helpers::FormatIpv4(tuple.dst_ip), tuple.dst_port,
        nat::helpers::FormatProtocol(tuple.protocol))};
    return std::formatter<std::string_view>::format(text, ctx);
  }
};

template <>
struct std::formatter<nat::engine::NatSession> : std::formatter<std::string_view> {
  auto format(const nat::engine::NatSession& s, std::format_context& ctx) const {
    const std::string text{
        std::format("Inside: [{}] <=> Outside: [{}] (pkts={}, bytes={})", s.inside, s.outside,
                    s.total_packets, s.total_bytes)};
    return std::formatter<std::string_view>::format(text, ctx);
  }
};
