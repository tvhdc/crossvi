#pragma once

#include <memory>

#include "VCodexStatsImporter.h"
#include "activities/Activity.h"

class VCodexStatsImportActivity final : public Activity {
 public:
  VCodexStatsImportActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                            VCodexStatsImporter::ProbeResult probeResult, const VCodexStatsImportSummary& summary,
                            std::unique_ptr<Activity> nextActivity = nullptr);

  static std::unique_ptr<VCodexStatsImportActivity> forManualImport(GfxRenderer& renderer,
                                                                    MappedInputManager& mappedInput);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool handleGlobalShortcut(GlobalShortcut shortcut) override { return handleSafeGlobalShortcut(shortcut); }

 private:
  enum class State : uint8_t { Prompt, Success, Failed, NotAvailable, NotEmpty };

  void continueAfterResult();
  void setImportResult(VCodexStatsImporter::ImportResult result);

  VCodexStatsImportSummary summary_;
  std::unique_ptr<Activity> nextActivity_;
  VCodexStatsImporter::ProbeResult probeResult_;
  State state_ = State::Prompt;
  bool initialInputReleased_ = false;
  uint8_t unlockedAchievements_ = 0;
};
