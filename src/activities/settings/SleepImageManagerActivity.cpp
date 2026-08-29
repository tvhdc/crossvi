#include "SleepImageManagerActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <utility>

#include "CrossPointSettings.h"
#include "activities/ActivityResult.h"
#include "activities/boot_sleep/SleepFrameStore.h"
#include "activities/boot_sleep/SleepImageNormalizer.h"
#include "activities/home/FileBrowserActivity.h"
#include "activities/settings/SleepImagePositionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
SleepImageSelectionStore::ImageTransform legacyTransform() {
  return {SETTINGS.sleepScreenImageZoom, SETTINGS.sleepScreenImageOffsetX, SETTINGS.sleepScreenImageOffsetY};
}

std::string fileName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}
}  // namespace

void SleepImageManagerActivity::onEnter() {
  Activity::onEnter();
  loadCatalog();
  selectedIndex_ = 0;
  requestUpdate();
}

int SleepImageManagerActivity::itemCount() const {
  if (!catalog_.loaded) return 0;
  const int images = static_cast<int>(catalog_.images.size());
  return mode_ == Mode::Manage ? images + 1 : images;
}

void SleepImageManagerActivity::loadCatalog() {
  catalog_ = {};
  if (SleepImageSelectionStore::loadCatalog(catalog_, legacyTransform()) !=
      SleepImageSelectionStore::CatalogStatus::Ok) {
    notice_ = Notice::IoError;
  }
}

void SleepImageManagerActivity::loop() {
  if (optionPopup_.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  const int count = itemCount();
  if (count <= 1) return;
  buttonNavigator_.onNext([this, count] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, count);
    requestUpdate();
  });
  buttonNavigator_.onPrevious([this, count] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, count);
    requestUpdate();
  });
}

void SleepImageManagerActivity::handleSelection() {
  if (!catalog_.loaded) return;
  const int images = static_cast<int>(catalog_.images.size());
  if (mode_ == Mode::Manage && selectedIndex_ == images) {
    openImagePicker();
    return;
  }
  if (selectedIndex_ < 0 || selectedIndex_ >= images) return;

  const auto& image = catalog_.images[static_cast<size_t>(selectedIndex_)];
  if (mode_ == Mode::Manage) {
    confirmDelete(image.id, image.name);
  } else {
    showPlacementActions(image.id, image.name);
  }
}

void SleepImageManagerActivity::openImagePicker() {
  if (!catalog_.loaded) return;
  if (catalog_.images.size() >= SleepImageSelectionStore::MAX_IMAGES) {
    notice_ = Notice::Full;
    requestUpdate();
    return;
  }
  startActivityForResult(
      std::make_unique<FileBrowserActivity>(renderer, mappedInput, "/", FileBrowserActivity::Mode::PickImage),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) addImage(std::get<FilePathResult>(result.data).path);
      });
}

void SleepImageManagerActivity::addImage(const std::string& path) {
  if (!catalog_.loaded) return;
  if (catalog_.images.size() >= SleepImageSelectionStore::MAX_IMAGES) {
    notice_ = Notice::Full;
    requestUpdate();
    return;
  }
  GUI.drawPopup(renderer, tr(STR_SLEEP_IMAGE_PREPARING));
  SleepImageNormalizer::Result prepared;
  {
    GfxRenderer::FrameBufferLoan loan(renderer);
    prepared = SleepImageNormalizer::prepare(path, true, renderer.getDisplayHeight(), renderer.getDisplayWidth());
  }

  if (prepared.status == SleepImageNormalizer::Status::TooLarge) {
    notice_ = Notice::TooLarge;
  } else if (prepared.status == SleepImageNormalizer::Status::Invalid) {
    notice_ = Notice::Invalid;
  } else if (prepared.status != SleepImageNormalizer::Status::Ready) {
    notice_ = Notice::IoError;
  } else {
    SleepImageSelectionStore::ImageEntry added;
    const auto status = SleepImageSelectionStore::addPreparedImage(
        catalog_, prepared.target, SleepImageNormalizer::stagingPath(prepared.target), fileName(path), &added);
    if (status == SleepImageSelectionStore::CatalogStatus::Ok) {
      SleepFrameStore::discard();
      const auto it = std::find_if(catalog_.images.begin(), catalog_.images.end(),
                                   [id = added.id](const auto& image) { return image.id == id; });
      selectedIndex_ = it == catalog_.images.end() ? 0 : static_cast<int>(it - catalog_.images.begin());
      notice_ = Notice::Ready;
    } else {
      notice_ = status == SleepImageSelectionStore::CatalogStatus::Full ? Notice::Full : Notice::IoError;
    }
  }
  requestUpdate();
}

void SleepImageManagerActivity::confirmDelete(const uint16_t id, const std::string& name) {
  startActivityForResult(
      std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE), name, tr(STR_CANCEL),
                                             tr(STR_DELETE)),
      [this, id](const ActivityResult& result) {
        if (result.isCancelled) return;
        if (SleepImageSelectionStore::removeImage(catalog_, id) != SleepImageSelectionStore::CatalogStatus::Ok) {
          notice_ = Notice::IoError;
          return;
        }
        SleepFrameStore::discard();
        selectedIndex_ = std::min(selectedIndex_, std::max(0, itemCount() - 1));
      });
}

void SleepImageManagerActivity::showPlacementActions(const uint16_t id, const std::string& name) {
  const char* actions[] = {tr(STR_SLEEP_IMAGE_ZOOM), tr(STR_SLEEP_IMAGE_POSITION)};
  optionPopup_.show(name.c_str(), actions, 2, 0, [this, id](const int action) { openPositionEditor(id, action == 0); });
  requestUpdate();
}

void SleepImageManagerActivity::openPositionEditor(const uint16_t id, const bool zoom) {
  const auto mode = zoom ? SleepImagePositionActivity::Mode::Zoom : SleepImagePositionActivity::Mode::Position;
  startActivityForResult(std::make_unique<SleepImagePositionActivity>(renderer, mappedInput, mode, id),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) return;
                           const int previous = selectedIndex_;
                           loadCatalog();
                           selectedIndex_ = std::min(previous, std::max(0, itemCount() - 1));
                         });
}

void SleepImageManagerActivity::render(RenderLock&&) {
  if (optionPopup_.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  const StrId title =
      mode_ == Mode::Manage ? StrId::STR_MANAGE_SLEEP_IMAGES : StrId::STR_CUSTOMIZE_SLEEP_IMAGE_POSITION;
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, I18N.get(title));

  int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int count = itemCount();
  if (!catalog_.loaded) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 24, tr(STR_SLEEP_IMAGE_SAVE_FAILED));
  } else if (catalog_.images.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, contentTop + 24, tr(STR_SLEEP_IMAGE_NOT_SELECTED));
    if (mode_ == Mode::Manage) {
      contentTop += 58;
      contentHeight -= 58;
    }
  }

  if (count > 0) {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, count, selectedIndex_,
        [this](const int index) {
          if (index >= 0 && index < static_cast<int>(catalog_.images.size())) {
            const auto& image = catalog_.images[static_cast<size_t>(index)];
            return image.name.empty() ? fileName(image.path) : image.name;
          }
          return std::string(tr(STR_ADD_SLEEP_IMAGE));
        },
        nullptr, nullptr,
        [this](const int index) -> std::string {
          if (mode_ != Mode::Placement || index < 0 || index >= static_cast<int>(catalog_.images.size())) return "";
          char value[16];
          snprintf(value, sizeof(value), tr(STR_PERCENT_VALUE_FORMAT),
                   static_cast<unsigned>(catalog_.images[static_cast<size_t>(index)].transform.zoom));
          return std::string(value);
        });
  }

  const char* select = count > 0 ? tr(STR_SELECT) : "";
  const char* up = count > 1 ? tr(STR_DIR_UP) : "";
  const char* down = count > 1 ? tr(STR_DIR_DOWN) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), select, up, down);
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (notice_ != Notice::None) {
    const Notice notice = std::exchange(notice_, Notice::None);
    switch (notice) {
      case Notice::Ready:
        drawTransientPopup(StrId::STR_SLEEP_IMAGE_READY);
        break;
      case Notice::Full:
        drawTransientPopup(StrId::STR_SLEEP_IMAGE_LIMIT_REACHED);
        break;
      case Notice::TooLarge:
        drawTransientPopup(StrId::STR_SLEEP_IMAGE_TOO_LARGE);
        break;
      case Notice::Invalid:
        drawTransientPopup(StrId::STR_SLEEP_IMAGE_INVALID);
        break;
      case Notice::IoError:
        drawTransientPopup(StrId::STR_SLEEP_IMAGE_SAVE_FAILED);
        break;
      case Notice::None:
        break;
    }
    return;
  }

  renderer.displayBuffer();
}
