#include "SdCardFontSystem.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <MemoryBudget.h>

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
  if (!registry_.lastDiscoverySucceeded()) registryDirty_.store(true, std::memory_order_release);

  // Register this system as the SD font ID resolver in settings.
  // Uses a static trampoline since CrossPointSettings stores a plain function pointer.
  SETTINGS.sdFontIdResolver = [](void* ctx, const char* familyName, uint8_t fontSizeEnum) -> int {
    return static_cast<SdCardFontSystem*>(ctx)->resolveFontId(familyName, fontSizeEnum);
  };
  SETTINGS.sdFontResolverCtx = this;

  // The saved family (and any invalid-selection repair) is deliberately not
  // loaded here: loadFamily() reads the whole .cpfont over SD, which would
  // stall every boot. ensureLoaded() covers it before the reader lays out, and
  // nothing outside the active reader needs the loaded family.
  LOG_DBG("SDFS", "SD font system ready (%d families discovered)", registry_.getFamilyCount());
}

bool SdCardFontSystem::rediscoverIfDirty() {
  if (!registryDirty_.exchange(false, std::memory_order_acquire)) return false;
  LOG_DBG("SDFS", "Registry dirty — re-discovering fonts");
  registry_.discover();
  if (registry_.lastDiscoverySucceeded()) return true;
  registryDirty_.store(true, std::memory_order_release);
  return false;
}

void SdCardFontSystem::refreshIfDirty() { rediscoverIfDirty(); }

void SdCardFontSystem::releaseLoadedFont(GfxRenderer& renderer) {
  if (manager_.currentFamilyName().empty()) return;

  LOG_DBG("SDFS", "Releasing SD font outside active reader: %s", manager_.currentFamilyName().c_str());
  // FontCacheManager can retain glyph bitmaps derived from the SdCardFont.
  // Drop those before deleting the owner so no cache survives with stale font
  // data and the whole allocation is returned to the TLS/parser workload.
  if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
  manager_.unloadAll(renderer);
}

void SdCardFontSystem::ensureLoaded(GfxRenderer& renderer, const bool persistInvalidSelection) {
  // If the web server (or another task) installed/deleted fonts, re-discover.
  // Track whether we just re-discovered so we can force a reload below even
  // when the wanted family/size still maps to the same point size — the file
  // contents on disk may have changed (e.g. user re-uploaded a new build).
  const bool registryWasRefreshed = rediscoverIfDirty();

  const char* wantedFamily = SETTINGS.sdFontFamilyName;
  const std::string& currentFamily = manager_.currentFamilyName();
  const uint8_t sizeEnum = fontSizeEnumFromSettings();

  if (wantedFamily[0] == '\0') {
    if (!currentFamily.empty()) {
      releaseLoadedFont(renderer);
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
      if (!registry_.lastDiscoverySucceeded()) {
        LOG_ERR("SDFS", "Font registry unavailable; keeping selection for retry: %s", wantedFamily);
        return;
      }
      LOG_DBG("SDFS", "SD font family disappeared: %s (clearing)", wantedFamily);
      releaseLoadedFont(renderer);
      SETTINGS.sdFontFamilyName[0] = '\0';
      normalizeBuiltinFontSize();
      if (persistInvalidSelection) SETTINGS.saveToFile();
      return;
    }
    const auto* selected = family->findClosestReaderSize(sizeEnum);
    const uint8_t wantedPt = selected ? selected->pointSize : 0;
    if (!registryWasRefreshed && wantedPt == manager_.currentPointSize()) return;
    LOG_DBG("SDFS", "Reloading %s: size %u -> %u (enum %u)%s", wantedFamily, manager_.currentPointSize(), wantedPt,
            sizeEnum, registryWasRefreshed ? " [registry refreshed]" : "");
  }

  bool fontCachesCleared = false;
  if (!currentFamily.empty()) {
    releaseLoadedFont(renderer);
    fontCachesCleared = true;
  }

  const auto* family = registry_.findFamily(wantedFamily);
  if (family) {
    // Loading a .cpfont creates several persistent lookup tables. Release all
    // disposable glyph data first, then require both total and contiguous
    // headroom. With exceptions disabled this gate is what turns a fragmented
    // heap into a normal font-load failure rather than an allocation abort.
    if (!fontCachesCleared) {
      if (auto* cache = renderer.getFontCacheManager()) cache->clearAllCaches();
    }
    const auto memory = MemoryBudget::snapshot();
    MemoryBudget::logStage("SDFS", "load_begin");
    if (!MemoryBudget::hasHeadroom(memory, MemoryBudget::SD_FONT_LOAD)) {
      LOG_ERR("SDFS", "Not enough heap to load SD font: free=%u maxalloc=%u", memory.freeHeap, memory.maxAllocHeap);
      // Keep the selection so a later attempt after leaving a memory-heavy
      // activity can load it. The resolver returns the built-in fallback for
      // this frame instead of persisting a destructive settings change.
      return;
    }
    if (manager_.loadFamily(*family, renderer, sizeEnum)) {
      LOG_DBG("SDFS", "Loaded SD font family: %s", wantedFamily);
    } else {
      // A known family can still fail transiently because the heap is
      // fragmented or the SD card is briefly unavailable. Keep the user's
      // choice so the next reader entry can retry instead of converting that
      // temporary failure into a persisted Noto fallback.
      LOG_ERR("SDFS", "Failed to load SD font family: %s (keeping selection for retry)", wantedFamily);
    }
  } else {
    if (!registry_.lastDiscoverySucceeded()) {
      LOG_ERR("SDFS", "Font registry unavailable; keeping selection for retry: %s", wantedFamily);
      return;
    }
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
