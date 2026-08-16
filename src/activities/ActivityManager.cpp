#include "ActivityManager.h"

#include <FontCacheManager.h>
#include <HalPowerManager.h>
#include <HalStorage.h>

#include <algorithm>

#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "RenderGeneration.h"
#include "boot_sleep/BootActivity.h"
#include "boot_sleep/SleepActivity.h"
#include "browser/OpdsBookBrowserActivity.h"
#include "home/CrashActivity.h"
#include "home/FileBrowserActivity.h"
#include "home/HomeActivity.h"
#include "home/RecentBooksActivity.h"
#include "network/CrossPointWebServerActivity.h"
#include "reader/ReaderActivity.h"
#include "reader/SavedClippingsActivity.h"
#include "reader/VCodexStatsImportActivity.h"
#include "settings/OpdsServerListActivity.h"
#include "settings/SettingsActivity.h"
#include "util/FullScreenMessageActivity.h"

static portMUX_TYPE activityManagerSpinlock = portMUX_INITIALIZER_UNLOCKED;

void ActivityManager::begin() {
  xTaskCreatePinnedToCore(&renderTaskTrampoline, "ActivityManagerRender",
                          8192,               // Stack size
                          this,               // Parameters
                          1,                  // Priority
                          &renderTaskHandle,  // Task handle
                          0                   // Pin to core 0 (PRO_CPU)
  );
  assert(renderTaskHandle != nullptr && "Failed to create render task");
}

void ActivityManager::renderTaskTrampoline(void* param) {
  auto* self = static_cast<ActivityManager*>(param);
  self->renderTaskLoop();
}

void ActivityManager::renderTaskLoop() {
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint32_t targetGeneration = requestedRenderGeneration.load(std::memory_order_acquire);
    // Acquire the lock before reading currentActivity to avoid a TOCTOU race
    // where the main task deletes the activity between the null-check and render().
    RenderLock lock;
    if (currentActivity) {
      HalPowerManager::Lock powerLock;  // Ensure we don't go into low-power mode while rendering
      currentActivity->render(std::move(lock));
    }
    // Notify any task blocked in requestUpdateAndWait() that the render is done.
    TaskHandle_t waiter = nullptr;
    taskENTER_CRITICAL(&activityManagerSpinlock);
    completedRenderGeneration.store(targetGeneration, std::memory_order_release);
    if (waitingTaskHandle && RenderGeneration::reached(targetGeneration, waitingRenderGeneration)) {
      waiter = waitingTaskHandle;
      waitingTaskHandle = nullptr;
      waitingRenderGeneration = 0;
    }
    taskEXIT_CRITICAL(&activityManagerSpinlock);
    if (waiter) {
      xTaskNotify(waiter, 1, eIncrement);
    }
  }
}

void ActivityManager::loop() {
  if (currentActivity) {
    // Note: do not hold a lock here, the loop() method must be responsible for acquire one if needed
    currentActivity->loop();
  }

  while (pendingAction != PendingAction::None) {
    if (pendingAction == PendingAction::Pop) {
      RenderLock lock;

      if (!currentActivity) {
        // Should never happen in practice
        LOG_ERR("ACT", "Pop set but currentActivity is null; ignoring pop request");
        pendingAction = PendingAction::None;
        continue;
      }

      // Probe before every pop, including nested Settings activities. Their
      // onExit/result handlers may persist state; after card removal those
      // writes must fail fast instead of each waiting for an SD timeout.
      Storage.probeMedia();

      ActivityResult pendingResult = std::move(currentActivity->result);

      // Destroy the current activity
      exitActivity(lock);
      pendingAction = PendingAction::None;

      if (stackActivities.empty()) {
        LOG_DBG("ACT", "No more activities on stack, going home");
        lock.unlock();  // goHome may acquire its own lock
        goHome();
        continue;  // Will launch goHome immediately

      } else {
        currentActivity = std::move(stackActivities.back());
        stackActivities.pop_back();
        LOG_DBG("ACT", "Popped from activity stack, new size = %zu", stackActivities.size());
        // Handle result if necessary
        if (currentActivity->resultHandler) {
          LOG_DBG("ACT", "Handling result for popped activity");

          // Move it here to avoid the case where handler calling another startActivityForResult()
          auto handler = std::move(currentActivity->resultHandler);
          currentActivity->resultHandler = nullptr;
          lock.unlock();  // Handler may acquire its own lock
          handler(pendingResult);
        }

        // Request an update to ensure the popped activity gets re-rendered
        if (pendingAction == PendingAction::None) {
          // Match onPause() from the Push path. Run without the render lock so
          // activities may safely perform their normal resume work.
          lock.unlock();
          currentActivity->onResume();
          requestUpdate();
        }

        // Handler may request another pending action, we will handle it in the next loop iteration
        continue;
      }

    } else if (pendingActivity) {
      // Current activity has requested a new activity to be launched
      RenderLock lock;

      if (pendingAction == PendingAction::Replace) {
        // Destroy the current activity
        exitActivity(lock);
        // Clear the stack
        while (!stackActivities.empty()) {
          stackActivities.back()->onExit();
          stackActivities.pop_back();
        }
      } else if (pendingAction == PendingAction::Push) {
        // The current activity stays alive, but is no longer visible while the
        // child is on top of it.
        currentActivity->onPause();
        // Move current activity to stack
        stackActivities.push_back(std::move(currentActivity));
        LOG_DBG("ACT", "Pushed to activity stack, new size = %zu", stackActivities.size());
      }
      pendingAction = PendingAction::None;
      currentActivity = std::move(pendingActivity);

      lock.unlock();  // onEnter may acquire its own lock
      currentActivity->onEnter();

      // onEnter may request another pending action, we will handle it in the next loop iteration
      continue;
    }
  }

  if (requestedUpdate.exchange(false)) {
    // Using direct notification to signal the render task to update
    // Increment counter so multiple rapid calls won't be lost
    if (renderTaskHandle) {
      requestedRenderGeneration.fetch_add(1, std::memory_order_release);
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  }
}

void ActivityManager::exitActivity(const RenderLock& lock) {
  // Note: lock must be held by the caller
  if (currentActivity) {
    currentActivity->onExit();
    currentActivity.reset();
  }
}

void ActivityManager::replaceActivity(std::unique_ptr<Activity>&& newActivity) {
  // Note: no lock here, this is usually called by loop() and we may run into deadlock
  if (currentActivity) {
    // Defer launch if we're currently in an activity, to avoid deleting the current activity
    // leading to the "delete this" problem
    pendingActivity = std::move(newActivity);
    pendingAction = PendingAction::Replace;
  } else {
    // No current activity, safe to launch immediately
    currentActivity = std::move(newActivity);
    currentActivity->onEnter();
  }
}

void ActivityManager::maybeOfferVCodexStatsImport() {
  if (pendingAction != PendingAction::Replace || !pendingActivity) return;
  VCodexStatsImportSummary summary;
  const VCodexStatsImporter::ProbeResult probe = VCodexStatsImporter::probe(summary);
  if (probe != VCodexStatsImporter::ProbeResult::Offer && probe != VCodexStatsImporter::ProbeResult::PendingRecovery) {
    return;
  }
  pendingActivity =
      std::make_unique<VCodexStatsImportActivity>(renderer, mappedInput, probe, summary, std::move(pendingActivity));
}

void ActivityManager::goToFileTransfer() {
  yourBooksReturnState.reset();
  savedClippingsReturnState.reset();
  replaceActivity(std::make_unique<CrossPointWebServerActivity>(renderer, mappedInput));
}

void ActivityManager::goToSettings() {
  yourBooksReturnState.reset();
  savedClippingsReturnState.reset();
  replaceActivity(std::make_unique<SettingsActivity>(renderer, mappedInput));
}

void ActivityManager::goToFileBrowser(std::string path) {
  yourBooksReturnState.reset();
  savedClippingsReturnState.reset();
  replaceActivity(std::make_unique<FileBrowserActivity>(renderer, mappedInput, std::move(path)));
}

void ActivityManager::goToYourBooks(std::optional<YourBooksReturnState> returnState) {
  if (returnState.has_value()) {
    replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput, std::move(returnState)));
    return;
  }
  yourBooksReturnState.reset();
  savedClippingsReturnState.reset();
  replaceActivity(std::make_unique<RecentBooksActivity>(renderer, mappedInput));
}

void ActivityManager::goToSavedClippings(std::optional<SavedClippingsReturnState> returnState) {
  if (returnState.has_value()) {
    replaceActivity(std::make_unique<SavedClippingsActivity>(renderer, mappedInput, std::move(returnState)));
    return;
  }
  savedClippingsReturnState.reset();
  replaceActivity(std::make_unique<SavedClippingsActivity>(renderer, mappedInput));
}

void ActivityManager::goToBrowser() {
  const auto& servers = OPDS_STORE.getServers();
  // Skip the server picker when there's only one server configured
  if (servers.size() == 1) {
    replaceActivity(std::make_unique<OpdsBookBrowserActivity>(renderer, mappedInput, servers[0]));
  } else {
    replaceActivity(std::make_unique<OpdsServerListActivity>(renderer, mappedInput, true));
  }
}

void ActivityManager::goToReader(std::string path, const bool allowFastInitialRefresh,
                                 const ReaderOpenOrigin openOrigin, const bool completionStatsAlreadyRecovered,
                                 const bool readerOpenFeedbackAlreadyShown,
                                 const RawSourceIdentityHandoff* const preparedSourceIdentity) {
  if (!readerOpenFeedbackAlreadyShown) beginReaderOpenMetric();
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path), allowFastInitialRefresh,
                                                   openOrigin, completionStatsAlreadyRecovered,
                                                   readerOpenFeedbackAlreadyShown, preparedSourceIdentity));
}

void ActivityManager::goToReader(std::unique_ptr<Epub>&& preparedEpub, const ReaderOpenOrigin openOrigin,
                                 const bool completionStatsAlreadyRecovered,
                                 const bool readerOpenFeedbackAlreadyShown) {
  if (!readerOpenFeedbackAlreadyShown) beginReaderOpenMetric();
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(preparedEpub), openOrigin,
                                                   completionStatsAlreadyRecovered, readerOpenFeedbackAlreadyShown));
}

void ActivityManager::goToReader(std::unique_ptr<Xtc>&& preparedXtc, const ReaderOpenOrigin openOrigin,
                                 const bool completionStatsAlreadyRecovered,
                                 const bool readerOpenFeedbackAlreadyShown) {
  if (!readerOpenFeedbackAlreadyShown) beginReaderOpenMetric();
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(preparedXtc), openOrigin,
                                                   completionStatsAlreadyRecovered, readerOpenFeedbackAlreadyShown));
}

void ActivityManager::goToReader(std::unique_ptr<Txt>&& preparedTxt, const ReaderOpenOrigin openOrigin,
                                 const bool completionStatsAlreadyRecovered,
                                 const bool readerOpenFeedbackAlreadyShown) {
  if (!readerOpenFeedbackAlreadyShown) beginReaderOpenMetric();
  replaceActivity(std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(preparedTxt), openOrigin,
                                                   completionStatsAlreadyRecovered, readerOpenFeedbackAlreadyShown));
}

void ActivityManager::goToReader(std::string path, ClippingJumpResult clippingJump, const ReaderOpenOrigin openOrigin) {
  beginReaderOpenMetric();
  replaceActivity(
      std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path), std::move(clippingJump), openOrigin));
}

void ActivityManager::goToReader(std::string path, SavedBookmarkJumpResult bookmarkJump,
                                 const ReaderOpenOrigin openOrigin) {
  beginReaderOpenMetric();
  replaceActivity(
      std::make_unique<ReaderActivity>(renderer, mappedInput, std::move(path), std::move(bookmarkJump), openOrigin));
}

void ActivityManager::beginReaderOpenMetric() {
  readerOpenStartedMs.store(static_cast<uint32_t>(millis()), std::memory_order_relaxed);
  readerOpenMetricActive.store(true, std::memory_order_release);
}

void ActivityManager::reportReaderOpenStage(const char* const format, const char* const stage,
                                            const uint32_t stageStartedMs) const {
  if (!readerOpenMetricActive.load(std::memory_order_acquire)) return;
  const uint32_t now = static_cast<uint32_t>(millis());
  const uint32_t openStarted = readerOpenStartedMs.load(std::memory_order_relaxed);
  LOG_DBG("ROPM", "stage format=%s name=%s elapsed_ms=%u total_ms=%u", format, stage,
          static_cast<unsigned>(now - stageStartedMs), static_cast<unsigned>(now - openStarted));
}

void ActivityManager::finishReaderOpenMetric(const char* const format, const uint32_t visibleAtMs) {
  if (!readerOpenMetricActive.exchange(false, std::memory_order_acq_rel)) return;
  const uint32_t openStarted = readerOpenStartedMs.load(std::memory_order_relaxed);
  LOG_DBG("ROPM", "first_visible format=%s total_ms=%u", format, static_cast<unsigned>(visibleAtMs - openStarted));
}

void ActivityManager::cancelReaderOpenMetric(const char* const reason) {
  if (!readerOpenMetricActive.exchange(false, std::memory_order_acq_rel)) return;
  const uint32_t now = static_cast<uint32_t>(millis());
  const uint32_t openStarted = readerOpenStartedMs.load(std::memory_order_relaxed);
  LOG_DBG("ROPM", "cancel reason=%s total_ms=%u", reason, static_cast<unsigned>(now - openStarted));
}

void ActivityManager::goToSleep() {
  replaceActivity(std::make_unique<SleepActivity>(renderer, mappedInput));
  loop();  // Important: sleep screen must be rendered immediately, the caller will go to sleep right after this returns
}

void ActivityManager::goToBoot(const bool minimalWakeScreen) {
  replaceActivity(std::make_unique<BootActivity>(renderer, mappedInput, minimalWakeScreen));
}

void ActivityManager::goToFullScreenMessage(std::string message, EpdFontFamily::Style style) {
  replaceActivity(std::make_unique<FullScreenMessageActivity>(renderer, mappedInput, std::move(message), style));
}

void ActivityManager::goHome(HomeMenuItem initialMenuItem) {
  yourBooksReturnState.reset();
  savedClippingsReturnState.reset();
  // Probe before the current activity's onExit() runs. If the card was
  // removed, persistence calls then fail immediately instead of each waiting
  // for a separate SD timeout before Home can appear.
  Storage.probeMedia();
  if (initialMenuItem == HomeMenuItem::NONE && currentActivity) {
    const auto& activityName = currentActivity->name;
    if (activityName == "FileBrowser") {
      initialMenuItem = HomeMenuItem::FILE_BROWSER;
    } else if (activityName == "YourBooks") {
      initialMenuItem = HomeMenuItem::RECENTS;
    } else if (activityName == "SavedClippings") {
      initialMenuItem = HomeMenuItem::SAVED_ITEMS;
    } else if (activityName == "OpdsBookBrowser") {
      initialMenuItem = HomeMenuItem::OPDS_BROWSER;
    } else if (activityName == "CrossPointWebServer") {
      initialMenuItem = HomeMenuItem::FILE_TRANSFER;
    } else if (activityName == "Settings") {
      initialMenuItem = HomeMenuItem::SETTINGS_MENU;
    }
  }
  replaceActivity(std::make_unique<HomeActivity>(renderer, mappedInput, initialMenuItem));
}

void ActivityManager::captureYourBooksReturnContext(const uint8_t tab, const size_t selectedIndex,
                                                    std::string selectedPath, std::string searchQuery) {
  constexpr size_t MAX_RETURN_PATH_BYTES = 511;
  constexpr size_t MAX_RETURN_QUERY_BYTES = 64;
  if (selectedPath.size() > MAX_RETURN_PATH_BYTES) selectedPath.resize(MAX_RETURN_PATH_BYTES);
  if (searchQuery.size() > MAX_RETURN_QUERY_BYTES) searchQuery.resize(MAX_RETURN_QUERY_BYTES);
  yourBooksReturnState = YourBooksReturnState{tab, selectedIndex, std::move(selectedPath), std::move(searchQuery)};
}

void ActivityManager::captureSavedClippingsReturnContext(const size_t selectedIndex, std::string selectedPath) {
  constexpr size_t MAX_RETURN_PATH_BYTES = 511;
  if (selectedPath.size() > MAX_RETURN_PATH_BYTES) selectedPath.resize(MAX_RETURN_PATH_BYTES);
  savedClippingsReturnState = SavedClippingsReturnState{selectedIndex, std::move(selectedPath)};
}

void ActivityManager::returnFromReaderOrHome() {
  if (savedClippingsReturnState.has_value()) {
    auto state = std::move(savedClippingsReturnState);
    savedClippingsReturnState.reset();
    goToSavedClippings(std::move(state));
    return;
  }
  if (!yourBooksReturnState.has_value()) {
    goHome();
    return;
  }
  auto state = std::move(yourBooksReturnState);
  yourBooksReturnState.reset();
  goToYourBooks(std::move(state));
}
void ActivityManager::goToCrashReport() { replaceActivity(std::make_unique<CrashActivity>(renderer, mappedInput)); }

void ActivityManager::pushActivity(std::unique_ptr<Activity>&& activity) {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while pushActivity is not expected");
    pendingActivity.reset();
  }
  pendingActivity = std::move(activity);
  pendingAction = PendingAction::Push;
}

void ActivityManager::popActivity() {
  if (pendingActivity) {
    // Should never happen in practice
    LOG_ERR("ACT", "pendingActivity while popActivity is not expected");
    pendingActivity.reset();
  }
  pendingAction = PendingAction::Pop;
}

bool ActivityManager::preventAutoSleep() const { return currentActivity && currentActivity->preventAutoSleep(); }

bool ActivityManager::handleGlobalShortcut(const GlobalShortcut shortcut) {
  if (shortcut == GlobalShortcut::RefreshScreen) {
    if (!handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
    return true;
  }
  return currentActivity && currentActivity->handleGlobalShortcut(shortcut);
}

bool ActivityManager::handleReaderShortcut(const uint8_t function) {
  return currentActivity && currentActivity->isReaderActivity() && currentActivity->handleReaderShortcut(function);
}

bool ActivityManager::handleForcedRefresh() { return currentActivity && currentActivity->handleForcedRefresh(); }

bool ActivityManager::handleSafeGlobalShortcut(const GlobalShortcut shortcut) {
  if (shortcut == GlobalShortcut::GoHome) {
    if (currentActivity && currentActivity->name == "Home") return true;
    goHome();
    return true;
  }
  if (shortcut != GlobalShortcut::ResumeReading) return false;

  if (currentActivity && currentActivity->isReaderActivity()) return true;
  if (!stackActivities.empty() && stackActivities.back()->isReaderActivity()) {
    popActivity();
    return true;
  }

  for (const auto& book : RECENT_BOOKS.getBooks()) {
    if (!book.path.empty() && Storage.exists(book.path.c_str())) {
      goToReader(book.path);
      return true;
    }
  }
  goHome();
  return true;
}

bool ActivityManager::isReaderActivity() const { return currentActivity && currentActivity->isReaderActivity(); }

bool ActivityManager::hasReaderActivity() const {
  if (isReaderActivity()) return true;
  return std::any_of(stackActivities.begin(), stackActivities.end(),
                     [](const auto& activity) { return activity && activity->isReaderActivity(); });
}

bool ActivityManager::skipLoopDelay() const { return currentActivity && currentActivity->skipLoopDelay(); }

ScreenshotInfo ActivityManager::getScreenshotInfo() const {
  if (currentActivity) {
    const ScreenshotInfo info = currentActivity->getScreenshotInfo();
    if (info.readerType != ScreenshotInfo::ReaderType::None) return info;
  }
  for (auto it = stackActivities.rbegin(); it != stackActivities.rend(); ++it) {
    if (*it) {
      const ScreenshotInfo info = (*it)->getScreenshotInfo();
      if (info.readerType != ScreenshotInfo::ReaderType::None) return info;
    }
  }
  return {};
}

void ActivityManager::requestUpdate(bool immediate) {
  if (immediate) {
    if (renderTaskHandle) {
      requestedRenderGeneration.fetch_add(1, std::memory_order_release);
      xTaskNotify(renderTaskHandle, 1, eIncrement);
    }
  } else {
    // Deferring the update until current loop is finished
    // This is to avoid multiple updates being requested in the same loop
    requestedUpdate = true;
  }
}
void ActivityManager::requestUpdateAndWait() {
  if (!renderTaskHandle) {
    return;
  }

  auto currTaskHandler = xTaskGetCurrentTaskHandle();
  auto mutexHolder = xSemaphoreGetMutexHolder(renderingMutex);
  bool isRenderTask = (currTaskHandler == renderTaskHandle);
  bool holdingRenderLock = (mutexHolder == currTaskHandler);

  // Render task cannot call requestUpdateAndWait() or it will cause a deadlock
  assert(!isRenderTask && "Render task cannot call requestUpdateAndWait()");
  // Cannot call while holding RenderLock or it will cause a deadlock
  assert(!holdingRenderLock && "Cannot call requestUpdateAndWait() while holding RenderLock");
  if (isRenderTask || holdingRenderLock) return;

  uint32_t targetGeneration = 0;
  bool alreadyWaiting = false;
  taskENTER_CRITICAL(&activityManagerSpinlock);
  alreadyWaiting = waitingTaskHandle != nullptr;
  if (!alreadyWaiting) {
    targetGeneration = requestedRenderGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    waitingTaskHandle = currTaskHandler;
    waitingRenderGeneration = targetGeneration;
  }
  taskEXIT_CRITICAL(&activityManagerSpinlock);

  // There should never be the case where 2 tasks are waiting for a render at the same time
  assert(!alreadyWaiting && "Already waiting for a render to complete");
  if (alreadyWaiting) return;

  xTaskNotify(renderTaskHandle, 1, eIncrement);
  do {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  } while (!RenderGeneration::reached(completedRenderGeneration.load(std::memory_order_acquire), targetGeneration));
}

// RenderLock

RenderLock::RenderLock() {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::RenderLock(std::try_to_lock_t) { isLocked = xSemaphoreTake(activityManager.renderingMutex, 0) == pdTRUE; }

RenderLock::RenderLock([[maybe_unused]] Activity&) {
  xSemaphoreTake(activityManager.renderingMutex, portMAX_DELAY);
  isLocked = true;
}

RenderLock::~RenderLock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

void RenderLock::unlock() {
  if (isLocked) {
    xSemaphoreGive(activityManager.renderingMutex);
    isLocked = false;
  }
}

/**
 *
 * Checks if renderingMutex is busy.
 *
 * @return true if renderingMutex is busy, otherwise false.
 *
 */
bool RenderLock::peek() { return xQueuePeek(activityManager.renderingMutex, NULL, 0) != pdTRUE; };
