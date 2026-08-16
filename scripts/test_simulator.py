#!/usr/bin/env python3
"""Build and smoke-test the CrossVi X3/X4 desktop simulator."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import unicodedata
import zipfile
import zlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build" / "simulator-tests"
GOLDEN_PATH = ROOT / "simulator" / "tests" / "golden_home.json"
XTC_FIXTURES = ROOT / "test" / "xtc" / "resources"
TYPOGRAPHY_LINES = (
    "Kiểm thử chữ Việt",
    "Tiếng Việt: Trường học, cộng đồng, kỹ thuật, Nguyễn Nhật Ánh.",
    "Đậm: Ă Â Ê Ô Ơ Ư Đ — ă â ê ô ơ ư đ.",
    "Nghiêng: à á ả ã ạ — ằ ắ ẳ ẵ ặ — ề ế ể ễ ệ.",
    "Đậm nghiêng: ờ ớ ở ỡ ợ — ừ ứ ử ữ ự — ỳ ý ỷ ỹ ỵ.",
)


def platformio() -> str:
    executable = shutil.which("pio") or shutil.which("platformio")
    if executable:
        return executable
    bundled = Path.home() / ".platformio" / "penv" / "bin" / "pio"
    if bundled.exists():
        return str(bundled)
    raise RuntimeError("PlatformIO is missing")


def run(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
    print("+", shlex.join(command))
    return subprocess.run(command, cwd=ROOT, text=True, check=True, **kwargs)


def compile_and_run_test(name: str, sources: list[str], flags: list[str]) -> None:
    binary = BUILD / name
    run(
        [
            shutil.which("g++") or "g++",
            "-std=gnu++20",
            "-Isimulator/src",
            *sources,
            "-o",
            str(binary),
            *flags,
        ]
    )
    run([str(binary)])


def test_host_adapters() -> None:
    flags = shlex.split(
        subprocess.check_output([sys.executable, "scripts/simulator_build_flags.py"], cwd=ROOT, text=True)
    )
    BUILD.mkdir(parents=True, exist_ok=True)
    compile_and_run_test(
        "controls-test",
        ["simulator/tests/SimulatorControlsTest.cpp", "simulator/src/SimulatorControls.cpp"],
        flags,
    )
    compile_and_run_test(
        "input-test",
        [
            "simulator/tests/SimulatorInputTest.cpp",
            "simulator/src/HalGPIO.cpp",
            "simulator/src/SimulatorControls.cpp",
            "simulator/src/SimulatorLifecycle.cpp",
        ],
        flags,
    )
    compile_and_run_test(
        "storage-test",
        ["simulator/tests/SimulatorStorageTest.cpp", "simulator/src/HalStorage.cpp"],
        flags,
    )


def bmp_dimensions(path: Path) -> tuple[int, int]:
    header = path.read_bytes()[:26]
    if len(header) < 26 or header[:2] != b"BM":
        raise AssertionError(f"Not a BMP screenshot: {path}")
    width, height = struct.unpack_from("<ii", header, 18)
    return width, abs(height)


def copy_bmp_as_top_down(source: Path, destination: Path) -> None:
    """Mirror the row-order contract used by production cover thumbnails."""
    data = bytearray(source.read_bytes())
    if len(data) < 54 or data[:2] != b"BM":
        raise AssertionError(f"Not a BMP cover fixture: {source}")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    compression = struct.unpack_from("<I", data, 30)[0]
    height = abs(signed_height)
    row_bytes = (width * bits_per_pixel + 31) // 32 * 4
    pixel_bytes = row_bytes * height
    if width <= 0 or signed_height == 0 or compression != 0 or pixel_offset + pixel_bytes > len(data):
        raise AssertionError(f"Unsupported BMP cover fixture: {source}")
    if signed_height > 0:
        rows = [data[pixel_offset + row * row_bytes : pixel_offset + (row + 1) * row_bytes] for row in range(height)]
        data[pixel_offset : pixel_offset + pixel_bytes] = b"".join(reversed(rows))
        struct.pack_into("<i", data, 22, -height)
    destination.write_bytes(data)


def bmp_grayscale_pixels(path: Path) -> tuple[int, int, list[int]]:
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        raise AssertionError(f"Not a BMP screenshot: {path}")
    pixel_offset = struct.unpack_from("<I", data, 10)[0]
    width, signed_height = struct.unpack_from("<ii", data, 18)
    bits_per_pixel = struct.unpack_from("<H", data, 28)[0]
    if width <= 0 or signed_height == 0 or bits_per_pixel != 32:
        raise AssertionError(f"Unexpected simulator BMP layout: {path}")
    height = abs(signed_height)
    row_bytes = width * 4
    if pixel_offset + row_bytes * height > len(data):
        raise AssertionError(f"Truncated simulator BMP: {path}")
    pixels = [0] * (width * height)
    for logical_y in range(height):
        stored_y = logical_y if signed_height < 0 else height - 1 - logical_y
        row = pixel_offset + stored_y * row_bytes
        for x in range(width):
            blue, green, red, _ = data[row + x * 4 : row + x * 4 + 4]
            if red != green or green != blue:
                raise AssertionError(f"Non-grayscale simulator pixel in {path}")
            pixels[logical_y * width + x] = red
    return width, height, pixels


def create_typography_epub(path: Path, lines: tuple[str, ...]) -> None:
    container = (
        '<?xml version="1.0"?>'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
        '<rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles></container>'
    )
    package = (
        '<?xml version="1.0"?>'
        '<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid">'
        '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
        '<dc:title>Vietnamese Typography</dc:title><dc:creator>CrossVi Tests</dc:creator>'
        '<dc:language>vi</dc:language><dc:identifier id="bookid">crossvi-typography</dc:identifier>'
        '</metadata><manifest><item id="chapter" href="chapter.xhtml" '
        'media-type="application/xhtml+xml"/></manifest><spine><itemref idref="chapter"/></spine></package>'
    )
    chapter = (
        '<?xml version="1.0" encoding="UTF-8"?>'
        '<html xmlns="http://www.w3.org/1999/xhtml"><head><title>Typography</title></head><body>'
        f'<h1>{lines[0]}</h1><p>{lines[1]}</p><p><strong>{lines[2]}</strong></p>'
        f'<p><em>{lines[3]}</em></p><p><strong><em>{lines[4]}</em></strong></p>'
        '</body></html>'
    )
    with zipfile.ZipFile(path, "w") as book:
        book.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        book.writestr("META-INF/container.xml", container, compress_type=zipfile.ZIP_DEFLATED)
        book.writestr("OEBPS/content.opf", package, compress_type=zipfile.ZIP_DEFLATED)
        book.writestr("OEBPS/chapter.xhtml", chapter, compress_type=zipfile.ZIP_DEFLATED)


def install_sd_font_family(sd: Path, family_dir: Path) -> str:
    destination = sd / ".fonts" / family_dir.name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(family_dir, destination)
    return family_dir.name


def write_per_book_reader_settings(
    path: Path, *, custom: bool, font_size: int, sd_font_family: str,
    font_family: int = 0, line_spacing: int = 1, screen_margin: int = 5, version: int = 4,
) -> None:
    payload = bytearray(49 if version >= 6 else 48)
    payload[0] = 1 if custom else 0
    payload[1] = font_family
    payload[2] = font_size
    payload[3] = line_spacing
    payload[4] = 0
    payload[5] = 0
    payload[6] = screen_margin
    payload[7] = 1
    payload[10] = 1
    payload[11] = 1
    encoded_name = sd_font_family.encode("utf-8")
    if len(encoded_name) >= 32:
        raise AssertionError("Simulator per-book font fixture name is too long")
    payload[14:14 + len(encoded_name)] = encoded_name
    payload[47] = 1  # EpubRenderMode::Balanced when no per-book render override is present.
    path.write_bytes(
        b"CVRS" + bytes([version]) + struct.pack("<H", len(payload)) + struct.pack("<I", zlib.crc32(payload)) + payload
    )


def read_per_book_reader_settings(path: Path) -> tuple[int, int, int, int, str]:
    data = path.read_bytes()
    version = data[4] if len(data) > 4 else 0
    payload_size = 49 if version >= 6 else 48
    if (
        len(data) != 11 + payload_size
        or data[:4] != b"CVRS"
        or version not in (4, 5, 6)
        or struct.unpack_from("<H", data, 5)[0] != payload_size
    ):
        raise AssertionError(f"Invalid per-book reader settings fixture: {path}")
    payload = data[11:]
    if zlib.crc32(payload) != struct.unpack_from("<I", data, 7)[0]:
        raise AssertionError(f"Invalid per-book reader settings CRC: {path}")
    family_name = payload[14:46].split(b"\0", 1)[0].decode("utf-8")
    return payload[0], payload[1], payload[2], payload[3], family_name


def read_per_book_screen_margin(path: Path) -> tuple[int, int]:
    data = path.read_bytes()
    version = data[4] if len(data) > 4 else 0
    payload_size = 49 if version >= 6 else 48
    if (
        len(data) != 11 + payload_size
        or data[:4] != b"CVRS"
        or version not in (5, 6)
        or struct.unpack_from("<H", data, 5)[0] != payload_size
    ):
        raise AssertionError(f"Invalid v5 per-book reader settings fixture: {path}")
    payload = data[11:]
    if zlib.crc32(payload) != struct.unpack_from("<I", data, 7)[0]:
        raise AssertionError(f"Invalid per-book reader settings CRC: {path}")
    return payload[0], payload[6]


def reset_simulator_navigation(control: Path) -> None:
    for name in ("state.json", "state.json.bak", "recent.json", "recent.json.bak"):
        (control / name).unlink(missing_ok=True)


def prime_book_cache(device: str, sd: Path, book_format: str) -> Path:
    control = sd / ".crosspoint"
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM",
            "CROSSVI_SIM_EXIT_AFTER_MS": "4800",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=12)
    log = completed.stdout + completed.stderr
    expected_activity = "TxtReader" if book_format == "txt" else "EpubReader"
    if f"Entering activity: {expected_activity}" not in log:
        raise AssertionError(f"{device.upper()} could not prime the {book_format.upper()} cache:\n{log}")
    candidates = list(control.glob(f"{book_format}_*/source_identity.bin"))
    if len(candidates) != 1:
        raise AssertionError(f"{device.upper()} found {len(candidates)} {book_format.upper()} cache identities")
    reset_simulator_navigation(control)
    return candidates[0].parent


def run_typography_case(
    device: str, book_format: str, normalization: str, font_family: int = 0, font_size: int = 1,
    sd_font_family: Path | None = None,
) -> tuple[list[int], str]:
    family_key = sd_font_family.name if sd_font_family else str(font_family)
    output = BUILD / f"{device}-typography-{family_key}-{font_size}-{book_format}-{normalization.lower()}"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    shots.mkdir(parents=True)
    settings = {
        "language": "VI",
        "fontFamily": font_family,
        "fontSize": font_size,
        "lineSpacing": 1,
        "paragraphAlignment": 1,
        "screenMargin": 5,
        "embeddedStyle": 1,
        "hyphenationEnabled": 0,
        "textAntiAliasing": 1,
        "statusBarChapterPageCount": 0,
        "statusBarProgressBar": 2,
        "statusBarTitle": 2,
        "statusBarBattery": 0,
    }
    if sd_font_family:
        settings["sdFontFamilyName"] = install_sd_font_family(sd, sd_font_family)
    (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
    lines = tuple(unicodedata.normalize(normalization, line) for line in TYPOGRAPHY_LINES)
    if book_format == "txt":
        (sd / "typography.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
        expected_activity = "TxtReader"
    elif book_format == "epub":
        create_typography_epub(sd / "typography.epub", lines)
        expected_activity = "EpubReader"
    else:
        raise AssertionError(f"Unsupported typography fixture: {book_format}")

    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM,5200:SCREENSHOT",
            "CROSSVI_SIM_EXIT_AFTER_MS": "6200",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    if f"Entering activity: {expected_activity}" not in log:
        raise AssertionError(f"{device.upper()} did not open the {book_format.upper()} typography fixture:\n{log}")
    missed = [int(value) for value in re.findall(r"(\d+) missed\)", log)]
    # Built-ins report explicit prewarm counts. SD fonts prewarm through their
    # bounded on-demand cache and do not emit the same summary line.
    if (sd_font_family is None and not missed) or any(missed):
        raise AssertionError(f"{device.upper()} {book_format.upper()} {normalization} missed glyphs: {missed}\n{log}")
    _, _, pixels = bmp_grayscale_pixels(find_single(shots, "*.bmp"))
    return pixels, log


def smoke_large_sd_font_typography(device: str, family_dir: Path) -> None:
    size20, log20 = run_typography_case(device, "txt", "NFC", font_size=4, sd_font_family=family_dir)
    size28, log28 = run_typography_case(device, "txt", "NFD", font_size=8, sd_font_family=family_dir)
    for point_size, log in ((20, log20), (28, log28)):
        if not re.search(rf"Loaded .* size={point_size}\b", log):
            raise AssertionError(f"{device.upper()} did not load the physical {point_size} pt SD font")
    if not any(pixel < 255 for pixel in size20) or not any(pixel < 255 for pixel in size28):
        raise AssertionError(f"{device.upper()} large SD font rendered no text")

    normal, normal_log = run_typography_case(device, "epub", "NFC", font_size=8, sd_font_family=family_dir)
    decomposed, _ = run_typography_case(device, "epub", "NFD", font_size=8, sd_font_family=family_dir)
    if normal != decomposed:
        raise AssertionError(f"{device.upper()} 28 pt SD font differs for NFC/NFD text")
    if not re.search(r"Loaded .* size=28\b", normal_log):
        raise AssertionError(f"{device.upper()} EPUB did not use the 28 pt SD font")
    print(f"{device.upper()}: 20/28 pt SD font content with four styles smoke passed")


def smoke_vietnamese_typography(device: str) -> None:
    if unicodedata.normalize("NFC", TYPOGRAPHY_LINES[1]) == unicodedata.normalize("NFD", TYPOGRAPHY_LINES[1]):
        raise AssertionError("Vietnamese typography fixture does not exercise normalization")

    txt_nfc, txt_nfc_log = run_typography_case(device, "txt", "NFC")
    txt_nfd, txt_nfd_log = run_typography_case(device, "txt", "NFD")
    txt_nfc_pages = re.search(r"Built page index: (\d+)(?: known)? pages", txt_nfc_log)
    txt_nfd_pages = re.search(r"Built page index: (\d+)(?: known)? pages", txt_nfd_log)
    if not txt_nfc_pages or not txt_nfd_pages or txt_nfc_pages.group(1) != txt_nfd_pages.group(1):
        raise AssertionError(f"{device.upper()} TXT NFC/NFD pagination differs")
    if not any(pixel < 255 for pixel in txt_nfc) or not any(pixel < 255 for pixel in txt_nfd):
        raise AssertionError(f"{device.upper()} TXT typography fixture rendered no ink")

    normal, _ = run_typography_case(device, "epub", "NFC")
    decomposed, _ = run_typography_case(device, "epub", "NFD")
    if normal != decomposed:
        raise AssertionError(f"{device.upper()} EPUB NFC/NFD typography differs after normalization")

    legacy_sans_txt, _ = run_typography_case(device, "txt", "NFC", font_family=1)
    legacy_sans_epub, _ = run_typography_case(device, "epub", "NFD", font_family=1)
    if legacy_sans_txt != txt_nfc or legacy_sans_epub != decomposed:
        raise AssertionError(f"{device.upper()} legacy Noto Sans setting did not fall back to Noto Serif")
    print(f"{device.upper()}: Noto Serif typography and legacy Noto Sans fallback smoke passed")


def xtg_source(path: Path) -> tuple[int, int, bytes]:
    data = path.read_bytes()
    table_offset = struct.unpack_from("<Q", data, 24)[0]
    page_offset, page_size, width, height = struct.unpack_from("<QIHH", data, table_offset)
    if data[page_offset : page_offset + 4] != b"XTG\0" or page_size < 22:
        raise AssertionError(f"Not an uncompressed XTG fixture: {path}")
    return width, height, data[page_offset + 22 : page_offset + page_size]


def create_navigation_fixture(source: Path, destination: Path) -> None:
    """Derive a small multi-page/chapter fixture from the real converter output."""
    width, height, first_payload = xtg_source(source)
    page_count = 3
    header_size = 56
    metadata_size = 256
    chapter_size = 96
    chapter_count = 2
    page_header_size = 22
    table_entry_size = 16
    metadata_offset = header_size
    chapter_offset = metadata_offset + metadata_size
    table_offset = chapter_offset + chapter_count * chapter_size
    data_offset = table_offset + page_count * table_entry_size
    page_size = page_header_size + len(first_payload)
    book = bytearray(data_offset + page_count * page_size)

    struct.pack_into("<IBBHBBBBIQQQQQ", book, 0, 0x00435458, 1, 0, page_count, 0, 1, 0, 1, 1,
                     metadata_offset, table_offset, data_offset, 0, chapter_offset)
    book[metadata_offset : metadata_offset + 27] = b"CrossVi navigation fixture\0"
    book[metadata_offset + 128 : metadata_offset + 143] = b"CrossVi tests\0\0"
    struct.pack_into("<H", book, metadata_offset + 196, chapter_count)
    chapters = (("Chapter 1", 1, 2), ("Chapter 2", 3, 3))
    for index, (name, start, end) in enumerate(chapters):
        offset = chapter_offset + index * chapter_size
        encoded = name.encode("ascii")
        book[offset : offset + len(encoded)] = encoded
        struct.pack_into("<HH", book, offset + 0x50, start, end)

    payloads = (
        first_payload,
        bytes(value ^ 0xFF for value in first_payload),
        bytes(0x00 if (index // 60) % 2 == 0 else 0xFF for index in range(len(first_payload))),
    )
    for index, payload in enumerate(payloads):
        offset = data_offset + index * page_size
        struct.pack_into("<QIHH", book, table_offset + index * table_entry_size, offset, page_size, width, height)
        struct.pack_into("<IHHBBIQ", book, offset, 0x00475458, width, height, 0, 0, len(payload), 0)
        book[offset + page_header_size : offset + page_size] = payload
    destination.write_bytes(book)


def invert_xtg_pages(path: Path) -> None:
    book = bytearray(path.read_bytes())
    page_count = struct.unpack_from("<H", book, 6)[0]
    table_offset = struct.unpack_from("<Q", book, 24)[0]
    for index in range(page_count):
        page_offset, page_size = struct.unpack_from("<QI", book, table_offset + index * 16)
        payload_offset = page_offset + 22
        for offset in range(payload_offset, page_offset + page_size):
            book[offset] ^= 0xFF
    path.write_bytes(book)


def assert_xtg_mapping(device: str, fixture: Path, screenshot: Path) -> None:
    source_width, source_height, payload = xtg_source(fixture)
    width, height, pixels = bmp_grayscale_pixels(screenshot)
    if device == "x4":
        viewport = (0, 0, 480, 800)
    else:
        viewport = (26, 0, 475, 792)
    view_x, view_y, view_width, view_height = viewport
    if (width, height) != ((480, 800) if device == "x4" else (528, 792)):
        raise AssertionError(f"Unexpected {device.upper()} XTC screenshot size: {(width, height)}")

    row_bytes = (source_width + 7) // 8
    for y in range(height):
        for x in range(width):
            if x < view_x or x >= view_x + view_width or y < view_y or y >= view_y + view_height:
                expected = 255
            else:
                source_x = (x - view_x) * (source_width - 1) // (view_width - 1)
                source_y = (y - view_y) * (source_height - 1) // (view_height - 1)
                bit = (payload[source_y * row_bytes + source_x // 8] >> (7 - source_x % 8)) & 1
                expected = 255 if bit else 0
            if pixels[y * width + x] != expected:
                raise AssertionError(
                    f"{device.upper()} XTG mapping differs at ({x}, {y}): "
                    f"{pixels[y * width + x]} != {expected}"
                )


def find_single(path: Path, pattern: str) -> Path:
    matches = list(path.glob(pattern))
    if len(matches) != 1:
        raise AssertionError(f"Expected one {pattern} under {path}, found {len(matches)}")
    return matches[0]


def smoke_xtc_fixture(device: str, fixture_name: str) -> None:
    suffix = Path(fixture_name).suffix[1:]
    output = BUILD / f"{device}-{suffix}"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    (sd / ".crosspoint").mkdir()
    (sd / ".crosspoint" / "settings.json").write_text('{"sleepScreen":3}\n', encoding="utf-8")
    fixture = XTC_FIXTURES / fixture_name
    shutil.copy2(fixture, sd / f"converter.{suffix}")

    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM,4200:SCREENSHOT,5000:POWER:800",
            "CROSSVI_SIM_EXIT_ON_SLEEP": "1",
            "CROSSVI_SIM_EXIT_AFTER_MS": "7500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    expected_pages = "Opened file: /converter." + suffix + " (1 pages, 480x800)"
    if expected_pages not in log or "Entering activity: XtcReader" not in log:
        raise AssertionError(f"{device.upper()} did not open {suffix.upper()} through File Browser:\n{log}")
    if "Rendering sleep cover:" not in log:
        raise AssertionError(f"{device.upper()} did not render the {suffix.upper()} sleep cover:\n{log}")

    screenshot = find_single(shots, "*.bmp")
    find_single(shots, "*.framebuffer.bin")
    if suffix == "xtc":
        assert_xtg_mapping(device, fixture, screenshot)
    else:
        _, _, grayscale = bmp_grayscale_pixels(screenshot)
        levels = set(grayscale)
        if len(levels) < 4:
            raise AssertionError(f"{device.upper()} XTCH screenshot exposed only {sorted(levels)}")

    progress = find_single(sd / ".crosspoint", "xtc_*/progress.bin")
    if progress.read_bytes() != b"\0\0\0\0":
        raise AssertionError(f"{device.upper()} saved the wrong first-page progress for {suffix.upper()}")
    find_single(sd / ".crosspoint", "xtc_*/source_identity.bin")
    generated_cover = find_single(sd / ".crosspoint", "xtc_*/cover.bmp")
    recent = json.loads((sd / ".crosspoint" / "recent.json").read_text(encoding="utf-8"))
    recent_paths = [entry.get("path") for entry in recent.get("books", [])]
    if f"/converter.{suffix}" not in recent_paths:
        raise AssertionError(f"{device.upper()} did not retain {suffix.upper()} in Recent Books")

    # Home / Your Books now consume an existing thumbnail progressively; they do
    # not synchronously generate one while holding the render lock. Seed the
    # fixture from the cover produced by the reader so this smoke continues to
    # exercise the cached-cover render path without reintroducing that stall.
    shutil.copy2(generated_cover, generated_cover.parent / "thumb_168.bmp")

    resume_shots = output / "resume-screenshots"
    resume_shots.mkdir()
    resume_environment = os.environ.copy()
    resume_environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(resume_shots),
            "CROSSVI_SIM_INPUT_SCRIPT": "2500:SCREENSHOT,3200:BACK,5200:SCREENSHOT",
            "CROSSVI_SIM_EXIT_AFTER_MS": "6200",
        }
    )
    resumed = run([str(binary)], env=resume_environment, capture_output=True, timeout=15)
    resume_log = resumed.stdout + resumed.stderr
    if "Loaded progress: page 0" not in resume_log or "Entering activity: Home" not in resume_log:
        raise AssertionError(f"{device.upper()} did not reload {suffix.upper()} progress and return Home:\n{resume_log}")
    find_single(sd / ".crosspoint", "xtc_*/thumb_*.bmp")
    if len(list(resume_shots.glob("*.bmp"))) != 2:
        raise AssertionError(f"{device.upper()} did not capture reader and Home after {suffix.upper()} resume")
    print(f"{device.upper()}: {suffix.upper()} File Browser/open/progress/cached-cover/sleep smoke passed")


def smoke_xtc_status_modes(device: str) -> None:
    hidden_shot = find_single(BUILD / f"{device}-xtc" / "screenshots", "*.bmp")
    width, height, hidden_pixels = bmp_grayscale_pixels(hidden_shot)
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    for mode, expected_region in ((1, "bottom"), (2, "top")):
        output = BUILD / f"{device}-xtc-status-{expected_region}"
        if output.exists():
            shutil.rmtree(output)
        sd = output / "sd"
        shots = output / "screenshots"
        sd.mkdir(parents=True)
        shots.mkdir(parents=True)
        (sd / ".crosspoint").mkdir()
        (sd / ".crosspoint" / "settings.json").write_text(
            json.dumps({"xtcStatusBarMode": mode}) + "\n", encoding="utf-8"
        )
        shutil.copy2(XTC_FIXTURES / "crossvi-converter-480x800.xtc", sd / "status.xtc")
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
                "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM,4200:SCREENSHOT",
                "CROSSVI_SIM_EXIT_AFTER_MS": "5000",
            }
        )
        completed = run([str(binary)], env=environment, capture_output=True, timeout=10)
        log = completed.stdout + completed.stderr
        if "Rendered page 1/1 (1-bit)" not in log:
            raise AssertionError(f"{device.upper()} did not render XTC with {expected_region} status bar:\n{log}")
        status_width, status_height, status_pixels = bmp_grayscale_pixels(find_single(shots, "*.bmp"))
        if (status_width, status_height) != (width, height):
            raise AssertionError(f"{device.upper()} status mode changed the logical viewport")
        changed_rows = {
            index // width
            for index, (hidden, status) in enumerate(zip(hidden_pixels, status_pixels))
            if hidden != status
        }
        if not changed_rows:
            raise AssertionError(f"{device.upper()} {expected_region} status bar did not alter the frame")
        if expected_region == "top" and max(changed_rows) >= 100:
            raise AssertionError(f"{device.upper()} top status bar changed page mapping below its overlay")
        if expected_region == "bottom" and min(changed_rows) < height - 100:
            raise AssertionError(f"{device.upper()} bottom status bar changed page mapping above its overlay")
    print(f"{device.upper()}: XTC hidden/top/bottom status-bar mapping smoke passed")


def smoke_xtc_navigation(device: str) -> None:
    output = BUILD / f"{device}-xtc-navigation"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    fixture = sd / "navigation.xtc"
    create_navigation_fixture(XTC_FIXTURES / "crossvi-converter-480x800.xtc", fixture)

    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:CONFIRM,2600:CONFIRM,4000:SCREENSHOT,4500:RIGHT,5500:SCREENSHOT,"
                "6000:LEFT,7000:SCREENSHOT,7500:CONFIRM,8500:CONFIRM,9500:RIGHT,"
                "10500:CONFIRM,12000:SCREENSHOT,13000:POWER:800"
            ),
            "CROSSVI_SIM_EXIT_ON_SLEEP": "1",
            "CROSSVI_SIM_EXIT_AFTER_MS": "15500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=20)
    log = completed.stdout + completed.stderr
    required = (
        "Opened file: /navigation.xtc (3 pages, 480x800)",
        "Rendered page 1/3 (1-bit)",
        "Rendered page 2/3 (1-bit)",
        "Entering activity: EpubReaderMenu",
        "Entering activity: XtcReaderChapterSelection",
        "Rendered page 3/3 (1-bit)",
    )
    missing = [item for item in required if item not in log]
    if missing:
        raise AssertionError(f"{device.upper()} XTC navigation missed {missing}:\n{log}")
    progress = find_single(sd / ".crosspoint", "xtc_*/progress.bin")
    if progress.read_bytes() != struct.pack("<I", 2):
        raise AssertionError(f"{device.upper()} did not save the displayed chapter-jump page")
    screenshots = sorted(shots.glob("*.bmp"))
    if len(screenshots) != 4:
        raise AssertionError(f"{device.upper()} XTC navigation produced {len(screenshots)} screenshots")
    digests = [hashlib.sha256(path.read_bytes()).hexdigest() for path in screenshots]
    if len(set(digests)) != 3 or digests[0] != digests[2]:
        raise AssertionError(f"{device.upper()} forward/back/chapter screenshots do not match their pages")
    print(f"{device.upper()}: XTC forward/back/chapter/progress smoke passed")


def smoke_xtc_saved_items(device: str) -> None:
    output = BUILD / f"{device}-xtc-saved-items"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    shots.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI"}) + "\n", encoding="utf-8"
    )
    fixture = sd / "saved.xtc"
    create_navigation_fixture(XTC_FIXTURES / "crossvi-converter-480x800.xtc", fixture)

    # Fixed-layout menu: chapter, Home, bookmark, Saved. Toggle page one's
    # bookmark, reopen the menu, then enter Saved. Right moves focus from the
    # sole Bookmark tab to its first row; it must not expose text highlighting.
    events = (
        "1200:CONFIRM,2600:CONFIRM,4200:CONFIRM,4800:DOWN,5400:DOWN,6000:CONFIRM,"
        "7200:CONFIRM,7800:DOWN,8400:DOWN,9000:DOWN,9600:CONFIRM,"
        "10800:SCREENSHOT,11400:RIGHT,12200:SCREENSHOT"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "13000",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=20)
    log = completed.stdout + completed.stderr
    if log.count("Entering activity: EpubReaderMenu") != 2 or "Entering activity: BookSavedItems" not in log:
        raise AssertionError(f"{device.upper()} did not follow XTC bookmark -> Saved menu order:\n{log}")
    if "Entering activity: ClipSelection" in log:
        raise AssertionError(f"{device.upper()} exposed text highlighting for fixed-layout XTC")

    bookmark_files = list((control / "bookmarks").glob("book_*.json"))
    if len(bookmark_files) != 1:
        raise AssertionError(f"{device.upper()} wrote {len(bookmark_files)} XTC bookmark documents")
    document = json.loads(bookmark_files[0].read_text(encoding="utf-8"))
    metadata = document.get("book", {})
    bookmarks = document.get("bookmarks", [])
    if metadata.get("path") != "/saved.xtc" or metadata.get("type") != "xtc" or len(bookmarks) != 1:
        raise AssertionError(f"{device.upper()} wrote invalid XTC bookmark metadata")
    if bookmarks[0].get("positionKind") != "fixed" or bookmarks[0].get("pageIndex") != 0:
        raise AssertionError(f"{device.upper()} did not store the current XTC page index")

    screenshots = sorted(shots.glob("*.framebuffer.bin"))
    if len(screenshots) != 2 or screenshots[0].read_bytes() == screenshots[1].read_bytes():
        raise AssertionError(f"{device.upper()} XTC Saved screen did not focus its bookmark row")
    print(f"{device.upper()}: XTC page bookmark and bookmark-only Saved screen smoke passed")


def stress_xtc_page_turns() -> None:
    """Exercise the real reader/render tasks with 1,000 rapid page requests."""
    device = "x3"
    output = BUILD / f"{device}-xtc-stress"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    control = sd / ".crosspoint"
    control.mkdir()
    (control / "settings.json").write_text(json.dumps({"longPressMenuFunction": 2}) + "\n", encoding="utf-8")
    fixture = sd / "stress.xtc"
    source = XTC_FIXTURES / "crossvi-converter-480x800.xtc"
    create_navigation_fixture(source, fixture)

    first_turn_at = 3500
    # Keep every press visible across at least one ~16 ms simulator polling
    # cycle. A 3 ms pulse can begin and end between two polls, which stresses
    # the input script rather than the reader state/render contract.
    turn_interval = 32
    press_duration = 16
    events = ["1200:CONFIRM", "2600:CONFIRM"]
    for index in range(1000):
        key = "RIGHT" if index % 2 == 0 else "LEFT"
        event_at = first_turn_at + index * turn_interval
        events.append(f"{event_at}:{key}:{press_duration}")
    bookmark_at = first_turn_at + 1000 * turn_interval + 100
    # Submit a main-task bookmark notice while the render task can still be
    # draining the rapid page-turn queue, without overlapping page-key pulses.
    events.append(f"{bookmark_at}:CONFIRM:600")
    screenshot_at = bookmark_at + 4000
    events.append(f"{screenshot_at}:SCREENSHOT")

    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": ",".join(events),
            "CROSSVI_SIM_EXIT_AFTER_MS": str(screenshot_at + 700),
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=45)
    log = completed.stdout + completed.stderr
    if "Opened file: /stress.xtc (3 pages, 480x800)" not in log or "Rendered page 2/3 (1-bit)" not in log:
        raise AssertionError(f"X3 did not exercise the XTC reader during page-turn stress:\n{log}")
    screenshot = find_single(shots, "*.bmp")
    assert_xtg_mapping(device, source, screenshot)
    progress = find_single(sd / ".crosspoint", "xtc_*/progress.bin")
    if progress.read_bytes() != struct.pack("<I", 0):
        raise AssertionError("X3 stress ended with bitmap and saved progress on different pages")
    bookmark_document = find_single(control / "bookmarks", "book_*.json")
    bookmarks = json.loads(bookmark_document.read_text(encoding="utf-8")).get("bookmarks", [])
    if len(bookmarks) != 1 or bookmarks[0].get("positionKind") != "fixed":
        raise AssertionError("X3 stress lost the bookmark action submitted during rendering")
    print("X3: 1,000 rapid XTC page turns kept bitmap/progress aligned and preserved a concurrent bookmark action")


def smoke_xtc_replacement() -> None:
    device = "x3"
    output = BUILD / f"{device}-xtc-replacement"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots_a = output / "screenshots-a"
    sd.mkdir(parents=True)
    shots_a.mkdir(parents=True)
    (sd / ".crosspoint").mkdir()
    (sd / ".crosspoint" / "settings.json").write_text('{"sleepScreen":3}\n', encoding="utf-8")
    book = sd / "same.xtc"
    create_navigation_fixture(XTC_FIXTURES / "crossvi-converter-480x800.xtc", book)
    original_size = book.stat().st_size
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"

    environment_a = os.environ.copy()
    environment_a.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots_a),
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:CONFIRM,2600:CONFIRM,4000:RIGHT,5000:BACK,6200:SCREENSHOT,7000:POWER:800"
            ),
            "CROSSVI_SIM_EXIT_ON_SLEEP": "1",
            "CROSSVI_SIM_EXIT_AFTER_MS": "9000",
        }
    )
    run_a = run([str(binary)], env=environment_a, capture_output=True, timeout=15)
    log_a = run_a.stdout + run_a.stderr
    if "Rendered page 2/3 (1-bit)" not in log_a or "Entering activity: Home" not in log_a:
        raise AssertionError(f"X3 did not prepare replacement state from book A:\n{log_a}")
    cache_a = find_single(sd / ".crosspoint", "xtc_*")
    progress_a = (cache_a / "progress.bin").read_bytes()
    identity_a = (cache_a / "source_identity.bin").read_bytes()
    cover_a = (cache_a / "cover.bmp").read_bytes()
    # Thumbnail generation is deliberately no longer performed from Home or
    # the library render path. Seed the cache from the reader cover so the
    # replacement test still verifies that an old derived thumbnail cannot be
    # inherited by a same-path/same-size replacement.
    shutil.copy2(cache_a / "cover.bmp", cache_a / "thumb_168.bmp")
    thumb_a = find_single(cache_a, "thumb_*.bmp").read_bytes()
    if progress_a != struct.pack("<I", 1):
        raise AssertionError("X3 did not save book A on page 2 before replacement")

    invert_xtg_pages(book)
    if book.stat().st_size != original_size:
        raise AssertionError("Replacement fixture unexpectedly changed size")
    shots_b = output / "screenshots-b"
    shots_b.mkdir()
    environment_b = os.environ.copy()
    environment_b.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots_b),
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:CONFIRM,2600:CONFIRM,4200:SCREENSHOT,5000:BACK,6200:SCREENSHOT,7000:POWER:800"
            ),
            "CROSSVI_SIM_EXIT_ON_SLEEP": "1",
            "CROSSVI_SIM_EXIT_AFTER_MS": "9000",
        }
    )
    run_b = run([str(binary)], env=environment_b, capture_output=True, timeout=15)
    log_b = run_b.stdout + run_b.stderr
    if "Rendered page 1/3 (1-bit)" not in log_b or "Loaded progress: page 1" in log_b:
        raise AssertionError(f"X3 replacement inherited book A progress:\n{log_b}")
    cache_b = find_single(sd / ".crosspoint", "xtc_*")
    if (cache_b / "progress.bin").read_bytes() != struct.pack("<I", 0):
        raise AssertionError("Book B did not start with fresh progress")
    if (cache_b / "source_identity.bin").read_bytes() == identity_a:
        raise AssertionError("Book B retained book A source identity")
    if (cache_b / "cover.bmp").read_bytes() == cover_a:
        raise AssertionError("Book B retained book A cover")
    thumbnails_b = sorted(cache_b.glob("thumb_*.bmp"))
    if any(thumbnail.read_bytes() == thumb_a for thumbnail in thumbnails_b):
        raise AssertionError("Book B retained book A thumbnail")
    print("X3: same-path/same-size XTC replacement discarded old progress, cover and thumbnail")


def smoke_book_search_actions(device: str, source: str) -> None:
    output = BUILD / f"{device}-book-search-{source}"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    control.mkdir()
    settings = {
        "uiTheme": 5,
        "homeLayout": 1,
        "homeLayoutVersion": 2,
        "hideTxtBooks": 0,
    }
    if device == "x3":
        settings["language"] = "VI"
    (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
    books = []
    for index in range(20):
        name = f"12-book-{index}.txt"
        (sd / name).write_text(f"Book {index}\n\fSecond page\n", encoding="utf-8")
        books.append({"path": f"/{name}", "title": f"12 book {index}", "author": "", "coverBmpPath": ""})
    initial_pinned = []
    if source == "recent":
        initial_pinned = [book["path"] for book in books[:12]]
        (control / "recent.json").write_text(
            json.dumps({"books": books, "pinned": initial_pinned}) + "\n", encoding="utf-8"
        )

    # X3 starts without recents: six Down presses must wrap across the six
    # actual menu items. X4 uses
    # Your Books so both action-popup callers get exercised across the pair.
    if source == "browser":
        enter = "1000:DOWN,1400:DOWN,1800:DOWN,2200:DOWN,2600:DOWN,3000:DOWN,3600:CONFIRM"
        focus_first_book = ""
        focus_search = "8100:UP"
        reopen_search = "15000:BACK,15500:UP,16000:CONFIRM"
    else:
        enter = "1200:DOWN,1700:DOWN,2300:CONFIRM"
        # Your Books opens with the Recent tab focused. Enter the first book
        # explicitly; a held Back then opens search without a toolbar row.
        focus_first_book = "3300:DOWN"
        focus_search = "8000:BACK:600"
        reopen_search = "15000:BACK,15400:BACK:600"
    if source == "recent":
        # Recent/All no longer expose Search or Refresh as focusable rows.
        # Search is opened with a held Back from the selected book (or tab).
        events = [
            "800:SCREENSHOT",
            enter,
            focus_first_book,
            "5000:CONFIRM:1000",
            "5600:SCREENSHOT",
            "6200:SCREENSHOT",
            "7000:BACK",  # dismiss the book-action popup
            "7600:BACK:600",  # hold Back: launch search
            "8500:CONFIRM",  # type the initially selected '1'
            "9000:SCREENSHOT",
            "9400:DOWN,9800:DOWN,10200:DOWN,10600:DOWN",
            "11000:RIGHT,11400:RIGHT,11800:RIGHT,12200:RIGHT",
            "12600:CONFIRM",
            "13600:SCREENSHOT",
            "14500:BACK",  # book -> tab
            "15000:BACK:600",  # hold Back again: query is prefilled
            "16000:CONFIRM",
            "17000:SCREENSHOT",
            "17400:DOWN,17800:DOWN,18200:DOWN,18600:DOWN",
            "19000:RIGHT,19400:RIGHT,19800:RIGHT,20200:RIGHT",
            "20600:CONFIRM",
            "21600:SCREENSHOT",
            "22000:CONFIRM",  # open the first ranked result
        ]
    else:
        events = ["800:SCREENSHOT", enter]
        if focus_first_book:
            events.append(focus_first_book)
        events.extend(
            [
                "5000:CONFIRM:1000",  # popup must appear at 5500, before release at 6000
                "5600:SCREENSHOT",
                "6200:SCREENSHOT",
                "7600:BACK",
                focus_search,
                "8600:CONFIRM",
                "9400:CONFIRM",  # type the initially selected '1'
                "9800:SCREENSHOT",
                "10200:DOWN,10600:DOWN,11000:DOWN,11400:DOWN",
                "11800:RIGHT,12200:RIGHT,12600:RIGHT,13000:RIGHT",
                "13400:CONFIRM",
                "14400:SCREENSHOT",
                reopen_search,
                "17000:SCREENSHOT",
                "17500:DOWN,17900:DOWN,18300:DOWN,18700:DOWN",
                "19100:RIGHT,19500:RIGHT,19900:RIGHT,20300:RIGHT",
                "20700:CONFIRM",
                "21500:CONFIRM",
            ]
        )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": ",".join(events),
            "CROSSVI_SIM_EXIT_AFTER_MS": "26500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=35)
    log = completed.stdout + completed.stderr
    activity = "FileBrowser" if source == "browser" else "YourBooks"
    if f"Entering activity: {activity}" not in log or log.count("Entering activity: KeyboardEntry") < 2:
        raise AssertionError(f"{device.upper()} did not complete the {source} search flow:\n{log}")
    if source == "browser" and "Entering activity: TxtReader" not in log:
        raise AssertionError(f"{device.upper()} did not open the wrapped first search result:\n{log}")

    screenshots = sorted(shots.glob("*.bmp"))
    expected_screenshots = 7 if source == "recent" else 6
    if len(screenshots) != expected_screenshots:
        raise AssertionError(f"{device.upper()} {source} search produced {len(screenshots)} screenshots")
    if screenshots[1].read_bytes() != screenshots[2].read_bytes():
        raise AssertionError(f"{device.upper()} Confirm release selected the first popup action")

    recent = json.loads((control / "recent.json").read_text(encoding="utf-8"))
    if recent.get("pinned", []) != initial_pinned:
        raise AssertionError(f"{device.upper()} popup release changed the pinned-book list")
    if not recent.get("books") or recent["books"][0].get("path") != "/12-book-0.txt":
        raise AssertionError(f"{device.upper()} search did not open its first ranked result")
    print(f"{device.upper()}: {source} long-press/search/query-retention smoke passed")


def smoke_your_books_press_edges(device: str) -> None:
    """Held and rapid directions must survive slow library refreshes."""

    for direction, expected_index in (("DOWN", 1), ("UP", 0)):
        output = BUILD / f"{device}-your-books-{direction.lower()}-edge"
        if output.exists():
            shutil.rmtree(output)
        sd = output / "sd"
        control = sd / ".crosspoint"
        control.mkdir(parents=True)
        (control / "settings.json").write_text(
            json.dumps({"homeLayout": 1, "homeLayoutVersion": 2, "hideTxtBooks": 0}) + "\n",
            encoding="utf-8",
        )
        books = []
        for index in range(6):
            name = f"edge-{index}.txt"
            (sd / name).write_text(f"Edge {index}\n", encoding="utf-8")
            books.append(
                {"path": f"/{name}", "title": f"Edge {index}", "author": "", "coverBmpPath": ""}
            )
        (control / "recent.json").write_text(
            json.dumps({"books": books, "pinned": []}) + "\n", encoding="utf-8"
        )

        environment = os.environ.copy()
        navigation = f"3500:{direction}:1600"
        if direction == "UP":
            # Start one book below the top edge. From the first book, Up is
            # intentionally allowed to focus the action row instead.
            navigation = "3200:DOWN,3500:UP:1600"
        # Your Books opens with the Recent tab focused. A short Down enters
        # the first book; only a held Up/Down switches tabs now, so direction
        # edge tests must not use a hold when they intend row navigation.
        if direction == "DOWN":
            navigation = "3200:DOWN,3500:DOWN"
        else:
            navigation = "3200:DOWN,3500:DOWN,3800:UP"
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_INPUT_SCRIPT": (
                    f"1200:DOWN,1700:DOWN,2300:CONFIRM,{navigation},5700:CONFIRM"
                ),
                "CROSSVI_SIM_EXIT_AFTER_MS": "8500",
            }
        )
        binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
        completed = run([str(binary)], env=environment, capture_output=True, timeout=14)
        log = completed.stdout + completed.stderr
        if "Entering activity: TxtReader" not in log:
            raise AssertionError(f"{device.upper()} did not open a book after held {direction}:\n{log}")
        recent = json.loads((control / "recent.json").read_text(encoding="utf-8"))
        expected_path = f"/edge-{expected_index}.txt"
        if not recent.get("books") or recent["books"][0].get("path") != expected_path:
            raise AssertionError(
                f"{device.upper()} held {direction} auto-repeated; expected {expected_path}, got {recent.get('books')}"
            )

    output = BUILD / f"{device}-your-books-rapid-edges"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps({"homeLayout": 1, "homeLayoutVersion": 2, "hideTxtBooks": 0}) + "\n",
        encoding="utf-8",
    )
    books = []
    for index in range(8):
        name = f"rapid-{index}.txt"
        (sd / name).write_text(f"Rapid {index}\n", encoding="utf-8")
        books.append(
            {"path": f"/{name}", "title": f"Rapid {index}", "author": "", "coverBmpPath": ""}
        )
    (control / "recent.json").write_text(
        json.dumps({"books": books, "pinned": []}) + "\n", encoding="utf-8"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:DOWN,1700:DOWN,2300:CONFIRM,3500:DOWN,3700:DOWN,"
                "3900:DOWN,5200:CONFIRM"
            ),
            "CROSSVI_SIM_EXIT_AFTER_MS": "8500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=14)
    log = completed.stdout + completed.stderr
    if "Entering activity: TxtReader" not in log:
        raise AssertionError(f"{device.upper()} did not open a book after rapid directions:\n{log}")
    recent = json.loads((control / "recent.json").read_text(encoding="utf-8"))
    if not recent.get("books") or recent["books"][0].get("path") != "/rapid-2.txt":
        raise AssertionError(f"{device.upper()} lost rapid directions: {recent.get('books')}")

    print(f"{device.upper()}: Your Books held and rapid directions were preserved")


def smoke_your_books_catalog(device: str) -> None:
    output = BUILD / f"{device}-your-books-catalog"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    nested = sd / "Nested"
    control.mkdir(parents=True)
    nested.mkdir()
    shots.mkdir()

    settings = {
        "uiTheme": 1,
        "language": "VI" if device == "x3" else "EN",
        "allLibraryView": 1,
        "allLibraryGrid": 2,
        "hideTxtBooks": 0,
    }
    (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
    expected_count = 1000
    (sd / "00-alpha.txt").write_text("Alpha\n\fSecond page\n", encoding="utf-8")
    for index in range(1, expected_count - 2):
        parent = nested if index % 2 else sd
        (parent / f"book-{index:02d}.txt").write_text(f"Book {index}\n", encoding="utf-8")
    (sd / "notes.md").write_text("# Notes\n", encoding="utf-8")
    shutil.copy2(ROOT / "test" / "epubs" / "test_kerning_ligature.epub", sd / "kerning.epub")

    events = (
        "1000:RIGHT,1400:CONFIRM,"  # Home -> Your Books
        # Enter the first Recent book, return to the tab focus, then Confirm
        # changes Recent -> All. Up from the tab intentionally enters the last
        # book, so this uses the unambiguous Down -> Up sequence.
        "1800:DOWN,2000:UP,2200:CONFIRM,"
        # Incremental finalization deliberately copies one record per catalog
        # step. Keep the activity open until all 1,000 records are published.
        "28000:SCREENSHOT,30000:DOWN,30400:DOWN,30800:DOWN,31200:DOWN,"
        "31600:SCREENSHOT,32000:CONFIRM"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "35000",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=42)
    log = completed.stdout + completed.stderr
    opened_reader = any(
        marker in log
        for marker in (
            "Entering activity: EpubReader",
            "Entering activity: TxtReader",
            "Entering activity: XtcReader",
        )
    )
    if "Entering activity: YourBooks" not in log or not opened_reader:
        raise AssertionError(f"{device.upper()} did not browse and open the All catalog:\n{log}")

    catalog = control / "library.idx"
    if not catalog.exists():
        raise AssertionError(f"{device.upper()} did not publish the bounded library catalog")
    header = catalog.read_bytes()[:28]
    if len(header) != 28:
        raise AssertionError(f"{device.upper()} library catalog header is truncated")
    magic, version, _record_size, count, _generation, phase, truncated, _reserved, _crc = struct.unpack(
        "<8sHHIIBBHI", header
    )
    if magic != b"CVLIB01\0" or version != 2 or count != expected_count or phase != 4 or truncated != 0:
        raise AssertionError(
            f"{device.upper()} invalid catalog header: magic={magic!r}, version={version}, "
            f"count={count}, phase={phase}, truncated={truncated}"
        )
    if (control / "library.dirty").exists() or len(list(shots.glob("*.bmp"))) != 2:
        raise AssertionError(f"{device.upper()} catalog did not finish cleanly or screenshots are missing")
    persisted_settings = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if (
        "uiTheme" in persisted_settings
        or persisted_settings.get("libraryView") != settings["allLibraryView"]
        or persisted_settings.get("libraryGrid") != 0
        or persisted_settings.get("libraryGridLayoutVersion") != 2
    ):
        raise AssertionError(f"{device.upper()} did not migrate the legacy theme and per-tab grid to shared 3x2")

    original_catalog = catalog.read_bytes()

    # Reuse the published 1,000-book catalog so the search checks exercise the
    # cooperative scanner itself rather than spending another run indexing.
    search_shots = output / "search-screenshots"
    search_shots.mkdir()
    search_events = [
        "1200:DOWN,1700:DOWN,2300:CONFIRM",  # Home -> Your Books
        "3000:DOWN,3300:UP,3600:CONFIRM",  # first Recent book -> tab -> All
        "4400:DOWN,4800:BACK:600",  # enter the first book, then hold Back -> search
        "5600:CONFIRM,6000:SCREENSHOT",  # enter '1' and capture the query
        "6400:DOWN,6800:DOWN,7200:DOWN,7600:DOWN",
        "8000:RIGHT,8400:RIGHT,8800:RIGHT,9200:RIGHT,9600:CONFIRM",
        "10000:SCREENSHOT,10300:BACK,10900:SCREENSHOT",  # loading -> cancel -> full catalog
        "11400:BACK:600,12200:SCREENSHOT",  # reopen with retained query
        "12800:DOWN,13200:DOWN,13600:DOWN,14000:DOWN",
        "14400:RIGHT,14800:RIGHT,15200:RIGHT,15600:RIGHT,16000:CONFIRM",
        "18000:SCREENSHOT,18600:CONFIRM",  # completed results -> open first match
    ]
    search_environment = os.environ.copy()
    search_environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(search_shots),
            "CROSSVI_SIM_INPUT_SCRIPT": ",".join(search_events),
            "CROSSVI_SIM_EXIT_AFTER_MS": "21000",
        }
    )
    search_run = run([str(binary)], env=search_environment, capture_output=True, timeout=28)
    search_log = search_run.stdout + search_run.stderr
    if search_log.count("Entering activity: KeyboardEntry") < 2 or "Entering activity: TxtReader" not in search_log:
        raise AssertionError(f"{device.upper()} did not cancel, resume and complete the All search:\n{search_log}")
    search_frames = sorted(search_shots.glob("*.bmp"))
    if len(search_frames) != 5:
        raise AssertionError(f"{device.upper()} All search produced {len(search_frames)} screenshots")
    if search_frames[0].read_bytes() != search_frames[3].read_bytes():
        raise AssertionError(f"{device.upper()} All search did not retain its query after cancellation")
    if search_frames[1].read_bytes() == search_frames[2].read_bytes():
        raise AssertionError(f"{device.upper()} All search did not leave its loading state when cancelled")
    if search_frames[2].read_bytes() == search_frames[4].read_bytes():
        raise AssertionError(f"{device.upper()} All search did not atomically publish its completed result set")
    if catalog.read_bytes() != original_catalog:
        raise AssertionError(f"{device.upper()} read-only All search changed the catalog")

    print(f"{device.upper()}: Your Books All-tab catalog/grid smoke passed ({expected_count} books)")


def smoke_your_books_grid(device: str, grid_setting: int, grid_name: str, capacity: int) -> None:
    if (grid_setting, grid_name, capacity) != (0, "3x2", 6):
        raise AssertionError("Unsupported visual library grid fixture")
    book_count = capacity + 1
    output = BUILD / f"{device}-your-books-grid-{grid_name}-{book_count}"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    shots.mkdir(parents=True)
    settings = {
        "language": "VI" if device == "x3" else "EN",
        # This smoke test exercises the library grid, not Home style 1's
        # variable-length recent list.  Pin Home to the single-book layout so
        # the scripted route to My Books stays deterministic.
        "homeLayout": 1,
        "homeLayoutVersion": 2,
        "hideTxtBooks": 0,
        "libraryView": 1,
        "libraryGrid": grid_setting,
        "libraryGridLayoutVersion": 2,
    }
    (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")

    # Seed only the Home-sized thumbnail. The library asks for its grid
    # height, so this also guards the production fallback to an existing cache
    # instead of silently replacing a valid cover with a placeholder.
    cover = control / "library-thumb_168.bmp"
    copy_bmp_as_top_down(ROOT / "test" / "language" / "RTL" / "Bidi-Test_ch1_p1_0pct_398667.bmp", cover)
    books: list[dict[str, str]] = []
    extensions = ("txt", "md", "epub", "xtc", "xtch")
    for index in range(book_count):
        extension = extensions[index % len(extensions)]
        name = f"book-{index:02d}.{extension}"
        path = sd / name
        if extension == "epub":
            shutil.copy2(ROOT / "test" / "epubs" / "test_kerning_ligature.epub", path)
        elif extension == "xtc":
            shutil.copy2(XTC_FIXTURES / "crossvi-converter-480x800.xtc", path)
        elif extension == "xtch":
            shutil.copy2(XTC_FIXTURES / "crossvi-converter-480x800.xtch", path)
        else:
            path.write_text(("# " if extension == "md" else "") + f"Book {index}\n", encoding="utf-8")
        title = (
            "Nghệ thuật tinh tế của việc đếch quan tâm — tên sách tiếng Việt rất dài để kiểm tra hai dòng và dấu cắt"
            if index == 0
            else f"Library book {index:02d}"
        )
        books.append(
            {
                "path": f"/{name}",
                "title": title,
                "author": "CrossVi Tests",
                "coverBmpPath": "/.crosspoint/library-thumb_[HEIGHT].bmp" if index in (0, 3) else "",
            }
        )

    # The store keeps ten recent entries and twelve pins. Keep the long-title
    # real cover first, then extend the visible list without changing store caps.
    pinned = [books[0]["path"], *[book["path"] for book in books[6:]]]
    (control / "recent.json").write_text(
        json.dumps({"books": books, "pinned": pinned}) + "\n",
        encoding="utf-8",
    )

    page_turn_start = 5000
    page_turn_interval = 300
    page_two_screenshot = page_turn_start + capacity * page_turn_interval + 500
    events = [
        "1200:DOWN,1700:DOWN,2300:CONFIRM",  # current book -> Browse -> Your Books
        "3600:SCREENSHOT",
        # Your Books starts on the Recent tab. A short Down enters the first
        # book; subsequent Down presses cross the cover-page boundary.
        "4000:DOWN,5000:SCREENSHOT",
    ]
    page_turn_start = 6400
    page_two_screenshot = page_turn_start + capacity * page_turn_interval + 500
    events.extend(
        [
            ",".join(f"{page_turn_start + index * page_turn_interval}:DOWN" for index in range(capacity)),
            f"{page_two_screenshot}:SCREENSHOT",
        ]
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": ",".join(events),
            "CROSSVI_SIM_EXIT_AFTER_MS": str(page_two_screenshot + 1000),
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=18)
    log = completed.stdout + completed.stderr
    if "Entering activity: YourBooks" not in log:
        raise AssertionError(f"{device.upper()} did not open the {grid_name} Your Books fixture:\n{log}")
    screenshots = sorted(shots.glob("*.bmp"))
    expected_screenshot_count = 3
    if len(screenshots) != expected_screenshot_count:
        raise AssertionError(
            f"{device.upper()} {grid_name} grid produced {len(screenshots)} screenshots"
        )
    expected_dimensions = (528, 792) if device == "x3" else (480, 800)
    if any(bmp_dimensions(screenshot) != expected_dimensions for screenshot in screenshots):
        raise AssertionError(f"{device.upper()} {grid_name} screenshot dimensions changed")
    if screenshots[0].read_bytes() == screenshots[1].read_bytes():
        raise AssertionError(f"{device.upper()} did not expose the tab as a distinct focus target")
    if screenshots[-2].read_bytes() == screenshots[-1].read_bytes():
        raise AssertionError(
            f"{device.upper()} did not cross the {grid_name} page boundary after {capacity} books"
        )
    print(f"{device.upper()}: Your Books {grid_name} visual smoke passed ({book_count} books)")


def smoke_vietnamese_telex(device: str) -> None:
    output = BUILD / f"{device}-vietnamese-telex"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    control.mkdir()
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI"}) + "\n", encoding="utf-8"
    )
    expected_path = "/đ dd.txt"
    (sd / expected_path.removeprefix("/")).write_text("Vietnamese Telex\n", encoding="utf-8")

    # Open File Browser and its search keyboard. Type "dd" as "đ", then
    # hold #@! long enough to disable Telex before release and type raw "dd".
    events = (
        "1000:CONFIRM,1400:UP,1800:CONFIRM,2400:DOWN,2800:DOWN,3200:RIGHT,3600:RIGHT,"
        "4000:CONFIRM,4400:CONFIRM,4800:DOWN,5200:DOWN,5600:SCREENSHOT,"
        "6200:CONFIRM:1000,6900:SCREENSHOT,7600:SCREENSHOT,8000:RIGHT,8400:CONFIRM,"
        "8800:LEFT,9200:UP,9600:UP,10000:CONFIRM,10400:CONFIRM,10800:DOWN,"
        "11200:DOWN,11600:RIGHT,12000:RIGHT,12400:RIGHT,12800:CONFIRM,"
        "14000:SCREENSHOT,14800:CONFIRM,16000:SCREENSHOT"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "17000",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=25)
    log = completed.stdout + completed.stderr
    if "Entering activity: KeyboardEntry" not in log or "Entering activity: TxtReader" not in log:
        raise AssertionError(f"{device.upper()} did not complete the Vietnamese Telex flow:\n{log}")

    screenshots = sorted(shots.glob("*.framebuffer.bin"))
    if len(screenshots) != 5:
        raise AssertionError(f"{device.upper()} Telex smoke produced {len(screenshots)} screenshots")
    if screenshots[0].read_bytes() == screenshots[1].read_bytes():
        raise AssertionError(f"{device.upper()} Telex toggle did not render while Confirm was still held")
    if screenshots[1].read_bytes() != screenshots[2].read_bytes():
        raise AssertionError(f"{device.upper()} Confirm release triggered an extra #@! action")

    recent = json.loads((control / "recent.json").read_text(encoding="utf-8"))
    if not recent.get("books") or recent["books"][0].get("path") != expected_path:
        raise AssertionError(f"{device.upper()} did not compose and open the exact query 'đ dd'")
    print(f"{device.upper()}: Vietnamese Telex composition and hold-toggle smoke passed")


def smoke_home_stats_menu(device: str) -> None:
    output = BUILD / f"{device}-home-stats-menu"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    control.mkdir()
    settings = {"uiTheme": 5}
    if device == "x3":
        settings["language"] = "VI"
    (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            # Home -> stats hub -> Overview -> hub -> book picker -> hub ->
            # Calendar -> selected-day detail. The hub keeps its selected row
            # when each child closes.
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:DOWN,1700:DOWN,2200:CONFIRM,3000:SCREENSHOT,3500:CONFIRM,"
                "4400:SCREENSHOT,4900:BACK,5500:DOWN,6000:CONFIRM,7400:SCREENSHOT,"
                "7900:BACK,8500:DOWN,9000:CONFIRM,10000:SCREENSHOT,10500:CONFIRM,"
                "11400:SCREENSHOT"
            ),
            "CROSSVI_SIM_EXIT_AFTER_MS": "12200",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=20)
    log = completed.stdout + completed.stderr
    expected = (
        "Entering activity: ReadingStatsMenu",
        "Entering activity: ReadingStats",
        "Entering activity: BookStatsSelection",
        "Entering activity: ReadingCalendar",
        "Entering activity: ReadingDayDetail",
    )
    missing = [activity for activity in expected if activity not in log]
    if missing:
        raise AssertionError(f"{device.upper()} Reading Stats flow missed {missing}:\n{log}")
    screenshots = sorted(shots.glob("*.bmp"))
    if len(screenshots) != 5:
        raise AssertionError(f"{device.upper()} Reading Stats flow produced {len(screenshots)} screenshots")
    print(f"{device.upper()}: stats hub, overview, book picker, calendar and day detail smoke passed")


def smoke_home_layouts_without_reading_summary(device: str) -> None:
    output = BUILD / f"{device}-home-layouts-no-summary"
    if output.exists():
        shutil.rmtree(output)
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"

    for layout in range(4):
        sd = output / f"style-{layout + 1}" / "sd"
        shots = output / f"style-{layout + 1}" / "screenshots"
        control = sd / ".crosspoint"
        control.mkdir(parents=True)
        shots.mkdir(parents=True)
        books = []
        for index in range(5):
            name = f"book-{index + 1}.txt"
            (sd / name).write_text(f"Book {index + 1}\n", encoding="utf-8")
            books.append(
                {
                    "path": f"/{name}",
                    "title": f"Cuốn sách gần đây {index + 1}",
                    "author": f"Tác giả {index + 1}",
                    "coverBmpPath": "",
                }
            )
        settings = {"uiTheme": 5, "homeLayout": layout, "homeLayoutVersion": 2}
        if device == "x3":
            settings["language"] = "VI"
        (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
        (control / "recent.json").write_text(json.dumps({"books": books, "pinned": []}) + "\n", encoding="utf-8")
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
                "CROSSVI_SIM_INPUT_SCRIPT": "1500:SCREENSHOT",
                "CROSSVI_SIM_EXIT_AFTER_MS": "2200",
            }
        )
        run([str(binary)], env=environment, capture_output=True, timeout=8)
        saved_settings = json.loads((control / "settings.json").read_text(encoding="utf-8"))
        if saved_settings.get("homeLayout") != layout:
            raise AssertionError(
                f"{device.upper()} migrated Home style {layout + 1} to {saved_settings.get('homeLayout')}"
            )
        screenshots = sorted(shots.glob("*.bmp"))
        if len(screenshots) != 1:
            raise AssertionError(f"{device.upper()} Home style {layout + 1} produced {len(screenshots)} screenshots")
    print(f"{device.upper()}: Home styles 1–4 rendered without the Today/Goal summary")


def smoke_home_saved_items(device: str) -> None:
    output = BUILD / f"{device}-home-saved-items"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    shots.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI"}) + "\n", encoding="utf-8"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            # Home navigation is sequential: Browse -> Your Books -> Reading Stats -> Saved.
            "CROSSVI_SIM_INPUT_SCRIPT": "1200:DOWN,1500:DOWN,1800:DOWN,2200:CONFIRM,3000:SCREENSHOT",
            "CROSSVI_SIM_EXIT_AFTER_MS": "3800",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=10)
    log = completed.stdout + completed.stderr
    if "Entering activity: SavedClippings" not in log:
        raise AssertionError(f"{device.upper()} Home Saved tile did not open the global catalog:\n{log}")
    find_single(shots, "*.framebuffer.bin")
    print(f"{device.upper()}: Home Saved tile opened the on-demand global catalog")


def smoke_txt_saved_items_menu(device: str) -> None:
    output = BUILD / f"{device}-txt-saved-items"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    shots.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI"}) + "\n", encoding="utf-8"
    )
    text = "".join(f"Dòng thử số {index} có nhiều từ tiếng Việt để chọn tô sáng.\n" for index in range(1, 81))
    (sd / "saved-flow.txt").write_text(text, encoding="utf-8")

    # TXT menu starts with Home, Book settings, Bookmark, Highlight, Saved.
    # Exercise the three saved-item rows. The first Back clears selection point 1/2; the
    # second selection is completed and must appear beside the bookmark.
    events = (
        "1200:CONFIRM,2400:CONFIRM,4500:CONFIRM,5100:DOWN,5700:DOWN,6300:CONFIRM,"
        "7200:CONFIRM,7800:DOWN,8400:DOWN,9000:DOWN,9600:CONFIRM,"
        "10500:SCREENSHOT,11100:CONFIRM,11800:SCREENSHOT,12400:BACK,13100:SCREENSHOT,"
        "13700:CONFIRM,14300:RIGHT,14900:CONFIRM,"
        "15900:CONFIRM,16500:DOWN,17100:DOWN,17700:DOWN,18300:CONFIRM,"
        "19200:CONFIRM,19800:RIGHT,20400:RIGHT,21000:CONFIRM,"
        "22000:CONFIRM,22600:DOWN,23200:DOWN,23800:DOWN,24400:DOWN,25000:CONFIRM,25900:SCREENSHOT"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "26700",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=36)
    log = completed.stdout + completed.stderr
    if log.count("Entering activity: EpubReaderMenu") != 4:
        raise AssertionError(f"{device.upper()} did not reopen the TXT tools menu for all saved actions:\n{log}")
    if log.count("Entering activity: ClipSelection") != 2 or "Entering activity: BookSavedItems" not in log:
        raise AssertionError(f"{device.upper()} did not follow TXT bookmark -> highlight -> Saved menu order:\n{log}")

    screenshots = sorted(shots.glob("*.framebuffer.bin"))
    if len(screenshots) != 4:
        raise AssertionError(f"{device.upper()} TXT selection flow produced {len(screenshots)} screenshots")
    if screenshots[0].read_bytes() == screenshots[1].read_bytes():
        raise AssertionError(f"{device.upper()} did not render the second highlight-selection stage")
    if screenshots[0].read_bytes() != screenshots[2].read_bytes():
        raise AssertionError(f"{device.upper()} first Back did not restore highlight-selection stage 1/2")

    bookmark_files = list((control / "bookmarks").glob("book_*.json"))
    clipping_files = list((control / "clippings").glob("txt_*.bin"))
    if len(bookmark_files) != 1 or len(clipping_files) != 1:
        raise AssertionError(f"{device.upper()} did not persist the TXT bookmark/highlight stores")
    document = json.loads(bookmark_files[0].read_text(encoding="utf-8"))
    if document.get("book", {}).get("path") != "/saved-flow.txt" or len(document.get("bookmarks", [])) != 1:
        raise AssertionError(f"{device.upper()} wrote invalid TXT bookmark metadata")
    if clipping_files[0].stat().st_size == 0:
        raise AssertionError(f"{device.upper()} wrote an empty TXT highlight store")
    clipping_data = clipping_files[0].read_bytes()
    if len(clipping_data) < 18 or struct.unpack_from("<H", clipping_data, 16)[0] != 2:
        raise AssertionError(f"{device.upper()} did not preserve two highlights on the same TXT page")
    print(f"{device.upper()}: TXT bookmark/two highlights/Saved grouping and two-stage Back smoke passed")


def smoke_text_highlight_restart(device: str, extension: str) -> None:
    output = BUILD / f"{device}-{extension}-highlight-restart"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    control = sd / ".crosspoint"
    first_shots = output / "screenshots-before"
    restart_shots = output / "screenshots-restart"
    control_shots = output / "screenshots-no-highlight"
    control.mkdir(parents=True)
    first_shots.mkdir(parents=True)
    restart_shots.mkdir(parents=True)
    control_shots.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps(
            {
                "uiTheme": 5,
                "language": "VI",
                "screenMargin": 5,
                "paragraphAlignment": 1,
                "statusBarChapterPageCount": 0,
                "statusBarProgressBar": 2,
                "statusBarTitle": 2,
                "statusBarBattery": 0,
            }
        )
        + "\n",
        encoding="utf-8",
    )
    book = sd / f"highlight-restart.{extension}"
    book.write_text(
        "".join(f"Dòng {index}: vùng tô sáng bền vững qua lần mở lại.\n" for index in range(1, 81)),
        encoding="utf-8",
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"

    create_environment = os.environ.copy()
    create_environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(first_shots),
            # Capture the untouched page, then choose the PlainText menu's
            # Highlight row and persist a two-word selection.
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:CONFIRM,2600:CONFIRM,4200:SCREENSHOT,5000:CONFIRM,"
                "5600:DOWN,6200:DOWN,6800:DOWN,7400:CONFIRM,8200:CONFIRM,"
                "8800:RIGHT,9400:CONFIRM"
            ),
            "CROSSVI_SIM_EXIT_AFTER_MS": "10800",
        }
    )
    created = run([str(binary)], env=create_environment, capture_output=True, timeout=18)
    create_log = created.stdout + created.stderr
    if "Entering activity: TxtReader" not in create_log or "Entering activity: ClipSelection" not in create_log:
        raise AssertionError(f"{device.upper()} could not create the {extension.upper()} highlight:\n{create_log}")
    clipping_files = list((control / "clippings").glob("txt_*.bin"))
    if len(clipping_files) != 1 or clipping_files[0].stat().st_size == 0:
        raise AssertionError(f"{device.upper()} did not persist the {extension.upper()} highlight")

    no_highlight_sd = output / "sd-no-highlight"
    shutil.copytree(sd, no_highlight_sd)
    no_highlight_control = no_highlight_sd / ".crosspoint"
    shutil.rmtree(no_highlight_control / "clippings")
    reset_simulator_navigation(control)
    reset_simulator_navigation(no_highlight_control)

    def reopen(sd_root: Path, screenshot_dir: Path) -> str:
        restart_environment = os.environ.copy()
        restart_environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd_root),
                "CROSSVI_SIM_SCREENSHOT_DIR": str(screenshot_dir),
                "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM,5200:SCREENSHOT",
                "CROSSVI_SIM_EXIT_AFTER_MS": "6200",
            }
        )
        reopened = run([str(binary)], env=restart_environment, capture_output=True, timeout=15)
        return reopened.stdout + reopened.stderr

    reopen_log = reopen(sd, restart_shots)
    if "Entering activity: TxtReader" not in reopen_log:
        raise AssertionError(f"{device.upper()} could not reopen the {extension.upper()} highlight fixture:\n{reopen_log}")
    control_log = reopen(no_highlight_sd, control_shots)
    if "Entering activity: TxtReader" not in control_log:
        raise AssertionError(f"{device.upper()} could not open the no-highlight {extension.upper()} control")
    highlighted = find_single(restart_shots, "*.framebuffer.bin").read_bytes()
    no_highlight = find_single(control_shots, "*.framebuffer.bin").read_bytes()
    if highlighted == no_highlight:
        raise AssertionError(f"{device.upper()} {extension.upper()} highlight was not rendered after restart")
    print(f"{device.upper()}: {extension.upper()} highlight persisted and rendered after restart")


def smoke_epub_highlight_restart(device: str) -> None:
    output = BUILD / f"{device}-epub-highlight-restart"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    control = sd / ".crosspoint"
    restart_shots = output / "screenshots-restart"
    control_shots = output / "screenshots-no-highlight"
    control.mkdir(parents=True)
    restart_shots.mkdir(parents=True)
    control_shots.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps(
            {
                "uiTheme": 5,
                "language": "VI",
                "screenMargin": 5,
                "paragraphAlignment": 1,
                "statusBarChapterPageCount": 0,
                "statusBarProgressBar": 2,
                "statusBarTitle": 2,
                "statusBarBattery": 0,
            }
        )
        + "\n",
        encoding="utf-8",
    )
    create_typography_epub(sd / "highlight-restart.epub", TYPOGRAPHY_LINES * 8)
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"

    create_environment = os.environ.copy()
    create_environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            # This EPUB has no TOC: Home, Book settings, Bookmark, Highlight.
            # The route also guards the production hasChapters flag passed by
            # EpubReaderActivity instead of the menu constructor's old default.
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "1200:CONFIRM,2600:CONFIRM,6000:CONFIRM,6600:DOWN,"
                "7200:DOWN,7800:DOWN,8400:CONFIRM,9300:CONFIRM,"
                "9900:RIGHT,10600:CONFIRM,11600:CONFIRM,12200:DOWN,"
                "12800:DOWN,13400:DOWN,14000:CONFIRM,14900:CONFIRM,"
                "15500:RIGHT,16100:RIGHT,16700:CONFIRM"
            ),
            "CROSSVI_SIM_EXIT_AFTER_MS": "18000",
        }
    )
    created = run([str(binary)], env=create_environment, capture_output=True, timeout=26)
    create_log = created.stdout + created.stderr
    if "Entering activity: EpubReader" not in create_log or create_log.count("Entering activity: ClipSelection") != 2:
        raise AssertionError(f"{device.upper()} could not create the EPUB highlight:\n{create_log}")
    clipping_files = list((control / "clippings").glob("epub_*.bin"))
    if len(clipping_files) != 1 or clipping_files[0].stat().st_size == 0:
        raise AssertionError(f"{device.upper()} did not persist the EPUB highlight")
    clipping_data = clipping_files[0].read_bytes()
    if len(clipping_data) < 18 or struct.unpack_from("<H", clipping_data, 16)[0] != 2:
        raise AssertionError(f"{device.upper()} did not preserve two highlights on the same EPUB page")

    no_highlight_sd = output / "sd-no-highlight"
    shutil.copytree(sd, no_highlight_sd)
    no_highlight_control = no_highlight_sd / ".crosspoint"
    shutil.rmtree(no_highlight_control / "clippings")
    reset_simulator_navigation(control)
    reset_simulator_navigation(no_highlight_control)

    def reopen(sd_root: Path, screenshot_dir: Path) -> str:
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd_root),
                "CROSSVI_SIM_SCREENSHOT_DIR": str(screenshot_dir),
                "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM,5600:SCREENSHOT",
                "CROSSVI_SIM_EXIT_AFTER_MS": "6600",
            }
        )
        completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
        return completed.stdout + completed.stderr

    highlighted_log = reopen(sd, restart_shots)
    control_log = reopen(no_highlight_sd, control_shots)
    if "Entering activity: EpubReader" not in highlighted_log or "Entering activity: EpubReader" not in control_log:
        raise AssertionError(f"{device.upper()} could not reopen the EPUB highlight fixture")
    highlighted = find_single(restart_shots, "*.framebuffer.bin").read_bytes()
    no_highlight = find_single(control_shots, "*.framebuffer.bin").read_bytes()
    if highlighted == no_highlight:
        raise AssertionError(f"{device.upper()} EPUB highlight was not rendered after restart")
    print(f"{device.upper()}: two EPUB highlights persisted and rendered after restart")


def smoke_per_book_screen_margin(device: str, book_format: str) -> None:
    output = BUILD / f"{device}-{book_format}-per-book-margin"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps(
            {
                "uiTheme": 5,
                "language": "VI",
                "screenMargin": 5,
                "paragraphAlignment": 1,
                "statusBarChapterPageCount": 0,
                "statusBarProgressBar": 2,
                "statusBarTitle": 2,
                "statusBarBattery": 0,
            }
        )
        + "\n",
        encoding="utf-8",
    )
    if book_format == "txt":
        (sd / "margin.txt").write_text(
            "".join(f"Lề sách dòng {index} cần hiển thị rõ ràng.\n" for index in range(1, 81)), encoding="utf-8"
        )
    else:
        create_typography_epub(sd / "margin.epub", TYPOGRAPHY_LINES * 8)
    cache = prime_book_cache(device, sd, book_format)
    profile = cache / "crossvi_reader_settings.bin"
    write_per_book_reader_settings(
        profile, custom=True, font_size=1, sd_font_family="", screen_margin=70, version=5
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    menu_downs = 1
    settings_downs = 5 if book_format == "txt" else 11

    def run_picker(name: str, apply: bool) -> tuple[bytes, bytes, str]:
        reset_simulator_navigation(control)
        shots = output / f"screenshots-{name}"
        shots.mkdir()
        events = ["1200:CONFIRM", "2600:CONFIRM", "5200:SCREENSHOT", "6000:CONFIRM"]
        at = 6600
        for _ in range(menu_downs):
            events.append(f"{at}:DOWN")
            at += 360
        events.append(f"{at + 240}:CONFIRM")
        at += 1100
        for _ in range(settings_downs):
            events.append(f"{at}:DOWN")
            at += 360
        events.extend(
            (
                f"{at + 240}:CONFIRM",
                f"{at + 840}:DOWN",
                f"{at + 1440}:{'CONFIRM' if apply else 'BACK'}",
                f"{at + 2240}:BACK",
                f"{at + 3800}:SCREENSHOT",
            )
        )
        exit_after = at + 4700
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
                "CROSSVI_SIM_INPUT_SCRIPT": ",".join(events),
                "CROSSVI_SIM_EXIT_AFTER_MS": str(exit_after),
            }
        )
        completed = run([str(binary)], env=environment, capture_output=True, timeout=25)
        log = completed.stdout + completed.stderr
        if "Entering activity: BookReaderSettings" not in log:
            raise AssertionError(f"{device.upper()} did not open {book_format.upper()} per-book settings:\n{log}")
        frames = sorted(shots.glob("*.framebuffer.bin"))
        if len(frames) != 2:
            raise AssertionError(f"{device.upper()} {book_format.upper()} margin flow produced {len(frames)} frames")
        return frames[0].read_bytes(), frames[1].read_bytes(), log

    before_cancel, after_cancel, _ = run_picker("cancel", False)
    flags, margin = read_per_book_screen_margin(profile)
    if not (flags & 1) or margin != 70 or before_cancel != after_cancel:
        raise AssertionError(f"{device.upper()} {book_format.upper()} Back did not preserve the 70 px per-book margin")

    before_confirm, after_confirm, _ = run_picker("confirm", True)
    flags, margin = read_per_book_screen_margin(profile)
    if not (flags & 1) or margin != 5 or before_confirm == after_confirm:
        raise AssertionError(f"{device.upper()} {book_format.upper()} Confirm did not apply the 5 px per-book margin")
    print(f"{device.upper()}: {book_format.upper()} per-book margin v5 rendered 70/5 with Back/Confirm semantics")


def smoke_settings_directional_navigation(device: str) -> None:
    output = BUILD / f"{device}-settings-directional-navigation"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    shots.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps(
            {
                "uiTheme": 5,
                "language": "VI",
                "homeLayout": 0,
                "fontSize": 1,
            }
        )
        + "\n",
        encoding="utf-8",
    )

    # Sequential Home menu -> Settings. Exercise both grouped settings screens:
    # Appearance owns the Home-layout popup. Text Settings exposes the
    # Font/Size/Layout/Style tab bar and renders font/size choices inline.
    events = (
        "1200:DOWN,1400:DOWN,1600:DOWN,1800:DOWN,2000:DOWN,2400:CONFIRM,"
        "3200:DOWN,3600:CONFIRM,4200:SCREENSHOT,4600:CONFIRM,5100:SCREENSHOT,"
        "5500:DOWN,5900:CONFIRM,6400:BACK,7000:SCREENSHOT,7400:BACK,"
        "7800:CONFIRM,8300:SCREENSHOT,8700:DOWN,9100:CONFIRM,9600:SCREENSHOT,"
        "10000:DOWN,10400:DOWN,10800:CONFIRM,11300:SCREENSHOT,11700:BACK,"
        "12100:CONFIRM,12600:SCREENSHOT,13000:CONFIRM,13400:DOWN,13800:DOWN,"
        "14200:DOWN,14600:CONFIRM,15100:SCREENSHOT"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "15800",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=22)
    log = completed.stdout + completed.stderr
    if "Entering activity: Settings" not in log or "Entering activity: SettingsSubmenu" not in log:
        raise AssertionError(f"{device.upper()} did not enter Settings through the Home grid:\n{log}")
    if "Entering activity: TextSettings" not in log or "Entering activity: FontSizeSelect" in log:
        raise AssertionError(f"{device.upper()} did not keep font and size choices inside Text Settings:\n{log}")
    screenshots = sorted(shots.glob("*.framebuffer.bin"))
    if len(screenshots) != 8 or screenshots[0].read_bytes() == screenshots[1].read_bytes():
        raise AssertionError(f"{device.upper()} did not render the Home-layout option popup")
    if screenshots[2].read_bytes() == screenshots[3].read_bytes():
        raise AssertionError(f"{device.upper()} Confirm on the tab did not change category")
    if (
        screenshots[4].read_bytes() == screenshots[5].read_bytes()
        or screenshots[5].read_bytes() == screenshots[6].read_bytes()
        or screenshots[6].read_bytes() == screenshots[7].read_bytes()
    ):
        raise AssertionError(f"{device.upper()} inline font/size lists did not render distinct states")
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("homeLayout") != 1:
        raise AssertionError(f"{device.upper()} Appearance Settings did not preserve the Home-layout selection")
    if saved.get("fontSize") != 2:
        raise AssertionError(f"{device.upper()} inline Size tab did not persist the selected 16 pt size")
    print(f"{device.upper()}: grouped Appearance/Text settings navigation and rendering smoke passed")


def smoke_screen_margin_settings(device: str) -> None:
    output = BUILD / f"{device}-screen-margin-settings"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI", "screenMargin": 5}) + "\n", encoding="utf-8"
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"

    enter_margin_picker = (
        "700:DOWN,900:DOWN,1100:DOWN,1300:DOWN,1500:DOWN,1700:CONFIRM,"
        "2300:CONFIRM,2600:DOWN,2900:CONFIRM,3300:CONFIRM,3600:CONFIRM,"
        "4500:DOWN,4800:CONFIRM"
    )

    def run_case(name: str, actions: str, exit_after_ms: int) -> str:
        shots = output / f"screenshots-{name}"
        shots.mkdir()
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
                "CROSSVI_SIM_INPUT_SCRIPT": enter_margin_picker + "," + actions,
                "CROSSVI_SIM_EXIT_AFTER_MS": str(exit_after_ms),
            }
        )
        completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
        log = completed.stdout + completed.stderr
        if "Entering activity: Settings" not in log:
            raise AssertionError(f"{device.upper()} could not enter the screen-margin picker:\n{log}")
        return log

    # Starting at 5, nine rows reach 70. Starting at the last row, one more
    # Down wraps to 5; cancel must retain 70, while Confirm must persist 5.
    down_to_70 = ",".join(f"{5400 + index * 280}:DOWN" for index in range(9))
    run_case("confirm-70", down_to_70 + ",8000:CONFIRM,8600:BACK", 9300)
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("screenMargin") != 70:
        raise AssertionError(f"{device.upper()} screen-margin picker did not persist 70")

    run_case("cancel-5", "5400:DOWN,6000:BACK,6600:BACK", 7300)
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("screenMargin") != 70:
        raise AssertionError(f"{device.upper()} Back changed the persisted screen margin")

    run_case("confirm-5", "5400:DOWN,6000:CONFIRM,6600:BACK", 7300)
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("screenMargin") != 5:
        raise AssertionError(f"{device.upper()} ten-row screen-margin picker did not wrap and persist 5")
    print(f"{device.upper()}: exact 10-value 5-70 margin picker Confirm/Back/persistence smoke passed")


def smoke_landscape_screen_margins(device: str) -> None:
    output = BUILD / f"{device}-landscape-screen-margins"
    if output.exists():
        shutil.rmtree(output)
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    widths: dict[int, int] = {}

    for margin in (5, 70):
        sd = output / f"sd-{margin}"
        control = sd / ".crosspoint"
        shots = output / f"screenshots-{margin}"
        control.mkdir(parents=True)
        shots.mkdir(parents=True)
        (control / "settings.json").write_text(
            json.dumps(
                {
                    "uiTheme": 5,
                    "language": "VI",
                    "orientation": 1,
                    "screenMargin": margin,
                    "statusBarChapterPageCount": 0,
                    "statusBarProgressBar": 2,
                    "statusBarTitle": 2,
                    "statusBarBattery": 0,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        (sd / "landscape-margin.txt").write_text(
            "".join(f"Lề ngang {margin}, dòng {index}.\n" for index in range(1, 81)), encoding="utf-8"
        )
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
                "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM,5200:SCREENSHOT",
                "CROSSVI_SIM_EXIT_AFTER_MS": "6200",
            }
        )
        completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
        log = completed.stdout + completed.stderr
        if "Entering activity: TxtReader" not in log:
            raise AssertionError(f"{device.upper()} could not open the landscape margin fixture:\n{log}")
        viewport = re.search(r"Viewport: (\d+)x(\d+)", log)
        if not viewport:
            raise AssertionError(f"{device.upper()} did not report the landscape viewport for margin {margin}")
        widths[margin] = int(viewport.group(1))
        find_single(shots, "*.framebuffer.bin")

    if widths[5] - widths[70] != 130:
        raise AssertionError(f"{device.upper()} landscape margin changed viewport by {widths[5] - widths[70]}, expected 130")
    print(f"{device.upper()}: landscape TXT margins 5/70 changed viewport width by exactly 130 px")


def smoke_font_size_settings(device: str) -> None:
    output = BUILD / f"{device}-font-size-settings"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    shots.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI", "fontSize": 1}) + "\n", encoding="utf-8"
    )

    # Home -> Settings -> Text Settings -> Size. Apply Small, then Large,
    # capture both inline preview states and verify the persisted value.
    events = (
        "800:DOWN,1050:DOWN,1300:DOWN,1550:DOWN,1800:DOWN,2400:CONFIRM,"
        "3100:CONFIRM,3400:DOWN,3800:CONFIRM,4300:CONFIRM,4700:DOWN,5100:CONFIRM,"
        "5600:SCREENSHOT,6000:DOWN,6400:DOWN,7200:CONFIRM,7700:SCREENSHOT,"
        "8200:BACK,8700:BACK"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "9400",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    if "Entering activity: TextSettings" not in log or "Entering activity: FontSizeSelect" in log:
        raise AssertionError(f"{device.upper()} did not keep the font-size flow inside Text Settings:\n{log}")
    if "Outside range" in log or "page buffer slots full" in log:
        raise AssertionError(f"{device.upper()} font-size preview leaked drawing/cache state:\n{log}")
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("fontSize") != 2:
        raise AssertionError(f"{device.upper()} did not persist the confirmed Large font size")
    if len(list(shots.glob("*.bmp"))) != 2:
        raise AssertionError(f"{device.upper()} inline font-size list did not render both preview states")
    frames = sorted(shots.glob("*.framebuffer.bin"))
    if frames[0].read_bytes() == frames[1].read_bytes():
        raise AssertionError(f"{device.upper()} inline font-size preview did not change")
    print(f"{device.upper()}: inline font-size preview/confirm/persist smoke passed")


def smoke_extended_sd_font_sizes(device: str, family_dir: Path) -> None:
    output = BUILD / f"{device}-extended-font-size-settings"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    shots.mkdir(parents=True)
    family_name = install_sd_font_family(sd, family_dir)
    (control / "settings.json").write_text(
        json.dumps(
            {
                "uiTheme": 5,
                "language": "VI" if device == "x3" else "EN",
                "fontSize": 1,
                "sdFontFamilyName": family_name,
            }
        )
        + "\n",
        encoding="utf-8",
    )

    # Enter the font-size picker, navigate from 14 to the exact 28 pt file,
    # cancel, then repeat and confirm. This covers scrolling and Back restore.
    events = (
        "800:DOWN,1050:DOWN,1300:DOWN,1550:DOWN,1800:DOWN,2400:CONFIRM,"
        "3100:CONFIRM,3400:DOWN,3800:CONFIRM,4300:CONFIRM,4700:DOWN,5100:CONFIRM,"
        "5500:DOWN,5800:DOWN,6100:DOWN,6400:DOWN,6700:DOWN,7000:DOWN,7300:DOWN,"
        "7700:SCREENSHOT,8100:BACK,8700:CONFIRM,"
        "9100:DOWN,9400:DOWN,9700:DOWN,10000:DOWN,10300:DOWN,10600:DOWN,10900:DOWN,"
        "11300:SCREENSHOT,11700:CONFIRM,12300:BACK,12700:BACK"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "13400",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=20)
    log = completed.stdout + completed.stderr
    if log.count("Entering activity: FontSizeSelect") != 2:
        raise AssertionError(f"{device.upper()} did not complete both extended-size picker flows:\n{log}")
    if "Outside range" in log or "page buffer slots full" in log:
        raise AssertionError(f"{device.upper()} large-size picker overflowed display/cache state:\n{log}")
    if not re.search(r"Loaded .* size=28\b", log):
        raise AssertionError(f"{device.upper()} picker never loaded the exact 28 pt file:\n{log}")
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("fontSize") != 8 or saved.get("sdFontFamilyName") != family_name:
        raise AssertionError(f"{device.upper()} did not persist the confirmed 28 pt SD font selection")
    if len(list(shots.glob("*.bmp"))) != 2:
        raise AssertionError(f"{device.upper()} extended-size picker did not render both 28 pt states")
    reboot_environment = os.environ.copy()
    reboot_environment.update(
        {"SDL_VIDEODRIVER": "dummy", "CROSSVI_SIM_SD": str(sd), "CROSSVI_SIM_EXIT_AFTER_MS": "1600"}
    )
    rebooted = run([str(binary)], env=reboot_environment, capture_output=True, timeout=10)
    reboot_log = rebooted.stdout + rebooted.stderr
    if not re.search(r"Loaded .* size=28\b", reboot_log):
        raise AssertionError(f"{device.upper()} did not reload the persisted 28 pt SD font after reboot")
    print(f"{device.upper()}: extended-size picker reached exact 28 pt and cancel/confirm/persist passed")


def smoke_failed_font_size_preview_rollback(device: str, family_dir: Path) -> None:
    output = BUILD / f"{device}-font-size-preview-rollback-{family_dir.name}"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    family_name = install_sd_font_family(sd, family_dir)
    broken = sd / ".fonts" / family_name / f"{family_name}_16.cpfont"
    broken.write_bytes(b"invalid cpfont preview fixture")
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI", "fontSize": 1, "sdFontFamilyName": family_name}) + "\n",
        encoding="utf-8",
    )
    events = (
        "800:DOWN,1050:DOWN,1300:DOWN,1550:DOWN,1800:DOWN,2400:CONFIRM,"
        "3100:CONFIRM,3400:DOWN,3800:CONFIRM,4300:CONFIRM,4700:DOWN,5100:CONFIRM,"
        "5800:DOWN,6500:BACK,7200:BACK,7800:BACK"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_INPUT_SCRIPT": events,
            "CROSSVI_SIM_EXIT_AFTER_MS": "8500",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("fontFamily", 0) != 0 or saved.get("fontSize") != 1 or saved.get("sdFontFamilyName") != family_name:
        raise AssertionError(f"{device.upper()} did not restore the original family and size after failed preview")
    if len(re.findall(rf"Loaded .*{re.escape(family_name)}_14\.cpfont size=14\b", log)) < 2:
        raise AssertionError(f"{device.upper()} did not reload the original 14 pt font after failed preview:\n{log}")
    if "Failed to load SD font family" not in log:
        raise AssertionError(f"{device.upper()} did not exercise the malformed preview fallback")
    print(f"{device.upper()}: malformed size preview rolled back family, size and loaded font")


def smoke_per_book_font_size_confirm_back(device: str, family_dir: Path) -> None:
    cases = (
        ("normal-confirm", False, False, True, 2, family_dir.name),
        ("normal-back", False, False, False, 8, family_dir.name),
        ("failed-confirm", True, False, True, 2, ""),
        ("missing-original-back", False, True, False, 8, family_dir.name),
    )
    for case_name, break_preview, remove_original_on_entry, confirm, expected_size, expected_sd_family in cases:
        output = BUILD / f"{device}-per-book-size-{case_name}"
        if output.exists():
            shutil.rmtree(output)
        sd = output / "sd"
        control = sd / ".crosspoint"
        control.mkdir(parents=True)
        family_name = install_sd_font_family(sd, family_dir)
        family_path = sd / ".fonts" / family_name
        if break_preview:
            (family_path / f"{family_name}_16.cpfont").write_bytes(b"invalid cpfont preview fixture")
        (control / "settings.json").write_text(
            json.dumps(
                {
                    "uiTheme": 5,
                    "language": "VI",
                    "fontFamily": 0,
                    "fontSize": 1,
                    "sdFontFamilyName": family_name,
                }
            )
            + "\n",
            encoding="utf-8",
        )
        (sd / "font-size-result.txt").write_text("Kiểm tra cỡ chữ riêng từng sách.\n", encoding="utf-8")
        cache = prime_book_cache(device, sd, "txt")
        profile = cache / "crossvi_reader_settings.bin"
        write_per_book_reader_settings(profile, custom=False, font_size=8, sd_font_family=family_name)

        picker_action = "CONFIRM" if confirm else "BACK"
        events = (
            "1200:CONFIRM,2600:CONFIRM,5000:CONFIRM,5500:DOWN,5900:DOWN,6300:DOWN,6700:DOWN,"
            "7000:DOWN,7200:CONFIRM,7800:DOWN,8200:DOWN,8700:CONFIRM,9300:DOWN,"
            f"10100:{picker_action},11100:BACK"
        )
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_INPUT_SCRIPT": events,
                "CROSSVI_SIM_EXIT_AFTER_MS": "12300",
            }
        )
        binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
        if remove_original_on_entry:
            print("+", binary)
            process = subprocess.Popen(
                [str(binary)],
                cwd=ROOT,
                env=environment,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
            )
            lines: list[str] = []
            removed = False
            assert process.stdout is not None
            for line in process.stdout:
                lines.append(line)
                if not removed and "Entering activity: FontSizeSelect" in line:
                    (family_path / f"{family_name}_14.cpfont").unlink()
                    removed = True
            return_code = process.wait(timeout=18)
            if return_code != 0 or not removed:
                raise AssertionError(
                    f"{device.upper()} could not remove the original font during picker test: rc={return_code}"
                )
            log = "".join(lines)
        else:
            completed = run([str(binary)], env=environment, capture_output=True, timeout=18)
            log = completed.stdout + completed.stderr

        if "Entering activity: BookReaderSettings" not in log or "Entering activity: FontSizeSelect" not in log:
            raise AssertionError(f"{device.upper()} {case_name} did not enter the expected typography pickers:\n{log}")
        flags, family, size, _, sd_family = read_per_book_reader_settings(profile)
        expected_custom = confirm
        if bool(flags & 1) != expected_custom or family != 0 or size != expected_size or sd_family != expected_sd_family:
            raise AssertionError(
                f"{device.upper()} {case_name} returned wrong typography state: "
                f"flags={flags} family={family} size={size} sd={sd_family!r}\n{log}"
            )
        if (break_preview or remove_original_on_entry) and "Failed to load SD font family" not in log:
            raise AssertionError(f"{device.upper()} {case_name} did not exercise the expected font-load failure")
    print(f"{device.upper()}: per-book size Confirm/Back preserved complete typography state")


def smoke_disabled_custom_font_switches(device: str, first_family: Path, second_family: Path) -> None:
    # SdCardFontRegistry sorts discovered families alphabetically, independent
    # of the order fixtures are installed or passed to this smoke test.
    first_family, second_family = sorted((first_family, second_family), key=lambda family: family.name)
    cases = (
        ("builtin-to-sd", "", 0, ["DOWN", "DOWN"], first_family.name),
        ("sd-to-builtin", first_family.name, 0, ["UP", "UP"], ""),
        ("sd-to-sd", first_family.name, 0, ["DOWN"], second_family.name),
    )
    for case_name, global_sd_family, global_family, picker_moves, expected_sd_family in cases:
        output = BUILD / f"{device}-custom-off-{case_name}"
        if output.exists():
            shutil.rmtree(output)
        sd = output / "sd"
        control = sd / ".crosspoint"
        control.mkdir(parents=True)
        install_sd_font_family(sd, first_family)
        install_sd_font_family(sd, second_family)
        settings = {"uiTheme": 5, "language": "VI", "fontFamily": global_family, "fontSize": 1}
        if global_sd_family:
            settings["sdFontFamilyName"] = global_sd_family
        (control / "settings.json").write_text(json.dumps(settings) + "\n", encoding="utf-8")
        (sd / "custom-off.txt").write_text("Kiểm tra đổi phông chữ riêng từng sách.\n", encoding="utf-8")
        cache = prime_book_cache(device, sd, "txt")
        profile = cache / "crossvi_reader_settings.bin"
        write_per_book_reader_settings(profile, custom=False, font_size=8, sd_font_family=second_family.name)

        events = [
            "1200:CONFIRM", "2600:CONFIRM", "5000:CONFIRM", "5500:DOWN", "5900:DOWN", "6300:DOWN",
            "6700:DOWN", "7100:DOWN", "7500:CONFIRM", "8000:DOWN", "8500:CONFIRM",
        ]
        move_time = 9000
        for move in picker_moves:
            events.append(f"{move_time}:{move}")
            move_time += 400
        events.extend((f"{move_time}:CONFIRM", f"{move_time + 800}:CONFIRM", f"{move_time + 1600}:BACK"))
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_INPUT_SCRIPT": ",".join(events),
                "CROSSVI_SIM_EXIT_AFTER_MS": str(move_time + 2600),
            }
        )
        binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
        completed = run([str(binary)], env=environment, capture_output=True, timeout=18)
        log = completed.stdout + completed.stderr
        if "Entering activity: BookReaderSettings" not in log or log.count("Entering activity: FontSelect") != 1:
            raise AssertionError(
                f"{device.upper()} did not complete exactly one font selection for {case_name}:\n{log}"
            )
        flags, family, size, _, sd_family = read_per_book_reader_settings(profile)
        if not (flags & 1) or family != 0 or size != 1 or sd_family != expected_sd_family:
            raise AssertionError(
                f"{device.upper()} {case_name} restored stale custom size: "
                f"flags={flags} family={family} size={size} sd={sd_family!r}"
            )
    print(f"{device.upper()}: Custom-off built-in/SD/SD family switches retained the global 14 pt size")


def smoke_per_book_missing_font_persistence(device: str, book_format: str, requested_size: int) -> None:
    expected_size = requested_size if requested_size < 4 else 3
    requested_points = (12, 14, 16, 18, 20, 22, 24, 26, 28)[requested_size]
    expected_points = (12, 14, 16, 18)[expected_size]
    output = BUILD / f"{device}-{book_format}-missing-book-font-{requested_size}"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps({"uiTheme": 5, "language": "VI", "fontFamily": 0, "fontSize": 1}) + "\n",
        encoding="utf-8",
    )
    if book_format == "txt":
        (sd / "missing-font.txt").write_text("Sách thử tiếng Việt.\n", encoding="utf-8")
    else:
        create_typography_epub(sd / "missing-font.epub", TYPOGRAPHY_LINES)
    cache = prime_book_cache(device, sd, book_format)
    profile = cache / "crossvi_reader_settings.bin"
    write_per_book_reader_settings(
        profile, custom=True, font_size=requested_size, sd_font_family="MissingPerBookFont", line_spacing=2
    )

    def reopen() -> str:
        reset_simulator_navigation(control)
        environment = os.environ.copy()
        environment.update(
            {
                "SDL_VIDEODRIVER": "dummy",
                "CROSSVI_SIM_SD": str(sd),
                "CROSSVI_SIM_INPUT_SCRIPT": "1200:CONFIRM,2600:CONFIRM",
                "CROSSVI_SIM_EXIT_AFTER_MS": "5200",
            }
        )
        binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
        completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
        return completed.stdout + completed.stderr

    first_log = reopen()
    flags, family, size, line_spacing, sd_family = read_per_book_reader_settings(profile)
    if not (flags & 1) or family != 0 or size != expected_size or line_spacing != 2 or sd_family:
        raise AssertionError(f"{device.upper()} {book_format.upper()} did not persist the safe per-book fallback")
    if "Persisted missing per-book font fallback" not in first_log:
        raise AssertionError(f"{device.upper()} {book_format.upper()} did not exercise fallback persistence")
    second_log = reopen()
    if "Persisted missing per-book font fallback" in second_log or "MissingPerBookFont" in second_log:
        raise AssertionError(f"{device.upper()} {book_format.upper()} repeated the repaired fallback after reopen")
    print(
        f"{device.upper()}: {book_format.upper()} missing {requested_points} pt font fallback "
        f"persisted as {expected_points} pt across reopen"
    )


def smoke_missing_sd_font_fallback(device: str) -> None:
    output = BUILD / f"{device}-missing-sd-font-fallback"
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    control = sd / ".crosspoint"
    control.mkdir(parents=True)
    (control / "settings.json").write_text(
        json.dumps({"fontSize": 8, "sdFontFamilyName": "MissingLargeFont"}) + "\n", encoding="utf-8"
    )
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            # Missing SD selections are repaired lazily when a font-owning
            # screen opens, keeping registry/file work off the boot path.
            "CROSSVI_SIM_INPUT_SCRIPT": (
                "800:DOWN,1050:DOWN,1300:DOWN,1550:DOWN,1800:DOWN,2400:CONFIRM,"
                "3100:CONFIRM,3400:DOWN,3800:CONFIRM,4600:BACK"
            ),
            "CROSSVI_SIM_EXIT_AFTER_MS": "5200",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=10)
    log = completed.stdout + completed.stderr
    if "Entering activity: TextSettings" not in log:
        raise AssertionError(f"{device.upper()} did not reach Text Settings for lazy font repair:\n{log}")
    saved = json.loads((control / "settings.json").read_text(encoding="utf-8"))
    if saved.get("fontSize") != 3 or saved.get("sdFontFamilyName", ""):
        raise AssertionError(f"{device.upper()} did not persist the safe built-in 18 pt fallback:\n{log}")
    print(f"{device.upper()}: missing SD font safely persisted the built-in 18 pt fallback")


def smoke_device(device: str, expected: dict[str, object], update: bool) -> tuple[str, str]:
    output = BUILD / device
    if output.exists():
        shutil.rmtree(output)
    sd = output / "sd"
    shots = output / "screenshots"
    sd.mkdir(parents=True)
    shots.mkdir(parents=True)
    environment = os.environ.copy()
    environment.update(
        {
            "SDL_VIDEODRIVER": "dummy",
            "CROSSVI_SIM_SD": str(sd),
            "CROSSVI_SIM_SCREENSHOT_DIR": str(shots),
            "CROSSVI_SIM_SCREENSHOT_AFTER_MS": "800",
            "CROSSVI_SIM_EXIT_AFTER_MS": "1200",
        }
    )
    binary = ROOT / ".pio" / "build" / f"simulator_{device}" / "program"
    completed = run([str(binary)], env=environment, capture_output=True, timeout=15)
    log = completed.stdout + completed.stderr
    if f"Hardware detect: {device.upper()}" not in log or "Entering activity: Home" not in log:
        raise AssertionError(f"{device.upper()} did not boot to Home:\n{log}")

    bmps = list(shots.glob("*.bmp"))
    raw_files = list(shots.glob("*.framebuffer.bin"))
    if len(bmps) != 1 or len(raw_files) != 1:
        raise AssertionError(f"{device.upper()} produced an unexpected screenshot set")
    dimensions = bmp_dimensions(bmps[0])
    expected_dimensions = (int(expected["logical_width"]), int(expected["logical_height"]))
    if dimensions != expected_dimensions:
        raise AssertionError(f"{device.upper()} screenshot is {dimensions}, expected {expected_dimensions}")
    raw = raw_files[0].read_bytes()
    if len(raw) != int(expected["framebuffer_bytes"]):
        raise AssertionError(f"{device.upper()} framebuffer has {len(raw)} bytes")
    digest = hashlib.sha256(raw).hexdigest()
    if not update and digest != expected["sha256"]:
        raise AssertionError(
            f"{device.upper()} Home framebuffer changed: {digest}\n"
            "If this UI change is intentional, run scripts/test_simulator.py --update-golden."
        )
    bmp_digest = hashlib.sha256(bmps[0].read_bytes()).hexdigest()
    if not update and bmp_digest != expected["bmp_sha256"]:
        raise AssertionError(
            f"{device.upper()} logical Home screenshot changed: {bmp_digest}\n"
            "If this UI change is intentional, run scripts/test_simulator.py --update-golden."
        )
    print(
        f"{device.upper()}: Home {dimensions[0]}x{dimensions[1]}, {len(raw)} bytes, "
        f"raw sha256={digest}, BMP sha256={bmp_digest}"
    )
    return digest, bmp_digest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--skip-build", action="store_true", help="reuse existing simulator binaries")
    parser.add_argument("--update-golden", action="store_true", help="accept the current empty-Home frame")
    parser.add_argument(
        "--full",
        action="store_true",
        help="run the extended simulator regression suite (the default is a 2–3 minute smoke test)",
    )
    parser.add_argument(
        "--large-font-family-dir",
        type=Path,
        default=os.environ.get("CROSSVI_LARGE_FONT_FAMILY_DIR"),
        help="optional generated 12–28 pt .cpfont family used by extended typography smoke tests",
    )
    parser.add_argument(
        "--second-large-font-family-dir",
        type=Path,
        default=os.environ.get("CROSSVI_SECOND_LARGE_FONT_FAMILY_DIR"),
        help="second generated family used for SD-to-SD and full two-family typography checks",
    )
    args = parser.parse_args()

    large_font_family = Path(args.large_font_family_dir).resolve() if args.large_font_family_dir else None
    second_large_font_family = (
        Path(args.second_large_font_family_dir).resolve() if args.second_large_font_family_dir else None
    )
    if large_font_family and not list(large_font_family.glob("*.cpfont")):
        raise AssertionError(f"No .cpfont fixtures found in {large_font_family}")
    if second_large_font_family and not list(second_large_font_family.glob("*.cpfont")):
        raise AssertionError(f"No .cpfont fixtures found in {second_large_font_family}")
    if second_large_font_family and not large_font_family:
        raise AssertionError("The second large font family requires --large-font-family-dir")
    full_suite = args.full or large_font_family is not None
    if large_font_family and not args.full:
        print("Font fixtures requested; enabling the full simulator regression suite.")

    run([sys.executable, "scripts/setup_simulator_deps.py"])
    test_host_adapters()
    pio = platformio() if not args.skip_build else None

    golden = json.loads(GOLDEN_PATH.read_text(encoding="utf-8"))
    for device in ("x3", "x4"):
        # PlatformIO may invalidate another native environment when its generated
        # project checksum changes. Build each model immediately before running
        # its smoke suite so the binary cannot be removed by the other build.
        if pio:
            run([pio, "run", "-e", f"simulator_{device}"])
        if not full_suite:
            raw_digest, bmp_digest = smoke_device(device, golden[device], args.update_golden)
            golden[device]["sha256"] = raw_digest
            golden[device]["bmp_sha256"] = bmp_digest
            smoke_settings_directional_navigation(device)
            smoke_xtc_navigation(device)
            continue
        smoke_home_layouts_without_reading_summary(device)
        smoke_home_stats_menu(device)
        smoke_home_saved_items(device)
        smoke_settings_directional_navigation(device)
        smoke_screen_margin_settings(device)
        smoke_landscape_screen_margins(device)
        smoke_font_size_settings(device)
        smoke_missing_sd_font_fallback(device)
        smoke_vietnamese_telex(device)
        smoke_vietnamese_typography(device)
        if large_font_family:
            smoke_extended_sd_font_sizes(device, large_font_family)
            smoke_large_sd_font_typography(device, large_font_family)
            smoke_failed_font_size_preview_rollback(device, large_font_family)
            smoke_per_book_font_size_confirm_back(device, large_font_family)
        if second_large_font_family:
            smoke_extended_sd_font_sizes(device, second_large_font_family)
            smoke_large_sd_font_typography(device, second_large_font_family)
            smoke_disabled_custom_font_switches(device, large_font_family, second_large_font_family)
            for requested_size in (0, 8):
                smoke_per_book_missing_font_persistence(device, "txt", requested_size)
                smoke_per_book_missing_font_persistence(device, "epub", requested_size)
        smoke_your_books_catalog(device)
        smoke_your_books_grid(device, 0, "3x2", 6)
        if device == "x3":
            smoke_book_search_actions(device, "browser")
        smoke_book_search_actions(device, "recent")
        smoke_your_books_press_edges(device)
        smoke_txt_saved_items_menu(device)
        smoke_text_highlight_restart(device, "txt")
        smoke_text_highlight_restart(device, "md")
        smoke_epub_highlight_restart(device)
        smoke_per_book_screen_margin(device, "txt")
        smoke_per_book_screen_margin(device, "epub")
        raw_digest, bmp_digest = smoke_device(device, golden[device], args.update_golden)
        golden[device]["sha256"] = raw_digest
        golden[device]["bmp_sha256"] = bmp_digest
        smoke_xtc_fixture(device, "crossvi-converter-480x800.xtc")
        smoke_xtc_status_modes(device)
        smoke_xtc_fixture(device, "crossvi-converter-480x800.xtch")
        smoke_xtc_navigation(device)
        smoke_xtc_saved_items(device)
        if device == "x3":
            stress_xtc_page_turns()
            smoke_xtc_replacement()
    if args.update_golden:
        GOLDEN_PATH.write_text(json.dumps(golden, indent=2) + "\n", encoding="utf-8")
        print(f"Updated {GOLDEN_PATH.relative_to(ROOT)}")
    print(f"{'Full' if full_suite else 'Quick'} simulator checks passed.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (AssertionError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
