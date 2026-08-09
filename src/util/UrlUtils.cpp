#include "UrlUtils.h"

#include <cstdio>
#include <string_view>
#include <vector>

namespace UrlUtils {
namespace {
bool isHexDigit(const char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'); }

bool shouldEncode(const unsigned char c) {
  if (c <= 0x20 || c >= 0x7f) return true;
  switch (c) {
    case '"':
    case '<':
    case '>':
    case '\\':
    case '^':
    case '`':
    case '{':
    case '|':
    case '}':
      return true;
    default:
      return false;
  }
}

std::string normaliseUrlPath(const std::string_view path) {
  const bool absolute = !path.empty() && path.front() == '/';
  const bool trailingSlash = !path.empty() && path.back() == '/';
  std::vector<std::string_view> segments;
  segments.reserve(8);

  size_t start = absolute ? 1 : 0;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const size_t end = slash == std::string_view::npos ? path.size() : slash;
    const std::string_view segment = path.substr(start, end - start);
    if (segment == "..") {
      if (!segments.empty()) segments.pop_back();
    } else if (!segment.empty() && segment != ".") {
      segments.push_back(segment);
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }

  std::string result;
  if (absolute) result.push_back('/');
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i != 0) result.push_back('/');
    result.append(segments[i]);
  }
  if (trailingSlash && !result.empty() && result.back() != '/') result.push_back('/');
  if (absolute && result.empty()) return "/";
  return result;
}

size_t authorityEnd(const std::string& url) {
  const size_t protocolEnd = url.find("://");
  const size_t hostStart = protocolEnd == std::string::npos ? 0 : protocolEnd + 3;
  const size_t end = url.find_first_of("/?#", hostStart);
  return end == std::string::npos ? url.size() : end;
}
}  // namespace

std::string ensureProtocol(const std::string& url) {
  if (url.find("://") == std::string::npos) {
    return "http://" + url;
  }
  return url;
}

std::string extractHost(const std::string& url) { return url.substr(0, authorityEnd(url)); }

std::string encodeUnsafeUrlChars(const std::string& url) {
  std::string out;
  out.reserve(url.size());
  for (size_t i = 0; i < url.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(url[i]);
    if (c == '%' && i + 2 < url.size() && isHexDigit(url[i + 1]) && isHexDigit(url[i + 2])) {
      out += url[i];
      out += url[i + 1];
      out += url[i + 2];
      i += 2;
    } else if (c == '%' || shouldEncode(c)) {
      char encoded[4];
      snprintf(encoded, sizeof(encoded), "%%%02X", c);
      out += encoded;
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

std::string buildUrl(const std::string& serverUrl, const std::string& path) {
  // If path is already an absolute URL (has protocol), use it directly
  if (path.find("://") != std::string::npos) {
    return encodeUnsafeUrlChars(path);
  }
  const std::string urlWithProtocol = ensureProtocol(serverUrl);
  if (path.empty()) {
    return encodeUnsafeUrlChars(urlWithProtocol);
  }
  if (path.rfind("//", 0) == 0) {
    const size_t colon = urlWithProtocol.find(':');
    return encodeUnsafeUrlChars(
        (colon == std::string::npos ? std::string("http:") : urlWithProtocol.substr(0, colon + 1)) + path);
  }
  if (path[0] == '?') {
    const size_t suffix = urlWithProtocol.find_first_of("?#");
    return encodeUnsafeUrlChars(urlWithProtocol.substr(0, suffix) + path);
  }
  if (path[0] == '#') {
    const size_t fragment = urlWithProtocol.find('#');
    return encodeUnsafeUrlChars(urlWithProtocol.substr(0, fragment) + path);
  }

  const size_t referenceSuffix = path.find_first_of("?#");
  const std::string_view referencePath = std::string_view(path).substr(0, referenceSuffix);
  const std::string referenceTail = referenceSuffix == std::string::npos ? std::string() : path.substr(referenceSuffix);
  if (path[0] == '/') {
    // Absolute path - use just the host
    return encodeUnsafeUrlChars(extractHost(urlWithProtocol) + normaliseUrlPath(referencePath) + referenceTail);
  }
  // Relative path - resolve against the directory containing the base resource.
  std::string base = urlWithProtocol;
  const size_t suffixPos = base.find_first_of("?#");
  if (suffixPos != std::string::npos) base.resize(suffixPos);
  const size_t hostEnd = authorityEnd(base);
  const size_t lastSlash = base.rfind('/');
  const std::string directory = lastSlash == std::string::npos || lastSlash < hostEnd ? base.substr(0, hostEnd) + "/"
                                                                                      : base.substr(0, lastSlash + 1);
  const std::string mergedPath = directory.substr(hostEnd) + std::string(referencePath);
  return encodeUnsafeUrlChars(base.substr(0, hostEnd) + normaliseUrlPath(mergedPath) + referenceTail);
}

}  // namespace UrlUtils
