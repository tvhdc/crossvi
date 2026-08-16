#pragma once

#include <HalStorage.h>

#include <array>
#include <string>
#include <vector>

#include "MappedInputManager.h"
#include "activities/Activity.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                    bool loadingFeedbackAlreadyShown = false);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  bool skipLoopDelay() override { return siblingScanActive || navigationHintsPending; }

 private:
  void beginSiblingImageScan();
  bool stepSiblingImageScan(size_t maxEntries);
  void finishSiblingImageScan();
  void cancelSiblingImageScan();
  void updateNavigationHints();
  void selectSibling(int index);
  void doSetSleepCover();

  std::string filePath;
  std::vector<std::string> siblingImages;
  HalFile siblingDirectory;
  std::array<char, 500> siblingNameBuffer{};
  size_t siblingNameBytes = 0;
  int currentImageIndex = -1;
  bool siblingScanStarted = false;
  bool siblingScanActive = false;
  bool navigationHintsPending = false;
  bool loadingFeedbackAlreadyShown = false;
};
