#include "HttpTransportPolicy.h"

#include <cctype>
#include <cstdint>
#include <limits>

namespace HttpTransportPolicy {
namespace {

struct Origin {
  std::string_view scheme;
  std::string_view host;
  uint16_t port = 0;
};

bool asciiEqualIgnoreCase(const std::string_view left, const std::string_view right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) != std::tolower(static_cast<unsigned char>(right[i]))) {
      return false;
    }
  }
  return true;
}

bool parsePort(const std::string_view value, uint16_t& port) {
  if (value.empty()) return false;
  uint32_t parsed = 0;
  for (const unsigned char byte : value) {
    if (byte < '0' || byte > '9') return false;
    parsed = parsed * 10U + static_cast<uint32_t>(byte - '0');
    if (parsed > std::numeric_limits<uint16_t>::max()) return false;
  }
  if (parsed == 0) return false;
  port = static_cast<uint16_t>(parsed);
  return true;
}

bool parseOrigin(const std::string_view url, Origin& origin) {
  if (url.empty() || url.size() > MAX_URL_BYTES) return false;
  for (const unsigned char byte : url) {
    if (byte <= 0x20 || byte == 0x7f || byte == '\\') return false;
  }

  const size_t schemeEnd = url.find("://");
  if (schemeEnd == std::string_view::npos) return false;
  origin.scheme = url.substr(0, schemeEnd);
  if (!asciiEqualIgnoreCase(origin.scheme, "http") && !asciiEqualIgnoreCase(origin.scheme, "https")) return false;

  const size_t authorityStart = schemeEnd + 3;
  const size_t authorityEnd = url.find_first_of("/?#", authorityStart);
  const std::string_view authority = url.substr(
      authorityStart, authorityEnd == std::string_view::npos ? std::string_view::npos : authorityEnd - authorityStart);
  if (authority.empty() || authority.find('@') != std::string_view::npos) return false;

  std::string_view portText;
  if (authority.front() == '[') {
    const size_t closing = authority.find(']');
    if (closing == std::string_view::npos || closing == 1) return false;
    origin.host = authority.substr(0, closing + 1);
    if (closing + 1 < authority.size()) {
      if (authority[closing + 1] != ':') return false;
      portText = authority.substr(closing + 2);
    }
  } else {
    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
      if (authority.find(':') != colon) return false;  // IPv6 literals must use brackets.
      origin.host = authority.substr(0, colon);
      portText = authority.substr(colon + 1);
    } else {
      origin.host = authority;
    }
  }
  if (origin.host.empty()) return false;

  if (portText.empty()) {
    origin.port = asciiEqualIgnoreCase(origin.scheme, "https") ? 443 : 80;
  } else if (!parsePort(portText, origin.port)) {
    return false;
  }
  return true;
}

bool isPrivateIpv4(const std::string_view host) {
  uint32_t parts[4] = {};
  size_t start = 0;
  for (size_t i = 0; i < 4; ++i) {
    const size_t end = host.find('.', start);
    const std::string_view token =
        host.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (token.empty() || token.size() > 3) return false;
    uint32_t value = 0;
    for (const unsigned char byte : token) {
      if (byte < '0' || byte > '9') return false;
      value = value * 10U + static_cast<uint32_t>(byte - '0');
    }
    if (value > 255) return false;
    parts[i] = value;
    if (i < 3) {
      if (end == std::string_view::npos) return false;
      start = end + 1;
    } else if (end != std::string_view::npos) {
      return false;
    }
  }
  return parts[0] == 10 || parts[0] == 127 || (parts[0] == 169 && parts[1] == 254) ||
         (parts[0] == 192 && parts[1] == 168) || (parts[0] == 172 && parts[1] >= 16 && parts[1] <= 31);
}

bool isLocalHost(const std::string_view host) {
  if (asciiEqualIgnoreCase(host, "localhost") || host == "[::1]") return true;
  if (host.size() > 6 && asciiEqualIgnoreCase(host.substr(host.size() - 6), ".local")) return true;
  return isPrivateIpv4(host);
}

bool sameOrigin(const Origin& left, const Origin& right) {
  return asciiEqualIgnoreCase(left.scheme, right.scheme) && asciiEqualIgnoreCase(left.host, right.host) &&
         left.port == right.port;
}

}  // namespace

bool isSupportedUrl(const std::string_view url) {
  Origin origin;
  return parseOrigin(url, origin);
}

bool isHttpsUrl(const std::string_view url) {
  Origin origin;
  return parseOrigin(url, origin) && asciiEqualIgnoreCase(origin.scheme, "https");
}

bool credentialsAllowed(const std::string_view url) {
  Origin origin;
  return parseOrigin(url, origin) && (asciiEqualIgnoreCase(origin.scheme, "https") || isLocalHost(origin.host));
}

bool redirectAllowed(const std::string_view from, const std::string_view to, const bool hasSensitiveHeaders) {
  Origin source;
  Origin destination;
  if (!parseOrigin(from, source) || !parseOrigin(to, destination)) return false;
  if (asciiEqualIgnoreCase(source.scheme, "https") && !asciiEqualIgnoreCase(destination.scheme, "https")) return false;
  return !hasSensitiveHeaders || sameOrigin(source, destination);
}

bool isGithubReleaseAssetRedirect(const std::string_view from, const std::string_view to) {
  Origin source;
  Origin destination;
  return parseOrigin(from, source) && parseOrigin(to, destination) && asciiEqualIgnoreCase(source.scheme, "https") &&
         asciiEqualIgnoreCase(source.host, "github.com") && source.port == 443 &&
         asciiEqualIgnoreCase(destination.scheme, "https") &&
         asciiEqualIgnoreCase(destination.host, "release-assets.githubusercontent.com") && destination.port == 443;
}

}  // namespace HttpTransportPolicy
