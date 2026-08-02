#pragma once

#include <cstdint>

#include "activities/home/DashboardStatsPolicy.h"

// Data prepared once by HomeActivity and consumed by themes. Themes must not
// perform SD-card reads while drawing; this keeps render latency predictable.
struct HomeBookSummary {
  DashboardMetricState bookStatsState = DashboardMetricState::NotTracked;
  DashboardMetricState progressState = DashboardMetricState::NotTracked;
  bool hasProgress = false;
  bool progressEstimated = false;
  bool progressBelowOnePercent = false;
  uint8_t progressPercent = 0;
  bool hasStartedReading = false;
  uint32_t bookReadingSeconds = 0;
};

// Compact progress-only data for Home layouts that show more than one recent
// book. It deliberately excludes statistics and strings so loading two extra
// progress files does not repeat the full Dashboard work.
enum class HomeBookAction : uint8_t { Open, Start, Continue, ReadAgain };

inline bool hasReliableHomeBookProgress(const HomeBookSummary& summary) {
  return summary.hasProgress && summary.progressState == DashboardMetricState::Available && !summary.progressEstimated;
}

inline HomeBookAction homeBookAction(const HomeBookSummary& summary) {
  if (hasReliableHomeBookProgress(summary) && summary.progressPercent >= 100) return HomeBookAction::ReadAgain;
  if (summary.hasStartedReading) return HomeBookAction::Continue;
  const bool hasTrustedBookState = (summary.bookStatsState == DashboardMetricState::Available ||
                                    summary.bookStatsState == DashboardMetricState::NoData) &&
                                   (summary.progressState == DashboardMetricState::Available ||
                                    summary.progressState == DashboardMetricState::NoData);
  return hasTrustedBookState ? HomeBookAction::Start : HomeBookAction::Open;
}
