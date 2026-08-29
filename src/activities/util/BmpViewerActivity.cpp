#include "BmpViewerActivity.h"

#include <Bitmap.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>

#include <algorithm>
#include <string_view>

#include "CrossPointSettings.h"
#include "activities/boot_sleep/SleepFrameStore.h"
#include "activities/boot_sleep/SleepImageNormalizer.h"
#include "activities/boot_sleep/SleepImageSelectionStore.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr size_t SIBLING_SCAN_ENTRIES_PER_TICK = 8;
constexpr size_t MAX_SIBLING_IMAGES = 256;
constexpr size_t MAX_SIBLING_NAME_BYTES = 24U * 1024U;
}  // namespace

BmpViewerActivity::BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string path,
                                     const bool loadingFeedbackAlreadyShown)
    : Activity("BmpViewer", renderer, mappedInput),
      filePath(std::move(path)),
      loadingFeedbackAlreadyShown(loadingFeedbackAlreadyShown) {}

void BmpViewerActivity::beginSiblingImageScan() {
  cancelSiblingImageScan();
  siblingScanStarted = true;
  siblingImages.clear();
  siblingNameBytes = 0;
  currentImageIndex = -1;

  if (filePath.empty()) return;

  const std::string dirPath = FsHelpers::extractFolderPath(filePath);
  const size_t lastSlash = filePath.find_last_of('/');
  const std::string fileName = (lastSlash != std::string::npos) ? filePath.substr(lastSlash + 1) : filePath;
  const size_t currentNameBytes = fileName.size() + 1U;
  if (fileName.empty() || currentNameBytes > MAX_SIBLING_NAME_BYTES) return;

  // Keep the already-visible image navigable even when a very large directory
  // reaches a scan bound before its entry would otherwise be encountered.
  siblingImages.push_back(fileName);
  siblingNameBytes = currentNameBytes;

  siblingDirectory = Storage.open(dirPath.c_str());
  if (!siblingDirectory || !siblingDirectory.isDirectory()) {
    finishSiblingImageScan();
    return;
  }
  siblingDirectory.rewindDirectory();
  siblingScanActive = true;
}

bool BmpViewerActivity::stepSiblingImageScan(const size_t maxEntries) {
  if (!siblingScanActive || !siblingDirectory || maxEntries == 0) return false;

  for (size_t scanned = 0; scanned < maxEntries; ++scanned) {
    HalFile file = siblingDirectory.openNextFile();
    if (!file) {
      finishSiblingImageScan();
      return true;
    }

    siblingNameBuffer.front() = '\0';
    siblingNameBuffer.back() = '\0';
    const size_t nameLength = file.getName(siblingNameBuffer.data(), siblingNameBuffer.size());
    const bool isDirectory = file.isDirectory();
    file.close();
    if (isDirectory || nameLength == 0 || nameLength >= siblingNameBuffer.size() || siblingNameBuffer.back() != '\0' ||
        siblingNameBuffer.front() == '.') {
      continue;
    }

    const std::string_view fileName{siblingNameBuffer.data()};
    const size_t currentSlash = filePath.find_last_of('/');
    const std::string_view currentName = currentSlash != std::string::npos
                                             ? std::string_view(filePath).substr(currentSlash + 1)
                                             : std::string_view(filePath);
    if (!FsHelpers::hasBmpExtension(fileName) || fileName == currentName) continue;

    const size_t storedNameBytes = fileName.size() + 1U;
    if (siblingImages.size() >= MAX_SIBLING_IMAGES || storedNameBytes > MAX_SIBLING_NAME_BYTES - siblingNameBytes) {
      finishSiblingImageScan();
      return true;
    }
    std::string candidate(fileName);
    const auto position = std::lower_bound(
        siblingImages.begin(), siblingImages.end(), candidate,
        [](const std::string& left, const std::string& right) { return FsHelpers::naturalLess(left, right); });
    siblingImages.insert(position, std::move(candidate));
    siblingNameBytes += storedNameBytes;
  }
  return false;
}

void BmpViewerActivity::finishSiblingImageScan() {
  if (siblingDirectory) siblingDirectory.close();
  siblingScanActive = false;

  const size_t lastSlash = filePath.find_last_of('/');
  const std::string_view fileName =
      lastSlash != std::string::npos ? std::string_view(filePath).substr(lastSlash + 1) : std::string_view(filePath);
  for (size_t i = 0; i < siblingImages.size(); ++i) {
    if (siblingImages[i] == fileName) {
      currentImageIndex = static_cast<int>(i);
      break;
    }
  }
  navigationHintsPending = siblingImages.size() > 1 && currentImageIndex >= 0;
}

void BmpViewerActivity::cancelSiblingImageScan() {
  if (siblingDirectory) siblingDirectory.close();
  siblingScanActive = false;
}

void BmpViewerActivity::updateNavigationHints() {
  if (!navigationHintsPending) return;
  RenderLock lock(std::try_to_lock);
  if (!lock.ownsLock()) return;
  const bool hasPrevious = currentImageIndex > 0;
  const bool hasNext = currentImageIndex >= 0 && currentImageIndex < static_cast<int>(siblingImages.size()) - 1;
  const auto labels =
      mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), hasPrevious ? "<" : "", hasNext ? ">" : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  navigationHintsPending = false;
}

void BmpViewerActivity::selectSibling(const int index) {
  if (index < 0 || index >= static_cast<int>(siblingImages.size())) return;
  currentImageIndex = index;
  std::string dirPath = FsHelpers::extractFolderPath(filePath);
  if (!dirPath.empty() && dirPath.back() != '/') dirPath += "/";
  filePath = dirPath + siblingImages[static_cast<size_t>(currentImageIndex)];
  onEnter();
}

void BmpViewerActivity::onEnter() {
  Activity::onEnter();

  HalFile file;

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  if (!loadingFeedbackAlreadyShown) GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  loadingFeedbackAlreadyShown = false;
  // 1. Open the file
  if (Storage.openFileForRead("BMP", filePath, file)) {
    Bitmap bitmap(file, true);

    // 2. Parse headers to get dimensions
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      int x, y;

      if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
        float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
        const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

        if (ratio > screenRatio) {
          // Wider than screen
          x = 0;
          y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
        } else {
          // Taller than screen
          x = std::round((static_cast<float>(pageWidth) - static_cast<float>(pageHeight) * ratio) / 2);
          y = 0;
        }
      } else {
        // Center small images
        x = (pageWidth - bitmap.getWidth()) / 2;
        y = (pageHeight - bitmap.getHeight()) / 2;
      }

      // 4. Prepare Rendering
      bool hasPrevious = (siblingImages.size() > 1 && currentImageIndex > 0);
      bool hasNext = (siblingImages.size() > 1 && currentImageIndex != -1 &&
                      currentImageIndex < static_cast<int>(siblingImages.size()) - 1);

      const auto labels =
          mappedInput.mapLabels(tr(STR_BACK), tr(STR_SET_SLEEP_COVER), (hasPrevious ? "<" : ""), (hasNext ? ">" : ""));

      renderer.clearScreen();
      if (renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, 0, 0)) {
        // Draw UI hints on the base layer
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
        // Single pass for non-grayscale images
        renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      } else {
        LOG_ERR("BMP", "Failed to render bitmap: %s", filePath.c_str());
        renderer.clearScreen();
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_PAGE_LOAD_ERROR));
        const auto errorLabels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
        GUI.drawButtonHints(renderer, errorLabels.btn1, errorLabels.btn2, errorLabels.btn3, errorLabels.btn4);
        renderer.displayBuffer(HalDisplay::HALF_REFRESH);
      }

    } else {
      // Handle file parsing error
      renderer.clearScreen();
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_INVALID_BMP_FILE));
      const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }

    file.close();
  } else {
    // Handle file open error
    renderer.clearScreen();
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_FILE_OPEN_FAILED));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer(HalDisplay::HALF_REFRESH);
  }
  activityManager.finishReaderOpenMetric("bmp", static_cast<uint32_t>(millis()));
  if (!siblingScanStarted) beginSiblingImageScan();
}

void BmpViewerActivity::onExit() {
  Activity::onExit();
  cancelSiblingImageScan();
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::HALF_REFRESH);
}

void BmpViewerActivity::doSetSleepCover() {
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));

  SleepImageSelectionStore::Catalog catalog;
  const SleepImageSelectionStore::ImageTransform legacyTransform{
      SETTINGS.sleepScreenImageZoom, SETTINGS.sleepScreenImageOffsetX, SETTINGS.sleepScreenImageOffsetY};
  bool success =
      SleepImageSelectionStore::loadCatalog(catalog, legacyTransform) == SleepImageSelectionStore::CatalogStatus::Ok;
  success = success && catalog.images.size() < SleepImageSelectionStore::MAX_IMAGES;
  SleepImageSelectionStore::ImageEntry added;
  if (success) {
    SleepImageNormalizer::Result prepared;
    {
      GfxRenderer::FrameBufferLoan loan(renderer);
      prepared = SleepImageNormalizer::prepare(filePath, true, renderer.getDisplayHeight(), renderer.getDisplayWidth());
    }
    const size_t slash = filePath.find_last_of('/');
    const std::string name = slash == std::string::npos ? filePath : filePath.substr(slash + 1);
    success = prepared.status == SleepImageNormalizer::Status::Ready &&
              SleepImageSelectionStore::addPreparedImage(catalog, prepared.target,
                                                         SleepImageNormalizer::stagingPath(prepared.target), name,
                                                         &added) == SleepImageSelectionStore::CatalogStatus::Ok;
  }

  if (success) {
    const uint8_t previousMode = SETTINGS.sleepScreen;
    if (previousMode != CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM) {
      SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM;
      if (!SETTINGS.saveToFile()) {
        SETTINGS.sleepScreen = previousMode;
        SleepImageSelectionStore::removeImage(catalog, added.id);
        success = false;
      }
    }
    if (success) SleepFrameStore::discard();
  }
  GUI.drawPopup(renderer, I18N.get(success ? StrId::STR_DONE : StrId::STR_FAILED_LOWER));

  delay(1000);
  onEnter();
}

void BmpViewerActivity::loop() {
  // Keep CPU awake/polling so 1st click works
  Activity::loop();

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    activityManager.goToFileBrowser(filePath);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    doSetSleepCover();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
      mappedInput.wasReleased(MappedInputManager::Button::Up)) {
    if (siblingImages.size() > 1 && currentImageIndex > 0) {
      selectSibling(currentImageIndex - 1);
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Right) ||
      mappedInput.wasReleased(MappedInputManager::Button::Down)) {
    if (siblingImages.size() > 1 && currentImageIndex != -1 &&
        currentImageIndex < static_cast<int>(siblingImages.size()) - 1) {
      selectSibling(currentImageIndex + 1);
    }
    return;
  }

  const bool inputHeld = mappedInput.isPressed(MappedInputManager::Button::Back) ||
                         mappedInput.isPressed(MappedInputManager::Button::Confirm) ||
                         mappedInput.isPressed(MappedInputManager::Button::Left) ||
                         mappedInput.isPressed(MappedInputManager::Button::Right) ||
                         mappedInput.isPressed(MappedInputManager::Button::Up) ||
                         mappedInput.isPressed(MappedInputManager::Button::Down);
  if (!inputHeld && siblingScanActive) stepSiblingImageScan(SIBLING_SCAN_ENTRIES_PER_TICK);
  if (!inputHeld) updateNavigationHints();
}
