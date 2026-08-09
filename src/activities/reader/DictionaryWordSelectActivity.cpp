#include "DictionaryWordSelectActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Memory.h>
#include <Utf8.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <climits>
#include <cstdlib>

#include "CrossPointSettings.h"
#include "DictionaryDefinitionActivity.h"
#include "SdCardFontSystem.h"
#include "components/UITheme.h"
#include "util/DictionaryHistoryStore.h"
#include "util/DictionaryQuery.h"

namespace {

constexpr unsigned long POPUP_DURATION_MS = 1500;

void indexBuildYield(void*) { vTaskDelay(1); }

StrId lookupErrorMessage(const Dictionary::LookupResult result) {
  switch (result) {
    case Dictionary::LookupResult::LowMemory:
      return StrId::STR_DICT_LOW_MEMORY;
    case Dictionary::LookupResult::Decompress:
      return StrId::STR_DICT_DECOMPRESS_ERROR;
    case Dictionary::LookupResult::ReadError:
      return StrId::STR_DICT_READ_FAILED;
    case Dictionary::LookupResult::Found:
    case Dictionary::LookupResult::NotFound:
    default:
      return StrId::STR_DICT_NOT_FOUND;
  }
}

StrId indexErrorMessage(const Dictionary::IndexResult result) {
  return result == Dictionary::IndexResult::LowMemory ? StrId::STR_DICT_LOW_MEMORY : StrId::STR_DICT_READ_FAILED;
}

}  // namespace

void DictionaryWordSelectActivity::onEnter() {
  Activity::onEnter();
  // This activity is the narrow exception to the UI-font-only rule: it must
  // reproduce the active reading page exactly while selecting a word.
  sdFontSystem.ensureLoaded(renderer, false);
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  // No null check: a failed allocation just disables the differential
  // fast path (drawHighlightWithSnapshot skips the read), keeping the
  // full-repaint path as the fallback.
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
  extractWords();
  // Start on the middle row's word nearest mid-screen instead of top-left:
  // any word on the page is then at most half a page of moves away.
  if (!words.empty()) {
    const int initial = closestInRow(rowCount / 2, renderer.getScreenWidth() / 2);
    if (initial >= 0) selected = initial;
  }
  requestUpdate();
}

void DictionaryWordSelectActivity::onExit() {
  sdFontSystem.releaseLoadedFont(renderer);
  Activity::onExit();
}

void DictionaryWordSelectActivity::onPause() {
  // Definition/history screens render only UI/dictionary text and benefit
  // from the same contiguous-heap recovery as the rest of the reader menu.
  sdFontSystem.releaseLoadedFont(renderer);
}

void DictionaryWordSelectActivity::onResume() {
  sdFontSystem.ensureLoaded(renderer, false);
  fontId = SETTINGS.getReaderFontId();
  lineHeight = renderer.getLineHeight(fontId);
  const int oldSelection = selected;
  extractWords();
  selected = words.empty() ? 0 : std::clamp(oldSelection, 0, static_cast<int>(words.size()) - 1);
  snapshotIdx = -1;
}

void DictionaryWordSelectActivity::extractWords() {
  words.clear();
  words.reserve(MAX_VISIBLE_WORDS);
  rowCount = 0;

  // Single walk: collect the selectable words while accumulating their text
  // and styles (~2KB transient string, freed on return). Widths are measured
  // afterwards: merging the page's codepoints into the SD font's persistent
  // advance table first keeps getTextAdvanceX on the in-RAM path instead of
  // loading glyphs from SD one overflow slot at a time.
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;

  for (const auto& element : page->elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto& block = line->getBlock();
    if (!block || !block->valid()) continue;

    bool rowHasWords = false;
    for (uint16_t i = 0; i < block->wordCount(); i++) {
      if (words.size() >= MAX_VISIBLE_WORDS) break;
      const char* text = block->wordText(i);
      if (!utf8ContainsLookupCharacter(text)) continue;

      WordBox box;
      box.x = static_cast<int16_t>(line->xPos + block->wordXpos(i) + marginLeft);
      box.y = static_cast<int16_t>(line->yPos + marginTop);
      box.style = block->wordStyle(i);
      box.width = 0;  // measured below, once the advance table is ready
      box.row = rowCount;
      box.blockIdentity = block.get();
      box.text = text;
      box.joinWithoutSpaceBefore = false;
      words.push_back(box);
      rowHasWords = true;

      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(box.style) & 0x03));
    }
    if (rowHasWords) rowCount++;
    if (words.size() >= MAX_VISIBLE_WORDS) break;
  }

  if (styleMask == 0) styleMask = 0x01;  // REGULAR
  renderer.ensureSdCardFontReady(fontId, pageText.c_str(), styleMask);
  for (auto& word : words) {
    word.width = static_cast<int16_t>(renderer.getTextAdvanceX(fontId, word.text, word.style));
  }
  const int naturalSpaceWidth = renderer.getSpaceWidth(fontId);
  for (size_t index = 1; index < words.size(); ++index) {
    auto& current = words[index];
    const auto& previous = words[index - 1];
    if (current.blockIdentity != previous.blockIdentity || current.row != previous.row) continue;
    const int gap =
        current.x >= previous.x ? current.x - (previous.x + previous.width) : previous.x - (current.x + current.width);
    current.joinWithoutSpaceBefore = gap < naturalSpaceWidth / 2;
  }
}

// Index of the word in `row` whose horizontal center is closest to centerX;
// -1 when the row has no words.
int DictionaryWordSelectActivity::closestInRow(const uint16_t row, const int centerX) const {
  int best = -1;
  int bestDistance = INT_MAX;
  for (int i = 0; i < static_cast<int>(words.size()); i++) {
    if (words[i].row != row) continue;
    const int distance = std::abs(words[i].x + words[i].width / 2 - centerX);
    if (distance < bestDistance) {
      bestDistance = distance;
      best = i;
    }
  }
  return best;
}

void DictionaryWordSelectActivity::moveVertical(const int direction) {
  const WordBox& current = words[selected];
  const int targetRow = static_cast<int>(current.row) + direction;
  if (targetRow < 0 || targetRow >= static_cast<int>(rowCount)) return;

  const int best = closestInRow(static_cast<uint16_t>(targetRow), current.x + current.width / 2);
  if (best >= 0 && best != selected) {
    selected = best;
    selectionCount = 1;
    snapshotIdx = -1;
    requestUpdate();
  }
}

void DictionaryWordSelectActivity::moveHorizontal(const int direction) {
  const int next = selected + direction;
  if (next < 0 || next >= static_cast<int>(words.size())) return;
  selected = next;
  selectionCount = 1;
  snapshotIdx = -1;
  requestUpdate();
}

bool DictionaryWordSelectActivity::buildSelectedPhrase(const size_t count, std::string& out) const {
  if (selected < 0 || count == 0 || static_cast<size_t>(selected) + count > words.size()) return false;
  const char* tokens[DictionaryQuery::MAX_PHRASE_TOKENS] = {};
  bool joins[DictionaryQuery::MAX_PHRASE_TOKENS] = {};
  for (size_t i = 0; i < count; i++) {
    tokens[i] = words[selected + i].text;
    joins[i] = words[selected + i].joinWithoutSpaceBefore;
  }
  return DictionaryQuery::buildPhrase(tokens, count, out, joins);
}

bool DictionaryWordSelectActivity::canExtendSelection() const {
  if (selectionCount >= DictionaryQuery::MAX_PHRASE_TOKENS ||
      static_cast<size_t>(selected) + selectionCount >= words.size()) {
    return false;
  }
  const WordBox& previous = words[selected + selectionCount - 1];
  const WordBox& next = words[selected + selectionCount];
  if (previous.blockIdentity != next.blockIdentity) return false;

  const std::string previousToken = previous.text ? previous.text : "";
  const size_t last = previousToken.find_last_not_of(" \t\r\n");
  if (last != std::string::npos && (previousToken[last] == '.' || previousToken[last] == '!' ||
                                    previousToken[last] == '?' || previousToken[last] == ';')) {
    return false;
  }

  std::string phrase;
  return buildSelectedPhrase(selectionCount + 1, phrase);
}

bool DictionaryWordSelectActivity::resizeSelection(const int delta) {
  if (delta > 0) {
    if (!canExtendSelection()) return false;
    selectionCount++;
  } else {
    if (selectionCount <= 1) return false;
    selectionCount--;
  }
  snapshotIdx = -1;
  requestUpdate();
  return true;
}

void DictionaryWordSelectActivity::performLookup() {
  popup = Popup::Busy;
  if (!dictOpenAttempted) {
    dictOpenAttempted = true;
    dictOpenOk = dict.open(SETTINGS.dictionaryName);
    dictNeedsIndex = dictOpenOk && dict.needsIndex();
  }
  popupMsg = dictNeedsIndex ? StrId::STR_DICT_INDEXING : StrId::STR_DICT_LOOKING_UP;
  requestUpdateAndWait();  // paint the page + busy popup before blocking on SD

  bool ok = dictOpenOk;
  Dictionary::IndexResult indexResult = Dictionary::IndexResult::Ok;
  if (ok && dictNeedsIndex) {
    ok = dict.buildIndex(&indexBuildYield, nullptr, &indexResult);
    if (ok) dictNeedsIndex = false;
  }

  std::string definition;
  std::string headword;
  std::string successfulQuery;
  Dictionary::LookupResult lookupResult = Dictionary::LookupResult::NotFound;
  bool found = false;
  if (ok) {
    for (size_t count = selectionCount; count > 0 && !found; count--) {
      std::string query;
      if (!buildSelectedPhrase(count, query)) continue;
      found = count == 1 ? dict.lookup(query.c_str(), definition, headword, &lookupResult)
                         : dict.lookupExact(query.c_str(), definition, headword, &lookupResult);
      if (found) successfulQuery = std::move(query);
      if (!found && lookupResult != Dictionary::LookupResult::NotFound) break;
    }
  }

  if (found) {
    auto definitionActivity = makeUniqueNoThrow<DictionaryDefinitionActivity>(
        renderer, mappedInput, std::move(headword), std::move(definition));
    if (!definitionActivity) {
      LOG_ERR("DICT", "OOM allocating DictionaryDefinitionActivity (%u bytes)",
              static_cast<unsigned>(sizeof(DictionaryDefinitionActivity)));
      popup = Popup::Error;
      popupMsg = StrId::STR_DICT_LOW_MEMORY;
      popupTime = millis();
      requestUpdate();
      return;
    }
    DICTIONARY_HISTORY.record(successfulQuery);
    popup = Popup::None;
    startActivityForResult(std::move(definitionActivity),
                           [this](const ActivityResult&) { requestUpdate(); });
    return;
  }
  if (!ok) {
    popup = Popup::Error;
    popupMsg = dictNeedsIndex ? indexErrorMessage(indexResult) : StrId::STR_DICT_ERROR;
  } else if (lookupResult == Dictionary::LookupResult::NotFound) {
    popup = Popup::NotFound;
    popupMsg = StrId::STR_DICT_NOT_FOUND;
  } else {
    popup = Popup::Error;
    popupMsg = lookupErrorMessage(lookupResult);
  }
  popupTime = millis();
  requestUpdate();
}

void DictionaryWordSelectActivity::loop() {
  if (popup == Popup::NotFound || popup == Popup::Error) {
    if (millis() - popupTime >= POPUP_DURATION_MS) {
      popup = Popup::None;
      requestUpdate();
    }
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) confirmPressSeen = true;

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) && confirmPressSeen && !words.empty()) {
    performLookup();
    return;
  }

  if (words.empty()) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    leftHeld = true;
    leftLongHandled = false;
  }
  if (mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    rightHeld = true;
    rightLongHandled = false;
  }
  if (leftHeld && !leftLongHandled && mappedInput.isPressed(MappedInputManager::Button::Left) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Left) >= LONG_PRESS_MS) {
    leftLongHandled = true;
    resizeSelection(-1);
    return;
  }
  if (rightHeld && !rightLongHandled && mappedInput.isPressed(MappedInputManager::Button::Right) &&
      mappedInput.getHeldTime(MappedInputManager::Button::Right) >= LONG_PRESS_MS) {
    rightLongHandled = true;
    resizeSelection(1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Left) && leftHeld) {
    const bool move = !leftLongHandled;
    leftHeld = false;
    if (move) moveHorizontal(-1);
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Right) && rightHeld) {
    const bool move = !rightLongHandled;
    rightHeld = false;
    if (move) moveHorizontal(1);
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    moveVertical(1);
  }
}

// Saves the pixels under words[selected]'s highlight box, then draws the
// highlight over them. Returns false when the pixels could not be saved
// (no buffer / oversize box) — the highlight is drawn regardless, but the
// next cursor move must do a full repaint.
bool DictionaryWordSelectActivity::drawHighlightWithSnapshot() {
  const WordBox& word = words[selected];
  int hx = word.x - 2;
  int hy = word.y - 2;
  int hw = word.width + 4;
  int hh = lineHeight + 4;
  // Clamp to the panel so save, draw and restore all use the same box.
  if (hx < 0) {
    hw += hx;
    hx = 0;
  }
  if (hy < 0) {
    hh += hy;
    hy = 0;
  }

  bool saved = false;
  if (snapshot && hw > 0 && hh > 0) {
    saved = renderer.readFramebufferRegion(hx, hy, hw, hh, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(hx);
  snapshotY = static_cast<int16_t>(hy);
  snapshotW = static_cast<int16_t>(hw);
  snapshotH = static_cast<int16_t>(hh);
  snapshotIdx = saved ? selected : -1;

  renderer.fillRect(hx, hy, hw, hh, true);
  renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
  return saved;
}

void DictionaryWordSelectActivity::drawSelection() {
  for (size_t i = 0; i < selectionCount && static_cast<size_t>(selected) + i < words.size(); i++) {
    const WordBox& word = words[selected + i];
    renderer.fillRect(word.x - 2, word.y - 2, word.width + 4, lineHeight + 4, true);
    renderer.drawText(fontId, word.x, word.y, word.text, false, word.style);
  }
}

// Front-button bar (Back/Confirm/Left/Right). Drawn last on every repaint
// path, including the differential highlight-only path, so it always ends
// up as the top layer even when a highlighted word's box falls under a
// hint's screen area. No side-button hints: Up/Down row jump has no spare
// screen area on this page (it reuses the reader's full-bleed layout), and
// a hint box there would hide text instead of sitting in a reserved gutter.
void DictionaryWordSelectActivity::drawHints() const {
  // No selectable word on this page: Confirm/Left/Right are all no-ops
  // (guarded by words.empty() in loop()/performLookup), so only Back does
  // anything and only Back is hinted.
  if (words.empty()) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    return;
  }
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_LOOKUP), tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void DictionaryWordSelectActivity::render(RenderLock&&) {
  // Differential fast path: only the highlight moved and the framebuffer
  // still holds a clean page (no popup or sub-activity since the last full
  // repaint). Restore the pixels under the old highlight, draw the new one,
  // and push — skipping the two-pass page render entirely.
  if (popup == Popup::None && selectionCount == 1 && snapshotIdx >= 0 && !words.empty() && selected != snapshotIdx) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    // The full path's PrewarmScope cleared the glyph cache on exit; batch-load
    // just the highlighted word's glyphs before drawing them white-on-black.
    renderer.getFontCacheManager()->prewarmCache(
        fontId, words[selected].text, static_cast<uint8_t>(1u << (static_cast<uint8_t>(words[selected].style) & 0x03)));
    if (drawHighlightWithSnapshot()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
    // Snapshot failed (oversize box) — fall through to a full repaint.
  }

  renderer.clearScreen();

  // Same prewarm-scan-then-render pass the reader uses, so SD-card fonts hit
  // the in-RAM glyph cache during the real draw.
  auto* fcm = renderer.getFontCacheManager();
  auto scope = fcm->createPrewarmScope();
  page->render(renderer, fontId, marginLeft, marginTop);
  scope.endScanAndPrewarm();
  page->render(renderer, fontId, marginLeft, marginTop);

  if (!words.empty()) {
    if (selectionCount == 1) {
      drawHighlightWithSnapshot();
    } else {
      snapshotIdx = -1;
      drawSelection();
    }
  }

  drawHints();

  if (popup != Popup::None) {
    // The popup overdraws the page, so the snapshot no longer matches the
    // framebuffer — force the next render onto the full-repaint path.
    snapshotIdx = -1;
    // drawPopup overlays the framebuffer and refreshes the display itself.
    // I18N.get directly: tr() only accepts literal key names.
    GUI.drawPopup(renderer, I18N.get(popupMsg));
    return;
  }
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
