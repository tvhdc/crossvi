#include "CrossPointWebServerActivity.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <WiFi.h>

#include <atomic>
#include <cstddef>
#include <new>

#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "NetworkModeSelectionActivity.h"
#include "SilentRestart.h"
#include "WifiSelectionActivity.h"
#include "activities/network/CalibreConnectActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"
#include "util/TaskWatchdog.h"
#include "util/WifiLifecycle.h"

namespace {
// AP Mode configuration
constexpr const char* AP_SSID = "CrossVi";
constexpr const char* AP_PASSWORD = nullptr;  // Open network for ease of use
constexpr const char* AP_HOSTNAME = "crosspoint";
constexpr uint8_t AP_CHANNEL = 1;
constexpr uint8_t AP_MAX_CONNECTIONS = 4;
constexpr int QR_CODE_WIDTH = 198;
constexpr int QR_CODE_HEIGHT = 198;

#if !defined(SIMULATOR) && defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
std::atomic<uint32_t> wifiDisconnectSequence{0};
std::atomic<uint32_t> wifiDisconnectAt{0};
std::atomic<uint8_t> wifiDisconnectReason{0};
WiFiEventId_t wifiDisconnectEventId = 0;
uint32_t reportedWifiDisconnectSequence = 0;

void recordWifiDisconnect(WiFiEvent_t, WiFiEventInfo_t info) {
  wifiDisconnectReason.store(info.wifi_sta_disconnected.reason, std::memory_order_relaxed);
  wifiDisconnectAt.store(millis(), std::memory_order_relaxed);
  wifiDisconnectSequence.fetch_add(1, std::memory_order_release);
}
#endif

// DNS server for captive portal (redirects all DNS queries to our IP)
DNSServer* dnsServer = nullptr;
constexpr uint16_t DNS_PORT = 53;

void stopDnsServer() {
  if (!dnsServer) return;

  dnsServer->stop();
  delete dnsServer;
  dnsServer = nullptr;
}

void restartMdns(const char* hostname, const char* tag) {
  MDNS.end();
  if (MDNS.begin(hostname)) {
    LOG_DBG(tag, "mDNS started: http://%s.local/", hostname);
  } else {
    LOG_DBG(tag, "WARNING: mDNS failed to start");
  }
}

// 0..4 bars from RSSI (dBm), with 3 dBm hysteresis on currentBars to suppress flicker.
int barsForRssi(int rssi, int currentBars) {
  static constexpr int RISE_DBM[] = {-85, -75, -65, -55};
  static constexpr int FALL_DBM[] = {-88, -78, -68, -58};
  int bars = std::clamp(currentBars, 0, 4);
  while (bars < 4 && rssi >= RISE_DBM[bars]) bars++;
  while (bars > 0 && rssi < FALL_DBM[bars - 1]) bars--;
  return bars;
}
}  // namespace

void CrossPointWebServerActivity::onEnter() {
  Activity::onEnter();

  LOG_DBG("WEBACT", "Free heap at onEnter: %d bytes", ESP.getFreeHeap());

  // Reset state
  state = WebServerActivityState::MODE_SELECTION;
  networkMode = NetworkMode::JOIN_NETWORK;
  isApMode = false;
  connectedIP.clear();
  connectedSSID.clear();
  lastHandleClientTime = 0;
  lastReceivedName.clear();
  lastReceivedPath.clear();
  lastReceivedAt = 0;
  restartToReader = false;

#if !defined(SIMULATOR) && defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (wifiDisconnectEventId != 0) WiFi.removeEvent(wifiDisconnectEventId);
  reportedWifiDisconnectSequence = wifiDisconnectSequence.load(std::memory_order_acquire);
  wifiDisconnectEventId = WiFi.onEvent(recordWifiDisconnect, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#endif
  // Launch network mode selection subactivity
  LOG_DBG("WEBACT", "Launching NetworkModeSelectionActivity...");
  startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled) {
                             onGoHome();
                           } else {
                             onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                           }
                         });
}

void CrossPointWebServerActivity::onExit() {
  Activity::onExit();

  LOG_DBG("WEBACT", "Free heap at onExit start: %d bytes", ESP.getFreeHeap());

  state = WebServerActivityState::SHUTTING_DOWN;
#if !defined(SIMULATOR) && defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
  if (wifiDisconnectEventId != 0) {
    WiFi.removeEvent(wifiDisconnectEventId);
    wifiDisconnectEventId = 0;
  }
#endif
  webServer.reset();
  stopDnsServer();
  MDNS.end();

  // Opening a received book still uses the established restart-to-reader path.
  // Ordinary Back exits now tear Wi-Fi down in place and return immediately.
  if (restartToReader && WiFi.getMode() != WIFI_MODE_NULL) {
    if (isApMode) {
      WiFi.softAPdisconnect(true);
    } else {
      WiFi.disconnect(false);
    }
    delay(30);
    silentRestartToReader();
  } else if (!WifiLifecycle::shutDown()) {
    silentRestart();
  }

  LOG_DBG("WEBACT", "Free heap at onExit end: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServerActivity::onNetworkModeSelected(const NetworkMode mode) {
  const char* modeName = "Join Network";
  if (mode == NetworkMode::CONNECT_CALIBRE) {
    modeName = "Connect to Calibre";
  } else if (mode == NetworkMode::CREATE_HOTSPOT) {
    modeName = "Create Hotspot";
  }
  LOG_DBG("WEBACT", "Network mode selected: %s", modeName);

  networkMode = mode;
  isApMode = (mode == NetworkMode::CREATE_HOTSPOT);

  if (mode == NetworkMode::CONNECT_CALIBRE) {
    startActivityForResult(
        std::make_unique<CalibreConnectActivity>(renderer, mappedInput), [this](const ActivityResult& result) {
          state = WebServerActivityState::MODE_SELECTION;

          startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                                 [this](const ActivityResult& result) {
                                   if (result.isCancelled) {
                                     onGoHome();
                                   } else {
                                     onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                                   }
                                 });
        });
    return;
  }

  if (mode == NetworkMode::JOIN_NETWORK) {
    // STA mode - launch WiFi selection
    state = WebServerActivityState::WIFI_SELECTION;
    LOG_DBG("WEBACT", "Launching WifiSelectionActivity...");
    startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& wifi = std::get<WifiResult>(result.data);
                               connectedIP = wifi.ip;
                               connectedSSID = wifi.ssid;
                             }
                             onWifiSelectionComplete(!result.isCancelled);
                           });
  } else {
    // AP mode - start access point
    state = WebServerActivityState::AP_STARTING;
    requestUpdateAndWait();
    startAccessPoint();
  }
}

void CrossPointWebServerActivity::onWifiSelectionComplete(const bool connected) {
  LOG_DBG("WEBACT", "WifiSelectionActivity completed, connected=%d", connected);

  if (connected) {
    // Get connection info before exiting subactivity
    isApMode = false;

    // Start mDNS for hostname resolution
    restartMdns(AP_HOSTNAME, "WEBACT");

    // Start the web server
    startWebServer();
  } else {
    // User cancelled - go back to mode selection
    state = WebServerActivityState::MODE_SELECTION;

    startActivityForResult(std::make_unique<NetworkModeSelectionActivity>(renderer, mappedInput),
                           [this](const ActivityResult& result) {
                             if (result.isCancelled) {
                               onGoHome();
                             } else {
                               onNetworkModeSelected(std::get<NetworkModeResult>(result.data).mode);
                             }
                           });
  }
}

void CrossPointWebServerActivity::startAccessPoint() {
  LOG_DBG("WEBACT", "Starting Access Point mode...");
  LOG_DBG("WEBACT", "Free heap before AP start: %d bytes", ESP.getFreeHeap());

  // Configure and start the AP
  WiFi.mode(WIFI_AP);
  delay(100);

  // Start soft AP
  bool apStarted;
  if (AP_PASSWORD && strlen(AP_PASSWORD) >= 8) {
    apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  } else {
    // Open network (no password)
    apStarted = WiFi.softAP(AP_SSID, nullptr, AP_CHANNEL, false, AP_MAX_CONNECTIONS);
  }

  if (!apStarted) {
    LOG_ERR("WEBACT", "ERROR: Failed to start Access Point!");
    onGoHome();
    return;
  }

  delay(100);  // Wait for AP to fully initialize

  // Get AP IP address
  const IPAddress apIP = WiFi.softAPIP();
  char ipStr[16];
  snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d", apIP[0], apIP[1], apIP[2], apIP[3]);
  connectedIP = ipStr;
  connectedSSID = AP_SSID;

  LOG_DBG("WEBACT", "Access Point started!");
  LOG_DBG("WEBACT", "SSID: %s", AP_SSID);
  LOG_DBG("WEBACT", "IP: %s", connectedIP.c_str());

  // Start mDNS for hostname resolution
  restartMdns(AP_HOSTNAME, "WEBACT");

  // Start DNS server for captive portal behavior
  // This redirects all DNS queries to our IP, making any domain typed resolve to us
  stopDnsServer();
  dnsServer = new (std::nothrow) DNSServer();
  if (dnsServer) {
    dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer->start(DNS_PORT, "*", apIP);
    LOG_DBG("WEBACT", "DNS server started for captive portal");
  } else {
    // File transfer still works through the displayed IP address when the
    // optional captive-portal redirect cannot be allocated.
    LOG_ERR("WEBACT", "Out of memory starting captive-portal DNS");
  }

  LOG_DBG("WEBACT", "Free heap after AP start: %d bytes", ESP.getFreeHeap());

  // Start the web server
  startWebServer();
}

void CrossPointWebServerActivity::startWebServer() {
  LOG_DBG("WEBACT", "Starting web server...");

  // Keep rendering from repopulating the glyph cache during heap-critical server setup.
  RenderLock lock(*this);
  if (auto* cache = renderer.getFontCacheManager()) cache->clearCache();
  // Create the web server instance
  webServer.reset(new (std::nothrow) CrossPointWebServer());
  if (webServer) webServer->begin();
  lock.unlock();

  if (!webServer) {
    LOG_ERR("WEBACT", "Out of memory starting web server");
    onGoHome();
    return;
  }

  if (webServer->isRunning()) {
    state = WebServerActivityState::SERVER_RUNNING;
    LOG_DBG("WEBACT", "Web server started successfully");
    lastWifiBars = isApMode ? 0 : barsForRssi(WiFi.RSSI(), 0);
#ifndef SIMULATOR
    if (!isApMode) {
      LOG_DBG("WEBACT", "WiFi diagnostics: RSSI=%d dBm channel=%d BSSID=%s modemSleep=%d", WiFi.RSSI(), WiFi.channel(),
              WiFi.BSSIDstr().c_str(), static_cast<int>(WiFi.getSleep()));
    }
#endif

    // Force an immediate render since we're transitioning from a subactivity
    // that had its own rendering task. We need to make sure our display is shown.
    requestUpdate();
  } else {
    LOG_ERR("WEBACT", "ERROR: Failed to start web server!");
    webServer.reset();
    // Go back on error
    onGoHome();
  }
}

void CrossPointWebServerActivity::loop() {
  // Handle different states
  if (state == WebServerActivityState::SERVER_RUNNING) {
    // Consume the edge captured by main.cpp before the bounded HTTP burst.
    // The burst periodically polls input again for responsiveness, and that
    // update clears one-shot edges even when the button remains held.
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }
    bool confirmReleased = mappedInput.wasReleased(MappedInputManager::Button::Confirm);
#if !defined(SIMULATOR) && defined(ENABLE_SERIAL_LOG) && defined(LOG_LEVEL) && LOG_LEVEL >= 2
    const uint32_t disconnectSequence = wifiDisconnectSequence.load(std::memory_order_acquire);
    if (disconnectSequence != reportedWifiDisconnectSequence) {
      const uint8_t reason = wifiDisconnectReason.load(std::memory_order_relaxed);
      const uint32_t disconnectedAt = wifiDisconnectAt.load(std::memory_order_relaxed);
      LOG_DBG("WEBACT", "WiFi disconnected at %lu ms: reason=%u (%s), status=%d, RSSI=%d dBm, channel=%d",
              disconnectedAt, reason, WiFi.disconnectReasonName(static_cast<wifi_err_reason_t>(reason)), WiFi.status(),
              WiFi.RSSI(), WiFi.channel());
      reportedWifiDisconnectSequence = disconnectSequence;
    }
#endif
    // Handle DNS requests for captive portal (AP mode only)
    if (isApMode && dnsServer) {
      dnsServer->processNextRequest();
    }

    // STA mode: Monitor WiFi connection health
    if (!isApMode && webServer && webServer->isRunning()) {
      static unsigned long lastWifiCheck = 0;
      if (millis() - lastWifiCheck > 2000) {  // Check every 2 seconds
        lastWifiCheck = millis();
        const wl_status_t wifiStatus = WiFi.status();
        // Driver auto-reconnect handles retries; abandon (via onGoHome) only
        // after WIFI_ABANDON_MS, otherwise the activity freezes on a blip.
        bool repaint = false;
        if (wifiStatus != WL_CONNECTED) {
          if (consecutiveDisconnects == 0) {
            firstDisconnectAt = millis();
            repaint = true;
          }
          consecutiveDisconnects++;
          LOG_DBG("WEBACT", "WiFi not connected (status=%d, consecutive=%d, total=%lu ms)", wifiStatus,
                  consecutiveDisconnects, millis() - firstDisconnectAt);
          if (millis() - firstDisconnectAt > WIFI_ABANDON_MS) {
            LOG_DBG("WEBACT", "WiFi unavailable for >%lu s; returning to network selection", WIFI_ABANDON_MS / 1000UL);
            state = WebServerActivityState::SHUTTING_DOWN;
            onGoHome();
            return;
          }
        } else {
          if (consecutiveDisconnects > 0) {
            LOG_DBG("WEBACT", "WiFi recovered after %d failed checks (%lu ms)", consecutiveDisconnects,
                    millis() - firstDisconnectAt);
            repaint = true;
          }
          consecutiveDisconnects = 0;
          firstDisconnectAt = 0;
          const int rssi = WiFi.RSSI();
          if (rssi < -75) {
            LOG_DBG("WEBACT", "Warning: Weak WiFi signal: %d dBm", rssi);
          }
          const int bars = barsForRssi(rssi, lastWifiBars);
          if (bars != lastWifiBars) {
            lastWifiBars = bars;
            repaint = true;
          }
        }
        if (repaint) requestUpdate();
      }
    }

    // Handle web server requests - maximize throughput with watchdog safety
    if (webServer && webServer->isRunning()) {
      const unsigned long timeSinceLastHandleClient = millis() - lastHandleClientTime;

      // Log if there's a significant gap between handleClient calls (>100ms)
      if (lastHandleClientTime > 0 && timeSinceLastHandleClient > 100) {
        LOG_DBG("WEBACT", "WARNING: %lu ms gap since last handleClient", timeSinceLastHandleClient);
      }

      // Reset watchdog BEFORE processing - HTTP header parsing can be slow
      resetTaskWatchdogIfSubscribed();

      // A client that is only browsing (or an idle keep-alive) needs a few
      // iterations per main-loop cycle; the full burst is reserved for active
      // transfers. Modem sleep remains disabled for the lifetime of this
      // activity because it has caused real-device association drops.
      const bool transferActive = webServer->hasActiveTransfer();

      // Process HTTP requests in a bounded loop.
      // More iterations = more data processed per main loop cycle.
      constexpr int TRANSFER_ITERATIONS = 500;
      constexpr int IDLE_ITERATIONS = 12;
      const int maxIterations =
          webServer->hasCooperativeUpload() ? 1 : (transferActive ? TRANSFER_ITERATIONS : IDLE_ITERATIONS);
      for (int i = 0; i < maxIterations && webServer->isRunning(); i++) {
        webServer->handleClient();
        // The browser sends cooperative uploads as independent 64 KiB
        // requests. Return to the activity loop after each one so button
        // input and WiFi health checks cannot be starved by a queued burst.
        if (webServer->hasCooperativeUpload()) break;
        // Reset watchdog every 32 iterations
        if ((i & 0x1F) == 0x1F) {
          resetTaskWatchdogIfSubscribed();
        }
        // Yield and check for exit button every 64 iterations
        if ((i & 0x3F) == 0x3F) {
          yield();
          // Force trigger an update of which buttons are being pressed so be have accurate state
          // for back button checking
          mappedInput.update();
          // Check for exit button inside loop for responsiveness
          if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
            onGoHome();
            return;
          }
          confirmReleased = confirmReleased || mappedInput.wasReleased(MappedInputManager::Button::Confirm);
        }
      }
      lastHandleClientTime = millis();

      const auto uploadStatus = webServer->getUploadStatus();
      if (uploadStatus.lastCompleteAt != 0 && uploadStatus.lastCompleteAt != lastReceivedAt) {
        lastReceivedAt = uploadStatus.lastCompleteAt;
        lastReceivedName.assign(uploadStatus.lastCompleteName.data(), uploadStatus.lastCompleteName.size());
        lastReceivedPath.assign(uploadStatus.lastCompletePath.data(), uploadStatus.lastCompletePath.size());
        requestUpdate();
      }
      std::string openPath;
      if (webServer->takeOpenRequest(openPath)) {
        openReceivedBook(openPath);
        return;
      }
    }

    if (!lastReceivedPath.empty() && confirmReleased) {
      openReceivedBook(lastReceivedPath);
      return;
    }
  }
}

void CrossPointWebServerActivity::openReceivedBook(const std::string& path) {
  if (path.empty()) return;
  const std::string previousPath = APP_STATE.openEpubPath;
  APP_STATE.openEpubPath = path;
  if (!APP_STATE.saveToFile()) {
    APP_STATE.openEpubPath = previousPath;
    LOG_ERR("WEBACT", "Could not persist Inbox open request");
    showOpenError = true;
    requestUpdate();
    return;
  }
  restartToReader = true;
  onGoHome();
}

void CrossPointWebServerActivity::render(RenderLock&&) {
  // Only render our own UI when server is running
  // Subactivities handle their own rendering
  if (state == WebServerActivityState::SERVER_RUNNING || state == WebServerActivityState::AP_STARTING) {
    renderer.clearScreen();
    if (state == WebServerActivityState::SERVER_RUNNING) {
      renderServerRunning();
    } else {
      const auto& metrics = UITheme::getInstance().getMetrics();
      const auto pageWidth = renderer.getScreenWidth();
      const auto pageHeight = renderer.getScreenHeight();
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                     isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);
      const auto height = renderer.getLineHeight(UI_10_FONT_ID);
      const auto top = (pageHeight - height) / 2;
      renderer.drawCenteredText(UI_10_FONT_ID, top, tr(STR_STARTING_HOTSPOT));
    }

    if (showOpenError) {
      showOpenError = false;
      drawTransientPopup(StrId::STR_ERROR_GENERAL_FAILURE);
      return;
    }
    renderer.displayBuffer();
  }
}

void CrossPointWebServerActivity::renderServerRunning() const {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 isApMode ? tr(STR_HOTSPOT_MODE) : tr(STR_FILE_TRANSFER), nullptr);
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    connectedSSID.c_str());

  if (!isApMode) {
    renderWifiIndicator(metrics.topPadding + metrics.headerHeight);
  }

  int startY = metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing * 2;
  int height10 = renderer.getLineHeight(UI_10_FONT_ID);
  if (isApMode) {
    // AP mode display
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_CONNECT_WIFI_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for Wifi
    // follows spec at https://github.com/zxing/zxing/wiki/Barcode-Contents#wi-fi-network-config-android-ios-11
    const std::string wifiConfig = std::string("WIFI:T:nopass;S:") + connectedSSID + ";;";
    const Rect qrBoundsWifi(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsWifi, wifiConfig);

    // Show network name
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      connectedSSID.c_str());

    startY += QR_CODE_HEIGHT + 2 * metrics.verticalSpacing;

    // Show primary URL (hostname)
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, startY, tr(STR_OPEN_URL_HINT), true,
                      EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    std::string hostnameUrl = std::string("http://") + AP_HOSTNAME + ".local/";
    std::string ipUrl = tr(STR_OR_HTTP_PREFIX) + connectedIP + "/";

    // Show QR code for URL
    const Rect qrBoundsUrl(metrics.contentSidePadding, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBoundsUrl, hostnameUrl);

    // Show IP address as fallback
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 80,
                      hostnameUrl.c_str());
    renderer.drawText(SMALL_FONT_ID, metrics.contentSidePadding + QR_CODE_WIDTH + metrics.verticalSpacing, startY + 100,
                      ipUrl.c_str());
  } else {
    startY += metrics.verticalSpacing * 2;

    // STA mode display (original behavior)
    // std::string ipInfo = "IP Address: " + connectedIP;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_OPEN_URL_HINT), true, EpdFontFamily::BOLD);
    startY += height10;
    renderer.drawCenteredText(UI_10_FONT_ID, startY, tr(STR_SCAN_QR_HINT), true, EpdFontFamily::BOLD);
    startY += height10 + metrics.verticalSpacing * 2;

    // Show QR code for URL
    std::string webInfo = "http://" + connectedIP + "/";
    const Rect qrBounds((pageWidth - QR_CODE_WIDTH) / 2, startY, QR_CODE_WIDTH, QR_CODE_HEIGHT);
    QrUtils::drawQrCode(renderer, qrBounds, webInfo);
    startY += QR_CODE_HEIGHT + metrics.verticalSpacing * 2;

    // Show web server URL prominently
    renderer.drawCenteredText(UI_10_FONT_ID, startY, webInfo.c_str(), true);
    startY += height10 + 5;

    // Also show hostname URL
    std::string hostnameUrl = std::string(tr(STR_OR_HTTP_PREFIX)) + AP_HOSTNAME + ".local/";
    renderer.drawCenteredText(SMALL_FONT_ID, startY, hostnameUrl.c_str(), true);
  }

  if (!lastReceivedName.empty()) {
    const int footerY = renderer.getScreenHeight() - metrics.buttonHintsHeight - renderer.getLineHeight(SMALL_FONT_ID) -
                        metrics.verticalSpacing;
    const std::string received =
        renderer.truncatedText(SMALL_FONT_ID, (std::string(tr(STR_LAST_RECEIVED)) + ": " + lastReceivedName).c_str(),
                               pageWidth - metrics.contentSidePadding * 2, EpdFontFamily::REGULAR);
    renderer.drawCenteredText(SMALL_FONT_ID, footerY, received.c_str(), true);
  }
  const auto labels = mappedInput.mapLabels(tr(STR_EXIT), lastReceivedPath.empty() ? "" : tr(STR_OPEN_NOW), "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void CrossPointWebServerActivity::renderWifiIndicator(int subHeaderTop) const {
  constexpr int BAR_COUNT = 4;
  constexpr int BAR_WIDTH = 4;
  constexpr int BAR_GAP = 2;
  constexpr int ICON_HEIGHT = 14;
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int iconWidth = BAR_COUNT * BAR_WIDTH + (BAR_COUNT - 1) * BAR_GAP;
  const int iconRight = renderer.getScreenWidth() - metrics.contentSidePadding;
  const int iconLeft = iconRight - iconWidth;
  const int iconBottom = subHeaderTop + metrics.tabBarHeight - metrics.verticalSpacing;

  const bool wifiUp = (WiFi.status() == WL_CONNECTED) && (consecutiveDisconnects == 0);
  if (wifiUp) {
    for (int i = 0; i < BAR_COUNT; i++) {
      const int barHeight = (i + 1) * ICON_HEIGHT / BAR_COUNT;
      const int x = iconLeft + i * (BAR_WIDTH + BAR_GAP);
      const int y = iconBottom - barHeight;
      if (i < lastWifiBars) {
        renderer.fillRect(x, y, BAR_WIDTH, barHeight, true);
      } else {
        renderer.drawRect(x, y, BAR_WIDTH, barHeight, true);
      }
    }
  } else {
    const int xSize = ICON_HEIGHT;
    const int x0 = iconRight - xSize;
    const int y0 = iconBottom - xSize;
    renderer.drawLine(x0, y0, x0 + xSize, y0 + xSize, 2, true);
    renderer.drawLine(x0, y0 + xSize, x0 + xSize, y0, 2, true);
  }
}
