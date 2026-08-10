# SD Card Fonts

CrossVi supports loading additional fonts from the SD card, including fonts
with extended Unicode coverage (CJK, Cyrillic, Greek, etc.).

The built-in reader family is Noto Serif at 12, 14, 16 and 18 pt. Every size
uses the corresponding source Regular, Bold, Italic and Bold Italic face rather
than synthesizing styles. Noto Sans remains available as an optional SD-card
font package. The Ubuntu UI fonts use their
small dedicated Vietnamese fallback; reader fonts are not loaded merely to
render menus. The bundled Noto files are distributed under the SIL Open Font
License in `lib/EpdFont/builtinFonts/source/Noto*/OFL.txt`; Ubuntu's terms are
in `lib/EpdFont/builtinFonts/source/Ubuntu/UFL.txt`.

## Installing Fonts

There are three ways to install fonts:

### Option 1: Download from device (recommended)

1. Connect your CrossVi reader to Wi-Fi
2. Go to **Settings > Reader > Manage Fonts**
3. Browse available font families and tap to download
4. Downloaded fonts appear immediately in **Settings > Reader > Font Family**

### Option 2: Upload via web browser

1. Start **File Transfer** and connect through **Join Network** or **Create Hotspot**
2. Open the web interface URL shown on the reader
3. Navigate to the **Fonts** tab
4. Upload `.cpfont` files using the upload form

### Option 3: Manual SD card copy

1. Download font files from the
   [crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts)
2. Copy font family folders to one of two locations on your SD card:

   - `/.fonts/` — hidden directory (preferred; keeps the SD root tidy
     when mounted on a desktop)
   - `/fonts/` — visible directory (use this if your OS hides dot-files
     and you'd rather see the folder in your file manager)

   Both roots are always scanned at boot and the results are merged: a
   family installed in `/fonts/` shows up even when `/.fonts/` also
   exists, and vice versa. The two roots only collide if the same family
   name appears in both — in that case the copy in `/.fonts/` wins and
   the duplicate in `/fonts/` is ignored.

       SD Card Root/
       ├── .fonts/                     ← Hidden root (preferred)
       │   └── Literata/
       │       ├── Literata_12.cpfont
       │       ├── Literata_14.cpfont
       │       ├── Literata_16.cpfont
       │       └── Literata_18.cpfont
       └── fonts/                      ← Visible root (equally valid)
           └── Merriweather/
               ├── Merriweather_12.cpfont
               └── ...

3. Insert the SD card and power on your CrossVi reader

## Available Pre-Built Fonts

The current list of pre-built fonts is maintained in the
[crosspoint-fonts repository](https://github.com/crosspoint-reader/crosspoint-fonts).

## Converting Custom Fonts

To convert your own TrueType/OpenType fonts:

### Prerequisites

    pip install freetype-py fonttools

### Single font (one style)

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      MyFont-Regular.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --style regular \
      --name MyFont \
      --output-dir ./MyFont/

### Multi-style font

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --intervals latin-ext \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

### Vietnamese reading font

`vietnamese-reading` is the recommended single preset for Vietnamese. Unlike
the legacy `vietnamese` subset, it is self-contained: Basic Latin, Vietnamese
precomposed letters (NFC), the combining marks needed by decomposed text (NFD),
common book punctuation, currency symbols and the replacement character are
all included. Selecting it automatically enables strict Vietnamese coverage
checking; `--require-vietnamese` can also be passed explicitly.

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py \
      --regular MyFont-Regular.ttf \
      --bold MyFont-Bold.ttf \
      --italic MyFont-Italic.ttf \
      --bolditalic MyFont-BoldItalic.ttf \
      --fallback-regular NotoSerif-Regular.ttf \
      --fallback-bold NotoSerif-Bold.ttf \
      --fallback-italic NotoSerif-Italic.ttf \
      --fallback-bolditalic NotoSerif-BoldItalic.ttf \
      --intervals vietnamese-reading \
      --sizes 12,14,16,18 \
      --name MyFont \
      --output-dir ./MyFont/

Coverage is checked separately for every style. Missing characters are printed
as `U+XXXX`. A fallback is used only for glyphs absent from the matching source
style; Regular is not silently reused as the bitmap source for Bold or Italic.
If both source and fallback lack a required character, conversion fails. The
converter cannot invent a visually matching Vietnamese glyph.

The families declared in `lib/EpdFont/scripts/sd-fonts.yaml` include this
preset. `build-sd-fonts.py` supplies matching Noto Sans Regular, Bold, Italic
and Bold Italic fallbacks, rather than reusing Regular for every style.

Regular is mandatory. Bold, Italic and Bold Italic are optional; the converter
warns when they are absent and the reader resolves a requested style to the
closest safe style in the file. Font preview reflects the style actually used.

### Available Unicode interval presets

| Preset | Coverage |
|--------|----------|
| `ascii` | U+0020–U+007E (Basic Latin) |
| `latin1` | U+0080–U+00FF (Latin-1 Supplement) |
| `latin-ext` | European languages (Latin + Extended-A/B + punctuation + ligatures) |
| `greek` | Greek + Extended Greek |
| `cyrillic` | Cyrillic + Supplement |
| `hebrew` | Hebrew + Alphabetic Presentation Forms |
| `georgian` | Georgian + Georgian Supplement |
| `armenian` | Armenian |
| `ethiopic` | Ethiopic + Extended |
| `vietnamese` | Vietnamese subset (ơ/ư and combining marks) |
| `vietnamese-reading` | Self-contained Vietnamese reading set (Latin, NFC, NFD marks, punctuation and currency) |
| `punctuation` | General punctuation (U+2000–U+206F) |
| `cjk` | CJK Unified Ideographs + Hiragana + Katakana + Fullwidth |
| `hangul` | Korean Hangul syllables + Jamo + Compatibility Jamo |
| `cherokee` | Cherokee (historic + supplement block) |
| `tifinagh` | Tifinagh |
| `symbols` | Math, currency, arrows, box-drawing, misc symbols, dingbats |
| `reading` | Literary fiction coverage: Latin, Greek, Cyrillic, math/symbol blocks, supplemental punctuation, and CJK quote marks |
| `builtin` | Matches the firmware's built-in font conversion intervals |

Combine presets with commas: `--intervals latin-ext,greek,cyrillic`

You can also specify arbitrary Unicode ranges directly:
`--intervals latin-ext,(0x2100-0x214F)`

To list all presets with codepoint counts:

    python3 lib/EpdFont/scripts/fontconvert_sdcard.py --list-presets

### Additional options

`--force-autohint` — force FreeType's auto-hinter instead of the font's native hinting (useful when a font's built-in hints produce poor results at small sizes).

Install custom fonts via the web interface or manual SD card copy.

Family directory names are limited to 31 ASCII letters, digits, `-` or `_` so
the selected name can be saved and restored without truncation. A `.cpfont`
filename is limited to 96 bytes and must use the same basename characters.
Longer or path-like names are rejected rather than shortened.

## Validation and runtime limits

Before a `.cpfont` is published by the device downloader or Web UI, CrossVi
checks its magic/version, bounded style and table counts, unique style IDs,
section offsets, interval layout, glyph metadata and bitmap ranges using the
same parser used by the renderer. Kerning and ligature lookup tables must also
be complete, unique and strictly sorted. A newer version, truncated read, invalid
offset or missing mandatory Regular style fails closed. Staging and backup
files keep an existing valid font in place until the replacement validates and
publishes successfully. A malformed file copied manually is skipped; it does
not replace the selected built-in fallback or cause a retry loop.

Only one physical `.cpfont` size is loaded for the active family. Glyph pixels
are fetched on demand and bounded caches are reused; the whole file is not read
into RAM. Under **Settings > Reader > Font size**, CrossVi offers 12/14/16/18 pt
with a Vietnamese preview. If that family lacks the exact size, the closest
file is selected and the actual point size is shown without switching family.

TTF/OTF parsing is intentionally host-only: convert fonts before copying them
to the reader. A `.cpfont` can contain only the glyphs supplied by its source or
explicit fallbacks. Font copyright and license remain those of the font author;
do not upload or redistribute fonts whose license does not permit it.
