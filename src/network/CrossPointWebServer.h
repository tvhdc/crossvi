#pragma once

#include <HalStorage.h>
#include <NetworkUdp.h>
#include <StagedFileTransaction.h>
#include <WebServer.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>

#include "network/JsonBodyBuffer.h"

// Structure to hold file information
struct FileInfo {
  String name;
  size_t size;
  bool isEpub;
  bool isDirectory;
};

class CrossPointWebServer {
 public:
  struct UploadStatus {
    bool inProgress = false;
    size_t received = 0;
    size_t total = 0;
    std::string_view filename;
    std::string_view lastCompleteName;
    std::string_view lastCompletePath;
    size_t lastCompleteSize = 0;
    unsigned long lastCompleteAt = 0;
  };

  // Used by POST upload handler
  struct UploadState {
    HalFile file;
    String fileName;
    String path = "/";
    String stagingPath;
    size_t size = 0;
    bool ownsStagingFile = false;
    bool success = false;
    bool restoreModemSleep = false;
    String error = "";

    size_t bufferPos = 0;
  } upload;

  CrossPointWebServer();
  ~CrossPointWebServer();

  // Start the web server (call after WiFi is connected)
  void begin();

  // Stop the web server
  void stop();

  // Call this periodically to handle client requests
  void handleClient();

  // Check if server is running
  bool isRunning() const { return running; }
  bool hasActiveTransfer() const;
  bool hasCooperativeUpload() const { return cooperativeUpload.ownsStagingFile; }

  UploadStatus getUploadStatus() const;
  bool takeOpenRequest(std::string& path);

  // Get the port number
  uint16_t getPort() const { return port; }

 private:
  std::unique_ptr<WebServer> server = nullptr;
  bool running = false;
  bool apMode = false;  // true when running in AP mode, false for STA mode
  uint16_t port = 80;
  NetworkUDP udp;
  bool udpActive = false;
  std::string lastCompleteName;
  std::string lastCompletePath;
  size_t lastCompleteSize = 0;
  unsigned long lastCompleteAt = 0;
  std::string pendingOpenPath;

  // The HTTP server is single-threaded and only one upload is accepted at a
  // time, so books and fonts can safely share one fixed staging buffer.
  static constexpr size_t TRANSFER_BUFFER_SIZE = 4096;
  static constexpr size_t COOPERATIVE_UPLOAD_CHUNK_SIZE = 64U * 1024U;
  std::array<uint8_t, TRANSFER_BUFFER_SIZE> transferBuffer{};

  enum class CooperativeUploadKind : uint8_t { None, Book, Font };

  struct CooperativeUploadState {
    HalFile file;
    String fileName;
    String path = "/";
    String familyName;
    String finalPath;
    String stagingPath;
    String backupPath;
    String error;
    StagedFileTransaction::Digest streamDigest;
    StagedFileTransaction::Digest requestDigestStart;
    size_t committed = 0;
    size_t total = 0;
    size_t requestSize = 0;
    size_t requestReceived = 0;
    size_t bufferPos = 0;
#if defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    size_t lastLoggedSize = 0;
#endif
    int responseStatus = 400;
    bool ownsStagingFile = false;
    bool requestAccepted = false;
    bool requestComplete = false;
    bool requestReplay = false;
    bool uploadComplete = false;
    CooperativeUploadKind kind = CooperativeUploadKind::None;
  } cooperativeUpload;

  static constexpr size_t MAX_JSON_BODY_SIZE = 8192;
  JsonBodyBuffer::State jsonBody;

  bool clearStaleBookUploadStaging(const String& stagingPath);
  bool flushUploadBuffer(UploadState& state);
  bool flushCooperativeUploadBuffer();
  void discardCooperativeUpload(bool removeStaging);

  // File scanning
  using FileVisitor = void (*)(const FileInfo& info, void* context);
  void scanFiles(const char* path, FileVisitor visitor, void* context) const;
  bool isEpubFile(const String& filename) const;

  // Request handlers
  void handleRoot() const;
  void handleJszip() const;
  void handleNotFound() const;
  void handleStatus() const;
  void handleFileList() const;
  void handleFileListData() const;
  void handleDownload() const;
  void handleUpload(UploadState& state);
  void handleUploadPost(UploadState& state) const;
  void handleCooperativeUploadData();
  void handleCooperativeUploadPost();
  void handleCooperativeUploadCancel();
  void handleInboxOpen();
  void handleCreateFolder() const;
  void handleRename() const;
  void handleMove() const;
  void handleDelete() const;
  void handleJsonBody();
  std::unique_ptr<uint8_t[]> takeJsonBody(const char* errorContentType = "text/plain");

  // Settings handlers
  void handleSettingsPage() const;
  void handleGetSettings() const;
  void handlePostSettings();

  // Font management handlers
  void handleFontsPage() const;
  void handleFontList() const;
  void handleFontDelete();

  // OPDS server handlers
  void handleGetOpdsServers() const;
  void handlePostOpdsServer();
  void handleDeleteOpdsServer();

  // Wi-Fi credential handlers
  void handleGetWifiNetworks() const;
  void handlePostWifiNetwork();
  void handleDeleteWifiNetwork();
};
