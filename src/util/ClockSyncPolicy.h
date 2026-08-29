#pragma once

namespace ClockSyncPolicy {

inline bool shouldSyncFromNetwork(const bool syncedBefore, const bool systemTimeValid) {
  return !syncedBefore || !systemTimeValid;
}

template <typename Settings>
bool markSynced(Settings& settings) {
  const auto previous = settings.clockHasBeenSynced;
  settings.clockHasBeenSynced = 1;
  if (settings.saveToFile()) return true;
  settings.clockHasBeenSynced = previous;
  return false;
}

}  // namespace ClockSyncPolicy
