#include "Activity.h"

#include <Epub.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "ActivityManager.h"
#include "components/UITheme.h"

void Activity::onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) {
  cancelTransientPopup();
  activityManager.requestUpdate(immediate);
}

void Activity::requestUpdateAndWait() {
  cancelTransientPopup();
  activityManager.requestUpdateAndWait();
}

bool Activity::handleSafeGlobalShortcut(const GlobalShortcut shortcut) {
  return activityManager.handleSafeGlobalShortcut(shortcut);
}

void Activity::onGoHome(HomeMenuItem item) { activityManager.goHome(item); }

void Activity::openBookWithFeedback(const std::string& path, const ReaderOpenOrigin openOrigin,
                                    const bool completionStatsAlreadyRecovered,
                                    const RawSourceIdentityHandoff* const preparedSourceIdentity) {
  activityManager.beginReaderOpenMetric();
  openingBook.store(true, std::memory_order_release);
  requestUpdateAndWait();
  activityManager.goToReader(path, false, openOrigin, completionStatsAlreadyRecovered, true, preparedSourceIdentity);
}

void Activity::openBookWithFeedback(std::unique_ptr<Epub>&& preparedEpub, const ReaderOpenOrigin openOrigin,
                                    const bool completionStatsAlreadyRecovered) {
  if (!preparedEpub) return;
  activityManager.goToReader(std::move(preparedEpub), openOrigin, completionStatsAlreadyRecovered, false);
}

void Activity::openBookWithFeedback(std::unique_ptr<Xtc>&& preparedXtc, const ReaderOpenOrigin openOrigin,
                                    const bool completionStatsAlreadyRecovered) {
  if (!preparedXtc) return;
  activityManager.goToReader(std::move(preparedXtc), openOrigin, completionStatsAlreadyRecovered, false);
}

void Activity::openBookWithFeedback(std::unique_ptr<Txt>&& preparedTxt, const ReaderOpenOrigin openOrigin,
                                    const bool completionStatsAlreadyRecovered) {
  if (!preparedTxt) return;
  activityManager.goToReader(std::move(preparedTxt), openOrigin, completionStatsAlreadyRecovered, false);
}

bool Activity::renderBookLoadingOverlay() {
  if (!openingBook.load(std::memory_order_acquire)) return false;
  GUI.drawPopup(renderer, tr(STR_LOADING_BOOK));
  return true;
}

void Activity::showReaderExitFeedback() {
  exitingReader.store(true, std::memory_order_release);
  requestUpdateAndWait();
}

bool Activity::renderReaderExitOverlay() {
  if (!exitingReader.load(std::memory_order_acquire)) return false;
  GUI.drawPopup(renderer, tr(STR_EXITING_READER));
  return true;
}

void Activity::queueBlockingFeedback(const StrId message) {
  blockingFeedback.store(message, std::memory_order_release);
  requestUpdate(true);
}

void Activity::clearBlockingFeedback() { blockingFeedback.store(StrId::_COUNT, std::memory_order_release); }

void Activity::showBlockingFeedback(const StrId message) {
  blockingFeedback.store(message, std::memory_order_release);
  requestUpdateAndWait();
}

bool Activity::renderBlockingFeedbackOverlay() {
  const StrId message = blockingFeedback.exchange(StrId::_COUNT, std::memory_order_acq_rel);
  if (message == StrId::_COUNT) return false;
  GUI.drawPopup(renderer, I18N.get(message));
  return true;
}

void Activity::drawTransientPopup(const StrId message) { drawTransientPopup(I18N.get(message)); }

void Activity::drawTransientPopup(const char* const message) {
  GUI.drawPopup(renderer, message);
  uint32_t deadline = static_cast<uint32_t>(millis()) + TRANSIENT_POPUP_DURATION_MS;
  // Zero means "inactive"; remap the single wrap-around value without losing
  // the deadline's wrap-safe comparison semantics.
  deadline += static_cast<uint32_t>(deadline == 0);
  transientPopupDeadlineMs.store(deadline, std::memory_order_release);
}

void Activity::cancelTransientPopup() { transientPopupDeadlineMs.store(0, std::memory_order_release); }

bool Activity::dismissTransientPopupIfExpired() {
  uint32_t deadline = transientPopupDeadlineMs.load(std::memory_order_acquire);
  if (deadline == 0 || static_cast<int32_t>(static_cast<uint32_t>(millis()) - deadline) < 0) return false;
  return transientPopupDeadlineMs.compare_exchange_strong(deadline, 0, std::memory_order_acq_rel);
}

void Activity::onSelectBook(const std::string& path) { openBookWithFeedback(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
