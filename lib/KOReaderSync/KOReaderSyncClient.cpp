#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <HttpTransportPolicy.h>
#include <Logging.h>
#include <MemoryBudget.h>
#include <SecureHttpClient.h>
#if defined(ENABLE_SERIAL_LOG)
#include <WiFi.h>
#endif
#include <base64.h>

#include <array>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>

#include "KOReaderCredentialStore.h"

extern "C" void wolfSSL_Arduino_Serial_Print(const char* const message) { LOG_DBG("WOLFSSL", "%s", message); }

int KOReaderSyncClient::lastHttpCode = 0;

namespace {
// CrossVi display name; retain the established device ID for sync compatibility.
constexpr char DEVICE_NAME[] = "CrossVi";
constexpr char DEVICE_ID[] = "crosspoint-reader";

constexpr size_t MAX_RESPONSE_BYTES = 64 * 1024;
constexpr size_t MAX_REQUEST_BYTES = 64 * 1024;
constexpr int HTTP_TIMEOUT_MS = 60000;

enum class RequestMethod { GET, POST, PUT };

struct RequestHeader {
  const char* name;
  const char* value;
};

struct ResponseAccumulator {
  std::string* response;
  bool overflow = false;
};

const char* methodName(const RequestMethod method) {
  switch (method) {
    case RequestMethod::POST:
      return "POST";
    case RequestMethod::PUT:
      return "PUT";
    case RequestMethod::GET:
    default:
      return "GET";
  }
}

// True when free heap is too low to risk a TLS handshake.
bool insufficientHeap() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAllocHeap = ESP.getMaxAllocHeap();
  if (!MemoryBudget::hasHeadroom(freeHeap, maxAllocHeap, MemoryBudget::KOREADER_TLS)) {
    LOG_ERR("KOSync", "Insufficient heap for TLS handshake: %u bytes free (need %u), %u max alloc (need %u)", freeHeap,
            MemoryBudget::KOREADER_TLS.freeHeap, maxAllocHeap, MemoryBudget::KOREADER_TLS.maxAllocHeap);
    return true;
  }
  return false;
}

bool validHeader(const RequestHeader& header) {
  if (!header.name || !*header.name || !header.value) return false;
  return std::string_view(header.name).find_first_of("\r\n:") == std::string_view::npos &&
         std::string_view(header.value).find_first_of("\r\n") == std::string_view::npos;
}

bool verifiedRequest(const RequestMethod method, const std::string& url, const std::string_view body,
                     const RequestHeader* headers, const size_t headerCount, std::string& response, int& httpStatus) {
  response.clear();
  httpStatus = 0;
  if (!HttpTransportPolicy::credentialsAllowed(url) || body.size() > MAX_REQUEST_BYTES ||
      (headerCount > 0 && !headers)) {
    LOG_ERR("KOSync", "Rejected URL or request size");
    return false;
  }
  for (size_t i = 0; i < headerCount; ++i) {
    if (!validHeader(headers[i])) {
      LOG_ERR("KOSync", "Rejected invalid request header");
      return false;
    }
  }

#if defined(ENABLE_SERIAL_LOG)
  LOG_DBG("KOSync", "HTTP start: %s %s, wifi=%d, rssi=%d, epoch=%lld, heap=%u, max_alloc=%u, body=%u",
          methodName(method), url.c_str(), static_cast<int>(WiFi.status()), WiFi.RSSI(),
          static_cast<long long>(time(nullptr)), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(body.size()));
#endif

  freeink::SecureHttpClient client;
  client.setTimeout(HTTP_TIMEOUT_MS);
  client.setReuse(false);
  // SecureNet currently has no CA bundle. This is the same wolfSSL transport
  // used by upstream CrossPoint for KOSync: it avoids mbedTLS's certificate
  // verification OOM/signature failure on ESP32-C3 while keeping traffic
  // encrypted. HttpTransportPolicy still rejects credential-bearing remote
  // requests unless the URL uses HTTPS.
  client.setInsecure();
  if (!client.begin(url)) {
    LOG_ERR("KOSync", "SecureHttpClient begin failed: %s", url.c_str());
    return false;
  }

  ResponseAccumulator accumulator{&response};
  for (size_t i = 0; i < headerCount; ++i) {
    client.addHeader(headers[i].name, headers[i].value);
  }

  const int status = client.sendRequest(
      methodName(method), reinterpret_cast<const uint8_t*>(body.data()), body.size(),
      [&accumulator](const uint8_t* data, const size_t length) {
        if (!accumulator.response || length > MAX_RESPONSE_BYTES - accumulator.response->size()) {
          accumulator.overflow = true;
          return false;
        }
        accumulator.response->append(reinterpret_cast<const char*>(data), length);
        return true;
      });
  httpStatus = status;
  const size_t declaredLength = client.hasContentLength() ? client.getContentLength() : 0;
  const int contentLength =
      declaredLength <= static_cast<size_t>(std::numeric_limits<int>::max()) ? static_cast<int>(declaredLength) : -1;
  const bool valid = status > 0 && !accumulator.overflow && client.responseComplete() &&
                     !(status >= 300 && status < 400) &&
                     (!client.hasContentLength() || declaredLength <= MAX_RESPONSE_BYTES) &&
                     (!client.hasContentLength() || response.size() == declaredLength);
#if defined(ENABLE_SERIAL_LOG)
  LOG_DBG("KOSync",
          "HTTP done: status=%d, complete=%d, declared=%d, received=%u, overflow=%d, heap=%u, max_alloc=%u",
          httpStatus, client.responseComplete() ? 1 : 0, contentLength, static_cast<unsigned>(response.size()),
          accumulator.overflow ? 1 : 0, static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));
#endif
  if (!valid) {
    LOG_ERR("KOSync", "Request rejected: status=%d, complete=%d, declared=%d, received=%u, overflow=%d", httpStatus,
            client.responseComplete() ? 1 : 0, contentLength, static_cast<unsigned>(response.size()),
            accumulator.overflow ? 1 : 0);
  }
  client.end();
  return valid;
}

bool performRequest(const RequestMethod method, const std::string& url, const std::string_view body,
                    const bool authenticated, std::string& response, int& httpStatus) {
  const std::string accept = "application/vnd.koreader.v1+json";
  const std::string user = KOREADER_STORE.getUsername();
  const std::string authKey = KOREADER_STORE.getMd5Password();
  const std::string credentials = user + ":" + KOREADER_STORE.getPassword();
  const std::string authorization = "Basic " + std::string(base64::encode(credentials.c_str()).c_str());
  const std::array<RequestHeader, 5> allHeaders = {
      RequestHeader{"Accept", accept.c_str()},           RequestHeader{"x-auth-user", user.c_str()},
      RequestHeader{"x-auth-key", authKey.c_str()},      RequestHeader{"Authorization", authorization.c_str()},
      RequestHeader{"Content-Type", "application/json"},
  };
  const size_t headerCount = authenticated ? (body.empty() ? 4 : 5) : (body.empty() ? 1 : 2);
  if (!authenticated && !body.empty()) {
    const std::array<RequestHeader, 2> publicHeaders = {
        RequestHeader{"Accept", accept.c_str()},
        RequestHeader{"Content-Type", "application/json"},
    };
    return verifiedRequest(method, url, body, publicHeaders.data(), publicHeaders.size(), response, httpStatus);
  }
  return verifiedRequest(method, url, body, allHeaders.data(), headerCount, response, httpStatus);
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  std::string response;
  int httpCode = 0;
  if (!performRequest(RequestMethod::GET, url, {}, true, response, httpCode)) {
    return NETWORK_ERROR;
  }
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Auth response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 200) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::createUser() {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/users/create";
  LOG_DBG("KOSync", "Creating account: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  JsonDocument doc;
  doc["username"] = KOREADER_STORE.getUsername();
  doc["password"] = KOREADER_STORE.getMd5Password();
  std::string body;
  serializeJson(doc, body);

  std::string response;
  int httpCode = 0;
  if (!performRequest(RequestMethod::POST, url, body, false, response, httpCode)) {
    return NETWORK_ERROR;
  }
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Create user response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 201) return OK;
  if (httpCode == 402) return USER_EXISTS;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  std::string response;
  int httpCode = 0;
  if (!performRequest(RequestMethod::GET, url, {}, true, response, httpCode)) {
    return NETWORK_ERROR;
  }
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode <= 0) {
    return NETWORK_ERROR;
  }

  if (httpCode == 200) {
    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, response.c_str());

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    outProgress.position.reset();
    if (KOREADER_STORE.usesCrossPointSyncServer()) {
      const JsonObjectConst pos = doc["position"].as<JsonObjectConst>();
      if (!pos.isNull()) {
        KOReaderRichPosition rich;
        rich.pctQ = pos["pctQ"].as<uint32_t>();
        rich.spineIndex = pos["spine"].as<uint16_t>();
        rich.pageNumber = pos["page"].as<uint16_t>();
        const uint16_t pages = pos["pages"].as<uint16_t>();
        rich.totalPages = pages > 0 ? pages : 1;
        const uint16_t para = pos["para"].as<uint16_t>();
        if (para > 0) rich.paragraphIndex = para;
        rich.xpath = pos["xpath"].as<const char*>() ? pos["xpath"].as<const char*>() : "";
        LOG_DBG("KOSync", "Got rich position: spine=%u page=%u/%u para=%u", rich.spineIndex, rich.pageNumber,
                rich.totalPages, para);
        outProgress.position = std::move(rich);
      }
    }

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  if (httpCode == 401) return AUTH_FAILED;
  if (httpCode == 404) return NOT_FOUND;
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  lastHttpCode = 0;
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  const std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s (heap: %u)", url.c_str(), (unsigned)ESP.getFreeHeap());
  if (insufficientHeap()) return LOW_MEMORY;

  // Build JSON body
  JsonDocument doc;
  doc["document"] = progress.document;
  if (progress.metadata.has_value()) {
    auto meta = doc["metadata"].to<JsonObject>();
    meta["filename"] = progress.metadata->filename;
    meta["title"] = progress.metadata->title;
    meta["authors"] = progress.metadata->authors;
  }
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;
  if (progress.position.has_value() && KOREADER_STORE.usesCrossPointSyncServer()) {
    // CrossPoint-specific extension: do not send it to third-party KOSync servers.
    const auto& p = *progress.position;
    auto pos = doc["position"].to<JsonObject>();
    pos["pctQ"] = p.pctQ;
    pos["spine"] = p.spineIndex;
    pos["page"] = p.pageNumber;
    pos["pages"] = p.totalPages;
    if (p.paragraphIndex.has_value()) pos["para"] = *p.paragraphIndex;
    // Server rejects the whole position object if xpath exceeds 120 bytes.
    if (!p.xpath.empty() && p.xpath.size() <= 120) pos["xpath"] = p.xpath;
  }

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  std::string response;
  int httpCode = 0;
  if (!performRequest(RequestMethod::PUT, url, body, true, response, httpCode)) {
    return NETWORK_ERROR;
  }
  lastHttpCode = httpCode;

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode <= 0) return NETWORK_ERROR;
  if (httpCode == 200 || httpCode == 202) return OK;
  if (httpCode == 401) return AUTH_FAILED;
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    case LOW_MEMORY:
      return "Not enough memory for sync — please retry";
    default:
      return "Unknown error";
  }
}
