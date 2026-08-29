#include "DictionaryHistoryActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "activities/util/ConfirmationActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictionaryHistoryStore.h"

namespace {
constexpr unsigned long ERROR_DURATION_MS = 1500;
void indexYield(void*) { vTaskDelay(1); }

StrId lookupErrorMessage(const Dictionary::LookupResult result) {
  switch (result) {
    case Dictionary::LookupResult::NotFound:
      return StrId::STR_DICT_NOT_FOUND;
    case Dictionary::LookupResult::LowMemory:
      return StrId::STR_DICT_LOW_MEMORY;
    case Dictionary::LookupResult::Decompress:
      return StrId::STR_DICT_DECOMPRESS_ERROR;
    case Dictionary::LookupResult::ReadError:
      return StrId::STR_DICT_READ_FAILED;
    case Dictionary::LookupResult::Found:
    default:
      return StrId::STR_DICT_ERROR;
  }
}
}  // namespace

void DictionaryHistoryActivity::onEnter() {
  Activity::onEnter();
  refreshEntries();
  requestUpdate();
}

void DictionaryHistoryActivity::refreshEntries() {
  const auto& source = DICTIONARY_HISTORY.entries();
  entries_.assign(source.begin(), source.end());
  const int itemCount =
      static_cast<int>(entries_.size()) + (!entries_.empty() && DICTIONARY_HISTORY.isWritable() ? 1 : 0);
  if (itemCount == 0) {
    selected_ = 0;
  } else if (selected_ >= itemCount) {
    selected_ = itemCount - 1;
  }
}

void DictionaryHistoryActivity::lookupSelected() {
  if (selected_ < 0 || selected_ >= static_cast<int>(entries_.size())) return;
  busy_ = true;
  requestUpdateAndWait();

  bool ok = dictionary_.isOpen() || dictionary_.open(SETTINGS.dictionaryName);
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictionary_.needsIndex()) ok = dictionary_.buildIndex(indexYield, nullptr, &indexResult);
  std::string definition;
  std::string headword;
  Dictionary::LookupResult lookupResult = Dictionary::LookupResult::NotFound;
  if (ok) ok = dictionary_.lookup(entries_[selected_].c_str(), definition, headword, &lookupResult);
  busy_ = false;

  if (!ok) {
    if (indexResult == Dictionary::IndexResult::LowMemory) {
      errorMessage_ = StrId::STR_DICT_LOW_MEMORY;
    } else if (indexResult == Dictionary::IndexResult::ReadError) {
      errorMessage_ = StrId::STR_DICT_READ_FAILED;
    } else if (dictionary_.isOpen()) {
      errorMessage_ = lookupErrorMessage(lookupResult);
    } else {
      errorMessage_ = StrId::STR_DICT_ERROR;
    }
    error_ = true;
    errorAt_ = millis();
    requestUpdate();
    return;
  }

  auto definitionActivity = makeUniqueNoThrow<DictionaryDefinitionActivity>(renderer, mappedInput, std::move(headword),
                                                                            std::move(definition));
  if (!definitionActivity) {
    LOG_ERR("DHIST", "OOM allocating DictionaryDefinitionActivity (%u bytes)",
            static_cast<unsigned>(sizeof(DictionaryDefinitionActivity)));
    errorMessage_ = StrId::STR_DICT_LOW_MEMORY;
    error_ = true;
    errorAt_ = millis();
    requestUpdate();
    return;
  }
  DICTIONARY_HISTORY.record(entries_[selected_]);
  refreshEntries();
  startActivityForResult(std::move(definitionActivity), [](const ActivityResult&) {});
}

void DictionaryHistoryActivity::confirmClear() {
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DICT_HISTORY_CLEAR),
                                                                tr(STR_DICT_HISTORY_CLEAR_CONFIRM)),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             if (!DICTIONARY_HISTORY.clear()) {
                               errorMessage_ = StrId::STR_DICT_ERROR;
                               error_ = true;
                               errorAt_ = millis();
                             }
                             refreshEntries();
                           }
                         });
}

void DictionaryHistoryActivity::loop() {
  if (error_) {
    if (millis() - errorAt_ >= ERROR_DURATION_MS) {
      error_ = false;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  const int itemCount =
      static_cast<int>(entries_.size()) + (!entries_.empty() && DICTIONARY_HISTORY.isWritable() ? 1 : 0);
  if (itemCount == 0) return;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selected_ < static_cast<int>(entries_.size())) {
      lookupSelected();
    } else {
      confirmClear();
    }
    return;
  }
  navigator_.onNextRelease([this, itemCount] {
    selected_ = ButtonNavigator::nextIndex(selected_, itemCount);
    requestUpdate();
  });
  navigator_.onPreviousRelease([this, itemCount] {
    selected_ = ButtonNavigator::previousIndex(selected_, itemCount);
    requestUpdate();
  });
  navigator_.onNextContinuous([this, itemCount] {
    selected_ = ButtonNavigator::nextPageIndex(selected_, itemCount, pageItems_);
    requestUpdate();
  });
  navigator_.onPreviousContinuous([this, itemCount] {
    selected_ = ButtonNavigator::previousPageIndex(selected_, itemCount, pageItems_);
    requestUpdate();
  });
}

void DictionaryHistoryActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_DICT_HISTORY));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = height - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
  pageItems_ = std::max(1, contentHeight / 52);
  const int itemCount =
      static_cast<int>(entries_.size()) + (!entries_.empty() && DICTIONARY_HISTORY.isWritable() ? 1 : 0);
  if (itemCount == 0) {
    const char* message =
        DICTIONARY_HISTORY.isWritable() ? tr(STR_DICT_HISTORY_EMPTY) : tr(STR_DICT_HISTORY_UNAVAILABLE);
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + metrics.verticalSpacing, message);
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, width, contentHeight}, itemCount, selected_,
        [this](int index) {
          return index < static_cast<int>(entries_.size()) ? entries_[index] : std::string(tr(STR_DICT_HISTORY_CLEAR));
        },
        [](int) { return std::string{}; });
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), itemCount > 0 ? tr(STR_SELECT) : "",
                                            itemCount > 1 ? tr(STR_DIR_UP) : "", itemCount > 1 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  if (busy_) {
    GUI.drawPopup(renderer, tr(STR_DICT_LOOKING_UP));
  } else if (error_) {
    GUI.drawPopup(renderer, I18N.get(errorMessage_));
  } else {
    renderer.displayBuffer();
  }
}
