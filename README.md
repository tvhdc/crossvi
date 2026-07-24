# CrossVi

[**English**](README.md) | [Tiếng Việt](README.vi.md)

CrossVi is an open-source, multilingual e-reader firmware for the ESP32-C3-based
Xteink X3 and X4. It is independently developed from
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), with
an emphasis on reliable reading, Vietnamese typography, safe SD-card data, and
features that fit the devices' limited resources.

> **Project status:** CrossVi is a development preview. Automated host tests,
> firmware builds, static analysis, and X3/X4 simulator checks are available,
> but a stable release still requires validation on physical X3 and X4 devices.

![CrossVi X4 desktop simulator](docs/images/crossvi-simulator-x4.png)

## Highlights

- EPUB 2/3, plain-text TXT/Markdown, XTC/XTCH, and BMP support.
- Per-book typography, EPUB render modes, EPUB Safe Mode, bookmarks, clippings,
  StarDict lookup, reading statistics, focus reading, and automatic page turns.
- Built-in Vietnamese-capable Noto Serif and Noto Sans fonts, plus validated
  `.cpfont` families loaded from the SD card.
- File browser, recent books, Web UI transfers, WebDAV, Calibre Wireless, OPDS,
  KOReader progress sync, and OTA updates.
- Themes designed for the 4-inch display, configurable controls, sleep screens,
  screenshots, and tilt page turns on X3.
- 30 interface languages, including a complete Vietnamese interface for
  CrossVi-specific features.
- Experimental Nearby Sync for confirmed position or statistics exchange
  between two nearby CrossVi readers. Nearby traffic is not encrypted.

CrossVi keeps the reader focused: it does not aim to add web browsing, media
playback, games, or other background-heavy applications.

## Supported hardware and formats

| Device | Status | Notes |
| --- | --- | --- |
| Xteink X3 | Supported target | ESP32-C3; tilt page turn available |
| Xteink X4 | Supported target | ESP32-C3 |

| Format | Support |
| --- | --- |
| `.epub` | Reflowable EPUB 2/3 reader |
| `.txt`, `.md` | Plain-text reader; Markdown is currently treated as text |
| `.xtc`, `.xtch` | Tested uncompressed v1.0 fixed-layout subset using 480×800 pages |
| `.bmp` | Image viewer and sleep-screen images |

XTC/XTCH pages display 1:1 on X4 and are fitted and centred on X3. They do not
provide EPUB typography, dictionary selection, clippings, or KOReader position
sync. See the [XTC/XTCH format contract](lib/Xtc/README).

## Typography and custom fonts

The built-in Noto Serif and Noto Sans reader families provide Regular, Bold,
Italic, and Bold Italic faces at 12, 14, 16, and 18 pt.

Compatible SD-card `.cpfont` families can additionally provide 20, 22, 24, 26,
and 28 pt. The size picker only shows sizes available for the selected family,
and the reader loads one physical `.cpfont` file at a time. Large font files are
not bundled into the firmware, so a fresh installation still exposes only the
built-in sizes until a compatible SD font family is installed.

To create or install a font family:

1. Open the [CrossPoint font builder](https://crosspointreader.com/fonts), or use
   `lib/EpdFont/scripts/fontconvert_sdcard.py` locally.
2. Convert TTF/OTF sources to `.cpfont`; TTF/OTF files are not parsed on-device.
3. Copy the generated family to `/fonts/FamilyName/` or
   `/.fonts/FamilyName/` on the SD card.
4. Select it under **Settings → Reader → Font Family**.

Use the `vietnamese-reading` preset when preparing Vietnamese fonts. It checks
NFC letters, required NFD combining marks, punctuation, and every emitted style.
The source font's licence continues to apply to every generated `.cpfont` file.

## Installation

### Important safety notice

Some devices sold by third parties have USB flashing locked. The public Xteink
Unlocker currently supports CrossPoint and CrossInk as unlock payloads, not
CrossVi. **Do not use CrossVi as the unlock payload on a USB-locked device**;
doing so may leave the device without a supported recovery path.

CrossVi does not currently publish a hardware-validated stable release. If you
intentionally test a development build, back up the SD card first and use a
`firmware.bin` from a trusted build or build it locally.

### Web flasher

1. Wake the reader and connect it with a data-capable USB-C cable.
2. Open the [CrossPoint flash tools](https://crosspointreader.com/#flash-tools).
3. Select X3 or X4, choose **Custom .bin**, and open CrossVi's `firmware.bin`.
4. Keep the reader connected until flashing completes.

### Command line

With Python 3.10 or newer, install the latest
[`esptool`](https://github.com/espressif/esptool), then flash the app image at
`0x10000`:

```bash
python3 -m pip install --upgrade esptool
esptool --chip esp32c3 --port /dev/ttyACM0 --baud 921600 \
  write-flash 0x10000 /path/to/firmware.bin
```

Replace `/dev/ttyACM0` with the device port on your system. Official CrossPoint
firmware can be restored through the same web flasher.

## SD-card data

CrossVi retains CrossPoint-compatible working data under `/.crosspoint` where
supported. This directory contains both generated caches and important reader
data such as settings, positions, bookmarks, clippings, and statistics.

Do not delete the entire directory as routine troubleshooting. Use the
firmware's cache-clear action first, and always back up the SD card before
switching firmware or manually changing reader data. See
[file formats](docs/file-formats.md) for the on-card layout.

## Development

### Prerequisites

- [pioarduino](https://github.com/pioarduino/pioarduino), or VS Code with its
  extension
- Python 3.8 or newer
- `clang-format` 21 for formatting contributions
- A data-capable USB-C cable for physical-device work

### Build

```bash
git clone --recursive https://github.com/tvhdc/crossvi.git
cd crossvi
pio run
```

If the repository was cloned without submodules:

```bash
git submodule update --init --recursive
```

Run the main contributor checks with:

```bash
./bin/clang-format-fix
pio check --fail-on-defect low --fail-on-defect medium --fail-on-defect high
pio run
```

### X3/X4 desktop simulator

The simulator runs the real CrossVi interface and renderer without a physical
reader:

```bash
python3 scripts/run_simulator.py x3
python3 scripts/run_simulator.py x4
```

It validates desktop-visible behaviour, not e-paper ghosting, SD-card timing,
radio behaviour, battery use, or peak memory on real hardware. See the
[simulator guide](docs/contributing/simulator.md) and
[hardware validation checklist](docs/contributing/hardware-validation.md).

## Documentation

- [User Guide](USER_GUIDE.md)
- [Contributing Guide](docs/contributing/README.md)
- [Architecture](docs/contributing/architecture.md)
- [Testing and Debugging](docs/contributing/testing-debugging.md)
- [Web Server](docs/webserver.md)

## Contributing and support

Focused-reading contributions are welcome. Before starting a large
change, open an [Ideas discussion](https://github.com/tvhdc/crossvi/discussions/categories/ideas)
so work is not duplicated.

- Report reproducible bugs through [GitHub Issues](https://github.com/tvhdc/crossvi/issues).
- Ask questions and propose features in [GitHub Discussions](https://github.com/tvhdc/crossvi/discussions).
- Read [GOVERNANCE.md](GOVERNANCE.md) before contributing.

## Upstream, credits, and license

CrossVi is an independent fork of
[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).
CrossPoint and its contributors remain the technical foundation of this project.
Some reading-experience ideas were informed by
[CrossInk](https://github.com/uxjulia/CrossInk) and independently adapted for
CrossVi's resource and compatibility constraints.

The project is distributed under the [MIT License](LICENSE) while preserving
upstream copyright and attribution. Font files retain their own licences.
CrossVi is not affiliated with Xteink or any device manufacturer.
