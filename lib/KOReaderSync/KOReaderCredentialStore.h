#pragma once
#include <ArduinoJson.h>
#include <LazyStoreState.h>
#include <PersistableStore.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Document matching method for KOReader sync
enum class DocumentMatchMethod : uint8_t {
  FILENAME = 0,  // Match by filename (simpler, works across different file sources)
  BINARY = 1,    // Match by partial MD5 of file content (more accurate, but files must be identical)
};

// How manual "Sync Progress" resolves differences after fetching remote progress.
enum class KOReaderSyncBehavior : uint8_t {
  ASK_EVERY_TIME = 0,  // Preserve legacy behavior: always show Apply/Upload choices.
  SMART = 1,           // Auto-resolve simple cases using furthest progress.
};

/**
 * Singleton class for storing KOReader sync credentials on the SD card.
 * Passwords are XOR-obfuscated with the device's unique hardware MAC address
 * and base64-encoded before writing to JSON (not cryptographically secure,
 * but prevents casual reading and ties credentials to the specific device).
 */

class KOReaderCredentialStore : public PersistableStore<KOReaderCredentialStore> {
 private:
  std::string username;
  std::string password;
  std::string serverUrl;  // Custom sync server URL (empty = default)
  std::vector<std::string> customServers;
  DocumentMatchMethod matchMethod = DocumentMatchMethod::FILENAME;  // Default to filename for compatibility
  bool sendMetadata = false;                                        // Send document metadata with progress sync
  KOReaderSyncBehavior syncBehavior = KOReaderSyncBehavior::ASK_EVERY_TIME;
  LazyStoreState loadState;

  void rememberSelectedCustomServer();

  // Private constructor for singleton
  KOReaderCredentialStore() = default;
  ~KOReaderCredentialStore() = default;

  friend class PersistableStore<KOReaderCredentialStore>;

 public:
  static constexpr size_t MAX_USERNAME_BYTES = 64;
  static constexpr size_t MAX_PASSWORD_BYTES = 64;
  static constexpr size_t MAX_SERVER_URL_BYTES = 128;
  static constexpr size_t MAX_CUSTOM_SERVERS = 6;

  static constexpr const char* crossPointServerUrl() { return "https://sync.crosspointreader.com"; }
  static constexpr const char* koSyncServerUrl() { return "https://kosync.eu"; }

  static const char* getFilePath() { return "/.crosspoint/koreader.json"; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);
  bool loadFromFile();
  bool ensureLoaded();
  bool saveToFile() const;
  void markReadOnlyForRecovery();

  // Credential management
  void setCredentials(const std::string& user, const std::string& pass);
  const std::string& getUsername() const {
    const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
    return username;
  }
  const std::string& getPassword() const {
    const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
    return password;
  }

  // Get MD5 hash of password for API authentication
  std::string getMd5Password() const;

  // Check if credentials are set
  bool hasCredentials() const;

  // Clear credentials
  void clearCredentials();

  // Server URL management
  void setServerUrl(const std::string& url);
  const std::string& getServerUrl() const {
    const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
    return serverUrl;
  }

  // Get base URL for API calls (with http:// normalization if no protocol, falls back to default)
  std::string getBaseUrl() const;

  // Server presets are immutable; only entries in this bounded list can be edited or deleted.
  const std::vector<std::string>& getCustomServers() const;
  bool selectServerUrl(const std::string& url);
  bool addCustomServer(const std::string& url);
  bool updateCustomServer(size_t index, const std::string& url);
  bool removeCustomServer(size_t index);
  static bool isBuiltInServerUrl(const std::string& url);

  // Whether the configured endpoint supports CrossPoint-only protocol fields.
  bool usesCrossPointSyncServer() const;

  // Document matching method
  void setMatchMethod(DocumentMatchMethod method);
  DocumentMatchMethod getMatchMethod() const {
    const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
    return matchMethod;
  }

  // Send metadata setting
  void setSendMetadata(bool enabled);
  bool getSendMetadata() const {
    const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
    return sendMetadata;
  }

  // Sync behavior
  void setSyncBehavior(KOReaderSyncBehavior behavior);
  KOReaderSyncBehavior getSyncBehavior() const {
    const_cast<KOReaderCredentialStore*>(this)->ensureLoaded();
    return syncBehavior;
  }
};

// Helper macro to access credential store
#define KOREADER_STORE KOReaderCredentialStore::getInstance()
