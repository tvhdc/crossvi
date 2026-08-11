#pragma once

struct RecentBook;
struct BookReadingStats;
struct ReadingStatsPresentation;

// Loads only trusted per-book statistics. This avoids pulling device and peer
// aggregates into lightweight consumers such as the sleep-screen summary.
bool loadTrustedBookReadingStats(const RecentBook& recent, BookReadingStats& stats, bool* plainText = nullptr);
bool loadBookStatsPresentation(const RecentBook& recent, ReadingStatsPresentation& presentation);
