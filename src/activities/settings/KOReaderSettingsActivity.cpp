#include "KOReaderSettingsActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <cstring>

#include "KOReaderAuthActivity.h"
#include "KOReaderCredentialStore.h"
#include "MappedInputManager.h"
#include "activities/util/ConfirmationActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 8;
const StrId menuNames[MENU_ITEMS] = {StrId::STR_USERNAME,          StrId::STR_PASSWORD,      StrId::STR_SYNC_SERVER_URL,
                                     StrId::STR_DOCUMENT_MATCHING, StrId::STR_SEND_METADATA, StrId::STR_SYNC_BEHAVIOR,
                                     StrId::STR_SIGN_UP,           StrId::STR_AUTHENTICATE};
}  // namespace

void KOReaderSettingsActivity::onEnter() {
  Activity::onEnter();

  selectedIndex = 0;
  requestUpdate();
}

void KOReaderSettingsActivity::loop() {
  if (optionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return;

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  // Handle navigation
  buttonNavigator.onNext([this] {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    requestUpdate();
  });

  buttonNavigator.onPrevious([this] {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    requestUpdate();
  });
}

void KOReaderSettingsActivity::handleSelection() {
  if (selectedIndex == 0) {
    // Username
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                               renderer, mappedInput, tr(STR_KOREADER_USERNAME), KOREADER_STORE.getUsername(),
                               KOReaderCredentialStore::MAX_USERNAME_BYTES, InputType::Identifier),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string previousUsername = KOREADER_STORE.getUsername();
                               if (kb.text == previousUsername) return;
                               const std::string password = KOREADER_STORE.getPassword();
                               KOREADER_STORE.setCredentials(kb.text, password);
                               const bool saved = KOREADER_STORE.saveToFile();
                               if (!saved) KOREADER_STORE.setCredentials(previousUsername, password);
                               showSaveError = !saved;
                             }
                           });
  } else if (selectedIndex == 1) {
    // Password
    startActivityForResult(std::make_unique<KeyboardEntryActivity>(
                               renderer, mappedInput, tr(STR_KOREADER_PASSWORD), KOREADER_STORE.getPassword(),
                               KOReaderCredentialStore::MAX_PASSWORD_BYTES, InputType::Password),
                           [this](const ActivityResult& result) {
                             if (!result.isCancelled) {
                               const auto& kb = std::get<KeyboardResult>(result.data);
                               const std::string username = KOREADER_STORE.getUsername();
                               const std::string previousPassword = KOREADER_STORE.getPassword();
                               if (kb.text == previousPassword) return;
                               KOREADER_STORE.setCredentials(username, kb.text);
                               const bool saved = KOREADER_STORE.saveToFile();
                               if (!saved) KOREADER_STORE.setCredentials(username, previousPassword);
                               showSaveError = !saved;
                             }
                           });
  } else if (selectedIndex == 2) {
    showServerPicker();
  } else if (selectedIndex == 3) {
    // Document Matching - toggle between Filename and Binary
    const auto current = KOREADER_STORE.getMatchMethod();
    const auto newMethod =
        (current == DocumentMatchMethod::FILENAME) ? DocumentMatchMethod::BINARY : DocumentMatchMethod::FILENAME;
    KOREADER_STORE.setMatchMethod(newMethod);
    const bool saved = KOREADER_STORE.saveToFile();
    if (!saved) KOREADER_STORE.setMatchMethod(current);
    reportSaveResult(saved);
  } else if (selectedIndex == 4) {
    // Send Metadata - toggle on/off
    const bool current = KOREADER_STORE.getSendMetadata();
    KOREADER_STORE.setSendMetadata(!current);
    const bool saved = KOREADER_STORE.saveToFile();
    if (!saved) KOREADER_STORE.setSendMetadata(current);
    reportSaveResult(saved);
  } else if (selectedIndex == 5) {
    // Sync behavior - toggle between Ask and Smart
    const auto current = KOREADER_STORE.getSyncBehavior();
    const auto newBehavior = (current == KOReaderSyncBehavior::ASK_EVERY_TIME) ? KOReaderSyncBehavior::SMART
                                                                               : KOReaderSyncBehavior::ASK_EVERY_TIME;
    KOREADER_STORE.setSyncBehavior(newBehavior);
    const bool saved = KOREADER_STORE.saveToFile();
    if (!saved) KOREADER_STORE.setSyncBehavior(current);
    reportSaveResult(saved);
  } else if (selectedIndex == 6) {
    // Sign Up - create a new account on the sync server with the entered credentials
    if (!KOREADER_STORE.hasCredentials()) {
      return;
    }
    startActivityForResult(
        std::make_unique<KOReaderAuthActivity>(renderer, mappedInput, KOReaderAuthActivity::Mode::SIGN_UP),
        [](const ActivityResult&) {});
  } else if (selectedIndex == 7) {
    // Authenticate
    if (!KOREADER_STORE.hasCredentials()) {
      // Can't authenticate without credentials - just show message briefly
      return;
    }
    startActivityForResult(std::make_unique<KOReaderAuthActivity>(renderer, mappedInput), [](const ActivityResult&) {});
  }
}

void KOReaderSettingsActivity::reportSaveResult(const bool saved) {
  showSaveError = !saved;
  requestUpdate();
}

void KOReaderSettingsActivity::showServerPicker() {
  std::vector<std::string> servers = {KOReaderCredentialStore::crossPointServerUrl(),
                                      KOReaderCredentialStore::koSyncServerUrl()};
  const auto& customServers = KOREADER_STORE.getCustomServers();
  servers.insert(servers.end(), customServers.begin(), customServers.end());
  servers.push_back(tr(STR_ADD_SERVER));

  const std::string current = KOREADER_STORE.getBaseUrl();
  int currentIndex = 0;
  for (size_t i = 0; i + 1 < servers.size(); ++i) {
    std::string normalized = servers[i];
    if (normalized.find("://") == std::string::npos) normalized.insert(0, "http://");
    while (!normalized.empty() && normalized.back() == '/') normalized.pop_back();
    if (normalized == current) {
      currentIndex = static_cast<int>(i);
      break;
    }
  }

  const size_t customCount = customServers.size();
  optionPopup.show(StrId::STR_SYNC_SERVER_URL, std::move(servers), currentIndex, [this, customCount](const int index) {
    if (index == 0) {
      reportSaveResult(KOREADER_STORE.selectServerUrl(KOReaderCredentialStore::crossPointServerUrl()));
    } else if (index == 1) {
      reportSaveResult(KOREADER_STORE.selectServerUrl(KOReaderCredentialStore::koSyncServerUrl()));
    } else if (index >= 2 && static_cast<size_t>(index - 2) < customCount) {
      showCustomServerActions(static_cast<size_t>(index - 2));
    } else {
      openNewServerEditor();
    }
  });
  requestUpdate();
}

void KOReaderSettingsActivity::showCustomServerActions(const size_t customIndex) {
  const auto& servers = KOREADER_STORE.getCustomServers();
  if (customIndex >= servers.size()) {
    requestUpdate();
    return;
  }
  const std::string title = servers[customIndex];
  const std::string select = tr(STR_SELECT);
  const std::string edit = tr(STR_EDIT_SERVER);
  const std::string remove = tr(STR_DELETE_SERVER);
  const char* actions[] = {select.c_str(), edit.c_str(), remove.c_str()};
  optionPopup.show(title.c_str(), actions, 3, 0, [this, customIndex](const int action) {
    if (action == 0) {
      const auto& currentServers = KOREADER_STORE.getCustomServers();
      reportSaveResult(customIndex < currentServers.size() &&
                       KOREADER_STORE.selectServerUrl(currentServers[customIndex]));
    } else if (action == 1) {
      openCustomServerEditor(customIndex);
    } else {
      confirmCustomServerDelete(customIndex);
    }
  });
  requestUpdate();
}

void KOReaderSettingsActivity::openNewServerEditor() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ADD_SERVER), "https://",
                                              KOReaderCredentialStore::MAX_SERVER_URL_BYTES, InputType::Url),
      [this](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto& entered = std::get<KeyboardResult>(result.data).text;
        const bool valid = entered != "https://" && entered != "http://";
        reportSaveResult(valid && KOREADER_STORE.addCustomServer(entered));
      });
}

void KOReaderSettingsActivity::openCustomServerEditor(const size_t customIndex) {
  const auto& servers = KOREADER_STORE.getCustomServers();
  if (customIndex >= servers.size()) {
    requestUpdate();
    return;
  }
  const std::string current = servers[customIndex];
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_EDIT_SERVER), current,
                                              KOReaderCredentialStore::MAX_SERVER_URL_BYTES, InputType::Url),
      [this, customIndex](const ActivityResult& result) {
        if (result.isCancelled) return;
        const auto& entered = std::get<KeyboardResult>(result.data).text;
        const bool valid = !entered.empty() && entered != "https://" && entered != "http://";
        reportSaveResult(valid && KOREADER_STORE.updateCustomServer(customIndex, entered));
      });
}

void KOReaderSettingsActivity::confirmCustomServerDelete(const size_t customIndex) {
  const auto& servers = KOREADER_STORE.getCustomServers();
  if (customIndex >= servers.size()) {
    requestUpdate();
    return;
  }
  const std::string server = servers[customIndex];
  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_DELETE_SERVER), server),
                         [this, customIndex](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             reportSaveResult(KOREADER_STORE.removeCustomServer(customIndex));
                           }
                         });
}

void KOReaderSettingsActivity::render(RenderLock&&) {
  if (optionPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_KOREADER_SYNC));

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  GUI.drawList(
      renderer, Rect{0, contentTop, pageWidth, contentHeight}, static_cast<int>(MENU_ITEMS),
      static_cast<int>(selectedIndex), [](int index) { return std::string(I18N.get(menuNames[index])); }, nullptr,
      nullptr,
      [this](int index) {
        // Draw status for each setting
        if (index == 0) {
          auto username = KOREADER_STORE.getUsername();
          return username.empty() ? std::string(tr(STR_NOT_SET)) : username;
        } else if (index == 1) {
          return KOREADER_STORE.getPassword().empty() ? std::string(tr(STR_NOT_SET)) : std::string("******");
        } else if (index == 2) {
          std::string serverUrl = KOREADER_STORE.getBaseUrl();
          const auto schemeEnd = serverUrl.find("://");
          if (schemeEnd != std::string::npos) {
            serverUrl.erase(0, schemeEnd + 3);
          }
          return serverUrl;
        } else if (index == 3) {
          return KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME ? std::string(tr(STR_FILENAME))
                                                                                  : std::string(tr(STR_BINARY));
        } else if (index == 4) {
          return KOREADER_STORE.getSendMetadata() ? std::string(tr(STR_STATE_ON)) : std::string(tr(STR_STATE_OFF));
        } else if (index == 5) {
          return KOREADER_STORE.getSyncBehavior() == KOReaderSyncBehavior::SMART ? std::string(tr(STR_SMART_SYNC))
                                                                                 : std::string(tr(STR_ASK_EVERY_TIME));
        } else if (index == 6 || index == 7) {
          return KOREADER_STORE.hasCredentials() ? "" : std::string("[") + tr(STR_SET_CREDENTIALS_FIRST) + "]";
        }
        return std::string(tr(STR_NOT_SET));
      },
      true);

  // Draw help text at bottom
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (showSaveError) {
    showSaveError = false;
    drawTransientPopup(StrId::STR_ERROR_GENERAL_FAILURE);
    return;
  }

  renderer.displayBuffer();
}
