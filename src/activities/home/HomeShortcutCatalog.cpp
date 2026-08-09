#include "HomeShortcutCatalog.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalTiltSensor.h>

namespace {
using Id = HomeShortcutId;
using Target = HomeShortcutTarget;

constexpr std::array<HomeShortcutDescriptor, static_cast<size_t>(Id::Count)> CATALOG = {{
    {Id::Appearance, StrId::STR_INTERFACE_CUSTOMIZATION, Target::Appearance, nullptr},
    {Id::TextSettings, StrId::STR_TEXT_SETTINGS, Target::TextSettings, nullptr},
    {Id::QuickResume, StrId::STR_QUICK_RESUME, Target::Setting, "quickResumeSleepScreen"},
    {Id::SleepScreen, StrId::STR_SLEEP_SCREEN, Target::Setting, "sleepScreen"},
    {Id::HideBattery, StrId::STR_SHOW_BATTERY_PERCENTAGE, Target::Setting, "hideBatteryPercentage"},
    {Id::StatusBar, StrId::STR_CUSTOMISE_STATUS_BAR, Target::StatusBar, nullptr},
    {Id::HomeLayout, StrId::STR_HOME_LAYOUT, Target::Setting, "homeLayout"},
    {Id::ShowDeviceName, StrId::STR_SHOW_DEVICE_NAME_HOME, Target::Setting, "showDeviceNameOnHome"},
    {Id::OutsideReaderClock, StrId::STR_CLOCK_OUTSIDE_READER, Target::Setting, "outsideReaderClock"},
    {Id::OutsideReaderDate, StrId::STR_DATE_OUTSIDE_READER, Target::Setting, "showDateOutsideReader"},
    {Id::LibraryView, StrId::STR_LIBRARY_DISPLAY_MODE, Target::Setting, "libraryView"},
    {Id::LibrarySort, StrId::STR_LIBRARY_SORT, Target::Setting, "librarySort"},
    {Id::HideTxtBooks, StrId::STR_SHOW_TXT_BOOKS, Target::Setting, "hideTxtBooks"},
    {Id::ReaderDarkMode, StrId::STR_READER_DARK_MODE, Target::Setting, "readerDarkMode"},
    {Id::TextAntiAliasing, StrId::STR_TEXT_AA, Target::Setting, "textAntiAliasing"},
    {Id::LineSpacing, StrId::STR_LINE_SPACING, Target::Setting, "lineSpacing"},
    {Id::WordSpacing, StrId::STR_WORD_SPACING, Target::Setting, "wordSpacing"},
    {Id::ScreenMargin, StrId::STR_SCREEN_MARGIN, Target::Setting, "screenMargin"},
    {Id::EmbeddedStyle, StrId::STR_EMBEDDED_STYLE, Target::Setting, "embeddedStyle"},
    {Id::ParagraphAlignment, StrId::STR_PARA_ALIGNMENT, Target::Setting, "paragraphAlignment"},
    {Id::ExtraParagraphSpacing, StrId::STR_EXTRA_SPACING, Target::Setting, "extraParagraphSpacing"},
    {Id::ForceParagraphIndents, StrId::STR_FORCE_PARAGRAPH_INDENTS, Target::Setting, "forceParagraphIndents"},
    {Id::Hyphenation, StrId::STR_HYPHENATION, Target::Setting, "hyphenationEnabled"},
    {Id::FocusReading, StrId::STR_FOCUS_READING, Target::Setting, "focusReadingEnabled"},
    {Id::ImageRendering, StrId::STR_IMAGES, Target::Setting, "imageRendering"},
    {Id::SkipEpubCover, StrId::STR_SKIP_EPUB_COVER_PAGE, Target::Setting, "skipEpubCoverPage"},
    {Id::Orientation, StrId::STR_ORIENTATION, Target::Setting, "orientation"},
    {Id::RefreshFrequency, StrId::STR_REFRESH_EVERY, Target::Setting, "refreshFrequency"},
    {Id::SideButtonLayout, StrId::STR_SIDE_BTN_LAYOUT, Target::Setting, "sideButtonLayout"},
    {Id::FrontButtonsFollowOrientation, StrId::STR_FRONT_BTN_FOLLOW_ORIENTATION, Target::Setting,
     "frontButtonFollowOrientation"},
    {Id::ShortPowerButton, StrId::STR_SHORT_PWR_BTN, Target::Setting, "shortPwrBtn"},
    {Id::TiltPageTurn, StrId::STR_TILT_PAGE_TURN, Target::Setting, "tiltPageTurn"},
    {Id::LongPressMenu, StrId::STR_LONG_PRESS_MENU, Target::Setting, "longPressMenuFunction"},
    {Id::LongPressBehavior, StrId::STR_LONG_PRESS_BEHAVIOR, Target::Setting, "longPressButtonBehavior"},
    {Id::DoublePowerReading, StrId::STR_DOUBLE_POWER_READING, Target::Setting, "doublePowerReadingFunction"},
    {Id::DoublePowerOutsideReader, StrId::STR_DOUBLE_POWER_ACTION, Target::Setting, "doublePowerAction"},
    {Id::BackToFileBrowser, StrId::STR_WHEN_LEAVING_READER, Target::Setting, "backShortToFileBrowser"},
    {Id::SleepTimeout, StrId::STR_TIME_TO_SLEEP, Target::Setting, "sleepTimeoutMinutes"},
    {Id::TimeSettings, StrId::STR_TIME_SETTINGS, Target::Time, nullptr},
    {Id::Language, StrId::STR_LANGUAGE, Target::Language, nullptr},
    {Id::FontManager, StrId::STR_MANAGE_FONTS, Target::FontManager, nullptr},
    {Id::WifiNetworks, StrId::STR_WIFI_NETWORKS, Target::WifiNetworks, nullptr},
    {Id::KOReaderSettings, StrId::STR_KOREADER_SYNC, Target::KOReaderSettings, nullptr},
    {Id::OpdsServers, StrId::STR_OPDS_SERVERS, Target::OpdsServers, nullptr},
}};
static_assert(CATALOG.size() == static_cast<size_t>(Id::Count));
}  // namespace

const std::array<HomeShortcutDescriptor, static_cast<size_t>(HomeShortcutId::Count)>& homeShortcutCatalog() {
  return CATALOG;
}

const HomeShortcutDescriptor* findHomeShortcut(const HomeShortcutId id) {
  const size_t index = static_cast<size_t>(id);
  return index < CATALOG.size() && CATALOG[index].id == id ? &CATALOG[index] : nullptr;
}

bool isHomeShortcutAvailable(const HomeShortcutId id, const GfxRenderer& renderer) {
  switch (id) {
    case HomeShortcutId::OutsideReaderClock:
    case HomeShortcutId::OutsideReaderDate:
    case HomeShortcutId::TimeSettings:
      return halClock.isAvailable();
    case HomeShortcutId::TiltPageTurn:
      return halTiltSensor.isAvailable();
    case HomeShortcutId::TextAntiAliasing:
      return renderer.supportsStripGrayscale();
    default:
      return findHomeShortcut(id) != nullptr;
  }
}
