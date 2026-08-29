#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#include <FsHelpers.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <StagedFileTransaction.h>
#include <Version.h>
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <string_view>
#include <vector>

#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"
#include "SettingsApiUtils.h"
#include "SettingsList.h"
#include "UploadPathGuard.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "network/HttpFileStreamer.h"
#include "util/BookCacheUtils.h"
#include "util/BookPathMoveUtils.h"
#include "util/TaskWatchdog.h"

namespace {
// Folders/files to hide from the web interface file browser
// Note: Items starting with "." are automatically hidden
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;
constexpr unsigned long UPLOAD_SOCKET_TIMEOUT_MS = 30000;

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool normalizeSafeWebPath(const String& inputPath, String& outputPath, const bool allowRoot = true) {
  String candidate = inputPath;
  if (candidate.isEmpty()) candidate = "/";
  if (!candidate.startsWith("/")) candidate = "/" + candidate;

  // Validate before normalising so traversal/repeated-separator spellings are
  // rejected instead of being silently canonicalised into a protected path.
  if (!UploadPathGuard::isSafeAbsolutePath(candidate.c_str(), allowRoot)) return false;

  outputPath = normalizeWebPath(candidate);
  return UploadPathGuard::isSafeAbsolutePath(outputPath.c_str(), allowRoot);
}

bool isProtectedItemName(const String& name) {
  if (name.startsWith(".")) {
    return true;
  }
  for (const auto* item : HIDDEN_ITEMS) {
    if (name.equals(item)) {
      return true;
    }
  }
  return false;
}

bool isSupportedReaderFile(const std::string_view path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);
}

}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  // AP mode can reach the web settings without ever opening WifiSelectionActivity.
  // Load before exposing mutation endpoints so saving cannot replace the file
  // from an empty in-memory credential list.
  WIFI_STORE.loadFromFile();

  LOG_DBG("WEB", "Creating web server on port %d...", port);

  server = makeUniqueNoThrow<WebServer>(port);

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer");
    return;
  }

  // Keep the radio awake while the file-transfer server is open. This matches
  // the upstream reliability policy and avoids beacon/association loss seen on
  // real X3 hardware. Wi-Fi is still shut down when the activity exits.
  WiFi.setSleep(false);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  const auto jsonBodyHandler = [this] { handleJsonBody(); };
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });
  server->on(
      "/api/upload/chunk", HTTP_POST, [this] { handleCooperativeUploadPost(); },
      [this] { handleCooperativeUploadData(); });
  server->on("/api/upload/cancel", HTTP_POST, [this] { handleCooperativeUploadCancel(); });
  server->on("/api/inbox/open", HTTP_POST, [this] { handleInboxOpen(); }, jsonBodyHandler);

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); }, jsonBodyHandler);

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); }, jsonBodyHandler);

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); }, jsonBodyHandler);
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); }, jsonBodyHandler);

  // Wi-Fi credential endpoints
  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); }, jsonBodyHandler);
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); }, jsonBodyHandler);

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  // Collect WebDAV headers and register handler
  const char* requestHeaders[] = {"Content-Type",
                                  "Depth",
                                  "Destination",
                                  "Overwrite",
                                  "If",
                                  "Lock-Token",
                                  "Timeout",
                                  "X-CrossVi-Upload-Path",
                                  "X-CrossVi-Upload-Name",
                                  "X-CrossVi-Upload-Offset",
                                  "X-CrossVi-Upload-Total",
                                  "X-CrossVi-Upload-Type",
                                  "X-CrossVi-Font-Family"};
  server->collectHeaders(requestHeaders, 13);
  auto* webDavHandler = new (std::nothrow) WebDAVHandler();
  if (!webDavHandler) {
    LOG_ERR("WEB", "Failed to allocate WebDAV handler");
    server.reset();
    return;
  }
  server->addHandler(webDavHandler);  // WebServer owns and deletes the handler.
  LOG_DBG("WEB", "WebDAV handler initialized");

  server->begin();

  lastCompleteName.clear();
  lastCompleteSize = 0;
  lastCompleteAt = 0;
  lastCompletePath.clear();
  pendingOpenPath.clear();

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

bool CrossPointWebServer::clearStaleBookUploadStaging(const String& stagingPath) {
  if (!Storage.exists(stagingPath.c_str())) return true;

  // A live HTTP upload owns its staging file. Never discard it merely because
  // another client retries the same name.
  if (upload.ownsStagingFile && upload.stagingPath == stagingPath) {
    LOG_DBG("WEB", "[UPLOAD] Staging file is still owned: %s", stagingPath.c_str());
    return false;
  }
  if (cooperativeUpload.ownsStagingFile && cooperativeUpload.stagingPath == stagingPath) {
    LOG_DBG("WEB", "[UPLOAD] Cooperative staging file is still owned: %s", stagingPath.c_str());
    return false;
  }

  // No active uploader can own this sibling, so it is an interrupted upload
  // from an earlier connection or reboot and can be safely restarted.
  if (!Storage.remove(stagingPath.c_str())) {
    LOG_DBG("WEB", "[UPLOAD] Failed to remove stale staging file: %s", stagingPath.c_str());
    return false;
  }
  LOG_DBG("WEB", "[UPLOAD] Removed stale staging file: %s", stagingPath.c_str());
  return true;
}

void CrossPointWebServer::stop() {
  jsonBody = {};
  discardCooperativeUpload(true);
  if (upload.file) upload.file.close();
  if (upload.ownsStagingFile && !upload.stagingPath.isEmpty()) Storage.remove(upload.stagingPath.c_str());
  upload.ownsStagingFile = false;
  upload.stagingPath = "";

  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  static unsigned long lastDebugPrint = 0;
#endif

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }
#endif

  server->handleClient();

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(port);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::UploadStatus CrossPointWebServer::getUploadStatus() const {
  UploadStatus status;
  status.inProgress = upload.ownsStagingFile || cooperativeUpload.ownsStagingFile;
  if (cooperativeUpload.ownsStagingFile) {
    status.received = cooperativeUpload.committed + cooperativeUpload.requestReceived;
    status.total = cooperativeUpload.total;
    status.filename = cooperativeUpload.fileName.c_str();
  } else {
    status.received = upload.size;
    status.filename = upload.fileName.c_str();
  }
  status.lastCompleteName = lastCompleteName;
  status.lastCompletePath = lastCompletePath;
  status.lastCompleteSize = lastCompleteSize;
  status.lastCompleteAt = lastCompleteAt;
  return status;
}

bool CrossPointWebServer::hasActiveTransfer() const {
  return upload.ownsStagingFile || cooperativeUpload.ownsStagingFile;
}

bool CrossPointWebServer::takeOpenRequest(std::string& path) {
  if (pendingOpenPath.empty()) return false;
  path = std::move(pendingOpenPath);
  pendingOpenPath.clear();
  return true;
}

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleJszip() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "application/javascript", jszip_minJs, jszip_minJsCompressedSize);
  LOG_DBG("WEB", "Served jszip.min.js");
}

void CrossPointWebServer::handleNotFound() const {
  // in AP mode, redirect unmatched browser/captive-portal requests to "/" so the OS auto-opens the browser
  // API requests (/api/*) still return 404 so XHR errors surface correctly
  // see https://en.wikipedia.org/wiki/Captive_portal#Detection
  if (apMode && !server->uri().startsWith("/api/")) {
    server->sendHeader("Location", "/", true);
    server->send(302, "text/plain", "");
    return;
  }

  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleJsonBody() {
  const String contentType = server->header("Content-Type");
  if (!JsonBodyBuffer::acceptsRawContentType(contentType.c_str())) {
    jsonBody.rejectContentType();
    return;
  }

  HTTPRaw& raw = server->raw();
  if (raw.status == RAW_START) {
    jsonBody.start(MAX_JSON_BODY_SIZE);
    return;
  }

  if (raw.status == RAW_WRITE) {
    jsonBody.write(raw.buf, raw.currentSize, MAX_JSON_BODY_SIZE);
    return;
  }

  if (raw.status == RAW_END) {
    jsonBody.finish();
    return;
  }

  jsonBody.abort();
}

std::unique_ptr<uint8_t[]> CrossPointWebServer::takeJsonBody(const char* errorContentType) {
  const String contentType = server->header("Content-Type");
  if (!JsonBodyBuffer::acceptsRawContentType(contentType.c_str())) {
    jsonBody = {};
    server->send(400, errorContentType, "JSON endpoint does not accept form data");
    return nullptr;
  }

  JsonBodyBuffer::State body = jsonBody.take();
  if (body.error == JsonBodyBuffer::Error::TooLarge) {
    server->send(413, errorContentType, "JSON request is too large");
  } else if (body.error == JsonBodyBuffer::Error::Allocation) {
    server->send(503, errorContentType, "Not enough memory for request");
  } else if (body.error == JsonBodyBuffer::Error::Aborted || !body.complete) {
    server->send(400, errorContentType, "Incomplete JSON body");
  } else if (body.size == 0) {
    server->send(400, errorContentType, "Missing JSON body");
  } else {
    return std::move(body.data);
  }
  return nullptr;
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = CROSSPOINT_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";
  doc["language"] = I18N.getLanguage() == Language::VI ? "VI" : "EN";

  char snBuf[33] = {0};
  bool valid = false;
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, snBuf, 256) == ESP_OK) {
    valid = snBuf[0] != '\0' && snBuf[0] != (char)0xFF;
    for (int i = 0; i < 32 && snBuf[i] != '\0'; i++) {
      if (!std::isprint(static_cast<unsigned char>(snBuf[i]))) {
        valid = false;
        break;
      }
    }
  }

  if (valid) {
    doc["serial"] = snBuf;
  } else {
    doc["serial"] = "Not found";
  }

  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
}

void CrossPointWebServer::scanFiles(const char* path, const FileVisitor visitor, void* context) const {
  HalFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  HalFile file = root.openNextFile();
  char name[500];
  while (file) {
    name[0] = '\0';
    name[sizeof(name) - 1] = '\0';
    const size_t nameLength = file.getName(name, sizeof(name));
    if (nameLength == 0 || nameLength >= sizeof(name) || name[0] == '\0' || name[nameLength] != '\0') {
      LOG_ERR("WEB", "Failed to read a directory entry name in: %s", path);
      file.close();
      yield();
      resetTaskWatchdogIfSubscribed();
      file = root.openNextFile();
      continue;
    }
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide =
        isBookFileTransactionArtifact(fileName.c_str()) || (!SETTINGS.showHiddenFiles && fileName.startsWith("."));

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (const auto* item : HIDDEN_ITEMS) {
        if (fileName.equals(item)) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      visitor(info, context);
    }

    file.close();
    yield();                          // Yield to allow WiFi and other tasks to process during long scans
    resetTaskWatchdogIfSubscribed();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath;
  const String requestedPath = server->hasArg("path") ? server->arg("path") : String("/");
  if (!normalizeSafeWebPath(requestedPath, currentPath)) {
    server->send(403, "application/json", "{\"error\":\"Cannot access protected path\"}");
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  constexpr size_t BATCH_CAPACITY = 1400;
  auto batch = makeUniqueNoThrow<char[]>(BATCH_CAPACITY);
  char output[512];
  JsonDocument doc;

  struct FileListContext {
    WebServer* server;
    char* batch;
    size_t batchLength;
    char* output;
    size_t outputCapacity;
    JsonDocument* document;
    bool seenFirst;
  } context{server.get(), batch.get(), 0, output, sizeof(output), &doc, false};

  if (batch) {
    batch[context.batchLength++] = '[';
  } else {
    LOG_ERR("WEB", "OOM: file list batch buffer; using per-entry sends");
    server->sendContent("[");
  }

  scanFiles(
      currentPath.c_str(),
      [](const FileInfo& info, void* rawContext) {
        auto& context = *static_cast<FileListContext*>(rawContext);
        context.document->clear();
        (*context.document)["name"] = info.name;
        (*context.document)["size"] = info.size;
        (*context.document)["isDirectory"] = info.isDirectory;
        (*context.document)["isEpub"] = info.isEpub;

        const size_t written = serializeJson(*context.document, context.output, context.outputCapacity);
        if (written >= context.outputCapacity) {
          LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
          return;
        }

        const size_t required = written + (context.seenFirst ? 1 : 0);
        if (context.batch) {
          if (context.batchLength + required > BATCH_CAPACITY) {
            context.server->sendContent(context.batch, context.batchLength);
            context.batchLength = 0;
          }
          if (context.seenFirst) context.batch[context.batchLength++] = ',';
          memcpy(context.batch + context.batchLength, context.output, written);
          context.batchLength += written;
        } else {
          if (context.seenFirst) context.server->sendContent(",");
          context.server->sendContent(context.output);
        }
        context.seenFirst = true;
      },
      &context);

  if (batch) {
    if (context.batchLength + 1 > BATCH_CAPACITY) {
      server->sendContent(batch.get(), context.batchLength);
      context.batchLength = 0;
    }
    batch[context.batchLength++] = ']';
    server->sendContent(batch.get(), context.batchLength);
  } else {
    server->sendContent("]");
  }
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
  LOG_DBG("WEB", "Served file listing page for path: %s", currentPath.c_str());
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath;
  if (!normalizeSafeWebPath(server->arg("path"), itemPath, false)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  uint8_t buffer[4096];
  // A file download streams synchronously inside this single handleClient
  // call, which can take seconds. The activity's per-batch modem-sleep toggle
  // was already decided before the request was processed, so keep the radio
  // awake here — like the upload path — or throughput drops during the stream.
  // Restore the previous sleep state afterwards.
  const bool modemSleep = !apMode && WiFi.getSleep();
  if (modemSleep) WiFi.setSleep(false);
  const auto result = streamHttpFile(file, client, buffer, sizeof(buffer), [] {
    resetTaskWatchdogIfSubscribed();
    yield();
  });
  if (modemSleep) WiFi.setSleep(true);
  if (!result.complete()) {
    LOG_ERR("WEB", "Download interrupted: sent=%u expected=%u", static_cast<unsigned>(result.bytesSent),
            static_cast<unsigned>(result.expectedBytes));
  }
  client.clear();
  client.stop();
  file.close();
}

#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;
#endif

bool CrossPointWebServer::flushUploadBuffer(UploadState& state) {
  if (state.bufferPos > 0 && state.file) {
    resetTaskWatchdogIfSubscribed();  // Reset watchdog before potentially slow SD write
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const unsigned long writeStart = millis();
#endif
    const size_t written = state.file.write(transferBuffer.data(), state.bufferPos);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    totalWriteTime += millis() - writeStart;
    writeCount++;
#endif
    resetTaskWatchdogIfSubscribed();  // Reset watchdog after SD write

    if (written != state.bufferPos) {
      LOG_DBG("WEB", "[UPLOAD] Buffer flush failed: expected %d, wrote %d", state.bufferPos, written);
      state.bufferPos = 0;
      return false;
    }
    state.bufferPos = 0;
  }
  return true;
}

bool CrossPointWebServer::flushCooperativeUploadBuffer() {
  auto& state = cooperativeUpload;
  if (state.bufferPos == 0) return true;
  if (!state.file) {
    state.bufferPos = 0;
    return false;
  }

  resetTaskWatchdogIfSubscribed();
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const unsigned long writeStart = millis();
#endif
  const size_t written = state.file.write(transferBuffer.data(), state.bufferPos);
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  totalWriteTime += millis() - writeStart;
  ++writeCount;
#endif
  resetTaskWatchdogIfSubscribed();
  if (written != state.bufferPos) {
    LOG_ERR("WEB", "[UPLOAD-COOP] Buffer flush failed: expected=%u wrote=%u", static_cast<unsigned>(state.bufferPos),
            static_cast<unsigned>(written));
    state.bufferPos = 0;
    return false;
  }
  state.bufferPos = 0;
  return true;
}

void CrossPointWebServer::discardCooperativeUpload(const bool removeStaging) {
  auto& state = cooperativeUpload;
  if (state.file) state.file.close();
  if (removeStaging && state.ownsStagingFile && !state.stagingPath.isEmpty()) {
    Storage.remove(state.stagingPath.c_str());
  }
  state = CooperativeUploadState{};
}

void CrossPointWebServer::handleCooperativeUploadData() {
  auto& state = cooperativeUpload;
  resetTaskWatchdogIfSubscribed();

  const String contentType = server->header("Content-Type");
  if (!contentType.startsWith("application/octet-stream")) {
    state.requestAccepted = false;
    state.requestComplete = true;
    state.responseStatus = 400;
    state.error = "Chunk upload requires application/octet-stream";
    return;
  }

  HTTPRaw& raw = server->raw();
  const auto failRequest = [&](const int status, const String& error, const bool discardUpload) {
    if (discardUpload) discardCooperativeUpload(true);
    state.requestAccepted = false;
    state.requestComplete = true;
    state.responseStatus = status;
    state.error = error;
  };

  if (raw.status == RAW_START) {
    state.requestAccepted = false;
    state.requestComplete = false;
    state.requestSize = 0;
    state.requestReceived = 0;
    state.bufferPos = 0;
    state.requestReplay = false;
    state.responseStatus = 400;
    state.error = "";

    if (!server->hasHeader("X-CrossVi-Upload-Name") || !server->hasHeader("X-CrossVi-Upload-Offset") ||
        !server->hasHeader("X-CrossVi-Upload-Total")) {
      failRequest(400, "Missing chunk upload parameters", false);
      return;
    }

    const String uploadType =
        server->hasHeader("X-CrossVi-Upload-Type") ? server->header("X-CrossVi-Upload-Type") : String("book");
    const CooperativeUploadKind requestedKind =
        uploadType == "font" ? CooperativeUploadKind::Font : CooperativeUploadKind::Book;
    if (uploadType != "book" && uploadType != "font") {
      failRequest(400, "Unsupported upload type", false);
      return;
    }

    String path = "/";
    String familyName;
    if (requestedKind == CooperativeUploadKind::Book) {
      if (!server->hasHeader("X-CrossVi-Upload-Path")) {
        failRequest(400, "Missing upload path", false);
        return;
      }
      path = WebServer::urlDecode(server->header("X-CrossVi-Upload-Path"));
      if (!path.startsWith("/")) path = "/" + path;
      if (path.length() > 1 && path.endsWith("/")) path.remove(path.length() - 1);
    } else {
      if (!server->hasHeader("X-CrossVi-Font-Family")) {
        failRequest(400, "Missing font family", false);
        return;
      }
      familyName = WebServer::urlDecode(server->header("X-CrossVi-Font-Family"));
    }

    const String fileName = WebServer::urlDecode(server->header("X-CrossVi-Upload-Name"));
    size_t offset = 0;
    size_t total = 0;
    const long contentLength = server->clientContentLength();
    if (!UploadPathGuard::parseSize(server->header("X-CrossVi-Upload-Offset").c_str(), offset) ||
        !UploadPathGuard::parseSize(server->header("X-CrossVi-Upload-Total").c_str(), total) || contentLength <= 0 ||
        !UploadPathGuard::isValidChunkRange(offset, total, static_cast<size_t>(contentLength),
                                            COOPERATIVE_UPLOAD_CHUNK_SIZE)) {
      failRequest(contentLength > static_cast<long>(COOPERATIVE_UPLOAD_CHUNK_SIZE) ? 413 : 400, "Invalid chunk range",
                  false);
      return;
    }
    const bool validDestination =
        requestedKind == CooperativeUploadKind::Font
            ? FontInstaller::isValidFamilyName(familyName.c_str()) &&
                  FontInstaller::isValidCpfontFilename(fileName.c_str())
            : UploadPathGuard::isSafeLeafName(fileName.c_str()) && UploadPathGuard::isSafeAbsolutePath(path.c_str());
    if (!validDestination) {
      failRequest(400,
                  requestedKind == CooperativeUploadKind::Font ? "Invalid font family or file name"
                                                               : "Invalid upload path or file name",
                  false);
      return;
    }

    const size_t requestEnd = offset + static_cast<size_t>(contentLength);
    const bool sameActiveUpload = state.ownsStagingFile && state.kind == requestedKind && state.fileName == fileName &&
                                  state.path == path && state.familyName == familyName && state.total == total;
    const bool canResumeRequest = sameActiveUpload && (state.committed == offset || state.committed == requestEnd);

    if (offset == 0) {
      if (upload.ownsStagingFile) {
        failRequest(409, "Another upload is already in progress", false);
        return;
      }
      if (state.ownsStagingFile && !canResumeRequest) {
        // A normal offset-zero request starts a fresh upload. An exact retry
        // keeps the existing staging file so a transient socket failure does
        // not force the client to resend the whole book.
        discardCooperativeUpload(true);
      }

      if (!state.ownsStagingFile) {
        String finalPath;
        String stagingPath;
        String backupPath;
        if (requestedKind == CooperativeUploadKind::Font) {
          FontInstaller installer(sdFontSystem.registry());
          if (!installer.ensureFamilyDir(familyName.c_str())) {
            failRequest(500, "Could not create the font folder", false);
            return;
          }
          char destination[FontStorageUtils::FONT_PATH_CAPACITY];
          if (!FontInstaller::buildFontPath(familyName.c_str(), fileName.c_str(), destination, sizeof(destination))) {
            failRequest(400, "Font name or path is too long", false);
            return;
          }
          finalPath = destination;
          stagingPath = finalPath + ".upload.tmp";
          backupPath = finalPath + ".upload.bak";
          const auto validateFont = [](const char* candidate, void* context) {
            return static_cast<FontInstaller*>(context)->validateCpfontFile(candidate);
          };
          if (StagedFileTransaction::recover(finalPath.c_str(), backupPath.c_str(), validateFont, &installer) ==
                  StagedFileTransaction::Status::IoError ||
              (Storage.exists(stagingPath.c_str()) && !Storage.remove(stagingPath.c_str()))) {
            failRequest(500, "Could not recover an interrupted font upload", false);
            return;
          }
        } else {
          if (path == "/Inbox" && !isSupportedReaderFile(fileName.c_str())) {
            failRequest(400, "Inbox accepts reader files only", false);
            return;
          }
          if (path == "/Inbox" && !Storage.exists("/Inbox") && !Storage.mkdir("/Inbox")) {
            failRequest(500, "Could not create Inbox", false);
            return;
          }
          HalFile uploadDirectory = Storage.open(path.c_str());
          if (!uploadDirectory || !uploadDirectory.isDirectory()) {
            if (uploadDirectory) uploadDirectory.close();
            failRequest(400, "Upload folder does not exist", false);
            return;
          }
          uploadDirectory.close();

          finalPath = path;
          if (!finalPath.endsWith("/")) finalPath += "/";
          finalPath += fileName;
          if (Storage.exists(finalPath.c_str())) {
            failRequest(409, "File already exists: " + fileName, false);
            return;
          }
          stagingPath = hiddenBookFileSibling(finalPath.c_str(), ".crossvi-upload.tmp").c_str();
          if (!clearStaleBookUploadStaging(stagingPath)) {
            failRequest(500, "Could not clear interrupted upload", false);
            return;
          }
        }

        state.fileName = fileName;
        state.path = path;
        state.familyName = familyName;
        state.finalPath = finalPath;
        state.stagingPath = stagingPath;
        state.backupPath = backupPath;
        state.kind = requestedKind;
        state.total = total;
        state.streamDigest = {};
        if (!Storage.openFileForWrite("WEB", state.stagingPath, state.file)) {
          failRequest(500,
                      requestedKind == CooperativeUploadKind::Font ? "Could not create the temporary font file"
                                                                   : "Failed to create file on SD card",
                      false);
          return;
        }
        state.ownsStagingFile = true;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
        uploadStartTime = millis();
        totalWriteTime = 0;
        writeCount = 0;
#endif
        LOG_DBG("WEB", "[UPLOAD-COOP] START: type=%s %s (%u bytes) to %s", uploadType.c_str(), state.fileName.c_str(),
                static_cast<unsigned>(state.total), state.finalPath.c_str());
#ifndef SIMULATOR
        if (!apMode) {
          LOG_DBG("WEB", "[UPLOAD-COOP] WiFi: RSSI=%d dBm channel=%d BSSID=%s", WiFi.RSSI(), WiFi.channel(),
                  WiFi.BSSIDstr().c_str());
        }
#endif
        LOG_DBG("WEB", "[UPLOAD-COOP] Heap: free=%u maxalloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      }
    } else if (!canResumeRequest) {
      failRequest(409, "Chunk does not match the active upload", false);
      return;
    }

    state.requestReplay = state.committed == requestEnd;
    if (!state.requestReplay && (!state.file || !state.file.seek(state.committed))) {
      failRequest(500, "Could not resume the interrupted upload", true);
      return;
    }
    state.requestDigestStart = state.streamDigest;

    server->client().setTimeout(UPLOAD_SOCKET_TIMEOUT_MS);
    state.requestSize = static_cast<size_t>(contentLength);
    state.requestAccepted = true;
    state.responseStatus = 200;
    return;
  }

  if (raw.status == RAW_WRITE) {
    if (!state.requestAccepted) return;
    if (raw.currentSize > state.requestSize - state.requestReceived) {
      failRequest(400, "Chunk body exceeds declared range", true);
      return;
    }

    if (state.requestReplay) {
      state.requestReceived += raw.currentSize;
      return;
    }

    const uint8_t* data = raw.buf;
    size_t remaining = raw.currentSize;
    while (remaining > 0) {
      const size_t space = transferBuffer.size() - state.bufferPos;
      const size_t toCopy = std::min(remaining, space);
      memcpy(transferBuffer.data() + state.bufferPos, data, toCopy);
      if (state.kind == CooperativeUploadKind::Font) {
        StagedFileTransaction::updateDigest(state.streamDigest, data, toCopy);
      }
      state.bufferPos += toCopy;
      data += toCopy;
      remaining -= toCopy;
      if (state.bufferPos == transferBuffer.size() && !flushCooperativeUploadBuffer()) {
        failRequest(500, "Failed to write to SD card", true);
        return;
      }
    }
    state.requestReceived += raw.currentSize;
    return;
  }

  if (raw.status == RAW_ABORTED) {
    const size_t partialBytes = state.requestReceived;
    state.bufferPos = 0;
    state.streamDigest = state.requestDigestStart;
    const bool rewound = state.file && state.file.seek(state.committed);
    LOG_ERR("WEB", "[UPLOAD-COOP] Request aborted at %u/%u bytes (%u-byte request discarded)",
            static_cast<unsigned>(state.committed), static_cast<unsigned>(state.total),
            static_cast<unsigned>(partialBytes));
    if (!rewound) {
      discardCooperativeUpload(true);
      return;
    }
    state.requestAccepted = false;
    state.requestComplete = false;
    state.requestReplay = false;
    state.requestSize = 0;
    state.requestReceived = 0;
    state.error = "";
    return;
  }

  if (!state.requestAccepted) {
    state.requestComplete = true;
    return;
  }
  if (state.requestReceived != state.requestSize || (!state.requestReplay && !flushCooperativeUploadBuffer())) {
    failRequest(500, "Incomplete chunk or SD write failure", true);
    return;
  }

  if (state.requestReplay) {
    state.requestComplete = true;
    return;
  }

  state.committed += state.requestReceived;
  state.requestComplete = true;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (state.committed - state.lastLoggedSize >= 1024U * 1024U || state.committed == state.total) {
    const unsigned long elapsed = millis() - uploadStartTime;
    const float kbps = elapsed > 0 ? (state.committed / 1024.0f) / (elapsed / 1000.0f) : 0;
    LOG_DBG("WEB", "[UPLOAD-COOP] %u/%u bytes, %.1f KB/s, %u writes", static_cast<unsigned>(state.committed),
            static_cast<unsigned>(state.total), kbps, static_cast<unsigned>(writeCount));
    state.lastLoggedSize = state.committed;
  }
#endif
  if (state.committed < state.total) return;

  const bool sizeMatches = state.file.size() == state.total;
  const bool synced = sizeMatches && state.file.sync();
  const bool closed = state.file.close();
  if (!sizeMatches || !synced || !closed) {
    failRequest(500, "Could not safely store uploaded file", true);
    return;
  }

  if (state.kind == CooperativeUploadKind::Font) {
    FontInstaller installer(sdFontSystem.registry());
    const auto validateFont = [](const char* candidate, void* context) {
      return static_cast<FontInstaller*>(context)->validateCpfontFile(candidate);
    };
    const auto published =
        StagedFileTransaction::publishAndVerify(state.finalPath.c_str(), state.stagingPath.c_str(),
                                                state.backupPath.c_str(), state.streamDigest, validateFont, &installer);
    if (published != StagedFileTransaction::Status::Published) {
      failRequest(published == StagedFileTransaction::Status::InvalidStaging ? 400 : 500,
                  published == StagedFileTransaction::Status::InvalidStaging
                      ? "The uploaded file is not a valid compatible .cpfont"
                      : "The validated font could not be installed",
                  true);
      return;
    }
    sdFontSystem.markRegistryDirty();
  } else {
    const BookFilePublishResult published = publishStagedBookFile(state.stagingPath.c_str(), state.finalPath.c_str());
    if (published != BookFilePublishResult::Published && published != BookFilePublishResult::Unchanged) {
      failRequest(published == BookFilePublishResult::InvalidStagedFile ? 400 : 500,
                  published == BookFilePublishResult::InvalidStagedFile ? "Uploaded book file is invalid"
                                                                        : "Could not safely publish uploaded file",
                  true);
      return;
    }
  }

  state.ownsStagingFile = false;
  state.uploadComplete = true;
  if (state.kind == CooperativeUploadKind::Book) {
    lastCompletePath =
        isSupportedReaderFile(state.finalPath.c_str()) ? std::string(state.finalPath.c_str()) : std::string{};
    lastCompleteName = state.fileName.c_str();
    lastCompleteSize = state.total;
    lastCompleteAt = millis();
  }
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  const unsigned long elapsed = millis() - uploadStartTime;
  const float writePercent = elapsed > 0 ? totalWriteTime * 100.0f / elapsed : 0;
  LOG_DBG("WEB", "[UPLOAD-COOP] Complete: %s (%u bytes in %lu ms, %u writes, SD %.1f%%)", state.fileName.c_str(),
          static_cast<unsigned>(state.total), elapsed, static_cast<unsigned>(writeCount), writePercent);
#endif
}

void CrossPointWebServer::handleCooperativeUploadPost() {
  auto& state = cooperativeUpload;
  if (!state.requestComplete) {
    server->send(400, "text/plain", "Incomplete cooperative upload request");
    return;
  }

  if (!state.error.isEmpty()) {
    const int status = state.responseStatus;
    const String error = state.error;
    const bool activeUploadRemains = state.ownsStagingFile;
    state.requestAccepted = false;
    state.requestComplete = false;
    state.requestSize = 0;
    state.requestReceived = 0;
    state.error = "";
    if (!activeUploadRemains) discardCooperativeUpload(false);
    server->send(status, "text/plain", error);
    return;
  }

  const bool complete = state.uploadComplete;
  const size_t committed = state.committed;
  state.requestAccepted = false;
  state.requestComplete = false;
  state.requestSize = 0;
  state.requestReceived = 0;
  server->send(200, "text/plain", String(committed));
  if (complete) discardCooperativeUpload(false);
}

void CrossPointWebServer::handleCooperativeUploadCancel() {
  const bool hadUpload = cooperativeUpload.ownsStagingFile;
  discardCooperativeUpload(true);
  server->send(hadUpload ? 200 : 204, "text/plain", hadUpload ? "Upload cancelled" : "");
}

void CrossPointWebServer::handleUpload(UploadState& state) {
  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  resetTaskWatchdogIfSubscribed();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const String contentType = server->header("Content-Type");
  if (!JsonBodyBuffer::acceptsMultipartUploadContentType(contentType.c_str())) {
    state.success = false;
    state.error = "Upload requires multipart/form-data";
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // Reset watchdog - this is the critical 1% crash point
    resetTaskWatchdogIfSubscribed();

    // Arduino-ESP32 resets every accepted client to a 5-second read timeout.
    // A brief Wi-Fi retry can otherwise abort an otherwise healthy large
    // multipart upload. This only extends the active upload socket; a closed
    // connection still fails immediately.
    server->client().setTimeout(UPLOAD_SOCKET_TIMEOUT_MS);

    if (state.ownsStagingFile || cooperativeUpload.ownsStagingFile) {
      state.error = "Upload already in progress";
      return;
    }
    state.restoreModemSleep = !apMode && WiFi.getSleep();
    if (state.restoreModemSleep) WiFi.setSleep(false);
    if (state.file) state.file.close();
    state.fileName = upload.filename;
    state.size = 0;
    state.success = false;
    state.error = "";
    state.stagingPath = "";
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    uploadStartTime = millis();
#endif
    state.bufferPos = 0;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    totalWriteTime = 0;
    writeCount = 0;
#endif

    if (!UploadPathGuard::isSafeLeafName(state.fileName.c_str())) {
      state.error = "Invalid file name";
      return;
    }

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = server->arg("path");
      // Ensure path starts with /
      if (!state.path.startsWith("/")) {
        state.path = "/" + state.path;
      }
      // Remove trailing slash unless it's root
      if (state.path.length() > 1 && state.path.endsWith("/")) {
        state.path = state.path.substring(0, state.path.length() - 1);
      }
    } else {
      state.path = "/";
    }

    if (!UploadPathGuard::isSafeAbsolutePath(state.path.c_str())) {
      state.error = "Invalid upload path";
      return;
    }
    if (state.path == "/Inbox" && !isSupportedReaderFile(state.fileName.c_str())) {
      state.error = "Inbox accepts reader files only";
      return;
    }
    if (state.path == "/Inbox" && !Storage.exists("/Inbox") && !Storage.mkdir("/Inbox")) {
      state.error = "Could not create Inbox";
      return;
    }
    HalFile uploadDirectory = Storage.open(state.path.c_str());
    if (!uploadDirectory || !uploadDirectory.isDirectory()) {
      if (uploadDirectory) uploadDirectory.close();
      state.error = "Upload folder does not exist";
      return;
    }
    uploadDirectory.close();

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s (socket timeout=%lu ms)", state.fileName.c_str(), state.path.c_str(),
            UPLOAD_SOCKET_TIMEOUT_MS);
#ifndef SIMULATOR
    if (!apMode) {
      LOG_DBG("WEB", "[UPLOAD] WiFi: RSSI=%d dBm channel=%d BSSID=%s", WiFi.RSSI(), WiFi.channel(),
              WiFi.BSSIDstr().c_str());
    }
#endif
    LOG_DBG("WEB", "[UPLOAD] Heap: free=%u maxalloc=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    resetTaskWatchdogIfSubscribed();
    if (Storage.exists(filePath.c_str())) {
      state.error = "File already exists: " + state.fileName;
      LOG_DBG("WEB", "[UPLOAD] Collision: %s", filePath.c_str());
      return;
    }

    state.stagingPath = hiddenBookFileSibling(filePath.c_str(), ".crossvi-upload.tmp").c_str();
    if (!clearStaleBookUploadStaging(state.stagingPath)) {
      state.error = "Could not clear interrupted upload";
      return;
    }

    // Open file for writing - this can be slow due to FAT cluster allocation
    resetTaskWatchdogIfSubscribed();
    if (!Storage.openFileForWrite("WEB", state.stagingPath, state.file)) {
      state.error = "Failed to create file on SD card";
      LOG_DBG("WEB", "[UPLOAD] FAILED to create file: %s", filePath.c_str());
      return;
    }
    state.ownsStagingFile = true;
    resetTaskWatchdogIfSubscribed();

    LOG_DBG("WEB", "[UPLOAD] Staging file created successfully: %s", state.stagingPath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    // The bundled UI uses multipart only to create a zero-byte file because an
    // empty raw request never reaches WebServer's raw-body callback. Every
    // upload with content must use the short, retryable cooperative requests.
    if (upload.currentSize > 0 && state.error.isEmpty()) {
      state.bufferPos = 0;
      if (state.file) state.file.close();
      if (state.ownsStagingFile && !state.stagingPath.isEmpty()) Storage.remove(state.stagingPath.c_str());
      state.ownsStagingFile = false;
      state.error = "Non-empty uploads require /api/upload/chunk";
      LOG_DBG("WEB", "[UPLOAD] Rejected legacy multipart data for: %s", state.fileName.c_str());
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.file) {
      // Flush any remaining buffered data
      bool synced = false;
      if (state.error.isEmpty()) {
        if (!flushUploadBuffer(state)) {
          state.error = "Failed to write final data to SD card";
        } else {
          synced = state.file.sync();
        }
      }
      const bool closed = state.file.close();
      const bool durable = synced && closed;

      if (state.error.isEmpty() && durable) {
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, write=%lu ms (%.1f%%), free=%u maxalloc=%u", writeCount,
                totalWriteTime, writePercent, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#endif

        String filePath = state.path;
        if (!filePath.endsWith("/")) filePath += "/";
        filePath += state.fileName;
        const BookFilePublishResult published = publishStagedBookFile(state.stagingPath.c_str(), filePath.c_str());
        if (published == BookFilePublishResult::Published || published == BookFilePublishResult::Unchanged) {
          state.ownsStagingFile = false;
          state.success = true;
          lastCompletePath = isSupportedReaderFile(filePath.c_str()) ? std::string(filePath.c_str()) : std::string{};
          lastCompleteName = state.fileName.c_str();
          lastCompleteSize = state.size;
          lastCompleteAt = millis();
        } else {
          state.error = published == BookFilePublishResult::InvalidStagedFile
                            ? "Uploaded book file is invalid"
                            : "Could not safely publish uploaded file";
        }
      } else if (state.error.isEmpty()) {
        state.error = "Could not safely store uploaded file";
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.file) state.file.close();
    if (state.ownsStagingFile && !state.stagingPath.isEmpty()) Storage.remove(state.stagingPath.c_str());
    state.ownsStagingFile = false;
    state.error = "Upload aborted";
    if (state.restoreModemSleep) WiFi.setSleep(true);
    state.restoreModemSleep = false;
    LOG_DBG("WEB", "Upload aborted at %zu bytes: free=%u maxalloc=%u", state.size, ESP.getFreeHeap(),
            ESP.getMaxAllocHeap());
  }
}

void CrossPointWebServer::handleInboxOpen() {
  if (upload.ownsStagingFile || cooperativeUpload.ownsStagingFile) {
    server->send(409, "text/plain", "An upload is still in progress");
    return;
  }
  auto body = takeJsonBody();
  if (!body) return;
  JsonDocument doc;
  const DeserializationError error = deserializeJson(doc, reinterpret_cast<const char*>(body.get()));
  const char* name = error ? nullptr : doc["name"].as<const char*>();
  if (!name || !UploadPathGuard::isSafeLeafName(name) || strlen(name) > 230 || !isSupportedReaderFile(name) ||
      isBookFileTransactionArtifact(name)) {
    server->send(400, "text/plain", "Invalid book name");
    return;
  }

  const std::string path = std::string("/Inbox/") + name;
  HalFile file = Storage.open(path.c_str());
  if (!file || file.isDirectory()) {
    if (file) file.close();
    server->send(404, "text/plain", "Book not found");
    return;
  }
  file.close();

  pendingOpenPath = path;
  server->send(202, "text/plain", "Book will open on the reader");
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  if (state.success) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    if (state.ownsStagingFile && !state.stagingPath.isEmpty()) Storage.remove(state.stagingPath.c_str());
    state.ownsStagingFile = false;
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
  state.success = false;
  state.error = "";
  state.fileName = "";
  state.size = 0;
  if (state.restoreModemSleep) WiFi.setSleep(true);
  state.restoreModemSleep = false;
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  String folderName = server->arg("name");
  folderName.trim();

  // Validate folder name
  if (!UploadPathGuard::isSafeLeafName(folderName.c_str())) {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }

  // Get parent path
  String parentPath;
  const String requestedParent = server->hasArg("path") ? server->arg("path") : String("/");
  if (!normalizeSafeWebPath(requestedParent, parentPath)) {
    server->send(403, "text/plain", "Cannot create folder in protected path");
    return;
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath;
  String newName = server->arg("name");
  newName.trim();

  if (!normalizeSafeWebPath(server->arg("path"), itemPath, false)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!UploadPathGuard::isSafeLeafName(newName.c_str())) {
    server->send(400, "text/plain", "Invalid file name");
    return;
  }
  if (isProtectedItemName(newName)) {
    server->send(403, "text/plain", "Cannot rename to protected name");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  file.close();
  const BookPathMoveResult move = moveBookFilePreservingUserState(itemPath.c_str(), newPath.c_str());

  if (move == BookPathMoveResult::Moved) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else if (move == BookPathMoveResult::StateUnavailable) {
    server->send(409, "text/plain", "Rename refused because book state could not be migrated safely");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath;
  String destPath;

  if (!normalizeSafeWebPath(server->arg("path"), itemPath, false)) {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (!normalizeSafeWebPath(server->arg("dest"), destPath)) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (isProtectedItemName(itemName)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (destPath != "/") {
    const String destName = destPath.substring(destPath.lastIndexOf('/') + 1);
    if (isProtectedItemName(destName)) {
      server->send(403, "text/plain", "Cannot move into protected folder");
      return;
    }
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  HalFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  file.close();
  const BookPathMoveResult move = moveBookFilePreservingUserState(itemPath.c_str(), newPath.c_str());

  if (move == BookPathMoveResult::Moved) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else if (move == BookPathMoveResult::StateUnavailable) {
    server->send(409, "text/plain", "Move refused because book state could not be migrated safely");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    if (pathsArg.length() > 8192) {
      server->send(413, "text/plain", "Delete request is too large");
      return;
    }
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    if (pathsArg.length() > 512) {
      server->send(413, "text/plain", "Delete path is too long");
      return;
    }
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }
  constexpr size_t maxDeleteItems = 64;
  if (paths.size() > maxDeleteItems) {
    server->send(413, "text/plain", "Too many items in one delete request");
    return;
  }

  // Keep the connection open and report one bounded operation at a time.  The
  // old implementation performed a whole batch without yielding, so a slow
  // SD card could starve the web task long enough to trip the watchdog.
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  const String start = String("START:") + String(paths.size()) + "\n";
  server->send(200, "text/plain", start);

  bool allSuccess = true;
  size_t failedCount = 0;
  String firstFailure;
  size_t index = 0;

  for (const auto& p : paths) {
    resetTaskWatchdogIfSubscribed();
    String itemPath;
    const String requestedPath = p.as<String>();
    bool success = false;
    String failure;

    // Validate path
    if (!normalizeSafeWebPath(requestedPath, itemPath, false)) {
      failure = requestedPath + " (invalid or protected path)";
    } else {
      // Security check: prevent deletion of protected items
      const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

      // Hidden/system files are protected
      if (itemName.startsWith(".")) {
        failure = itemPath + " (hidden/system file)";
      } else {
        // Check against explicitly protected items
        bool isProtected = false;
        for (const auto* item : HIDDEN_ITEMS) {
          if (itemName.equals(item)) {
            isProtected = true;
            break;
          }
        }
        if (isProtected) {
          failure = itemPath + " (protected file)";
        } else if (!Storage.exists(itemPath.c_str())) {
          failure = itemPath + " (not found)";
        } else {
          // Decide whether it's a directory or file by opening it
          HalFile f = Storage.open(itemPath.c_str());
          if (f && f.isDirectory()) {
            // For folders, ensure empty before removing
            HalFile entry = f.openNextFile();
            if (entry) {
              entry.close();
              f.close();
              failure = itemPath + " (folder not empty)";
            } else {
              f.close();
              resetTaskWatchdogIfSubscribed();
              success = Storage.rmdir(itemPath.c_str());
              resetTaskWatchdogIfSubscribed();
              if (!success) failure = itemPath + " (deletion failed)";
            }
          } else {
            // It's a file (or couldn't open as dir) — remove file
            if (f) f.close();
            if (!canDeleteOrRelocateBookFile(itemPath.c_str())) {
              failure = itemPath + " (book statistics recovery pending)";
            } else {
              resetTaskWatchdogIfSubscribed();
              success = Storage.remove(itemPath.c_str());
              resetTaskWatchdogIfSubscribed();
              if (success) {
                removeBookUserStateAfterDelete(itemPath.c_str());
                resetTaskWatchdogIfSubscribed();
              } else {
                failure = itemPath + " (deletion failed)";
              }
            }
          }
        }
      }
    }

    if (!success) {
      allSuccess = false;
      ++failedCount;
      if (firstFailure.isEmpty()) {
        firstFailure = failure.isEmpty() ? itemPath + " (deletion failed)" : failure;
      }
    }

    ++index;
    const String progress =
        String("PROGRESS:") + String(index) + ":" + String(paths.size()) + ":" + (success ? "OK\n" : "ERROR\n");
    resetTaskWatchdogIfSubscribed();
    server->sendContent(progress);
    delay(1);
  }

  const String result =
      String("DONE:") + (allSuccess ? "OK" : "ERROR") + ":" + String(failedCount) + ":" + firstFailure + "\n";
  resetTaskWatchdogIfSubscribed();
  server->sendContent(result);
  server->sendContent("");
}

void CrossPointWebServer::handleSettingsPage() const {
  sendHtmlContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml));
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::handleGetSettings() const {
  // The web UI can use a language different from the device language. Resolve
  // it locally so the render task never observes a temporary firmware language.
  Language responseLanguage = I18N.getLanguage();
  if (server->hasArg("lang")) {
    const String requestedLanguage = server->arg("lang");
    // Web UI uses lower-case BCP-47-style tags, while firmware settings use
    // upper-case ISO tags.  Do not pass the lower-case tag through
    // languageFromCode(), which would silently fall back to English.
    if (requestedLanguage == "vi") {
      responseLanguage = Language::VI;
    } else if (requestedLanguage == "en") {
      responseLanguage = Language::EN;
    }
  }

  // Pass the SD font registry so the fontFamily setting's enumStringValues
  // includes SD-resident families — otherwise the web API only exposes the
  // three built-in fonts.
  const auto& settings = getSettingsList(&sdFontSystem.registry(), nullptr, responseLanguage);

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key) continue;
    if (!display.supportsStripGrayscale() && s.valuePtr == &CrossPointSettings::textAntiAliasing) continue;

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId, responseLanguage);
    doc["category"] = I18N.get(s.category, responseLanguage);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt, responseLanguage));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        } else if (s.value16Ptr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.value16Ptr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
    yield();
    resetTaskWatchdogIfSubscribed();
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served settings API");
}

void CrossPointWebServer::handlePostSettings() {
  auto body = takeJsonBody();
  if (!body) return;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(body.get()));
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }
  if (!doc.is<JsonObjectConst>() || doc.as<JsonObjectConst>().size() == 0) {
    server->send(400, "text/plain", "Settings payload must be a non-empty JSON object");
    return;
  }

  const auto& settings = getSettingsList(&sdFontSystem.registry());
  int applied = 0;
  bool hasDeviceSettings = false;
  bool hasKoReaderSettings = false;

  const auto isKoReaderSetting = [](const SettingInfo& setting) {
    return setting.category == StrId::STR_KOREADER_SYNC;
  };

  const auto maximumStringBytes = [](const SettingInfo& setting) -> size_t {
    if (setting.stringMaxLen > 0) return setting.stringMaxLen - 1;
    if (!setting.key) return 0;
    if (std::strcmp(setting.key, "koUsername") == 0) return KOReaderCredentialStore::MAX_USERNAME_BYTES;
    if (std::strcmp(setting.key, "koPassword") == 0) return KOReaderCredentialStore::MAX_PASSWORD_BYTES;
    if (std::strcmp(setting.key, "koServerUrl") == 0) return KOReaderCredentialStore::MAX_SERVER_URL_BYTES;
    return 0;
  };

  const JsonObjectConst request = doc.as<JsonObjectConst>();
  for (const JsonPairConst entry : request) {
    const auto setting = std::find_if(settings.begin(), settings.end(), [&entry](const SettingInfo& candidate) {
      return candidate.key && std::strcmp(candidate.key, entry.key().c_str()) == 0;
    });
    if (setting == settings.end()) {
      server->send(400, "text/plain", "Unknown setting");
      return;
    }

    const JsonVariantConst value = entry.value();
    const auto reject = [this](const char* reason) { server->send(400, "text/plain", reason); };
    switch (setting->type) {
      case SettingType::TOGGLE:
        if (!value.is<int>() || !SettingsApiUtils::isValidToggle(value.as<int>())) {
          reject("Invalid toggle value for setting");
          return;
        }
        break;
      case SettingType::ENUM: {
        const size_t optionCount =
            setting->enumStringValues.empty() ? setting->enumValues.size() : setting->enumStringValues.size();
        if (!value.is<int>() || !SettingsApiUtils::isValidEnumIndex(value.as<int>(), optionCount)) {
          reject("Invalid enum index for setting");
          return;
        }
        break;
      }
      case SettingType::VALUE:
        if (!value.is<int>() ||
            !SettingsApiUtils::isValidValue(value.as<int>(), setting->valueRange.min, setting->valueRange.max)) {
          reject("Invalid numeric value for setting");
          return;
        }
        break;
      case SettingType::STRING: {
        if (!value.is<const char*>()) {
          reject("Invalid text value for setting");
          return;
        }
        const size_t maximum = maximumStringBytes(*setting);
        if (maximum == 0 || !SettingsApiUtils::isValidStringLength(std::strlen(value.as<const char*>()), maximum)) {
          reject("Text value is too long for setting");
          return;
        }
        break;
      }
      case SettingType::ACTION:
        reject("Setting cannot be changed through the web API");
        return;
    }

    if (isKoReaderSetting(*setting)) {
      hasKoReaderSettings = true;
    } else {
      hasDeviceSettings = true;
    }
  }

  if (hasDeviceSettings && !SETTINGS.isPersistenceWritable()) {
    server->send(503, "text/plain", "Device settings are read-only until their storage is recovered");
    return;
  }
  if (hasKoReaderSettings && (!KOREADER_STORE.ensureLoaded() || !KOREADER_STORE.isPersistenceWritable())) {
    server->send(503, "text/plain", "KOReader settings are read-only until their storage is recovered");
    return;
  }

  struct OriginalSettingValue {
    const SettingInfo* setting;
    uint16_t numericValue = 0;
    std::string stringValue;
  };
  struct DynamicDeviceState {
    uint8_t sleepScreen = SETTINGS.sleepScreen;
    uint8_t screenMargin = SETTINGS.screenMargin;
    uint8_t fontFamily = SETTINGS.fontFamily;
    uint8_t fontSize = SETTINGS.fontSize;
    uint8_t tiltPageTurn = SETTINGS.tiltPageTurn;
    std::array<char, CrossPointSettings::SD_FONT_FAMILY_NAME_CAPACITY> sdFontFamilyName{};

    DynamicDeviceState() { std::memcpy(sdFontFamilyName.data(), SETTINGS.sdFontFamilyName, sdFontFamilyName.size()); }

    void restore() const {
      SETTINGS.sleepScreen = sleepScreen;
      SETTINGS.screenMargin = screenMargin;
      SETTINGS.fontFamily = fontFamily;
      SETTINGS.fontSize = fontSize;
      SETTINGS.tiltPageTurn = tiltPageTurn;
      std::memcpy(SETTINGS.sdFontFamilyName, sdFontFamilyName.data(), sdFontFamilyName.size());
    }
  };
  const DynamicDeviceState originalDynamicDeviceState;
  std::vector<OriginalSettingValue> originalValues;
  originalValues.reserve(std::min(settings.size(), doc.as<JsonObjectConst>().size()));
  bool deviceChanged = false;
  bool koReaderChanged = false;

  const auto markChanged = [&isKoReaderSetting, &deviceChanged, &koReaderChanged](const SettingInfo& setting) {
    if (isKoReaderSetting(setting)) {
      koReaderChanged = true;
    } else {
      deviceChanged = true;
    }
  };

  for (const auto& s : settings) {
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          if (SETTINGS.*(s.valuePtr) == val) break;
          originalValues.push_back({&s, SETTINGS.*(s.valuePtr), {}});
          SETTINGS.*(s.valuePtr) = val;
          markChanged(s);
          applied++;
        }
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const size_t optionCount = s.enumStringValues.empty() ? s.enumValues.size() : s.enumStringValues.size();
        if (SettingsApiUtils::isValidEnumIndex(val, optionCount)) {
          if (s.valuePtr) {
            if (SETTINGS.*(s.valuePtr) == val) break;
            originalValues.push_back({&s, SETTINGS.*(s.valuePtr), {}});
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.valueSetter) {
            const uint8_t previous = s.valueGetter ? s.valueGetter() : uint8_t{0};
            if (previous == val) break;
            originalValues.push_back({&s, previous, {}});
            s.valueSetter(static_cast<uint8_t>(val));
          } else {
            break;
          }
          markChanged(s);
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            if (SETTINGS.*(s.valuePtr) == val) break;
            originalValues.push_back({&s, SETTINGS.*(s.valuePtr), {}});
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          } else if (s.value16Ptr) {
            if (SETTINGS.*(s.value16Ptr) == val) break;
            originalValues.push_back({&s, SETTINGS.*(s.value16Ptr), {}});
            SETTINGS.*(s.value16Ptr) = static_cast<uint16_t>(val);
          } else {
            break;
          }
          markChanged(s);
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (s.stringSetter) {
          const std::string previous = s.stringGetter ? s.stringGetter() : std::string{};
          if (previous == val) break;
          originalValues.push_back({&s, 0, previous});
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          if (val == ptr) break;
          originalValues.push_back({&s, 0, ptr});
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        } else {
          break;
        }
        markChanged(s);
        applied++;
        break;
      }
      default:
        break;
    }
  }

  const auto rollbackSettings = [&originalValues, &isKoReaderSetting,
                                 &originalDynamicDeviceState](const bool koReader) {
    for (auto original = originalValues.rbegin(); original != originalValues.rend(); ++original) {
      const SettingInfo& setting = *original->setting;
      if (isKoReaderSetting(setting) != koReader) continue;

      switch (setting.type) {
        case SettingType::TOGGLE:
        case SettingType::ENUM:
          if (setting.valuePtr) {
            SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(original->numericValue);
          } else if (setting.valueSetter && koReader) {
            setting.valueSetter(static_cast<uint8_t>(original->numericValue));
          }
          break;
        case SettingType::VALUE:
          if (setting.valuePtr) {
            SETTINGS.*(setting.valuePtr) = static_cast<uint8_t>(original->numericValue);
          } else if (setting.value16Ptr) {
            SETTINGS.*(setting.value16Ptr) = original->numericValue;
          }
          break;
        case SettingType::STRING:
          if (setting.stringSetter) {
            setting.stringSetter(original->stringValue);
          } else if (setting.stringMaxLen > 0) {
            char* destination = reinterpret_cast<char*>(&SETTINGS) + setting.stringOffset;
            strncpy(destination, original->stringValue.c_str(), setting.stringMaxLen - 1);
            destination[setting.stringMaxLen - 1] = '\0';
          }
          break;
        case SettingType::ACTION:
          break;
      }
    }
    if (!koReader) originalDynamicDeviceState.restore();
  };

  const auto persistenceResult = SettingsApiUtils::persistBatches(
      deviceChanged, koReaderChanged, [] { return SETTINGS.saveToFile(); }, [] { return KOREADER_STORE.saveToFile(); },
      [&rollbackSettings] { rollbackSettings(false); }, [&rollbackSettings] { rollbackSettings(true); });
  if (persistenceResult != SettingsApiUtils::PersistenceResult::Saved) {
    const bool partial = persistenceResult == SettingsApiUtils::PersistenceResult::KoReaderFailed && deviceChanged;
    LOG_ERR("WEB", "Failed to persist %s web settings; rolled back unsaved memory",
            persistenceResult == SettingsApiUtils::PersistenceResult::DeviceFailed ? "device" : "KOReader");
    server->send(500, "text/plain",
                 partial ? "Device settings saved, but KOReader settings failed"
                         : "Failed to save settings; no changes were kept");
    return;
  }

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;
  bool seenFirst = false;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (seenFirst) server->sendContent(",");
    server->sendContent(output);
    seenFirst = true;
    yield();
    resetTaskWatchdogIfSubscribed();
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served OPDS servers API (%zu servers)", servers.size());
}

void CrossPointWebServer::handlePostOpdsServer() {
  auto body = takeJsonBody();
  if (!body) return;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(body.get()));
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    // Preserve existing password if not explicitly provided
    if (!hasPasswordField) {
      const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
      if (existing) password = existing->password;
    }
    opdsServer.password = password;
    if (!OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer)) {
      server->send(400, "text/plain", "Failed to update OPDS server");
      return;
    }
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  auto body = takeJsonBody();
  if (!body) return;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(body.get()));
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  if (!OPDS_STORE.removeServer(static_cast<size_t>(idx))) {
    server->send(400, "text/plain", "Failed to delete OPDS server");
    return;
  }
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const auto credentials = WIFI_STORE.getCredentialSummaries();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;
  bool seenFirst = false;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = credentials[i].hasPassword;
    doc["isLastConnected"] = credentials[i].isLastConnected;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (seenFirst) server->sendContent(",");
    server->sendContent(output);
    seenFirst = true;
    yield();
    resetTaskWatchdogIfSubscribed();
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served Wi-Fi credentials API (%zu network(s))", credentials.size());
}

void CrossPointWebServer::handlePostWifiNetwork() {
  auto body = takeJsonBody();
  if (!body) return;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(body.get()));
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }
    const auto credential = WIFI_STORE.getCredentialAt(static_cast<size_t>(idx));
    if (!credential) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    if (!hasPasswordField) {
      password = credential->password;
    }

    if (!WIFI_STORE.updateCredential(static_cast<size_t>(idx), ssid, password)) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  auto body = takeJsonBody();
  if (!body) return;
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(body.get()));
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }
  const auto ssid = WIFI_STORE.getSsidAt(static_cast<size_t>(idx));
  if (!ssid) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  if (!WIFI_STORE.removeCredential(*ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid->c_str());
  server->send(200, "text/plain", "OK");
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  sendHtmlContent(server.get(), FontsPageHtml, sizeof(FontsPageHtml));
  LOG_DBG("WEB", "Served fonts page");
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  const_cast<SdCardFontSystem&>(sdFontSystem).refreshIfDirty();
  const auto& families = sdFontSystem.registry().getFamilies();

  JsonDocument doc;
  JsonArray arr = doc["families"].to<JsonArray>();
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;

  for (const auto& family : families) {
    JsonObject fObj = arr.add<JsonObject>();
    fObj["name"] = family.name;

    JsonArray sizes = fObj["sizes"].to<JsonArray>();
    for (uint8_t s : family.availableSizes()) {
      sizes.add(s);
    }

    JsonArray files = fObj["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      JsonObject fileObj = files.add<JsonObject>();
      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      fileObj["name"] = name ? name + 1 : file.path.c_str();

      // Stat the file for size
      HalFile f;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileObj["size"] = static_cast<unsigned long>(f.size());
        f.close();
      } else {
        fileObj["size"] = 0;
      }
    }
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleFontDelete() {
  auto body = takeJsonBody("application/json");
  if (!body) return;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, reinterpret_cast<const char*>(body.get()));

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  if (cooperativeUpload.ownsStagingFile && cooperativeUpload.kind == CooperativeUploadKind::Font &&
      cooperativeUpload.familyName.equalsIgnoreCase(familyName)) {
    server->send(409, "application/json", "{\"error\":\"Font upload in progress\"}");
    return;
  }
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}
