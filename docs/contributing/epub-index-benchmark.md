# EPUB Index Benchmark on X3/X4

This checklist measures real-device behavior without changing the index algorithm or writing benchmark data to the SD
card. Use the same firmware commit, EPUB files, reader settings, and SD card for every comparison.

## Prepare

1. Build and flash the `default` PlatformIO environment (`LOG_LEVEL=2`). Release builds intentionally omit these
   counters and timers.
2. Open a serial monitor and retain lines tagged `IDX` from boot until the book is closed.
3. Copy the fixed, copyright-free corpus below to the SD card. Verify each SHA-256 before testing:

   | Scenario | Repository file | SHA-256 |
   | --- | --- | --- |
   | Many short spine items | `test/epubs/test_br_section_break.epub` | `9b142844ed267471b02a954a4fcede8c831907b9309618f9d242eb67453f51bd` |
   | Large single spine | `test/epubs/benchmark_large_single_spine.epub` | `50d36bc0afe6e81220ac37b466d60319ddacb18548be840fd086dbedf78c6966` |
   | Embedded styles and images | `test/epubs/test_jpeg_images.epub` | `39c05073f6ab789058772fcc33e383bd038e3a84f7f5ad4c66351c7951c60c06` |

   Regenerate the large-spine fixture with `python3 scripts/generate_large_single_spine_epub.py`; its ZIP metadata
   and synthetic Vietnamese text are deterministic.
4. Record the X3/X4 model, SD-card model and filesystem, firmware commit, orientation, font, font size, margins,
   embedded-style setting, and image-rendering setting.
   Start with portrait orientation, Noto Serif Medium, Normal line spacing, 5 px margins, embedded styles enabled,
   images displayed, text anti-aliasing enabled at Normal darkness, and hyphenation disabled. Change only the setting
   named by a scenario.
5. Run each scenario at least five times. Compare medians; also retain the slowest run. Do not compare one cold run
   with one warm run.

## Scenarios

- **Cold metadata:** back up the SD card, use the reader menu's per-book **Delete Book Cache**, then open it. Verify that
  progress, statistics, settings, and clippings remain intact.
- **Warm metadata:** close and reopen the same book without changing settings. The log should report a metadata cache
  hit.
- **Finalized section hit:** reopen a fully built chapter at the saved page.
- **Uncached chapter switch:** move forward and backward across a spine boundary that has not been rendered with the
  current layout.
- **Deep jump:** jump more than 20 pages into an uncached chapter and record the synchronous page count.
- **Partial resume:** open the large single-spine book, allow several pages to build, close it, and reopen first well
  before and then near the saved partial watermark. Confirm that the distant reopen does not immediately rebuild the
  whole spine.
- **Prefetch pressure:** turn pages rapidly until the visible page catches the background builder. Confirm input still
  responds and record any synchronous catch-up.
- **Layout invalidation:** repeat after one font, margin, orientation, or embedded-style change. Progress must remain
  at the corresponding reading position even though section pages are rebuilt.

## Record

For each run, copy these `IDX` fields into the result sheet:

- metadata cache `hit` or `rebuild`, result, and `elapsed_ms`;
- section cache `hit`, `partial`, or `miss`;
- section-visible `elapsed_ms` and whether `switch_wait` was included;
- `sync_pages` and `background_pages`;
- final `total_pages` when the build completes;
- `heap_delta`, `free_heap`, and `min_free_heap`.

Also note time to the first readable page with an external stopwatch, button responsiveness during background work,
unexpected popups, page-position changes, crashes, and reboots. Serial timing alone does not measure e-paper refresh or
human-visible latency.

## Acceptance checks

- No crash, reboot, page loss, or user-data loss in any scenario.
- A partial cache is never shown as the final chapter page count.
- Warm metadata and finalized-section opens report cache hits.
- Background work does not block page turns or menus for a visibly long interval.
- Rebuilding layout does not reset reading progress.
- Results are reported separately for X3 and X4. Do not claim device performance from host tests or firmware build
  success alone.
