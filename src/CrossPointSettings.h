#pragma once

#include <Epub/EpubRenderMode.h>
#include <HalStorage.h>
#include <ReaderFontSize.h>

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <mutex>

#include "HomeShortcuts.h"

class CrossPointSettings {
 private:
  mutable std::mutex _mutex;
  mutable bool persistenceWritable = true;

  // Private constructor for singleton
  CrossPointSettings() = default;

  // Static instance
  static CrossPointSettings instance;

 public:
  // Delete copy constructor and assignment
  CrossPointSettings(const CrossPointSettings&) = delete;
  CrossPointSettings& operator=(const CrossPointSettings&) = delete;

  // Access the settings mutex for protecting multi-field reads/writes from other cores.
  // Callers must not re-enter SETTINGS methods that lock _mutex while holding it.
  std::mutex& getMutex() const { return _mutex; }

  enum SLEEP_SCREEN_MODE {
    // Values 1, 4 and 6 are retained for compatibility with settings written
    // by older firmware. The settings UI exposes only Default, Cover, Custom,
    // Blank, Reading statistics and the two image-with-statistics variants
    // through the compact selection helpers below.
    DARK = 0,
    LIGHT = 1,
    CUSTOM = 2,
    COVER = 3,
    COVER_CUSTOM = 4,
    BLANK = 5,
    QUICK_RESUME = 6,
    READING_CALENDAR = 7,
    COVER_STATS = 8,
    CUSTOM_STATS = 9,
    SLEEP_SCREEN_MODE_COUNT
  };
  enum SLEEP_SCREEN_SELECTION {
    SLEEP_SCREEN_DEFAULT = 0,
    SLEEP_SCREEN_COVER = 1,
    SLEEP_SCREEN_CUSTOM = 2,
    SLEEP_SCREEN_BLANK = 3,
    SLEEP_SCREEN_READING_CALENDAR = 4,
    SLEEP_SCREEN_COVER_STATS = 5,
    SLEEP_SCREEN_CUSTOM_STATS = 6,
    SLEEP_SCREEN_SELECTION_COUNT
  };
  static constexpr uint8_t sleepScreenSelection(const uint8_t mode) {
    switch (mode) {
      case COVER:
      case COVER_CUSTOM:
        return SLEEP_SCREEN_COVER;
      case CUSTOM:
        return SLEEP_SCREEN_CUSTOM;
      case BLANK:
        return SLEEP_SCREEN_BLANK;
      case READING_CALENDAR:
        return SLEEP_SCREEN_READING_CALENDAR;
      case COVER_STATS:
        return SLEEP_SCREEN_COVER_STATS;
      case CUSTOM_STATS:
        return SLEEP_SCREEN_CUSTOM_STATS;
      case DARK:
      case LIGHT:
      case QUICK_RESUME:
      default:
        return SLEEP_SCREEN_DEFAULT;
    }
  }
  static constexpr uint8_t sleepScreenMode(const uint8_t selection) {
    switch (selection) {
      case SLEEP_SCREEN_COVER:
        return COVER;
      case SLEEP_SCREEN_CUSTOM:
        return CUSTOM;
      case SLEEP_SCREEN_BLANK:
        return BLANK;
      case SLEEP_SCREEN_READING_CALENDAR:
        return READING_CALENDAR;
      case SLEEP_SCREEN_COVER_STATS:
        return COVER_STATS;
      case SLEEP_SCREEN_CUSTOM_STATS:
        return CUSTOM_STATS;
      case SLEEP_SCREEN_DEFAULT:
      default:
        return LIGHT;
    }
  }
  enum SLEEP_SCREEN_COVER_MODE { FIT = 0, CROP = 1, SLEEP_SCREEN_COVER_MODE_COUNT };
  enum SLEEP_SCREEN_COVER_FILTER {
    NO_FILTER = 0,
    BLACK_AND_WHITE = 1,
    INVERTED_BLACK_AND_WHITE = 2,
    SLEEP_SCREEN_COVER_FILTER_COUNT
  };

  // Status bar enum - legacy
  enum STATUS_BAR_MODE {
    NONE = 0,
    NO_PROGRESS = 1,
    FULL = 2,
    BOOK_PROGRESS_BAR = 3,
    ONLY_BOOK_PROGRESS_BAR = 4,
    CHAPTER_PROGRESS_BAR = 5,
    STATUS_BAR_MODE_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR {
    BOOK_PROGRESS = 0,
    CHAPTER_PROGRESS = 1,
    HIDE_PROGRESS = 2,
    STATUS_BAR_PROGRESS_BAR_COUNT
  };
  enum STATUS_BAR_PROGRESS_BAR_THICKNESS {
    PROGRESS_BAR_THIN = 0,
    PROGRESS_BAR_NORMAL = 1,
    PROGRESS_BAR_THICK = 2,
    STATUS_BAR_PROGRESS_BAR_THICKNESS_COUNT
  };
  enum STATUS_BAR_TITLE { BOOK_TITLE = 0, CHAPTER_TITLE = 1, HIDE_TITLE = 2, STATUS_BAR_TITLE_COUNT };
  enum XTC_STATUS_BAR_MODE {
    XTC_STATUS_BAR_HIDE = 0,
    XTC_STATUS_BAR_BOTTOM = 1,
    XTC_STATUS_BAR_TOP = 2,
    XTC_STATUS_BAR_MODE_COUNT
  };

  enum STATUS_BAR_CLOCK_MODE {
    STATUS_BAR_CLOCK_HIDE = 0,
    STATUS_BAR_CLOCK_RIGHT = 1,
    STATUS_BAR_CLOCK_LEFT = 2,
    STATUS_BAR_CLOCK_MODE_COUNT
  };

  enum ORIENTATION {
    PORTRAIT = 0,       // 480x800 logical coordinates (current default)
    LANDSCAPE_CW = 1,   // 800x480 logical coordinates, rotated 180° (swap top/bottom)
    INVERTED = 2,       // 480x800 logical coordinates, inverted
    LANDSCAPE_CCW = 3,  // 800x480 logical coordinates, native panel orientation
    ORIENTATION_COUNT
  };

  // Front button layout options (legacy)
  // Default: Back, Confirm, Left, Right
  // Swapped: Left, Right, Back, Confirm
  enum FRONT_BUTTON_LAYOUT {
    BACK_CONFIRM_LEFT_RIGHT = 0,
    LEFT_RIGHT_BACK_CONFIRM = 1,
    LEFT_BACK_CONFIRM_RIGHT = 2,
    BACK_CONFIRM_RIGHT_LEFT = 3,
    FRONT_BUTTON_LAYOUT_COUNT
  };

  // Front button hardware identifiers (for remapping)
  enum FRONT_BUTTON_HARDWARE {
    FRONT_HW_BACK = 0,
    FRONT_HW_CONFIRM = 1,
    FRONT_HW_LEFT = 2,
    FRONT_HW_RIGHT = 3,
    FRONT_BUTTON_HARDWARE_COUNT
  };

  // Side button layout options
  // Default: Up = Previous, Down = Next
  enum SIDE_BUTTON_LAYOUT {
    PREV_NEXT = 0,
    NEXT_PREV = 1,
    SIDE_BUTTONS_DISABLED = 2,
    NEXT_NEXT = 3,
    SIDE_BUTTON_LAYOUT_COUNT
  };

  // Font family options (built-in fonts only; SD card fonts use sdFontFamilyName).
  // Value 1 used to select the removed built-in Noto Sans reader family.
  enum FONT_FAMILY { NOTOSERIF = 0, FONT_FAMILY_COUNT };
  enum DICTIONARY_FONT_FAMILY {
    DICTIONARY_FONT_READER = 0,
    DICTIONARY_FONT_NOTO_SERIF = 1,
    DICTIONARY_FONT_FAMILY_COUNT,
  };
  static constexpr uint8_t LEGACY_NOTOSANS = 1;
  static constexpr uint8_t LEGACY_DICTIONARY_FONT_NOTO_SANS = 2;
  static constexpr uint8_t LEGACY_OPENDYSLEXIC = 2;
  static constexpr uint8_t BUILTIN_FONT_COUNT = FONT_FAMILY_COUNT;

  enum VOCABULARY_QUIZ_SIZE {
    VOCABULARY_QUIZ_5 = 0,
    VOCABULARY_QUIZ_10 = 1,
    VOCABULARY_QUIZ_20 = 2,
    VOCABULARY_QUIZ_30 = 3,
    VOCABULARY_QUIZ_SIZE_COUNT,
  };
  enum VOCABULARY_QUESTION_TIME {
    VOCABULARY_TIME_10_SECONDS = 0,
    VOCABULARY_TIME_15_SECONDS = 1,
    VOCABULARY_TIME_20_SECONDS = 2,
    VOCABULARY_TIME_30_SECONDS = 3,
    VOCABULARY_TIME_UNLIMITED = 4,
    VOCABULARY_QUESTION_TIME_COUNT,
  };
  enum VOCABULARY_ANSWER_COUNT {
    VOCABULARY_ANSWERS_3 = 0,
    VOCABULARY_ANSWERS_4 = 1,
    VOCABULARY_ANSWER_COUNT_COUNT,
  };
  // Font size options
  enum FONT_SIZE {
    SMALL = 0,
    MEDIUM = 1,
    LARGE = 2,
    EXTRA_LARGE = 3,
    SIZE_20 = 4,
    SIZE_22 = 5,
    SIZE_24 = 6,
    SIZE_26 = 7,
    SIZE_28 = 8,
    FONT_SIZE_COUNT = ReaderFontSize::COUNT
  };
  static_assert(SMALL == 0 && MEDIUM == 1 && LARGE == 2 && EXTRA_LARGE == 3,
                "Existing font-size settings must retain their stored meaning");
  enum LINE_COMPRESSION { TIGHT = 0, NORMAL = 1, WIDE = 2, LINE_COMPRESSION_COUNT };
  enum DATE_FORMAT {
    DATE_FORMAT_MONTH_DAY_YEAR_LONG = 0,
    DATE_FORMAT_DAY_MONTH_YEAR_LONG = 1,
    DATE_FORMAT_MONTH_DAY_YEAR_NUMERIC = 2,
    DATE_FORMAT_DAY_MONTH_YEAR_NUMERIC = 3,
    DATE_FORMAT_YEAR_MONTH_DAY_NUMERIC = 4,
    DATE_FORMAT_MONTH_DAY_NUMERIC = 5,
    DATE_FORMAT_DAY_MONTH_NUMERIC = 6,
    DATE_FORMAT_MONTH_DAY_LONG = 7,
    DATE_FORMAT_DAY_MONTH_LONG = 8,
    DATE_FORMAT_COUNT,
  };
  enum DATE_SEPARATOR {
    DATE_SEPARATOR_PERIOD = 0,
    DATE_SEPARATOR_HYPHEN = 1,
    DATE_SEPARATOR_SLASH = 2,
    DATE_SEPARATOR_COUNT,
  };
  enum PARAGRAPH_ALIGNMENT {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    BOOK_STYLE = 4,
    PARAGRAPH_ALIGNMENT_COUNT
  };

  // Auto-sleep timeout options (in minutes)
  enum SLEEP_TIMEOUT {
    SLEEP_1_MIN = 0,
    SLEEP_5_MIN = 1,
    SLEEP_10_MIN = 2,
    SLEEP_15_MIN = 3,
    SLEEP_30_MIN = 4,
    SLEEP_TIMEOUT_COUNT
  };

  // E-ink refresh frequency (pages between full refreshes)
  enum REFRESH_FREQUENCY {
    REFRESH_1 = 0,
    REFRESH_5 = 1,
    REFRESH_10 = 2,
    REFRESH_15 = 3,
    REFRESH_30 = 4,
    REFRESH_FREQUENCY_COUNT
  };

  // Short power button press actions
  enum SHORT_PWRBTN { IGNORE = 0, SLEEP = 1, PAGE_TURN = 2, FORCE_REFRESH = 3, FOOTNOTES = 4, SHORT_PWRBTN_COUNT };

  // Long-press Confirm action while reading an EPUB. The setting cycles through these values.
  // Persisted in settings.json by index: any new function (e.g. dictionary, bookmark) MUST use a
  // value >= 2 and be appended at the END of the enumValues array in SettingsList.h, otherwise the
  // stored indices shift and existing saves are silently misinterpreted.
  enum LONG_PRESS_MENU_FUNCTION {
    LP_MENU_KOSYNC = 0,
    LP_MENU_DISABLED = 1,
    LP_MENU_BOOKMARK = 2,
    LP_MENU_DICTIONARY = 3,
    LP_MENU_READING_STATS = 4,
    LP_MENU_AUTO_PAGE_TURN = 5,
    LP_MENU_HIGHLIGHT = 6,
    LP_MENU_SCREENSHOT = 7,
    LP_MENU_REFRESH = 8,
    LONG_PRESS_MENU_FUNCTION_COUNT
  };

  enum DOUBLE_POWER_ACTION {
    DOUBLE_POWER_DISABLED = 0,
    DOUBLE_POWER_HOME = 1,
    DOUBLE_POWER_RESUME = 2,
    DOUBLE_POWER_REFRESH = 3,
    DOUBLE_POWER_SCREENSHOT = 4,
    DOUBLE_POWER_ACTION_COUNT
  };

  enum LIBRARY_VIEW { LIBRARY_LIST = 0, LIBRARY_COVERS = 1, LIBRARY_VIEW_COUNT };
  // Ordering for the All tab. Recent remains recency-ordered by RecentBooksStore.
  // Keep the numeric values stable because they are persisted in settings.json.
  enum LIBRARY_SORT {
    LIBRARY_SORT_DATE_ADDED_DESC = 0,
    LIBRARY_SORT_TITLE_ASC = 1,
    LIBRARY_SORT_AUTHOR_ASC = 2,
    LIBRARY_SORT_COUNT
  };
  // homeLayoutVersion migrates the former value 2 (Carousel) to Style 4. Keep
  // these canonical values stable after that one-time migration.
  enum HOME_LAYOUT {
    HOME_LAYOUT_STYLE_1 = 0,
    HOME_LAYOUT_STYLE_2 = 1,
    HOME_LAYOUT_STYLE_3 = 2,
    HOME_LAYOUT_STYLE_4 = 3,
    HOME_LAYOUT_COUNT
  };
  static constexpr uint8_t HOME_LAYOUT_VERSION = 2;
  static constexpr uint8_t canonicalHomeLayout(const int rawValue, const uint8_t storedVersion) {
    if (storedVersion < HOME_LAYOUT_VERSION && rawValue == HOME_LAYOUT_STYLE_3) return HOME_LAYOUT_STYLE_4;
    return rawValue >= 0 && rawValue < HOME_LAYOUT_COUNT ? static_cast<uint8_t>(rawValue) : HOME_LAYOUT_STYLE_2;
  }
  static_assert(HOME_LAYOUT_STYLE_1 == 0 && HOME_LAYOUT_STYLE_2 == 1 && HOME_LAYOUT_STYLE_3 == 2 &&
                HOME_LAYOUT_STYLE_4 == 3);
  static constexpr bool needsSharedCoverThumbnail(const uint8_t homeLayout, const uint8_t libraryView) {
    return homeLayout != HOME_LAYOUT_STYLE_1 || libraryView == LIBRARY_COVERS;
  }
  static constexpr bool needsCarouselCoverThumbnail(const uint8_t homeLayout) {
    return homeLayout == HOME_LAYOUT_STYLE_4;
  }
  // Home style 1 has no cover surface of its own. Only an open from a
  // coverless launcher may skip cache generation, and only when the library
  // itself is configured as a list. If the library displays covers, opening
  // from Home must prepare both shared and carousel thumbnails.
  static constexpr bool skipReaderCoverCacheBuild(const uint8_t homeLayout, const uint8_t libraryView,
                                                  const bool openedFromCoverlessSurface) {
    return openedFromCoverlessSurface && homeLayout == HOME_LAYOUT_STYLE_1 && libraryView == LIBRARY_LIST;
  }
  // The 4x3 value is retained only so older settings can be decoded.  New
  // firmware exposes one shared 3x2 cover layout for both library tabs.
  enum LIBRARY_GRID { LIBRARY_GRID_3X2 = 0, LIBRARY_GRID_4X3 = 1, LIBRARY_GRID_COUNT = 1 };

  // Hide battery percentage
  enum HIDE_BATTERY_PERCENTAGE { HIDE_NEVER = 0, HIDE_READER = 1, HIDE_ALWAYS = 2, HIDE_BATTERY_PERCENTAGE_COUNT };

  // Page turn button long press behavior
  enum LONG_PRESS_BUTTON_BEHAVIOR {
    OFF = 0,
    CHAPTER_SKIP = 1,
    ORIENTATION_CHANGE = 2,
    LONG_PRESS_BUTTON_BEHAVIOR_COUNT
  };

  // Image rendering in EPUB reader
  enum IMAGE_RENDERING { IMAGES_DISPLAY = 0, IMAGES_PLACEHOLDER = 1, IMAGES_SUPPRESS = 2, IMAGE_RENDERING_COUNT };

  // Keep the historic numeric directions stable across CrossPoint/CrossInk
  // settings. The three-choice UI maps these values to normal/reversed labels
  // without rewriting the user's current physical direction.
  enum TILT_PAGE_TURN {
    TILT_OFF = 0,
    TILT_ON = 1,
    TILT_INVERTED = 2,
    TILT_LEGACY_LEFT_NEXT = TILT_INVERTED,
    TILT_PAGE_TURN_COUNT
  };

  enum QUICK_RESUME_SLEEP_SCREEN {
    QUICK_RESUME_NEVER = 0,
    QUICK_RESUME_AFTER_TIMEOUT = 1,
    QUICK_RESUME_SLEEP_SCREEN_COUNT
  };

  enum HOME_BACK_ACTION {
    HOME_BACK_SHORTCUTS = 0,
    HOME_BACK_CONTINUE_READING = 1,
    HOME_BACK_NONE = 2,
    HOME_BACK_ACTION_COUNT
  };

  enum OUTSIDE_READER_DATE_TIME_ORDER {
    OUTSIDE_READER_DATE_THEN_TIME = 0,
    OUTSIDE_READER_TIME_THEN_DATE = 1,
    OUTSIDE_READER_DATE_TIME_ORDER_COUNT
  };

  // Sleep screen settings
  uint8_t sleepScreen = LIGHT;
  // Sleep screen cover mode settings
  uint8_t sleepScreenCoverMode = FIT;
  // Sleep screen cover filter
  uint8_t sleepScreenCoverFilter = NO_FILTER;
  // Status bar settings (statusBar retained for migration only)
  uint8_t statusBar = FULL;
  uint8_t statusBarChapterPageCount = 1;
  uint8_t statusBarBookProgressPercentage = 1;
  uint8_t statusBarProgressBar = HIDE_PROGRESS;
  uint8_t statusBarProgressBarThickness = PROGRESS_BAR_NORMAL;
  uint8_t statusBarTitle = CHAPTER_TITLE;
  uint8_t statusBarBattery = 1;
  uint8_t xtcStatusBarMode = XTC_STATUS_BAR_HIDE;
  // Clock display in status bar (X3 only, requires DS3231 RTC)
  uint8_t statusBarClock = STATUS_BAR_CLOCK_HIDE;
  // Clock display in headers outside the reader (X3 only, requires DS3231 RTC).
  // Boolean setting: 0 = hidden, 1 = shown at the fixed right-side position.
  uint8_t outsideReaderClock = 0;
  // Optionally show the local date next to the outside-reader clock.
  uint8_t showDateOutsideReader = 0;
  // Ordering used only when both the outside-reader date and clock are shown.
  uint8_t outsideReaderDateTimeOrder = OUTSIDE_READER_DATE_THEN_TIME;
  // Clock UTC offset in quarter-hour steps, biased by 48 so it fits in uint8_t.
  // Value 48 = UTC+0, 0 = UTC-12:00, 104 = UTC+14:00.
  // Quarter-hour granularity supports oddball zones like Nepal (+5:45) and Chatham (+12:45).
  uint8_t clockUtcOffsetQ = 48;
  static constexpr uint8_t VIETNAM_UTC_OFFSET_Q = 76;  // UTC+07:00
  // Clock display format: 0 = 24-hour, 1 = 12-hour
  uint8_t clockFormat = 0;
  uint8_t dateFormat = DATE_FORMAT_MONTH_DAY_YEAR_LONG;
  uint8_t dateSeparator = DATE_SEPARATOR_SLASH;
  // Set once an NTP sync succeeds. Used to skip re-syncing on every WiFi connect.
  // Resetting to 0 (e.g. via the web UI) forces a re-sync on next WiFi connect.
  uint8_t clockHasBeenSynced = 0;
  // Text rendering settings
  uint8_t extraParagraphSpacing = 1;
  uint8_t forceParagraphIndents = 0;
  uint8_t textAntiAliasing = 1;
  // Reader-only inverse page mode. Menus and the rest of the UI stay light.
  uint8_t readerDarkMode = 0;
  // Short power button click behaviour
  uint8_t shortPwrBtn = IGNORE;
  // Optional global double-click action for the power button.
  uint8_t doublePowerAction = DOUBLE_POWER_DISABLED;
  // Optional reader shortcut invoked by a double-click while a reader is visible.
  // Uses the same persisted indices as longPressMenuFunction.
  uint8_t doublePowerReadingFunction = LP_MENU_DISABLED;
  // EPUB reading orientation settings
  // 0 = portrait (default), 1 = landscape clockwise, 2 = inverted, 3 = landscape counter-clockwise
  uint8_t orientation = PORTRAIT;
  // Button layouts (front layout retained for migration only)
  uint8_t frontButtonLayout = BACK_CONFIRM_LEFT_RIGHT;
  uint8_t sideButtonLayout = PREV_NEXT;
  uint8_t frontButtonFollowOrientation = 0;
  // Front button remap (logical -> hardware)
  // Used by MappedInputManager to translate logical buttons into physical front buttons.
  uint8_t frontButtonBack = FRONT_HW_BACK;
  uint8_t frontButtonConfirm = FRONT_HW_CONFIRM;
  uint8_t frontButtonLeft = FRONT_HW_LEFT;
  uint8_t frontButtonRight = FRONT_HW_RIGHT;
  // Reader font settings
  uint8_t fontFamily = NOTOSERIF;
  uint8_t fontSize = MEDIUM;
  uint8_t dictionaryFontFamily = DICTIONARY_FONT_READER;
  // 0 follows the reader size; 1-4 map to the four built-in sizes.
  uint8_t dictionaryFontSize = 0;
  uint8_t lineSpacing = NORMAL;
  // Adds 0-40 px to natural word gaps without changing CJK/no-space text.
  uint8_t wordSpacing = 0;
  uint8_t paragraphAlignment = JUSTIFIED;
  // EPUB render mode and Safe Mode are transient per-book overlays. They are
  // persisted only by PerBookReaderSettings, never in global settings.json.
  uint8_t epubRenderMode = static_cast<uint8_t>(EpubRenderMode::Balanced);
  uint8_t epubRenderModeOverride = 0;
  uint8_t epubSafeMode = 0;
  // Auto-sleep timeout setting (default 10 minutes). Legacy sleepTimeout enum values are migration-only.
  uint8_t sleepTimeoutMinutes = 10;
  // E-ink refresh frequency (default 15 pages)
  uint8_t refreshFrequency = REFRESH_15;
  uint8_t hyphenationEnabled = 0;

  // Reader screen margin settings
  uint8_t screenMargin = 5;
  // OPDS download destination folder ("" = SD root). Global; edited from the
  // OPDS server list. Persisted via a category-less SettingInfo::String in
  // SettingsList.h, so it stays out of the on-device Settings screen.
  char opdsDownloadFolder[64] = "";
  // On-disk filename format for OPDS downloads (0=Author-Title default, 1=Title-Author,
  // 2=Title). See OpdsFilenameFormat. Persisted via a category-less SettingInfo::Enum,
  // edited from the OPDS server list; hidden from the on-device Settings screen.
  uint8_t opdsFilenameFormat = 0;
  // Hide battery percentage
  uint8_t hideBatteryPercentage = HIDE_NEVER;
  // "Your Books" display preferences are shared by Recent and All.  The old
  // per-tab fields are intentionally not retained: JsonSettingsIO migrates
  // their values into these two fields before the next save.
  uint8_t libraryView = LIBRARY_LIST;
  uint8_t libraryGrid = LIBRARY_GRID_3X2;
  uint8_t librarySort = LIBRARY_SORT_DATE_ADDED_DESC;
  // Hide plain-text books from the Your Books library (enabled by default).
  uint8_t hideTxtBooks = 1;
  uint8_t homeLayout = HOME_LAYOUT_STYLE_1;
  // Long-press page turn button behavior
  uint8_t longPressButtonBehavior = OFF;
  // Long-press Confirm function while reading (cycles through LONG_PRESS_MENU_FUNCTION values).
  // Defaults to Disabled so shortcut-based bookmark toggling remains opt-in.
  uint8_t longPressMenuFunction = LP_MENU_DISABLED;
  // Local display name shown by the CrossVi Home theme. It does not change
  // protocol identities or Nearby Sync device binding.
  char deviceDisplayName[64] = "";
  // Show the local device name in the Home header.
  uint8_t showDeviceNameOnHome = 1;
  // Sunlight fading compensation
  uint8_t fadingFix = 0;
  // Power button return from footnotes (1 = enabled, 0 = disabled)
  uint8_t pwrBtnFootnoteBack = 1;
  // Use book's embedded CSS styles for EPUB rendering (1 = enabled, 0 = disabled)
  uint8_t embeddedStyle = 1;
  // Focus Reading - emphasizes the first part of words with bold
  uint8_t focusReadingEnabled = 0;
  // SD card font family name (empty = use built-in fontFamily)
  static constexpr size_t SD_FONT_FAMILY_NAME_CAPACITY = 32;
  char sdFontFamilyName[SD_FONT_FAMILY_NAME_CAPACITY] = "";
  static constexpr size_t OTA_VERSION_CAPACITY = 24;
  // Populated only by an explicit OTA check; displaying it never starts Wi-Fi.
  char availableOtaVersion[OTA_VERSION_CAPACITY] = "";
  // Dictionary folder name under /dictionaries (empty = no dictionary)
  char dictionaryName[32] = "";
  // Show hidden files/directories (starting with '.') in the file browser (0 = hidden, 1 = show)
  uint8_t showHiddenFiles = 0;
  // Remove a book from the Recent Books list when its End-of-Book screen is reached (0 = off, 1 = on)
  uint8_t removeReadBooksFromRecents = 0;
  // Move epub to /Read/ folder on SD card when finished (0 = disabled, 1 = enabled)
  uint8_t moveFinishedToReadFolder = 0;
  // Deprecated persisted fields kept for source/binary migration compatibility.
  // Runtime navigation is fixed: short reader Back returns Home and Home Back
  // opens Shortcuts.
  uint8_t backShortToFileBrowser = 0;
  uint8_t homeBackAction = HOME_BACK_SHORTCUTS;
  HomeShortcutList homeShortcuts;
  // Image rendering mode in EPUB reader
  uint8_t imageRendering = IMAGES_DISPLAY;
  // Skip EPUB pages that contain only the declared cover image while reading.
  // This is global because it changes reader navigation, not typography.
  uint8_t skipEpubCoverPage = 1;
  // Tilt-based page turning (X3 only — requires QMI8658 IMU)
  uint8_t tiltPageTurn = TILT_OFF;
  // Language setting (Language enum index, default 0 = EN)
  uint8_t language = 0;
  // Vietnamese-only vocabulary trainer, launched explicitly from Shortcuts.
  uint8_t vocabularyQuizSize = VOCABULARY_QUIZ_10;
  uint8_t vocabularyQuestionTime = VOCABULARY_TIME_15_SECONDS;
  uint8_t vocabularyAnswerCount = VOCABULARY_ANSWERS_3;
  // Quick Resume: keep current content visible with moon icon instead of showing a static sleep screen.
  uint8_t quickResumeSleepScreen = QUICK_RESUME_NEVER;

  ~CrossPointSettings() = default;

  // Get singleton instance
  static CrossPointSettings& getInstance() { return instance; }

  static constexpr uint8_t MIN_SLEEP_TIMEOUT_MINUTES = 1;
  static constexpr uint8_t SLEEP_TIMEOUT_NEVER_MINUTES = 31;
  static constexpr uint8_t MAX_SLEEP_TIMEOUT_MINUTES = SLEEP_TIMEOUT_NEVER_MINUTES;
  // Wake validation is intentionally shorter than the in-app Power hold.
  // It only filters accidental power-on taps during the early boot path.
  static constexpr uint16_t POWER_BUTTON_WAKE_SHORT_MS = 10;
  static constexpr uint16_t POWER_BUTTON_WAKE_LONG_MS = 200;
  // Callback to resolve SD card font IDs. Set by SdCardFontSystem::begin().
  // Returns font ID or 0 if not found.
  using SdFontIdResolver = int (*)(void* ctx, const char* familyName, uint8_t fontSize);
  SdFontIdResolver sdFontIdResolver = nullptr;
  void* sdFontResolverCtx = nullptr;

  uint16_t getPowerButtonDuration() const {
    return (shortPwrBtn == CrossPointSettings::SHORT_PWRBTN::SLEEP) ? POWER_BUTTON_WAKE_SHORT_MS
                                                                    : POWER_BUTTON_WAKE_LONG_MS;
  }
  int getReaderFontId() const;
  int getDictionaryFontId() const;

  // If count_only is true, returns the number of settings items that would be written.
  uint8_t writeSettings(HalFile& file, bool count_only = false) const;

  bool saveToFile() const;
  bool loadFromFile();
  void markReadOnlyForRecovery() { persistenceWritable = false; }
  bool isPersistenceWritable() const { return persistenceWritable; }

  static void validateFrontButtonMapping(CrossPointSettings& settings);
  static uint8_t sleepTimeoutEnumToMinutes(uint8_t legacyValue);

 private:
  bool loadFromBinaryFile();
  bool migrateLanguageBinaryFile();

 public:
  float getReaderLineCompression() const;
  unsigned long getSleepTimeoutMs() const;
  int getRefreshFrequency() const;
};

static_assert(CrossPointSettings::canonicalHomeLayout(2, 0) == CrossPointSettings::HOME_LAYOUT_STYLE_4);
static_assert(CrossPointSettings::canonicalHomeLayout(2, CrossPointSettings::HOME_LAYOUT_VERSION) ==
              CrossPointSettings::HOME_LAYOUT_STYLE_3);
static_assert(CrossPointSettings::canonicalHomeLayout(3, CrossPointSettings::HOME_LAYOUT_VERSION) ==
              CrossPointSettings::HOME_LAYOUT_STYLE_4);
static_assert(CrossPointSettings::canonicalHomeLayout(99, CrossPointSettings::HOME_LAYOUT_VERSION) ==
              CrossPointSettings::HOME_LAYOUT_STYLE_2);
static_assert(!CrossPointSettings::needsSharedCoverThumbnail(CrossPointSettings::HOME_LAYOUT_STYLE_1,
                                                             CrossPointSettings::LIBRARY_LIST));
static_assert(CrossPointSettings::needsSharedCoverThumbnail(CrossPointSettings::HOME_LAYOUT_STYLE_1,
                                                            CrossPointSettings::LIBRARY_COVERS));
static_assert(CrossPointSettings::needsSharedCoverThumbnail(CrossPointSettings::HOME_LAYOUT_STYLE_2,
                                                            CrossPointSettings::LIBRARY_LIST));
static_assert(CrossPointSettings::needsSharedCoverThumbnail(CrossPointSettings::HOME_LAYOUT_STYLE_3,
                                                            CrossPointSettings::LIBRARY_LIST));
static_assert(CrossPointSettings::needsSharedCoverThumbnail(CrossPointSettings::HOME_LAYOUT_STYLE_4,
                                                            CrossPointSettings::LIBRARY_LIST));
static_assert(CrossPointSettings::needsCarouselCoverThumbnail(CrossPointSettings::HOME_LAYOUT_STYLE_4));
static_assert(CrossPointSettings::skipReaderCoverCacheBuild(CrossPointSettings::HOME_LAYOUT_STYLE_1,
                                                            CrossPointSettings::LIBRARY_LIST, true));
static_assert(!CrossPointSettings::skipReaderCoverCacheBuild(CrossPointSettings::HOME_LAYOUT_STYLE_1,
                                                             CrossPointSettings::LIBRARY_COVERS, true));
static_assert(!CrossPointSettings::skipReaderCoverCacheBuild(CrossPointSettings::HOME_LAYOUT_STYLE_1,
                                                             CrossPointSettings::LIBRARY_LIST, false));
static_assert(!CrossPointSettings::skipReaderCoverCacheBuild(CrossPointSettings::HOME_LAYOUT_STYLE_2,
                                                             CrossPointSettings::LIBRARY_LIST, true));
static_assert(!CrossPointSettings::skipReaderCoverCacheBuild(CrossPointSettings::HOME_LAYOUT_STYLE_4,
                                                             CrossPointSettings::LIBRARY_LIST, true));

// Helper macro to access settings
#define SETTINGS CrossPointSettings::getInstance()
