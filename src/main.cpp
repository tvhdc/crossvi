#include <Arduino.h>
#include <BootRecovery.h>
#include <Epub.h>
#include <FontCacheManager.h>
#include <FontDecompressor.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <I18n.h>
#include <Logging.h>
#ifndef SIMULATOR
#include <Preferences.h>
#endif
#include <SPI.h>
#include <Version.h>
#include <WiFi.h>
#include <builtinFonts/all.h>
#ifndef SIMULATOR
#include <driver/gpio.h>
#include <esp_ota_ops.h>
#include <esp_sleep.h>
#include <esp_system.h>
#endif

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "OpdsServerStore.h"
#include "RecentBooksStore.h"
#include "SdCardFontSystem.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/boot_sleep/SafeBootActivity.h"
#include "activities/boot_sleep/SleepFrameStore.h"
#include "activities/settings/SdFirmwareUpdateActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/DefaultSleepScreens.h"
#include "images/LoadingIcon.h"
#include "images/Logo120.h"
#include "util/ButtonNavigator.h"
#include "util/PowerButtonGesture.h"
#include "util/ScreenshotUtil.h"

GfxRenderer renderer(display);
MappedInputManager mappedInputManager(gpio, renderer);
ActivityManager activityManager(renderer, mappedInputManager);
FontDecompressor fontDecompressor;
SdCardFontSystem sdFontSystem;
FontCacheManager fontCacheManager(renderer.getFontMap(), renderer.getSdCardFonts());
static unsigned long allowSleepAt = 0;
static PowerButtonGesture powerButtonGesture;

// Fonts
EpdFont notoserif14RegularFont(&notoserif_14_regular);
EpdFont notoserif14BoldFont(&notoserif_14_bold);
EpdFont notoserif14ItalicFont(&notoserif_14_italic);
EpdFont notoserif14BoldItalicFont(&notoserif_14_bolditalic);
EpdFontFamily notoserif14FontFamily(&notoserif14RegularFont, &notoserif14BoldFont, &notoserif14ItalicFont,
                                    &notoserif14BoldItalicFont);
#ifndef OMIT_FONTS
EpdFont notoserif12RegularFont(&notoserif_12_regular);
EpdFont notoserif12BoldFont(&notoserif_12_bold);
EpdFont notoserif12ItalicFont(&notoserif_12_italic);
EpdFont notoserif12BoldItalicFont(&notoserif_12_bolditalic);
EpdFontFamily notoserif12FontFamily(&notoserif12RegularFont, &notoserif12BoldFont, &notoserif12ItalicFont,
                                    &notoserif12BoldItalicFont);
EpdFont notoserif16RegularFont(&notoserif_16_regular);
EpdFont notoserif16BoldFont(&notoserif_16_bold);
EpdFont notoserif16ItalicFont(&notoserif_16_italic);
EpdFont notoserif16BoldItalicFont(&notoserif_16_bolditalic);
EpdFontFamily notoserif16FontFamily(&notoserif16RegularFont, &notoserif16BoldFont, &notoserif16ItalicFont,
                                    &notoserif16BoldItalicFont);
EpdFont notoserif18RegularFont(&notoserif_18_regular);
EpdFont notoserif18BoldFont(&notoserif_18_bold);
EpdFont notoserif18ItalicFont(&notoserif_18_italic);
EpdFont notoserif18BoldItalicFont(&notoserif_18_bolditalic);
EpdFontFamily notoserif18FontFamily(&notoserif18RegularFont, &notoserif18BoldFont, &notoserif18ItalicFont,
                                    &notoserif18BoldItalicFont);

#endif  // OMIT_FONTS

EpdFont smallFont(&notosans_8_regular);
EpdFontFamily smallFontFamily(&smallFont);

EpdFont ui10RegularFont(&ubuntu_10_regular);
EpdFont ui10BoldFont(&ubuntu_10_bold);
EpdFontFamily ui10FontFamily(&ui10RegularFont, &ui10BoldFont);

EpdFont ui12RegularFont(&ubuntu_12_regular);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFontFamily ui12FontFamily(&ui12RegularFont, &ui12BoldFont);

// measurement of power button press duration calibration value
unsigned long t1 = 0;
unsigned long t2 = 0;

// Definitions for SilentRestart.h. RTC_NOINIT survives ESP.restart() but not power loss.
RTC_NOINIT_ATTR uint32_t silentRebootMagic;
RTC_NOINIT_ATTR uint32_t silentRebootTarget;
constexpr uint32_t SILENT_REBOOT_MAGIC = 0xC1EAB007;
constexpr uint32_t SILENT_REBOOT_TARGET_HOME = 0;
constexpr uint32_t SILENT_REBOOT_TARGET_READER = 1;
// UP/DOWN must already be held with POWER at boot. InputManager debounces in
// 5 ms; 40 ms gives several stable samples without adding the previous fixed
// half-second delay to every normal wake.
constexpr unsigned long BOOT_CHORD_SETTLE_MS = 40;
// A normal power-button wake deliberately pays for the strongest X3 cleanup.
// Other controllers ignore the extra conditioning count but still honor the
// explicit full refresh used by the boot/resume frame.
constexpr uint8_t WAKE_CONDITION_PASSES = 1;

// How the device is coming back to life, resolved once at boot. Both resume
// flows suppress the splash and retain a matching saved panel frame; a plain
// boot shows the splash. See setup() for the resolution.
enum class BootResume : uint8_t {
  Splash,     // cold boot, flash, panic, or plain reboot
  Silent,     // heap-defrag ESP.restart() (RTC flag; lost on power loss)
  SavedFrame  // wake from deep sleep with a validated SD framebuffer
};

constexpr bool canRestoreSavedSleepFrame(const bool powerWake, const bool frameArmed, const bool frameReady,
                                         const bool recoveryBlocked) {
  return powerWake && frameArmed && frameReady && !recoveryBlocked;
}

static_assert(canRestoreSavedSleepFrame(true, true, true, false));
static_assert(!canRestoreSavedSleepFrame(false, true, true, false));
static_assert(!canRestoreSavedSleepFrame(true, true, true, true));

// Latched true once enterDeepSleep() commits to sleeping, before it tears down
// the current activity. WiFi activities call silentRestart() in onExit() to
// clear heap fragmentation on the way out, but deep sleep is a full chip reset
// on wake and already clears the heap, so rebooting here would just power the
// device back up against the user's sleep gesture. Never cleared:
// startDeepSleep() does not return, so a set latch only ends at the wakeup reset.
static bool deepSleepInProgress = false;

void silentRestart() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_HOME;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=home)");
  // E-ink retains the previous frame until Home's first paint lands (~2-3s).
  // Without an overlay, users don't see the reboot and fire input through to
  // Home. Select on the default selectorIndex=0 then opens the most-recent
  // book, looking like a trampoline back to the reader they just exited.
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void silentRestartToReader() {
  if (deepSleepInProgress) return;  // sleeping supersedes the heap-defrag reboot
  silentRebootTarget = SILENT_REBOOT_TARGET_READER;
  silentRebootMagic = SILENT_REBOOT_MAGIC;
  LOG_DBG("MAIN", "Silent restart (target=reader)");
  GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  delay(50);
  ESP.restart();
}

void waitForPowerRelease() {
  gpio.update();
  while (gpio.isPressed(HalGPIO::BTN_POWER)) {
    delay(50);
    gpio.update();
  }
}

constexpr char WAKE_NVS_NAMESPACE[] = "crosspoint";
constexpr char WAKE_SHORT_PRESS_KEY[] = "wakeShortPr";

static bool readWakeShortPressFromNvs() {
#ifdef SIMULATOR
  return false;
#else
  Preferences preferences;
  if (!preferences.begin(WAKE_NVS_NAMESPACE, true)) return false;
  const bool shortPressWakes = preferences.getBool(WAKE_SHORT_PRESS_KEY, false);
  preferences.end();
  return shortPressWakes;
#endif
}

static void mirrorWakeShortPressToNvs() {
#ifndef SIMULATOR
  const bool shortPressWakes = SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP;
  Preferences preferences;
  if (!preferences.begin(WAKE_NVS_NAMESPACE, false)) return;
  if (preferences.getBool(WAKE_SHORT_PRESS_KEY, false) != shortPressWakes) {
    preferences.putBool(WAKE_SHORT_PRESS_KEY, shortPressWakes);
  }
  preferences.end();
#endif
}

static void drawBundledDefaultSleepScreen() {
  renderer.clearScreen();
  const int pageWidth = renderer.getScreenWidth();
  const int pageHeight = renderer.getScreenHeight();
  if (pageWidth == DEFAULT_SLEEP_X3_WIDTH && pageHeight == DEFAULT_SLEEP_X3_HEIGHT) {
    drawBundledDefaultScreen(renderer, DEFAULT_SLEEP_X3_WIDTH, DEFAULT_SLEEP_X3_HEIGHT, DefaultSleepX3Rows,
                             DefaultSleepX3Runs);
  } else if (pageWidth == DEFAULT_SLEEP_X4_WIDTH && pageHeight == DEFAULT_SLEEP_X4_HEIGHT) {
    drawBundledDefaultScreen(renderer, DEFAULT_SLEEP_X4_WIDTH, DEFAULT_SLEEP_X4_HEIGHT, DefaultSleepX4Rows,
                             DefaultSleepX4Runs);
  } else {
    renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
  }
}

// Some wake checks intentionally return to deep sleep before the activity and
// font systems are initialized. Even those early exits must leave the panel in
// a clean, powered-down state instead of cutting power behind a stale frame.
void enterStartupDeepSleep(const bool tryRestoreSleepFrame) {
  constexpr uint8_t STARTUP_SLEEP_CONDITION_PASSES = 2;
  constexpr bool TURN_OFF_SCREEN_AFTER_REFRESH = true;

  const unsigned long startedAt = millis();
  LOG_INF("SLW", "startup-sleep begin restore_requested=%u", static_cast<unsigned>(tryRestoreSleepFrame));
  display.begin(false);
  renderer.begin();
  const bool quickResumeFrame =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  const bool canRestoreFrame = tryRestoreSleepFrame && !APP_STATE.showBootScreen && !quickResumeFrame &&
                               SleepFrameStore::ready(gpio.deviceIsX3());
  if (!canRestoreFrame || !SleepFrameStore::load(display, false)) {
    drawBundledDefaultSleepScreen();
  }
  display.requestResync(STARTUP_SLEEP_CONDITION_PASSES);
  display.triggerDisplay(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_REFRESH);
  display.deepSleep();
  LOG_INF("SLW", "startup-sleep panel parked elapsed_ms=%lu", millis() - startedAt);
  powerManager.startDeepSleep(gpio);
}

// Enter deep sleep mode
void enterDeepSleep() {
  const unsigned long sleepStartedAt = millis();
  HalPowerManager::Lock powerLock;  // Ensure we are at normal CPU frequency for sleep preparation
  APP_STATE.lastSleepFromReader = activityManager.hasReaderActivity();

  const bool rendersLastScreen =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  bool savedWakeFrame = false;
  LOG_INF("SLW", "sleep-request screen=%u quick_resume=%u from_reader=%u", static_cast<unsigned>(SETTINGS.sleepScreen),
          static_cast<unsigned>(rendersLastScreen), static_cast<unsigned>(APP_STATE.lastSleepFromReader));

  // Detect a removed card once up front so all subsequent writes fail
  // immediately. Last-screen mode must preserve the outgoing frame before the
  // moon marker is drawn; normal sleep modes are captured after their refresh
  // has started so the saved controller baseline matches the frame being sent.
  {
    const unsigned long frameStartedAt = millis();
    RenderLock lock;
    Storage.probeMedia();
    if (rendersLastScreen) savedWakeFrame = SleepFrameStore::save(renderer);
    LOG_INF("SLW", "sleep pre-render frame_saved=%u elapsed_ms=%lu", static_cast<unsigned>(savedWakeFrame),
            millis() - frameStartedAt);
  }

  // Commit to sleeping before goToSleep() runs the outgoing activity's onExit():
  // a WiFi activity would otherwise silentRestart() here and reboot instead.
  deepSleepInProgress = true;
  const unsigned long activitySleepStartedAt = millis();
  LOG_INF("SLW", "sleep activity transition begin");
  const bool wakeFrameReplayable = activityManager.goToSleep();
  LOG_INF("SLW", "sleep activity transition complete elapsed_ms=%lu", millis() - activitySleepStartedAt);

  {
    const unsigned long persistenceStartedAt = millis();
    RenderLock lock;
    if (!rendersLastScreen && wakeFrameReplayable) savedWakeFrame = SleepFrameStore::save(renderer);
    APP_STATE.showBootScreen = !savedWakeFrame;
    if (!APP_STATE.saveToFile()) {
      // Never advertise a frame that was not durably paired with its one-shot
      // state. A normal splash is slower but cannot diff against stale pixels.
      SleepFrameStore::discard();
      APP_STATE.showBootScreen = true;
    }
    LOG_INF("SLW", "sleep persistence complete frame_saved=%u show_boot=%u elapsed_ms=%lu",
            static_cast<unsigned>(savedWakeFrame), static_cast<unsigned>(APP_STATE.showBootScreen),
            millis() - persistenceStartedAt);
  }

  // Tear down WiFi so the modem power domain isn't held alive across deep sleep.
  // Wake from deep sleep is effectively a chip reset, so no state needs to survive.
  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  halTiltSensor.deepSleep();
  const unsigned long panelSleepStartedAt = millis();
  LOG_INF("SLW", "sleep controller shutdown begin total_elapsed_ms=%lu", millis() - sleepStartedAt);
  display.deepSleep();
  LOG_INF("SLW", "sleep controller shutdown complete elapsed_ms=%lu", millis() - panelSleepStartedAt);
  mirrorWakeShortPressToNvs();
  LOG_INF("SLW", "deep-sleep entry total_elapsed_ms=%lu", millis() - sleepStartedAt);

  powerManager.startDeepSleep(gpio);
}

void setupDisplayAndFonts(bool seamless = false) {
  display.begin(seamless);
  renderer.begin();
  activityManager.begin();
  LOG_DBG("MAIN", "Display initialized");

  // Initialize font decompressor for compressed reader fonts
  if (!fontDecompressor.init()) {
    LOG_ERR("MAIN", "Font decompressor init failed");
  }
  fontCacheManager.setFontDecompressor(&fontDecompressor);
  renderer.setFontCacheManager(&fontCacheManager);
  renderer.insertFont(NOTOSERIF_14_FONT_ID, notoserif14FontFamily);
#ifndef OMIT_FONTS
  renderer.insertFont(NOTOSERIF_12_FONT_ID, notoserif12FontFamily);
  renderer.insertFont(NOTOSERIF_16_FONT_ID, notoserif16FontFamily);
  renderer.insertFont(NOTOSERIF_18_FONT_ID, notoserif18FontFamily);

#endif  // OMIT_FONTS
  renderer.insertFont(UI_10_FONT_ID, ui10FontFamily);
  renderer.insertFont(UI_12_FONT_ID, ui12FontFamily);
  renderer.insertFont(SMALL_FONT_ID, smallFontFamily);

  LOG_DBG("MAIN", "Built-in fonts setup");
}

void setup() {
#ifndef SIMULATOR
  // Keep battery-latched boards alive before the power button is released.
  // Runtime detection calls this again after selecting the final board profile.
  BoardConfig::holdPowerRails();
#endif
  t1 = millis();

#ifdef ENABLE_SERIAL_LOG
  // Earliest possible Serial setup. The 250 ms stall before begin() lets the
  // USB Serial/JTAG peripheral finish power-on and lets the host complete USB
  // enumeration before we touch the CDC state — otherwise cold boot races
  // and the host has to be physically replugged for logs to flow. Warm reboot
  // worked without the delay because USB was already enumerated.
  delay(250);
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  logSerial.setTxTimeoutMs(1);  // This is a load-bearing 1. Do not modify.
#endif
#endif

  HalSystem::begin();
#ifndef SIMULATOR
  LOG_INF("SLW", "wake-entry serial_ms=%lu reset_reason=%u raw_wakeup=%u", millis(),
          static_cast<unsigned>(esp_reset_reason()), static_cast<unsigned>(esp_sleep_get_wakeup_cause()));
#else
  LOG_INF("SLW", "wake-entry simulator serial_ms=%lu", millis());
#endif

  // Read-and-clear so a panic later in setup() doesn't loop into silent reboot.
  // Bound the target range too — RTC_NOINIT memory is uninitialized on cold boot.
  const bool isSilentReboot = (silentRebootMagic == SILENT_REBOOT_MAGIC);
  const uint32_t snapshotTarget =
      (isSilentReboot && silentRebootTarget <= SILENT_REBOOT_TARGET_READER) ? silentRebootTarget : 0;
  silentRebootMagic = 0;
  silentRebootTarget = 0;

  gpio.begin();
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  LOG_INF("MAIN", "Hardware detect: %s", gpio.deviceIsX3() ? "X3" : "X4");
  LOG_INF("SLW", "wake hardware ready model=%s elapsed_ms=%lu", gpio.deviceIsX3() ? "X3" : "X4", millis());

  const auto wakeupReason = gpio.getWakeupReason();
  LOG_INF("SLW", "wake logical_reason=%u", static_cast<unsigned>(wakeupReason));
  bool recoveryFirmwareMode = false;
  bool manualSafeBoot = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    const unsigned long settleStart = millis();
    while (millis() - settleStart < BOOT_CHORD_SETTLE_MS) {
      gpio.update();
      delay(10);
    }
    // UP keeps priority because it is the established firmware-recovery chord.
    recoveryFirmwareMode = gpio.isPressed(HalGPIO::BTN_UP);
    manualSafeBoot = !recoveryFirmwareMode && gpio.isPressed(HalGPIO::BTN_DOWN);
    if (recoveryFirmwareMode) {
      LOG_INF("MAIN", "Recovery firmware mode (UP + POWER held at boot)");
    } else if (manualSafeBoot) {
      LOG_INF("MAIN", "Manual safe startup (DOWN + POWER held at boot)");
    }
  }
  BootRecovery::begin(manualSafeBoot);

  // Verify the wake gesture before SD mount/settings I/O. The only setting
  // needed here is mirrored into NVS when settings load and again at sleep.
  // X3 treats the PowerButton wake cause itself as a deliberate one-tap wake.
  // X4 keeps the configured continuous-hold check so a later second tap cannot
  // be mistaken for the press that originally powered the device.
  bool powerWakeRejected = false;
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton) {
    const bool shortPressWakes = gpio.deviceIsX3() || readWakeShortPressFromNvs();
    const uint16_t requiredDuration = shortPressWakes ? CrossPointSettings::POWER_BUTTON_WAKE_SHORT_MS
                                                      : CrossPointSettings::POWER_BUTTON_WAKE_LONG_MS;
    LOG_DBG("MAIN", "Verifying power button press duration (%u ms)", requiredDuration);
    powerWakeRejected = !gpio.verifyPowerButtonWakeup(requiredDuration, shortPressWakes);
    LOG_INF("SLW", "wake power validation short=%u required_ms=%u rejected=%u", static_cast<unsigned>(shortPressWakes),
            static_cast<unsigned>(requiredDuration), static_cast<unsigned>(powerWakeRejected));
  }

  // SD Card Initialization
  // We need 6 open files concurrently when parsing a new chapter
  const unsigned long storageStartedAt = millis();
  const bool storageReady = Storage.begin();
  LOG_INF("SLW", "wake SD mount ready=%u elapsed_ms=%lu", static_cast<unsigned>(storageReady),
          millis() - storageStartedAt);
  if (!storageReady) {
    if (powerWakeRejected) {
      enterStartupDeepSleep(true);
      return;
    }
    LOG_ERR("MAIN", "SD card initialization failed");
    setupDisplayAndFonts(false);
    activityManager.goToFullScreenMessage("SD card error", EpdFontFamily::BOLD);
    return;
  }

  HalSystem::checkPanic();

  if (BootRecovery::shouldSkip(BootStage::Settings)) {
    SETTINGS.markReadOnlyForRecovery();
  } else {
    BootRecovery::StageGuard stage(BootStage::Settings);
    SETTINGS.loadFromFile();
  }
  if (SETTINGS.deviceDisplayName[0] == '\0') {
    const char* defaultDeviceName = gpio.deviceIsX3() ? "Xteink X3" : "Xteink X4";
    std::strncpy(SETTINGS.deviceDisplayName, defaultDeviceName, sizeof(SETTINGS.deviceDisplayName) - 1);
    SETTINGS.deviceDisplayName[sizeof(SETTINGS.deviceDisplayName) - 1] = '\0';
    // Safe-start may intentionally make settings read-only; the in-memory
    // default still works and a later normal boot can persist it.
    SETTINGS.saveToFile();
  }
  if (BootRecovery::shouldSkip(BootStage::AppState)) {
    APP_STATE.markReadOnlyForRecovery();
  } else {
    BootRecovery::StageGuard stage(BootStage::AppState);
    APP_STATE.loadFromFile();
  }
  LOG_INF("SLW", "wake settings/state ready show_boot=%u last_reader=%u open_path=%u elapsed_ms=%lu",
          static_cast<unsigned>(APP_STATE.showBootScreen), static_cast<unsigned>(APP_STATE.lastSleepFromReader),
          static_cast<unsigned>(!APP_STATE.openEpubPath.empty()), millis());
  mirrorWakeShortPressToNvs();
  if (powerWakeRejected) {
    enterStartupDeepSleep(true);
    return;
  }
  I18N.setLanguage(static_cast<Language>(SETTINGS.language));
  ButtonNavigator::setMappedInputManager(mappedInputManager);

  switch (wakeupReason) {
    case HalGPIO::WakeupReason::AfterUSBPower:
      // If USB power caused a cold boot, go back to sleep
      LOG_DBG("MAIN", "Wakeup reason: After USB Power");
      enterStartupDeepSleep(false);
      return;
    case HalGPIO::WakeupReason::PowerButton:
    case HalGPIO::WakeupReason::AfterFlash:
      // After flashing, just proceed to boot
    case HalGPIO::WakeupReason::Other:
    default:
      break;
  }

  // First serial output only here to avoid timing inconsistencies for power button press duration verification
  LOG_DBG("MAIN", "Starting CrossVi version %s", CROSSPOINT_VERSION);

  // A saved frame is valid only for a normal power-button wake. Flashing,
  // panic/recovery and silent-reboot paths deliberately retain their existing
  // clean presentation and must never expose an old sleep frame.
  const bool recoveryBlocksSavedFrame =
      isSilentReboot || recoveryFirmwareMode || BootRecovery::active() || HalSystem::isRebootFromPanic();
  const bool frameReady =
      !recoveryBlocksSavedFrame && !APP_STATE.showBootScreen && SleepFrameStore::ready(gpio.deviceIsX3());
  const bool canRestoreSavedFrame =
      canRestoreSavedSleepFrame(wakeupReason == HalGPIO::WakeupReason::PowerButton, !APP_STATE.showBootScreen,
                                frameReady, recoveryBlocksSavedFrame);
  const BootResume resume = BootRecovery::active() ? BootResume::Splash
                            : isSilentReboot       ? BootResume::Silent
                            : canRestoreSavedFrame ? BootResume::SavedFrame
                                                   : BootResume::Splash;
  const bool quickResumeWake =
      resume == BootResume::SavedFrame &&
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  LOG_INF("SLW", "wake resume decision mode=%u frame_ready=%u quick=%u recovery_block=%u",
          static_cast<unsigned>(resume), static_cast<unsigned>(frameReady), static_cast<unsigned>(quickResumeWake),
          static_cast<unsigned>(recoveryBlocksSavedFrame));
  if (resume != BootResume::SavedFrame) {
    SleepFrameStore::discard();
    if (!APP_STATE.showBootScreen) {
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
    }
  }
  const bool minimalBlankWake =
      wakeupReason == HalGPIO::WakeupReason::PowerButton &&
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::BLANK &&
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_NEVER &&
      !recoveryFirmwareMode && !manualSafeBoot && !BootRecovery::active();
  bool allowFastInitialReaderRefresh = false;

  // Only a validated saved frame retains the physical panel baseline. A silent
  // reboot (for example when leaving WiFi) still establishes a clean frame.
  // Quick Resume is the sole wake path that intentionally keeps the saved
  // baseline and skips the strong full-panel cleanup.
  const unsigned long displaySetupStartedAt = millis();
  LOG_INF("SLW", "wake display init begin seamless=%u", static_cast<unsigned>(resume == BootResume::SavedFrame));
  setupDisplayAndFonts(resume == BootResume::SavedFrame);
  LOG_INF("SLW", "wake display init complete elapsed_ms=%lu", millis() - displaySetupStartedAt);
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton && !quickResumeWake) {
    display.requestResync(WAKE_CONDITION_PASSES);
    LOG_INF("SLW", "wake strong resync armed passes=%u", static_cast<unsigned>(WAKE_CONDITION_PASSES));
  }

  switch (resume) {
    case BootResume::Silent:
      activityManager.goToBoot(false);
      break;
    case BootResume::SavedFrame:
      // One-shot flag: re-arm the splash for the next boot. Save before any
      // painting so a hang cannot strand us in a resume-with-no-frame loop.
      APP_STATE.showBootScreen = true;
      APP_STATE.saveToFile();
      if (SleepFrameStore::load(display)) {
        const auto pageHeight = renderer.getScreenHeight();
        renderer.drawImage(LoadingIcon, 0, pageHeight - LOADINGICON_HEIGHT, LOADINGICON_WIDTH, LOADINGICON_HEIGHT);
        // Normal wake visibly cleans the complete panel before partial updates
        // resume. Quick Resume deliberately keeps its seamless saved-frame
        // paint and lets the first reader frame stay fast on both devices.
        const unsigned long wakeRefreshStartedAt = millis();
        LOG_INF("SLW", "wake saved-frame refresh begin mode=%s", quickResumeWake ? "fast" : "full");
        renderer.displayBuffer(quickResumeWake ? HalDisplay::FAST_REFRESH : HalDisplay::FULL_REFRESH);
        LOG_INF("SLW", "wake saved-frame refresh complete elapsed_ms=%lu", millis() - wakeRefreshStartedAt);
        allowFastInitialReaderRefresh = quickResumeWake || gpio.deviceIsX3();
      } else {
        display.requestResync(1);
        activityManager.goToBoot(minimalBlankWake);  // frame file missing, fall back to the splash
      }
      break;
    case BootResume::Splash:
      activityManager.goToBoot(minimalBlankWake);
      break;
  }

  // The panel now contains a stable boot/resume frame. Load optional startup
  // data afterwards so slow SD access or font discovery cannot leave an old
  // WiFi/sleep frame progressively darkening on the powered panel.
  LOG_DBG("MAIN", "Initial boot frame ready at %lu ms", millis());
  LOG_INF("SLW", "wake initial frame ready resume=%u elapsed_ms=%lu", static_cast<unsigned>(resume), millis());
  const bool deferReaderStores = resume == BootResume::SavedFrame && !APP_STATE.openEpubPath.empty() &&
                                 APP_STATE.lastSleepFromReader && APP_STATE.readerActivityLoadCount == 0 &&
                                 !mappedInputManager.isPressed(MappedInputManager::Button::Back) &&
                                 !HalSystem::isRebootFromPanic();
  if (BootRecovery::shouldSkip(BootStage::RecentBooks)) {
    RECENT_BOOKS.markReadOnlyForRecovery();
  } else if (!deferReaderStores) {
    BootRecovery::StageGuard stage(BootStage::RecentBooks);
    RECENT_BOOKS.loadFromFile();
  }
  if (BootRecovery::shouldSkip(BootStage::KOReader)) {
    KOREADER_STORE.markReadOnlyForRecovery();
  } else if (!deferReaderStores) {
    BootRecovery::StageGuard stage(BootStage::KOReader);
    KOREADER_STORE.loadFromFile();
  }
  if (BootRecovery::shouldSkip(BootStage::Opds)) {
    OPDS_STORE.markReadOnlyForRecovery();
  } else if (!deferReaderStores) {
    BootRecovery::StageGuard stage(BootStage::Opds);
    OPDS_STORE.loadFromFile();
  }
  if (!BootRecovery::shouldSkip(BootStage::SdFonts)) {
    BootRecovery::StageGuard stage(BootStage::SdFonts);
    sdFontSystem.begin();
  }
  LOG_DBG("MAIN", "Startup stores and SD fonts ready at %lu ms", millis());

  const bool safeStartup = BootRecovery::active();
  const bool manualSafeStartup = BootRecovery::manual();
  BootRecovery::completeStartup();

  if (recoveryFirmwareMode) {
    // Skip normal home/reader routing: jump straight into the SD firmware picker.
    activityManager.replaceActivity(
        std::make_unique<SdFirmwareUpdateActivity>(renderer, mappedInputManager, /*recoveryMode=*/true));
    LOG_INF("SLW", "wake route=recovery-firmware");
  } else if (safeStartup) {
    activityManager.replaceActivity(
        std::make_unique<SafeBootActivity>(renderer, mappedInputManager, manualSafeStartup));
    LOG_INF("SLW", "wake route=safe-boot");
  } else if (HalSystem::isRebootFromPanic()) {
    // If we rebooted from a panic, go to crash report screen to show the panic info
    activityManager.goToCrashReport();
    LOG_INF("SLW", "wake route=crash-report");
  } else if (resume == BootResume::Silent && snapshotTarget == SILENT_REBOOT_TARGET_READER &&
             !APP_STATE.openEpubPath.empty()) {
    activityManager.goToReader(APP_STATE.openEpubPath);
    LOG_INF("SLW", "wake route=silent-reader");
  } else if (resume == BootResume::Silent) {
    // target == home (or reader with no open book): land on home — don't fall
    // through to the sleep-wake "resume reader" logic, which fires on stale
    // openEpubPath + lastSleepFromReader from a prior session.
    activityManager.goHome();
    LOG_INF("SLW", "wake route=silent-home");
  } else if (APP_STATE.openEpubPath.empty() || !APP_STATE.lastSleepFromReader ||
             mappedInputManager.isPressed(MappedInputManager::Button::Back) || APP_STATE.readerActivityLoadCount > 0) {
    // Boot to home screen if no book is open, last sleep was not from reader, back button is held, or reader activity
    // crashed (indicated by readerActivityLoadCount > 0)
    activityManager.goHome();
    LOG_INF("SLW", "wake route=home");
  } else {
    // Clear app state to avoid getting into a boot loop if the epub doesn't load
    const auto path = APP_STATE.openEpubPath;
    APP_STATE.openEpubPath = "";
    APP_STATE.readerActivityLoadCount++;
    APP_STATE.saveToFile();
    activityManager.goToReader(path, allowFastInitialReaderRefresh);
    LOG_INF("SLW", "wake route=reader fast_initial=%u", static_cast<unsigned>(allowFastInitialReaderRefresh));
  }

  // Recovery, Safe Boot and crash-report routes remain authoritative. Only a
  // normal startup destination may be wrapped by the one-time VCodex prompt.
  if (!recoveryFirmwareMode && !safeStartup && !HalSystem::isRebootFromPanic()) {
    activityManager.maybeOfferVCodexStatsImport();
  }

  if (resume == BootResume::Silent) {
    // Silent boot now displays BootActivity early. Apply the pending Home or
    // Reader replacement before waiting for the target frame.
    activityManager.loop();
    // Block until the first paint physically completes. refreshDisplay()
    // waits on the panel BUSY pin so when this returns the user can see the
    // new activity. Without the wait, an edge captured by gpio.update()
    // during boot dispatches against an invisible Home and the default
    // selectorIndex=0 opens the most-recent book.
    activityManager.requestUpdateAndWait();
    // Absorb any button held at this point into currentState as a non-edge:
    // two gpio.update() calls separated by > InputManager's 5ms debounce
    // transition the held bit through lastDebounceTime into currentState
    // without setting pressedEvents, so the first loop()'s own gpio.update()
    // sees state == currentState and emits nothing.
    gpio.update();
    delay(10);
    gpio.update();
  }

  // Ensure we're not still holding the power button before leaving setup
  waitForPowerRelease();
  allowSleepAt = millis() + 2000;
  LOG_INF("SLW", "wake setup complete elapsed_ms=%lu", millis());
}

#ifndef SIMULATOR
// Arduino normally accepts a pending OTA image before setup() runs. Defer that
// decision so a firmware that immediately boot-loops can still roll back.
extern "C" bool verifyRollbackLater() { return true; }

namespace {
void markOtaValidOnceHealthy() {
  static bool done = false;
  static unsigned long lastAttempt = 0;
  const unsigned long now = millis();
  if (done || now < 10000 || (lastAttempt != 0 && now - lastAttempt < 1000)) return;
  lastAttempt = now;

  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  const esp_err_t stateResult = running ? esp_ota_get_state_partition(running, &state) : ESP_ERR_NOT_FOUND;
  if (stateResult != ESP_OK) {
    LOG_ERR("OTA", "Could not read running image state: %s", esp_err_to_name(stateResult));
    return;
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    done = true;
    return;
  }

  const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
  if (result == ESP_OK) {
    done = true;
    LOG_INF("OTA", "Image marked valid after healthy boot");
  } else {
    LOG_ERR("OTA", "Could not mark image valid: %s", esp_err_to_name(result));
  }
}
}  // namespace
#endif

namespace {
constexpr size_t SERIAL_COMMAND_MAX_BYTES = 48;
constexpr size_t SERIAL_BYTES_PER_LOOP = 32;
char serialCommandBuffer[SERIAL_COMMAND_MAX_BYTES + 1]{};
size_t serialCommandLength = 0;
bool discardSerialCommand = false;

void handleSerialCommand(const char* const line, const size_t length) {
  constexpr char PREFIX[] = "CMD:";
  constexpr char SCREENSHOT[] = "SCREENSHOT";
  if (!line || length < sizeof(PREFIX) - 1 || std::memcmp(line, PREFIX, sizeof(PREFIX) - 1) != 0) return;

  size_t begin = sizeof(PREFIX) - 1;
  size_t end = length;
  while (begin < end && static_cast<unsigned char>(line[begin]) <= ' ') ++begin;
  while (end > begin && static_cast<unsigned char>(line[end - 1]) <= ' ') --end;
  if (end - begin != sizeof(SCREENSHOT) - 1 || std::memcmp(line + begin, SCREENSHOT, sizeof(SCREENSHOT) - 1) != 0) {
    return;
  }

  RenderLock lock;
  const uint32_t bufferSize = display.getBufferSize();
  uint8_t* const buf = display.getFrameBuffer();
  if (buf) {
    logSerial.printf("SCREENSHOT_START:%d\n", bufferSize);
    logSerial.write(buf, bufferSize);
    logSerial.printf("SCREENSHOT_END\n");
  } else {
    logSerial.printf("SCREENSHOT_ERROR:BUSY\n");
  }
}

void pumpSerialCommands() {
  size_t processed = 0;
  while (processed < SERIAL_BYTES_PER_LOOP && logSerial.available() > 0) {
    const int value = logSerial.read();
    if (value < 0) break;
    ++processed;
    const char byte = static_cast<char>(value);
    if (byte == '\n') {
      if (!discardSerialCommand) {
        serialCommandBuffer[serialCommandLength] = '\0';
        handleSerialCommand(serialCommandBuffer, serialCommandLength);
      }
      serialCommandLength = 0;
      discardSerialCommand = false;
      continue;
    }
    if (discardSerialCommand) continue;
    if (serialCommandLength == SERIAL_COMMAND_MAX_BYTES) {
      discardSerialCommand = true;
      continue;
    }
    serialCommandBuffer[serialCommandLength++] = byte;
  }
}
}  // namespace

void loop() {
#ifndef SIMULATOR
  markOtaValidOnceHealthy();
#endif
  static unsigned long maxLoopDuration = 0;
  constexpr unsigned long READER_DEBOUNCE_REPOLL_MS = 6;
  const unsigned long loopStartTime = millis();
  static unsigned long lastMemPrint = 0;

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  static uint32_t lastInputPollAtMs = 0;
  const bool readerVisibleBeforeInput = activityManager.isReaderActivity();
  const uint32_t inputPollAtMs = static_cast<uint32_t>(millis());
  const uint32_t inputPollGapMs = inputPollAtMs - lastInputPollAtMs;
#endif
  gpio.update();
  const bool readerVisible = activityManager.isReaderActivity();
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (lastInputPollAtMs != 0 && (readerVisibleBeforeInput || readerVisible) && inputPollGapMs > 25) {
    LOG_DBG("INP", "stage=poll_gap elapsed_ms=%u cpu_mhz=%u debounce=%u pending_render=%u busy=%u",
            static_cast<unsigned>(inputPollGapMs), static_cast<unsigned>(getCpuFrequencyMhz()),
            gpio.isDebouncePending() ? 1U : 0U, activityManager.hasPendingRender() ? 1U : 0U,
            activityManager.skipLoopDelay() ? 1U : 0U);
  }
  lastInputPollAtMs = inputPollAtMs;
#endif
  // A raw reader-button edge is not yet visible to the mapped input layer.
  // Give the 5 ms debouncer its second sample before any SD, pagination, or
  // post-visible work can monopolize the main loop and erase the candidate.
  if (readerVisible && gpio.isDebouncePending()) {
    delay(READER_DEBOUNCE_REPOLL_MS);
    return;
  }
  halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, readerVisible);

  renderer.setFadingFix(SETTINGS.fadingFix);

  const bool doublePowerEnabled =
      SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE &&
      (SETTINGS.doublePowerAction != CrossPointSettings::DOUBLE_POWER_ACTION::DOUBLE_POWER_DISABLED ||
       SETTINGS.doublePowerReadingFunction != CrossPointSettings::LP_MENU_DISABLED);
  mappedInputManager.setPowerReleaseOverride(doublePowerEnabled, false);

  if (Serial && millis() - lastMemPrint >= 10000) {
    LOG_INF("MEM", "Free: %d bytes, Total: %d bytes, Min Free: %d bytes, MaxAlloc: %d bytes", ESP.getFreeHeap(),
            ESP.getHeapSize(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap());
    lastMemPrint = millis();
  }

  // Read a bounded number of bytes without waiting for a newline. Oversized
  // lines are ignored through their terminator so a prefix can never execute.
  pumpSerialCommands();

  // Check for any user activity (button press or release) or active background work
  static unsigned long lastActivityTime = millis();
  if (gpio.wasAnyPressed() || gpio.wasAnyReleased() || halTiltSensor.hadActivity() ||
      activityManager.preventAutoSleep()) {
    lastActivityTime = millis();         // Reset inactivity timer
    powerManager.setPowerSaving(false);  // Restore normal CPU frequency on user activity
  }

  static bool screenshotButtonsReleased = true;
  static bool screenshotComboActive = false;
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.isPressed(HalGPIO::BTN_DOWN)) {
    powerButtonGesture.cancel();
    screenshotComboActive = true;
    if (screenshotButtonsReleased) {
      screenshotButtonsReleased = false;
      {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
      }
    }
    return;
  }
  if (screenshotComboActive) {
    powerButtonGesture.cancel();
    // Keep owning input until both buttons are up. This also consumes a Down
    // release that arrives after Power, so the screenshot chord cannot turn a
    // reader page as a side effect.
    if (gpio.isPressed(HalGPIO::BTN_POWER) || gpio.isPressed(HalGPIO::BTN_DOWN)) return;
    screenshotButtonsReleased = true;
    screenshotComboActive = false;
    return;
  }

  const PowerButtonGesture::Event powerEvent = powerButtonGesture.update(
      static_cast<uint32_t>(millis()), gpio.wasPressed(HalGPIO::BTN_POWER), gpio.wasReleased(HalGPIO::BTN_POWER),
      gpio.isPressed(HalGPIO::BTN_POWER), static_cast<uint32_t>(gpio.getPowerButtonHeldTime()), doublePowerEnabled,
      millis() >= allowSleepAt);
  mappedInputManager.setPowerReleaseOverride(doublePowerEnabled, powerEvent == PowerButtonGesture::Event::Single);

  if (powerEvent == PowerButtonGesture::Event::Hold) {
    if (activityManager.preventAutoSleep()) return;
    enterDeepSleep();
    return;
  }

  if (powerEvent == PowerButtonGesture::Event::Double) {
    if (activityManager.isReaderActivity()) {
      if (SETTINGS.doublePowerReadingFunction == CrossPointSettings::LP_MENU_REFRESH) {
        activityManager.handleGlobalShortcut(GlobalShortcut::RefreshScreen);
      } else if (SETTINGS.doublePowerReadingFunction != CrossPointSettings::LP_MENU_DISABLED) {
        activityManager.handleReaderShortcut(SETTINGS.doublePowerReadingFunction);
      }
      return;
    }
    switch (static_cast<CrossPointSettings::DOUBLE_POWER_ACTION>(SETTINGS.doublePowerAction)) {
      case CrossPointSettings::DOUBLE_POWER_HOME:
        activityManager.handleGlobalShortcut(GlobalShortcut::GoHome);
        break;
      case CrossPointSettings::DOUBLE_POWER_RESUME:
        activityManager.handleGlobalShortcut(GlobalShortcut::ResumeReading);
        break;
      case CrossPointSettings::DOUBLE_POWER_REFRESH:
        activityManager.handleGlobalShortcut(GlobalShortcut::RefreshScreen);
        break;
      case CrossPointSettings::DOUBLE_POWER_SCREENSHOT: {
        RenderLock lock;
        ScreenshotUtil::takeScreenshot(renderer);
        break;
      }
      case CrossPointSettings::DOUBLE_POWER_DISABLED:
      default:
        break;
    }
    return;
  }

  if (powerEvent == PowerButtonGesture::Event::Single &&
      SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) {
    if (activityManager.preventAutoSleep()) return;
    enterDeepSleep();
    return;
  }

  const unsigned long sleepTimeoutMs = SETTINGS.getSleepTimeoutMs();
  if (sleepTimeoutMs > 0 && millis() - lastActivityTime >= sleepTimeoutMs) {
    LOG_DBG("SLP", "Auto-sleep triggered after %lu ms of inactivity", sleepTimeoutMs);
    enterDeepSleep();
    // This should never be hit as `enterDeepSleep` calls esp_deep_sleep_start
    return;
  }

  // Refresh screen when power button is short-pressed with FORCE_REFRESH setting.
  if (SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::FORCE_REFRESH &&
      mappedInputManager.wasReleased(MappedInputManager::Button::Power)) {
    LOG_DBG("MAIN", "Manual screen refresh triggered");
    if (!activityManager.handleForcedRefresh()) {
      RenderLock lock;
      renderer.displayBuffer(HalDisplay::HALF_REFRESH);
    }
  }

  // Refresh the battery icon when USB is plugged or unplugged.
  // Placed after sleep guards so we never queue a render that won't be processed.
  if (gpio.wasUsbStateChanged()) {
    activityManager.requestUpdate();
  }

  const unsigned long activityStartTime = millis();
  activityManager.loop();
  const unsigned long activityDuration = millis() - activityStartTime;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (readerVisible && activityDuration > 25) {
    LOG_DBG("INP", "stage=main_work elapsed_ms=%lu cpu_mhz=%u pending_render=%u busy=%u", activityDuration,
            static_cast<unsigned>(getCpuFrequencyMhz()), activityManager.hasPendingRender() ? 1U : 0U,
            activityManager.skipLoopDelay() ? 1U : 0U);
  }
#endif

  const unsigned long loopDuration = millis() - loopStartTime;
  if (loopDuration > maxLoopDuration) {
    maxLoopDuration = loopDuration;
    if (maxLoopDuration > 50) {
      LOG_DBG("LOOP", "New max loop duration: %lu ms (activity: %lu ms)", maxLoopDuration, activityDuration);
    }
  }

  // Add delay at the end of the loop to prevent tight spinning
  // When an activity requests skip loop delay (e.g., webserver running), use yield() for faster response
  // Otherwise, use longer delay to save power
  const unsigned long responsiveLoopDelay = readerVisible && gpio.isDebouncePending() ? READER_DEBOUNCE_REPOLL_MS : 10;
  if (activityManager.skipLoopDelay()) {
    powerManager.setPowerSaving(false);  // Make sure we're at full performance when skipLoopDelay is requested
    yield();                             // Give FreeRTOS a chance to run tasks, but return immediately
  } else {
    if (millis() - lastActivityTime >= HalPowerManager::IDLE_POWER_SAVING_MS) {
      // If we've been inactive for a while, increase the delay to save power
      powerManager.setPowerSaving(true);  // Lower CPU frequency after extended inactivity
      // A raw edge inside the debounce window needs a second sample promptly;
      // otherwise the 50 ms idle cadence can swallow a short tap entirely.
      delay(gpio.isDebouncePending() || readerVisible ? responsiveLoopDelay : 50);
    } else {
      // Short delay to prevent tight loop while still being responsive
      delay(responsiveLoopDelay);
    }
  }
}
