#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "GlobalShortcut.h"
#include "MappedInputManager.h"
#include "home/HomeMenuMapping.h"
#include "util/ScreenshotInfo.h"

class Activity;    // forward declaration
class RenderLock;  // forward declaration
struct ClippingJumpResult;
struct SavedBookmarkJumpResult;

// Keep coverless opening surfaces explicit instead of inferring them from
// return state or settings.
enum class ReaderOpenOrigin : uint8_t {
  Default,
  HomeRecent,
  SavedItems,
};

// Small, bounded state used to restore the library entry point after a reader
// replaces the activity that opened it. It contains no catalog or cover data.
struct YourBooksReturnState {
  uint8_t tab = 0;
  size_t selectedIndex = 0;
  std::string selectedPath;
  std::string searchQuery;
};

// Bounded state used when a reader was opened from the device-wide saved-items
// list.  The reader replaces that activity, so keep only enough information to
// restore the selected book after Back; never retain catalog or cover data.
struct SavedClippingsReturnState {
  size_t selectedIndex = 0;
  std::string selectedPath;
};

/**
 * ActivityManager
 *
 * This mirrors the same concept of Activity in Android, where an activity represents a single screen of the UI. The
 * manager is responsible for launching activities, and ensuring that only one activity is active at a time.
 *
 * It also provides a stack mechanism to allow activities to launch sub-activities and get back the results when the
 * sub-activity is done. For example, the WebServer activity can launch a WifiSelect activity to let the user choose a
 * wifi network, and get back the selected network when the user is done.
 *
 * Main differences from Android's ActivityManager:
 * - onPause/onResume only bracket a child activity; there are no concurrently running background activities
 * - onActivityResult is implemented via a callback instead of a separate method, for simplicity
 */
class ActivityManager {
  friend class RenderLock;

 protected:
  GfxRenderer& renderer;
  MappedInputManager& mappedInput;
  std::vector<std::unique_ptr<Activity>> stackActivities;
  std::unique_ptr<Activity> currentActivity;

  void exitActivity(const RenderLock& lock);

  // Pending activity to be launched on next loop iteration
  std::unique_ptr<Activity> pendingActivity;
  enum class PendingAction { None, Push, Pop, Replace };
  PendingAction pendingAction = PendingAction::None;

  // Task to render and display the activity
  TaskHandle_t renderTaskHandle = nullptr;
  static void renderTaskTrampoline(void* param);
  [[noreturn]] virtual void renderTaskLoop();

  // Set by requestUpdateAndWait(); read and cleared by the render task after render completes.
  // Note: only one waiting task is supported at a time
  TaskHandle_t waitingTaskHandle = nullptr;
  uint32_t waitingRenderGeneration = 0;

  // Mutex to protect rendering operations from race conditions
  // Must only be used via RenderLock
  SemaphoreHandle_t renderingMutex = nullptr;

  // Whether to trigger a render after the current loop()
  // This variable must only be set by the main loop, to avoid race conditions
  std::atomic<bool> requestedUpdate{false};
  // Incremented only after render() (including the blocking panel refresh)
  // returns. Tilt page turning uses this acknowledgement so a second gesture
  // cannot be accepted merely because the render task has not taken its mutex
  // yet.
  std::atomic<uint32_t> completedRenderGeneration{0};
  std::atomic<uint32_t> requestedRenderGeneration{0};
  std::optional<YourBooksReturnState> yourBooksReturnState;
  std::optional<SavedClippingsReturnState> savedClippingsReturnState;

 public:
  explicit ActivityManager(GfxRenderer& renderer, MappedInputManager& mappedInput)
      : renderer(renderer), mappedInput(mappedInput), renderingMutex(xSemaphoreCreateMutex()) {
    assert(renderingMutex != nullptr && "Failed to create rendering mutex");
    stackActivities.reserve(10);
  }
  ~ActivityManager() { assert(false); /* should never be called */ };

  void begin();
  void loop();

  // Will replace currentActivity and drop all activities on stack
  void replaceActivity(std::unique_ptr<Activity>&& newActivity);

  // goTo... functions are convenient wrapper for replaceActivity()
  void goToFileTransfer();
  void goToSettings();
  void goToFileBrowser(std::string path = {});
  void goToYourBooks(std::optional<YourBooksReturnState> returnState = std::nullopt);
  void goToSavedClippings(std::optional<SavedClippingsReturnState> returnState = std::nullopt);
  void goToBrowser();
  void goToReader(std::string path, bool allowFastInitialRefresh = false,
                  ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default);
  void goToReader(std::string path, ClippingJumpResult clippingJump,
                  ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default);
  void goToReader(std::string path, SavedBookmarkJumpResult bookmarkJump,
                  ReaderOpenOrigin openOrigin = ReaderOpenOrigin::Default);
  void goToSleep();
  void goToBoot(bool minimalWakeScreen = false);
  void goToFullScreenMessage(std::string message, EpdFontFamily::Style style = EpdFontFamily::REGULAR);
  void goToCrashReport();
  void goHome(HomeMenuItem initialMenuItem = HomeMenuItem::NONE);
  // Wraps the already-selected normal startup destination without changing
  // recovery/crash routing. No-op when there is nothing eligible to import.
  void maybeOfferVCodexStatsImport();

  void captureYourBooksReturnContext(uint8_t tab, size_t selectedIndex, std::string selectedPath,
                                     std::string searchQuery = {});
  void captureSavedClippingsReturnContext(size_t selectedIndex, std::string selectedPath);
  bool hasYourBooksReturnContext() const { return yourBooksReturnState.has_value(); }
  bool hasSavedClippingsReturnContext() const { return savedClippingsReturnState.has_value(); }
  void returnFromReaderOrHome();

  // This will move current activity to stack instead of deleting it
  void pushActivity(std::unique_ptr<Activity>&& activity);

  // Remove the currentActivity, returning the last one on stack
  // Note: if popActivity() on last activity on the stack, we will goHome()
  void popActivity();

  bool preventAutoSleep() const;
  bool handleGlobalShortcut(GlobalShortcut shortcut);
  bool handleReaderShortcut(uint8_t function);
  // Called only by an Activity that explicitly opts into global navigation.
  bool handleSafeGlobalShortcut(GlobalShortcut shortcut);
  // True only when the currently visible activity is a page-turn reader.
  // A paused reader below a modal does not qualify.
  bool isReaderActivity() const;
  // True when the current activity or a paused activity below it is a reader.
  // Sleep cannot restore the modal stack, but it must still resume its book.
  bool hasReaderActivity() const;
  uint32_t getCompletedRenderGeneration() const { return completedRenderGeneration.load(std::memory_order_acquire); }
  bool handleForcedRefresh();
  bool skipLoopDelay() const;
  ScreenshotInfo getScreenshotInfo() const;

  // If immediate is true, the update will be triggered immediately.
  // Otherwise, it will be deferred until the end of the current loop iteration.
  void requestUpdate(bool immediate = false);

  // Trigger a render and block until it completes.
  // Must NOT be called from the render task or while holding a RenderLock.
  void requestUpdateAndWait();
};

extern ActivityManager activityManager;  // singleton, to be defined in main.cpp
