#include "Activity.h"

#include <I18n.h>

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

void Activity::openBookWithFeedback(const std::string& path, const ReaderOpenOrigin openOrigin) {
  openingBook.store(true, std::memory_order_release);
  requestUpdateAndWait();
  activityManager.goToReader(path, false, openOrigin);
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

void Activity::onSelectBook(const std::string& path) { openBookWithFeedback(path); }

void Activity::startActivityForResult(std::unique_ptr<Activity>&& activity, ActivityResultHandler resultHandler) {
  this->resultHandler = std::move(resultHandler);
  activityManager.pushActivity(std::move(activity));
}

void Activity::setResult(ActivityResult&& result) { this->result = std::move(result); }

void Activity::finish() { activityManager.popActivity(); }
