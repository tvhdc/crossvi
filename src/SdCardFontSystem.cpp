#include "SdCardFontSystem.h"

#include <GfxRenderer.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "FontInstaller.h"

namespace {

static uint8_t fontSizeEnumFromSettings() {
  uint8_t e = SETTINGS.fontSize;
  if (e >= CrossPointSettings::FONT_SIZE_COUNT) e = 1;  // default to MEDIUM
  return e;
}

bool normalizeBuiltinFontSize() {
  if (SETTINGS.fontSize < ReaderFontSize::BUILTIN_COUNT) return false;
  SETTINGS.fontSize = CrossPointSettings::EXTRA_LARGE;
  return true;
}

}  // namespace

void SdCardFontSystem::begin() {
  if (SETTINGS.sdFontFamilyName[0] != '\0') {
    FontInstaller installer(registry_);
    if (!installer.recoverInterruptedFamilyDownload(SETTINGS.sdFontFamilyName)) {
      LOG_ERR("SDFS", "Failed to recover interrupted font update: %s", SETTINGS.sdFontFamilyName);
    }
  }
  registry_.discover();

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // The saved family (and any invalid-selection repair) is deliberately not
  // loaded here: loadFamily() reads the whole .cpfont over SD, which would
  // stall every boot. ensureLoaded() covers it before the reader lays out, and
  // nothing outside the reader/settings needs the loaded family.
  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, const bool persistInvalidSelection) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasDirty = registryDirty_.exchange(false, std::memory_order_acquire);
  if (registryWasDirty) {
    LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
    registry_.discover();
  }

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      manager_.unloadAll(renderer);
    }
    if (normalizeBuiltinFontSize() && persistInvalidSelection) SETTINGS.saveToFile();
    return;
  }

  // Reload if family changed OR if the user-selected size maps to a
  // different file than what's currently loaded OR if the registry was
  // just rediscovered (file may have been replaced on disk).
  bool familyMatches = (currentFamily == wantedFamily);
  if (familyMatches) {
    const auto* family = registry_.findFamily(wantedFamily);
    if (!family) {
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      manager_.unloadAll(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      normalizeBuiltinFontSize();
      if (persistInvalidSelection) SETTINGS.saveToFile();
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasDirty && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasDirty ? " [registry dirty]" : "");
  }

  if (!currentFamily.empty()) {
    manager_.unloadAll(renderer);
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      LOG_ERR("SDFS", "Failed to load SD font family: %s (clearing)", wantedFamily);
      SETTINGS.sdFontFamilyName[0] = '\0';
      normalizeBuiltinFontSize();
      if (persistInvalidSelection) SETTINGS.saveToFile();
    }
  } else {
    LOG_DBG("SDFS", "SD font family not found: %s (clearing)", wantedFamily);
    SETTINGS.sdFontFamilyName[0] = '\0';
    normalizeBuiltinFontSize();
    if (persistInvalidSelection) SETTINGS.saveToFile();
  }
}

int SdCardFontSystem::resolveFontId(const char* familyName, uint8_t /*fontSizeEnum*/) const {
  // The manager loads exactly one size (closest to SETTINGS.fontSize), so the
  // enum is implicit — always return the single loaded font ID for this family.
  // ensureLoaded() must have been called with the current settings before this.
  return manager_.getFontId(familyName);
}

uint8_t SdCardFontSystem::selectedPointSize(const char* familyName, const uint8_t fontSizeEnum) const {
  if (!familyName || familyName[0] == '\0') return 0;
  const auto* family = registry_.findFamily(familyName);
  if (!family) return 0;
  const auto* selected = family->findClosestReaderSize(fontSizeEnum);
  return selected ? selected->pointSize : 0;
}
