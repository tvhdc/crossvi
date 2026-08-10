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
        helper = sleep[sleep.index("void displayStrongSleepFrame") : sleep.index("void SleepActivity::onEnter")]
        self.assertIn("constexpr uint8_t X3_SLEEP_CONDITION_PASSES = 1;", sleep)
        self.assertIn("prepareStrongSleepRefresh();", helper)
        self.assertIn("display.displayBuffer(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);", helper)
        self.assertNotIn("display.triggerDisplay(", helper)
        self.assertEqual(sleep.count("displayStrongSleepFrame();"), 5)
        quick_resume = sleep[sleep.index("if (renderQuickResume)") : sleep.index("switch (SETTINGS.sleepScreen)")]
        self.assertIn("renderLastScreenSleepScreen()", quick_resume)
        last_screen = sleep[sleep.index("void SleepActivity::renderLastScreenSleepScreen") :]
        self.assertIn("displayStrongSleepFrame();", last_screen)
        grayscale = sleep[sleep.index("if (hasGreyscale)") : sleep.index("void SleepActivity::renderCoverSleepScreen")]
        self.assertIn("prepareStrongSleepRefresh();", grayscale)
        self.assertIn("renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);", grayscale)
        calendar = sleep[
            sleep.index("void SleepActivity::renderReadingCalendarSleepScreen") :
            sleep.index("void SleepActivity::renderCustomSleepScreen")
        ]
        self.assertIn("displayStrongSleepFrame();", calendar)
        self.assertNotIn("displayBuffer", calendar)

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

    def test_early_startup_sleep_uses_strong_refresh_before_deep_sleep(self):
        main = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8")
        helper = main[main.index("void enterStartupDeepSleep()") : main.index("// Enter deep sleep mode")]
        self.assertIn("display.begin(false);", helper)
        self.assertIn("display.clearScreen();", helper)
        self.assertIn("display.requestResync(STARTUP_SLEEP_CONDITION_PASSES);", helper)
        self.assertIn(
            "display.triggerDisplay(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_REFRESH);", helper
        )
        self.assertLess(helper.index("display.deepSleep();"), helper.index("powerManager.startDeepSleep(gpio);"))
        self.assertEqual(main.count("enterStartupDeepSleep();"), 2)

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
        self.assertIn(
            "selectedRow_ < 0 ? tr(STR_BACK) : I18N.get(TAB_LABELS[selectedTab_])",
            text_settings,
        )
        self.assertIn("rebuildFontOptions()", text_settings)
        self.assertIn("rebuildSizeOptions()", text_settings)
        self.assertNotIn("make_unique<FontSelectionActivity>", text_settings)
        self.assertNotIn("make_unique<FontSizeSelectionActivity>", text_settings)
        self.assertIn("preparedPreviewFontId_ != fontId", text_settings)
        self.assertIn("cache->prewarmCache(fontId, tr(STR_FONT_PREVIEW_TEXT), 0x01)", text_settings)
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

    def test_txt_bom_and_saved_offsets_fail_closed(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        header = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.h").read_text(encoding="utf-8")
        self.assertIn("constexpr uint8_t CACHE_VERSION = 6", reader)
        self.assertIn("TxtLineWrap::leadingUtf8BomBytes(buffer, chunkSize)", reader)
        self.assertIn("bool jumpToStoredByteOffset(uint32_t byteOffset);", header)
        helper = reader[reader.index("bool TxtReaderActivity::jumpToStoredByteOffset") :]
        helper = helper[: helper.index("\n}") + 2]
        self.assertIn("byteOffset >= txt->getFileSize()", helper)
        self.assertGreaterEqual(reader.count("jumpToStoredByteOffset("), 5)

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
        submenu = (REPO_ROOT / "src/activities/settings/SettingsSubmenuActivity.cpp").read_text(encoding="utf-8")
        self.assertIn("SettingInfo::Toggle(StrId::STR_QUICK_RESUME", settings)
        self.assertIn("SettingInfo::DynamicEnum(\n            StrId::STR_SLEEP_SCREEN", settings)
        self.assertNotIn("StrId::STR_COVER_CUSTOM", settings)
        self.assertIn("StrId::STR_READING_STATS", settings)
        self.assertIn("StrId::STR_READING_STATS", submenu)

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
        self.assertEqual(module.compute_version("gh_release", str(REPO_ROOT)), "1.0.1")
        self.assertEqual(module.compute_version("slim", str(REPO_ROOT)), "1.0.1-slim")
        self.assertEqual(module.compute_version("simulator_x3", str(REPO_ROOT)), "1.0.1-simulator")
        self.assertEqual(module.compute_version("simulator_x4", str(REPO_ROOT)), "1.0.1-simulator")

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

    def test_txt_font_scan_skips_unused_layout_measurements(self):
        reader = (REPO_ROOT / "src/activities/reader/TxtReaderActivity.cpp").read_text(encoding="utf-8")
        prewarm = reader[reader.index("void TxtReaderActivity::prewarmCurrentPageFont") :
                         reader.index("void TxtReaderActivity::renderCurrentPageLines")]
        self.assertNotIn("renderCurrentPageLines()", prewarm)
        self.assertIn("renderer.drawText(cachedFontId, 0, 0, line.c_str())", prewarm)

        render_lines = reader[reader.index("void TxtReaderActivity::renderCurrentPageLines") :
                              reader.index("void TxtReaderActivity::renderPage")]
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
