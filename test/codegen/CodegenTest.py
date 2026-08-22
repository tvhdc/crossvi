#!/usr/bin/env python3

import importlib.util
import json
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS = REPO_ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))

from write_if_changed import write_if_changed


def load_git_branch():
    spec = importlib.util.spec_from_file_location("git_branch", SCRIPTS / "git_branch.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CodegenTest(unittest.TestCase):
    def test_vocabulary_data_is_exact_and_ui_font_covers_pronunciations(self):
        generated = (REPO_ROOT / "src/vocabulary/VocabularyData.generated.h").read_text(encoding="utf-8")
        self.assertIn("inline constexpr size_t ENTRY_COUNT = 3000;", generated)

        pronunciations = []
        entry_pattern = re.compile(
            r'^\s+"(?:\\.|[^"\\])*\\0"\s+"((?:\\.|[^"\\])*)\\0"', re.MULTILINE
        )
        for match in entry_pattern.finditer(generated):
            pronunciations.append(json.loads('"' + match.group(1) + '"'))
        self.assertEqual(len(pronunciations), 3000)

        font = (REPO_ROOT / "lib/EpdFont/builtinFonts/ubuntu_10_regular.h").read_text(encoding="utf-8")
        interval_start = font.index("ubuntu_10_regularIntervals")
        interval_end = font.index("};", interval_start)
        intervals = [
            (int(start, 16), int(end, 16))
            for start, end in re.findall(
                r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),", font[interval_start:interval_end]
            )
        ]
        missing = {
            character
            for pronunciation in pronunciations
            for character in pronunciation
            if not any(start <= ord(character) <= end for start, end in intervals)
        }
        self.assertEqual(missing, set())

    def test_vocabulary_is_explicit_only_and_waits_for_continue(self):
        readers = [
            REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp",
            REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp",
            REPO_ROOT / "src/activities/reader/XtcReaderActivity.cpp",
        ]
        for reader in readers:
            source = reader.read_text(encoding="utf-8")
            self.assertNotIn("VocabularyPromptScheduler", source)
            self.assertNotIn("VocabularyLearningActivity", source)

        activity = (REPO_ROOT / "src/activities/reader/VocabularyLearningActivity.cpp").read_text(encoding="utf-8")
        self.assertNotIn("FEEDBACK_DURATION_MS", activity)
        self.assertNotIn("feedbackStartedAt_", activity)
        self.assertIn("STR_VOCAB_TIME_REMAINING", activity)
        self.assertIn("mappedInput.wasReleased(MappedInputManager::Button::Confirm)", activity)
        self.assertIn("AnswerState::TimedOut", activity)
        self.assertGreaterEqual(activity.count("drawAnswerStateIcon(record.state"), 2)
        self.assertIn("labelWidth + LABEL_GAP", activity)
        self.assertIn("buildAnswerSlotOrder(answerCount_", activity)
        self.assertIn("ReaderUtils::SKIP_HOLD_MS", activity)
        self.assertIn("skipHold_.onRelease() == ReaderUtils::HoldRelease::Short", activity)

        settings = (REPO_ROOT / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
        self.assertNotIn("STR_VOCABULARY_LEARNING", settings)

    def test_home_back_is_always_shortcuts(self):
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("std::make_unique<HomeShortcutsActivity>", home)
        self.assertIn("HomeShortcutsActivity>(renderer, mappedInput, returnMenuItem)", home)
        self.assertNotIn("SETTINGS.homeBackAction", home)

        shortcuts = (REPO_ROOT / "src/activities/home/HomeShortcutsActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("onGoHome(returnMenuItem_);", shortcuts)

        settings = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        self.assertNotIn('"homeBackAction"', settings)
        self.assertNotIn('"backShortToFileBrowser"', settings)

    def test_wake_refresh_is_strong_except_for_valid_quick_resume(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        boot = (REPO_ROOT / "src/activities/boot_sleep/BootActivity.cpp").read_text(encoding="utf-8")
        saved_frame = main[main.index("case BootResume::SavedFrame:") : main.index("case BootResume::Splash:")]
        self.assertIn("const bool quickResumeWake", main)
        self.assertIn("&& !quickResumeWake", main)
        self.assertIn(
            "quickResumeWake ? HalDisplay::FAST_REFRESH : HalDisplay::FULL_REFRESH", saved_frame
        )
        self.assertNotIn("displayGrayscaleBase(HalDisplay::FAST_REFRESH)", saved_frame)
        self.assertIn("display.requestResync(WAKE_CONDITION_PASSES);", main)
        self.assertIn("constexpr uint8_t WAKE_CONDITION_PASSES = 1;", main)
        self.assertEqual(boot.count("renderer.displayBuffer(HalDisplay::FULL_REFRESH);"), 2)

    def test_every_sleep_screen_uses_the_strong_full_panel_refresh(self):
        sleep = (REPO_ROOT / "src/activities/boot_sleep/SleepActivity.cpp").read_text(encoding="utf-8")
        driver = (REPO_ROOT / "freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.cpp").read_text(
            encoding="utf-8"
        )
        helper = sleep[sleep.index("void displayStrongSleepFrame") : sleep.index("void SleepActivity::onEnter")]
        self.assertIn("constexpr uint8_t X3_SLEEP_CONDITION_PASSES = 1;", sleep)
        self.assertNotIn("prepareStrongSleepRefresh();", helper)
        self.assertIn("applySleepGhostingTreatment();", helper)
        self.assertIn("display.displayBuffer(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);", helper)
        self.assertLess(helper.index("applySleepGhostingTreatment();"),
                        helper.index("display.displayBuffer(HalDisplay::FULL_REFRESH"))
        self.assertNotIn("display.triggerDisplay(", helper)
        finish = driver[driver.index("void Uc8253X3Driver::displayFinish") :
                        driver.index("void Uc8253X3Driver::setFastLutFrameCount")]
        self.assertIn("const bool finalSleepParking = turnOff && doFullSync;", finish)
        parking = finish[finish.index("if (finalSleepParking)") : finish.index("if (turnOff)")]
        self.assertIn("bus.cmd(CMD_POWER_OFF);", parking)
        self.assertIn("bus.waitBusy(\" X3_POF\");", parking)
        self.assertIn("return;", parking)
        self.assertNotIn("loadProfiledFastBank", parking)
        self.assertNotIn("triggerRefresh", parking)
        self.assertLess(finish.index("waitRefreshComplete"), finish.index("if (finalSleepParking)"))
        self.assertGreaterEqual(sleep.count("displayStrongSleepFrame();"), 7)
        quick_resume = sleep[sleep.index("if (renderQuickResume)") : sleep.index("switch (SETTINGS.sleepScreen)")]
        self.assertIn("renderLastScreenSleepScreen()", quick_resume)
        self.assertIn("renderTransparentSleepScreen(transparentBaseSaved)", sleep)
        transparent = sleep[
            sleep.index("void SleepActivity::renderTransparentSleepScreen") :
            sleep.index("void SleepActivity::renderBitmapSleepScreen")
        ]
        self.assertIn("findTransparentSleepOverlay()", transparent)
        self.assertIn("renderOverlayImage(overlay.path, renderer)", transparent)
        self.assertIn("SleepFrameStore::load(display, false)", transparent)
        self.assertIn("displayStrongSleepFrame();", transparent)
        self.assertNotIn("displayGrayscaleBase", transparent)
        self.assertNotIn("displayGrayBuffer", transparent)
        last_screen = sleep[sleep.index("void SleepActivity::renderLastScreenSleepScreen") :]
        self.assertIn("displayStrongSleepFrame();", last_screen)
        grayscale = sleep[sleep.index("if (hasGreyscale)") : sleep.index("void SleepActivity::renderCoverSleepScreen")]
        self.assertIn("applySleepGhostingTreatment();", grayscale)
        self.assertIn("prepareStrongSleepRefresh();", grayscale)
        self.assertIn("renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);", grayscale)
        self.assertLess(grayscale.index("applySleepGhostingTreatment();"),
                        grayscale.index("renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);"))
        self.assertGreaterEqual(grayscale.count("renderer.fillRect(statsCard.x"), 2)
        bitmap = sleep[
            sleep.index("void SleepActivity::renderBitmapSleepScreen") :
            sleep.index("void SleepActivity::renderCoverSleepScreen")
        ]
        self.assertIn("drawSleepBookStatsOverlay(renderer, statsSummary)", bitmap)
        self.assertLess(bitmap.index("drawSleepBookStatsOverlay(renderer, statsSummary)"),
                        bitmap.index("displayStrongSleepFrame();"))
        summary = sleep[sleep.index("struct SleepBookSummary") : sleep.index("Rect drawSleepBookStatsOverlay")]
        self.assertIn("std::string chapter;", summary)
        self.assertIn("loadSleepBookPosition(recent, summary);", summary)
        self.assertIn("STR_CHAPTER_PREFIX", sleep)
        self.assertIn("STR_STATS_PROGRESS", sleep)
        self.assertIn("DashboardProgress::fillWidth", sleep)
        self.assertIn("summary.chapter.empty()", sleep)
        modes = sleep[sleep.index("switch (SETTINGS.sleepScreen)") : sleep.index("void SleepActivity::renderReadingCalendarSleepScreen")]
        self.assertIn("SLEEP_SCREEN_MODE::COVER_STATS", modes)
        self.assertIn("renderCoverSleepScreen(true)", modes)
        self.assertIn("SLEEP_SCREEN_MODE::CUSTOM_STATS", modes)
        self.assertIn("renderCustomSleepScreen(true)", modes)
        custom = sleep[
            sleep.index("void SleepActivity::renderCustomSleepScreen") :
            sleep.index("void SleepActivity::renderDefaultSleepScreen")
        ]
        fallback = custom[custom.rindex("if (withBookStats)") :]
        self.assertIn("renderer.clearScreen();", fallback)
        self.assertIn("drawSleepBookStatsOverlay(renderer);", fallback)
        self.assertIn("displayStrongSleepFrame();", fallback)
        self.assertLess(fallback.index("return;"), fallback.index("renderDefaultSleepScreen();"))
        calendar = sleep[
            sleep.index("void SleepActivity::renderReadingCalendarSleepScreen") :
            sleep.index("void SleepActivity::renderCustomSleepScreen")
        ]
        self.assertIn("displayStrongSleepFrame();", calendar)
        self.assertNotIn("displayBuffer", calendar)

    def test_sleep_ghosting_menu_exposes_bounded_pre_refresh_sequences(self):
        sleep = (REPO_ROOT / "src/activities/boot_sleep/SleepActivity.cpp").read_text(encoding="utf-8")
        settings_header = (REPO_ROOT / "src/CrossPointSettings.h").read_text(encoding="utf-8")
        settings = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        submenu = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(encoding="utf-8")

        treatment = sleep[sleep.index("void applySleepGhostingTreatment") :
                          sleep.index("void displayStrongSleepFrame")]
        for mode in (
            "SLEEP_GHOST_FULL_ONLY",
            "SLEEP_GHOST_FAST_FULL",
            "SLEEP_GHOST_FAST_TWICE_FULL",
            "SLEEP_GHOST_FAST_CLEAN_FULL",
            "SLEEP_GHOST_FAST_CLEAN_TWICE_FULL",
            "SLEEP_GHOST_HALF_FULL",
            "SLEEP_GHOST_HALF_TWICE_FULL",
            "SLEEP_GHOST_FULL_TWICE",
            "SLEEP_GHOST_FULL_THREE_TIMES",
        ):
            self.assertIn(mode, settings_header)
            self.assertIn(mode, treatment)
        self.assertIn("sleepGhostingTreatment = SLEEP_GHOST_FAST_CLEAN_FULL", settings_header)
        self.assertIn('"sleepGhostingTreatment"', settings)
        self.assertIn("STR_SLEEP_GHOSTING_TREATMENT", settings)
        self.assertIn("sleepGhostingTreatmentLabels()", settings)
        self.assertIn("STR_SLEEP_GHOSTING_TREATMENT", submenu)
        self.assertIn("sleepGhostingTreatmentLabels()", submenu)
        self.assertIn("if (!display.supportsX3GhostCleanup())", sleep)
        self.assertIn("display.displayBuffer(HalDisplay::HALF_REFRESH, false);", sleep)
        self.assertIn("display.cleanX3GhostingNow()", sleep)
        self.assertNotIn("TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH", treatment)

    def test_transparent_sleep_overlay_is_append_only_and_alpha_preserving(self):
        settings_header = (REPO_ROOT / "src/CrossPointSettings.h").read_text(encoding="utf-8")
        settings = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        submenu = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(encoding="utf-8")
        sleep = (REPO_ROOT / "src/activities/boot_sleep/SleepActivity.cpp").read_text(encoding="utf-8")
        decoder = (REPO_ROOT / "lib/Epub/Epub/converters/PngToFramebufferConverter.cpp").read_text(encoding="utf-8")
        sleep_tool = (REPO_ROOT / "docs/tools/sleep-image-converter/index.html").read_text(encoding="utf-8")
        render_config = (REPO_ROOT / "lib/Epub/Epub/converters/ImageToFramebufferDecoder.h").read_text(
            encoding="utf-8"
        )
        pixel_writer = (REPO_ROOT / "lib/Epub/Epub/converters/DirectPixelWriter.h").read_text(encoding="utf-8")

        enum_block = settings_header[settings_header.index("enum SLEEP_SCREEN_MODE") :
                                     settings_header.index("enum SLEEP_SCREEN_COVER_MODE")]
        self.assertLess(enum_block.index("READING_CALENDAR = 7"), enum_block.index("TRANSPARENT_CUSTOM = 10"))
        self.assertIn("SLEEP_SCREEN_TRANSPARENT = 7", enum_block)
        self.assertIn("case TRANSPARENT_CUSTOM:", enum_block)
        self.assertIn("case SLEEP_SCREEN_TRANSPARENT:", enum_block)
        for source in (settings, submenu):
            self.assertIn("StrId::STR_TRANSPARENT_SLEEP", source)

        self.assertIn('rootOverlayCandidate("/sleep-overlay.bmp")', sleep)
        self.assertIn('rootOverlayCandidate("/sleep-overlay.png")', sleep)
        self.assertIn('directoryOverlayCandidate("/.sleep-overlay")', sleep)
        self.assertIn('directoryOverlayCandidate("/sleep-overlay")', sleep)
        self.assertIn("filename.size() <= 256", sleep)
        self.assertIn("MAX_SLEEP_DIRECTORY_ENTRIES = 4096", sleep)
        self.assertIn("SLEEP_DIRECTORY_YIELD_INTERVAL = 16", sleep)
        self.assertIn("directoryImageCandidate", sleep)
        self.assertIn("freshCount", sleep)
        self.assertNotIn("std::vector<std::string> files", sleep)
        self.assertIn("APP_STATE.pushRecentSleep", sleep)
        self.assertIn("capturePopupSnapshot", sleep)
        self.assertIn("restorePopupSnapshot", sleep)
        self.assertIn('const outputCanvas = document.createElement("canvas");', sleep_tool)
        self.assertIn("function renderFirmwarePreview()", sleep_tool)
        self.assertIn("alpha <= threshold", sleep_tool)
        self.assertIn("gray < oneBitThreshold ? 0 : 255", sleep_tool)
        self.assertIn("makeOptimizedOverlayPngBlob(outputCanvas, outputCtx)", sleep_tool)
        self.assertIn("SleepFrameStore::save(renderer)", sleep)
        self.assertIn("config.preserveAlpha = true;", sleep)
        self.assertIn("config.writeWhiteInBw = true;", sleep)

        self.assertIn("bool preserveAlpha = false", render_config)
        self.assertIn("bool writeWhiteInBw = false", render_config)
        self.assertIn("alphaCoveragePasses", decoder)
        self.assertIn("ctx.caching = !config.cachePath.empty() && !config.preserveAlpha;", decoder)
        self.assertIn("ctx.transparentColor = png->getTransparentColor();", decoder)
        self.assertIn("getSupportedDimensionsStatic", sleep)
        self.assertIn("const uint8_t sample = readPackedSample", decoder)
        self.assertIn("hasAlpha && sample == static_cast<uint8_t>(transparentColor)", decoder)
        self.assertIn("if (outY < 0 || outY >= ctx->screenHeight) continue;", decoder)
        self.assertIn("if (outX >= 0 && outX < screenWidth)", decoder)
        self.assertIn("pw.init(*ctx->renderer, ctx->config->writeWhiteInBw);", decoder)
        self.assertIn("writeWhiteInBw", pixel_writer)
        self.assertIn("function detectSourceRect(image)", sleep_tool)
        self.assertIn("return { x: 0, y: 0, width: image.naturalWidth, height: image.naturalHeight };", sleep_tool)
        self.assertIn("idx = alphas.length;", sleep_tool)
        self.assertNotIn("idx = palette.length;", sleep_tool)
        self.assertIn('const LANGUAGE_KEY = "crossvi.sleepConverter.language.v2";', sleep_tool)
        placement_select = sleep_tool[sleep_tool.index('<select id="fit">') : sleep_tool.index("</select>", sleep_tool.index('<select id="fit">'))]
        self.assertLess(placement_select.index('value="fill" selected'), placement_select.index('value="height"'))
        self.assertLess(placement_select.index('value="height"'), placement_select.index('value="width"'))
        language_init = sleep_tool[sleep_tool.index("function initialLanguage") : sleep_tool.index("function t(key")]
        self.assertIn('return "en";', language_init)
        self.assertNotIn("navigator.languages", language_init)
        checker_css = sleep_tool[sleep_tool.index(".checker {") : sleep_tool.index("canvas {")]
        self.assertNotIn("padding:", checker_css)
        self.assertNotIn("border:", checker_css)
        canvas_css = sleep_tool[sleep_tool.index("canvas {") : sleep_tool.index(".hidden")]
        self.assertNotIn("box-shadow", canvas_css)
        self.assertIn("function quantizeCanvasToFourGrayLevels(canvas, context)", sleep_tool)
        self.assertIn("view.setUint16(28, 4, true);", sleep_tool)
        self.assertIn("const rowStride = Math.ceil(width / 8) * 4;", sleep_tool)
        self.assertIn("const paletteBytes = 16 * 4;", sleep_tool)
        self.assertIn("const v = i < 4 ? i * 85 : 255;", sleep_tool)
        self.assertNotIn("view.setUint16(28, 1, true);", sleep_tool)

    def test_panel_refresh_wait_distinguishes_completion_start_failure_and_timeout(self):
        bus_header = (
            REPO_ROOT / "freeink-sdk/libs/display/FreeInkDisplay/src/bus/EpdBus.h"
        ).read_text(encoding="utf-8")
        bus = (REPO_ROOT / "freeink-sdk/libs/display/FreeInkDisplay/src/bus/EpdBus.cpp").read_text(
            encoding="utf-8"
        )
        driver = (
            REPO_ROOT / "freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.cpp"
        ).read_text(encoding="utf-8")

        for status in ("Completed", "NeverStarted", "TimedOut"):
            self.assertIn(status, bus_header)
            self.assertIn(f"RefreshWaitResult::{status}", bus)
        finish = driver[driver.index("void Uc8253X3Driver::displayFinish") :
                        driver.index("void Uc8253X3Driver::setFastLutFrameCount")]
        self.assertIn("_pendingRefreshStarted", finish)
        self.assertIn("const RefreshWaitResult refreshResult", finish)

    def test_markdown_uses_the_text_sleep_cover_and_cache_clear_paths(self):
        sleep = (REPO_ROOT / "src/activities/boot_sleep/SleepActivity.cpp").read_text(encoding="utf-8")
        cache = (REPO_ROOT / "src/util/BookCacheUtils.cpp").read_text(encoding="utf-8")
        clear_cache = cache[cache.index("void clearBookCache(") : cache.index("bool clearBookCacheDirectory")]
        expected = (
            "FsHelpers::hasTxtExtension(APP_STATE.openEpubPath) ||\n"
            "             FsHelpers::hasMarkdownExtension(APP_STATE.openEpubPath)"
        )
        self.assertIn(expected, sleep)
        self.assertIn("FsHelpers::hasMarkdownExtension(path)", clear_cache)

    def test_early_startup_sleep_preserves_the_sleep_frame_with_a_strong_refresh(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        helper = main[main.index("void enterStartupDeepSleep(") : main.index("// Enter deep sleep mode")]
        self.assertIn("display.begin(false);", helper)
        self.assertIn("SleepFrameStore::load(display, false)", helper)
        self.assertIn("drawBundledDefaultSleepScreen();", helper)
        self.assertIn("constexpr uint8_t STARTUP_SLEEP_CONDITION_PASSES = 2;", helper)
        self.assertIn("display.requestResync(STARTUP_SLEEP_CONDITION_PASSES);", helper)
        self.assertIn(
            "display.triggerDisplay(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_REFRESH);", helper
        )
        self.assertLess(helper.index("display.deepSleep();"), helper.index("powerManager.startDeepSleep(gpio);"))
        self.assertIn("enterStartupDeepSleep(true);", main)
        self.assertIn("enterStartupDeepSleep(false);", main)

        frame_store = (REPO_ROOT / "src/activities/boot_sleep/SleepFrameStore.cpp").read_text(encoding="utf-8")
        loader = frame_store[frame_store.index("bool load(") :]
        self.assertIn("const bool consume", loader)
        self.assertIn("if (consume) Storage.remove(SLEEP_FRAME_FILE);", loader)

    def test_power_wake_is_checked_before_sd_with_a_persisted_short_press_mirror(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        settings = (REPO_ROOT / "src/CrossPointSettings.h").read_text(encoding="utf-8")

        self.assertLess(main.index("gpio.verifyPowerButtonWakeup("), main.index("Storage.begin()"))
        self.assertIn("readWakeShortPressFromNvs()", main)
        self.assertIn("const bool shortPressWakes = gpio.deviceIsX3() || readWakeShortPressFromNvs();", main)
        self.assertGreaterEqual(main.count("mirrorWakeShortPressToNvs();"), 2)
        self.assertIn("POWER_BUTTON_WAKE_SHORT_MS = 10", settings)
        self.assertIn("POWER_BUTTON_WAKE_LONG_MS = 200", settings)

    def test_grayscale_sleep_frames_are_rebuilt_as_one_bit_wake_surrogates(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        sleep = (REPO_ROOT / "src/activities/boot_sleep/SleepActivity.cpp").read_text(encoding="utf-8")
        sleep_header = (REPO_ROOT / "src/activities/boot_sleep/SleepActivity.h").read_text(encoding="utf-8")
        frame_store = (REPO_ROOT / "src/activities/boot_sleep/SleepFrameStore.cpp").read_text(encoding="utf-8")
        manager = (REPO_ROOT / "src/activities/ActivityManager.cpp").read_text(encoding="utf-8")
        bitmap = sleep[sleep.index("void SleepActivity::renderBitmapSleepScreen") :
                       sleep.index("void SleepActivity::renderCoverSleepScreen")]
        deep_sleep = main[main.index("void enterDeepSleep") : main.index("void setupDisplayAndFonts")]

        self.assertNotIn("sleepScreenMayUseGrayscale", main)
        self.assertIn("bool wakeFrameReplayable() const", sleep_header)
        self.assertIn("bool ActivityManager::goToSleep()", manager)
        self.assertIn("const bool wakeFrameReplayable = activityManager.goToSleep();", deep_sleep)
        self.assertIn("renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);", bitmap)
        self.assertIn("renderer.setRenderMode(GfxRenderer::BW);", bitmap)
        self.assertGreaterEqual(bitmap.count("bitmap.rewindToData()"), 3)
        self.assertGreaterEqual(bitmap.count("renderer.drawBitmap(bitmap"), 4)
        self.assertIn("wakeFrameReplayable_ =", bitmap)
        self.assertLess(bitmap.index("renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);"),
                        bitmap.rindex("renderer.drawBitmap(bitmap"))
        self.assertIn("SleepFrameStore::save(renderer)", main)
        self.assertIn("StagedFileTransaction::publishAndVerify", frame_store)
        self.assertIn("SLEEP_FRAME_MAGIC", frame_store)
        self.assertIn("payloadDigest.hash == expectedHash", frame_store)
        self.assertIn("frameSpec(display.getDisplayWidth(), display.getDisplayHeight()", frame_store)
        discard = frame_store[frame_store.index("void discard()") :
                              frame_store.index("bool save(")]
        self.assertIn("Storage.remove(SLEEP_FRAME_TEMP_FILE);", discard)
        self.assertIn("Storage.remove(SLEEP_FRAME_BACKUP_FILE);", discard)
        self.assertIn("Storage.remove(SLEEP_FRAME_FILE);", discard)
        state_failure = deep_sleep[deep_sleep.index("if (!APP_STATE.saveToFile())") :
                                   deep_sleep.index("LOG_INF(\"SLW\", \"sleep persistence complete")]
        self.assertIn("SleepFrameStore::discard();", state_failure)

    def test_date_outside_reader_does_not_depend_on_clock_visibility(self):
        theme = (REPO_ROOT / "src/components/themes/crossvi/CrossViTheme.cpp").read_text(encoding="utf-8")
        helper = theme[theme.index("bool outsideDateTimeText") : theme.index("void drawOutsideClockRight")]
        self.assertIn("const bool showTime", helper)
        self.assertIn("const bool showDate", helper)
        self.assertIn("if (!showTime && !showDate) return false;", helper)
        self.assertNotIn("outsideReaderClock == CrossPointSettings::STATUS_BAR_CLOCK_HIDE) {\n    return false;", helper)
        self.assertNotIn("outsideDateTimeOnLeft", theme)
        self.assertIn("if (showDateTime) {", theme)

        settings = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        outside_clock = settings[
            settings.rfind("SettingInfo::", 0, settings.index("StrId::STR_CLOCK_OUTSIDE_READER")) :
            settings.index("StrId::STR_DATE_OUTSIDE_READER")
        ]
        self.assertIn("SettingInfo::Toggle", outside_clock)

    def test_opds_releases_font_cache_before_tls(self):
        activity = (REPO_ROOT / "src/activities/browser/OpdsBookBrowserActivity.cpp").read_text(encoding="utf-8")
        prepare = activity[activity.index("void OpdsBookBrowserActivity::prepareNetworkRequest") :
                           activity.index("void OpdsBookBrowserActivity::onEnter")]
        self.assertLess(prepare.index("requestUpdateAndWait()"), prepare.index("clearAllCaches()"))
        fetch = activity[activity.index("void OpdsBookBrowserActivity::fetchFeed") :
                         activity.index("void OpdsBookBrowserActivity::releaseEntries")]
        download = activity[activity.index("void OpdsBookBrowserActivity::downloadBook") :
                            activity.index("void OpdsBookBrowserActivity::launchSearch")]
        self.assertLess(fetch.index('prepareNetworkRequest("feed_tls")'), fetch.index("OpdsParser parser"))
        self.assertLess(download.index('prepareNetworkRequest("download_tls")'),
                        download.index("HttpDownloader::downloadToFile"))

    def test_settings_groups_interface_and_text_preferences(self):
        settings = (REPO_ROOT / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
        appearance = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(
            encoding="utf-8"
        )
        text_settings = (REPO_ROOT / "src/activities/settings/TextSettingsActivity.cpp").read_text(
            encoding="utf-8"
        )
        font_selection = (REPO_ROOT / "src/activities/settings/FontSelectionActivity.cpp").read_text(
            encoding="utf-8"
        )
        time_settings = (REPO_ROOT / "src/activities/settings/TimeSettingsActivity.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("SettingAction::Appearance", settings)
        self.assertIn("SettingAction::TextSettings", settings)
        for name in (
            "STR_HOME_LAYOUT",
            "STR_SHOW_DEVICE_NAME_HOME",
            "STR_CLOCK_OUTSIDE_READER",
            "STR_DATE_OUTSIDE_READER",
            "STR_LIBRARY_DISPLAY_MODE",
        ):
            self.assertIn(f"StrId::{name}", appearance)
        for tab in ("STR_TEXT_TAB_FONT", "STR_TEXT_TAB_SIZE", "STR_TEXT_TAB_LAYOUT", "STR_TEXT_TAB_STYLE"):
            self.assertIn(tab, text_settings)
        self.assertIn("ParsedText parsed(", text_settings)
        self.assertIn("SETTINGS.focusReadingEnabled", text_settings)
        self.assertIn("settings_[selectedRow_].nameId == StrId::STR_TEXT_AA", text_settings)
        self.assertIn("ReaderUtils::renderAntiAliased", text_settings)
        self.assertIn(
            "selectedRow_ < 0 ? tr(STR_BACK) : I18N.get(TAB_LABELS[selectedTab_])",
            text_settings,
        )
        self.assertIn("rebuildFontOptions()", text_settings)
        self.assertIn("rebuildSizeOptions()", text_settings)
        self.assertNotIn("make_unique<FontSelectionActivity>", text_settings)
        self.assertNotIn("make_unique<FontSizeSelectionActivity>", text_settings)
        self.assertIn("preparedPreviewFontId_ != fontId", text_settings)
        self.assertIn("SETTINGS.focusReadingEnabled ? 0x03 : 0x01", text_settings)
        self.assertIn("sdFontSystem.ensureLoaded(renderer, false);", text_settings)
        self.assertIn("renderer.copyRegionToBuffer", text_settings)
        self.assertIn("renderer.copyBufferToRegion", text_settings)
        for preview_setting in (
            "SETTINGS.screenMargin",
            "SETTINGS.getReaderLineCompression()",
            "SETTINGS.wordSpacing",
            "SETTINGS.paragraphAlignment",
            "SETTINGS.extraParagraphSpacing",
            "SETTINGS.forceParagraphIndents",
            "SETTINGS.readerDarkMode",
        ):
            self.assertIn(preview_setting, text_settings)
        self.assertIn("refreshPreviewAfterSettingChange(setting.nameId)", text_settings)
        self.assertIn("refreshPreviewAfterSettingChange(settingId)", text_settings)
        self.assertIn("invalidatePreviewLocked();", text_settings)
        self.assertIn("preparedPreviewFontId_ != fontId", font_selection)
        self.assertNotIn("STR_FONT_PREVIEW_BOLD", font_selection)
        self.assertNotIn("STR_FONT_PREVIEW_ITALIC", font_selection)
        self.assertNotIn("renderer.displayBuffer();\n  if (auto* fcm", font_selection)
        self.assertIn("ClockDateFormat::FORMAT_PATTERNS", time_settings)
        self.assertNotIn("dateFormatNames", time_settings)

    def test_write_if_changed_preserves_file_when_bytes_match(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "generated.h"
            self.assertTrue(write_if_changed(path, "same\n"))
            before = path.stat()
            self.assertFalse(write_if_changed(path, "same\n"))
            after = path.stat()
            self.assertEqual(before.st_ino, after.st_ino)
            self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)
            self.assertTrue(write_if_changed(path, "changed\n"))
            self.assertEqual(path.read_text(encoding="utf-8"), "changed\n")

    def test_html_codegen_is_reproducible_and_does_not_rewrite(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "src"
            source.mkdir()
            (source / "sample.html").write_text("<p>CrossVi</p>\n", encoding="utf-8")
            command = [sys.executable, str(SCRIPTS / "build_html.py")]
            subprocess.run(command, cwd=root, check=True, capture_output=True, text=True)
            generated = source / "sampleHtml.generated.h"
            before = generated.stat()
            payload = generated.read_bytes()
            subprocess.run(command, cwd=root, check=True, capture_output=True, text=True)
            after = generated.stat()
            self.assertEqual(payload, generated.read_bytes())
            self.assertEqual(before.st_ino, after.st_ino)
            self.assertEqual(before.st_mtime_ns, after.st_mtime_ns)

    def test_wifi_epub_optimizer_embeds_device_specific_crossvi_cover_sizes(self):
        files_page = (REPO_ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")
        self.assertIn("META-INF/crossvi/cover-144x240-v1.bmp", files_page)
        self.assertIn("META-INF/crossvi/cover-273x456-v2.bmp", files_page)
        self.assertIn("META-INF/crossvi/cover-249x415-v2.bmp", files_page)
        self.assertIn("CROSSVI_CAROUSEL_THUMB_WIDTH, CROSSVI_CAROUSEL_THUMB_HEIGHT, true", files_page)
        self.assertIn("CROSSVI_CAROUSEL_X4_THUMB_WIDTH, CROSSVI_CAROUSEL_X4_THUMB_HEIGHT, true", files_page)
        self.assertIn("const outputWidth = Math.max(1, Math.round(canvas.width * scale));", files_page)
        self.assertIn("if (white) result[62 + y * rowBytes", files_page)
        self.assertNotIn("if (!white) result[62 + y * rowBytes", files_page)
        self.assertIn("thumbnailSourceCanvas, CROSSVI_THUMB_WIDTH, CROSSVI_THUMB_HEIGHT", files_page)
        self.assertIn("out.file(CROSSVI_THUMB_PATH, coverThumbnailData, { compression: 'STORE'", files_page)
        self.assertIn("out.file(CROSSVI_CAROUSEL_THUMB_PATH, carouselThumbnailData, { compression: 'STORE'", files_page)
        self.assertIn("out.file(CROSSVI_CAROUSEL_X4_THUMB_PATH, carouselX4ThumbnailData", files_page)

    def test_file_transfer_auto_optimizes_standalone_image_uploads(self):
        files_page = (REPO_ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")
        self.assertIn("function isStandaloneImageUpload(file)", files_page)
        self.assertIn("async function optimizeStandaloneImageUpload(file)", files_page)
        self.assertIn("function detectStandaloneImageSource(file, img, width, height)", files_page)
        self.assertNotIn("if (alpha > 16) {", files_page)
        self.assertIn("idx = alphas.length;", files_page)
        self.assertNotIn("idx = palette.length;", files_page)
        self.assertIn("const hasAlpha = imageDataHasTransparency(ctx.getImageData(x, y, drawWidth, drawHeight));",
                      files_page)
        self.assertNotIn("ctx.getImageData(0, 0, canvas.width, canvas.height));\n  const outputName", files_page)
        self.assertIn("const scale = Math.min(profile.width / rect.width, profile.height / rect.height, 1);", files_page)
        self.assertIn("return hasAlpha ? `${base}.png` : `${base}.bmp`;", files_page)
        self.assertIn("function makeCrossViSleepBmp(canvas)", files_page)
        self.assertIn("writeLe16(view, 28, 4);", files_page)
        self.assertIn("const rowBytes = Math.ceil(width / 8) * 4;", files_page)
        self.assertIn("const bmp = makeCrossViSleepBmp(canvas);", files_page)
        self.assertIn("const needsImageOptimization = isUploadImage && convertEnabled;", files_page)
        self.assertIn("file = await optimizeStandaloneImageUpload(file);", files_page)
        self.assertIn("await uploadFileHTTP(file, onProgress, null, null);", files_page)

    def test_file_transfer_uses_only_the_bounded_http_upload_path(self):
        files_page = (REPO_ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")
        fonts_page = (REPO_ROOT / "src/network/html/FontsPage.html").read_text(encoding="utf-8")
        server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/network/CrossPointWebServer.h").read_text(encoding="utf-8")
        self.assertIn("await uploadFileHTTP(file, onProgress, null, null);", files_page)
        self.assertNotIn("new WebSocket", files_page)
        self.assertNotIn("WebSocketsServer", header)
        self.assertNotIn("onWebSocketEvent", server)
        self.assertIn("TRANSFER_BUFFER_SIZE = 4096", header)
        self.assertIn("COOPERATIVE_UPLOAD_CHUNK_SIZE = 64U * 1024U", header)
        self.assertIn("file.slice(offset, Math.min(offset + chunkSize, file.size))", files_page)
        self.assertIn("for (let attempt = 0; attempt < 4; attempt++)", files_page)
        self.assertIn("for (let attempt = 0; attempt < 4; attempt++)", fonts_page)
        self.assertIn("'/api/upload/chunk'", files_page)
        self.assertIn("X-CrossVi-Upload-Offset", files_page)
        self.assertIn('server->on(\n      "/api/upload/chunk"', server)
        self.assertIn("uploadFontInChunks(file, family, uploadName", fonts_page)
        self.assertIn("'X-CrossVi-Upload-Type': 'font'", fonts_page)
        self.assertIn("'X-CrossVi-Font-Family': encodeURIComponent(family)", fonts_page)
        self.assertIn("StagedFileTransaction::Digest requestDigestStart", header)
        self.assertIn("state.streamDigest = state.requestDigestStart", server)
        self.assertNotIn("new FormData()", fonts_page)
        self.assertNotIn('server->on("/api/fonts/upload"', server)

    def test_legacy_multipart_upload_only_creates_empty_files(self):
        files_page = (REPO_ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")
        server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        handler = server[server.index("void CrossPointWebServer::handleUpload(UploadState& state)") :]
        write_branch = handler[handler.index("UPLOAD_FILE_WRITE") : handler.index("UPLOAD_FILE_END")]

        self.assertIn("if (file.size === 0)", files_page)
        self.assertIn("Non-empty uploads require /api/upload/chunk", write_branch)
        self.assertNotIn("memcpy(", write_branch)

    def test_file_transfer_truncates_the_received_filename_to_the_screen(self):
        activity = (REPO_ROOT / "src/activities/network/CrossPointWebServerActivity.cpp").read_text(encoding="utf-8")
        footer = activity[activity.index("if (!lastReceivedName.empty())") :]
        self.assertIn("renderer.truncatedText(", footer)
        self.assertIn("pageWidth - metrics.contentSidePadding * 2", footer)
        self.assertLess(footer.index("renderer.truncatedText("), footer.index("renderer.drawCenteredText("))

    def test_file_manager_does_not_embed_untrusted_names_in_html_handlers(self):
        files_page = (REPO_ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")
        self.assertNotIn('onclick="openDeleteModal(', files_page)
        self.assertNotIn('onclick="openMoveModal(', files_page)
        self.assertNotIn('onclick="openRenameModal(', files_page)
        self.assertIn('data-file-action="delete"', files_page)
        self.assertIn("const safeImageName = escapeHtml(img.name);", files_page)
        self.assertNotIn('${img.name}</div>', files_page)

    def test_epub_optimizer_log_treats_dynamic_messages_as_text(self):
        files_page = (REPO_ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")
        self.assertIn("else msg.textContent = message;", files_page)
        self.assertIn("appendLog(message, type, tag, false);", files_page)
        self.assertIn("logHtml(`<strong>${escapeHtml(file.name)}</strong>", files_page)
        self.assertIn("escapeHtml(String(type))", files_page)
        self.assertIn("escapeHtml(String(detail))", files_page)
        self.assertIn("escapeHtml(String(reason))", files_page)

    def test_json_post_handlers_are_bounded_before_webserver_string_allocation(self):
        header = (REPO_ROOT / "src/network/CrossPointWebServer.h").read_text(encoding="utf-8")
        server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        self.assertIn("MAX_JSON_BODY_SIZE = 8192", header)
        handler = server[server.index("void CrossPointWebServer::handleJsonBody()") :]
        self.assertIn('server->header("Content-Type")', handler)
        self.assertLess(handler.index("acceptsRawContentType"), handler.index("server->raw()"))
        self.assertIn('"Content-Type"', server)
        self.assertIn('"X-CrossVi-Upload-Offset"', server)
        self.assertIn("server->collectHeaders(requestHeaders, 13)", server)
        self.assertIn("JsonBodyBuffer::State body = jsonBody.take();", server)
        self.assertIn("HTTPRaw& raw = server->raw();", server)
        upload_handler = server[server.index("void CrossPointWebServer::handleUpload(UploadState& state)") :]
        self.assertLess(upload_handler.index("acceptsMultipartUploadContentType"),
                        upload_handler.index("server->upload()"))
        self.assertIn("JsonBodyBuffer::Error::TooLarge", server)
        self.assertNotIn('server->arg("plain")', server)
        self.assertNotIn("enableCORS(true)", server)

    def test_web_server_loads_wifi_credentials_before_registering_routes(self):
        server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        begin = server[server.index("void CrossPointWebServer::begin()") :]
        self.assertLess(begin.index("WIFI_STORE.loadFromFile();"), begin.index('server->on("/api/wifi"'))

    def test_streamed_credential_json_uses_emitted_entry_commas(self):
        server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        opds = server[server.index("void CrossPointWebServer::handleGetOpdsServers() const") :]
        opds = opds[: opds.index("void CrossPointWebServer::handlePostOpdsServer()")]
        wifi = server[server.index("void CrossPointWebServer::handleGetWifiNetworks() const") :]
        wifi = wifi[: wifi.index("void CrossPointWebServer::handlePostWifiNetwork()")]
        for handler in (opds, wifi):
            self.assertIn("bool seenFirst = false;", handler)
            self.assertIn('if (seenFirst) server->sendContent(",");', handler)
            self.assertIn("seenFirst = true;", handler)
            self.assertNotIn("if (i > 0)", handler)

    def test_network_store_failures_are_reported_and_stale_transactions_are_retryable(self):
        server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        opds_update = server[server.index("void CrossPointWebServer::handlePostOpdsServer()") :]
        opds_update = opds_update[: opds_update.index("void CrossPointWebServer::handleDeleteOpdsServer()")]
        opds_delete = server[server.index("void CrossPointWebServer::handleDeleteOpdsServer()") :]
        opds_delete = opds_delete[: opds_delete.index("void CrossPointWebServer::handleGetWifiNetworks()")]
        wifi_update = server[server.index("void CrossPointWebServer::handlePostWifiNetwork()") :]
        wifi_update = wifi_update[: wifi_update.index("void CrossPointWebServer::handleDeleteWifiNetwork()")]
        self.assertIn("if (!OPDS_STORE.updateServer", opds_update)
        self.assertIn("if (!OPDS_STORE.removeServer", opds_delete)
        self.assertIn("WIFI_STORE.updateCredential", wifi_update)
        self.assertNotIn("removeCredential(oldSsid)", wifi_update)

        opds_browser = (REPO_ROOT / "src/activities/browser/OpdsBookBrowserActivity.cpp").read_text(
            encoding="utf-8"
        )
        download = opds_browser[opds_browser.index("void OpdsBookBrowserActivity::downloadBook") :]
        download = download[: download.index("void OpdsBookBrowserActivity::launchSearch")]
        self.assertIn("Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())", download)

        webdav = (REPO_ROOT / "src/network/WebDAVHandler.cpp").read_text(encoding="utf-8")
        self.assertGreaterEqual(
            webdav.count("Storage.exists(tempPath.c_str()) && !Storage.remove(tempPath.c_str())"), 1
        )
        self.assertIn("Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str())", webdav)

    def test_epub_text_reference_never_overrides_saved_spine_zero(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("if (!progress && currentSpineIndex == 0)", reader)
        self.assertNotIn("if (currentSpineIndex == 0) {\n    int textSpineIndex", reader)

    def test_txt_completion_does_not_count_a_nonexistent_page_turn(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        navigation = reader[reader.index("} else if (nextTriggered) {") :]
        navigation = navigation[: navigation.index("bool TxtReaderActivity::handleReaderShortcut")]
        advance = navigation.index("if (currentPage < totalPages - 1)")
        self.assertGreater(navigation.index("stopReadingPage(true", advance), advance)
        completion = navigation.index("markBookCompleted()")
        self.assertLess(navigation.index("stopReadingPage(false", advance), completion)

    def test_txt_auto_turn_only_runs_after_the_page_index_is_complete(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        loop = reader[reader.index("void TxtReaderActivity::loop()") :
                      reader.index("bool TxtReaderActivity::handleReaderShortcut")]
        auto_turn = loop[loop.index("if (automaticPageTurnActive && !indexWorkPending)") :
                         loop.index("if (ReaderUtils::handleBackNavigation")]

        self.assertIn("!initializationFailed && pageIndexComplete", loop)
        self.assertNotIn("!pageIndexComplete", auto_turn)
        self.assertIn("automaticPageTurnActive = false;", auto_turn)

    def test_txt_reflow_keeps_the_current_byte_offset_even_when_progress_save_fails(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.h").read_text(encoding="utf-8")
        settings = reader[reader.index("void TxtReaderActivity::openBookReaderSettings") :
                          reader.index("void TxtReaderActivity::applyOrientation")]
        orientation = reader[reader.index("void TxtReaderActivity::applyOrientation") :
                             reader.index("void TxtReaderActivity::updateAutoPageTurnPreference")]

        self.assertIn("void rememberCurrentByteOffset();", header)
        self.assertIn("bool pendingProgressSaveError = false;", header)
        for operation in (settings, orientation):
            self.assertLess(operation.index("rememberCurrentByteOffset();"), operation.index("invalidateReaderLayout();"))
            self.assertIn("if (!saveProgress()) pendingProgressSaveError = true;", operation)

    def test_epub_cover_skip_requires_the_declared_cover_resource(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
        cover_skip = reader[reader.index("bool EpubReaderActivity::skipCoverPageIfNeeded") :
                            reader.index("bool EpubReaderActivity::sectionLandingReady")]
        page_turn = reader[reader.index("void EpubReaderActivity::pageTurn") :
                           reader.index("bool EpubReaderActivity::retargetQueuedPageTurns")]

        self.assertIn("std::atomic<bool> initialCoverSkipPending{false};", header)
        self.assertIn("initialCoverSkipPending", cover_skip)
        self.assertIn("page.isCoverOnly(epub->getCoverItemHref())", cover_skip)
        self.assertIn("moveOnePageWithoutRendering(true)", cover_skip)
        self.assertNotIn("moveOnePageWithoutRendering(false)", cover_skip)
        self.assertIn("initialCoverSkipPending.store(false", page_turn)
        self.assertNotIn("leadingImageOnly", cover_skip)
        self.assertNotIn("page.isImageOnly()", cover_skip)

    def test_epub_image_only_pages_do_not_reuse_a_neighboring_text_offset(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
        remember = reader[reader.index("void EpubReaderActivity::rememberCurrentContentOffset") :
                          reader.index("void EpubReaderActivity::openBookReaderSettings")]
        save = reader[reader.index("bool EpubReaderActivity::saveProgress") :]
        save = save[: save.index("\n}") + 2]

        self.assertIn("int currentPageSourceOffsetSpine = -1;", header)
        self.assertIn("int currentPageSourceOffsetPage = -1;", header)
        self.assertLess(remember.index("PageSourceAnchor::first"),
                        remember.index("getVisibleTextOffsetForPage"))
        self.assertIn("cachedContentSourceOffset.has_value()", remember)
        self.assertIn("currentPageSourceOffset.has_value()", save)
        self.assertIn("currentPageSourceOffsetSpine == spineIndex", save)
        self.assertIn("currentPageSourceOffsetPage == currentPage", save)

    def test_epub_sequential_navigation_skips_non_linear_spine_entries(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        page_turn = reader[reader.index("void EpubReaderActivity::pageTurn") :
                           reader.index("bool EpubReaderActivity::moveOnePageWithoutRendering")]
        cover_move = reader[reader.index("bool EpubReaderActivity::moveOnePageWithoutRendering") :
                            reader.index("bool EpubReaderActivity::skipCoverPageIfNeeded")]

        for operation in (page_turn, cover_move):
            self.assertIn("getAdjacentLinearSpineIndex", operation)
            self.assertNotIn("currentSpineIndex++", operation)
            self.assertNotIn("currentSpineIndex--", operation)
            self.assertNotIn("++currentSpineIndex", operation)
            self.assertNotIn("--currentSpineIndex", operation)

    def test_txt_finishes_initial_index_before_reading_in_bounded_ticks(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.h").read_text(encoding="utf-8")
        loop = reader[reader.index("void TxtReaderActivity::loop()") : reader.index("bool TxtReaderActivity::handleReaderShortcut")]
        indexing = reader[reader.index("void TxtReaderActivity::processPageIndex") :
                          reader.index("bool TxtReaderActivity::buildPageIndexBatch")]
        build = reader[reader.index("bool TxtReaderActivity::buildPageIndexBatch") :
                       reader.index("void TxtReaderActivity::markPageIndexFailed")]
        render = reader[reader.index("void TxtReaderActivity::render(RenderLock&&)") :]
        render = render[: render.index("void TxtReaderActivity::renderCurrentPage")]

        self.assertIn("RenderLock lock(std::try_to_lock);", indexing)
        self.assertIn("buildPageIndexBatch(INDEX_PAGES_PER_TICK)", indexing)
        self.assertIn("processPageIndex()", loop)
        self.assertIn("if (parsedPages >= maxPages) break;", build)
        self.assertIn("else if (pageIndexComplete)", indexing)
        self.assertLess(indexing.index("else if (pageIndexComplete)"),
                        indexing.index("finishReaderInitialization();", indexing.index("else if (pageIndexComplete)")))
        self.assertNotIn("completePageIndex()", render)
        self.assertNotIn("processBackgroundPageIndex", reader)
        self.assertNotIn("PageIndexWork", header)
        self.assertNotIn("pageIndexTarget", header)
        self.assertIn("return pageIndexing.load(std::memory_order_acquire);", header)

    def test_txt_resume_jump_and_clipping_reuse_the_complete_index(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        initialize = reader[reader.index("void TxtReaderActivity::initializeReader") :
                            reader.index("void TxtReaderActivity::finishReaderInitialization")]
        finish = reader[reader.index("void TxtReaderActivity::finishReaderInitialization") :
                        reader.index("bool TxtReaderActivity::ensureContentReadSession")]
        clipping = reader[reader.index("void TxtReaderActivity::openClippingSelection") :
                          reader.index("void TxtReaderActivity::openClippings")]
        jump = reader[reader.index("void TxtReaderActivity::jumpToByteOffset") :
                      reader.index("void TxtReaderActivity::applyIndexedByteOffset")]

        for operation in (initialize, clipping, jump):
            self.assertNotIn("buildPageIndexBatch(", operation)
        self.assertNotIn("ProgressFile::loadTxt", initialize)
        self.assertNotIn("validateClippingJump", initialize)
        self.assertEqual(finish.count("validateClippingJump"), 1)
        self.assertNotIn("STR_INDEXING", clipping)
        self.assertNotIn("STR_INDEXING", jump)
        self.assertIn("applyIndexedByteOffset(byteOffset);", jump)

    def test_txt_reuses_page_io_and_only_builds_the_initial_index_when_input_is_idle(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.h").read_text(encoding="utf-8")
        loop = reader[reader.index("void TxtReaderActivity::loop()") :
                      reader.index("bool TxtReaderActivity::handleReaderShortcut")]
        session = reader[reader.index("bool TxtReaderActivity::ensureContentReadSession") :
                         reader.index("void TxtReaderActivity::processPageIndex")]
        scratch_release = reader[reader.index("void TxtReaderActivity::releasePageIndexScratch") :
                                 reader.index("void TxtReaderActivity::releaseContentReadSession")]
        load = reader[reader.index("bool TxtReaderActivity::loadPageAtOffset(size_t offset") :
                      reader.index("bool TxtReaderActivity::loadPageAtOffsetWithScratch")]
        build = reader[reader.index("bool TxtReaderActivity::buildPageIndexBatch") :
                       reader.index("void TxtReaderActivity::markPageIndexFailed")]
        render = reader[reader.index("void TxtReaderActivity::render(RenderLock&&)") :
                        reader.index("void TxtReaderActivity::renderCurrentPage")]

        self.assertIn("HalFile contentFile;", header)
        self.assertIn("std::unique_ptr<uint8_t[]> pageScratch;", header)
        self.assertIn("if (!contentFile.isOpen()", session)
        self.assertIn("pageScratchSize < CHUNK_SIZE + 1", session)
        self.assertIn("std::vector<std::string>{}.swap(pageIndexScratchLines);", scratch_release)
        self.assertIn("std::vector<uint32_t>{}.swap(pageIndexScratchLineOffsets);", scratch_release)
        self.assertIn("if (!inputEdge && !readerInputHeld) processPageIndex();", loop)
        self.assertIn("finishDeferredOpenState();", loop)
        self.assertNotIn("processBackgroundPageIndex", loop)
        self.assertNotIn("makeUniqueNoThrow", load)
        self.assertNotIn("HalFile contentFile", load)
        self.assertIn("&pageIndexScratchLineOffsets", build)
        self.assertIn("pageIndexScratchOffset = static_cast<uint32_t>(pageStartOffset)", build)
        self.assertIn("currentPageLines.swap(pageIndexScratchLines)", render)
        self.assertIn("currentPageLineOffsets.swap(pageIndexScratchLineOffsets)", render)
        reused_scratch = render[render.index("if (pageIndexScratchOffset &&") :
                                render.index("if (!pageReused && pageIndexScratchOffset)")]
        self.assertIn("releasePageIndexScratch();", reused_scratch)
        stale_scratch = render[render.index("if (!pageReused && pageIndexScratchOffset)") :
                               render.index("const bool pageLoadFailed")]
        self.assertIn("releasePageIndexScratch();", stale_scratch)

    def test_txt_long_builtin_lines_use_single_pass_shaped_wrap(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        wrap = (REPO_ROOT / "src/activities/reader/TxtLineWrap.h").read_text(encoding="utf-8")

        self.assertIn("findLargestFittingShapedLineBreak", reader)
        self.assertIn("!renderer.isSdCardFont(cachedFontId)", reader)
        self.assertIn("widthBeforeCurrent", wrap)
        self.assertIn("largestFittingSpace", wrap)

    def test_epub_page_deserialize_reserves_validated_element_count(self):
        page = (REPO_ROOT / "lib/Epub/Epub/Page.cpp").read_text(encoding="utf-8")
        deserialize = page[page.index("std::unique_ptr<Page> Page::deserialize") :]
        validation = deserialize.index("count > SectionCacheValidation::MAX_PAGE_ELEMENTS")
        reserve = deserialize.index("page->elements.reserve(count);")
        loop = deserialize.index("for (uint16_t i = 0; i < count; i++)")

        self.assertLess(validation, reserve)
        self.assertLess(reserve, loop)

    def test_home_defers_all_book_summaries_until_after_first_frame(self):
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        on_enter = home[home.index("void HomeActivity::onEnter()") : home.index("void HomeActivity::onExit()")]
        loop = home[home.index("void HomeActivity::loop()") : home.index("void HomeActivity::render(RenderLock&&)")]

        self.assertNotIn("loadRecentNonEpubReadingStats()", on_enter)
        self.assertNotIn("loadBookSummary()", on_enter)
        self.assertIn("bookSummaryPending", on_enter)
        self.assertIn("firstRenderDone", loop)
        self.assertIn("!homeInputHeld", loop)
        self.assertIn("loadRecentNonEpubReadingStats()", loop)
        self.assertIn("loadBookSummary()", loop)

    def test_home_reuses_verified_xtc_for_deferred_cover_generation(self):
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        stats = home[home.index("bool HomeActivity::loadRecentNonEpubReadingStats") :]
        loop = home[home.index("void HomeActivity::loop()") : home.index("void HomeActivity::render(RenderLock&&)")]
        idle_preparation = loop[
            loop.index("if (!needsShared && !needsCarousel") : loop.index("const int carouselWidth")
        ]
        selection = home[home.index("void HomeActivity::onSelectBook") :
                         home.index("void HomeActivity::onFileBrowserOpen")]
        preparation = home[
            home.index("HomeActivity::SourcePreparationResult HomeActivity::stepPreparedXtc") :
            home.index("HomeActivity::SourcePreparationResult HomeActivity::stepRecentNonEpubSummarySource")
        ]

        self.assertIn("preparedXtc->stepLoad(XTC_RECORDS_PER_STEP, SOURCE_FINGERPRINT_BYTES_PER_STEP)", preparation)
        self.assertIn("preparedXtc->getSourceIdentity(currentIdentity)", stats)
        self.assertIn("stepPreparedXtc(path)", loop)
        self.assertIn("preparedXtc->beginThumbnailPreparation", loop)
        self.assertIn("preparedXtc->stepThumbnailPreparation(1024, 8)", loop)
        self.assertIn("preparedXtc->getSourceIdentityHandoff(preparedIdentity)", selection)
        self.assertIn("preparedTxt->getSourceIdentityHandoff(preparedIdentity)", selection)
        self.assertIn("openBookWithFeedback(std::move(preparedXtc)", selection)
        self.assertIn("openBookWithFeedback(std::move(preparedTxt)", selection)
        self.assertIn("preparedEpub.reset();", preparation)
        self.assertIn("preparedTxt.reset();", preparation)
        self.assertNotIn("xtc->load()", loop)
        self.assertIn("stepPreparedEpub", idle_preparation)
        self.assertNotIn("stepPreparedXtc", idle_preparation)
        self.assertNotIn("stepPreparedTxt", idle_preparation)

    def test_library_hands_only_the_selected_prepared_xtc_identity_to_reader(self):
        recent = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        queue = recent[recent.index("void RecentBooksActivity::processCoverQueue") :
                       recent.index("int RecentBooksActivity::noticeHeight")]
        preparation = recent[recent.index("void RecentBooksActivity::processSelectedSourcePreparation") :
                             recent.index("int RecentBooksActivity::noticeHeight")]
        opening = recent[recent.index("void RecentBooksActivity::openSelectedBook") :
                         recent.index("void RecentBooksActivity::restoreRememberedBook")]

        self.assertIn("offset == coverQueueSelected", queue)
        self.assertIn("xtc.getSourceIdentityHandoff(preparedIdentity)", queue)
        self.assertIn("xtc.stepThumbnailPreparation(1024, 8)", queue)
        self.assertIn("preparedXtc = std::move(coverPreparationXtc)", queue)
        self.assertIn("openBookWithFeedback(std::move(preparedXtc)", opening)
        self.assertIn("preparedXtcSourceIdentity->path == path", opening)
        self.assertIn("openBookWithFeedback(path, ReaderOpenOrigin::Default, false, reusableIdentity)", opening)
        for object_name in ("preparedEpub", "preparedXtc"):
            self.assertIn(f"if ({object_name} && {object_name}->getPath() != book.path) {object_name}.reset();",
                          preparation)
        self.assertNotIn("preparedTxt", preparation)
        self.assertNotIn("stepLoad(", preparation)

    def test_file_browser_scans_cooperatively_with_explicit_bounds(self):
        browser = (REPO_ROOT / "src/activities/home/FileBrowserActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/home/FileBrowserActivity.h").read_text(encoding="utf-8")
        on_enter = browser[browser.index("void FileBrowserActivity::onEnter()") : browser.index("void FileBrowserActivity::onExit()")]
        loop = browser[browser.index("void FileBrowserActivity::loop()") : browser.index("std::string getFileName")]

        self.assertIn("MAX_FILE_ENTRIES", browser)
        self.assertIn("MAX_FILE_NAME_BYTES", browser)
        self.assertIn("stepFileLoad(FILE_SCAN_ENTRIES_PER_TICK)", loop)
        self.assertIn("filesLoading", header)
        self.assertIn("filesTruncated", header)
        self.assertNotIn("for (auto file = root.openNextFile()", on_enter)
        preparation = browser[browser.index("void FileBrowserActivity::processSelectedSourcePreparation") :
                              browser.index("void FileBrowserActivity::loop()")]
        self.assertIn("stepCoreMetadataRead(metadata)", preparation)
        self.assertNotIn("stepLoad(", preparation)
        self.assertNotIn("preparedXtc", preparation)
        self.assertNotIn("preparedTxt", preparation)

    def test_sleep_screen_custom_image_picker_uses_existing_sleep_paths(self):
        browser = (REPO_ROOT / "src/activities/home/FileBrowserActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/home/FileBrowserActivity.h").read_text(encoding="utf-8")
        submenu = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(encoding="utf-8")
        selection = (REPO_ROOT / "src/activities/boot_sleep/SleepImageSelectionStore.h").read_text(encoding="utf-8")

        self.assertIn("PickImage", header)
        self.assertIn("FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename)", browser)
        self.assertIn("mode != Mode::Books && !isDirectory", browser)
        self.assertIn("FileBrowserActivity::Mode::PickImage", submenu)
        self.assertIn("showSleepImageDialog(SETTINGS.sleepScreen)", submenu)
        self.assertIn('NORMAL_BMP_PATH[] = "/sleep.bmp"', selection)
        self.assertIn('OVERLAY_BMP_PATH[] = "/sleep-overlay.bmp"', selection)
        self.assertIn('OVERLAY_PNG_PATH[] = "/sleep-overlay.png"', selection)
        self.assertIn("PngToBmpConverter::pngFileToBmpStreamWithSize", submenu)
        self.assertIn("pngFileToBmpStreamWithSize(input, output, width, height, false)", submenu)
        self.assertIn("SleepImageValidation::overlayPng(sourcePath)", submenu)
        self.assertIn("SleepImageSelectionStore::publish", submenu)
        self.assertIn("SleepFrameStore::discard();", submenu)
        self.assertIn('for (const char* path : {"/.sleep-overlay", "/sleep-overlay"})', submenu)
        self.assertIn('for (const char* directory : {"/.sleep", "/sleep"})', submenu)
        self.assertIn("SleepImageValidation::normalBmp", submenu)
        self.assertIn("STR_SLEEP_IMAGE_SELECT_FILE", submenu)
        self.assertIn("STR_SLEEP_IMAGE_SOURCE_FOLDER", submenu)
        self.assertIn("STR_SLEEP_IMAGE_SOURCE_INVALID", submenu)

    def test_sleep_image_placement_applies_to_cover_custom_and_overlay(self):
        sleep = (REPO_ROOT / "src/activities/boot_sleep/SleepActivity.cpp").read_text(encoding="utf-8")
        placement = (REPO_ROOT / "src/activities/boot_sleep/SleepImagePlacement.h").read_text(encoding="utf-8")
        position = (REPO_ROOT / "src/activities/settings/SleepImagePositionActivity.cpp").read_text(encoding="utf-8")
        position_header = (REPO_ROOT / "src/activities/settings/SleepImagePositionActivity.h").read_text(encoding="utf-8")
        submenu = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(encoding="utf-8")
        settings_list = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        web_page = (REPO_ROOT / "src/network/html/SettingsPage.html").read_text(encoding="utf-8")
        renderer = (REPO_ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")

        self.assertIn("sleepModeUsesSleepImagePlacement(SETTINGS.sleepScreen)", submenu)
        self.assertIn("const showCoverFilter = !quickResume && (sleepScreen === 1 || sleepScreen === 5);", web_page)
        self.assertNotIn("sleepScreenCoverMode", sleep)
        self.assertNotIn("STR_SLEEP_COVER_MODE", submenu)
        self.assertNotIn("STR_SLEEP_COVER_MODE", settings_list)
        self.assertNotIn("row-setting-sleepScreenCoverMode", web_page)
        self.assertIn("SleepImagePlacement placeSleepImage", sleep)
        self.assertIn("calculateSleepImagePlacement", sleep)
        self.assertIn("sourceWidth_", position)
        self.assertIn("sourceHeight_", position)
        self.assertIn("resolveSourceGeometry();", position)
        self.assertNotIn("pageWidth, pageHeight, pageWidth, pageHeight, zoom_", position)
        self.assertIn("sleepScreenImageZoom", submenu)
        self.assertIn("SettingAction::SleepImagePosition", submenu)
        self.assertIn("SLEEP_IMAGE_MIN_ZOOM = 50", placement)
        self.assertIn("SLEEP_IMAGE_MAX_ZOOM = 200", placement)
        self.assertIn("SLEEP_IMAGE_FRONT_ZOOM_STEP = 1", placement)
        self.assertIn("SLEEP_IMAGE_SIDE_ZOOM_STEP = 5", placement)
        self.assertIn("sleepImageMoveStep", placement)
        self.assertIn("shouldResetSleepImageTransform", placement)
        self.assertIn("enum class Mode { Zoom, Position }", position_header)
        self.assertIn("resetTransform();", position)
        self.assertIn("offsetX_", position)
        self.assertIn("offsetY_", position)
        self.assertIn("drawDashedRect", position)
        self.assertIn("renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width, placement.height, 0, 0, true)", sleep)
        self.assertIn("fitScale < 1.0f || allowUpscale", renderer)
        self.assertIn("return drawBitmap1Bit(bitmap, x, y, maxWidth, maxHeight, allowUpscale);", renderer)

        cover = sleep[
            sleep.index("void SleepActivity::renderCoverSleepScreen") :
            sleep.index("void SleepActivity::renderLastScreenSleepScreen")
        ]
        self.assertIn("renderBitmapSleepScreen(bitmap, true, withBookStats);", cover)

    def test_finished_books_open_statistics_instead_of_the_reader(self):
        menu = (REPO_ROOT / "src/activities/reader/ReadingStatsMenuActivity.cpp").read_text(encoding="utf-8")
        finished = (REPO_ROOT / "src/activities/reader/FinishedBooksActivity.cpp").read_text(encoding="utf-8")
        stats = (REPO_ROOT / "src/activities/reader/ReadingStatsActivity.cpp").read_text(encoding="utf-8")
        history = (REPO_ROOT / "src/activities/reader/BookReadingHistoryActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("StrId::STR_FINISHED_BOOKS", menu)
        self.assertIn("StrId::STR_STATS_FINISHED_BOOKS_SUBTITLE", menu)
        self.assertIn("std::make_unique<FinishedBooksActivity>", menu)
        self.assertIn("loadBookStatsPresentation", finished)
        self.assertIn("LIBRARY_CATALOG.loadRecord", finished)
        self.assertIn("loadTrustedBookReadingStats", finished)
        self.assertIn("stats.isCompleted", finished)
        self.assertIn("LIBRARY_CATALOG.loadRecords", finished)
        self.assertNotIn("FINISHED_BOOKS.", finished)
        self.assertIn("ReadingStatsActivity", finished)
        self.assertNotIn("openBookWithFeedback", finished)
        self.assertIn("renderCompletedBookSummary", stats)
        self.assertIn("BookReadingHistoryActivity", stats)
        self.assertIn("openNextFile", history)
        self.assertIn("DailyBookReadingHistory::load", history)
        self.assertIn("ClockDateFormat::format", history)
        self.assertIn("ReadingCalendarRenderer::formatDuration", history)

        day_detail = (REPO_ROOT / "src/activities/reader/ReadingDayDetailActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("ClockDateFormat::format", day_detail)
        self.assertIn("ReadingCalendarRenderer::formatDuration", day_detail)

    def test_reading_achievements_paginate_open_details_and_use_trophy_icon(self):
        activity = (REPO_ROOT / "src/activities/reader/ReadingAchievementsActivity.cpp").read_text(
            encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/ReadingAchievementsActivity.h").read_text(encoding="utf-8")
        achievements = (REPO_ROOT / "src/activities/reader/ReadingAchievements.cpp").read_text(encoding="utf-8")
        menu = (REPO_ROOT / "src/activities/reader/ReadingStatsMenuActivity.cpp").read_text(encoding="utf-8")
        theme = (REPO_ROOT / "src/components/themes/crossvi/CrossViTheme.cpp").read_text(encoding="utf-8")

        self.assertIn("STR_ACHIEVEMENT_PAGE_FORMAT", activity)
        self.assertIn("CATEGORY_TITLES.size()", activity)
        self.assertIn("mappedInput.wasReleased(MappedInputManager::Button::Confirm)", activity)
        self.assertIn("showDetail_ = true;", activity)
        self.assertIn("showDetail_ = false;", activity)
        self.assertIn("unlockRecognitionDay", activity)
        self.assertIn("bool showDetail_ = false;", header)
        self.assertIn("PAYLOAD_VERSION = 3", achievements)
        self.assertIn("LEGACY_PAYLOAD_VERSION = 1", achievements)
        self.assertIn("UIIcon::Trophy", menu)
        self.assertIn("return TrophyIcon;", theme)

    def test_all_readers_record_the_per_book_daily_breakdown_after_canonical_stats_save(self):
        for filename in ("EpubReaderActivity.cpp", "TxtReaderActivity.cpp", "XtcReaderActivity.cpp"):
            reader = (REPO_ROOT / "src/activities/reader" / filename).read_text(encoding="utf-8")
            commit_start = reader.index("::commitReadingSession()")
            save_start = reader.index("::saveReadingStats()", commit_start)
            next_function = reader.index("::markBookCompleted()", save_start)
            commit = reader[commit_start:save_start]
            save = reader[save_start:next_function]
            self.assertIn("dailyBookHistoryPending =", commit, filename)
            self.assertNotIn("DailyBookReadingHistory::record", commit, filename)
            self.assertIn("!bookReadingStatsDirty && !globalReadingStatsDirty", save, filename)
            self.assertIn("DailyBookReadingHistory::record", save, filename)
            self.assertIn("pendingGlobalReadingSpans.pendingDailyHistory", save, filename)

    def test_file_browser_guards_rendered_list_mutations(self):
        browser = (REPO_ROOT / "src/activities/home/FileBrowserActivity.cpp").read_text(encoding="utf-8")
        loop = browser[browser.index("void FileBrowserActivity::loop()") : browser.index("std::string getFileName")]
        long_back_start = loop.index("// Long press BACK")
        long_back = loop[long_back_start : loop.index("const int pathReserved", long_back_start)]
        confirm_start = loop.index("if (mappedInput.wasReleased(MappedInputManager::Button::Confirm))",
                                   long_back_start)
        back_start = loop.index("if (mappedInput.wasReleased(MappedInputManager::Button::Back))", confirm_start)
        confirm = loop[confirm_start : back_start]
        back = loop[back_start : loop.index("int listSize", back_start)]
        search_handler = browser[browser.index("void FileBrowserActivity::launchSearch()") :
                                 browser.index("void FileBrowserActivity::applySearch")]
        delete_handler = browser[browser.index("void FileBrowserActivity::promptDelete") :
                                 browser.index("void FileBrowserActivity::showBookActions")]

        self.assertLess(long_back.index("RenderLock lock(*this)"), long_back.index('basepath = "/"'))
        self.assertLess(confirm.index("RenderLock lock(*this)"), confirm.index("visibleEntry(selectorIndex)"))
        self.assertLess(confirm.index("lock.unlock()"), confirm.index("openPreparedBook(fullPath)"))
        self.assertLess(back.index("RenderLock lock(*this)"), back.index("clearSearch(true)"))
        self.assertLess(back.rindex("RenderLock lock(*this)"), back.rindex("loadFiles(dirName)"))
        self.assertLess(search_handler.index("RenderLock lock(*this)"), search_handler.index("applySearch("))
        self.assertLess(delete_handler.index("RenderLock lock(*this)"), delete_handler.index("loadFiles("))

    def test_network_servers_release_font_caches_before_allocation(self):
        cases = (
            ("src/activities/network/CalibreConnectActivity.cpp",
             "void CalibreConnectActivity::startWebServer()",
             "makeUniqueNoThrow<CrossPointWebServer>()"),
            ("src/activities/network/CrossPointWebServerActivity.cpp",
             "void CrossPointWebServerActivity::startWebServer()",
             "webServer.reset(new (std::nothrow) CrossPointWebServer())"),
        )
        for relative_path, signature, allocation in cases:
            source = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
            start = source[source.index(signature) :]
            start = start[:start.index("\n}")]
            self.assertLess(start.index("RenderLock lock(*this)"), start.index("clearCache()"))
            self.assertLess(start.index("clearCache()"), start.index(allocation))
            self.assertLess(start.index(allocation), start.index("webServer->begin()"))
            self.assertLess(start.index("webServer->begin()"), start.index("lock.unlock()"))

    def test_bmp_viewer_displays_before_bounded_cooperative_sibling_scan(self):
        viewer = (REPO_ROOT / "src/activities/util/BmpViewerActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/util/BmpViewerActivity.h").read_text(encoding="utf-8")
        on_enter = viewer[viewer.index("void BmpViewerActivity::onEnter()") :
                          viewer.index("void BmpViewerActivity::onExit()")]
        step = viewer[viewer.index("bool BmpViewerActivity::stepSiblingImageScan") :
                      viewer.index("void BmpViewerActivity::finishSiblingImageScan")]
        loop = viewer[viewer.index("void BmpViewerActivity::loop()") :]

        self.assertNotIn("openNextFile", on_enter)
        self.assertIn("if (!loadingFeedbackAlreadyShown)", on_enter)
        self.assertNotIn("fillPopupProgress", on_enter)
        self.assertLess(on_enter.index("renderer.displayBuffer"), on_enter.index("beginSiblingImageScan()"))
        self.assertLess(on_enter.index("finishReaderOpenMetric"), on_enter.index("beginSiblingImageScan()"))
        self.assertIn("scanned < maxEntries", step)
        self.assertIn("MAX_SIBLING_IMAGES", step)
        self.assertIn("MAX_SIBLING_NAME_BYTES", step)
        self.assertIn("std::lower_bound", step)
        self.assertNotIn("sortFileList", viewer)
        self.assertIn("stepSiblingImageScan(SIBLING_SCAN_ENTRIES_PER_TICK)", loop)
        self.assertIn("return siblingScanActive || navigationHintsPending", header)

    def test_reader_open_surfaces_cache_and_bmp_render_failures(self):
        for relative_path, class_name in (
                ("lib/Epub/Epub.cpp", "Epub"),
                ("lib/Txt/Txt.cpp", "Txt"),
                ("lib/Xtc/Xtc.cpp", "Xtc")):
            source = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
            setup = source[source.index(f"bool {class_name}::setupCacheDir() const") :]
            self.assertIn("if (!Storage.mkdir(cachePath.c_str()))", setup)
            self.assertIn("return false;", setup)

        for filename, object_name in (
                ("EpubReaderActivity.cpp", "epub"),
                ("TxtReaderActivity.cpp", "txt"),
                ("XtcReaderActivity.cpp", "xtc")):
            source = (REPO_ROOT / "src/activities/reader" / filename).read_text(encoding="utf-8")
            self.assertIn(f"if (!{object_name}->setupCacheDir()) pendingBookmarkStorageError = true;", source)

        renderer = (REPO_ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")
        viewer = (REPO_ROOT / "src/activities/util/BmpViewerActivity.cpp").read_text(encoding="utf-8")
        draw = renderer[renderer.index("bool GfxRenderer::drawBitmap(") :
                        renderer.index("bool GfxRenderer::drawBitmap1Bit(")]
        self.assertIn("return false;", draw)
        self.assertIn("if (renderer.drawBitmap(", viewer)
        self.assertIn("tr(STR_PAGE_LOAD_ERROR)", viewer)

    def test_txt_indexing_popup_does_not_refresh_twice(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        render = reader[reader.index("void TxtReaderActivity::render(RenderLock&&)") :
                        reader.index("void TxtReaderActivity::renderCurrentPage")]
        indexing = render[render.index("pageIndexing.load(std::memory_order_acquire)") :
                          render.index("if (initializationFailed)")]

        self.assertIn("GUI.drawPopup(renderer, tr(STR_INDEXING));", indexing)
        self.assertNotIn("renderer.displayBuffer", indexing)

    def test_text_settings_only_retries_failed_saves_at_navigation_boundaries(self):
        text_settings = (REPO_ROOT / "src/activities/settings/TextSettingsActivity.cpp").read_text(encoding="utf-8")
        settings = (REPO_ROOT / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
        persist = text_settings[text_settings.index("void TextSettingsActivity::persistSettings()") :
                                text_settings.index("void TextSettingsActivity::rebuildFontOptions()")]
        loop = text_settings[text_settings.index("void TextSettingsActivity::loop()") :
                             text_settings.index("void TextSettingsActivity::invalidatePreviewLocked()")]
        text_action = settings[settings.index("case SettingAction::TextSettings:") :
                               settings.index("case SettingAction::SleepSettings:")]

        self.assertIn("settingsSavePending_ = !SETTINGS.saveToFile()", persist)
        self.assertIn("if (settingsSavePending_) persistSettings();", loop)
        self.assertNotIn("SETTINGS.saveToFile()", loop)
        self.assertNotIn("SETTINGS.saveToFile()", text_action)

    def test_xtc_thumbnail_pair_advances_source_and_output_in_bounded_steps(self):
        xtc = (REPO_ROOT / "lib/Xtc/Xtc.cpp").read_text(encoding="utf-8")
        pair = xtc[xtc.index("class Xtc::ThumbnailPairJob") : xtc.index("Xtc::Xtc(std::string path")]

        self.assertIn("MAX_THUMBNAIL_SOURCE_CHUNK", pair)
        self.assertIn("sourceFile.read(firstChunk.data(), wanted)", pair)
        self.assertIn("secondPlaneFile.read(secondChunk.data(), wanted)", pair)
        self.assertIn("writeOutputRows(maxOutputRows)", pair)
        self.assertNotIn("malloc(bitmapSize)", pair)

    def test_x3_xtc_reader_streams_without_a_full_page_buffer(self):
        activity = (REPO_ROOT / "src/activities/reader/XtcReaderActivity.cpp").read_text(encoding="utf-8")
        one_bit = activity[activity.index("if (nativeX4Portrait)") : activity.index("bool XtcReaderActivity::saveProgress")]

        self.assertIn("book->loadPageStreaming", one_bit)
        self.assertIn("sourceRowBytes * 8U", one_bit)
        self.assertNotIn("book->loadPage(page", one_bit)
        self.assertNotIn("malloc(pageBufferSize)", one_bit)

    def test_epub_section_reuses_prepared_css_and_fails_closed_on_cache_error(self):
        section = (REPO_ROOT / "lib/Epub/Epub/Section.cpp").read_text(encoding="utf-8")
        start = section[section.index("bool Section::startBuild") : section.index("bool Section::beginParser")]

        self.assertIn("preparedCssParser->hasMaterializedCache()", start)
        self.assertIn("!preparedCssParser->loadFromCache()", start)
        self.assertIn("lastBuildStatus_ = EpubBuildStatus::CacheError", start)
        self.assertNotIn('LOG_ERR("SCT", "Failed to load CSS from cache");\n    }', start)

    def test_corrupt_cached_epub_html_is_evicted_and_retried_once(self):
        header = (REPO_ROOT / "lib/Epub/Epub/Section.h").read_text(encoding="utf-8")
        section = (REPO_ROOT / "lib/Epub/Epub/Section.cpp").read_text(encoding="utf-8")
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        abandon = section[section.index("void Section::abandonBuild()") :
                          section.index("std::unique_ptr<Page> Section::loadPageDuringBuild")]

        self.assertIn("StaleHtmlCache", header)
        self.assertIn("startedWithCachedHtml", header)
        self.assertIn("build_->startedWithCachedHtml", section)
        self.assertIn("Storage.remove(build_->htmlPath.c_str())", abandon)
        self.assertGreaterEqual(reader.count("failure == EpubBuildStatus::StaleHtmlCache"), 3)

    def test_reader_background_layout_yields_to_input_and_bookmarks_parse_once(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        loop = reader[reader.index("void EpubReaderActivity::loop()") :
                      reader.index("bool EpubReaderActivity::handleReaderShortcut")]
        bookmark_io = (REPO_ROOT / "src/BookmarkJsonIO.cpp").read_text(encoding="utf-8")
        load = bookmark_io[bookmark_io.index("JsonSettingsIO::BookmarkLoadStatus JsonSettingsIO::loadBookmarksFromFile") :
                           bookmark_io.index("JsonSettingsIO::BookmarkPathMoveStatus")]

        self.assertGreaterEqual(loop.count("!inputEdge && !readerInputHeld"), 3)
        self.assertIn("BookmarkParseContext parseContext{&bookmarks, metadata}", load)
        self.assertIn("validateBookmarkJson, &parseContext", load)
        self.assertNotIn("loadBookmarks(bookmarks, json.c_str()", load)

        for name in ("Epub", "Txt", "Xtc"):
            activity = (REPO_ROOT / f"src/activities/reader/{name}ReaderActivity.cpp").read_text(encoding="utf-8")
            self.assertIn("readerStateSaveRetryPending = !APP_STATE.saveToFile()", activity)
            self.assertIn("if (readerStateSaveRetryPending)", activity)
            self.assertIn("Could not persist reader exit state", activity)

    def test_reader_open_reuses_verified_primary_source_identity(self):
        reader = (REPO_ROOT / "src/activities/reader/ReaderActivity.cpp").read_text(encoding="utf-8")
        epub = reader[reader.index("std::unique_ptr<Epub> ReaderActivity::loadEpub") :
                      reader.index("bool ReaderActivity::beginXtcLoad")]
        begin_epub = reader[reader.index("bool ReaderActivity::beginEpubLoad") :
                            reader.index("bool ReaderActivity::finishEpubLoad")]
        xtc = reader[reader.index("bool ReaderActivity::finishXtcLoad") :
                     reader.index("bool ReaderActivity::beginTxtLoad")]
        begin_xtc = reader[reader.index("bool ReaderActivity::beginXtcLoad") :
                           reader.index("bool ReaderActivity::finishXtcLoad")]
        begin_txt = reader[reader.index("bool ReaderActivity::beginTxtLoad") :
                           reader.index("bool ReaderActivity::finishTxtLoad")]
        txt = reader[reader.index("bool ReaderActivity::finishTxtLoad") :
                     reader.index("void ReaderActivity::cancelCooperativeOpen")]

        self.assertIn("recoverInterruptedBookFileReplacement(path, &recoveredIdentity", begin_epub)
        self.assertIn("makeUniqueNoThrow<Epub>(path, \"/.crosspoint\", verifiedEpubIdentity)", epub)
        self.assertIn("ZipSourceIdentityJob", reader)
        self.assertIn("loadForCooperativeSourceCheck", epub)
        self.assertNotIn("ZipFile::getSourceIdentity", epub)
        self.assertIn("inspectSourceBindingForLoad()", epub)
        self.assertIn("if (bindingStatus != Epub::SourceBindingStatus::Match)", epub)
        self.assertGreaterEqual(begin_epub.count("hasBookFileReplacementArtifacts(path)"), 2)
        self.assertIn("preparedSourceIdentity->matchesOpenFile(path, preparedFile)", begin_epub)
        self.assertIn("Storage.probeMedia()", begin_epub)
        self.assertIn("return finishEpubLoad(preparedSourceIdentity->identity)", begin_epub)
        self.assertIn("prepareForReaderLoadAfterRecovery(verifiedEpubIdentity)", epub)
        self.assertIn("openingXtc = std::move(preparedXtc)", begin_xtc)
        self.assertIn("openingTxt = std::move(preparedTxt)", begin_txt)
        for loader in (xtc, txt):
            self.assertIn("identityStatus == SourceIdentityStore::LoadStatus::Primary", loader)
            self.assertIn("if (!identityAlreadyPrimary)", loader)
        for loader in (begin_xtc, begin_txt):
            self.assertGreaterEqual(loader.count("hasBookFileReplacementArtifacts(path)"), 2)
            self.assertIn("Storage.probeMedia()", loader)
            self.assertIn("beginLoad(reusableIdentity)", loader)

    def test_xtc_cover_generation_waits_for_first_page_and_idle_input(self):
        reader = (REPO_ROOT / "src/activities/reader/ReaderActivity.cpp").read_text(encoding="utf-8")
        xtc_loader = reader[reader.index("bool ReaderActivity::finishXtcLoad") :
                            reader.index("bool ReaderActivity::beginTxtLoad")]
        activity = (REPO_ROOT / "src/activities/reader/XtcReaderActivity.cpp").read_text(encoding="utf-8")
        pump = activity[activity.index("void XtcReaderActivity::pumpDeferredCoverPreparation") :
                        activity.index("void XtcReaderActivity::openGoToPage")]
        loop = activity[activity.index("void XtcReaderActivity::loop()") :
                        activity.index("bool XtcReaderActivity::handleReaderShortcut")]

        self.assertNotIn("generateThumbBmpPair", xtc_loader)
        self.assertIn("deferCoverPreparation = needsShared || needsCarousel", xtc_loader)
        self.assertIn("lastSuccessfullyRenderedPage.load", pump)
        self.assertIn("RenderLock lock(std::try_to_lock)", pump)
        self.assertIn("beginThumbnailPreparation", pump)
        self.assertIn("stepThumbnailPreparation(1024, 8)", pump)
        self.assertIn("ThumbnailPreparationStatus::InProgress) return", pump)
        self.assertIn("const bool inputEdge = mappedInput.wasAnyPressed() || mappedInput.wasAnyReleased()", loop)
        self.assertIn("!inputEdge && !readerInputHeld", loop)
        self.assertIn("pumpDeferredCoverPreparation()", loop)

    def test_txt_and_xtc_reader_open_runs_in_bounded_cancellable_steps(self):
        reader = (REPO_ROOT / "src/activities/reader/ReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/ReaderActivity.h").read_text(encoding="utf-8")
        pump = reader[reader.index("void ReaderActivity::pumpCooperativeOpen") :
                      reader.index("void ReaderActivity::goToLibrary")]

        self.assertIn("openingXtc->stepLoad(RECORDS_PER_STEP, FINGERPRINT_BYTES_PER_STEP)", pump)
        self.assertIn("openingTxt->stepLoad(FINGERPRINT_BYTES_PER_STEP)", pump)
        self.assertIn("openingEpub->stepCacheInspection(CACHE_ENTRIES_PER_STEP)", pump)
        self.assertIn("openingEpub->stepIndexing()", pump)
        self.assertIn("mappedInput.wasPressed(MappedInputManager::Button::Back)", pump)
        self.assertIn("cancelCooperativeOpen()", pump)
        self.assertIn("openingEpubFrameBufferLoan.reset()", pump)
        self.assertIn("openingEpub->cancelCacheInspection()", reader)
        self.assertIn("openingEpub->cancelIndexing()", reader)
        self.assertIn(
            "bool skipLoopDelay() override { return openingXtc || openingTxt || openingEpubIdentityJob || openingEpub; }",
            header,
        )

    def test_home_library_and_reader_share_cooperative_epub_preparation(self):
        epub = (REPO_ROOT / "lib/Epub/Epub.cpp").read_text(encoding="utf-8")
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        library = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        reader = (REPO_ROOT / "src/activities/reader/ReaderActivity.cpp").read_text(encoding="utf-8")

        home_prepare = home[home.index("HomeActivity::SourcePreparationResult HomeActivity::stepPreparedEpub") :
                            home.index("HomeActivity::SourcePreparationResult HomeActivity::stepPreparedXtc")]
        library_covers = library[library.index("void RecentBooksActivity::processCoverQueue") :
                                  library.index("int RecentBooksActivity::noticeHeight")]
        thumbnail_begin = epub[epub.index("Epub::ThumbnailPreparationStatus Epub::beginThumbnailPreparation") :
                               epub.index("Epub::ThumbnailPreparationStatus Epub::stepThumbnailPreparation")]
        reader_pump = reader[reader.index("void ReaderActivity::pumpCooperativeOpen") :
                             reader.index("void ReaderActivity::goToLibrary")]

        self.assertIn("beginCoreMetadataRead()", home_prepare)
        self.assertIn("stepCoreMetadataRead(metadata)", home_prepare)
        self.assertIn("getSourceIdentityHandoff(preparedIdentity)", home)
        self.assertIn("beginCoreMetadataRead()", library_covers)
        self.assertIn("stepCoreMetadataRead(metadata)", library_covers)
        self.assertIn("preparedEpubSourceIdentity", library_covers)
        self.assertIn("ThumbnailPreparationStatus::NeedsCoreMetadata", thumbnail_begin)
        self.assertNotIn("readCoreMetadata(", thumbnail_begin)
        self.assertIn("openingEpubFinalIdentityCheck = true", reader)
        self.assertIn("identity != openingEpubExpectedIdentity", reader_pump)

    def test_xtc_reader_defers_saved_items_until_they_are_needed(self):
        activity = (REPO_ROOT / "src/activities/reader/XtcReaderActivity.cpp").read_text(encoding="utf-8")
        on_enter = activity[activity.index("void XtcReaderActivity::onEnter()") :
                            activity.index("void XtcReaderActivity::onExit()")]
        menu = activity[activity.index("void XtcReaderActivity::openReaderMenu()") :
                        activity.index("void XtcReaderActivity::pumpDeferredCoverPreparation")]
        chapters = activity[activity.index("void XtcReaderActivity::openChapterSelection()") :
                            activity.index("void XtcReaderActivity::loop()")]
        toggle = activity[activity.index("bool XtcReaderActivity::toggleBookmark()") :
                          activity.index("void XtcReaderActivity::openSavedItems()")]

        self.assertNotIn("loadBookmarks()", on_enter)
        self.assertNotIn("getChapters()", on_enter)
        self.assertIn("ensureBookmarksLoaded()", menu)
        self.assertIn("const bool hasChapters = xtc->hasChapters()", menu)
        self.assertNotIn("getChapters()", menu)
        self.assertIn("RenderLock lock", chapters)
        self.assertIn("getChapters()", chapters)
        self.assertIn("ensureBookmarksLoaded()", toggle)

    def test_reader_open_recovers_completion_once_and_skips_unchanged_json_writes(self):
        reader = (REPO_ROOT / "src/activities/reader/ReaderActivity.cpp").read_text(encoding="utf-8")
        dispatch = reader[reader.index("void ReaderActivity::onEnter()") : reader.index("void ReaderActivity::onGoBack")]
        move = (REPO_ROOT / "src/util/BookPathMoveUtils.cpp").read_text(encoding="utf-8")
        recovery = move[move.index("bool recoverInterruptedBookFileReplacement") :
                        move.index("BookFilePublishResult publishStagedBookFile")]
        recent = (REPO_ROOT / "src/RecentBooksStore.cpp").read_text(encoding="utf-8")
        add_recent = recent[recent.index("void RecentBooksStore::addBook") :
                            recent.index("void RecentBooksStore::updateBook")]

        self.assertEqual(dispatch.count("ReadingStatsCompletionTransaction::recoverPending()"), 1)
        self.assertIn("if (!completionStatsAlreadyRecovered)", dispatch)
        self.assertIn("completionStatsWritableAtOpen = completionRecovery !=", dispatch)
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("ReaderOpenOrigin::HomeRecent, completionStatsAlreadyRecovered", home)
        self.assertIn("!completionTransactionResolved && !canDeleteOrRelocateBookFile(bookPath)", recovery)
        for name in ("EpubReaderActivity", "TxtReaderActivity", "XtcReaderActivity"):
            activity = (REPO_ROOT / f"src/activities/reader/{name}.cpp").read_text(encoding="utf-8")
            on_enter = activity[activity.index(f"void {name}::onEnter()") :
                                activity.index(f"void {name}::onExit()")]
            self.assertNotIn("recoverPending()", on_enter)
            self.assertIn("completionStatsWritableAtOpen", on_enter)
            self.assertIn("if (APP_STATE.openEpubPath !=", on_enter)

        self.assertIn("const RecentBook& current = recentBooks.front()", add_recent)
        self.assertIn("current.coverBmpPath == coverBmpPath", add_recent)
        self.assertLess(add_recent.index("current.coverBmpPath == coverBmpPath"), add_recent.index("recentBooks.erase"))

    def test_txt_loads_initial_progress_only_after_the_page_index_is_complete(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.h").read_text(encoding="utf-8")
        initialize = reader[reader.index("void TxtReaderActivity::initializeReader") :
                            reader.index("void TxtReaderActivity::finishReaderInitialization")]
        indexing = reader[reader.index("void TxtReaderActivity::processPageIndex") :
                          reader.index("bool TxtReaderActivity::buildPageIndexBatch")]
        load = reader[reader.index("void TxtReaderActivity::loadProgress()") :
                      reader.index("bool TxtReaderActivity::loadPageIndexCache()")]

        self.assertIn("std::optional<uint32_t> initialProgressOffset;", header)
        self.assertNotIn("ProgressFile::loadTxt", initialize)
        complete = indexing.index("else if (pageIndexComplete)")
        self.assertGreater(indexing.index("finishReaderInitialization();", complete), complete)
        self.assertLess(load.index("if (initialProgressOffset)"), load.index("ProgressFile::loadTxt"))
        self.assertIn("initialProgressOffset.reset();", load)
        self.assertGreaterEqual(load.count("ProgressFile::loadTxt"), 2)

    def test_txt_bom_and_saved_offsets_fail_closed(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.h").read_text(encoding="utf-8")
        self.assertIn("constexpr uint8_t CACHE_VERSION = 7", reader)
        self.assertIn("SourceIdentityCodec::Encoded: raw file source identity", reader)
        self.assertIn("storedIdentity != currentIdentity", reader)
        self.assertIn("TxtLineWrap::leadingUtf8BomBytes(buffer, chunkSize)", reader)
        self.assertIn("bool jumpToStoredByteOffset(uint32_t byteOffset);", header)
        helper = reader[reader.index("bool TxtReaderActivity::jumpToStoredByteOffset") :]
        helper = helper[: helper.index("\n}") + 2]
        self.assertIn("byteOffset >= txt->getFileSize()", helper)
        initialization = reader[reader.index("void TxtReaderActivity::finishReaderInitialization") :
                                reader.index("bool TxtReaderActivity::ensureContentReadSession")]
        self.assertIn("initialBookmarkJump->textByteOffset >= txt->getFileSize()", initialization)
        self.assertGreaterEqual(reader.count("jumpToStoredByteOffset("), 4)

    def test_thick_rectangle_border_stays_inside_declared_bounds(self):
        renderer = (REPO_ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")
        overload = renderer[renderer.index("// Border is inside the rectangle") :]
        overload = overload[: overload.index("void GfxRenderer::drawArc")]
        self.assertIn("x + width - 1 - i", overload)
        self.assertIn("y + height - 1 - i", overload)
        self.assertNotIn("x + width - i", overload)
        self.assertNotIn("y + height - i", overload)

    def test_end_of_book_consumes_screenshot_and_draws_notices_before_display(self):
        for source_path, marker, notice in (
            (
                "src/activities/reader/EpubReaderActivity.cpp",
                "if (currentSpineIndex == epub->getSpineItemsCount())",
                "showPendingSyncSaveError();",
            ),
            (
                "src/activities/reader/XtcReaderActivity.cpp",
                "if (page >= book->getPageCount())",
                "GUI.drawPopup",
            ),
        ):
            reader = (REPO_ROOT / source_path).read_text(encoding="utf-8")
            branch = reader[reader.index(marker) :]
            branch = branch[: branch.index("return;\n  }") + len("return;\n  }")]
            self.assertIn("pendingScreenshot.exchange(false", branch)
            self.assertLess(branch.index(notice), branch.index("renderer.displayBuffer()"))
            self.assertLess(branch.index("renderer.displayBuffer()"), branch.index("ScreenshotUtil::takeScreenshot"))

    def test_end_of_book_shows_live_book_summary_and_opens_detailed_stats(self):
        options = (REPO_ROOT / "src/activities/reader/EndOfBookOptions.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/EndOfBookOptions.h").read_text(encoding="utf-8")
        finder = (REPO_ROOT / "src/util/NextBookFinder.cpp").read_text(encoding="utf-8")

        self.assertIn("const EndOfBookSummary& summary", header)
        self.assertIn("Action::ViewStats", options)
        self.assertIn("STR_STATS_READING_TIME", options)
        self.assertIn("STR_STATS_SESSIONS", options)
        self.assertIn("STR_STATS_PAGES_TURNED", options)
        self.assertIn("STR_STATS_DAYS_TO_FINISH", options)
        self.assertNotIn("cover", options.lower())
        self.assertIn("stepSuggestions(size_t maxEntries)", header)
        self.assertIn("suggestionScan.step(maxEntries)", options)
        self.assertIn("processed < maxEntries", finder)
        self.assertNotIn("findNextBooks(", options)

        for reader_name in ("EpubReaderActivity", "XtcReaderActivity"):
            reader = (REPO_ROOT / f"src/activities/reader/{reader_name}.cpp").read_text(encoding="utf-8")
            self.assertIn("case EndOfBookOptions::Action::ViewStats:", reader)
            self.assertIn("previewReadingStatsSession", reader)
            self.assertIn("EndOfBookSummary{", reader)
            self.assertIn("endOfBookOptions.stepSuggestions(8)", reader)

    def test_sd_font_discovery_caps_allocations_while_scanning(self):
        registry = (REPO_ROOT / "lib/EpdFont/SdCardFontRegistry.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "lib/EpdFont/SdCardFontRegistry.h").read_text(encoding="utf-8")
        download = (REPO_ROOT / "src/activities/settings/FontDownloadActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("MAX_FILES_PER_FAMILY = 16", header)
        self.assertIn("while (family.files.size() < MAX_FILES_PER_FAMILY)", registry)
        self.assertIn("while (out.size() < static_cast<size_t>(MAX_SD_FAMILIES))", registry)
        self.assertIn("SdCardFontRegistry::MAX_FILES_PER_FAMILY", download)

    def test_crosspoint_sync_extension_is_not_sent_to_custom_servers(self):
        credential_header = (REPO_ROOT / "lib/KOReaderSync/KOReaderCredentialStore.h").read_text(encoding="utf-8")
        credentials = (REPO_ROOT / "lib/KOReaderSync/KOReaderCredentialStore.cpp").read_text(encoding="utf-8")
        client = (REPO_ROOT / "lib/KOReaderSync/KOReaderSyncClient.cpp").read_text(encoding="utf-8")
        activity = (REPO_ROOT / "src/activities/reader/KOReaderSyncActivity.cpp").read_text(encoding="utf-8")
        settings = (REPO_ROOT / "src/activities/settings/KOReaderSettingsActivity.cpp").read_text(encoding="utf-8")
        self.assertIn('DEFAULT_SERVER_URL[] = "https://sync.koreader.rocks:443"', credentials)
        self.assertIn(
            "KOReaderSyncBehavior syncBehavior = KOReaderSyncBehavior::ASK_EVERY_TIME;",
            credential_header,
        )
        missing_store_defaults = credentials[
            credentials.index("bool KOReaderCredentialStore::loadFromFile()") :
            credentials.index("bool KOReaderCredentialStore::ensureLoaded()")
        ]
        self.assertIn("syncBehavior = KOReaderSyncBehavior::ASK_EVERY_TIME;", missing_store_defaults)
        self.assertIn('CROSSPOINT_SERVER_URL[] = "https://sync.crosspointreader.com"', credentials)
        self.assertIn("getBaseUrl() == CROSSPOINT_SERVER_URL", credentials)
        self.assertIn("const std::string prefillUrl = KOREADER_STORE.getBaseUrl();", settings)
        self.assertGreaterEqual(client.count("KOREADER_STORE.usesCrossPointSyncServer()"), 2)
        self.assertIn("if (KOREADER_STORE.usesCrossPointSyncServer())", activity)

    def test_koreader_sync_releases_font_caches_before_tls(self):
        activity = (REPO_ROOT / "src/activities/reader/KOReaderSyncActivity.cpp").read_text(encoding="utf-8")
        fetch = activity[activity.index("void KOReaderSyncActivity::performSync()") :]
        upload = activity[activity.index("void KOReaderSyncActivity::performUpload()") :]
        self.assertLess(fetch.index("clearAllCaches()"), fetch.index("KOReaderSyncClient::getProgress"))
        self.assertLess(
            fetch.index("showBlockingFeedback(StrId::STR_LOADING_POPUP);"),
            fetch.index("KOReaderSyncClient::getProgress"),
        )
        render = activity[activity.index("void KOReaderSyncActivity::render(RenderLock&&)") :]
        self.assertIn("if (renderBlockingFeedbackOverlay()) return;", render)
        self.assertLess(upload.index("clearAllCaches()"), upload.index("KOReaderSyncClient::updateProgress"))

        auth = (REPO_ROOT / "src/activities/settings/KOReaderAuthActivity.cpp").read_text(encoding="utf-8")
        authenticate = auth[auth.index("void KOReaderAuthActivity::onWifiSelectionComplete") :
                            auth.index("void KOReaderAuthActivity::performAuthentication")]
        self.assertLess(authenticate.index("requestUpdateAndWait()"), authenticate.index("clearAllCaches()"))
        self.assertLess(authenticate.index("clearAllCaches()"), authenticate.index("performAuthentication();"))

        for source_path, activity_name in (
            ("src/activities/reader/EpubReaderActivity.cpp", "EpubReaderActivity"),
            ("src/activities/reader/TxtReaderActivity.cpp", "TxtReaderActivity"),
            ("src/activities/reader/XtcReaderActivity.cpp", "XtcReaderActivity"),
        ):
            reader = (REPO_ROOT / source_path).read_text(encoding="utf-8")
            on_exit = reader[reader.index(f"void {activity_name}::onExit()") :
                             reader.index(f"void {activity_name}::onPause()")]
            self.assertIn("clearAllCaches()", on_exit)

        epub_reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        shortcut = epub_reader[epub_reader.index("case CrossPointSettings::LP_MENU_KOSYNC:") :
                               epub_reader.index("case CrossPointSettings::LP_MENU_DICTIONARY:")]
        menu_sync = epub_reader[epub_reader.index("case EpubReaderMenuActivity::MenuAction::SYNC:") :
                                epub_reader.index("case EpubReaderMenuActivity::MenuAction::NEARBY_POSITION_SYNC:")]
        self.assertIn("launchKOReaderSync()", shortcut)
        self.assertIn("launchKOReaderSync()", menu_sync)
        self.assertIn(
            "activityManager.replaceActivity(std::make_unique<KOReaderSyncActivity>", epub_reader
        )

    def test_koreader_wifi_cancel_consumes_back_release_before_reopening_reader(self):
        activity = (REPO_ROOT / "src/activities/reader/KOReaderSyncActivity.cpp").read_text(encoding="utf-8")
        cancel = activity[
            activity.index("void KOReaderSyncActivity::onWifiSelectionComplete") :
            activity.index("void KOReaderSyncActivity::performSync")
        ]
        self.assertIn("returnAfterWifiCancel = true;", cancel)
        self.assertIn("suppressWifiCancelBackRelease = true;", cancel)
        self.assertNotIn("returnToReader();", cancel)

        loop = activity[activity.index("void KOReaderSyncActivity::loop()") :]
        drain = loop[
            loop.index("if (returnAfterWifiCancel)") :
            loop.index("if (ReaderUtils::consumeInitialRelease(suppressInitialConfirmRelease")
        ]
        self.assertIn("consumeInitialRelease(suppressWifiCancelBackRelease", drain)
        self.assertIn("mappedInput.wasReleased(MappedInputManager::Button::Back)", drain)
        self.assertIn("mappedInput.isPressed(MappedInputManager::Button::Back)", drain)
        self.assertLess(drain.index("consumeInitialRelease"), drain.index("returnToReader();"))

    def test_sd_reader_font_is_released_outside_active_page_rendering(self):
        system_header = (REPO_ROOT / "src/SdCardFontSystem.h").read_text(encoding="utf-8")
        system_source = (REPO_ROOT / "src/SdCardFontSystem.cpp").read_text(encoding="utf-8")
        self.assertIn("void releaseLoadedFont(GfxRenderer& renderer);", system_header)
        release = system_source[
            system_source.index("void SdCardFontSystem::releaseLoadedFont") :
            system_source.index("void SdCardFontSystem::ensureLoaded")
        ]
        self.assertLess(release.index("clearAllCaches()"), release.index("manager_.unloadAll(renderer)"))
        self.assertIn("keeping selection for retry", system_source)

        font_selection = (REPO_ROOT / "src/activities/settings/FontSelectionActivity.cpp").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("currentSupportsVietnamese()", font_selection)
        font_on_enter = font_selection[
            font_selection.index("void FontSelectionActivity::onEnter()") :
            font_selection.index("void FontSelectionActivity::onExit()")
        ]
        self.assertNotIn("sdFontSystem.ensureLoaded", font_on_enter)
        self.assertIn("if (sdPreview && customPreviewPending_)", font_selection)
        self.assertIn("sdFontSystem.ensureLoaded(renderer, false);", font_selection)
        self.assertIn("renderer.copyRegionToBuffer", font_selection)
        self.assertIn("renderer.copyBufferToRegion", font_selection)
        self.assertLess(
            font_selection.index("renderer.copyRegionToBuffer"),
            font_selection.index("sdFontSystem.releaseLoadedFont(renderer);", font_selection.index("void FontSelectionActivity::render(")),
        )

        for source_path in ("src/activities/settings/FontSizeSelectionActivity.cpp",):
            settings_preview = (REPO_ROOT / source_path).read_text(encoding="utf-8")
            self.assertIn("const int fontId = sdFontSelected ? 0 : SETTINGS.getReaderFontId();", settings_preview)

        for source_path, activity_name in (
            ("src/activities/reader/EpubReaderActivity.cpp", "EpubReaderActivity"),
            ("src/activities/reader/TxtReaderActivity.cpp", "TxtReaderActivity"),
        ):
            reader = (REPO_ROOT / source_path).read_text(encoding="utf-8")
            on_exit = reader[reader.index(f"void {activity_name}::onExit()") :
                             reader.index(f"void {activity_name}::onPause()")]
            on_pause = reader[reader.index(f"void {activity_name}::onPause()") :
                              reader.index(f"void {activity_name}::onResume()")]
            resume_start = reader.index(f"void {activity_name}::onResume()")
            resume_end = reader.find("\nvoid ", resume_start + 5)
            on_resume = reader[resume_start : resume_end if resume_end >= 0 else len(reader)]
            self.assertIn("releaseLoadedFont(renderer)", on_exit)
            self.assertIn("releaseLoadedFont(renderer)", on_pause)
            self.assertIn("ensureLoaded(renderer, false)", on_resume)

        text_settings = (REPO_ROOT / "src/activities/settings/TextSettingsActivity.cpp").read_text(
            encoding="utf-8"
        )
        self.assertIn("sdFontSystem.ensureLoaded(renderer, false);", text_settings)
        self.assertIn("renderer.copyRegionToBuffer", text_settings)
        self.assertIn("renderer.copyBufferToRegion", text_settings)
        self.assertLess(
            text_settings.index("renderer.copyRegionToBuffer"),
            text_settings.index(
                "sdFontSystem.releaseLoadedFont(renderer);",
                text_settings.index("void TextSettingsActivity::render("),
            ),
        )

        for source_path in (
            "src/activities/settings/FontSizeSelectionActivity.cpp",
            "src/activities/reader/BookReaderSettingsActivity.cpp",
        ):
            settings_activity = (REPO_ROOT / source_path).read_text(encoding="utf-8")
            self.assertIn("releaseLoadedFont(renderer)", settings_activity)
            self.assertNotIn("sdFontSystem.ensureLoaded", settings_activity)

        font_download = (REPO_ROOT / "src/activities/settings/FontDownloadActivity.cpp").read_text(encoding="utf-8")
        on_enter = font_download[
            font_download.index("void FontDownloadActivity::onEnter()") :
            font_download.index("void FontDownloadActivity::onExit()")
        ]
        self.assertIn("releaseLoadedFont(renderer)", on_enter)

        for source_path, activity_name in (
            ("src/activities/reader/DictionaryWordSelectActivity.cpp", "DictionaryWordSelectActivity"),
            ("src/activities/reader/ClipSelectionActivity.cpp", "ClipSelectionActivity"),
            ("src/activities/reader/EpubInBookSearchActivity.cpp", "EpubInBookSearchActivity"),
            ("src/activities/reader/ClippingReanchorActivity.cpp", "ClippingReanchorActivity"),
        ):
            page_tool = (REPO_ROOT / source_path).read_text(encoding="utf-8")
            on_enter = page_tool[page_tool.index(f"void {activity_name}::onEnter()") :
                                 page_tool.index(f"void {activity_name}::onExit()")]
            on_exit = page_tool[page_tool.index(f"void {activity_name}::onExit()") :]
            self.assertIn("ensureLoaded(renderer, false)", on_enter)
            self.assertIn("releaseLoadedFont(renderer)", on_exit)

    def test_web_server_does_not_subscribe_request_task_to_watchdog(self):
        header = (REPO_ROOT / "src/network/CrossPointWebServer.h").read_text(encoding="utf-8")
        source = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        self.assertNotIn("watchdogTaskRegistered", header)
        self.assertNotIn("esp_task_wdt_add(nullptr)", source)
        self.assertNotIn("esp_task_wdt_delete(nullptr)", source)

    def test_serial_command_input_is_static_bounded_and_nonblocking(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        pump = main[main.index("void pumpSerialCommands()") : main.index("void loop()")]
        self.assertIn("char serialCommandBuffer[SERIAL_COMMAND_MAX_BYTES + 1]", main)
        self.assertIn("SERIAL_BYTES_PER_LOOP = 32", main)
        self.assertIn("processed < SERIAL_BYTES_PER_LOOP", pump)
        self.assertIn("discardSerialCommand = true", pump)
        self.assertIn("if (byte == '\\n')", pump)
        self.assertNotIn("readStringUntil", main)
        self.assertNotIn("String line", main)

    def test_settings_page_serializes_large_api_requests_and_yields_between_chunks(self):
        page = (REPO_ROOT / "src/network/html/SettingsPage.html").read_text(encoding="utf-8")
        server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        startup = page[page.index("const initialLanguageReady") : page.index("</script>")]
        self.assertLess(startup.rindex("await loadSettings();"), startup.rindex("await loadWifiNetworks();"))
        self.assertLess(startup.rindex("await loadWifiNetworks();"), startup.rindex("await loadOpdsServers();"))

        for handler_name in ("handleGetSettings", "handleGetOpdsServers", "handleGetWifiNetworks"):
            handler = server[server.index(f"void CrossPointWebServer::{handler_name}") :]
            handler = handler[: handler.index("\n}")]
            self.assertIn("server->sendContent(output);", handler)
            self.assertIn("yield();", handler)
            self.assertIn("resetTaskWatchdogIfSubscribed();", handler)

    def test_manual_reader_refresh_rerenders_before_displaying(self):
        manager = (REPO_ROOT / "src/activities/ActivityManager.cpp").read_text(encoding="utf-8")
        epub = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        for header in ("EpubReaderActivity.h", "TxtReaderActivity.h", "XtcReaderActivity.h"):
            source = (REPO_ROOT / "src/activities/reader" / header).read_text(encoding="utf-8")
            self.assertIn("bool handleForcedRefresh() override", source)
            self.assertIn("requestUpdate();", source)
        self.assertIn("if (!handleForcedRefresh())", manager)
        self.assertIn("manualRefreshPending || pagesUntilFullRefresh <= 1", epub)

    def test_epub_background_build_is_heap_gated_and_releases_image_callback(self):
        header = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("BACKGROUND_BUILD_MIN_FREE_HEAP = 32 * 1024", header)
        self.assertIn("BACKGROUND_BUILD_MIN_MAX_ALLOC = 16 * 1024", header)
        self.assertIn("BACKGROUND_BUILD_PAGES_PER_TICK = 1", header)
        # The background tick must gate heap only after it owns the render lock.
        # Counting a second unlocked pre-check would reintroduce the section race.
        self.assertGreaterEqual(reader.count("buildTickHeapGate()"), 2)
        self.assertIn("RenderLock lock(std::try_to_lock);", reader)
        self.assertIn(
            "if (!inputEdge && !readerInputHeld && lock.ownsLock() && !activityManager.hasPendingRender())",
            reader,
        )
        self.assertIn("section && section->isBuilding() && withinBuildWindow", reader)
        self.assertIn("if (targetBuildRequired)", reader)
        self.assertIn("else if (landingWarmupActive)", reader)
        self.assertIn("sectionLandingWarmupPending = false", reader)
        self.assertGreaterEqual(reader.count("ImageBlock::setExtractor(nullptr, nullptr)"), 3)

    def test_reader_background_work_yields_to_queued_renders(self):
        manager = (REPO_ROOT / "src/activities/ActivityManager.h").read_text(encoding="utf-8")
        epub = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        txt = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")

        self.assertIn("bool hasPendingRender() const", manager)
        self.assertIn("requestedUpdate.load(std::memory_order_acquire)", manager)
        self.assertIn("requestedRenderGeneration.load(std::memory_order_acquire)", manager)
        self.assertIn("completedRenderGeneration.load(std::memory_order_acquire)", manager)

        epub_loop = epub[epub.index("void EpubReaderActivity::loop()") :
                         epub.index("void EpubReaderActivity::pageTurn")]
        self.assertGreaterEqual(epub_loop.count("!activityManager.hasPendingRender()"), 3)
        pump = epub[epub.index("bool EpubReaderActivity::pumpImagePreparation") :
                    epub.index("void EpubReaderActivity::pumpDeferredBookmarkLoad")]
        self.assertGreaterEqual(pump.count("activityManager.hasPendingRender()"), 2)

        txt_idle = txt[txt.index("if (!prevTriggered && !nextTriggered)") :
                       txt.index("if (pageGesture.longPress")]
        self.assertIn("!activityManager.hasPendingRender()", txt_idle)

    def test_epub_next_page_image_extraction_is_bounded_and_outside_render(self):
        epub = (REPO_ROOT / "lib/Epub/Epub.cpp").read_text(encoding="utf-8")
        page = (REPO_ROOT / "lib/Epub/Epub/Page.h").read_text(encoding="utf-8")
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        reader_header = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
        preparation = epub[epub.index("Epub::ImagePreparationStatus Epub::beginImagePreparation") :
                           epub.index("bool Epub::getItemSize")]
        pump = reader[reader.index("bool EpubReaderActivity::pumpImagePreparation") :
                      reader.index("void EpubReaderActivity::recordReadingSample")]
        queue_visible = reader[reader.index("void EpubReaderActivity::queueVisiblePageImagePreparation") :
                               reader.index("bool EpubReaderActivity::pumpImagePreparation")]
        render = reader[reader.index("void EpubReaderActivity::render(RenderLock&&") :]
        render_contents = reader[reader.index("std::optional<bool> EpubReaderActivity::renderContents") :]
        self.assertIn("imageStreamJob->beginCooperativeLookup", preparation)
        self.assertIn("4096", preparation)
        self.assertIn("StagedFileTransaction::beginPendingPublish", preparation)
        self.assertIn("imagePublishedDigestJob->step(4096)", preparation)
        self.assertIn('finalPath + ".pending"', preparation)
        self.assertLess(
            preparation.index("imagePublishedDigestJob->step(4096)"),
            preparation.index("imageSourceIdentityJob->step(4096"),
        )
        self.assertNotIn("StagedFileTransaction::publish(", preparation)
        self.assertIn("stepImageNeedingExtraction", page)
        self.assertIn("MAX_ELEMENTS_PER_STEP = 64", page)
        self.assertIn("MAX_IMAGE_PROBES_PER_STEP = 2", page)
        self.assertIn("needsRawPreparation", page)
        self.assertIn("section->currentPage + (visiblePagePreparation ? 0 : 1)", pump)
        self.assertIn("epub->stepImagePreparation()", pump)
        self.assertLess(
            pump.index("const bool visibleImageRequested"),
            pump.index("if (visibleImageRequested && epub->imagePreparationActive()"),
        )
        preemption = pump[
            pump.index("if (visibleImageRequested && epub->imagePreparationActive()") :
            pump.index("if (epub->imagePreparationActive())")
        ]
        self.assertIn("if (epub->deferImagePreparationCleanup())", preemption)
        self.assertNotIn("cancelImagePreparation", preemption)
        self.assertGreaterEqual(reader.count("imagePrefetchSectionGeneration == sectionGeneration"), 4)
        self.assertNotIn("preparePixelCache", pump)
        self.assertIn("RenderLock lock(std::try_to_lock);", pump)
        self.assertIn("scanStatus == PageImageScanStatus::More", pump)
        self.assertEqual(pump.count("section->loadPage(targetPage)"), 1)
        self.assertIn("if (!imagePrefetchScanPage)", pump)
        self.assertIn("imagePrefetchScanPage->stepImageNeedingExtraction", pump)
        self.assertIn("std::unique_ptr<Page> imagePrefetchScanPage", reader_header)
        found = pump[pump.index("imagePreparationPath = candidate.imagePath") :]
        self.assertLess(found.index("resetImagePageScan()"),
                        found.index("epub->beginImagePreparation"))
        self.assertLess(found.index("if (epub->thumbnailPreparationActive())"),
                        found.index("epub->beginImagePreparation"))
        self.assertIn("deferredCoverStarted = false", found[:found.index("epub->beginImagePreparation")])
        self.assertIn("ZipSourceIdentityJob", preparation)
        self.assertIn("imageSourceIdentityJob->step(4096", preparation)
        self.assertIn("sourcePathMatchesIdentityJob(filepath.c_str(), currentIdentity", preparation)
        self.assertIn("input_.fileSize64() == expectedSize_", epub)
        self.assertIn("if (!path || !stamp.valid) return false", epub)
        self.assertIn("queueVisiblePageImagePreparation()", render)
        self.assertNotIn("stepImageNeedingExtraction", render_contents)
        self.assertNotIn("hasImagesNeedingDecode", render_contents)
        self.assertIn("page->deferMissingImageExtraction()", render_contents)
        self.assertIn("page->hasImagesAwaitingRawPreparation()", render_contents)
        self.assertIn("if (sameTarget && imagePrefetchPageComplete)", queue_visible)
        self.assertNotIn("cancelImagePreparation()", queue_visible)
        self.assertIn("return clippingHighlights.truncated", render_contents)
        self.assertIn("if (manualRefreshPending) forcedRefreshPending = true", render_contents)
        self.assertIn("preparationMatchesCurrentPage", reader)
        self.assertIn("imagePreparationForVisiblePage.store(true", reader)
        self.assertGreaterEqual(reader.count("ImageBlock::markPreparationFailure(imagePreparationPath)"), 2)
        self.assertIn("imagePreparationPath = candidate.imagePath", reader)

    def test_epub_image_failure_suppression_resets_on_explicit_reflow(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        invalidate = reader[reader.index("void EpubReaderActivity::invalidateReaderLayout") :
                            reader.index("void EpubReaderActivity::rememberCurrentContentOffset")]
        orientation = reader[reader.index("void EpubReaderActivity::applyOrientation") :
                             reader.index("void EpubReaderActivity::applyAutoPageTurnRuntime")]
        auto_turn = reader[reader.index("void EpubReaderActivity::applyAutoPageTurnRuntime") :
                           reader.index("void EpubReaderActivity::updateAutoPageTurnPreference")]
        self.assertIn("ImageBlock::clearSessionRenderFailures()", invalidate)
        self.assertIn("ImageBlock::clearSessionRenderFailures()", orientation)
        self.assertIn("ImageBlock::clearSessionRenderFailures()", auto_turn)

    def test_epub_page_turns_queue_while_layout_catches_up(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
        page_turn = reader[reader.index("void EpubReaderActivity::pageTurn") :
                           reader.index("bool EpubReaderActivity::moveOnePageWithoutRendering")]
        self.assertLess(page_turn.index("RenderLock lock(std::try_to_lock)"),
                        page_turn.index("if (sectionLandingPending || sectionRenderWaiting ||"))
        self.assertIn("ReaderUtils::queuePageTurns(pendingPageTurnDelta", page_turn)
        self.assertLess(page_turn.index("pendingPageTurnDelta"),
                        page_turn.index("stopReadingPage(isForwardTurn"))
        self.assertIn("const bool sectionTransition", page_turn)
        self.assertIn("sectionLandingPending || sectionRenderWaiting || sectionTransition", page_turn)
        self.assertNotIn("cancelImagePreparation()", page_turn)
        loop = reader[reader.index("void EpubReaderActivity::loop()") :
                      reader.index("void EpubReaderActivity::pageTurn")]
        self.assertIn("ReaderUtils::queuedPageTurns(pendingPageTurnDelta) != 0", loop)
        self.assertIn("RenderLock lock(std::try_to_lock)", loop)
        self.assertIn("!activityManager.hasPendingRender()", loop)
        self.assertIn("pageTurn(forward, true, true);", loop)
        self.assertIn("pageTurn(true, false);", loop)
        self.assertIn("ReaderUtils::clearQueuedPageTurns(pendingPageTurnDelta)", loop)
        self.assertIn("std::atomic<int8_t> pendingPageTurnDelta", header)
        self.assertIn("retargetQueuedPageTurns()", loop)
        automatic = loop[loop.index("if (automaticPageTurnActive)") :
                         loop.index("if (showBookmarkMessage")]
        self.assertIn("if (!section || RenderLock::peek())", automatic)
        self.assertNotIn("if (!section) {\n      requestUpdate();\n      return;", automatic)
        self.assertNotIn("if (RenderLock::peek()) {\n      lastPageTurnTime = millis();\n      return;", automatic)
        self.assertIn("if (landingWarmupActive && pendingPageTurnDelta != 0)", loop)
        self.assertIn("else if (pendingPageTurnDelta != 0)", loop)
        gesture = loop[loop.index("const auto pageGesture") :]
        manual_dispatch = gesture[gesture.index("if (prevTriggered)") :
                                  gesture.index("\n}\n\nbool EpubReaderActivity::handleReaderShortcut")]
        self.assertNotIn("if (!section)", manual_dispatch)
        self.assertIn("pageTurn(false);", manual_dispatch)
        self.assertIn("pageTurn(true);", manual_dispatch)

    def test_epub_progress_is_coalesced_until_the_turn_queue_is_stable(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
        render = reader[reader.index("void EpubReaderActivity::render(RenderLock&&") :
                        reader.index("bool EpubReaderActivity::applyDeferredReposition")]
        loop = reader[reader.index("void EpubReaderActivity::loop()") :
                      reader.index("bool EpubReaderActivity::handleReaderShortcut")]
        pause = reader[reader.index("void EpubReaderActivity::onPause()") :
                       reader.index("void EpubReaderActivity::onResume()")]
        exit_path = reader[reader.index("void EpubReaderActivity::onExit()") :
                           reader.index("void EpubReaderActivity::onPause()")]

        self.assertIn("struct PendingProgressSave", header)
        self.assertIn("stageProgressSave(currentSpineIndex, section->currentPage", render)
        self.assertNotIn("saveProgress(currentSpineIndex, section->currentPage", render)
        self.assertIn("pendingPageTurnDelta == 0", loop)
        self.assertIn("pendingProgressSave.active", loop)
        self.assertIn("!pendingProgressSave.retryBlocked", loop)
        self.assertIn("PROGRESS_SAVE_IDLE_MS", header)
        self.assertIn("pendingProgressSave.stagedAtMs", loop)
        self.assertIn("flushPendingProgressSave()", loop)
        self.assertIn("flushPendingProgressSave()", pause)
        self.assertIn("flushPendingProgressSave()", exit_path)

    def test_txt_and_xtc_page_turns_do_not_wait_for_the_render_mutex(self):
        for name in ("TxtReaderActivity", "XtcReaderActivity"):
            reader = (REPO_ROOT / f"src/activities/reader/{name}.cpp").read_text(encoding="utf-8")
            header = (REPO_ROOT / f"src/activities/reader/{name}.h").read_text(encoding="utf-8")
            navigation = reader[reader.index("const auto pageGesture") :
                                reader.index(f"bool {name}::handleReaderShortcut")]

            self.assertIn("std::atomic<int8_t> pendingPageTurnDelta", header)
            self.assertIn("ReaderUtils::queuePageTurns(pendingPageTurnDelta", navigation)
            self.assertIn("ReaderUtils::takeQueuedPageTurns(pendingPageTurnDelta", navigation)
            self.assertIn("retargetQueuedPageTurns()", reader)
            self.assertIn("RenderLock lock(std::try_to_lock)", navigation)
            self.assertNotIn("RenderLock lock(*this)", navigation)

    def test_page_renderers_abandon_stale_work_before_the_first_panel_refresh(self):
        epub = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        txt = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        xtc = (REPO_ROOT / "src/activities/reader/XtcReaderActivity.cpp").read_text(encoding="utf-8")

        epub_contents = epub[epub.index("EpubReaderActivity::renderContents") :
                             epub.index("#undef EPUB_RENDER_TIMESTAMP")]
        self.assertLess(epub_contents.index("retargetQueuedPageTurns()"),
                        epub_contents.index("renderer.displayBuffer"))
        self.assertIn("return std::nullopt", epub_contents)

        txt_page = txt[txt.index("bool TxtReaderActivity::renderPage") :
                       txt.index("void TxtReaderActivity::renderStatusBar")]
        self.assertLess(txt_page.index("retargetQueuedPageTurns()"),
                        txt_page.index("ReaderUtils::displayWithRefreshCycle"))

        xtc_page = xtc[xtc.index("XtcReaderActivity::PageRenderResult XtcReaderActivity::renderPage") :
                       xtc.index("bool XtcReaderActivity::saveProgress")]
        self.assertIn("PageRenderResult::Superseded", xtc_page)
        self.assertLess(xtc_page.index("retargetQueuedPageTurns()"),
                        xtc_page.index("ReaderUtils::displayWithRefreshCycle"))

    def test_readers_trace_input_queue_render_and_visible_page_boundaries(self):
        utils = (REPO_ROOT / "src/activities/reader/ReaderUtils.h").read_text(encoding="utf-8")
        self.assertIn("logPageTurnMetric", utils)
        self.assertIn('LOG_DBG("PTM"', utils)

        for name in ("EpubReaderActivity", "TxtReaderActivity", "XtcReaderActivity"):
            reader = (REPO_ROOT / f"src/activities/reader/{name}.cpp").read_text(encoding="utf-8")
            header = (REPO_ROOT / f"src/activities/reader/{name}.h").read_text(encoding="utf-8")
            self.assertIn("debugTurnSequence", header)
            for phase in ("input", "queued", "render_begin", "visible"):
                self.assertIn(f'"{phase}"', reader)

    def test_txt_page_index_recovery_keeps_backup_after_io_error(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        recovery = reader[reader.index("bool TxtReaderActivity::loadPageIndexCache()") :]
        recovery = recovery[: recovery.index("HalFile f;")]
        self.assertNotIn("Storage.remove(backupPath.c_str())", recovery)

    def test_renderer_does_not_keep_obsolete_full_frame_backup_storage(self):
        renderer = (REPO_ROOT / "lib/GfxRenderer/GfxRenderer.h").read_text(encoding="utf-8")
        implementation = (REPO_ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")
        for obsolete in ("bwBufferChunks", "storeBwBuffer", "restoreBwBuffer", "freeBwBufferChunks"):
            self.assertNotIn(obsolete, renderer)
            self.assertNotIn(obsolete, implementation)

    def test_home_never_decodes_an_original_epub_cover(self):
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        self.assertNotIn("Epub::ThumbnailMode::EmbeddedThenCover", home)
        self.assertIn("Epub::ThumbnailMode::EmbeddedOnly", home)

    def test_home_carousel_has_no_special_side_button_hold_targets(self):
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        self.assertNotIn("HOME_HOLD_MS", home)
        self.assertNotIn("carouselHold", home)
        self.assertIn("buttonNavigator.onNext(nextItem);", home)
        self.assertIn("buttonNavigator.onPrevious(previousItem);", home)

    def test_initial_epub_indexing_replaces_the_opening_popup(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("bool showPopup = pendingPercentJump;", reader)
        self.assertRegex(
            reader,
            r"if \(showPopup\) \{\s+renderer\.clearScreen\(\);\s+GUI\.drawPopup\(renderer, tr\(STR_INDEXING\)\);",
        )

    def test_epub_cold_extract_and_landing_are_cooperative(self):
        section = (REPO_ROOT / "lib/Epub/Epub/Section.cpp").read_text(encoding="utf-8")
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        start_build = section[section.index("bool Section::startBuild") : section.index("bool Section::beginParser")]
        extraction = section[section.index("Section::HtmlExtractionStep Section::stepHtmlExtraction") :
                             section.index("bool Section::buildSomeMore")]
        render = reader[reader.index("void EpubReaderActivity::render(RenderLock&&") :
                        reader.index("bool EpubReaderActivity::applyDeferredReposition")]

        self.assertNotIn("readItemContentsToStream", start_build)
        self.assertIn("htmlStreamJob.step()", extraction)
        self.assertEqual(render.count("section->buildSomeMore("), 1)
        self.assertIn("section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)", render)
        self.assertIn("bool EpubReaderActivity::sectionTurnBufferReady", reader)
        self.assertIn("target < static_cast<int>(section->pageCount)", render)
        landing = reader[reader.index("bool EpubReaderActivity::sectionLandingReady") :
                         reader.index("bool EpubReaderActivity::sectionTurnBufferReady")]
        self.assertNotIn("return sectionTurnBufferReady", landing)
        warmup = reader[reader.index("bool EpubReaderActivity::sectionLandingReadyForRender") :
                        reader.index("bool EpubReaderActivity::requestedSectionPageReady")]
        self.assertIn("sectionLandingWarmupPending", warmup)
        self.assertIn("sectionLandingTargetPage", warmup)
        self.assertIn("sectionTurnBufferReady", warmup)
        self.assertGreaterEqual(reader.count("if (!sectionLandingReadyForRender())"), 2)
        self.assertIn("partialTarget + PARTIAL_REBUILD_START_MARGIN", render)
        self.assertIn("sectionLandingWarmupPending = !pendingPercentJump && !partialTargetAvailable", render)
        idle = reader[reader.index("if (!prevTriggered && !nextTriggered)") :
                      reader.index("// At end of the book")]
        self.assertIn("sectionTurnBufferReady(section->currentPage)", idle)
        self.assertIn("sectionLandingPending = true;", render)
        self.assertIn("sectionRenderWaiting = true;", render)
        self.assertIn("requestedSectionPageReady()", reader)
        self.assertIn("BACKGROUND_BUILD_PARSE_STEPS_PER_TICK", reader)
        self.assertIn("maxParseSteps", section)
        self.assertIn("parseSteps >= maxParseSteps", section)

    def test_epub_cold_section_stream_does_not_borrow_the_framebuffer_across_ticks(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        render = reader[reader.index("void EpubReaderActivity::render(RenderLock&&") :
                        reader.index("bool EpubReaderActivity::applyDeferredReposition")]
        cold_start = render.index("const bool cacheComplete")
        initial_build = render[cold_start : render.index("sectionLandingPending = true;", cold_start)]

        self.assertIn("section->startBuild(", initial_build)
        self.assertIn("section->buildSomeMore(BACKGROUND_BUILD_PAGES_PER_TICK)", initial_build)
        self.assertNotIn("FrameBufferLoan", initial_build)

    def test_finalized_epub_section_cache_is_a_complete_landing_target(self):
        section = (REPO_ROOT / "lib/Epub/Epub/Section.cpp").read_text(encoding="utf-8")
        load = section[section.index("bool Section::loadSectionFile") : section.index("bool Section::clearCache")]

        # Backward chapter navigation uses UINT16_MAX as a last-page sentinel.
        # A finalized cache must therefore satisfy isBuildComplete(); otherwise
        # the reader waits forever for a build that was never started.
        self.assertIn("buildComplete_ = false;", load)
        self.assertIn("buildComplete_ = !filePartial;", load)

    def test_epub_font_prewarm_collects_text_without_a_discarded_render_pass(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        page_header = (REPO_ROOT / "lib/Epub/Epub/Page.h").read_text(encoding="utf-8")
        page = (REPO_ROOT / "lib/Epub/Epub/Page.cpp").read_text(encoding="utf-8")
        text_block = (REPO_ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp").read_text(encoding="utf-8")
        render_contents = reader[reader.index("std::optional<bool> EpubReaderActivity::renderContents") :
                                 reader.index("void EpubReaderActivity::renderStatusBar")]

        prewarm = render_contents[:render_contents.index("scope.endScanAndPrewarm()")]
        self.assertIn("collectFontText", page_header)
        self.assertIn("page->collectFontText(*fcm, fontId)", prewarm)
        self.assertNotIn("page->render(", prewarm)
        self.assertIn("void Page::collectFontText", page)
        collector = text_block[text_block.index("void TextBlock::collectFontText") :
                               text_block.index("void TextBlock::render")]
        self.assertIn("recordFontText(cache", collector)
        self.assertIn("cache.recordText", text_block)
        self.assertIn("hasRtlScriptBytes", collector)
        self.assertIn("BidiUtils::detectParagraphLevel", collector)
        self.assertIn("BidiUtils::applyBidiVisual", text_block)

    def test_epub_page_lut_growth_is_nothrow_and_heap_bounded(self):
        header = (REPO_ROOT / "lib/Epub/Epub/Section.h").read_text(encoding="utf-8")
        section = (REPO_ROOT / "lib/Epub/Epub/Section.cpp").read_text(encoding="utf-8")
        lut_growth = section[section.index("bool Section::PageLut::ensureAppendCapacity") :
                             section.index("uint32_t Section::onPageComplete")]
        start_build = section[section.index("bool Section::startBuild") :
                              section.index("bool Section::beginParser")]
        build_more = section[section.index("bool Section::buildSomeMore") :
                             section.index("bool Section::hasHtmlCache")]

        self.assertNotIn("std::vector<PageLutEntry>", header)
        self.assertIn("struct PageLut", header)
        self.assertIn("makeUniqueNoThrow<PageLutEntry[]>", lut_growth)
        self.assertIn("MemoryBudget::hasContiguousHeadroom", lut_growth)
        self.assertIn("if (!ctxPtr->lut.ensureAppendCapacity())", start_build)
        self.assertIn("callbackFailure = EpubBuildStatus::OutOfMemory", start_build)
        self.assertIn("if (build_->callbackFailure != EpubBuildStatus::Ok)", build_more)

    def test_home_cover_loading_message_is_epub_only(self):
        theme = (REPO_ROOT / "src/components/themes/crossvi/CrossViTheme.cpp").read_text(encoding="utf-8")
        self.assertIn(
            "FsHelpers::hasEpubExtension(currentBook.path) ? tr(STR_COVER_LOADS_WHEN_OPENED) : nullptr",
            theme,
        )

    def test_home_recent_cover_policy_and_reader_loading_phase_order(self):
        settings = (REPO_ROOT / "src/CrossPointSettings.h").read_text(encoding="utf-8")
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        reader = (REPO_ROOT / "src/activities/reader/ReaderActivity.cpp").read_text(encoding="utf-8")
        epub_reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        activity_manager = (REPO_ROOT / "src/activities/ActivityManager.h").read_text(encoding="utf-8")
        saved = (REPO_ROOT / "src/activities/reader/SavedClippingsActivity.cpp").read_text(encoding="utf-8")

        self.assertIn("skipReaderCoverCacheBuild", settings)
        self.assertIn("ReaderOpenOrigin::HomeRecent", home)
        self.assertIn("SavedItems", activity_manager)
        self.assertEqual(saved.count("ReaderOpenOrigin::SavedItems"), 2)
        self.assertIn("skipDerivedCoverCacheBuild()", reader)
        self.assertIn("ReaderOpenOrigin::HomeRecent || openOrigin == ReaderOpenOrigin::SavedItems", reader)
        self.assertIn("deferCoverPreparation = needsShared || needsCarousel;", reader)
        self.assertNotIn("beginThumbnailPreparation", reader)
        self.assertIn("deferredCoverFirstPageVisible = true", epub_reader)
        self.assertIn("epub->beginThumbnailPreparation(request)", epub_reader)
        self.assertIn("epub->stepThumbnailPreparation()", epub_reader)
        self.assertIn("epub->ensureThumbnails(request)", epub_reader)
        self.assertLess(epub_reader.index("detectPageTurnGesture"), epub_reader.index("pumpDeferredCoverPreparation();"))

    def test_saved_summary_reserves_only_its_drawn_icon_lane(self):
        saved = (REPO_ROOT / "src/activities/reader/SavedClippingsActivity.cpp").read_text(encoding="utf-8")
        theme = (REPO_ROOT / "src/components/themes/crossvi/CrossViTheme.cpp").read_text(encoding="utf-8")

        self.assertNotIn("savedCountReservation", saved)
        self.assertIn("SAVED_LIST_TITLE_VALUE_GAP", saved)
        self.assertIn("rowValueReservedWidth", theme)

    def test_quick_resume_is_a_direct_toggle_and_sleep_screen_is_compact(self):
        settings = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        submenu = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("SettingInfo::Toggle(StrId::STR_QUICK_RESUME", settings)
        self.assertIn("SettingInfo::DynamicEnum(\n            StrId::STR_SLEEP_SCREEN", settings)
        self.assertNotIn("StrId::STR_COVER_CUSTOM", settings)
        self.assertIn("StrId::STR_READING_STATS", settings)
        self.assertIn("StrId::STR_READING_STATS", submenu)
        self.assertIn("StrId::STR_COVER_WITH_STATS", settings)
        self.assertIn("StrId::STR_CUSTOM_WITH_STATS", settings)
        self.assertIn("StrId::STR_COVER_WITH_STATS", submenu)
        self.assertIn("StrId::STR_CUSTOM_WITH_STATS", submenu)

    def test_settings_hide_inapplicable_choices_and_defer_dictionary_scan(self):
        settings = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        activity = (REPO_ROOT / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
        submenu = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(encoding="utf-8")
        text_settings = (REPO_ROOT / "src/activities/settings/TextSettingsActivity.cpp").read_text(encoding="utf-8")
        web_server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        web_page = (REPO_ROOT / "src/network/html/SettingsPage.html").read_text(encoding="utf-8")
        json_settings = (REPO_ROOT / "src/JsonSettingsIO.cpp").read_text(encoding="utf-8")

        self.assertNotIn("SettingInfo::Enum(StrId::STR_LIBRARY_GRID", settings)
        self.assertIn('doc["libraryGrid"] = s.libraryGrid;', json_settings)
        self.assertLess(submenu.index("STR_SHOW_TXT_BOOKS"), submenu.index("STR_READ_BOOKS_IN_RECENTS"))
        text_layout = text_settings[text_settings.index("void TextSettingsActivity::rebuildSettings") :]
        self.assertLess(text_layout.index("buildScreenMarginSetting()"), text_layout.index("STR_LINE_SPACING"))
        self.assertLess(text_layout.index("STR_FORCE_PARAGRAPH_INDENTS"), text_layout.index("STR_EMBEDDED_STYLE"))

        on_enter = activity[
            activity.index("void SettingsActivity::onEnter") : activity.index("bool SettingsActivity::handleGlobalShortcut")
        ]
        self.assertNotIn("DictionaryRegistry::discover", on_enter)
        self.assertIn("selectedCategoryIndex == 1 && !dictionariesLoaded", activity)
        self.assertIn("alignment.enumValues.pop_back()", text_settings)
        self.assertIn("!display.supportsStripGrayscale()", web_server)
        self.assertNotIn("textDarkness", settings)
        self.assertNotIn("row-setting-textDarkness", web_page)
        self.assertIn("bookAlignment.hidden", web_page)

    def test_x3_periodic_refresh_uses_fast_cleanup_without_lut_or_idle_cleanup_settings(self):
        settings = (REPO_ROOT / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        reader_utils = (REPO_ROOT / "src/activities/reader/ReaderUtils.h").read_text(encoding="utf-8")
        settings_header = (REPO_ROOT / "src/CrossPointSettings.h").read_text(encoding="utf-8")
        settings_json = (REPO_ROOT / "src/JsonSettingsIO.cpp").read_text(encoding="utf-8")
        display = (REPO_ROOT / "freeink-sdk/libs/display/FreeInkDisplay/src/FreeInkDisplay.cpp").read_text(
            encoding="utf-8"
        )
        driver = (REPO_ROOT / "freeink-sdk/libs/display/FreeInkDisplay/src/driver/Uc8253X3Driver.cpp").read_text(
            encoding="utf-8"
        )

        for removed in (
            "STR_X3_FAST_LUT",
            "STR_X3_IDLE_GHOST_CLEANUP",
            "STR_X3_GHOST_CLEANUP_DELAY",
            "x3FastLutFrames",
            "x3IdleGhostCleanup",
            "x3IdleGhostCleanupDelayMs",
        ):
            self.assertNotIn(removed, settings)
            self.assertNotIn(removed, settings_header)
            self.assertNotIn(removed, settings_json)
            self.assertNotIn(removed, main)
        self.assertNotIn("scheduleX3GhostCleanup", reader_utils)
        self.assertNotIn("runScheduledX3GhostCleanup", main)

        cycle = reader_utils[
            reader_utils.index("inline void displayWithRefreshCycle") : reader_utils.index(
                "// Grayscale anti-aliasing pass"
            )
        ]
        self.assertIn("display.supportsX3GhostCleanup()", cycle)
        self.assertIn("waveform.requestCleanupAfterPageVisible();", cycle)
        self.assertIn("renderer.displayBuffer(HalDisplay::HALF_REFRESH);", cycle)
        self.assertIn("X3_FAST_CLEANUP_FRAMES = 10", driver)
        cleanup_driver = driver[driver.index("bool Uc8253X3Driver::cleanFastGhosting") :
                                driver.index("void Uc8253X3Driver::displayGrayscaleBase")]
        self.assertIn("loadProfiledFastBank(bus, false, X3_FAST_CLEANUP_FRAMES)", cleanup_driver)
        self.assertIn("loadProfiledFastBank(bus, true, X3_FAST_CLEANUP_FRAMES)", cleanup_driver)
        self.assertNotIn("sendPlaneFlipped", cleanup_driver)
        self.assertNotIn("triggerRefresh", cleanup_driver)
        self.assertIn("_pendingGhostCleanup = true", cleanup_driver)
        finish = driver[driver.index("void Uc8253X3Driver::displayFinish") :
                        driver.index("void Uc8253X3Driver::setFastLutFrameCount")]
        self.assertLess(finish.index("if (ghostCleanup)"), finish.index("sendPlaneFlipped(CMD_DTM1"))
        display_cleanup = display[display.index("bool FreeInkDisplay::cleanFastGhosting") :
                                  display.index("bool FreeInkDisplay::refreshBusy")]
        self.assertIn("_refreshPending = cleaned", display_cleanup)
        self.assertIn("_fastGhostCleanupEligible = fastMode && !doFullSync && !turnOff", driver)

        waveform = reader_utils[
            reader_utils.index("struct X3ReaderWaveformState") : reader_utils.index(
                "struct PageTurnGestureResult"
            )
        ]
        self.assertIn("void beginTransition()", waveform)
        self.assertIn("void leaveReader()", waveform)
        self.assertIn("void pageVisible()", waveform)
        self.assertIn("display.cleanX3GhostingNow();", waveform)
        self.assertNotIn("schedule", waveform.lower())

        for reader_name in ("EpubReaderActivity", "TxtReaderActivity", "XtcReaderActivity"):
            reader = (REPO_ROOT / f"src/activities/reader/{reader_name}.cpp").read_text(encoding="utf-8")
            self.assertGreaterEqual(reader.count("readerWaveform.beginTransition();"), 2)
            self.assertGreaterEqual(reader.count("readerWaveform.leaveReader();"), 2)
            self.assertIn("readerWaveform.pageVisible();", reader)

    def test_sunlight_fading_fix_does_not_turn_off_ordinary_refreshes(self):
        renderer = (REPO_ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")
        display_buffer = renderer[renderer.index("void GfxRenderer::displayBuffer") :
                                  renderer.index("size_t GfxRenderer::readFramebufferRegion")]

        self.assertIn("const bool actualTurnOff = turnOffScreen;", display_buffer)
        self.assertIn("display.displayBuffer(refreshMode, actualTurnOff);", display_buffer)
        self.assertNotIn("fadingFix || turnOffScreen", display_buffer)
        self.assertIn("display.displayGrayscaleBase(fallback, turnOffScreen);", renderer)
        self.assertIn("display.displayGrayBuffer(turnOffScreen);", renderer)
        self.assertNotIn("displayGrayscaleBase(fallback, fadingFix || turnOffScreen)", renderer)
        self.assertNotIn("displayGrayBuffer(fadingFix || turnOffScreen)", renderer)

    def test_reader_exit_does_not_inject_cleanup_into_destination_ui(self):
        hal_header = (REPO_ROOT / "lib/hal/HalDisplay.h").read_text(encoding="utf-8")
        hal = (REPO_ROOT / "lib/hal/HalDisplay.cpp").read_text(encoding="utf-8")
        manager = (REPO_ROOT / "src/activities/ActivityManager.cpp").read_text(encoding="utf-8")

        for removed in (
            "requestX3CleanupAfterNextRefresh",
            "x3CleanupAfterNextRefresh",
            "pendingReaderExitCleanupAllowed",
        ):
            self.assertNotIn(removed, hal_header)
        self.assertNotIn(removed, hal)
        self.assertNotIn(removed, manager)
        self.assertIn("replaceActivity(std::move(sleepActivity));", manager)

    def test_reader_input_debug_trace_covers_poll_debounce_and_gesture_layers(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        gpio = (REPO_ROOT / "lib/hal/HalGPIO.cpp").read_text(encoding="utf-8")
        reader_utils = (REPO_ROOT / "src/activities/reader/ReaderUtils.h").read_text(encoding="utf-8")

        self.assertIn('LOG_DBG("INP", "stage=poll_gap', main)
        self.assertIn('LOG_DBG("INP", "stage=main_work', main)
        self.assertIn("READER_DEBOUNCE_REPOLL_MS = 6", main)
        debounce_gate = main[main.index("gpio.update();") : main.index("halTiltSensor.update")]
        self.assertIn("readerVisible && gpio.isDebouncePending()", debounce_gate)
        self.assertIn("delay(READER_DEBOUNCE_REPOLL_MS);", debounce_gate)
        self.assertIn("return;", debounce_gate)
        self.assertIn("readerVisible && gpio.isDebouncePending() ? READER_DEBOUNCE_REPOLL_MS : 10", main)
        self.assertIn("gpio.isDebouncePending() || readerVisible ? responsiveLoopDelay : 50", main)
        self.assertIn("inputMgr.readButtonAdc", gpio)
        self.assertIn('"stage=adc adc1=', gpio)
        self.assertIn('LOG_DBG("INP", "stage=%s pending=', gpio)
        self.assertIn('"stage=gesture press_prev=', reader_utils)
        self.assertNotIn("beginAsync", main)

    def test_text_antialiasing_defaults_off_without_changing_the_persisted_field(self):
        settings_header = (REPO_ROOT / "src/CrossPointSettings.h").read_text(encoding="utf-8")
        per_book = (REPO_ROOT / "src/activities/reader/PerBookReaderSettings.h").read_text(encoding="utf-8")
        self.assertIn("uint8_t textAntiAliasing = 0;", settings_header)
        self.assertIn("uint8_t textAntiAliasing = 0;", per_book)

    def test_quick_resume_and_serial_screenshot_use_a_stable_framebuffer(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        sleep = main[main.index("void enterDeepSleep") : main.index("void setupDisplayAndFonts")]
        self.assertLess(sleep.index("SleepFrameStore::save(renderer)"), sleep.index("activityManager.goToSleep"))
        screenshot = main[main.index("void handleSerialCommand") : main.index("void pumpSerialCommands")]
        self.assertLess(screenshot.index("RenderLock lock;"), screenshot.index("display.getFrameBuffer()"))
        self.assertIn("if (buf)", screenshot)
        self.assertIn("SCREENSHOT_ERROR:BUSY", screenshot)

    def test_status_bar_clock_labels_are_bound_by_runtime_enum(self):
        settings = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        self.assertIn(
            "statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_RIGHT] = StrId::STR_DIR_RIGHT;", settings
        )
        self.assertIn(
            "statusBarClockValues[CrossPointSettings::STATUS_BAR_CLOCK_LEFT] = StrId::STR_DIR_LEFT;", settings
        )

    def test_version_mapping_preserves_existing_environment_contract(self):
        module = load_git_branch()
        self.assertEqual(module.compute_version("gh_release", str(REPO_ROOT)), "1.1.2")
        self.assertEqual(module.compute_version("slim", str(REPO_ROOT)), "1.1.2-slim")
        self.assertEqual(module.compute_version("simulator_x3", str(REPO_ROOT)), "1.1.2-simulator")
        self.assertEqual(module.compute_version("simulator_x4", str(REPO_ROOT)), "1.1.2-simulator")

    def test_ota_valid_mark_retries_transient_failures(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        helper_start = main.index("void markOtaValidOnceHealthy()")
        helper = main[helper_start : main.index("}  // namespace", helper_start)]
        first_done = helper.index("done = true;")
        self.assertGreater(first_done, helper.index("if (state != ESP_OTA_IMG_PENDING_VERIFY)"))
        self.assertEqual(helper.count("done = true;"), 2)
        self.assertIn("now - lastAttempt < 1000", helper)
        self.assertIn("if (stateResult != ESP_OK)", helper)
        self.assertIn("if (result == ESP_OK) {\n    done = true;", helper)

    def test_crash_report_is_only_cleared_after_durable_persistence(self):
        system = (REPO_ROOT / "lib" / "hal" / "HalSystem.cpp").read_text(encoding="utf-8")
        check = system[system.index("void checkPanic()") : system.index("bool panicReportPersisted()")]
        self.assertIn("written == panicInfo.size() && file.sync()", check)
        self.assertIn("const bool closed = file.close();", check)
        self.assertIn("written == panicInfo.size() && synced && closed", check)

        crash = (REPO_ROOT / "src/activities/home/CrashActivity.cpp").read_text(encoding="utf-8")
        on_enter = crash[crash.index("void CrashActivity::onEnter()") : crash.index("void CrashActivity::loop()")]
        self.assertIn("if (HalSystem::panicReportPersisted())", on_enter)
        self.assertLess(on_enter.index("panicReportPersisted()"), on_enter.index("HalSystem::clearPanic();"))

    def test_crash_screen_requires_a_crossvi_panic_capture(self):
        system = (REPO_ROOT / "lib" / "hal" / "HalSystem.cpp").read_text(encoding="utf-8")
        self.assertIn("RTC_NOINIT_ATTR uint32_t panicCaptureMagic;", system)
        self.assertIn("panicCaptureMagic = PANIC_CAPTURE_MAGIC;", system)
        reboot_check = system[system.index("bool isRebootFromPanic()") :]
        self.assertIn("panicReset && panicCaptureMagic == PANIC_CAPTURE_MAGIC", reboot_check)
        clear = system[system.index("void clearPanic()") : system.index("std::string getPanicInfo")]
        self.assertIn("panicCaptureMagic = 0;", clear)

    def test_riscv_crash_report_preserves_fault_registers(self):
        system = (REPO_ROOT / "lib" / "hal" / "HalSystem.cpp").read_text(encoding="utf-8")
        self.assertIn("struct RiscvPanicRegisters", system)
        self.assertIn("captureRiscvPanicRegisters(frame);", system)
        self.assertIn("exceptionFrame->mepc", system)
        self.assertIn("exceptionFrame->mcause", system)
        self.assertIn("exceptionFrame->mtval", system)
        self.assertIn("MEPC (faulting instruction)", system)
        self.assertIn("MTVAL (fault address/value)", system)
        clear = system[system.index("void clearPanic()") : system.index("std::string getPanicInfo")]
        self.assertIn("panicRiscvRegisters.captured = 0;", clear)

    def test_epub_navigation_clears_deferred_resume_only_after_a_real_move(self):
        reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.h").read_text(encoding="utf-8")
        self.assertIn("void clearDeferredReposition();", header)

        page_turn = reader[reader.index("void EpubReaderActivity::pageTurn") :
                           reader.index("bool EpubReaderActivity::retargetQueuedPageTurns")]
        no_move = page_turn.index("if (!moved)")
        cleared = page_turn.index("clearDeferredReposition();")
        self.assertGreater(cleared, no_move)

        retarget = reader[reader.index("bool EpubReaderActivity::retargetQueuedPageTurns") :
                          reader.index("bool EpubReaderActivity::moveOnePageWithoutRendering")]
        self.assertGreater(retarget.index("clearDeferredReposition();"), retarget.index("if (!moved)"))

        for start, end in (
            ("void EpubReaderActivity::jumpToPercent", "void EpubReaderActivity::onReaderMenuConfirm"),
            ("void EpubReaderActivity::applyBookmarkJump", "bool EpubReaderActivity::launchKOReaderSync"),
            ("void EpubReaderActivity::navigateToHref", "void EpubReaderActivity::restoreSavedPosition"),
            ("void EpubReaderActivity::restoreSavedPosition", "void EpubReaderActivity::loadCachedBookmarks"),
        ):
            navigation = reader[reader.index(start) : reader.index(end)]
            self.assertIn("clearDeferredReposition();", navigation)

    def test_epub_parser_accepts_only_trailing_data_after_closed_html(self):
        parser = (REPO_ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h").read_text(encoding="utf-8")
        self.assertIn("bool htmlEnded_ = false;", header)
        end_element = parser[parser.index("void XMLCALL ChapterHtmlSlimParser::endElement") :
                             parser.index("ChapterHtmlSlimParser::~ChapterHtmlSlimParser")]
        self.assertIn('strcmp(name, "html") == 0', end_element)
        self.assertIn("htmlEnded_ = true;", end_element)
        parse_step = parser[parser.index("ChapterHtmlSlimParser::ParseStatus ChapterHtmlSlimParser::parseStep") :
                            parser.index("void ChapterHtmlSlimParser::abortParse")]
        error_branch = parse_step[parse_step.index("XML_STATUS_ERROR") :]
        self.assertIn("if (htmlEnded_)", error_branch)
        self.assertIn("return ParseStatus::Done;", error_branch)

    def test_epub_entity_table_includes_xml_apostrophe(self):
        entities = (REPO_ROOT / "lib/Epub/Epub/htmlEntities.cpp").read_text(encoding="utf-8")
        self.assertIn('{"&apos;", "\'"}', entities)
        self.assertLess(entities.index('{"&ang;",'), entities.index('{"&apos;",'))
        self.assertLess(entities.index('{"&apos;",'), entities.index('{"&aring;",'))

    def test_button_navigation_uses_fixed_storage_for_one_logical_button(self):
        header = (REPO_ROOT / "src/util/ButtonNavigator.h").read_text(encoding="utf-8")
        self.assertIn("using Buttons = std::array<MappedInputManager::Button, 1>;", header)
        self.assertNotIn("#include <vector>", header)
        self.assertIn("static constexpr Buttons getNextButtons()", header)
        self.assertIn("static constexpr Buttons getPreviousButtons()", header)

    def test_reading_stats_confirm_hint_matches_its_action(self):
        activity = (REPO_ROOT / "src/activities/reader/ReadingStatsActivity.cpp").read_text(encoding="utf-8")
        render = activity[activity.index("void ReadingStatsActivity::render") :]
        self.assertIn("page == Page::Device", render)
        self.assertIn("StrId::STR_STATS_MANAGE", render)
        self.assertIn("page == Page::Book && allowBookDateEdit", render)
        self.assertIn("StrId::STR_STATS_EDIT_DATES", render)

    def test_home_shortcuts_always_end_with_customize(self):
        activity = (REPO_ROOT / "src/activities/home/HomeShortcutsActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("const int itemCount = static_cast<int>(items_.size()) + 1;", activity)
        self.assertIn("index == static_cast<int>(items_.size())", activity)
        self.assertIn("STR_CUSTOMIZE_SHORTCUTS", activity)
        self.assertIn("std::make_unique<HomeShortcutManagerActivity>", activity)

    def test_recent_book_removal_rolls_back_failed_publication(self):
        store = (REPO_ROOT / "src/RecentBooksStore.cpp").read_text(encoding="utf-8")
        remove = store[store.index("bool RecentBooksStore::removeByPath") : store.index("void RecentBooksStore::updatePath")]
        self.assertIn("RecentBook removed = std::move(*it);", remove)
        self.assertIn("recentBooks.insert(recentBooks.begin() + index, std::move(removed));", remove)
        self.assertIn("return false;", remove[remove.index("if (!saveToFile())") :])

    def test_recent_book_loader_drops_entries_without_a_path(self):
        store = (REPO_ROOT / "src/RecentBooksStore.cpp").read_text(encoding="utf-8")
        loader = store[store.index("bool RecentBooksStore::fromJson") : store.index("bool RecentBooksStore::loadFromFile")]
        self.assertLess(loader.index("if (storedPath[0] == '\\0') continue;"), loader.index("recentBooks.push_back(book);"))

    def test_recent_book_metadata_update_rolls_back_failed_publication(self):
        store = (REPO_ROOT / "src/RecentBooksStore.cpp").read_text(encoding="utf-8")
        update = store[store.index("void RecentBooksStore::updateBook") : store.index("bool RecentBooksStore::removeByPath")]
        self.assertIn("RecentBook previous = *it;", update)
        failed_save = update[update.index("if (!saveToFile())") :]
        self.assertIn("*it = std::move(previous);", failed_save)

    def test_recent_book_missing_checks_stay_off_first_frame_and_reader_open(self):
        store = (REPO_ROOT / "src/RecentBooksStore.cpp").read_text(encoding="utf-8")
        add = store[store.index("void RecentBooksStore::addBook") : store.index("void RecentBooksStore::updateBook")]
        step = store[store.index("RecentBooksStore::PruneStepResult RecentBooksStore::pruneMissingStep") :
                     store.index("RecentBook RecentBooksStore::getDataFromBook")]
        home = (REPO_ROOT / "src/activities/home/HomeActivity.cpp").read_text(encoding="utf-8")
        load = home[home.index("void HomeActivity::loadRecentBooks") : home.index("void HomeActivity::loadBookSummary")]
        maintenance = home[home.index("void HomeActivity::processRecentBooksMaintenance") :
                           home.index("void HomeActivity::loop()")]
        loop = home[home.index("void HomeActivity::loop()") : home.index("void HomeActivity::render(RenderLock&&)")]

        self.assertNotIn("pruneMissing()", add)
        self.assertNotIn("isMissing", load)
        self.assertIn("pruneMissingStep(recentPruneIndex, pinnedPruneIndex", maintenance)
        self.assertIn("processRecentBooksMaintenance()", loop)
        self.assertIn("if (Storage.exists(path.c_str()))", step)
        self.assertIn("if (!Storage.probeMedia())", step)
        self.assertIn("if (!saveToFile())", step)

    def test_empty_file_name_uses_generic_icon_without_reading_past_the_string(self):
        theme = (REPO_ROOT / "src/components/UITheme.cpp").read_text(encoding="utf-8")
        get_icon = theme[theme.index("UIIcon UITheme::getFileIcon") : theme.index("int UITheme::getStatusBarHeight")]
        self.assertLess(get_icon.index("filename.empty()"), get_icon.index("filename.back()"))

    def test_password_masking_preserves_utf8_codepoints_and_maps_the_cursor(self):
        keyboard = (REPO_ROOT / "src/activities/util/KeyboardEntryActivity.cpp").read_text(encoding="utf-8")
        render = keyboard[keyboard.index("void KeyboardEntryActivity::render") : keyboard.index("void KeyboardEntryActivity::onComplete")]
        self.assertNotIn("displayText[i] = '*'", render)
        self.assertIn("const size_t next = nextUtf8Boundary(text, position);", render)
        self.assertIn("displayText.append(text, position, next - position);", render)
        self.assertIn("displayText.push_back('*');", render)
        self.assertIn("if (position == cursorPos) displayCursorPos = displayText.size();", render)
        self.assertIn("nextUtf8Boundary(displayText, displayCursorPos)", render)

    def test_vietnamese_language_selection_sets_vietnam_utc_offset(self):
        settings = (REPO_ROOT / "src/CrossPointSettings.h").read_text(encoding="utf-8")
        language = (REPO_ROOT / "src/activities/settings/LanguageSelectActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("VIETNAM_UTC_OFFSET_Q = 76", settings)
        self.assertIn("selectedLanguage == Language::VI", language)
        self.assertIn("SETTINGS.clockUtcOffsetQ = CrossPointSettings::VIETNAM_UTC_OFFSET_Q;", language)
        self.assertIn("const uint8_t previousUtcOffsetQ = SETTINGS.clockUtcOffsetQ;", language)
        self.assertIn("SETTINGS.clockUtcOffsetQ = previousUtcOffsetQ;", language)

    def test_tilt_sensor_keeps_crosspoint_gyro_policy(self):
        sensor = (REPO_ROOT / "lib/hal/HalTiltSensor.h").read_text(encoding="utf-8")
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn("RATE_THRESHOLD_DPS = 270.0f", sensor)
        self.assertIn("NEUTRAL_RATE_DPS = 50.0f", sensor)
        self.assertIn("COOLDOWN_MS = 600", sensor)
        self.assertIn("POLL_INTERVAL_MS = 50", sensor)
        self.assertIn("TILT_INVERTED = 2", sensor)
        self.assertIn("void update(uint8_t mode, uint8_t orientation, bool inReader);", sensor)
        self.assertIn("halTiltSensor.update(SETTINGS.tiltPageTurn, SETTINGS.orientation, readerVisible);", main)

    def test_power_double_press_does_not_overlap_single_press_and_tilt_is_inline(self):
        settings = (REPO_ROOT / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
        submenu = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(encoding="utf-8")
        settings_list = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        settings_header = (REPO_ROOT / "src/CrossPointSettings.h").read_text(encoding="utf-8")
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")

        rebuild = submenu[submenu.index("void SettingsSubmenuActivity::rebuildSettings()") :]
        power_page = rebuild[rebuild.index("case Page::PowerButton:") : rebuild.index("case Page::FirmwareUpdate:")]
        self.assertIn("SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE", power_page)
        self.assertIn("StrId::STR_DOUBLE_PRESS_READING", power_page)
        self.assertIn("StrId::STR_DOUBLE_PRESS_OUTSIDE_READER", power_page)
        self.assertIn("StrId::STR_DOUBLE_POWER_REFRESH", power_page)

        self.assertIn("LP_MENU_REFRESH = 8", settings_header)
        self.assertIn("StrId::STR_SCREENSHOT_BUTTON, StrId::STR_DOUBLE_POWER_REFRESH", settings_list)
        self.assertIn("SETTINGS.shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::IGNORE &&", main)
        reader_double = main[main.index("if (powerEvent == PowerButtonGesture::Event::Double)") :
                             main.index("if (powerEvent == PowerButtonGesture::Event::Single")]
        self.assertIn("doublePowerReadingFunction == CrossPointSettings::LP_MENU_REFRESH", reader_double)
        self.assertIn("handleGlobalShortcut(GlobalShortcut::RefreshScreen)", reader_double)

        self.assertIn("SettingInfo::DynamicEnum(\n        StrId::STR_TILT_SENSOR", settings)
        self.assertIn("StrId::STR_TILT_PAGE_TURN_REVERSED", settings)
        self.assertIn("TiltPageTurnPolicy::settingOptionForMode(SETTINGS.tiltPageTurn)", settings)
        self.assertIn("TiltPageTurnPolicy::modeForSettingOption(value)", settings)
        self.assertIn("SettingInfo::DynamicEnum(\n                               StrId::STR_TILT_PAGE_TURN", settings_list)
        self.assertNotIn("TiltSensorSettings", settings)
        self.assertNotIn("Page::TiltSensor", submenu)

    def test_txt_font_scan_skips_unused_layout_measurements(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        prewarm = reader[reader.index("void TxtReaderActivity::prewarmCurrentPageFont") :
                         reader.index("void TxtReaderActivity::renderCurrentPageLines")]
        self.assertNotIn("renderCurrentPageLines()", prewarm)
        self.assertIn("renderer.drawText(cachedFontId, 0, 0, line.c_str())", prewarm)

        render_lines = reader[reader.index("void TxtReaderActivity::renderCurrentPageLines") :
                              reader.index("bool TxtReaderActivity::renderPage")]
        self.assertEqual(render_lines.count("renderer.getTextAdvanceX"), 2)

        interactive = reader[reader.index("int lineX = cachedOrientedMarginLeft;") :
                             reader.index("std::vector<int16_t> positions")]
        self.assertEqual(interactive.count("renderer.getTextAdvanceX"), 2)

    def test_library_order_is_validated_while_results_are_loaded(self):
        catalog = (REPO_ROOT / "src/LibraryCatalogStore.cpp").read_text(encoding="utf-8")
        load = catalog[catalog.index("bool LibraryCatalogStore::loadOrderedIndices") :
                       catalog.index("LibraryCatalogStore::FindPathResult")]
        self.assertNotIn("validateOrder(ORDER_PATH", load)
        self.assertIn("validateOrderEntry", load)
        self.assertIn("header.entriesCrc == ~crc", load)

        find = catalog[catalog.index("bool LibraryCatalogStore::findPathIndices") :]
        self.assertNotIn("validateOrder(ORDER_PATH", find)
        self.assertIn("validateOrderEntry", find)
        self.assertIn("orderHeader.entriesCrc == ~crc", find)

    def test_library_debug_trace_covers_cold_catalog_to_first_visible(self):
        recent = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        render = recent[recent.index("void RecentBooksActivity::render(RenderLock&&") :]

        for marker in (
                "recent_projection",
                "tab_request",
                "catalog_open",
                "catalog_phase",
                "order_phase",
                "projection ready=",
                "page_load",
                "render_visible",
                "first_visible",
                "nav_input",
                "nav_apply",
                "nav_visible",
        ):
            self.assertIn(f'"{marker}', recent)
        self.assertIn('traceCatalogState("step")', recent)
        self.assertLess(render.index("renderer.displayBuffer();"), render.index('LOG_DBG("LIBT", "first_visible'))
        self.assertLess(render.index("renderer.displayBuffer();"), render.index('LOG_DBG("LIBT",\n              "nav_visible'))
        self.assertNotIn('LOG_DBG("LIBT", "path=', recent)

    def test_library_no_cover_result_is_source_bound_and_persisted_in_catalog(self):
        recent = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        epub = (REPO_ROOT / "lib/Epub/Epub.cpp").read_text(encoding="utf-8")
        catalog = (REPO_ROOT / "src/LibraryCatalogStore.cpp").read_text(encoding="utf-8")
        epub_reader = (REPO_ROOT / "src/activities/reader/EpubReaderActivity.cpp").read_text(encoding="utf-8")
        queue = recent[recent.index("void RecentBooksActivity::processCoverQueue") :
                       recent.index("void RecentBooksActivity::processSelectedSourcePreparation")]
        cover_grid = recent[recent.index("} else if (viewMode() == CrossPointSettings::LIBRARY_COVERS)") :
                            recent.index("  } else {", recent.index("} else if (viewMode() == CrossPointSettings::LIBRARY_COVERS)"))]

        self.assertNotIn('".nocover"', recent)
        self.assertIn("hasVerifiedNoCoverThumbnail", epub)
        self.assertIn("hasVerifiedNoCoverThumbnail", catalog)
        self.assertIn("LibraryCatalogStore::markDirtyPath(book.path);", queue)
        self.assertIn("RECENT_BOOKS.updateBook(book.path, book.title, book.author, {});", queue)
        self.assertIn("coverQueueAbsentMask", recent)
        self.assertIn("coverQueueShownMask |= coverQueueAbsentMask", cover_grid)
        self.assertIn("renderPage[offset].coverBmpPath.clear();", cover_grid)
        self.assertIn("recentBooks[recentIndex].coverBmpPath.clear();", cover_grid)
        self.assertLess(queue.index("const bool coverKnownAbsent"),
                        queue.index("if (book.format == LibraryBookFormat::Epub)"))
        self.assertGreaterEqual(
            epub_reader.count("epub->getCoverItemHref().empty() ? std::string{} : epub->getThumbBmpPath()"),
            2,
        )

    def test_library_cover_grid_tab_previous_moves_to_current_page_tail(self):
        recent = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        navigation = recent[recent.index("void RecentBooksActivity::applyPendingNavigation") :
                            recent.index("void RecentBooksActivity::invalidateRenderPage")]

        self.assertIn("viewMode() == CrossPointSettings::LIBRARY_COVERS && count > 0", navigation)
        self.assertIn("LibraryGridModel::lastIndexOnPage(anchor, count, pageCapacity())", navigation)
        self.assertIn("selectorIndex = controlCount() + target;", navigation)

    def test_library_cover_preparation_preserves_snapshot_and_refreshes_only_new_pixels(self):
        recent = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        snapshot = recent[recent.index("bool RecentBooksActivity::storeGridSnapshot") :
                          recent.index("void RecentBooksActivity::freeGridSnapshot")]
        queue = recent[recent.index("void RecentBooksActivity::processCoverQueue") :
                       recent.index("void RecentBooksActivity::processSelectedSourcePreparation")]

        self.assertNotIn("coverPreparationEpub || coverPreparationXtc", snapshot)
        self.assertNotIn("freeGridSnapshot();", queue)
        self.assertIn("if (drawsCovers && coverQueueReadyMask != 0) requestUpdate();", queue)

    def test_cold_library_catalog_keeps_one_loading_frame_until_content_is_ready(self):
        recent = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        catalog_open_start = recent.index("if (catalogOpenPending && allTab()")
        catalog_open = recent[catalog_open_start : recent.index("constexpr size_t allIndex", catalog_open_start)]
        ready_projection_start = recent.index("if (allTab() && LIBRARY_CATALOG.isReady()", catalog_open_start)
        ready_projection = recent[ready_projection_start :
                                  recent.index("if (searchActive[allIndex]", ready_projection_start)]
        build = recent[recent.index("if (LIBRARY_CATALOG.isBuilding() || LIBRARY_CATALOG.isOrderBuilding())") :
                       recent.index("if (LIBRARY_CATALOG.consumeLastBuildFailed())")]

        self.assertIn("if (!LIBRARY_CATALOG.isBuilding() && !LIBRARY_CATALOG.isOrderBuilding()) requestUpdate();",
                      catalog_open)
        self.assertIn("!LIBRARY_CATALOG.isOrderBuilding()", ready_projection)
        self.assertIn("if (!LIBRARY_CATALOG.isOrderBuilding()) {", ready_projection)
        self.assertNotIn("progressDue", build)
        self.assertNotIn("requestUpdate();", build)

    def test_warm_all_books_skips_the_loading_only_panel_refresh(self):
        recent = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        select_tab = recent[recent.index("void RecentBooksActivity::selectTab") :
                            recent.index("bool RecentBooksActivity::refreshStorageAvailability")]
        on_enter = recent[recent.index("void RecentBooksActivity::onEnter()") :
                          recent.index("void RecentBooksActivity::onExit()")]

        self.assertIn("const bool warmAllBooks", select_tab)
        self.assertIn("if (!warmAllBooks) requestUpdate();", select_tab)
        self.assertIn("if (!warmAllBooks) requestUpdate();", on_enter)

    def test_one_bit_bitmap_fast_path_decodes_palette_once_per_packed_byte(self):
        renderer = (REPO_ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")
        one_bit = renderer[renderer.index("bool GfxRenderer::drawBitmap1Bit") :
                           renderer.index("void GfxRenderer::fillPolygon")]

        self.assertIn("zeroIsInk", one_bit)
        self.assertIn("oneIsInk", one_bit)
        self.assertIn("inkBits", one_bit)
        self.assertIn("if (!isScaled)", one_bit)

    def test_safe_hot_paths_avoid_repeated_work(self):
        renderer = (REPO_ROOT / "lib/GfxRenderer/GfxRenderer.cpp").read_text(encoding="utf-8")
        truncated = renderer[renderer.index("std::string GfxRenderer::truncatedText") :
                             renderer.index("std::vector<std::string> GfxRenderer::wrappedText")]
        self.assertIn("if (finalWidth) *finalWidth = textWidth;", truncated)
        wrapped = renderer[renderer.index("std::vector<std::string> GfxRenderer::wrappedText") :]
        self.assertIn("std::string_view remaining = text;", wrapped)
        self.assertIn("remaining.remove_prefix(spacePos + 1);", wrapped)
        self.assertNotIn("remaining.erase(0,", wrapped)

        base = (REPO_ROOT / "src/components/themes/BaseTheme.cpp").read_text(encoding="utf-8")
        battery_left = base[base.index("void BaseTheme::drawBatteryLeft") : base.index("int BaseTheme::drawBatteryRight")]
        battery_right = base[base.index("int BaseTheme::drawBatteryRight") : base.index("void BaseTheme::drawProgressBar")]
        self.assertNotIn("getBatteryPercentage", battery_left)
        self.assertNotIn("getBatteryPercentage", battery_right)
        self.assertIn("return clusterLeft;", battery_right)

        base_list = base[base.index("void BaseTheme::drawList") : base.index("void BaseTheme::drawHeader")]
        self.assertEqual(base_list.count("rowBadge(i)"), 1)
        crossvi = (REPO_ROOT / "src/components/themes/crossvi/CrossViTheme.cpp").read_text(encoding="utf-8")
        crossvi_list = crossvi[crossvi.index("void CrossViTheme::drawList") :
                               crossvi.index("void CrossViTheme::drawButtonHints")]
        self.assertEqual(crossvi_list.count("rowBadge(i)"), 1)
        self.assertNotIn("batteryClusterLeft", crossvi)

        kosync = (REPO_ROOT / "lib/KOReaderSync/KOReaderSyncClient.cpp").read_text(encoding="utf-8")
        request = kosync[kosync.index("bool performRequest") : kosync.index("}  // namespace")]
        self.assertLess(request.index("if (!authenticated)"), request.index("KOREADER_STORE.getUsername()"))
        self.assertNotIn('\"Basic \" + std::string', request)

        downloader = (REPO_ROOT / "src/network/HttpDownloader.cpp").read_text(encoding="utf-8")
        secure_get = downloader[downloader.index("HttpDownloader::DownloadError runGetSecure") :
                                downloader.index("std::string currentUrl")]
        self.assertNotIn('\"Basic \" + std::string', secure_get)

        recent = (REPO_ROOT / "src/activities/home/RecentBooksActivity.cpp").read_text(encoding="utf-8")
        projection = recent[recent.index("void RecentBooksActivity::rebuildPinnedProjection") :
                            recent.index("bool RecentBooksActivity::pinnedProjectionCurrent")]
        self.assertIn("loadOrderedIndices(SETTINGS.librarySort, excluded, allSourceIndices, paths, &resolved)", projection)
        self.assertNotIn("findPathIndices", projection)

        version_guard = (REPO_ROOT / "src/activities/reader/ReadingStatsVersionGuard.cpp").read_text(encoding="utf-8")
        scan = version_guard[version_guard.index("Result scan(") :]
        self.assertLess(scan.index("Storage.open(directoryPath)"), scan.index("Storage.exists(directoryPath)"))

        for filename, save_signature, next_signature in (
                ("BookReadingStats.cpp", "bool BookReadingStats::save(", "bool BookReadingStats::saveRedundant("),
                ("GlobalReadingStats.cpp", "bool GlobalReadingStats::save(", "bool GlobalReadingStats::saveRedundant(")):
            source = (REPO_ROOT / "src/activities/reader" / filename).read_text(encoding="utf-8")
            save = source[source.index(save_signature) : source.index(next_signature)]
            self.assertIn("storageAllowsPublish", save)
            self.assertNotIn("loadEnvelopePath", save)

    def test_reader_secondary_state_waits_for_first_visible_page(self):
        for filename, class_name in (
                ("EpubReaderActivity.cpp", "EpubReaderActivity"),
                ("TxtReaderActivity.cpp", "TxtReaderActivity"),
                ("XtcReaderActivity.cpp", "XtcReaderActivity")):
            source = (REPO_ROOT / "src/activities/reader" / filename).read_text(encoding="utf-8")
            on_enter = source[source.index(f"void {class_name}::onEnter()") :
                              source.index(f"void {class_name}::onExit()")]
            deferred = source[source.index(f"void {class_name}::finishDeferredOpenState()") :
                              source.index(f"void {class_name}::consumeReadingViewSignal()")]
            consume = source[source.index(f"void {class_name}::consumeReadingViewSignal()") :]
            loop = source[source.index(f"void {class_name}::loop()") :]

            self.assertIn("APP_STATE.saveToFile()", on_enter)
            self.assertNotIn("GlobalReadingStats::load", on_enter)
            self.assertNotIn("RECENT_BOOKS.addBook", on_enter)
            self.assertIn("GlobalReadingStats::load", deferred)
            self.assertIn("RECENT_BOOKS.addBook", deferred)
            self.assertIn("deferredOpenStateReady = true;", consume)
            self.assertIn("if (!deferredOpenStatePending || !deferredOpenStateReady) return;", deferred)
            self.assertIn("finishDeferredOpenState();", loop)
            self.assertIn("deferredGlobalPageTurns", source)

    def test_safe_hot_paths_do_not_clear_or_copy_overwritten_storage(self):
        clipping = (REPO_ROOT / "src/clippings/ClippingPageTools.h").read_text(encoding="utf-8")
        highlight_plan = clipping[clipping.index("struct HighlightLine") : clipping.index("// Invert after page text")]
        self.assertIn("std::array<HighlightLine, MAX_LINES> lines;", highlight_plan)
        self.assertIn("HighlightPlan() noexcept {}", highlight_plan)
        self.assertNotIn("lines{}", highlight_plan)

        for relative_path, declaration in (
                ("lib/Xtc/Xtc/XtcParser.cpp", "std::array<uint8_t, 1024> chunk;"),
                ("lib/Xtc/Xtc/XtcParser.cpp", "std::array<uint8_t, IDENTITY_CHUNK_SIZE> buffer;"),
                ("lib/Txt/Txt.cpp", "std::array<uint8_t, IDENTITY_CHUNK_SIZE> buffer;"),
                ("src/network/NearbyDocumentFingerprint.cpp", "std::array<uint8_t, 2048> buffer;"),
                ("lib/Serialization/StagedFileTransaction.cpp", "std::array<uint8_t, 512> buffer;"),
                ("lib/Epub/Epub.cpp", "std::array<uint8_t, 1024> buffer;"),
                ("lib/ZipFile/ZipFile.cpp", "std::array<uint8_t, 512> buffer;")):
            source = (REPO_ROOT / relative_path).read_text(encoding="utf-8")
            self.assertIn(declaration, source)

        web = (REPO_ROOT / "src/network/CrossPointWebServer.h").read_text(encoding="utf-8")
        upload_status = web[web.index("struct UploadStatus") : web.index("// Used by POST upload handler")]
        self.assertEqual(upload_status.count("std::string_view"), 3)
        self.assertNotIn("std::string filename", upload_status)

        popup = (REPO_ROOT / "src/components/OptionPopup.h").read_text(encoding="utf-8")
        self.assertIn("std::vector<std::string>&& options", popup)
        self.assertIn("ownedStrings = std::move(options);", popup)

        catalog = (REPO_ROOT / "src/LibraryCatalogStore.cpp").read_text(encoding="utf-8")
        read_record = catalog[catalog.index("bool readRecordAt") : catalog.index("size_t orderWorkEntrySize")]
        read_order = catalog[catalog.index("bool readOrderWorkEntryAt") : catalog.index("bool orderPathLess")]
        self.assertIn("DiskRecord disk;", read_record)
        self.assertNotIn("entry = {};", read_order)

        text_block = (REPO_ROOT / "lib/Epub/Epub/blocks/TextBlock.cpp").read_text(encoding="utf-8")
        self.assertIn("const bool hasSourceAnchors", text_block)
        self.assertIn("focusBoundary, focusSuffixX, {}, {}, blockStyle", text_block)
        self.assertNotIn("std::vector<uint32_t>(words.size()", text_block)

        txt = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        interactive = txt[txt.index("std::vector<int16_t> positions(words.size())") :
                          txt.index("std::vector<EpdFontFamily::Style> styles(words.size()")]
        self.assertIn("prefix[prefixLength] = '\\0';", interactive)
        self.assertNotIn("line.substr(0, ranges[index].first)", interactive)

        web = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        open_book = web[web.index("void CrossPointWebServer::handleInboxOpen") :
                        web.index("void CrossPointWebServer::handleUploadPost")]
        self.assertNotIn("Storage.exists(path.c_str())", open_book)


if __name__ == "__main__":
    unittest.main()
