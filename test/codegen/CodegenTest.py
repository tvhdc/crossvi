#!/usr/bin/env python3

import importlib.util
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

    def test_file_transfer_websocket_follows_the_http_port(self):
        files_page = (REPO_ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")
        self.assertIn("const HTTP_PORT = Number(window.location.port || 80);", files_page)
        self.assertIn("const WS_PORT = HTTP_PORT + 1;", files_page)
        self.assertNotIn("const WS_PORT = 81;", files_page)

    def test_file_manager_does_not_embed_untrusted_names_in_html_handlers(self):
        files_page = (REPO_ROOT / "src/network/html/FilesPage.html").read_text(encoding="utf-8")
        self.assertNotIn('onclick="openDeleteModal(', files_page)
        self.assertNotIn('onclick="openMoveModal(', files_page)
        self.assertNotIn('onclick="openRenameModal(', files_page)
        self.assertIn('data-file-action="delete"', files_page)
        self.assertIn("const safeImageName = escapeHtml(img.name);", files_page)
        self.assertNotIn('${img.name}</div>', files_page)

    def test_json_post_handlers_are_bounded_before_webserver_string_allocation(self):
        header = (REPO_ROOT / "src/network/CrossPointWebServer.h").read_text(encoding="utf-8")
        server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        self.assertIn("MAX_JSON_BODY_SIZE = 8192", header)
        handler = server[server.index("void CrossPointWebServer::handleJsonBody()") :]
        self.assertIn('server->header("Content-Type")', handler)
        self.assertLess(handler.index("acceptsRawContentType"), handler.index("server->raw()"))
        self.assertIn('{"Content-Type", "Depth"', server)
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

    def test_crosspoint_sync_extension_is_not_sent_to_custom_servers(self):
        credentials = (REPO_ROOT / "lib/KOReaderSync/KOReaderCredentialStore.cpp").read_text(encoding="utf-8")
        client = (REPO_ROOT / "lib/KOReaderSync/KOReaderSyncClient.cpp").read_text(encoding="utf-8")
        activity = (REPO_ROOT / "src/activities/reader/KOReaderSyncActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("getBaseUrl() == DEFAULT_SERVER_URL", credentials)
        self.assertGreaterEqual(client.count("KOREADER_STORE.usesCrossPointSyncServer()"), 2)
        self.assertIn("if (KOREADER_STORE.usesCrossPointSyncServer())", activity)

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
        # The background tick must gate heap only after it owns the render lock.
        # Counting a second unlocked pre-check would reintroduce the section race.
        self.assertGreaterEqual(reader.count("buildTickHeapGate()"), 2)
        self.assertIn("RenderLock lock(std::try_to_lock);", reader)
        self.assertIn("lock.ownsLock() && section && section->isBuilding()", reader)
        self.assertGreaterEqual(reader.count("ImageBlock::setExtractor(nullptr, nullptr)"), 3)

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
        self.assertRegex(
            reader,
            r"if \(needsFullBuild\) \{\s+renderer\.clearScreen\(\);\s+GUI\.drawPopup\(renderer, tr\(STR_INDEXING\)\);",
        )
        self.assertRegex(
            reader,
            r"if \(showPopup\) \{\s+renderer\.clearScreen\(\);\s+GUI\.drawPopup\(renderer, tr\(STR_INDEXING\)\);",
        )

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
        self.assertIn("SettingInfo::Toggle(StrId::STR_QUICK_RESUME", settings)
        self.assertIn("SettingInfo::DynamicEnum(\n            StrId::STR_SLEEP_SCREEN", settings)
        self.assertNotIn("StrId::STR_COVER_CUSTOM", settings)

    def test_settings_hide_inapplicable_choices_and_defer_dictionary_scan(self):
        settings = (REPO_ROOT / "src/SettingsList.h").read_text(encoding="utf-8")
        activity = (REPO_ROOT / "src/activities/settings/SettingsActivity.cpp").read_text(encoding="utf-8")
        web_server = (REPO_ROOT / "src/network/CrossPointWebServer.cpp").read_text(encoding="utf-8")
        web_page = (REPO_ROOT / "src/network/html/SettingsPage.html").read_text(encoding="utf-8")
        json_settings = (REPO_ROOT / "src/JsonSettingsIO.cpp").read_text(encoding="utf-8")

        self.assertNotIn("SettingInfo::Enum(StrId::STR_LIBRARY_GRID", settings)
        self.assertIn('doc["libraryGrid"] = s.libraryGrid;', json_settings)
        self.assertLess(settings.index("STR_HIDE_TXT_BOOKS"), settings.index("STR_REMOVE_READ_FROM_RECENTS"))
        self.assertLess(settings.index("STR_REMOVE_READ_FROM_RECENTS"), settings.index("STR_QUICK_RESUME"))
        self.assertLess(settings.index("STR_TEXT_AA"), settings.index("STR_TEXT_DARKNESS"))
        self.assertLess(settings.index("STR_EMBEDDED_STYLE"), settings.index("STR_PARA_ALIGNMENT"))

        on_enter = activity[
            activity.index("void SettingsActivity::onEnter") : activity.index("void SettingsActivity::onExit")
        ]
        self.assertNotIn("DictionaryRegistry::discover", on_enter)
        self.assertIn("selectedCategoryIndex == 1 && !dictionariesLoaded", activity)
        self.assertIn("!SETTINGS.textAntiAliasing || SETTINGS.readerDarkMode", activity)
        self.assertIn("setting.enumValues.pop_back()", activity)
        self.assertIn("!display.supportsStripGrayscale()", web_server)
        self.assertIn("row-setting-textDarkness", web_page)
        self.assertIn("bookAlignment.hidden", web_page)

    def test_quick_resume_and_serial_screenshot_use_a_stable_framebuffer(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        sleep = main[main.index("void enterDeepSleep") : main.index("void setupDisplayAndFonts")]
        self.assertLess(sleep.index("saveSleepFrameBuffer();"), sleep.index("activityManager.goToSleep"))
        screenshot = main[main.index('if (cmd == "SCREENSHOT")') :]
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
        self.assertEqual(module.compute_version("gh_release", str(REPO_ROOT)), "1.0.0")
        self.assertEqual(module.compute_version("slim", str(REPO_ROOT)), "1.0.0-slim")
        self.assertEqual(module.compute_version("simulator_x3", str(REPO_ROOT)), "1.0.0-simulator")
        self.assertEqual(module.compute_version("simulator_x4", str(REPO_ROOT)), "1.0.0-simulator")


if __name__ == "__main__":
    unittest.main()
