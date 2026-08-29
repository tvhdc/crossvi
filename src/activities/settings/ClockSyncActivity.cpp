#include "ClockSyncActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ClockSyncPolicy.h"
#include "util/WifiLifecycle.h"

void ClockSyncActivity::onEnter() {
  Activity::onEnter();
  state = SYNCING;
  syncedTime[0] = '\0';

  if (WiFi.status() == WL_CONNECTED) {
    requestUpdate();
    return;
  }

  wifiSelectionAutoSyncExpected =
      ClockSyncPolicy::shouldSyncFromNetwork(SETTINGS.clockHasBeenSynced, halClock.isSystemTimeValid());
  shouldTearDownWifiOnExit = true;
  launchWifiSelection();
}

void ClockSyncActivity::onExit() {
  Activity::onExit();

  if (shouldTearDownWifiOnExit && !WifiLifecycle::shutDown()) {
    silentRestart();
  }
}

void ClockSyncActivity::launchWifiSelection() {
  LOG_INF("CLK", "Manual sync requested without WiFi, launching WiFi selection");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void ClockSyncActivity::onWifiSelectionComplete(const bool connected) {
  if (!connected) {
    LOG_INF("CLK", "WiFi selection cancelled before manual clock sync");
    finish();
    return;
  }

  // WifiSelectionActivity performs the normal once-per-boot-validity NTP sync
  // immediately after connecting. Reuse that successful result instead of
  // issuing the same blocking NTP request again from the manual sync screen.
  if (wifiSelectionAutoSyncExpected && SETTINGS.clockHasBeenSynced && halClock.isSystemTimeValid()) {
    LOG_INF("CLK", "Reusing clock sync completed while WiFi was connecting");
    showSyncSuccess();
    return;
  }

  state = SYNCING;
  requestUpdate();
}

void ClockSyncActivity::showSyncSuccess() {
  // Read the freshly synced time back for the user-facing confirmation.
  char buf[9];
  if (halClock.formatTime(buf, sizeof(buf), SETTINGS.clockUtcOffsetQ, SETTINGS.clockFormat == 1)) {
    snprintf(syncedTime, sizeof(syncedTime), "%s", buf);
  }
  state = SUCCESS;
  requestUpdate();
}

void ClockSyncActivity::runSync() {
  if (WiFi.status() != WL_CONNECTED) {
    LOG_INF("CLK", "Manual sync requested but WiFi is not connected after selection");
    state = NO_WIFI;
    requestUpdate();
    return;
  }

  const bool ok = halClock.syncFromNTP();
  if (!ok) {
    state = FAILED;
    requestUpdate();
    return;
  }

  // Mark as synced so the auto-sync hook stops firing on future WiFi connects.
  if (!ClockSyncPolicy::markSynced(SETTINGS)) LOG_ERR("CLK", "Failed to persist successful clock sync");

  showSyncSuccess();
}

void ClockSyncActivity::loop() {
  if (state == SYNCING) {
    // onEnter(), or ActivityManager after the WiFi picker returns, has already
    // queued the syncing frame. Wait for that render instead of scheduling an
    // identical second panel refresh before the blocking NTP request.
    if (activityManager.hasPendingRender()) return;
    runSync();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void ClockSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_CLOCK_SYNC));

  const int midY = pageHeight / 2;

  switch (state) {
    case SYNCING:
      renderer.drawCenteredText(UI_12_FONT_ID, midY, tr(STR_CLOCK_SYNCING));
      break;
    case SUCCESS: {
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_OK), true, EpdFontFamily::BOLD);
      if (syncedTime[0] != '\0') {
        char line[64];
        snprintf(line, sizeof(line), "%s %s", tr(STR_CURRENT_TIME), syncedTime);
        renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);
      }
      break;
    }
    case NO_WIFI:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_NO_WIFI), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CLOCK_SYNC_NO_WIFI_HINT));
      break;
    case FAILED:
      renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_CLOCK_SYNC_FAIL), true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, tr(STR_CHECK_SERIAL_OUTPUT));
      break;
  }

  if (state != SYNCING) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
