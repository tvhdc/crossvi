#pragma once

#include <cstdint>

namespace SleepImageSelectionStore {

constexpr char NORMAL_BMP_PATH[] = "/sleep.bmp";
constexpr char OVERLAY_BMP_PATH[] = "/sleep-overlay.bmp";
constexpr char OVERLAY_PNG_PATH[] = "/sleep-overlay.png";

enum class Target : uint8_t { NormalBmp = 1, OverlayBmp = 2, OverlayPng = 3 };

// Reconcile an interrupted picker publication before either Settings or Sleep
// decides which canonical image is active.
bool recover();

// stagingPath must already be fully written and synced. On success the target
// is validated, published, and any conflicting overlay format is removed.
bool publish(Target target, const char* stagingPath);

}  // namespace SleepImageSelectionStore
