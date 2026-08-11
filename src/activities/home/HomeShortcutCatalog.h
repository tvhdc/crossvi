#pragma once

#include <I18nKeys.h>

#include <array>
#include <cstddef>

#include "HomeShortcuts.h"

class GfxRenderer;

enum class HomeShortcutTarget {
  Setting,
  Appearance,
  TextSettings,
  StatusBar,
  Time,
  Language,
  FontManager,
  WifiNetworks,
  KOReaderSettings,
  OpdsServers,
  VocabularyLearning,
};

struct HomeShortcutDescriptor {
  HomeShortcutId id;
  StrId label;
  HomeShortcutTarget target;
  const char* settingKey;
};

const std::array<HomeShortcutDescriptor, static_cast<size_t>(HomeShortcutId::Count)>& homeShortcutCatalog();
const HomeShortcutDescriptor* findHomeShortcut(HomeShortcutId id);
bool isHomeShortcutAvailable(HomeShortcutId id, const GfxRenderer& renderer);
