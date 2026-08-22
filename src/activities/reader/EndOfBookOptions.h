#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "BookReadingStats.h"
#include "util/NextBookFinder.h"

class GfxRenderer;
class MappedInputManager;

struct EndOfBookSummary {
  const std::string& title;
  const std::string& author;
  const BookReadingStats& stats;
  bool statsTrusted;
};

// Shared End-of-Book next-book menu for the EPUB and XTC readers. Collects up to
// MAX_SUGGESTIONS sibling books once per reader session, handles the menu input, and
// draws the end screen with a compact summary of the completed book.
class EndOfBookOptions {
 public:
  enum class Action { None, Redraw, OpenBook, ViewStats, GoHome, LastPage };

  static constexpr size_t MAX_SUGGESTIONS = 3;

  // Start immediately so summary/statistics/Home can render without waiting for
  // a directory scan. stepSuggestions() checks only a bounded number of entries
  // from the main loop and release-publishes the immutable result when done.
  bool start(const std::string& currentBookPath);
  bool stepSuggestions(size_t maxEntries);

  // True when the end-of-book menu is ready and should own the reader's input.
  bool menuActive() const;

  // Menu input handling, following the standard list idiom: side Up/Down and front
  // Left/Right move the selection (wrapping), Confirm opens it (or statistics/Home), and a short
  // Back press returns to the last page of the book. Fills openPath when the result is
  // OpenBook. Returns Action::None when nothing relevant was pressed; callers continue
  // their normal input path (keeping long-press Back to the file browser working).
  Action handleMenuInput(const MappedInputManager& input, std::string* openPath);

  // Draws the completed-book summary and actions onto a cleared buffer.
  void render(GfxRenderer& renderer, const MappedInputManager& input, const EndOfBookSummary& summary) const;

 private:
  std::string folder;
  NextBookFinder::Scan suggestionScan;
  std::atomic<int> selector{0};
  std::atomic<bool> isStarted{false};
  std::atomic<bool> suggestionsReady{false};

  const std::vector<std::string>& names() const;
  std::string fullPath(size_t index) const;
};
