#pragma once

#include <cstdint>

namespace TiltPageTurnPolicy {

// Stored modes 1 and 2 predate the inline three-choice setting.  Keep their
// physical behavior unchanged while presenting the existing CrossVi direction
// as "Reversed" and the opposite direction as the normal choice.
constexpr uint8_t settingOptionForMode(const uint8_t mode) { return mode == 2 ? 1 : mode == 1 ? 2 : 0; }

constexpr uint8_t modeForSettingOption(const uint8_t option) { return option == 1 ? 2 : option == 2 ? 1 : 0; }

}  // namespace TiltPageTurnPolicy
