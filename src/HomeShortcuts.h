#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

// Stable identifiers persisted in settings.json. Append new entries before
// Count; never reorder existing entries.
enum class HomeShortcutId : uint8_t {
  Appearance = 0,
  TextSettings = 1,
  QuickResume = 2,
  SleepScreen = 3,
  HideBattery = 4,
  StatusBar = 5,
  HomeLayout = 6,
  ShowDeviceName = 7,
  OutsideReaderClock = 8,
  OutsideReaderDate = 9,
  LibraryView = 10,
  LibrarySort = 11,
  HideTxtBooks = 12,
  ReaderDarkMode = 13,
  TextAntiAliasing = 14,
  LineSpacing = 15,
  WordSpacing = 16,
  ScreenMargin = 17,
  EmbeddedStyle = 18,
  ParagraphAlignment = 19,
  ExtraParagraphSpacing = 20,
  ForceParagraphIndents = 21,
  Hyphenation = 22,
  FocusReading = 23,
  ImageRendering = 24,
  SkipEpubCover = 25,
  Orientation = 26,
  RefreshFrequency = 27,
  SideButtonLayout = 28,
  FrontButtonsFollowOrientation = 29,
  ShortPowerButton = 30,
  TiltPageTurn = 31,
  LongPressMenu = 32,
  LongPressBehavior = 33,
  DoublePowerReading = 34,
  DoublePowerOutsideReader = 35,
  BackToFileBrowser = 36,
  SleepTimeout = 37,
  TimeSettings = 38,
  Language = 39,
  FontManager = 40,
  WifiNetworks = 41,
  KOReaderSettings = 42,
  OpdsServers = 43,
  Count = 44,
};

constexpr bool isValidHomeShortcutId(const uint8_t raw) { return raw < static_cast<uint8_t>(HomeShortcutId::Count); }

struct HomeShortcutList {
  static constexpr uint8_t CAPACITY = 8;

  uint8_t count = 6;
  uint8_t items[CAPACITY] = {
      static_cast<uint8_t>(HomeShortcutId::Appearance),  static_cast<uint8_t>(HomeShortcutId::TextSettings),
      static_cast<uint8_t>(HomeShortcutId::QuickResume), static_cast<uint8_t>(HomeShortcutId::SleepScreen),
      static_cast<uint8_t>(HomeShortcutId::HideBattery), static_cast<uint8_t>(HomeShortcutId::StatusBar),
  };

  [[nodiscard]] bool contains(const HomeShortcutId id, const int exceptIndex = -1) const {
    const uint8_t raw = static_cast<uint8_t>(id);
    for (uint8_t index = 0; index < count && index < CAPACITY; ++index) {
      if (index != exceptIndex && items[index] == raw) return true;
    }
    return false;
  }

  [[nodiscard]] HomeShortcutId at(const uint8_t index) const {
    return index < count && index < CAPACITY && isValidHomeShortcutId(items[index])
               ? static_cast<HomeShortcutId>(items[index])
               : HomeShortcutId::Appearance;
  }

  bool add(const HomeShortcutId id) {
    if (count >= CAPACITY || contains(id)) return false;
    items[count++] = static_cast<uint8_t>(id);
    return true;
  }

  bool replace(const uint8_t index, const HomeShortcutId id) {
    if (index >= count || index >= CAPACITY || contains(id, index)) return false;
    items[index] = static_cast<uint8_t>(id);
    return true;
  }

  bool remove(const uint8_t index) {
    if (index >= count || index >= CAPACITY) return false;
    for (uint8_t current = index; current + 1 < count; ++current) items[current] = items[current + 1];
    --count;
    items[count] = 0;
    return true;
  }

  bool move(const uint8_t from, const uint8_t to) {
    if (from >= count || to >= count || from == to) return false;
    const uint8_t moving = items[from];
    if (from < to) {
      for (uint8_t index = from; index < to; ++index) items[index] = items[index + 1];
    } else {
      for (uint8_t index = from; index > to; --index) items[index] = items[index - 1];
    }
    items[to] = moving;
    return true;
  }

  void clear() {
    count = 0;
    std::fill(items, items + CAPACITY, uint8_t{0});
  }
};
