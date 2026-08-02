#pragma once

#include <cstddef>
#include <string_view>

namespace HttpTransportPolicy {

constexpr size_t MAX_URL_BYTES = 2048;

bool isSupportedUrl(std::string_view url);
bool isHttpsUrl(std::string_view url);
bool credentialsAllowed(std::string_view url);
bool redirectAllowed(std::string_view from, std::string_view to, bool hasSensitiveHeaders);
bool isGithubReleaseAssetRedirect(std::string_view from, std::string_view to);

}  // namespace HttpTransportPolicy
