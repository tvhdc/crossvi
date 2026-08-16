#include "Activity.h"

#include <Epub.h>
#include <I18n.h>
#include <Txt.h>
#include <Xtc.h>

#include "ActivityManager.h"
#include "components/UITheme.h"

void Activity::onEnter() { LOG_DBG("ACT", "Entering activity: %s", name.c_str()); }

void Activity::onExit() { LOG_DBG("ACT", "Exiting activity: %s", name.c_str()); }

void Activity::requestUpdate(bool immediate) { activityManager.requestUpdate(immediate); }

void Activity::requestUpdateAndWait() { activityManager.requestUpdateAndWait(); }

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
  activityManager.beginReaderOpenMetric();
  openingBook.store(true, std::memory_order_release);
  requestUpdateAndWait();
  activityManager.goToReader(std::move(preparedEpub), openOrigin, completionStatsAlreadyRecovered, true);
}

void Activity::openBookWithFeedback(std::unique_ptr<Xtc>&& preparedXtc, const ReaderOpenOrigin openOrigin,
                                    const bool completionStatsAlreadyRecovered) {
  if (!preparedXtc) return;
  activityManager.beginReaderOpenMetric();
  openingBook.store(true, std::memory_order_release);
  requestUpdateAndWait();
  activityManager.goToReader(std::move(preparedXtc), openOrigin, completionStatsAlreadyRecovered, true);
}

void Activity::openBookWithFeedback(std::unique_ptr<Txt>&& preparedTxt, const ReaderOpenOrigin openOrigin,
                                    const bool completionStatsAlreadyRecovered) {
  if (!preparedTxt) return;
  activityManager.beginReaderOpenMetric();
  openingBook.store(true, std::memory_order_release);
  requestUpdateAndWait();
  activityManager.goToReader(std::move(preparedTxt), openOrigin, completionStatsAlreadyRecovered, true);
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

void Activity::onSelectBook(const std::string& path) { openBookWithFeedback(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
