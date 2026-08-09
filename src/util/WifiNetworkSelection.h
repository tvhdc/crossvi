#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct WifiNetworkInfo {
  std::string ssid;
  int32_t rssi = 0;
  bool isEncrypted = false;
  bool hasSavedPassword = false;
  bool isHiddenPlaceholder = false;
};

inline void mergeWifiScanResult(std::vector<WifiNetworkInfo>& networks, WifiNetworkInfo candidate) {
  auto existing = std::find_if(networks.begin(), networks.end(),
                               [&](const WifiNetworkInfo& network) { return network.ssid == candidate.ssid; });
  if (existing == networks.end()) {
    networks.push_back(std::move(candidate));
  } else if (candidate.rssi > existing->rssi) {
    *existing = std::move(candidate);
  }
}

inline void sortWifiNetworks(std::vector<WifiNetworkInfo>& networks) {
  std::sort(networks.begin(), networks.end(), [](const WifiNetworkInfo& left, const WifiNetworkInfo& right) {
    if (left.hasSavedPassword != right.hasSavedPassword) return left.hasSavedPassword;
    return left.rssi > right.rssi;
  });
}
